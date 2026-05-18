#!/usr/bin/env python3
"""mode2-profile — host tooling for gbl-chainload mode-2 profiles.

Subcommands:
  derive  <vbmeta.img> -o <profile.xml>   — derive a profile XML from a
                                            stock vbmeta image.
  compile <profile.xml> -o <profile.bin>  — compile a profile XML to the
                                            120-byte gbl_mode2_profile binary.

The derived values mirror what KeymasterClient.c hashes in the GREEN-locked
branch; the AVB logic is ported from the canoe reference ota_to_overrides.py.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import sys
from pathlib import Path

# ---- gbl_mode2_profile binary layout (tools/shared/gbl_mode2_profile.h) ----
M2P_MAGIC = b"GM2P"
M2P_VERSION = 1
M2P_SIZE = 120
# struct: magic[4] version:u16 reserved:u16 is_unlocked:u32 color:u32
#         system_version:u32 system_spl:u32 rot_digest[32] pubkey_digest[32] vbh[32]
M2P_STRUCT = "<4sHHIIII32s32s32s"
assert struct.calcsize(M2P_STRUCT) == M2P_SIZE


# ===================== avbtool integration =====================

def _import_avbtool():
    """avbtool.py is a script, not a packaged module. Honor $AVBTOOL, then
    fall back to ~/avbtool.py and a couple of standard locations."""
    candidates = []
    env = os.environ.get("AVBTOOL")
    if env:
        candidates.append(Path(env))
    candidates += [
        Path.home() / "avbtool.py",
        Path("/usr/bin/avbtool"),
        Path("/usr/local/bin/avbtool"),
    ]
    for path in candidates:
        if path.is_file():
            sys.path.insert(0, str(path.parent))
            try:
                return __import__(path.stem), path
            except Exception as e:
                print(f"warning: failed to import {path}: {e}", file=sys.stderr)
    raise SystemExit(
        "error: avbtool.py not found. Set AVBTOOL=/path/to/avbtool.py "
        "or place it at ~/avbtool.py."
    )


def _read_pubkey_blob(blob: bytes, avbtool, vbmeta_path: Path) -> bytes:
    """Pull the raw AVB pubkey blob out of a vbmeta image."""
    if len(blob) < 256:
        raise SystemExit(f"error: {vbmeta_path}: too small to be a vbmeta image")
    header = avbtool.AvbVBMetaHeader(blob[:256])
    aux_off = 256 + header.authentication_data_block_size
    pk_off = aux_off + header.public_key_offset
    pk_size = header.public_key_size
    if pk_size == 0:
        raise SystemExit(f"error: {vbmeta_path}: vbmeta has no public key (unsigned?)")
    if pk_off + pk_size > len(blob):
        raise SystemExit(f"error: {vbmeta_path}: public key extends past file")
    return blob[pk_off:pk_off + pk_size]


def _parse_props(blob: bytes, avbtool) -> dict:
    """Flatten the vbmeta property descriptors into a dict."""
    header = avbtool.AvbVBMetaHeader(blob[:256])
    aux_off = 256 + header.authentication_data_block_size
    aux = blob[aux_off:aux_off + header.auxiliary_data_block_size]
    props = {}
    for desc in avbtool.parse_descriptors(aux):
        if isinstance(desc, avbtool.AvbPropertyDescriptor):
            key = desc.key.decode() if isinstance(desc.key, bytes) else desc.key
            val = desc.value.decode() if isinstance(desc.value, bytes) else desc.value
            props[key] = val
    return props


def _compute_vbh(blob: bytes, avbtool) -> bytes:
    """vbh = sha256 of the meaningful vbmeta bytes (header + auth + aux blocks).

    This matches what avbtool calculate_vbmeta_digest hashes for the top-level
    vbmeta image.  We compute it inline rather than spawning the subprocess so
    that chain-partition images (boot.img, dtbo.img, …) do not need to be
    present alongside the vbmeta fixture.
    """
    header = avbtool.AvbVBMetaHeader(blob[:256])
    size = (header.SIZE
            + header.authentication_data_block_size
            + header.auxiliary_data_block_size)
    if size > len(blob):
        raise SystemExit(
            f"error: vbmeta blob declares {size} bytes but file is only {len(blob)}")
    return hashlib.sha256(blob[:size]).digest()


# ===================== encoders =====================

def _encode_os_version(version: str) -> int:
    """Bootloader-domain OS version: (Major<<14)|(Minor<<7)|SubMinor."""
    parts = (version or "0").split(".")
    while len(parts) < 3:
        parts.append("0")
    major, minor, sub = (int(parts[i]) for i in range(3))
    if minor > 0x7F or sub > 0x7F:
        raise SystemExit(f"error: OS version {version!r} component exceeds 7-bit limit")
    return (major << 14) | (minor << 7) | sub


def _encode_spl(spl: str) -> int:
    """Bootloader-domain SPL: (Day<<11)|((Year-2000)<<4)|Month."""
    m = re.match(r"^(\d{4})-(\d{2})-(\d{2})", spl or "")
    if not m:
        raise SystemExit(
            f"error: unrecognized security patch {spl!r} (expected YYYY-MM-DD)")
    year, month, day = (int(g) for g in m.groups())
    if not (2000 <= year <= 2127):
        raise SystemExit(f"error: SPL year {year} out of range (2000-2127)")
    if not (1 <= month <= 12) or not (1 <= day <= 31):
        raise SystemExit(f"error: SPL {spl!r} has out-of-range month/day")
    return (day << 11) | ((year - 2000) << 4) | month


# ===================== derive =====================

def cmd_derive(args) -> int:
    vbmeta_path = Path(args.vbmeta)
    if not vbmeta_path.is_file():
        raise SystemExit(f"error: vbmeta image not found: {vbmeta_path}")

    avbtool, _avbtool_path = _import_avbtool()
    blob = vbmeta_path.read_bytes()

    pubkey = _read_pubkey_blob(blob, avbtool, vbmeta_path)
    props = _parse_props(blob, avbtool)

    rot_digest = hashlib.sha256(pubkey + b"\x00").digest()
    pubkey_digest = hashlib.sha256(pubkey).digest()
    vbh = _compute_vbh(blob, avbtool)

    os_ver_str = props.get("com.android.build.boot.os_version")
    spl_str = props.get("com.android.build.boot.security_patch")
    if os_ver_str is None:
        raise SystemExit(
            "error: vbmeta has no com.android.build.boot.os_version property")
    if spl_str is None:
        raise SystemExit(
            "error: vbmeta has no com.android.build.boot.security_patch property")
    os_ver = _encode_os_version(os_ver_str)
    spl = _encode_spl(spl_str)

    src_sha = hashlib.sha256(blob).hexdigest()
    xml = (
        f"<!-- generated by mode2-profile derive\n"
        f"     source: {vbmeta_path}\n"
        f"     sha256: {src_sha}\n"
        f"     os_version: {os_ver_str!r} -> 0x{os_ver:x}"
        f"   spl: {spl_str!r} -> 0x{spl:X} -->\n"
        f'<gbl-chainload-mode2-profile version="1">\n'
        f"  <is-unlocked>0</is-unlocked>\n"
        f"  <color>0</color>\n"
        f"  <system-version>0x{os_ver:x}</system-version>\n"
        f"  <system-spl>0x{spl:X}</system-spl>\n"
        f"  <rot-digest>{rot_digest.hex()}</rot-digest>\n"
        f"  <pubkey-digest>{pubkey_digest.hex()}</pubkey-digest>\n"
        f"  <vbh>{vbh.hex()}</vbh>\n"
        f"</gbl-chainload-mode2-profile>\n"
    )
    Path(args.output).write_text(xml)
    print(f"wrote {args.output}")
    print(f"  rot_digest    = {rot_digest.hex()}")
    print(f"  pubkey_digest = {pubkey_digest.hex()}")
    print(f"  vbh           = {vbh.hex()}")
    print(f"  os_version    = {os_ver_str!r} -> 0x{os_ver:x}")
    print(f"  spl           = {spl_str!r} -> 0x{spl:X}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(prog="mode2-profile",
                                 description="gbl-chainload mode-2 profile tooling")
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("derive", help="derive a profile XML from a vbmeta image")
    d.add_argument("vbmeta", help="stock vbmeta.img")
    d.add_argument("-o", "--output", required=True, help="output profile XML path")
    d.set_defaults(func=cmd_derive)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
