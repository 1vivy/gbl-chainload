#!/usr/bin/env python3
"""synthesize-vbmeta.py — append a synthesized AVB vbmeta + AvbFooter onto a
partition image whose expected_digest is computed from the image's own
content.

Default output is FAKE-SIGNED (algorithm = SHA256_RSA2048) with a junk
signature so libavb returns AVB_VBMETA_VERIFY_RESULT_SIGNATURE_MISMATCH,
which propagates as AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION at the
slot level — the same recoverable class libavb returns for the
LKM-rooted init_boot path, which boots cleanly under mode-1 + patch10.

Unlike graft (which transplants stock vbmeta wholesale, baking in stock's
expected_digest and breaking the moment partition content differs from
stock), synthesize derives expected_digest from the actual content of the
image being signed-ish.

`--unsigned` switches to algorithm_type = NONE for diagnostic A/B
comparison; libavb returns OK_NOT_SIGNED there.

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
AVB_ALGORITHM_TYPE_NONE        = 0
AVB_ALGORITHM_TYPE_SHA256_RSA2048 = 1
AVB_VBMETA_REQUIRED_MAJOR      = 1
AVB_VBMETA_REQUIRED_MINOR      = 0
AVB_FOOTER_MAJOR               = 1
AVB_FOOTER_MINOR               = 0

# libavb requires authentication_data_block_size and auxiliary_data_block_size
# to be multiples of AVB_VBMETA_BLOCK_SIZE (64). A non-64-aligned vbmeta is
# rejected with INVALID_VBMETA_HEADER, which is NOT in result_should_continue.
AVB_VBMETA_BLOCK_SIZE          = 64

# RSA-2048 signed-junk constants.
AVB_RSA2048_HASH_LEN           = 32     # SHA-256 output
AVB_RSA2048_SIG_LEN            = 256    # RSA-2048 signature
AVB_RSA2048_PUBKEY_LEN         = 4 + 4 + 256 + 256  # AvbRSAPublicKey

DEFAULT_VBMETA_ALIGN           = 4096


class SynthesizeError(RuntimeError):
    pass


def _round_up(value: int, multiple: int) -> int:
    return ((value + multiple - 1) // multiple) * multiple


def build_hash_descriptor(
    partition_name: str,
    image_size:    int,
    digest:        bytes,
    salt:          bytes,
    hash_algo:     str = "sha256",
) -> bytes:
    """Construct an AvbHashDescriptor (descriptor header + body + tail data).

    Total bytes returned is padded to 8-byte boundary. The descriptor's
    `num_bytes_following` excludes its own 16 bytes.
    """
    name_bytes = partition_name.encode("utf-8")

    # Body (116 bytes after the 16-byte descriptor header):
    #   image_size              u64    (8 B)
    #   hash_algorithm[32]      char[] (32 B)
    #   partition_name_len      u32    (4 B)
    #   salt_len                u32    (4 B)
    #   digest_len              u32    (4 B)
    #   flags                   u32    (4 B)
    #   reserved[60]            u8[]   (60 B)
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

    # Variable trailer.
    tail = name_bytes + salt + digest

    payload_unpadded = body + tail
    pad = (-len(payload_unpadded)) & 7
    payload = payload_unpadded + b"\x00" * pad

    header = struct.pack(">QQ", AVB_DESCRIPTOR_TAG_HASH, len(payload))
    return header + payload


def build_rsa2048_junk_pubkey() -> bytes:
    """Build a 520-byte AvbRSAPublicKey blob for RSA-2048.

    Format (all big-endian):
        u32 key_num_bits   = 2048
        u32 n0inv          = 0 (don't care)
        u8  N[256]         = modulus, deliberately not a real key
        u8  RR[256]        = R^2 mod N, zero

    Purpose: libavb requires public_key_size to exactly match the algorithm's
    expected size (520 for SHA256_RSA2048); a wrong size returns
    INVALID_VBMETA_HEADER which is NOT recoverable. We satisfy the size
    requirement so libavb proceeds to the signature check, which then fails
    against our zero signature → SIGNATURE_MISMATCH → ERROR_VERIFICATION.
    """
    KEY_NUM_BITS = 2048
    return (
        struct.pack(">I", KEY_NUM_BITS) +
        struct.pack(">I", 0) +
        b"\xff" * 256 +                # N — large non-zero
        b"\x00" * 256                  # RR — zero
    )


def _build_header(
    auth_size:            int,
    aux_size:             int,
    algorithm_type:       int,
    hash_offset:          int,
    hash_size:            int,
    signature_offset:     int,
    signature_size:       int,
    public_key_offset:    int,
    public_key_size:      int,
    descriptors_offset:   int,
    descriptors_size:     int,
    release_str:          str = "synthesize-vbmeta 1.0",
) -> bytes:
    header = (
        AVB_VBMETA_MAGIC
        + struct.pack(">II", AVB_VBMETA_REQUIRED_MAJOR, AVB_VBMETA_REQUIRED_MINOR)
        + struct.pack(">Q",  auth_size)              # authentication_data_block_size
        + struct.pack(">Q",  aux_size)               # auxiliary_data_block_size
        + struct.pack(">I",  algorithm_type)
        + struct.pack(">Q",  hash_offset)
        + struct.pack(">Q",  hash_size)
        + struct.pack(">Q",  signature_offset)
        + struct.pack(">Q",  signature_size)
        + struct.pack(">Q",  public_key_offset)
        + struct.pack(">Q",  public_key_size)
        + struct.pack(">Q",  0)                      # public_key_metadata_offset
        + struct.pack(">Q",  0)                      # public_key_metadata_size
        + struct.pack(">Q",  descriptors_offset)
        + struct.pack(">Q",  descriptors_size)
        + struct.pack(">Q",  0)                      # rollback_index
        + struct.pack(">I",  0)                      # flags
        + struct.pack(">I",  0)                      # rollback_index_location
    )
    rs = release_str.encode("ascii")
    if len(rs) > 48:
        raise SynthesizeError(f"release_string too long: {release_str!r}")
    header += rs + b"\x00" * (48 - len(rs))          # release_string[48]
    header += b"\x00" * 80                            # reserved[80]
    if len(header) != AVB_VBMETA_HEADER_SIZE:
        raise SynthesizeError(
            f"internal: vbmeta header is {len(header)} bytes, want {AVB_VBMETA_HEADER_SIZE}")
    return header


def build_vbmeta_signed(
    partition_name: str,
    image_size:    int,
    digest:        bytes,
    salt:          bytes,
    hash_algo:     str = "sha256",
) -> bytes:
    """Construct a fake-signed AvbVBMetaImage.

    libavb will: parse sizes OK → compute SHA256(header||aux) → match the
    real hash we placed in auth → attempt RSA-2048 verify against zero
    signature → fail → SIGNATURE_MISMATCH → ERROR_VERIFICATION at slot
    level. With patch10's AVE-bit force + force-OK return, libavb is
    silenced; the per-vbmeta verify_result remains SIGNATURE_MISMATCH (not
    OK_NOT_SIGNED), which is what we want ABL's cmdline construction
    to see for mode-1 compatibility.
    """
    descriptor = build_hash_descriptor(partition_name, image_size, digest,
                                       salt, hash_algo)
    pubkey = build_rsa2048_junk_pubkey()

    # Aux block: descriptor then pubkey, padded to 64-byte alignment.
    descriptors_offset = 0
    descriptors_size   = len(descriptor)
    public_key_offset  = descriptors_size
    public_key_size    = len(pubkey)

    aux_unpadded = descriptor + pubkey
    aux_pad      = (-len(aux_unpadded)) & (AVB_VBMETA_BLOCK_SIZE - 1)
    aux_block    = aux_unpadded + b"\x00" * aux_pad
    aux_size     = len(aux_block)

    # Auth block: hash(32) + signature(256), padded to 64-byte alignment.
    hash_offset      = 0
    hash_size        = AVB_RSA2048_HASH_LEN
    signature_offset = AVB_RSA2048_HASH_LEN
    signature_size   = AVB_RSA2048_SIG_LEN
    auth_unpadded    = hash_size + signature_size
    auth_pad         = (-auth_unpadded) & (AVB_VBMETA_BLOCK_SIZE - 1)
    auth_size        = auth_unpadded + auth_pad

    header = _build_header(
        auth_size            = auth_size,
        aux_size             = aux_size,
        algorithm_type       = AVB_ALGORITHM_TYPE_SHA256_RSA2048,
        hash_offset          = hash_offset,
        hash_size            = hash_size,
        signature_offset     = signature_offset,
        signature_size       = signature_size,
        public_key_offset    = public_key_offset,
        public_key_size      = public_key_size,
        descriptors_offset   = descriptors_offset,
        descriptors_size     = descriptors_size,
    )

    # libavb hashes (header || aux_block). Auth block is excluded since the
    # hash field lives inside it. Compute that hash now, place it at
    # auth[hash_offset]; signature stays zero so RSA verify fails.
    h = hashlib.sha256()
    h.update(header)
    h.update(aux_block)
    auth_hash = h.digest()

    auth_block = (
        auth_hash                                       # hash[32]
        + b"\x00" * signature_size                      # signature[256]
        + b"\x00" * auth_pad                            # padding to 64 align
    )
    if len(auth_block) != auth_size:
        raise SynthesizeError(
            f"internal: auth_block is {len(auth_block)} bytes, want {auth_size}")

    return header + auth_block + aux_block


def build_vbmeta_unsigned(
    partition_name: str,
    image_size:    int,
    digest:        bytes,
    salt:          bytes,
    hash_algo:     str = "sha256",
) -> bytes:
    """Construct an unsigned AvbVBMetaImage (algorithm_type = NONE).

    libavb returns OK_NOT_SIGNED. Kept as an A/B comparison option;
    on-device this state has been observed to not boot under mode-1 + patch10
    (per-vbmeta verify_result_local stays OK_NOT_SIGNED).
    """
    descriptor = build_hash_descriptor(partition_name, image_size, digest,
                                       salt, hash_algo)

    aux_unpadded = descriptor
    aux_pad      = (-len(aux_unpadded)) & (AVB_VBMETA_BLOCK_SIZE - 1)
    aux_block    = aux_unpadded + b"\x00" * aux_pad
    aux_size     = len(aux_block)

    header = _build_header(
        auth_size            = 0,
        aux_size             = aux_size,
        algorithm_type       = AVB_ALGORITHM_TYPE_NONE,
        hash_offset          = 0,
        hash_size            = 0,
        signature_offset     = 0,
        signature_size       = 0,
        public_key_offset    = 0,
        public_key_size      = 0,
        descriptors_offset   = 0,
        descriptors_size     = len(descriptor),
    )
    return header + aux_block


def build_avb_footer(image_size: int, vbmeta_offset: int, vbmeta_size: int) -> bytes:
    """Construct the 64-byte AvbFooter trailer."""
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
    salt:           bytes  = b"",
    hash_algo:      str    = "sha256",
    signed:         bool   = True,
) -> bytes:
    """Build a partition-sized buffer = image + zero pad + vbmeta + footer.

    `signed=True` (default) produces a SHA256_RSA2048 fake-signed vbmeta
    (libavb → SIGNATURE_MISMATCH → ERROR_VERIFICATION). `signed=False`
    produces algorithm_type=NONE (libavb → OK_NOT_SIGNED) for diagnostic
    comparison.
    """
    if hash_algo != "sha256":
        raise SynthesizeError(f"only sha256 supported in this tool, got {hash_algo!r}")

    image_size = len(image)
    if image_size == 0:
        raise SynthesizeError("input image is empty")
    if partition_size <= image_size:
        raise SynthesizeError(
            f"partition_size ({partition_size}) must exceed image_size ({image_size})")

    # AVB convention: descriptor digest = SHA256(salt || image_data).
    h = hashlib.sha256()
    h.update(salt)
    h.update(image)
    digest = h.digest()

    if signed:
        vbmeta = build_vbmeta_signed(partition_name, image_size, digest,
                                     salt, hash_algo)
    else:
        vbmeta = build_vbmeta_unsigned(partition_name, image_size, digest,
                                       salt, hash_algo)
    vbmeta_size = len(vbmeta)

    vbmeta_offset = _round_up(image_size, DEFAULT_VBMETA_ALIGN)
    if vbmeta_offset + vbmeta_size + AVB_FOOTER_SIZE > partition_size:
        raise SynthesizeError(
            f"partition_size ({partition_size}) too small to hold "
            f"image ({image_size}) + vbmeta-align padding "
            f"({vbmeta_offset - image_size}) + vbmeta ({vbmeta_size}) "
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
    ap.add_argument("--out", required=True, type=Path,
                    help="output partition image (partition-size bytes)")
    ap.add_argument("--salt", default="",
                    help="hex-encoded salt prepended to image data before hashing")
    ap.add_argument("--hash-algorithm", default="sha256",
                    help="hash algorithm; only sha256 supported")
    ap.add_argument("--unsigned", action="store_true",
                    help="emit algorithm_type=NONE (OK_NOT_SIGNED); default is fake-signed RSA-2048 (SIGNATURE_MISMATCH)")
    args = ap.parse_args()

    try:
        if args.salt:
            try:
                salt = bytes.fromhex(args.salt)
            except ValueError as ve:
                raise SynthesizeError(f"--salt is not valid hex: {ve}")
        else:
            salt = b""
        image = args.in_path.read_bytes()
        out = synthesize(args.partition, image, args.partition_size,
                         salt=salt, hash_algo=args.hash_algorithm,
                         signed=not args.unsigned)
        args.out.write_bytes(out)
        mode = "unsigned (OK_NOT_SIGNED)" if args.unsigned else "fake-signed RSA-2048 (SIGNATURE_MISMATCH)"
        print(
            f"wrote {args.out} ({len(out)} bytes); "
            f"image_size={len(image)} "
            f"vbmeta_offset={_round_up(len(image), DEFAULT_VBMETA_ALIGN)} "
            f"partition={args.partition!r} "
            f"mode={mode} "
            f"digest=SHA256(salt||data)"
        )
    except SynthesizeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
