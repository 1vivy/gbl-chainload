#!/usr/bin/env bash
# 055_recovery_graft_lint.sh — host lint for recovery graft tooling/ZIP.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
for path in \
  scripts/graft-recovery-vbmeta.py \
  scripts/make-recovery-graft-zip.sh \
  zip/recovery-graft/META-INF/com/google/android/update-binary; do
  [[ -f "$path" ]] || { echo "FAIL: missing $path" >&2; fail=1; }
  [[ -x "$path" ]] || { echo "FAIL: $path must be executable" >&2; fail=1; }
done

grep -q 'AVBf' scripts/graft-recovery-vbmeta.py || { echo "FAIL: graft tool must parse AVB footer" >&2; fail=1; }
grep -q 'round_up' scripts/graft-recovery-vbmeta.py || { echo "FAIL: graft tool must align graft offset" >&2; fail=1; }
grep -q 'recovery_sha256' scripts/make-recovery-graft-zip.sh || { echo "FAIL: recovery ZIP manifest must include hash" >&2; fail=1; }
grep -q 'dd if=.*of=.*TARGET' zip/recovery-graft/META-INF/com/google/android/update-binary || { echo "FAIL: installer must explicitly write selected recovery target" >&2; fail=1; }
grep -q 'recovery-before-graft' zip/recovery-graft/META-INF/com/google/android/update-binary || { echo "FAIL: installer must back up current recovery" >&2; fail=1; }

python3 -m py_compile scripts/graft-recovery-vbmeta.py

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
python3 - "$TMP" <<'PY'
from pathlib import Path
import struct, sys
tmp = Path(sys.argv[1])
custom = b"CUSTOM" * 100
vbmeta = b"AVB0" + b"V" * 124
footer = bytearray(64)
footer[:4] = b"AVBf"
footer[4:8] = (1).to_bytes(4, "big")
footer[12:20] = (4096).to_bytes(8, "big")
footer[20:28] = (4096).to_bytes(8, "big")
footer[28:36] = (len(vbmeta)).to_bytes(8, "big")
stock = bytearray(4096 + len(vbmeta) + 64)
stock[:5] = b"STOCK"
stock[4096:4096+len(vbmeta)] = vbmeta
stock[-64:] = footer
(tmp / "custom.img").write_bytes(custom)
(tmp / "stock.img").write_bytes(stock)
PY
scripts/graft-recovery-vbmeta.py \
  --custom "$TMP/custom.img" \
  --stock "$TMP/stock.img" \
  --out "$TMP/out.img" \
  --partition-size 8192 >/dev/null
python3 - "$TMP/out.img" <<'PY'
from pathlib import Path
import struct, sys
data = Path(sys.argv[1]).read_bytes()
assert data[4096:4100] == b"AVB0"
assert data[-64:-60] == b"AVBf"
assert struct.unpack(">Q", data[-44:-36])[0] == 4096
print("ok synthetic recovery graft")
PY

[[ $fail -eq 0 ]] && echo "ok 055_recovery_graft_lint" || exit 1
