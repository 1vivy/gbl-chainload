#!/usr/bin/env python3
"""graft-vbmeta-from-stock.py — append the stock recovery.img's exact
vbmeta + footer bytes onto a custom recovery image, placed at the
custom image's natural offset (not stock's offset).

Because the vbmeta bytes are stock-verbatim (real OEM signature over
real OEM payload), libavb's avb_vbmeta_image_verify on a device with
patch10 produces:

    image_verify_result = OK      (real OEM sig + bytes; this is the
                                   per-vbmeta verify_result_local
                                   field, which ABL reads to build
                                   per-image cmdline state)

    slot-level ret      = ERROR_VERIFICATION  (the embedded hash
                                   descriptor's expected_digest is
                                   stock content's digest, but on-disk
                                   bytes are our custom image → hash
                                   mismatch when libavb processes the
                                   descriptor)

    recoverable per result_should_continue → patch10 forces final OK.

The asymmetry vs `synthesize-vbmeta` (which produced
verify_result_local = SIGNATURE_MISMATCH due to a zero signature) is
why graft is the path that boots the primary Android image: ABL emits
a clean per-vbmeta state for recovery despite the descriptor mismatch.

Placement detail: original graft tooling wrote the stock vbmeta at
stock's footer.vbmeta_offset, which on this device is 39288832 (end of
stock's 39 MB content).  If the custom recovery extends past that
(OrangeFox runs ~67 MB), the stock vbmeta bytes overwrite custom
content and break recovery boot.  This tool places the stock vbmeta
at round_up(custom_image_size, 4 KiB) instead, preserving the entire
custom image and matching how Baiqi's working implementation lays
the partition out.

Usage:
    graft-vbmeta-from-stock.py --partition recovery \\
                               --in custom.img \\
                               --partition-size 104857600 \\
                               --stock-recovery /path/to/stock_recovery.img \\
                               --out custom_with_grafted_footer.img
"""

from __future__ import annotations
import argparse
import struct
import sys
from pathlib import Path


AVB_FOOTER_MAGIC          = b"AVBf"
AVB_FOOTER_SIZE           = 64
AVB_VBMETA_MAGIC          = b"AVB0"
AVB_VBMETA_HEADER_SIZE    = 256
AVB_FOOTER_MAJOR          = 1
AVB_FOOTER_MINOR          = 0
DEFAULT_VBMETA_ALIGN      = 4096


class GraftError(RuntimeError):
    pass


def _round_up(value: int, multiple: int) -> int:
    return ((value + multiple - 1) // multiple) * multiple


def extract_stock_vbmeta_bytes(stock_path: Path) -> tuple[bytes, int, int]:
    """Open the stock recovery.img, parse its trailing AvbFooter, and slice
    out the full vbmeta image (header + auth + aux) it points to.

    Returns:
        (vbmeta_bytes, stock_vbmeta_offset, stock_vbmeta_size)

    The stock offset is informational only (we don't use it for placement);
    the returned vbmeta_bytes blob is what we graft onto the custom image.
    """
    data = stock_path.read_bytes()
    if len(data) < AVB_FOOTER_SIZE + AVB_VBMETA_HEADER_SIZE:
        raise GraftError(f"{stock_path}: too small to contain a vbmeta footer")

    footer = data[-AVB_FOOTER_SIZE:]
    if footer[:4] != AVB_FOOTER_MAGIC:
        raise GraftError(f"{stock_path}: no AvbFooter magic at end-64")

    f_major, f_minor = struct.unpack(">II", footer[4:12])
    if f_major != AVB_FOOTER_MAJOR:
        raise GraftError(f"{stock_path}: unsupported footer version {f_major}.{f_minor}")
    orig_image_size, vbm_off, vbm_size = struct.unpack(">QQQ", footer[12:36])

    if vbm_off >= len(data) - AVB_FOOTER_SIZE:
        raise GraftError(f"{stock_path}: footer.vbmeta_offset={vbm_off} past partition end")
    if vbm_off + vbm_size > len(data) - AVB_FOOTER_SIZE:
        raise GraftError(f"{stock_path}: vbmeta region overlaps footer")
    if vbm_size < AVB_VBMETA_HEADER_SIZE:
        raise GraftError(f"{stock_path}: vbmeta_size {vbm_size} smaller than header")

    vbmeta_bytes = data[vbm_off : vbm_off + vbm_size]
    if vbmeta_bytes[:4] != AVB_VBMETA_MAGIC:
        raise GraftError(f"{stock_path}: no AVB0 magic at vbmeta_offset={vbm_off}")

    return vbmeta_bytes, vbm_off, vbm_size


def build_new_footer(content_size: int, vbmeta_offset: int, vbmeta_size: int) -> bytes:
    """Build a fresh AvbFooter that references our placement of the
    grafted vbmeta.

    `original_image_size` is set to the custom image's content size so
    libavb (and any tool that reads the footer to "find the image") sees
    a consistent partition layout.
    """
    footer = (
        AVB_FOOTER_MAGIC
        + struct.pack(">II", AVB_FOOTER_MAJOR, AVB_FOOTER_MINOR)
        + struct.pack(">Q",  content_size)        # original_image_size
        + struct.pack(">Q",  vbmeta_offset)
        + struct.pack(">Q",  vbmeta_size)
    )
    footer += b"\x00" * (AVB_FOOTER_SIZE - len(footer))
    return footer


def graft(
    custom_image:   bytes,
    partition_size: int,
    stock_vbmeta:   bytes,
) -> bytes:
    """Assemble a partition-sized buffer:

        [0 .. content_size)             custom image bytes (unchanged)
        [content_size .. vbm_off)       zero pad to 4K
        [vbm_off .. vbm_off+vbm_size)   stock vbmeta bytes verbatim
        [vbm_off+vbm_size .. -64)       zero pad
        [-64 .. end)                    new AvbFooter
    """
    content_size = len(custom_image)
    if content_size == 0:
        raise GraftError("custom image is empty")
    if partition_size <= content_size:
        raise GraftError(
            f"partition_size ({partition_size}) must exceed custom image size ({content_size})")

    vbmeta_offset = _round_up(content_size, DEFAULT_VBMETA_ALIGN)
    vbmeta_size   = len(stock_vbmeta)

    if vbmeta_offset + vbmeta_size + AVB_FOOTER_SIZE > partition_size:
        raise GraftError(
            f"partition_size ({partition_size}) too small to hold "
            f"content ({content_size}) + pad + stock vbmeta ({vbmeta_size}) "
            f"+ footer ({AVB_FOOTER_SIZE})")

    footer = build_new_footer(content_size, vbmeta_offset, vbmeta_size)

    buf = bytearray(partition_size)
    buf[0:content_size]                                  = custom_image
    buf[vbmeta_offset:vbmeta_offset + vbmeta_size]       = stock_vbmeta
    buf[partition_size - AVB_FOOTER_SIZE:partition_size] = footer
    return bytes(buf)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--partition", required=True,
                    help="partition name (informational; the grafted vbmeta carries stock's descriptor)")
    ap.add_argument("--in", dest="in_path", required=True, type=Path,
                    help="custom image to graft footer onto (no existing AVB footer needed)")
    ap.add_argument("--partition-size", required=True, type=lambda s: int(s, 0),
                    help="target partition size in bytes (must exceed custom + vbmeta + footer)")
    ap.add_argument("--stock-recovery", required=True, type=Path,
                    help="stock recovery.img with its OEM-signed vbmeta + footer")
    ap.add_argument("--out", required=True, type=Path,
                    help="output partition image (partition-size bytes)")
    args = ap.parse_args()

    try:
        stock_vbmeta, stock_off, stock_size = extract_stock_vbmeta_bytes(args.stock_recovery)
        custom = args.in_path.read_bytes()
        out = graft(custom, args.partition_size, stock_vbmeta)
        args.out.write_bytes(out)
        natural_off = _round_up(len(custom), DEFAULT_VBMETA_ALIGN)
        print(
            f"wrote {args.out} ({len(out)} bytes); "
            f"custom_size={len(custom)} "
            f"placed_vbmeta_at={natural_off} "
            f"vbmeta_size={stock_size} "
            f"(stock had vbmeta at offset={stock_off}; we move it to natural offset to preserve custom content)"
        )
    except GraftError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
