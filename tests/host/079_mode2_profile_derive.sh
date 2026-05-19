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
  derive "$VBMETA" -o "$OUT/profile.toml" >"$OUT/derive.log" 2>&1 \
  || { echo "FAIL: derive failed"; cat "$OUT/derive.log"; exit 1; }

# TOML must contain version = 1.
grep -q '^version *= *1' "$OUT/profile.toml" \
  || { echo "FAIL: 'version = 1' missing from TOML"; exit 1; }

# All scalar keys must be present.
for key in is_unlocked color system_version system_spl; do
  grep -q "^${key} *=" "$OUT/profile.toml" \
    || { echo "FAIL: '${key}' missing from TOML"; exit 1; }
done

# Digest keys must be quoted 64-hex strings — check each one individually.
for key in rot_digest pubkey_digest vbh; do
  grep -Eq "^${key} *= *\"[0-9a-f]{64}\"" "$OUT/profile.toml" \
    || { echo "FAIL: '${key}' missing or malformed in TOML"; exit 1; }
done

# Encoded numeric values must be non-zero hex (catches encoder regressions that emit 0x0).
grep -Eq '^system_version *= *0x[0-9a-f]*[1-9a-f][0-9a-f]*' "$OUT/profile.toml" \
  || { echo "FAIL: system_version is missing or zero"; exit 1; }
grep -Eq '^system_spl *= *0x[0-9a-f]*[1-9a-f][0-9a-f]*' "$OUT/profile.toml" \
  || { echo "FAIL: system_spl is missing or zero"; exit 1; }

echo "PASS: 079 mode2 profile derive"
