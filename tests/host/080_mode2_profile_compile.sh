#!/usr/bin/env bash
# tests/host/080_mode2_profile_compile.sh — compile a profile TOML and confirm
# the EDK2 parser accepts the resulting binary.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers mode2_harness
H=tests/host/helpers/mode2_harness
M2P=tools/mode2-profile/mode2-profile.py
OUT=tests/host/.last/080
mkdir -p "$OUT"

# A well-formed profile TOML.
cat > "$OUT/good.toml" <<'TOML'
version        = 1
is_unlocked    = 0
color          = 0
system_version = 0x40000
system_spl     = 0x9A4
rot_digest     = "1111111111111111111111111111111111111111111111111111111111111111"
pubkey_digest  = "2222222222222222222222222222222222222222222222222222222222222222"
vbh            = "3333333333333333333333333333333333333333333333333333333333333333"
TOML

python3 "$M2P" compile "$OUT/good.toml" -o "$OUT/good.bin" >"$OUT/compile.log" 2>&1 \
  || { echo "FAIL: compile failed"; cat "$OUT/compile.log"; exit 1; }

# Exactly 120 bytes.
sz=$(stat -c%s "$OUT/good.bin")
[ "$sz" -eq 120 ] || { echo "FAIL: binary is $sz bytes, expected 120"; exit 1; }

# The EDK2 parser must accept it.
"$H" profile-parse "$OUT/good.bin" | grep -q 'status=0' \
  || { echo "FAIL: EDK2 parser rejected the compiled profile"; exit 1; }

# Rejection cases — each must exit non-zero and not write output.
reject() {  # <name> <toml-file>
  rm -f "$OUT/reject.bin"
  if python3 "$M2P" compile "$2" -o "$OUT/reject.bin" >/dev/null 2>&1; then
    echo "FAIL: compile accepted bad input ($1)"; exit 1
  fi
  if [ -f "$OUT/reject.bin" ]; then
    echo "FAIL: $1 left an output file behind"; exit 1
  fi
}

# bad version
sed 's/^version *= *1/version = 2/' "$OUT/good.toml" > "$OUT/badver.toml"
reject "version!=1" "$OUT/badver.toml"

# color > 3
sed 's/^color *= *0/color = 9/' "$OUT/good.toml" > "$OUT/badcolor.toml"
reject "color>3" "$OUT/badcolor.toml"

# color < 0 (negative integer — valid TOML, caught by 0 <= v guard)
sed 's/^color *= *0/color = -1/' "$OUT/good.toml" > "$OUT/badcolor_neg.toml"
reject "color<0" "$OUT/badcolor_neg.toml"

# is_unlocked > 1
sed 's/^is_unlocked *= *0/is_unlocked = 5/' "$OUT/good.toml" > "$OUT/badunlk.toml"
reject "is_unlocked>1" "$OUT/badunlk.toml"

# vbh digest too short (2 hex chars)
sed 's/^vbh *= *"33*/vbh = "33/' "$OUT/good.toml" > "$OUT/badvbh.toml"
reject "short vbh digest" "$OUT/badvbh.toml"

# unknown key
sed '/^version/a unknownkey = 1' "$OUT/good.toml" > "$OUT/unknownkey.toml"
reject "unknown key" "$OUT/unknownkey.toml"

# malformed TOML (truncated)
printf 'version = 1\nis_unlocked = ' > "$OUT/malformed.toml"
reject "malformed TOML" "$OUT/malformed.toml"

# Boundary case: max valid values (is_unlocked=1, color=3 = GBL_M2P_COLOR_RED).
cat > "$OUT/good_max.toml" <<'TOML'
version        = 1
is_unlocked    = 1
color          = 3
system_version = 0x40000
system_spl     = 0x9A4
rot_digest     = "1111111111111111111111111111111111111111111111111111111111111111"
pubkey_digest  = "2222222222222222222222222222222222222222222222222222222222222222"
vbh            = "3333333333333333333333333333333333333333333333333333333333333333"
TOML

python3 "$M2P" compile "$OUT/good_max.toml" -o "$OUT/good_max.bin" >"$OUT/compile_max.log" 2>&1 \
  || { echo "FAIL: compile rejected max-valid profile"; cat "$OUT/compile_max.log"; exit 1; }
"$H" profile-parse "$OUT/good_max.bin" | grep -q 'status=0' \
  || { echo "FAIL: EDK2 parser rejected max-valid profile (is_unlocked=1 color=3)"; exit 1; }

echo "PASS: 080 mode2 profile compile"
