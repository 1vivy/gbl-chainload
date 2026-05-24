#!/usr/bin/env bash
# tests/host/085_efisp_package.sh — efisp-package.py chains the host-side
# tools into a single-EFI + GBLP1 overlay the EDK2 parser can locate.
#
# Post-Task-13 invariants (PR1):
#   * gbl patch is invoked WITHOUT --no-mode1 (flag retired Task 12).
#   * gbl pack  is invoked WITH --manifest 0x0N derived from --mode N.
#   * --oem is allowed for ANY mode (decoupled from --mode 2).
#
# PR2 Task 8: the 7 host C tools collapsed into the `gbl` multicall;
# efisp-package.py now calls `gbl <sub>` rather than the standalone
# binaries. The argv-recording shim below wraps `gbl` itself and
# dispatches argv-capture per subcommand so we can assert what reached
# each one.
set -euo pipefail
cd "$(dirname "$0")/../.."

# Reproducible gbl pack output (see 060_pack_roundtrip.sh) — the shim chain
# inherits this env into the real gbl invoked by efisp-package.py.
: "${SOURCE_DATE_EPOCH:=0}"
export SOURCE_DATE_EPOCH

cargo build --release --quiet -p gbl
make -s -C tests/host/helpers parser_harness
H=tests/host/helpers/parser_harness
OUT=tests/host/.last/085
rm -rf "$OUT"; mkdir -p "$OUT/tools"

# Stage the real gbl multicall behind an argv-recording shim. The shim
# splits on the first argument (the subcommand) and writes per-subcommand
# argv files so the existing assertions ("--no-mode1" must not appear,
# "--manifest" + "0x01" must appear, etc.) keep working without
# subcommand-name changes.
REAL="$OUT/tools/real"
mkdir -p "$REAL"
cp "$PWD/target/release/gbl" "$REAL/gbl"

cat > "$OUT/tools/gbl" <<EOF
#!/usr/bin/env bash
sub=\${1:-}
shift || true
# Capture argv per-subcommand so the assert_argv calls below can pin
# what reached patch / pack / unwrap etc.
printf '%s\n' "\$@" > "$OUT/\${sub}.argv"
exec "$REAL/gbl" "\$sub" "\$@"
EOF
chmod +x "$OUT/tools/gbl"

# An fv-unwrap input is a raw ABL partition (LZMA-FV wrapped). The
# tests/images/ dir also holds non-ABL fixtures (grafted-recovery.img,
# vbmeta-*.img, …), so glob specifically for an *abl*.img.
ABL=$(ls tests/images/*abl*.img 2>/dev/null | head -1 || true)
[ -n "$ABL" ] || { echo "SKIP: 085 — no tests/images/*abl*.img fixture present"; exit 0; }

# A throwaway base EFI: efisp-package.py just concatenates it, so any
# small file with a PE 'MZ' header is enough for the structural check.
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

# mode 1 — plain gbl patch (no --oem), gbl pack gets --manifest 0x01.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 1 --efi "$OUT/base.efi" \
  --tools-dir "$OUT/tools" --out "$OUT/mode1.efi" \
  >"$OUT/m1.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 1 failed"; cat "$OUT/m1.log"; exit 1; }
"$H" scan-cached-abl "$OUT/mode1.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-1 output has no locatable cached-ABL overlay"; exit 1; }
assert_no_argv "$OUT/patch.argv" "--no-mode1" "mode 1 patch"
assert_no_argv "$OUT/patch.argv" "--oem"      "mode 1 patch (no --oem)"
assert_argv    "$OUT/pack.argv"  "--manifest" "mode 1 pack manifest flag"
assert_argv    "$OUT/pack.argv"  "0x01"       "mode 1 pack manifest bits"

# mode 0 — plain patch (no --oem, no --no-mode1), pack --manifest 0x00.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 0 --efi "$OUT/base.efi" \
  --tools-dir "$OUT/tools" --out "$OUT/mode0.efi" \
  >"$OUT/m0.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 0 failed"; cat "$OUT/m0.log"; exit 1; }
"$H" scan-cached-abl "$OUT/mode0.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-0 output has no locatable cached-ABL overlay"; exit 1; }
assert_no_argv "$OUT/patch.argv" "--no-mode1" "mode 0 patch"
assert_no_argv "$OUT/patch.argv" "--oem"      "mode 0 patch (no --oem)"
assert_argv    "$OUT/pack.argv"  "--manifest" "mode 0 pack manifest flag"
assert_argv    "$OUT/pack.argv"  "0x00"       "mode 0 pack manifest bits"

# mode 0 + --oem — now allowed (decoupled from --mode 2). patch must
# receive --oem oplus; pack still gets --manifest 0x00.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 0 --efi "$OUT/base.efi" --oem oplus \
  --tools-dir "$OUT/tools" --out "$OUT/mode0-oem.efi" \
  >"$OUT/m0oem.log" 2>&1 \
  || { echo "FAIL: efisp-package.py mode 0 + --oem failed"; cat "$OUT/m0oem.log"; exit 1; }
"$H" scan-cached-abl "$OUT/mode0-oem.efi" | grep -q 'status=0' \
  || { echo "FAIL: mode-0-with-oem output has no locatable cached-ABL overlay"; exit 1; }
assert_no_argv "$OUT/patch.argv" "--no-mode1" "mode 0+oem patch"
assert_argv    "$OUT/patch.argv" "--oem"      "mode 0+oem patch --oem flag"
assert_argv    "$OUT/patch.argv" "oplus"      "mode 0+oem patch --oem value"
assert_argv    "$OUT/pack.argv"  "0x00"       "mode 0+oem pack manifest bits"

# pre-flight gate: mode 2 without --stock-vbmeta must abort non-zero.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 2 --efi "$OUT/base.efi" --out "$OUT/bad.efi" \
  --tools-dir "$OUT/tools" \
  >/dev/null 2>&1 \
  && { echo "FAIL: mode 2 accepted without --stock-vbmeta"; exit 1; } || true

# pre-flight gate: --stock-vbmeta on mode 1 must abort non-zero.
python3 scripts/efisp-package.py \
  --abl "$ABL" --mode 1 --efi "$OUT/base.efi" --stock-vbmeta "$ABL" \
  --tools-dir "$OUT/tools" \
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
