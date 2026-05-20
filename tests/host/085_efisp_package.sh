#!/usr/bin/env bash
# tests/host/085_efisp_package.sh — efisp-package.py chains the host-side
# tools into a mode-N.efi + GBLP1 overlay the EDK2 parser can locate.
set -euo pipefail
cd "$(dirname "$0")/../.."

# Build the host tools and the parser harness this test needs.
make -s -C tools/fv-unwrap
make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tests/host/helpers parser_harness
H=tests/host/helpers/parser_harness
OUT=tests/host/.last/085
mkdir -p "$OUT/tools"

# efisp-package.py locates tools via --tools-dir / dist/<platform>/ /
# script-dir / PATH — a Linux host build (tools/<t>/<t>) is in none of
# those, so stage the three needed binaries into one dir and pass it.
cp tools/fv-unwrap/fv-unwrap tools/abl-patcher/abl-patcher \
   tools/gbl-pack/gbl-pack "$OUT/tools/"

# An fv-unwrap input is a raw ABL partition (LZMA-FV wrapped). The
# tests/images/ dir also holds non-ABL fixtures (grafted-recovery.img,
# vbmeta-*.img, …), so glob specifically for an *abl*.img.
ABL=$(ls tests/images/*abl*.img 2>/dev/null | head -1 || true)
[ -n "$ABL" ] || { echo "SKIP: 085 — no tests/images/*abl*.img fixture present"; exit 0; }

# A throwaway base EFI: efisp-package.py just concatenates it, so any
# small file with a PE 'MZ' header is enough for the structural check.
printf 'MZ' > "$OUT/base.efi"
head -c 4096 /dev/zero >> "$OUT/base.efi"

# mode 1 — plain abl-patcher, no profile.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 1 --efi "$OUT/base.efi" \
  --tools-dir "$OUT/tools" --out "$OUT/mode1.efi" \
  >"$OUT/m1.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 1 failed"; cat "$OUT/m1.log"; exit 1; }
"$H" scan-cached-abl "$OUT/mode1.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-1 output has no locatable cached-ABL overlay"; exit 1; }

# mode 0 — abl-patcher --no-mode1.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 0 --efi "$OUT/base.efi" \
  --tools-dir "$OUT/tools" --out "$OUT/mode0.efi" \
  >"$OUT/m0.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 0 failed"; cat "$OUT/m0.log"; exit 1; }
"$H" scan-cached-abl "$OUT/mode0.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-0 output has no locatable cached-ABL overlay"; exit 1; }

# pre-flight gate: mode 2 without --stock-vbmeta must abort non-zero.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 2 --efi "$OUT/base.efi" --out "$OUT/bad.efi" \
  >/dev/null 2>&1 \
  && { echo "FAIL: mode 2 accepted without --stock-vbmeta"; exit 1; } || true

# pre-flight gate: --stock-vbmeta on mode 1 must abort non-zero.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 1 --efi "$OUT/base.efi" --stock-vbmeta "$ABL" \
  --out "$OUT/bad.efi" >/dev/null 2>&1 \
  && { echo "FAIL: --stock-vbmeta accepted on mode 1"; exit 1; } || true

echo "PASS: 085 efisp package"
