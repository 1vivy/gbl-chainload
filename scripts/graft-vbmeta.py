#!/usr/bin/env python3
"""graft-vbmeta.py — transplant stock partition's embedded vbmeta region into a custom image.

Reads stock partition's AvbFooter (last 64 bytes), parses it, copies the
[partition_size - vbmeta_offset - vbmeta_size, partition_size] byte range
from stock onto the corresponding range of custom, and writes the result.

Usage:
    graft-vbmeta.py --partition recovery --stock STOCK.img --custom CUSTOM.img --out OUT.img
"""
import argparse
import struct
import sys
from pathlib import Path


AVB_FOOTER_MAGIC = b"AVBf"
AVB_FOOTER_SIZE  = 64
AVB_VBMETA_MAGIC = b"AVB0"


class GraftError(RuntimeError):
    pass


def parse_footer(partition: bytes) -> dict:
    if len(partition) < AVB_FOOTER_SIZE:
        raise GraftError(f"partition too small ({len(partition)} bytes) for AvbFooter")
    footer = partition[-AVB_FOOTER_SIZE:]
    if footer[:4] != AVB_FOOTER_MAGIC:
        raise GraftError(f"AvbFooter magic missing (got {footer[:4]!r}, want {AVB_FOOTER_MAGIC!r})")
    major, minor = struct.unpack(">II", footer[4:12])
    orig_size, vbm_off, vbm_size = struct.unpack(">QQQ", footer[12:36])
    if vbm_off + vbm_size > len(partition):
        raise GraftError(f"AvbFooter inconsistent: vbmeta_offset={vbm_off} + size={vbm_size} > partition={len(partition)}")
    return {
        "major": major, "minor": minor,
        "original_image_size": orig_size,
        "vbmeta_offset": vbm_off,
        "vbmeta_size": vbm_size,
    }


def parse_vbmeta_header(vbmeta: bytes) -> dict:
    if len(vbmeta) < 256:
        raise GraftError(f"vbmeta region too small ({len(vbmeta)} bytes) for AvbVBMetaImageHeader")
    if vbmeta[:4] != AVB_VBMETA_MAGIC:
        raise GraftError(f"AvbVBMetaImageHeader magic missing (got {vbmeta[:4]!r}, want {AVB_VBMETA_MAGIC!r})")
    major, minor = struct.unpack(">II", vbmeta[4:12])
    auth_sz, aux_sz = struct.unpack(">QQ", vbmeta[12:28])
    algo, = struct.unpack(">I", vbmeta[28:32])
    return {
        "major": major, "minor": minor,
        "auth_size": auth_sz, "aux_size": aux_sz,
        "algorithm_type": algo,
    }


def graft(partition_label: str, stock_path: Path, custom_path: Path, out_path: Path) -> None:
    stock = stock_path.read_bytes()
    custom = custom_path.read_bytes()

    if len(stock) != len(custom):
        if len(custom) > len(stock):
            raise GraftError(f"custom image larger ({len(custom)}) than stock partition size ({len(stock)})")
        custom = custom + bytes(len(stock) - len(custom))
        print(f"NOTE: padded custom from {custom_path.stat().st_size} to {len(custom)} bytes "
              f"to match stock partition size", file=sys.stderr)

    footer = parse_footer(stock)
    print(f"donor footer: vbmeta_offset={footer['vbmeta_offset']} size={footer['vbmeta_size']} "
          f"orig_image={footer['original_image_size']}")

    vbm_start = footer["vbmeta_offset"]
    vbm_end   = vbm_start + footer["vbmeta_size"]
    if vbm_end > len(stock):
        raise GraftError(f"vbmeta region extends beyond partition (end={vbm_end}, partition={len(stock)})")

    header = parse_vbmeta_header(stock[vbm_start:vbm_end])
    print(f"donor vbmeta header: avb={header['major']}.{header['minor']} algo={header['algorithm_type']} "
          f"auth_size={header['auth_size']} aux_size={header['aux_size']}")

    tail_size = (len(stock) - vbm_start)
    grafted = custom[:vbm_start] + stock[vbm_start:]

    if len(grafted) != len(stock):
        raise GraftError(f"graft size mismatch (got {len(grafted)}, want {len(stock)})")

    re_footer = parse_footer(grafted)
    assert re_footer == footer, "round-trip footer mismatch"

    out_path.write_bytes(grafted)
    print(f"wrote {out_path} ({len(grafted)} bytes); replaced last {tail_size} bytes "
          f"with donor's vbmeta region + footer")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--partition", required=True, help="recovery, dtbo, etc. (informational)")
    ap.add_argument("--stock",  required=True, type=Path)
    ap.add_argument("--custom", required=True, type=Path)
    ap.add_argument("--out",    required=True, type=Path)
    args = ap.parse_args()
    try:
        graft(args.partition, args.stock, args.custom, args.out)
    except GraftError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
