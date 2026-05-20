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

# ---- derive parity (SKIP-guarded if fixture or avbtool absent) ----

VBMETA="tests/images/vbmeta-infiniti-IN-16.0.7.201.img"
AVBTOOL="${AVBTOOL:-$HOME/avbtool.py}"

if [ -f "$VBMETA" ] && [ -f "$AVBTOOL" ]; then
  python3 "$PY" derive "$VBMETA" -o "$OUT/py_derive.toml" \
    >"$OUT/py_derive.log" 2>&1 \
    || { echo "FAIL: Python derive failed"; cat "$OUT/py_derive.log"; exit 1; }

  "$C_TOOL" derive "$VBMETA" -o "$OUT/c_derive.toml" \
    >"$OUT/c_derive.log" 2>&1 \
    || { echo "FAIL: C derive failed"; cat "$OUT/c_derive.log"; exit 1; }

  # TOML outputs must be byte-identical.
  cmp "$OUT/py_derive.toml" "$OUT/c_derive.toml" \
    || { echo "FAIL: C and Python derive TOML outputs differ"; exit 1; }

  # Now compile both derived TOMLs and verify the binaries are also identical.
  python3 "$PY" compile "$OUT/py_derive.toml" -o "$OUT/py_derive.bin" \
    >"$OUT/py_derive_compile.log" 2>&1 \
    || { echo "FAIL: Python compile of derived TOML failed"; \
         cat "$OUT/py_derive_compile.log"; exit 1; }

  "$C_TOOL" compile "$OUT/c_derive.toml" -o "$OUT/c_derive.bin" \
    >"$OUT/c_derive_compile.log" 2>&1 \
    || { echo "FAIL: C compile of derived TOML failed"; \
         cat "$OUT/c_derive_compile.log"; exit 1; }

  cmp "$OUT/py_derive.bin" "$OUT/c_derive.bin" \
    || { echo "FAIL: derived TOML -> binary: C and Python outputs differ"; exit 1; }

  echo "  derive parity: PASS (vbmeta fixture found)"
else
  echo "  derive parity: SKIP (fixture or avbtool absent)"
fi

echo "PASS: 082 mode2-profile parity"
