#!/usr/bin/env python3
"""synthesize-vbmeta.py — append a synthesized AVB vbmeta + AvbFooter onto a
partition image with the OEM's real public key extracted from stock vbmeta.img.

This is the structural twin of Baiqi's footer-transplant: ABL's libavb parser
gets a fully stock-shaped vbmeta image — same algorithm, same authentication
block size, same pubkey bytes as stock — so the parser code path is identical
to a real signed boot.  The only thing that differs is:

  - the hash descriptor's expected_digest = SHA256 of our actual content
    (so userspace's vbmeta-digest cmdline cross-check matches what's on disk)
  - the signature bytes are zero, so libavb's RSA verify fails

Outcome at libavb: SIGNATURE_MISMATCH → AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION
(recoverable per `result_should_continue`), which patch10's AVE-bit force +
return-OK rewrite tolerates.

By using the OEM's actual chain-descriptor pubkey (not a junk key), the
chain-pubkey check inside libavb passes — failure is purely at the signature
step, not at structural validation.

Layout produced (big-endian fields, AVB convention):

    [0 .. image_size)                              : partition content
    [image_size .. vbmeta_offset)                  : zero padding to 4K
    [vbmeta_offset .. vbmeta_offset+vbmeta_size)   : vbmeta (header+auth+aux)
    [vbmeta_offset+vbmeta_size .. partition_size-64) : zero padding
    [partition_size-64 .. partition_size)          : AvbFooter

Usage:
    synthesize-vbmeta.py --partition recovery \\
                         --in CUSTOM.img \\
                         --partition-size 67108864 \\
                         --vbmeta /path/to/stock/vbmeta.img \\
                         --out OUT.img
"""

from __future__ import annotations
import argparse
import hashlib
import struct
import sys
from pathlib import Path


AVB_FOOTER_MAGIC               = b"AVBf"
AVB_FOOTER_SIZE                = 64
AVB_VBMETA_MAGIC               = b"AVB0"
AVB_VBMETA_HEADER_SIZE         = 256
AVB_DESCRIPTOR_TAG_HASH        = 2
AVB_DESCRIPTOR_TAG_CHAIN       = 4
AVB_ALGORITHM_TYPE_SHA256_RSA2048 = 1
AVB_ALGORITHM_TYPE_SHA256_RSA4096 = 2
AVB_ALGORITHM_TYPE_SHA256_RSA8192 = 3
AVB_VBMETA_REQUIRED_MAJOR      = 1
AVB_VBMETA_REQUIRED_MINOR      = 0
AVB_FOOTER_MAJOR               = 1
AVB_FOOTER_MINOR               = 0
AVB_VBMETA_BLOCK_SIZE          = 64
DEFAULT_VBMETA_ALIGN           = 4096

# Map: AvbRSAPublicKey total bytes -> (algorithm_type, key_num_bits, sig_size)
PUBKEY_LEN_TO_ALGO = {
    8 + 256 + 256:   (AVB_ALGORITHM_TYPE_SHA256_RSA2048, 2048, 256),  # 520
    8 + 512 + 512:   (AVB_ALGORITHM_TYPE_SHA256_RSA4096, 4096, 512),  # 1032
    8 + 1024 + 1024: (AVB_ALGORITHM_TYPE_SHA256_RSA8192, 8192, 1024), # 2056
}
HASH_LEN = 32  # SHA-256


class SynthesizeError(RuntimeError):
    pass


def _round_up(value: int, multiple: int) -> int:
    return ((value + multiple - 1) // multiple) * multiple


def extract_chain_pubkey(vbmeta_path: Path, partition_name: str) -> bytes:
    """Parse a stock vbmeta image, walk descriptors, return the AvbRSAPublicKey
    blob from the CHAIN PARTITION descriptor for `partition_name`."""
    data = vbmeta_path.read_bytes()
    if data[:4] != AVB_VBMETA_MAGIC:
        raise SynthesizeError(f"{vbmeta_path}: missing AVB0 magic")
    auth_sz, aux_sz = struct.unpack(">QQ", data[12:28])
    desc_off, desc_sz = struct.unpack(">QQ", data[0x60:0x70])

    aux_start = AVB_VBMETA_HEADER_SIZE + auth_sz
    aux = data[aux_start:aux_start + aux_sz]
    if len(aux) < desc_off + desc_sz:
        raise SynthesizeError(f"{vbmeta_path}: aux block too small")

    pos = desc_off
    end = desc_off + desc_sz
    while pos + 16 <= end:
        tag, num_after = struct.unpack(">QQ", aux[pos:pos+16])
        if pos + 16 + num_after > end:
            break
        body = aux[pos+16 : pos+16+num_after]
        if tag == AVB_DESCRIPTOR_TAG_CHAIN:
            # AvbChainPartitionDescriptor body:
            #   rb_loc(u32) + name_len(u32) + pk_len(u32) + flags(u8) +
            #   reserved[3] + reserved2[60] = 76 B, then name + pubkey + pad.
            rb_loc, name_len, pk_len = struct.unpack(">III", body[0:12])
            trailer = body[76:]
            name = trailer[:name_len].decode("ascii", errors="replace")
            if name == partition_name:
                pubkey = trailer[name_len:name_len + pk_len]
                if len(pubkey) != pk_len:
                    raise SynthesizeError(
                        f"{vbmeta_path}: chain[{name!r}] pk_len={pk_len} "
                        f"but trailer only has {len(trailer) - name_len} bytes")
                return pubkey
        pos += 16 + num_after

    raise SynthesizeError(
        f"{vbmeta_path}: no chain partition descriptor for {partition_name!r}")


def build_hash_descriptor(
    partition_name: str,
    image_size:    int,
    digest:        bytes,
    salt:          bytes,
    hash_algo:     str = "sha256",
) -> bytes:
    """AvbHashDescriptor: 16 (parent) + 116 (body) + name + salt + digest,
    padded to 8 bytes. `num_bytes_following` excludes the 16-byte parent."""
    name_bytes = partition_name.encode("utf-8")
    algo_bytes = hash_algo.encode("ascii")
    if len(algo_bytes) > 32:
        raise SynthesizeError(f"hash_algorithm too long: {hash_algo!r}")
    algo_field = algo_bytes + b"\x00" * (32 - len(algo_bytes))

    body = (
        struct.pack(">Q", image_size)
        + algo_field
        + struct.pack(">III", len(name_bytes), len(salt), len(digest))
        + struct.pack(">I",   0)                                # flags
        + b"\x00" * 60                                          # reserved
    )
    tail = name_bytes + salt + digest
    payload_unpadded = body + tail
    pad = (-len(payload_unpadded)) & 7
    payload = payload_unpadded + b"\x00" * pad
    header = struct.pack(">QQ", AVB_DESCRIPTOR_TAG_HASH, len(payload))
    return header + payload


def _build_vbmeta_header(
    auth_size:            int,
    aux_size:             int,
    algorithm_type:       int,
    hash_size:            int,
    signature_size:       int,
    public_key_offset:    int,
    public_key_size:      int,
    descriptors_size:     int,
    release_str:          str = "synthesize-vbmeta 1.2",
) -> bytes:
    header = (
        AVB_VBMETA_MAGIC
        + struct.pack(">II", AVB_VBMETA_REQUIRED_MAJOR, AVB_VBMETA_REQUIRED_MINOR)
        + struct.pack(">Q",  auth_size)
        + struct.pack(">Q",  aux_size)
        + struct.pack(">I",  algorithm_type)
        + struct.pack(">Q",  0)                  # hash_offset
        + struct.pack(">Q",  hash_size)
        + struct.pack(">Q",  hash_size)          # signature_offset (after hash)
        + struct.pack(">Q",  signature_size)
        + struct.pack(">Q",  public_key_offset)
        + struct.pack(">Q",  public_key_size)
        + struct.pack(">Q",  0)                  # public_key_metadata_offset
        + struct.pack(">Q",  0)                  # public_key_metadata_size
        + struct.pack(">Q",  0)                  # descriptors_offset (in aux)
        + struct.pack(">Q",  descriptors_size)
        + struct.pack(">Q",  0)                  # rollback_index
        + struct.pack(">I",  0)                  # flags
        + struct.pack(">I",  0)                  # rollback_index_location
    )
    rs = release_str.encode("ascii")
    if len(rs) > 48:
        raise SynthesizeError(f"release_string too long: {release_str!r}")
    header += rs + b"\x00" * (48 - len(rs))
    header += b"\x00" * 80                        # reserved[80]
    if len(header) != AVB_VBMETA_HEADER_SIZE:
        raise SynthesizeError(
            f"internal: header is {len(header)} bytes, want {AVB_VBMETA_HEADER_SIZE}")
    return header


def build_vbmeta(
    partition_name: str,
    image_size:    int,
    content_digest: bytes,
    salt:          bytes,
    pubkey:        bytes,
    hash_algo:     str = "sha256",
) -> bytes:
    """Build a vbmeta image with stock OEM pubkey + junk signature.

    libavb at parse time:
      - reads sizes, validates alignment → OK
      - looks up algorithm from algorithm_type → OK
      - validates pubkey size matches algorithm → OK (matches stock chain desc)
      - computes SHA256(header || aux) → matches the real hash we placed in
        the auth block's hash field
      - RSA-verify(zero signature, hash, our_pubkey) → math fails
      → returns AVB_VBMETA_VERIFY_RESULT_SIGNATURE_MISMATCH

    Then libavb's chain-pubkey callback compares OUR pubkey bytes to the
    chain descriptor's expected pubkey bytes. They MATCH (we embedded
    stock's pubkey), so no PUBLIC_KEY_REJECTED.

    Final state: only signature differs from a fully stock-signed vbmeta.
    """
    if len(pubkey) not in PUBKEY_LEN_TO_ALGO:
        raise SynthesizeError(
            f"unsupported pubkey size {len(pubkey)} bytes; "
            f"expected one of {sorted(PUBKEY_LEN_TO_ALGO.keys())}")
    algo, key_num_bits, sig_size = PUBKEY_LEN_TO_ALGO[len(pubkey)]

    # Sanity-check the AvbRSAPublicKey header field matches our table.
    pk_kbits, _ = struct.unpack(">II", pubkey[:8])
    if pk_kbits != key_num_bits:
        raise SynthesizeError(
            f"pubkey blob's key_num_bits={pk_kbits} but size implies "
            f"{key_num_bits}; pubkey appears corrupt")

    descriptor = build_hash_descriptor(partition_name, image_size,
                                       content_digest, salt, hash_algo)

    # Aux block: descriptors first (offset 0), then pubkey, padded to 64.
    descriptors_size  = len(descriptor)
    public_key_offset = descriptors_size
    public_key_size   = len(pubkey)
    aux_unpadded = descriptor + pubkey
    aux_pad      = (-len(aux_unpadded)) & (AVB_VBMETA_BLOCK_SIZE - 1)
    aux_block    = aux_unpadded + b"\x00" * aux_pad
    aux_size     = len(aux_block)

    # Auth block: hash(32) at offset 0 + signature(sig_size) immediately after,
    # padded to 64-aligned.
    auth_unpadded = HASH_LEN + sig_size
    auth_pad      = (-auth_unpadded) & (AVB_VBMETA_BLOCK_SIZE - 1)
    auth_size     = auth_unpadded + auth_pad

    header = _build_vbmeta_header(
        auth_size          = auth_size,
        aux_size           = aux_size,
        algorithm_type     = algo,
        hash_size          = HASH_LEN,
        signature_size     = sig_size,
        public_key_offset  = public_key_offset,
        public_key_size    = public_key_size,
        descriptors_size   = descriptors_size,
    )

    # Compute the real SHA256 over header + aux block. Auth block is NOT
    # included in this hash (it contains the hash itself).
    auth_hash = hashlib.sha256(header + aux_block).digest()

    auth_block = (
        auth_hash                       # hash[32]
        + b"\x00" * sig_size            # signature (zero → RSA verify fails)
        + b"\x00" * auth_pad            # padding to 64
    )
    if len(auth_block) != auth_size:
        raise SynthesizeError(
            f"internal: auth_block {len(auth_block)} != expected {auth_size}")

    return header + auth_block + aux_block


def build_avb_footer(image_size: int, vbmeta_offset: int, vbmeta_size: int) -> bytes:
    footer = (
        AVB_FOOTER_MAGIC
        + struct.pack(">II", AVB_FOOTER_MAJOR, AVB_FOOTER_MINOR)
        + struct.pack(">Q",  image_size)
        + struct.pack(">Q",  vbmeta_offset)
        + struct.pack(">Q",  vbmeta_size)
    )
    footer += b"\x00" * (AVB_FOOTER_SIZE - len(footer))
    if len(footer) != AVB_FOOTER_SIZE:
        raise SynthesizeError(
            f"internal: footer is {len(footer)} bytes, want {AVB_FOOTER_SIZE}")
    return footer


def synthesize(
    partition_name: str,
    image:          bytes,
    partition_size: int,
    pubkey:         bytes,
    salt:           bytes  = b"",
    hash_algo:      str    = "sha256",
) -> bytes:
    if hash_algo != "sha256":
        raise SynthesizeError(f"only sha256 supported, got {hash_algo!r}")

    image_size = len(image)
    if image_size == 0:
        raise SynthesizeError("input image is empty")
    if partition_size <= image_size:
        raise SynthesizeError(
            f"partition_size ({partition_size}) must exceed image_size ({image_size})")

    content_digest = hashlib.sha256(salt + image).digest()
    vbmeta = build_vbmeta(partition_name, image_size, content_digest,
                          salt, pubkey, hash_algo)
    vbmeta_size = len(vbmeta)

    vbmeta_offset = _round_up(image_size, DEFAULT_VBMETA_ALIGN)
    if vbmeta_offset + vbmeta_size + AVB_FOOTER_SIZE > partition_size:
        raise SynthesizeError(
            f"partition_size ({partition_size}) too small to hold "
            f"image ({image_size}) + pad + vbmeta ({vbmeta_size}) "
            f"+ footer ({AVB_FOOTER_SIZE})")

    footer = build_avb_footer(image_size, vbmeta_offset, vbmeta_size)

    buf = bytearray(partition_size)
    buf[0:image_size]                                = image
    buf[vbmeta_offset:vbmeta_offset + vbmeta_size]   = vbmeta
    buf[partition_size - AVB_FOOTER_SIZE:partition_size] = footer
    return bytes(buf)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--partition", required=True,
                    help="partition name as in main vbmeta (e.g. recovery, dtbo)")
    ap.add_argument("--in", dest="in_path", required=True, type=Path,
                    help="raw custom image content (without any AVB footer)")
    ap.add_argument("--partition-size", required=True, type=lambda s: int(s, 0),
                    help="target partition size in bytes")
    ap.add_argument("--vbmeta", required=True, type=Path,
                    help="stock vbmeta.img to extract OEM pubkey from (chain descriptor for --partition)")
    ap.add_argument("--out", required=True, type=Path,
                    help="output partition image (partition-size bytes)")
    ap.add_argument("--salt", default="",
                    help="hex-encoded salt prepended to image data before hashing")
    ap.add_argument("--hash-algorithm", default="sha256",
                    help="hash algorithm; only sha256 supported")
    args = ap.parse_args()

    try:
        if args.salt:
            try:
                salt = bytes.fromhex(args.salt)
            except ValueError as ve:
                raise SynthesizeError(f"--salt is not valid hex: {ve}")
        else:
            salt = b""

        pubkey = extract_chain_pubkey(args.vbmeta, args.partition)
        algo, key_num_bits, _ = PUBKEY_LEN_TO_ALGO[len(pubkey)]

        image = args.in_path.read_bytes()
        out = synthesize(args.partition, image, args.partition_size,
                         pubkey, salt=salt, hash_algo=args.hash_algorithm)
        args.out.write_bytes(out)
        print(
            f"wrote {args.out} ({len(out)} bytes); "
            f"image_size={len(image)} "
            f"vbmeta_offset={_round_up(len(image), DEFAULT_VBMETA_ALIGN)} "
            f"partition={args.partition!r} "
            f"algo={algo} (RSA-{key_num_bits}) "
            f"pubkey_size={len(pubkey)} from {args.vbmeta}"
        )
    except SynthesizeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
