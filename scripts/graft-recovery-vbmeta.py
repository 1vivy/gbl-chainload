#!/usr/bin/env python3
"""Graft stock recovery AVB vbmeta/footer bytes onto a custom recovery image."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

AVB_FOOTER_SIZE = 64
AVB_FOOTER_MAGIC = b"AVBf"
AVB_VBMETA_MAGIC = b"AVB0"


def round_up(value: int, align: int) -> int:
    return (value + align - 1) // align * align


def parse_footer(image: bytes) -> tuple[int, int, bytes]:
    if len(image) < AVB_FOOTER_SIZE:
        raise ValueError("stock image too small for AVB footer")
    footer = bytearray(image[-AVB_FOOTER_SIZE:])
    if bytes(footer[:4]) != AVB_FOOTER_MAGIC:
        raise ValueError("stock image footer magic is not AVBf")
    original_image_size = struct.unpack(">Q", footer[12:20])[0]
    vbmeta_offset = struct.unpack(">Q", footer[20:28])[0]
    vbmeta_size = struct.unpack(">Q", footer[28:36])[0]
    if vbmeta_size <= 0:
        raise ValueError("stock footer vbmeta_size is zero")
    end = vbmeta_offset + vbmeta_size
    if end > len(image) - AVB_FOOTER_SIZE:
        raise ValueError("stock footer vbmeta range exceeds image")
    vbmeta = image[vbmeta_offset:end]
    if not vbmeta.startswith(AVB_VBMETA_MAGIC):
        raise ValueError("stock vbmeta magic is not AVB0")
    return original_image_size, vbmeta_offset, bytes(vbmeta)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--custom", required=True, type=Path, help="custom recovery image")
    ap.add_argument("--stock", required=True, type=Path, help="stock recovery image with AVB footer")
    ap.add_argument("--out", required=True, type=Path, help="grafted output image")
    ap.add_argument("--partition-size", type=lambda x: int(x, 0), help="maximum recovery partition size")
    ap.add_argument("--dry-run", action="store_true", help="report only; do not write output")
    args = ap.parse_args()

    custom = args.custom.read_bytes()
    stock = args.stock.read_bytes()
    _, stock_vbmeta_offset, stock_vbmeta = parse_footer(stock)

    graft_offset = round_up(len(custom), 4096)
    footer = bytearray(stock[-AVB_FOOTER_SIZE:])
    footer[12:20] = struct.pack(">Q", len(custom))
    footer[20:28] = struct.pack(">Q", graft_offset)
    footer[28:36] = struct.pack(">Q", len(stock_vbmeta))

    out_size = graft_offset + len(stock_vbmeta) + AVB_FOOTER_SIZE
    partition_size = args.partition_size if args.partition_size is not None else len(stock)
    if out_size > partition_size:
        raise SystemExit(
            f"grafted image would exceed partition size: {out_size} > {partition_size}"
        )

    print(f"custom_size={len(custom)}")
    print(f"graft_offset={graft_offset}")
    print(f"stock_vbmeta_offset={stock_vbmeta_offset}")
    print(f"stock_vbmeta_size={len(stock_vbmeta)}")
    print(f"output_size={out_size}")
    print(f"partition_size={partition_size}")

    if args.dry_run:
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("wb") as f:
        f.write(custom)
        f.write(b"\0" * (graft_offset - len(custom)))
        f.write(stock_vbmeta)
        f.write(footer)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
