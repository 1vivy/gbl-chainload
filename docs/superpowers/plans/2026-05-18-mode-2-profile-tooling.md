# Mode-2 Profile Tooling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the host-side mode-2 profile tooling — turn a stock `vbmeta.img` into a `gbl-chainload_profile.xml`, compile that to a 120-byte `mode2_profile` binary, embed it in a GBLP1 `0x0010` overlay via `gbl-pack`, and emit a staged-bootable `dist/mode-2-test.efi`.

**Architecture:** A Python CLI (`tools/mode2-profile/mode2-profile.py`) with two subcommands — `derive` (vbmeta→XML, reusing the canoe `ota_to_overrides.py` AVB logic) and `compile` (XML→120-byte binary, packing the `gbl_mode2_profile` struct). `gbl-pack` (C) gains a `--mode2-profile <bin>` flag that embeds the binary as a GBLP1 `0x0010` entry and supports a profile-only container. A bash orchestration chains build → derive → compile → pack → concat.

**Tech Stack:** Python 3 (stdlib + `avbtool.py`), C99 (`gbl-pack`), bash, the EDK2 Docker build (`scripts/build.sh`), host C harnesses under `tests/host/`.

**Scope:** Host side of design slice 3 (`docs/superpowers/specs/2026-05-18-mode-2-profile-tooling-design.md`). The device-side aarch64 helper, the `images/`-drop full orchestration, and the mode-2 ZIP are follow-ups. This work branches from `mode-2` (it depends on `tools/shared/gbl_mode2_profile.h` and the `mode2_harness`/`parser_harness` from PR #28).

---

## File structure

New files:
- `tools/mode2-profile/mode2-profile.py` — the Python CLI (`derive` + `compile` subcommands).
- `tools/mode2-profile/README.md` — short usage doc.
- `scripts/make-mode2-test-overlay.sh` — orchestration → `dist/mode-2-test.efi`.
- `tests/host/079_mode2_profile_derive.sh`, `080_mode2_profile_compile.sh`, `081_gbl_pack_mode2_profile.sh` — host tests.

Modified files:
- `tools/gbl-pack/pack.h` — add `mode2_profile` fields to `struct gbl_pack_inputs`.
- `tools/gbl-pack/pack.c` — generalize `gbl_pack_build` to a variable entry list (cached-only / profile-only / both).
- `tools/gbl-pack/gbl-pack.c` — `--mode2-profile` CLI flag; relax the required-args check.

---

### Task 1: `mode2-profile` CLI + `derive` subcommand

**Goal:** Create the `mode2-profile` Python CLI with a `derive` subcommand that turns a stock `vbmeta.img` into a `gbl-chainload_profile.xml`.

**Files:**
- Create: `tools/mode2-profile/mode2-profile.py`
- Create: `tools/mode2-profile/README.md`
- Create: `tests/host/079_mode2_profile_derive.sh`

**Acceptance Criteria:**
- [ ] `python3 tools/mode2-profile/mode2-profile.py derive <vbmeta.img> -o <out.xml>` writes a well-formed `gbl-chainload_profile.xml` with `is-unlocked`, `color`, `system-version`, `system-spl`, `rot-digest` (64 hex), `pubkey-digest` (64 hex), `vbh` (64 hex), and a provenance comment.
- [ ] `derive` exits non-zero with a clear message when `avbtool.py` cannot be found, when the vbmeta is missing/unsigned, or when the OS-version/SPL property descriptors are absent.
- [ ] `tests/host/079_mode2_profile_derive.sh` passes (or SKIPs cleanly when `~/avbtool.py` or the vbmeta fixture is absent).

**Verify:** `bash tests/host/079_mode2_profile_derive.sh` → final line `PASS: 079 mode2 profile derive` (or `SKIP: 079 ...`)

**Steps:**

- [ ] **Step 1: Write the CLI with the `derive` subcommand**

Create `tools/mode2-profile/mode2-profile.py`:

```python
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

    This intentionally hashes only the root vbmeta (header + auth + aux);
    `avbtool calculate_vbmeta_digest` additionally hashes chain-partition vbmeta
    blobs and produces a different value on images with chain descriptors; the
    device ABL's SET_VBH receives only the root digest.  We compute it inline
    rather than spawning the subprocess so that chain-partition images
    (boot.img, dtbo.img, …) do not need to be present alongside the vbmeta
    fixture.
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
```

- [ ] **Step 2: Write the README**

Create `tools/mode2-profile/README.md`:

```markdown
# mode2-profile

Host tooling for gbl-chainload mode-2 profiles.

    python3 mode2-profile.py derive  <vbmeta.img>   -o profile.xml
    python3 mode2-profile.py compile <profile.xml>  -o profile.bin

`derive` needs `avbtool.py` — set `AVBTOOL=/path/to/avbtool.py` or place it
at `~/avbtool.py`.

`profile.bin` is the 120-byte `gbl_mode2_profile` struct
(`tools/shared/gbl_mode2_profile.h`); pass it to `gbl-pack --mode2-profile`
to build a GBLP1 `0x0010` overlay.

See `docs/superpowers/specs/2026-05-18-mode-2-profile-tooling-design.md`.
```

- [ ] **Step 3: Write the test**

Create `tests/host/079_mode2_profile_derive.sh`:

```bash
#!/usr/bin/env bash
# tests/host/079_mode2_profile_derive.sh — mode2-profile derive on a real vbmeta.
set -euo pipefail
cd "$(dirname "$0")/../.."

VBMETA=images/vbmeta-infiniti-IN-16.0.7.201.img
AVBTOOL="${AVBTOOL:-$HOME/avbtool.py}"

[ -f "$VBMETA" ]  || { echo "SKIP: 079 — $VBMETA missing"; exit 0; }
[ -f "$AVBTOOL" ] || { echo "SKIP: 079 — avbtool.py not found (set AVBTOOL=)"; exit 0; }

OUT=tests/host/.last/079
mkdir -p "$OUT"

AVBTOOL="$AVBTOOL" python3 tools/mode2-profile/mode2-profile.py \
  derive "$VBMETA" -o "$OUT/profile.xml" >"$OUT/derive.log" 2>&1 \
  || { echo "FAIL: derive failed"; cat "$OUT/derive.log"; exit 1; }

# XML must contain all required elements with 64-hex digests.
grep -q '<gbl-chainload-mode2-profile version="1">' "$OUT/profile.xml" \
  || { echo "FAIL: root element missing"; exit 1; }
for tag in is-unlocked color system-version system-spl rot-digest pubkey-digest vbh; do
  grep -q "<$tag>" "$OUT/profile.xml" \
    || { echo "FAIL: <$tag> missing from XML"; exit 1; }
done
for tag in rot-digest pubkey-digest vbh; do
  val=$(sed -n "s:.*<$tag>\([0-9a-f]*\)</$tag>.*:\1:p" "$OUT/profile.xml")
  [ "${#val}" -eq 64 ] \
    || { echo "FAIL: <$tag> is ${#val} hex chars, expected 64"; exit 1; }
done

echo "PASS: 079 mode2 profile derive"
```

- [ ] **Step 4: Run the test**

Run: `bash tests/host/079_mode2_profile_derive.sh`
Expected: `PASS: 079 mode2 profile derive` (or a `SKIP:` line if `~/avbtool.py` or the vbmeta fixture is absent — both are acceptable per the acceptance criteria).

- [ ] **Step 5: Commit**

```bash
git add tools/mode2-profile/mode2-profile.py tools/mode2-profile/README.md \
  tests/host/079_mode2_profile_derive.sh
git commit -m "feat(mode-2): mode2-profile derive — vbmeta to profile XML"
```

---

### Task 2: `mode2-profile compile` subcommand

**Goal:** Add the `compile` subcommand that turns a `gbl-chainload_profile.xml` into the 120-byte `gbl_mode2_profile` binary.

**Files:**
- Modify: `tools/mode2-profile/mode2-profile.py`
- Create: `tests/host/080_mode2_profile_compile.sh`

**Acceptance Criteria:**
- [ ] `python3 tools/mode2-profile/mode2-profile.py compile <xml> -o <bin>` writes exactly 120 bytes: the packed `gbl_mode2_profile` struct (`GM2P` magic, version 1, the four scalars, the three 32-byte digests).
- [ ] `compile` rejects malformed XML, a wrong root `version`, `is-unlocked > 1`, `color > 3`, and any digest that is not exactly 64 hex characters — exiting non-zero without writing output.
- [ ] The compiled binary is accepted by the EDK2 parser: `mode2_harness profile-parse <bin>` reports `status=0`.
- [ ] `tests/host/080_mode2_profile_compile.sh` passes.

**Verify:** `bash tests/host/080_mode2_profile_compile.sh` → final line `PASS: 080 mode2 profile compile`

**Steps:**

- [ ] **Step 1: Add the `compile` implementation**

In `tools/mode2-profile/mode2-profile.py`, add `import xml.etree.ElementTree as ET` to the imports, then add this function before `main()`:

```python
# ===================== compile =====================

def _req_text(root, tag: str) -> str:
    el = root.find(tag)
    if el is None or el.text is None or el.text.strip() == "":
        raise SystemExit(f"error: <{tag}> missing or empty in profile XML")
    return el.text.strip()


def _parse_int(s: str, tag: str) -> int:
    try:
        return int(s, 0)  # accepts 0x.. and decimal
    except ValueError:
        raise SystemExit(f"error: <{tag}> value {s!r} is not an integer")


def _parse_digest(s: str, tag: str) -> bytes:
    if len(s) != 64 or re.fullmatch(r"[0-9a-fA-F]{64}", s) is None:
        raise SystemExit(
            f"error: <{tag}> must be exactly 64 hex characters (got {len(s)})")
    return bytes.fromhex(s)


def cmd_compile(args) -> int:
    xml_path = Path(args.xml)
    if not xml_path.is_file():
        raise SystemExit(f"error: profile XML not found: {xml_path}")
    try:
        root = ET.fromstring(xml_path.read_text())
    except ET.ParseError as e:
        raise SystemExit(f"error: malformed profile XML: {e}")

    if root.tag != "gbl-chainload-mode2-profile":
        raise SystemExit(f"error: unexpected root element <{root.tag}>")
    if root.get("version") != "1":
        raise SystemExit(
            f"error: profile version {root.get('version')!r}, expected \"1\"")

    is_unlocked = _parse_int(_req_text(root, "is-unlocked"), "is-unlocked")
    color = _parse_int(_req_text(root, "color"), "color")
    system_version = _parse_int(_req_text(root, "system-version"), "system-version")
    system_spl = _parse_int(_req_text(root, "system-spl"), "system-spl")
    rot_digest = _parse_digest(_req_text(root, "rot-digest"), "rot-digest")
    pubkey_digest = _parse_digest(_req_text(root, "pubkey-digest"), "pubkey-digest")
    vbh = _parse_digest(_req_text(root, "vbh"), "vbh")

    if is_unlocked > 1:
        raise SystemExit(f"error: is-unlocked must be 0 or 1 (got {is_unlocked})")
    if color > 3:
        raise SystemExit(f"error: color must be 0..3 (got {color})")
    for name, v in (("system-version", system_version), ("system-spl", system_spl)):
        if v > 0xFFFFFFFF:
            raise SystemExit(f"error: <{name}> exceeds 32 bits")

    blob = struct.pack(M2P_STRUCT, M2P_MAGIC, M2P_VERSION, 0,
                       is_unlocked, color, system_version, system_spl,
                       rot_digest, pubkey_digest, vbh)
    assert len(blob) == M2P_SIZE
    Path(args.output).write_bytes(blob)
    print(f"wrote {args.output} ({len(blob)} bytes)")
    return 0
```

- [ ] **Step 2: Register the subcommand**

In `main()`, after the `derive` subparser block and before `args = ap.parse_args()`, add:

```python
    c = sub.add_parser("compile", help="compile a profile XML to a 120-byte binary")
    c.add_argument("xml", help="gbl-chainload_profile.xml")
    c.add_argument("-o", "--output", required=True, help="output profile.bin path")
    c.set_defaults(func=cmd_compile)
```

- [ ] **Step 3: Write the test**

Create `tests/host/080_mode2_profile_compile.sh`:

```bash
#!/usr/bin/env bash
# tests/host/080_mode2_profile_compile.sh — compile a profile XML and confirm
# the EDK2 parser accepts the resulting binary.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers mode2_harness
H=tests/host/helpers/mode2_harness
M2P=tools/mode2-profile/mode2-profile.py
OUT=tests/host/.last/080
mkdir -p "$OUT"

# A well-formed profile XML.
cat > "$OUT/good.xml" <<'XML'
<gbl-chainload-mode2-profile version="1">
  <is-unlocked>0</is-unlocked>
  <color>0</color>
  <system-version>0x40000</system-version>
  <system-spl>0x9A4</system-spl>
  <rot-digest>1111111111111111111111111111111111111111111111111111111111111111</rot-digest>
  <pubkey-digest>2222222222222222222222222222222222222222222222222222222222222222</pubkey-digest>
  <vbh>3333333333333333333333333333333333333333333333333333333333333333</vbh>
</gbl-chainload-mode2-profile>
XML

python3 "$M2P" compile "$OUT/good.xml" -o "$OUT/good.bin" >"$OUT/compile.log" 2>&1 \
  || { echo "FAIL: compile failed"; cat "$OUT/compile.log"; exit 1; }

# Exactly 120 bytes.
sz=$(stat -c%s "$OUT/good.bin")
[ "$sz" -eq 120 ] || { echo "FAIL: binary is $sz bytes, expected 120"; exit 1; }

# The EDK2 parser must accept it.
"$H" profile-parse "$OUT/good.bin" | grep -q 'status=0' \
  || { echo "FAIL: EDK2 parser rejected the compiled profile"; exit 1; }

# Rejection cases — each must exit non-zero and not write output.
reject() {  # <name> <xml-file>
  if python3 "$M2P" compile "$2" -o "$OUT/reject.bin" >/dev/null 2>&1; then
    echo "FAIL: compile accepted bad input ($1)"; exit 1
  fi
}
sed 's:<color>0</color>:<color>9</color>:' "$OUT/good.xml" > "$OUT/badcolor.xml"
reject "color>3" "$OUT/badcolor.xml"
sed 's:<is-unlocked>0</is-unlocked>:<is-unlocked>5</is-unlocked>:' "$OUT/good.xml" > "$OUT/badunlk.xml"
reject "is-unlocked>1" "$OUT/badunlk.xml"
sed 's:version="1":version="2":' "$OUT/good.xml" > "$OUT/badver.xml"
reject "version!=1" "$OUT/badver.xml"
sed 's:<vbh>3333:<vbh>33:' "$OUT/good.xml" > "$OUT/badvbh.xml"
reject "short vbh digest" "$OUT/badvbh.xml"
printf '<gbl-chainload-mode2-profile version="1"><is-unlocked>' > "$OUT/malformed.xml"
reject "malformed XML" "$OUT/malformed.xml"

echo "PASS: 080 mode2 profile compile"
```

- [ ] **Step 4: Run the test**

Run: `bash tests/host/080_mode2_profile_compile.sh`
Expected: `PASS: 080 mode2 profile compile`.

- [ ] **Step 5: Commit**

```bash
git add tools/mode2-profile/mode2-profile.py tests/host/080_mode2_profile_compile.sh
git commit -m "feat(mode-2): mode2-profile compile — profile XML to 120-byte binary"
```

---

### Task 3: `gbl-pack --mode2-profile`

**Goal:** Extend `gbl-pack` to embed a 120-byte `mode2_profile` binary as a GBLP1 `0x0010` entry, supporting cached-only, profile-only, and combined containers.

**Files:**
- Modify: `tools/gbl-pack/pack.h`
- Modify: `tools/gbl-pack/pack.c`
- Modify: `tools/gbl-pack/gbl-pack.c`
- Create: `tests/host/081_gbl_pack_mode2_profile.sh`

**Acceptance Criteria:**
- [ ] `gbl-pack --mode2-profile <bin> --out <file>` (no `--cached-abl`) writes a GBLP1 container holding exactly one `0x0010` entry, and `parser_harness find-mode2-profile <file>` reports `status=0`.
- [ ] `gbl-pack` rejects a `--mode2-profile` input that is not exactly 120 bytes or lacks the `GM2P` magic.
- [ ] Invoking `gbl-pack` with neither `--cached-abl` nor `--mode2-profile` still errors with a usage message.
- [ ] The existing cached-ABL path is unchanged — `tests/host/060_pack_roundtrip.sh` and `062_efisp_scan_gate.sh` still pass.
- [ ] `tests/host/081_gbl_pack_mode2_profile.sh` passes.

**Verify:** `bash tests/host/081_gbl_pack_mode2_profile.sh` → final line `PASS: 081 gbl-pack mode2 profile`

**Steps:**

- [ ] **Step 1: Add the input fields to `pack.h`**

In `tools/gbl-pack/pack.h`, add two fields to `struct gbl_pack_inputs` (after the `extracted` line) and a status code:

```c
struct gbl_pack_inputs {
    const uint8_t *cached_abl;  size_t cached_abl_size;
    const uint8_t *source;      size_t source_size;
    const uint8_t *extracted;   size_t extracted_size;
    const uint8_t *mode2_profile; size_t mode2_profile_size;  /* optional */
    const char    *packer_version;   /* ASCII */
    const char    *timestamp_iso8601;/* ASCII */
};
```

And add `GBL_PACK_ERR_PROFILE_BAD` to `enum gbl_pack_status` (after `GBL_PACK_ERR_BAD_INPUT`).

- [ ] **Step 2: Generalize `gbl_pack_build` in `pack.c`**

Replace the entire body of `gbl_pack_build` in `tools/gbl-pack/pack.c` with a variable-entry-list builder. The new body:

```c
/* gbl_mode2_profile layout sanity (tools/shared/gbl_mode2_profile.h). */
#define GBL_M2P_MAGIC "GM2P"
#define GBL_M2P_SIZE  120u

enum gbl_pack_status
gbl_pack_build(const struct gbl_pack_inputs *in,
               uint8_t **out_buf, size_t *out_size)
{
    if (!in)
        return GBL_PACK_ERR_BAD_INPUT;
    int have_cached  = (in->cached_abl && in->cached_abl_size > 0);
    int have_profile = (in->mode2_profile && in->mode2_profile_size > 0);
    if (!have_cached && !have_profile)
        return GBL_PACK_ERR_BAD_INPUT;

    if (have_cached) {
        if (gbl_contains_utf16_efisp(in->cached_abl, in->cached_abl_size))
            return GBL_PACK_ERR_EFISP_PRESENT;
        if (gbl_pe_sanity(in->cached_abl, in->cached_abl_size) != GBL_PE_OK)
            return GBL_PACK_ERR_PE_INSANE;
    }
    if (have_profile) {
        if (in->mode2_profile_size != GBL_M2P_SIZE)
            return GBL_PACK_ERR_PROFILE_BAD;
        if (memcmp(in->mode2_profile, GBL_M2P_MAGIC, 4) != 0)
            return GBL_PACK_ERR_PROFILE_BAD;
    }

    /* source_meta payload (only emitted alongside cached_abl). */
    size_t pv_len = in->packer_version ? strlen(in->packer_version) : 0;
    size_t ts_len = in->timestamp_iso8601 ? strlen(in->timestamp_iso8601) : 0;
    size_t meta_size = 3 * (4 + 32) + 4 + pv_len + 4 + ts_len;

    /* Entry descriptors, in emission order. */
    struct { uint16_t type; const uint8_t *data; size_t size; } ents[3];
    uint32_t ec = 0;
    if (have_cached) {
        ents[ec].type = GBLP1_TYPE_CACHED_ABL;
        ents[ec].data = in->cached_abl; ents[ec].size = in->cached_abl_size; ec++;
        ents[ec].type = GBLP1_TYPE_SOURCE_META;
        ents[ec].data = NULL;          ents[ec].size = meta_size;            ec++;
    }
    if (have_profile) {
        ents[ec].type = GBLP1_TYPE_MODE2_PROFILE;
        ents[ec].data = in->mode2_profile; ents[ec].size = in->mode2_profile_size; ec++;
    }

    uint32_t entries_end = GBLP1_HEADER_SIZE + ec * GBLP1_ENTRY_SIZE;
    uint32_t off = align_up(entries_end, GBLP1_PAYLOAD_ALIGN);
    uint32_t payload_off[3];
    for (uint32_t i = 0; i < ec; i++) {
        payload_off[i] = off;
        off = align_up(off + (uint32_t)ents[i].size, GBLP1_PAYLOAD_ALIGN);
    }
    uint32_t total = off + GBLP1_FOOTER_SIZE;
    if (total > GBLP1_TOTAL_SIZE_CAP) return GBL_PACK_ERR_TOO_LARGE;

    uint8_t *buf = calloc(1, total);
    if (!buf) return GBL_PACK_ERR_OOM;

    /* Header. */
    memcpy(buf + 0, GBLP1_MAGIC, GBLP1_MAGIC_SIZE);
    wle16(buf + 8,  GBLP1_VERSION);
    wle16(buf + 10, GBLP1_HEADER_SIZE);
    wle32(buf + 12, GBLP1_FLAGS_LE);
    wle32(buf + 16, total);
    wle32(buf + 20, ec);

    /* Entry table + payloads. */
    for (uint32_t i = 0; i < ec; i++) {
        uint8_t *e = buf + GBLP1_HEADER_SIZE + i * GBLP1_ENTRY_SIZE;
        wle16(e + 0,  ents[i].type);
        wle16(e + 2,  0);
        wle32(e + 4,  payload_off[i]);
        wle32(e + 8,  (uint32_t)ents[i].size);
        wle32(e + 12, 0);
        if (ents[i].type == GBLP1_TYPE_SOURCE_META) {
            uint8_t *m = buf + payload_off[i];
            wle32(m, (uint32_t)in->source_size);    m += 4;
            if (in->source) gbl_sha256(in->source, in->source_size, m);
            m += 32;
            wle32(m, (uint32_t)in->extracted_size); m += 4;
            if (in->extracted) gbl_sha256(in->extracted, in->extracted_size, m);
            m += 32;
            wle32(m, (uint32_t)in->cached_abl_size); m += 4;
            gbl_sha256(in->cached_abl, in->cached_abl_size, m); m += 32;
            wle32(m, (uint32_t)pv_len);             m += 4;
            if (pv_len) memcpy(m, in->packer_version, pv_len);
            m += pv_len;
            wle32(m, (uint32_t)ts_len);             m += 4;
            if (ts_len) memcpy(m, in->timestamp_iso8601, ts_len);
        } else {
            memcpy(buf + payload_off[i], ents[i].data, ents[i].size);
        }
        gbl_sha256(buf + payload_off[i], ents[i].size, e + 16);
    }

    /* Footer + header CRC. */
    memcpy(buf + total - GBLP1_FOOTER_SIZE, GBLP1_FOOTER, GBLP1_FOOTER_SIZE);
    wle32(buf + 24, gbl_crc32(buf, 24));

    *out_buf  = buf;
    *out_size = total;
    return GBL_PACK_OK;
}
```

Add `#include <string.h>` if not already present (it is) — `memcmp` is used. The `align_up`, `wle16`, `wle32` helpers and the existing includes stay.

- [ ] **Step 3: Add the `--mode2-profile` flag to `gbl-pack.c`**

In `tools/gbl-pack/gbl-pack.c`, add a `profile` variable and arg, and relax the required-args check. Change the arg-parse loop and validation:

```c
    const char *cached = NULL, *source = NULL, *extracted = NULL,
               *out = NULL, *profile = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cached-abl") && i + 1 < argc)  cached    = argv[++i];
        else if (!strcmp(argv[i], "--source")    && i + 1 < argc)  source    = argv[++i];
        else if (!strcmp(argv[i], "--extracted") && i + 1 < argc)  extracted = argv[++i];
        else if (!strcmp(argv[i], "--mode2-profile") && i + 1 < argc) profile = argv[++i];
        else if (!strcmp(argv[i], "--out")       && i + 1 < argc)  out       = argv[++i];
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (!out || (!cached && !profile)) {
        fprintf(stderr,
            "usage: gbl-pack --out OUT "
            "[--cached-abl PE --source RAW --extracted PE] "
            "[--mode2-profile BIN]\n");
        return 2;
    }
    if (cached && (!source || !extracted)) {
        fprintf(stderr,
            "gbl-pack: --cached-abl requires --source and --extracted\n");
        return 2;
    }
```

Then make the `slurp` calls conditional:

```c
    struct gbl_pack_inputs in = {0};
    if (cached) {
        if (slurp(cached,    (uint8_t **)&in.cached_abl, &in.cached_abl_size)) return 1;
        if (slurp(source,    (uint8_t **)&in.source,      &in.source_size))     return 1;
        if (slurp(extracted, (uint8_t **)&in.extracted,   &in.extracted_size))  return 1;
    }
    if (profile) {
        if (slurp(profile, (uint8_t **)&in.mode2_profile, &in.mode2_profile_size))
            return 1;
    }
```

The `in.packer_version` / `in.timestamp_iso8601` block, the `gbl_pack_build` call, and the file write stay unchanged.

- [ ] **Step 4: Write the test**

Create `tests/host/081_gbl_pack_mode2_profile.sh`:

```bash
#!/usr/bin/env bash
# tests/host/081_gbl_pack_mode2_profile.sh — gbl-pack --mode2-profile builds a
# profile-only GBLP1 0x0010 container the EDK2 parser can locate.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/gbl-pack
make -s -C tests/host/helpers parser_harness
GP=tools/gbl-pack/gbl-pack
H=tests/host/helpers/parser_harness
OUT=tests/host/.last/081
mkdir -p "$OUT"

# A valid 120-byte mode2_profile binary: GM2P, ver 1, reserved 0, then fields.
python3 - "$OUT/profile.bin" <<'PY'
import struct, sys
b  = b"GM2P" + struct.pack("<HHIIII", 1, 0, 0, 0, 0x40000, 0x9A4)
b += bytes(96)   # rot_digest + pubkey_digest + vbh
assert len(b) == 120, len(b)
open(sys.argv[1], "wb").write(b)
PY

# Profile-only container.
"$GP" --mode2-profile "$OUT/profile.bin" --out "$OUT/overlay.bin" 2>"$OUT/pack.log" \
  || { echo "FAIL: gbl-pack --mode2-profile failed"; cat "$OUT/pack.log"; exit 1; }
"$H" find-mode2-profile "$OUT/overlay.bin" | grep -q 'status=0' \
  || { echo "FAIL: 0x0010 entry not locatable in profile-only container"; exit 1; }

# Wrong-size profile -> rejected.
head -c 119 "$OUT/profile.bin" > "$OUT/short.bin"
"$GP" --mode2-profile "$OUT/short.bin" --out "$OUT/bad.bin" >/dev/null 2>&1 \
  && { echo "FAIL: gbl-pack accepted a 119-byte profile"; exit 1; } || true

# Bad magic -> rejected.
python3 - "$OUT/badmagic.bin" "$OUT/profile.bin" <<'PY'
import sys
b = bytearray(open(sys.argv[2],"rb").read()); b[0]=ord('X')
open(sys.argv[1],"wb").write(b)
PY
"$GP" --mode2-profile "$OUT/badmagic.bin" --out "$OUT/bad.bin" >/dev/null 2>&1 \
  && { echo "FAIL: gbl-pack accepted a bad-magic profile"; exit 1; } || true

# Neither input -> usage error.
"$GP" --out "$OUT/bad.bin" >/dev/null 2>&1 \
  && { echo "FAIL: gbl-pack accepted no inputs"; exit 1; } || true

echo "PASS: 081 gbl-pack mode2 profile"
```

- [ ] **Step 5: Run the test + regression-check the cached path**

Run: `bash tests/host/081_gbl_pack_mode2_profile.sh` → expect `PASS: 081 ...`
Run: `bash tests/host/060_pack_roundtrip.sh` and `bash tests/host/062_efisp_scan_gate.sh` → expect their existing PASS lines (or `SKIP:` if a fixture is missing). This confirms the cached-ABL path did not regress.

- [ ] **Step 6: Commit**

```bash
git add tools/gbl-pack/pack.h tools/gbl-pack/pack.c tools/gbl-pack/gbl-pack.c \
  tests/host/081_gbl_pack_mode2_profile.sh
git commit -m "feat(mode-2): gbl-pack --mode2-profile — embed 0x0010 entry"
```

---

### Task 4: `make-mode2-test-overlay.sh` orchestration

**Goal:** A single script that builds the mode-2 EFI, derives + compiles a profile, packs the `0x0010` overlay, and emits `dist/mode-2-test.efi`.

**Files:**
- Create: `scripts/make-mode2-test-overlay.sh`

**Acceptance Criteria:**
- [ ] `bash scripts/make-mode2-test-overlay.sh` (no args) runs build → derive → compile → gbl-pack → concat and produces `dist/mode-2-test.efi`, defaulting the vbmeta to `images/vbmeta-infiniti-IN-16.0.7.201.img`.
- [ ] The script accepts an optional vbmeta path argument: `bash scripts/make-mode2-test-overlay.sh <vbmeta.img>`.
- [ ] `dist/mode-2-test.efi` is larger than `dist/mode-2-debug-verbose.efi` (the overlay was appended), and `parser_harness find-mode2-profile` accepts the intermediate overlay container with `status=0`.
- [ ] The script fails loudly with a clear message if the vbmeta or `avbtool.py` is missing.

**Verify:** `bash scripts/make-mode2-test-overlay.sh` → final line `==> dist/mode-2-test.efi ready (<N> bytes)` and exit 0.

**Steps:**

- [ ] **Step 1: Write the orchestration script**

Create `scripts/make-mode2-test-overlay.sh`:

```bash
#!/usr/bin/env bash
# scripts/make-mode2-test-overlay.sh — build a staged-bootable mode-2 test EFI.
#
# Pipeline: build mode-2 EFI -> derive profile XML from a stock vbmeta ->
# compile to a 120-byte binary -> gbl-pack into a GBLP1 0x0010 overlay ->
# concatenate onto dist/mode-2-debug-verbose.efi -> dist/mode-2-test.efi
#
# Usage: scripts/make-mode2-test-overlay.sh [vbmeta.img]
#   vbmeta.img defaults to images/vbmeta-infiniti-IN-16.0.7.201.img
#
# Test on device with:
#   fastboot stage dist/mode-2-test.efi
#   fastboot oem boot-efi
set -euo pipefail
cd "$(dirname "$0")/.."

VBMETA="${1:-images/vbmeta-infiniti-IN-16.0.7.201.img}"
AVBTOOL="${AVBTOOL:-$HOME/avbtool.py}"
OUT=dist/mode-2-test-build
M2P=tools/mode2-profile/mode2-profile.py
BUILT_EFI=dist/mode-2-debug-verbose.efi

[ -f "$VBMETA" ]  || { echo "error: vbmeta not found: $VBMETA" >&2; exit 1; }
[ -f "$AVBTOOL" ] || { echo "error: avbtool.py not found at $AVBTOOL (set AVBTOOL=)" >&2; exit 1; }

mkdir -p "$OUT" dist

echo "==> Compiling gbl-pack (fail-fast before slow Docker build)"
make -s -C tools/gbl-pack

echo "==> Building mode-2 EFI"
./scripts/build.sh --mode 2 --debug --verbose
[ -f "$BUILT_EFI" ] || { echo "error: build did not produce $BUILT_EFI" >&2; exit 1; }

echo "==> Deriving profile from $VBMETA"
AVBTOOL="$AVBTOOL" python3 "$M2P" derive "$VBMETA" -o "$OUT/profile.xml"

echo "==> Compiling profile"
python3 "$M2P" compile "$OUT/profile.xml" -o "$OUT/profile.bin"

echo "==> Packing GBLP1 0x0010 overlay"
tools/gbl-pack/gbl-pack --mode2-profile "$OUT/profile.bin" --out "$OUT/overlay.bin"

echo "==> Concatenating overlay onto $BUILT_EFI"
cat "$BUILT_EFI" "$OUT/overlay.bin" > dist/mode-2-test.efi

SZ=$(stat -c%s dist/mode-2-test.efi)
echo "==> dist/mode-2-test.efi ready ($SZ bytes)"
echo "    test on device:  fastboot stage dist/mode-2-test.efi && fastboot oem boot-efi"
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x scripts/make-mode2-test-overlay.sh`

- [ ] **Step 3: Run it and verify the output**

Run: `bash scripts/make-mode2-test-overlay.sh`
Expected: the four `==>` stage lines, then `==> dist/mode-2-test.efi ready (<N> bytes)`, exit 0.

Then verify the appended overlay is locatable:

```bash
make -s -C tests/host/helpers parser_harness
tests/host/helpers/parser_harness find-mode2-profile dist/mode-2-test-build/overlay.bin
```

Expected: `status=0`.

Then confirm the concat actually grew the file:

```bash
test "$(stat -c%s dist/mode-2-test.efi)" -gt "$(stat -c%s dist/mode-2-debug-verbose.efi)" \
  && echo "concat OK"
```

Expected: `concat OK`.

- [ ] **Step 4: Commit**

```bash
git add scripts/make-mode2-test-overlay.sh
git commit -m "feat(mode-2): make-mode2-test-overlay.sh — staged-bootable test EFI"
```

---

## Final verification

```bash
bash tests/host/079_mode2_profile_derive.sh
bash tests/host/080_mode2_profile_compile.sh
bash tests/host/081_gbl_pack_mode2_profile.sh
bash tests/host/060_pack_roundtrip.sh
bash tests/host/062_efisp_scan_gate.sh
bash scripts/make-mode2-test-overlay.sh
```

Expected: 080 and 081 print `PASS:`; 079/060/062 print `PASS:` or a clean `SKIP:` (fixture/avbtool absent); `make-mode2-test-overlay.sh` ends with `==> dist/mode-2-test.efi ready`.

On-device validation (manual, user-run — not part of automated execution): `fastboot stage dist/mode-2-test.efi` + `fastboot oem boot-efi`; under the `--debug --verbose` build the logfs/screen shows `mode2 | profile set ...` and `mode2 | km-rewrite | cmd=0x00000201` / `0x00000208` / `0x00000211` / `0x00000207` lines plus `mode2 | spss-rewrite`.

## Follow-up (out of scope)

- The device-side aarch64 `mode2-profile` equivalent (a ZIP/recovery-side profile parser).
- The `images/`-drop full build orchestration and the production cached-ABL + profile combined overlay.
- The mode-2 ZIP.
