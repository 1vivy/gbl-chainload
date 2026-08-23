#!/usr/bin/env bash
# tests/host/097_myron_mode2_package.sh — Xiaomi myron (sm8850) mode-2
# end-to-end host check.
#
# Myron is the first non-oplus mode-2 target (Redmi, HyperOS OS3.0.23 /
# Android 16). Two device-specific properties are pinned here:
#
#   1. Its stock vbmeta carries only GKI-era `init_boot.*` property
#      descriptors (no `com.android.build.boot.*`), so `gbl mode2
#      derive/build` must exercise the init_boot fallback.
#   2. Its ABL is the standard Qualcomm ELF→FV→LZMA→PE32 wrapper, and the
#      mandatory abl_permissive contract (patch6 + patch10) must hold on
#      it (also asserted cross-build in 088).
#
# The test drives the shippable path (`gbl mode2 build`, then
# efisp-package.py --mode 2) and verifies the produced GBLP1 overlay:
# cached patched ABL + 120-byte GM2P profile + manifest bits EXACTLY
# 0x0002 (WantProfileSpoof only — no fakelock bit, per the myron mode-2
# scope: no orange-state-hiding patch).
set -euo pipefail
cd "$(dirname "$0")/../.."

ABL=tests/images/xi-myron-OS3.0.23-abl.img
VBMETA=tests/images/vbmeta-myron-OS3.0.23.img

[ -f "$ABL" ]    || { echo "SKIP: 097 — $ABL missing"; exit 0; }
[ -f "$VBMETA" ] || { echo "SKIP: 097 — $VBMETA missing"; exit 0; }

# Reproducible gbl pack output (see 060_pack_roundtrip.sh).
: "${SOURCE_DATE_EPOCH:=0}"
export SOURCE_DATE_EPOCH

cargo build --release --quiet -p gbl
make -s -C tests/host/helpers parser_harness
H=tests/host/helpers/parser_harness
PATH="$PWD/target/release:$PATH"; export PATH

OUT=tests/host/.last/097
rm -rf "$OUT"; mkdir -p "$OUT"

# --- 1. mode2 profile: derive+compile from the myron stock vbmeta --------
gbl mode2 build "$VBMETA" -o "$OUT/profile.bin" >"$OUT/mode2-build.log" 2>&1 \
  || { echo "FAIL: gbl mode2 build (init_boot fallback?)"; cat "$OUT/mode2-build.log"; exit 1; }

[ "$(stat -c%s "$OUT/profile.bin")" -eq 120 ] \
  || { echo "FAIL: profile.bin is not 120 bytes"; exit 1; }

# Wire-format goldens for the committed fixture: magic GM2P, version 1,
# is_unlocked=0 / color=0 (locked + GREEN), system_version = 0x00040000
# (os_version "16" -> 16<<14), system_spl = 0x000009a1 ("2026-01-01").
python3 - "$OUT/profile.bin" <<'EOF' || exit 1
import struct, sys
b = open(sys.argv[1], "rb").read()
assert len(b) == 120, "size"
assert b[0:4] == b"GM2P", "magic"
assert struct.unpack_from("<H", b, 4)[0] == 1, "version"
assert struct.unpack_from("<H", b, 6)[0] == 0, "reserved"
assert struct.unpack_from("<I", b, 8)[0] == 0, "is_unlocked"
assert struct.unpack_from("<I", b, 12)[0] == 0, "color"
assert struct.unpack_from("<I", b, 16)[0] == 0x40000, "system_version"
assert struct.unpack_from("<I", b, 20)[0] == 0x9A1, "system_spl"
assert any(b[24:56]), "rot_digest all zero"
assert any(b[56:88]), "pubkey_digest all zero"
assert any(b[88:120]), "vbh all zero"
print("profile wire-form ok")
EOF

# --- 2. efisp-package --mode 2 over the myron ABL ------------------------
# Throwaway base EFI (085 pattern): efisp-package.py only concatenates the
# base, so a small MZ-headed file is enough for the structural check.
printf 'MZ' > "$OUT/base.efi"
head -c 4096 /dev/zero >> "$OUT/base.efi"

python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 2 --efi "$OUT/base.efi" \
  --stock-vbmeta "$VBMETA" --out "$OUT/installed.efi" \
  >"$OUT/package.log" 2>&1 \
  || { echo "FAIL: efisp-package.py --mode 2 on myron inputs"; cat "$OUT/package.log"; exit 1; }

[ -s "$OUT/installed.efi" ] || { echo "FAIL: empty output image"; exit 1; }

# --- 3. GBLP1 overlay contents --------------------------------------------
# Strip the base-EFI prefix so the buffer starts at the GBLP1 magic
# (recovery-install-validation.md B1 pattern); the find-* harness entry
# points expect a container at offset 0.
BASE_SZ=$(stat -c%s "$OUT/base.efi")
tail -c +"$((BASE_SZ + 1))" "$OUT/installed.efi" > "$OUT/payload.bin"

"$H" find-cached-abl "$OUT/payload.bin" >"$OUT/cached.log" 2>&1 \
  || { echo "FAIL: no cached-ABL entry"; cat "$OUT/cached.log"; exit 1; }
grep -q 'status=0' "$OUT/cached.log" \
  || { echo "FAIL: cached-ABL status != 0"; cat "$OUT/cached.log"; exit 1; }

"$H" find-mode2-profile "$OUT/payload.bin" >"$OUT/profile.log" 2>&1 \
  || { echo "FAIL: no mode2-profile entry"; cat "$OUT/profile.log"; exit 1; }
grep -q 'status=0 size=120' "$OUT/profile.log" \
  || { echo "FAIL: mode2-profile not 120 bytes"; cat "$OUT/profile.log"; exit 1; }

"$H" find-manifest "$OUT/payload.bin" >"$OUT/manifest.log" 2>&1 \
  || { echo "FAIL: no manifest entry"; cat "$OUT/manifest.log"; exit 1; }
grep -q 'present=1' "$OUT/manifest.log" \
  || { echo "FAIL: manifest present != 1"; cat "$OUT/manifest.log"; exit 1; }
# Mode 2 — and ONLY mode 2: WantProfileSpoof set, WantFakelockHook clear.
grep -q 'bits=0x0002' "$OUT/manifest.log" \
  || { echo "FAIL: manifest bits != 0x0002 (mode-2 only, no fakelock)"; cat "$OUT/manifest.log"; exit 1; }

# --- 4. gbl inspect agrees -------------------------------------------------
gbl inspect "$OUT/installed.efi" >"$OUT/inspect.log" 2>&1 \
  || { echo "FAIL: gbl inspect non-zero"; cat "$OUT/inspect.log"; exit 1; }
grep -q 'result: ok' "$OUT/inspect.log" \
  || { echo "FAIL: gbl inspect result != ok"; cat "$OUT/inspect.log"; exit 1; }
grep -q 'profile_spoof=yes' "$OUT/inspect.log" \
  || { echo "FAIL: inspect profile_spoof != yes"; cat "$OUT/inspect.log"; exit 1; }
grep -q 'fakelock_hook=no' "$OUT/inspect.log" \
  || { echo "FAIL: inspect fakelock_hook != no"; cat "$OUT/inspect.log"; exit 1; }

echo "PASS: 097 myron mode2 package"
