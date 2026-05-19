#!/usr/bin/env bash
# tests/host/082_mode2_profile_parity.sh — parity check: C mode2-profile tool
# produces byte-identical output to the Python tool for the same TOML input.
# The derive half (Task 3) is added in the next task; this test covers compile.
set -euo pipefail
cd "$(dirname "$0")/../.."

PY=tools/mode2-profile/mode2-profile.py
C_TOOL=tools/mode2-profile/mode2-profile
OUT=tests/host/.last/082
mkdir -p "$OUT"

# Build the C tool.
make -s -C tools/mode2-profile

# ---- compile parity ----

# A well-formed profile TOML (same fixture as 080).
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

python3 "$PY" compile "$OUT/good.toml" -o "$OUT/py.bin" >"$OUT/py_compile.log" 2>&1 \
  || { echo "FAIL: Python compile failed"; cat "$OUT/py_compile.log"; exit 1; }

"$C_TOOL" compile "$OUT/good.toml" -o "$OUT/c.bin" >"$OUT/c_compile.log" 2>&1 \
  || { echo "FAIL: C compile failed"; cat "$OUT/c_compile.log"; exit 1; }

# Both must be exactly 120 bytes.
sz_py=$(stat -c%s "$OUT/py.bin")
sz_c=$(stat -c%s "$OUT/c.bin")
[ "$sz_py" -eq 120 ] || { echo "FAIL: Python output is $sz_py bytes, expected 120"; exit 1; }
[ "$sz_c"  -eq 120 ] || { echo "FAIL: C output is $sz_c bytes, expected 120"; exit 1; }

# Byte-identical.
cmp "$OUT/py.bin" "$OUT/c.bin" \
  || { echo "FAIL: C and Python compile outputs differ"; exit 1; }

# ---- rejection parity: color = 9 ----

sed 's/^color *= *0/color = 9/' "$OUT/good.toml" > "$OUT/badcolor.toml"

# Python must reject.
if python3 "$PY" compile "$OUT/badcolor.toml" -o "$OUT/py_reject.bin" >/dev/null 2>&1; then
  echo "FAIL: Python accepted color=9"; exit 1
fi
[ ! -f "$OUT/py_reject.bin" ] \
  || { echo "FAIL: Python left output file after rejection (color=9)"; exit 1; }

# C tool must reject.
if "$C_TOOL" compile "$OUT/badcolor.toml" -o "$OUT/c_reject.bin" >/dev/null 2>&1; then
  echo "FAIL: C tool accepted color=9"; exit 1
fi
[ ! -f "$OUT/c_reject.bin" ] \
  || { echo "FAIL: C tool left output file after rejection (color=9)"; exit 1; }

# ---- (Task 3 derive parity block goes here) ----

echo "PASS: 082 mode2-profile parity"
