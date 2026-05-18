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

# Encoded numeric values must be non-zero hex (catches encoder regressions that emit 0x0).
grep -Eq '<system-version>0x[0-9a-f]*[1-9a-f][0-9a-f]*<' "$OUT/profile.xml" \
  || { echo "FAIL: <system-version> is missing or zero"; exit 1; }
grep -Eq '<system-spl>0x[0-9a-f]*[1-9a-f][0-9a-f]*<' "$OUT/profile.xml" \
  || { echo "FAIL: <system-spl> is missing or zero"; exit 1; }

echo "PASS: 079 mode2 profile derive"
