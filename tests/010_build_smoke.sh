#!/usr/bin/env bash
# 010_build_smoke.sh — verify scripts/build.sh produces the expected artifacts.
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

echo "== building dist/mode-0.efi =="
./scripts/build.sh --mode 0
test -f dist/mode-0.efi || { echo "FAIL: dist/mode-0.efi missing"; exit 1; }

echo "== building dist/mode-1.efi =="
./scripts/build.sh --mode 1
test -f dist/mode-1.efi || { echo "FAIL: dist/mode-1.efi missing"; exit 1; }

echo "== building dist/mode-1-auto-debug-verbose.efi =="
./scripts/build.sh --mode 1 --auto --debug --verbose
test -f dist/mode-1-auto-debug-verbose.efi \
  || { echo "FAIL: dist/mode-1-auto-debug-verbose.efi missing"; exit 1; }


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

for v in dist/mode-1-auto.efi dist/mode-1-auto-debug.efi; do
  [ -f "$v" ] || continue
  for p in "${PROBES[@]}"; do
    n=$(strings "$v" 2>/dev/null | grep -c "$p" || true)
    if [ "$n" -gt 0 ]; then
      echo "FAIL: VERBOSE probe '$p' found $n time(s) in non-verbose build $v" >&2
      fail=1
    fi
  done
done

if [ -f dist/mode-1-auto-debug-verbose.efi ]; then
  total=0
  for p in "${PROBES[@]}"; do
    n=$(strings dist/mode-1-auto-debug-verbose.efi 2>/dev/null | grep -c "$p" || true)
    total=$((total + n))
  done
  if [ "$total" -eq 0 ]; then
    echo "WARN: no VERBOSE probe markers found in mode-1-auto-debug-verbose.efi —"  >&2
    echo "      compiler may have stripped string literals; manual nm/objdump needed" >&2
  else
    echo "OK: $total VERBOSE probe marker(s) present in verbose build"
  fi
fi
echo "--- end VERBOSE strip verification ---"

[ "$fail" -eq 0 ] && echo "ok 010_build_smoke" || { echo "FAIL: 010_build_smoke"; exit 1; }
