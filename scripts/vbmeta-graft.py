#!/usr/bin/env python3
"""vbmeta-graft.py — convenience wrapper for `gbl avb`.

PR2 Task 8 collapsed the standalone vbmeta-graft binary into
`gbl avb {list,check,graft,list-hash}`. This wrapper keeps the
common-case ergonomics: a full custom partition image is the image
the user intends to write, so the final partition size is that custom
file's size — we infer --part-size from it and call `gbl avb graft`.
Use --part-size or --size-from when that is not true.
"""
import argparse
import os
import shutil
import stat
import struct
import subprocess
import sys
from typing import Optional

try:
    import fcntl
except ImportError:  # Windows: regular-file sizing still works.
    fcntl = None


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BLKGETSIZE64 = 0x80081272


def die(msg: str) -> None:
    sys.stderr.write(f"vbmeta-graft.py: error: {msg}\n")
    sys.exit(1)


def _read_version() -> str:
    for candidate in (SCRIPT_DIR, os.path.dirname(SCRIPT_DIR)):
        p = os.path.join(candidate, "VERSION")
        if os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                return f.read().strip()
    return "unknown"


def _candidates(name: str):
    if os.name == "nt":
        return (name + ".exe", name)
    return (name,)


def _resolve_tool(name: str, override: Optional[str]) -> str:
    cands = _candidates(name)
    if override:
        for c in cands:
            p = os.path.join(override, c)
            if os.path.isfile(p):
                return p
        die(f"--bin-dir does not contain '{name}': {override}")

    for c in cands:
        p = os.path.join(SCRIPT_DIR, "bin", c)
        if os.path.isfile(p):
            return p

    import platform as _plat
    plat_dir = {"linux": "linux", "darwin": "macos", "windows": "windows"}.get(
        _plat.system().lower()
    )
    if plat_dir:
        d = SCRIPT_DIR
        for _ in range(4):
            for c in cands:
                p = os.path.join(d, "dist", plat_dir, c)
                if os.path.isfile(p):
                    return p
            d = os.path.dirname(d)

    p = shutil.which(name)
    if not p:
        die(f"tool '{name}' not found in --bin-dir or {SCRIPT_DIR}/bin or $PATH")
    return p


def _path_size(path: str) -> int:
    try:
        st = os.stat(path)
    except OSError as e:
        die(f"cannot stat {path}: {e}")

    if stat.S_ISREG(st.st_mode):
        return st.st_size

    if stat.S_ISBLK(st.st_mode):
        if fcntl is None:
            die(f"block-device sizing is not supported on this platform: {path}")
        try:
            with open(path, "rb", buffering=0) as f:
                return struct.unpack("Q", fcntl.ioctl(f.fileno(), BLKGETSIZE64, b"\0" * 8))[0]
        except OSError as e:
            die(f"cannot determine block-device size for {path}: {e}")

    try:
        with open(path, "rb", buffering=0) as f:
            return f.seek(0, os.SEEK_END)
    except OSError as e:
        die(f"cannot determine size for {path}: {e}")


def _parse_size(s: str) -> int:
    try:
        n = int(s, 0)
    except ValueError:
        die(f"invalid size: {s}")
    if n <= 0:
        die("size must be positive")
    return n


def main() -> None:
    ap = argparse.ArgumentParser(
        prog="vbmeta-graft.py",
        description="Infer --part-size and call vbmeta-graft graft.",
    )
    ap.add_argument("--stock", help="stock partition image containing OEM vbmeta/footer")
    ap.add_argument("--custom", help="custom partition image, footered or bare")
    ap.add_argument("--out", help="output grafted partition image")
    size = ap.add_mutually_exclusive_group()
    size.add_argument("--part-size", help="final target partition size in bytes")
    size.add_argument("--size-from", help="derive final target partition size from this image/device")
    ap.add_argument("--main-vbmeta", help="optional main vbmeta.img for post-graft check")
    ap.add_argument("--partition", help="partition name for --main-vbmeta post-graft check")
    ap.add_argument("--bin-dir", "--tools-dir", dest="bin_dir", help="directory containing the gbl multicall binary")
    ap.add_argument("--dry-run", action="store_true", help="print resolved command but do not run it")
    ap.add_argument("--version", action="store_true", help="print the gbl-chainload version and exit")
    args = ap.parse_args()

    if args.version:
        print(_read_version())
        sys.exit(0)

    for req in ("stock", "custom", "out"):
        if not getattr(args, req):
            die(f"--{req} is required")
    if bool(args.main_vbmeta) != bool(args.partition):
        die("--main-vbmeta and --partition must be used together")

    for label, path in (("--stock", args.stock), ("--custom", args.custom)):
        if not os.path.exists(path):
            die(f"{label} not found: {path}")
    if args.main_vbmeta and not os.path.isfile(args.main_vbmeta):
        die(f"--main-vbmeta not found: {args.main_vbmeta}")

    if args.part_size:
        part_size = _parse_size(args.part_size)
        source = "--part-size"
    elif args.size_from:
        part_size = _path_size(args.size_from)
        source = f"--size-from {args.size_from}"
    else:
        part_size = _path_size(args.custom)
        source = "custom image size"
    if part_size <= 0:
        die(f"resolved non-positive part size from {source}")

    # PR2 Task 8: the standalone `vbmeta-graft` binary became `gbl avb <sub>`.
    # Keep the wrapper API identical (--bin-dir / --tools-dir overrides)
    # but locate `gbl` instead of `vbmeta-graft` and prepend `avb`.
    vg = _resolve_tool("gbl", args.bin_dir)
    graft_cmd = [
        vg,
        "avb",
        "graft",
        "--stock",
        args.stock,
        "--custom",
        args.custom,
        "--part-size",
        str(part_size),
        "--out",
        args.out,
    ]
    check_cmd = (
        [vg, "avb", "check", args.out, args.main_vbmeta, args.partition]
        if args.main_vbmeta
        else None
    )

    print(f"vbmeta-graft.py: part-size={part_size} ({source})")
    if args.dry_run:
        print("vbmeta-graft.py: " + " ".join(graft_cmd))
        if check_cmd:
            print("vbmeta-graft.py: " + " ".join(check_cmd))
        return

    res = subprocess.run(graft_cmd)
    if res.returncode != 0:
        die(f"vbmeta-graft graft failed (exit {res.returncode})")

    if check_cmd:
        res = subprocess.run(check_cmd)
        if res.returncode != 0:
            die(f"vbmeta-graft check failed (exit {res.returncode})")

    print(f"vbmeta-graft.py: wrote {args.out}")


if __name__ == "__main__":
    main()
