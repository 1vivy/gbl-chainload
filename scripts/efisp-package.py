#!/usr/bin/env python3
"""efisp-package.py — build a ready-to-flash EFISP payload off-device.

Chains the host-side tools into a single `mode-N.efi + GBLP1 overlay`
image, the off-device equivalent of the install ZIP's build_payload:

    fv-unwrap  <abl.img>            -> extracted.efi
    abl-patcher (mode-correct flags) -> patched.efi
    mode2-profile derive+compile     -> profile.bin     (mode 2 only)
    gbl-pack    (overlay)            -> payload.bin
    cat mode-N.efi payload.bin       -> <out>

The output is produced only; flashing is the user's manual step
(`fastboot stage` + `oem boot-efi`).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = ("fv-unwrap", "abl-patcher", "gbl-pack", "mode2-profile")


def die(msg):
    sys.stderr.write(f"efisp-package: error: {msg}\n")
    sys.exit(1)


def platform_dist_dir():
    """The dist/ subdir holding this platform's built tools, or None."""
    sub = {"win32": "windows", "darwin": "macos"}.get(sys.platform)
    if not sub:
        return None    # a Linux host build lives in tools/<t>/, not dist/
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(root, "dist", sub)


def find_tool(name, tools_dir):
    """Locate a tool binary: --tools-dir, the platform dist/ dir, the
    script's own dir, then PATH."""
    exe = name + (".exe" if os.name == "nt" else "")
    candidates = []
    if tools_dir:
        candidates.append(os.path.join(tools_dir, exe))
    pdd = platform_dist_dir()
    if pdd:
        candidates.append(os.path.join(pdd, exe))
    candidates.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), exe))
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    found = shutil.which(exe)
    if found:
        return found
    die(f"tool '{name}' not found "
        f"(looked in --tools-dir, dist/<platform>/, the script dir, PATH)")


def run(argv, label):
    """Run a tool; abort with its output on failure."""
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stdout)
        sys.stderr.write(res.stderr)
        die(f"{label} failed (exit {res.returncode})")


def main():
    ap = argparse.ArgumentParser(
        prog="efisp-package.py",
        description="Build a ready-to-flash EFISP payload off-device.")
    ap.add_argument("--abl", required=True, help="dumped ABL partition image")
    ap.add_argument("--mode", required=True, choices=("0", "1", "2"))
    ap.add_argument("--efi", required=True, help="base mode-N.efi")
    ap.add_argument("--stock-vbmeta", help="stock vbmeta image (mode 2 only)")
    ap.add_argument("--oem", help="OEM id for abl-patcher --oem (mode 2 only)")
    ap.add_argument("--tools-dir", help="directory holding the host-side tool binaries")
    ap.add_argument("--out", help="output path "
                    "(default: dist/efisp-payload/<abl>-mode<N>.efi)")
    args = ap.parse_args()

    # --- pre-flight: every gate fires before any tool runs ---
    for f in (args.abl, args.efi):
        if not os.path.isfile(f):
            die(f"input not found: {f}")
    if args.mode == "2":
        if not args.stock_vbmeta or not args.oem:
            die("--mode 2 requires --stock-vbmeta and --oem")
        if not os.path.isfile(args.stock_vbmeta):
            die(f"input not found: {args.stock_vbmeta}")
    else:
        if args.stock_vbmeta or args.oem:
            die("--stock-vbmeta / --oem are only valid for --mode 2")

    fv     = find_tool("fv-unwrap", args.tools_dir)
    patch  = find_tool("abl-patcher", args.tools_dir)
    pack   = find_tool("gbl-pack", args.tools_dir)
    m2p    = find_tool("mode2-profile", args.tools_dir) if args.mode == "2" else None

    out = args.out or os.path.join(
        "dist", "efisp-payload",
        f"{os.path.splitext(os.path.basename(args.abl))[0]}-mode{args.mode}.efi")

    tmp = tempfile.mkdtemp(prefix="efisp-package.")
    wrote_out = False
    try:
        extracted = os.path.join(tmp, "extracted.efi")
        patched   = os.path.join(tmp, "patched.efi")
        payload   = os.path.join(tmp, "payload.bin")

        # 1. unwrap the ABL PE out of the partition image
        run([fv, args.abl, extracted], "fv-unwrap")

        # 2. patch — mode-correct abl-patcher flags
        patch_argv = [patch, "--in", extracted, "--out", patched]
        if args.mode == "0":
            patch_argv.append("--no-mode1")
        elif args.mode == "2":
            patch_argv += ["--oem", args.oem, "--no-mode1"]
        run(patch_argv, "abl-patcher")

        # 3. mode 2: derive + compile the mode2 profile
        pack_extra = []
        if args.mode == "2":
            toml = os.path.join(tmp, "profile.toml")
            pbin = os.path.join(tmp, "profile.bin")
            run([m2p, "derive", args.stock_vbmeta, "-o", toml], "mode2-profile derive")
            run([m2p, "compile", toml, "-o", pbin], "mode2-profile compile")
            pack_extra = ["--mode2-profile", pbin]

        # 4. pack the GBLP1 overlay
        run([pack, "--cached-abl", patched, "--source", args.abl,
             "--extracted", extracted, *pack_extra, "--out", payload],
            "gbl-pack")

        # 5. concatenate base EFI + overlay -> the output payload
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        wrote_out = True
        with open(out, "wb") as o:
            with open(args.efi, "rb") as f:
                shutil.copyfileobj(f, o)
            with open(payload, "rb") as f:
                shutil.copyfileobj(f, o)
    except BaseException:
        if wrote_out and os.path.isfile(out):
            os.unlink(out)        # no partial output
        raise
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"efisp-package: wrote {out}")


if __name__ == "__main__":
    main()
