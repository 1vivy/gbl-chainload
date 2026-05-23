#!/usr/bin/env bash
# tests/host/085_efisp_package.sh — efisp-package.py chains the host-side
# tools into a single-EFI + GBLP1 overlay the EDK2 parser can locate.
#
# Post-Task-13 invariants:
#   * abl-patcher is invoked WITHOUT --no-mode1 (flag retired Task 12).
#   * gbl-pack is invoked WITH --manifest 0x0N derived from --mode N.
#   * --oem is allowed for ANY mode (decoupled from --mode 2).
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

# Install argv-recording shims for abl-patcher and gbl-pack so we can
# assert the exact argv shape efisp-package.py passes. The shims forward
# to the real binary after recording $@ on disk.
REAL=$OUT/tools/real
mkdir -p "$REAL"
mv "$OUT/tools/abl-patcher" "$REAL/abl-patcher"
mv "$OUT/tools/gbl-pack"    "$REAL/gbl-pack"

cat > "$OUT/tools/abl-patcher" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$@" > "$OUT/abl-patcher.argv"
exec "$REAL/abl-patcher" "\$@"
EOF
cat > "$OUT/tools/gbl-pack" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$@" > "$OUT/gbl-pack.argv"
exec "$REAL/gbl-pack" "\$@"
EOF
chmod +x "$OUT/tools/abl-patcher" "$OUT/tools/gbl-pack"

# An fv-unwrap input is a raw ABL partition (LZMA-FV wrapped). The
# tests/images/ dir also holds non-ABL fixtures (grafted-recovery.img,
# vbmeta-*.img, …), so glob specifically for an *abl*.img.
ABL=$(ls tests/images/*abl*.img 2>/dev/null | head -1 || true)
[ -n "$ABL" ] || { echo "SKIP: 085 — no tests/images/*abl*.img fixture present"; exit 0; }

# A throwaway base EFI: efisp-package.py just concatenates it, so any
# small file with a PE 'MZ' header is enough for the structural check.
# (Post-Task-12 this is a single gbl-chainload.efi; the script is
# name-agnostic — verified by the "any path works" cases below.)
printf 'MZ' > "$OUT/base.efi"
head -c 4096 /dev/zero >> "$OUT/base.efi"

# assert_argv FILE NEEDLE LABEL — fail if NEEDLE missing from argv FILE.
assert_argv() {
  grep -qxF -e "$2" "$1" \
    || { echo "FAIL: $3 — expected argv line '$2' in $1"; cat "$1"; exit 1; }
}

# assert_no_argv FILE NEEDLE LABEL — fail if NEEDLE present in argv FILE.
assert_no_argv() {
  if grep -qxF -e "$2" "$1"; then
    echo "FAIL: $3 — unexpected argv line '$2' in $1"; cat "$1"; exit 1
  fi
}

# mode 1 — plain abl-patcher (no --oem), gbl-pack gets --manifest 0x01.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 1 --efi "$OUT/base.efi" \
  --tools-dir "$OUT/tools" --out "$OUT/mode1.efi" \
  >"$OUT/m1.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 1 failed"; cat "$OUT/m1.log"; exit 1; }
"$H" scan-cached-abl "$OUT/mode1.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-1 output has no locatable cached-ABL overlay"; exit 1; }
assert_no_argv "$OUT/abl-patcher.argv" "--no-mode1" "mode 1 abl-patcher"
assert_no_argv "$OUT/abl-patcher.argv" "--oem"      "mode 1 abl-patcher (no --oem)"
assert_argv    "$OUT/gbl-pack.argv"    "--manifest" "mode 1 gbl-pack manifest flag"
assert_argv    "$OUT/gbl-pack.argv"    "0x01"       "mode 1 gbl-pack manifest bits"

# mode 0 — plain abl-patcher (no --oem, no --no-mode1), gbl-pack --manifest 0x00.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 0 --efi "$OUT/base.efi" \
  --tools-dir "$OUT/tools" --out "$OUT/mode0.efi" \
  >"$OUT/m0.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 0 failed"; cat "$OUT/m0.log"; exit 1; }
"$H" scan-cached-abl "$OUT/mode0.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-0 output has no locatable cached-ABL overlay"; exit 1; }
assert_no_argv "$OUT/abl-patcher.argv" "--no-mode1" "mode 0 abl-patcher"
assert_no_argv "$OUT/abl-patcher.argv" "--oem"      "mode 0 abl-patcher (no --oem)"
assert_argv    "$OUT/gbl-pack.argv"    "--manifest" "mode 0 gbl-pack manifest flag"
assert_argv    "$OUT/gbl-pack.argv"    "0x00"       "mode 0 gbl-pack manifest bits"

# mode 0 + --oem — now allowed (decoupled from --mode 2). abl-patcher must
# receive --oem oplus; gbl-pack still gets --manifest 0x00.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 0 --efi "$OUT/base.efi" --oem oplus \
  --tools-dir "$OUT/tools" --out "$OUT/mode0-oem.efi" \
  >"$OUT/m0oem.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 0 + --oem failed"; cat "$OUT/m0oem.log"; exit 1; }
"$H" scan-cached-abl "$OUT/mode0-oem.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-0-with-oem output has no locatable cached-ABL overlay"; exit 1; }
assert_no_argv "$OUT/abl-patcher.argv" "--no-mode1" "mode 0+oem abl-patcher"
assert_argv    "$OUT/abl-patcher.argv" "--oem"      "mode 0+oem abl-patcher --oem flag"
assert_argv    "$OUT/abl-patcher.argv" "oplus"      "mode 0+oem abl-patcher --oem value"
assert_argv    "$OUT/gbl-pack.argv"    "0x00"       "mode 0+oem gbl-pack manifest bits"

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

# --oem on mode 0 must NOT be gated any more (covered by mode 0+oem case
# above; this is the explicit negative-of-the-old-gate assertion).
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 0 --efi "$OUT/base.efi" --oem oplus \
  --tools-dir "$OUT/tools" --out "$OUT/mode0-oem2.efi" \
  >/dev/null 2>&1 \
  || { echo "FAIL: --oem rejected on mode 0 (old mode-2-only gate still firing)"; exit 1; }

echo "PASS: 085 efisp package"
