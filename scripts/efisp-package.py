#!/usr/bin/env python3
"""efisp-package.py — build a ready-to-flash EFISP payload off-device.

PR2 Task 8 consolidated the 7 host C tools into a single `gbl`
multicall binary; this script now chains its subcommands:

    gbl unwrap <abl.img> <extracted.efi>
    gbl patch  --in <extracted.efi> --out <patched.efi> [--oem ID]
    gbl mode2 derive <vbmeta> -o <toml>      (mode 2 only)
    gbl mode2 compile <toml>  -o <profile.bin>
    gbl pack   --cached-abl <patched.efi> --source <abl.img>
               --extracted <extracted.efi> [--mode2-profile <bin>]
               --manifest 0x0N --out <payload.bin>
    cat <base>.efi payload.bin -> <out>

Post-Task-13 the base EFI is the single `gbl-chainload.efi`; the script
is name-agnostic — any path the user passes to `--efi` is accepted and
concatenated as-is. Mode is selected at runtime via the gbl-pack
manifest, not by which EFI is being shipped.

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

TOOLS = ("gbl",)


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
    ap.add_argument("--oem", help="OEM id for abl-patcher --oem "
                                  "(allowed for any --mode)")
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
        if not args.stock_vbmeta:
            die("--mode 2 requires --stock-vbmeta")
        if not os.path.isfile(args.stock_vbmeta):
            die(f"input not found: {args.stock_vbmeta}")
    elif args.stock_vbmeta:
        die("--stock-vbmeta is only valid for --mode 2")
    # --oem is allowed for any mode; abl-patcher always applies abl_permissive.

    gbl_bin = _resolve_tool("gbl", args.bin_dir)

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
        run([gbl_bin, "unwrap", args.abl, extracted], "gbl unwrap")

        # 2. patch — abl_permissive is always applied; --oem is passed
        # through for any mode (decoupled from --mode post-Task-13).
        patch_argv = [gbl_bin, "patch", "--in", extracted, "--out", patched]
        if args.oem:
            patch_argv += ["--oem", args.oem]
        run(patch_argv, "gbl patch")

        # 3. mode 2: derive + compile the mode2 profile via `gbl mode2`.
        pack_extra = []
        if args.mode == "2":
            toml = os.path.join(tmp, "profile.toml")
            pbin = os.path.join(tmp, "profile.bin")
            run([gbl_bin, "mode2", "derive", args.stock_vbmeta, "-o", toml],
                "gbl mode2 derive")
            run([gbl_bin, "mode2", "compile", toml, "-o", pbin],
                "gbl mode2 compile")
            pack_extra = ["--mode2-profile", pbin]

        # 4. pack the GBLP1 overlay — manifest bits derived from --mode.
        #   mode 0 → 0x00              (no capability bits set)
        #   mode 1 → 0x01 WANT_FAKELOCK_HOOK
        #   mode 2 → 0x02 WANT_PROFILE_SPOOF
        manifest_bits = {"0": "0x00", "1": "0x01", "2": "0x02"}[args.mode]
        run([gbl_bin, "pack",
             "--cached-abl", patched, "--source", args.abl,
             "--extracted", extracted, *pack_extra,
             "--manifest", manifest_bits,
             "--out", payload],
            "gbl pack")

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
