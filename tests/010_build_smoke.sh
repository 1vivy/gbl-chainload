#!/usr/bin/env bash
# 010_build_smoke.sh — verify scripts/build.sh produces the expected artifact.
#
# Engine rework (Task 11): build.sh produces a single dist/gbl-chainload.efi
# (and dist/gbl-chainload-<suffix>.efi when --auto/--debug/--verbose). The
# per-mode loop is gone — activation lives in the runtime GBLP1 manifest.
#
# Requires docker (scripts/build.sh runs EDK-II inside a container). If docker
# is unavailable on the runner, skip cleanly — host-side lint/scan/patch tests
# still run, the EFI compile path simply isn't validated here.
set -euo pipefail
cd "$(dirname "$0")/.."
fail=0

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP: 010_build_smoke — docker not in PATH on this runner"
  echo "  (PATH=$PATH)"
  ls -la /usr/bin/docker 2>&1 | head -1 || true
  exit 0
fi

# Build verbose first, then non-verbose last. build-inside-docker.sh always
# copies its output to dist/gbl-chainload.efi as well as the suffixed name,
# so we order the runs so the verbose build does not clobber the non-verbose
# artifact that the VERBOSE-strip lint below reads.
echo "== building dist/gbl-chainload-auto-debug-verbose.efi =="
./scripts/build.sh --auto --debug --verbose
test -f dist/gbl-chainload-auto-debug-verbose.efi \
  || { echo "FAIL: dist/gbl-chainload-auto-debug-verbose.efi missing"; exit 1; }

echo "== building dist/gbl-chainload.efi =="
./scripts/build.sh
test -f dist/gbl-chainload.efi || { echo "FAIL: dist/gbl-chainload.efi missing"; exit 1; }


# ── VERBOSE compile-strip verification ────────────────────────────────────
# VERBOSE(fmt, ...) compile-strips to a no-op under GBL_VERBOSE=0, so the
# format strings of VERBOSE call sites must be ABSENT from .rodata of
# non-verbose builds. They appear in --verbose builds.
#
# Use multiple probe fragments so one missing isn't a false PASS.
echo "--- VERBOSE strip verification ---"
PROBES=(
  'section @ 0x'      # AblUnwrap per-section scan
  'qsee-buf'          # QseecomHook payload hex
  'first16='          # VerifiedBootHook payload hex
)

# Non-verbose artifact is dist/gbl-chainload.efi (no --verbose flag passed).
# VERBOSE() format-string fragments must be absent from its .rodata.
for v in dist/gbl-chainload.efi; do
  [ -f "$v" ] || continue
  for p in "${PROBES[@]}"; do
    n=$(strings "$v" 2>/dev/null | grep -c "$p" || true)
    if [ "$n" -gt 0 ]; then
      echo "FAIL: VERBOSE probe '$p' found $n time(s) in non-verbose build $v" >&2
      fail=1
    fi
  done
done

if [ -f dist/gbl-chainload-auto-debug-verbose.efi ]; then
  total=0
  for p in "${PROBES[@]}"; do
    n=$(strings dist/gbl-chainload-auto-debug-verbose.efi 2>/dev/null | grep -c "$p" || true)
    total=$((total + n))
  done
  if [ "$total" -eq 0 ]; then
    echo "WARN: no VERBOSE probe markers found in gbl-chainload-auto-debug-verbose.efi —"  >&2
    echo "      compiler may have stripped string literals; manual nm/objdump needed" >&2
  else
    echo "OK: $total VERBOSE probe marker(s) present in verbose build"
  fi
fi
echo "--- end VERBOSE strip verification ---"

[ "$fail" -eq 0 ] && echo "ok 010_build_smoke" || { echo "FAIL: 010_build_smoke"; exit 1; }
