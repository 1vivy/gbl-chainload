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
  rm -f "$OUT/reject.bin"
  if python3 "$M2P" compile "$2" -o "$OUT/reject.bin" >/dev/null 2>&1; then
    echo "FAIL: compile accepted bad input ($1)"; exit 1
  fi
  if [ -f "$OUT/reject.bin" ]; then
    echo "FAIL: $1 left an output file behind"; exit 1
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

# Boundary case: max valid values (is-unlocked=1, color=3 = GBL_M2P_COLOR_RED).
cat > "$OUT/good_max.xml" <<'XML'
<gbl-chainload-mode2-profile version="1">
  <is-unlocked>1</is-unlocked>
  <color>3</color>
  <system-version>0x40000</system-version>
  <system-spl>0x9A4</system-spl>
  <rot-digest>1111111111111111111111111111111111111111111111111111111111111111</rot-digest>
  <pubkey-digest>2222222222222222222222222222222222222222222222222222222222222222</pubkey-digest>
  <vbh>3333333333333333333333333333333333333333333333333333333333333333</vbh>
</gbl-chainload-mode2-profile>
XML

python3 "$M2P" compile "$OUT/good_max.xml" -o "$OUT/good_max.bin" >"$OUT/compile_max.log" 2>&1 \
  || { echo "FAIL: compile rejected max-valid profile"; cat "$OUT/compile_max.log"; exit 1; }
"$H" profile-parse "$OUT/good_max.bin" | grep -q 'status=0' \
  || { echo "FAIL: EDK2 parser rejected max-valid profile (is-unlocked=1 color=3)"; exit 1; }

echo "PASS: 080 mode2 profile compile"
