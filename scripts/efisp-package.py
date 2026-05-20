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

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TOOLS = ("fv-unwrap", "abl-patcher", "gbl-pack", "mode2-profile")


def die(msg):
    sys.stderr.write(f"efisp-package: error: {msg}\n")
    sys.exit(1)


def _read_version() -> str:
    for candidate in (SCRIPT_DIR, os.path.dirname(SCRIPT_DIR)):
        p = os.path.join(candidate, "VERSION")
        if os.path.isfile(p):
            with open(p) as f:
                return f.read().strip()
    return "unknown"


def _candidates(name: str):
    """On Windows, also look for <name>.exe — bundles ship platform-suffixed bins."""
    if os.name == "nt":
        return (name + ".exe", name)
    return (name,)


def _resolve_tool(name: str, override) -> str:
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
    # In-repo cross-build discovery: dist/<platform>/<tool>
    import platform as _plat
    sys_name = _plat.system().lower()
    plat_dir = {"linux": "linux", "darwin": "macos", "windows": "windows"}.get(sys_name)
    if plat_dir:
        # Walk up from SCRIPT_DIR looking for repo root with dist/<plat>/
        d = SCRIPT_DIR
        for _ in range(4):  # at most 4 levels up
            for c in cands:
                cand = os.path.join(d, "dist", plat_dir, c)
                if os.path.isfile(cand):
                    return cand
            d = os.path.dirname(d)
    # PATH lookup honors PATHEXT on Windows automatically via shutil.which.
    p = shutil.which(name)
    if not p:
        die(f"tool '{name}' not found in --bin-dir or {SCRIPT_DIR}/bin or $PATH")
    return p


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
    ap.add_argument("--abl", help="dumped ABL partition image")
    ap.add_argument("--mode", choices=("0", "1", "2"))
    ap.add_argument("--efi", help="base mode-N.efi")
    ap.add_argument("--stock-vbmeta", help="stock vbmeta image (mode 2 only)")
    ap.add_argument("--oem", help="OEM id for abl-patcher --oem (mode 2 only)")
    ap.add_argument("--out", help="output path "
                    "(default: dist/efisp-payload/<abl>-mode<N>.efi)")
    ap.add_argument("--version", action="store_true",
                    help="print the gbl-chainload version and exit")
    ap.add_argument("--bin-dir", "--tools-dir", dest="bin_dir",
                    help="directory containing the host-tool binaries "
                         "(default: ./bin next to this script, then "
                         "dist/<platform>/ in-repo, then $PATH). "
                         "--tools-dir is a backwards-compatible alias.")
    args = ap.parse_args()

    if args.version:
        print(_read_version())
        sys.exit(0)

    # required for normal operation (loosened above to allow --version)
    for req in ("abl", "mode", "efi"):
        if not getattr(args, req):
            die(f"--{req} is required")

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

    fv     = _resolve_tool("fv-unwrap", args.bin_dir)
    patch  = _resolve_tool("abl-patcher", args.bin_dir)
    pack   = _resolve_tool("gbl-pack", args.bin_dir)
    m2p    = _resolve_tool("mode2-profile", args.bin_dir) if args.mode == "2" else None

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
