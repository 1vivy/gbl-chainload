#!/usr/bin/env python3
"""synthesize-vbmeta.py — append an unsigned AVB vbmeta + AvbFooter onto a
partition image whose expected_digest is computed from the image's own
content.

Unlike graft (which transplants stock vbmeta wholesale, baking in stock's
expected_digest and breaking the moment partition content differs from
stock), synthesize derives expected_digest from the actual content of the
image being signed-ish. There's no signature here — the resulting vbmeta
parses as `AVB_VBMETA_VERIFY_RESULT_OK_NOT_SIGNED`, which patch10's
`avb_slot_verify` force-AVB-success rewrite lets through.

Layout produced (big-endian fields, AVB convention):

    [0 .. image_size)                    : custom partition content (unchanged)
    [image_size .. vbmeta_offset)        : zero padding to vbmeta alignment
    [vbmeta_offset .. vbmeta_offset+vbmeta_size) : vbmeta image (header + aux)
    [vbmeta_offset+vbmeta_size .. partition_size-64) : zero padding
    [partition_size-64 .. partition_size) : AvbFooter ("AVBf" magic, offsets)

Where:
    image_size   = len(input bytes)
    vbmeta_size  = 256 (header) + aux_size, padded to 8-byte alignment
    vbmeta_offset = round_up(image_size, 4096)
    partition_size = caller-provided (matches the on-device partition size)

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


AVB_FOOTER_MAGIC          = b"AVBf"
AVB_FOOTER_SIZE           = 64
AVB_VBMETA_MAGIC          = b"AVB0"
AVB_VBMETA_HEADER_SIZE    = 256
AVB_DESCRIPTOR_TAG_HASH   = 2
AVB_ALGORITHM_TYPE_NONE   = 0
AVB_VBMETA_REQUIRED_MAJOR = 1
AVB_VBMETA_REQUIRED_MINOR = 0
AVB_FOOTER_MAJOR          = 1
AVB_FOOTER_MINOR          = 0
DEFAULT_VBMETA_ALIGN      = 4096


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

    Total bytes returned is padded to 8-byte boundary so the next descriptor
    or end-of-aux-block aligns naturally. The descriptor header's
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

    # Round body + tail to 8-byte alignment.
    payload_unpadded = body + tail
    pad = (-len(payload_unpadded)) & 7
    payload = payload_unpadded + b"\x00" * pad

    # Descriptor header: tag + num_bytes_following (excludes the 16-byte header).
    header = struct.pack(">QQ", AVB_DESCRIPTOR_TAG_HASH, len(payload))

    return header + payload


def build_vbmeta_image(
    partition_name: str,
    image_size:    int,
    digest:        bytes,
    salt:          bytes,
    hash_algo:     str = "sha256",
    release_str:   str = "synthesize-vbmeta 1.0",
) -> bytes:
    """Construct an unsigned AvbVBMetaImage: 256-byte header + aux block."""
    descriptor = build_hash_descriptor(partition_name, image_size, digest,
                                       salt, hash_algo)
    aux_size   = len(descriptor)

    # 256-byte header (big-endian).
    header = (
        AVB_VBMETA_MAGIC
        + struct.pack(">II", AVB_VBMETA_REQUIRED_MAJOR, AVB_VBMETA_REQUIRED_MINOR)
        + struct.pack(">Q",  0)                # authentication_data_block_size
        + struct.pack(">Q",  aux_size)         # auxiliary_data_block_size
        + struct.pack(">I",  AVB_ALGORITHM_TYPE_NONE)
        + struct.pack(">Q",  0)                # hash_offset
        + struct.pack(">Q",  0)                # hash_size
        + struct.pack(">Q",  0)                # signature_offset
        + struct.pack(">Q",  0)                # signature_size
        + struct.pack(">Q",  0)                # public_key_offset
        + struct.pack(">Q",  0)                # public_key_size
        + struct.pack(">Q",  0)                # public_key_metadata_offset
        + struct.pack(">Q",  0)                # public_key_metadata_size
        + struct.pack(">Q",  0)                # descriptors_offset (within aux)
        + struct.pack(">Q",  aux_size)         # descriptors_size
        + struct.pack(">Q",  0)                # rollback_index
        + struct.pack(">I",  0)                # flags
        + struct.pack(">I",  0)                # rollback_index_location
    )
    # release_string[48]
    rs = release_str.encode("ascii")
    if len(rs) > 48:
        raise SynthesizeError(f"release_string too long: {release_str!r}")
    header += rs + b"\x00" * (48 - len(rs))
    # reserved[80]
    header += b"\x00" * 80

    if len(header) != AVB_VBMETA_HEADER_SIZE:
        raise SynthesizeError(
            f"internal: vbmeta header is {len(header)} bytes, want {AVB_VBMETA_HEADER_SIZE}")

    return header + descriptor


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
    salt:           bytes = b"",
    hash_algo:      str   = "sha256",
) -> bytes:
    """Build a partition-sized buffer = image + zero padding + vbmeta + footer.

    Returns exactly partition_size bytes. Caller should write this to
    `partition_size`-sized storage (or pass through to fastboot flash).
    """
    if hash_algo != "sha256":
        # Easy to extend; keeping the host tool single-algo for now.
        raise SynthesizeError(f"only sha256 supported in this tool, got {hash_algo!r}")

    image_size = len(image)
    if image_size == 0:
        raise SynthesizeError("input image is empty")
    if partition_size <= image_size:
        raise SynthesizeError(
            f"partition_size ({partition_size}) must exceed image_size ({image_size})")

    # AVB convention: digest = SHA256(salt || image_data).
    h = hashlib.sha256()
    h.update(salt)
    h.update(image)
    digest = h.digest()

    vbmeta = build_vbmeta_image(partition_name, image_size, digest, salt, hash_algo)
    vbmeta_size = len(vbmeta)

    vbmeta_offset = _round_up(image_size, DEFAULT_VBMETA_ALIGN)
    if vbmeta_offset + vbmeta_size + AVB_FOOTER_SIZE > partition_size:
        raise SynthesizeError(
            f"partition_size ({partition_size}) too small to hold "
            f"image ({image_size}) + vbmeta-align padding "
            f"({vbmeta_offset - image_size}) + vbmeta ({vbmeta_size}) "
            f"+ footer ({AVB_FOOTER_SIZE})")

    footer = build_avb_footer(image_size, vbmeta_offset, vbmeta_size)

    # Assemble.
    buf = bytearray(partition_size)
    buf[0:image_size]                                = image
    buf[vbmeta_offset:vbmeta_offset + vbmeta_size]   = vbmeta
    buf[partition_size - AVB_FOOTER_SIZE:partition_size] = footer
    return bytes(buf)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--partition", required=True,
                    help="partition name as it appears in main vbmeta (e.g. recovery, dtbo)")
    ap.add_argument("--in", dest="in_path", required=True, type=Path,
                    help="raw custom image content (without any AVB footer)")
    ap.add_argument("--partition-size", required=True, type=lambda s: int(s, 0),
                    help="target partition size in bytes (must exceed input + vbmeta + footer)")
    ap.add_argument("--out", required=True, type=Path,
                    help="output partition image (partition-size bytes)")
    ap.add_argument("--salt", default="",
                    help="hex-encoded salt prepended to image data before hashing (default: empty)")
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
        image = args.in_path.read_bytes()
        out = synthesize(args.partition, image, args.partition_size,
                         salt=salt, hash_algo=args.hash_algorithm)
        args.out.write_bytes(out)
        print(
            f"wrote {args.out} ({len(out)} bytes); "
            f"image_size={len(image)} "
            f"vbmeta_offset={_round_up(len(image), DEFAULT_VBMETA_ALIGN)} "
            f"partition={args.partition!r} "
            f"digest=SHA256(salt||data)"
        )
    except SynthesizeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
