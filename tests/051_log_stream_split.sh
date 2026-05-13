#!/usr/bin/env bash
# 051_log_stream_split.sh — regression check for cleanup-phase-1 PR-c.
#
# Asserts:
# 1. The gbl-chainload log filename constant in LogFsLib is the new
#    lowercase-hyphen form: gbl-chainload_Boot
# 2. The old form (GblChainload_Boot) is not referenced as an output
#    file path anywhere in LogFsLib source or headers.
# 3. DebugSink.c gates ConOut passthrough on DEBUG_ERROR (reads gDbgCurrentLevel).
# 4. GblDebugLib sets gDbgCurrentLevel before calling ConOut->OutputString.
# 5. Boundary markers ("gbl-chainload entered" / "gbl-chainload exiting")
#    are present in Application source at DEBUG_ERROR level.
#
# Host-side check; runtime stream split is verified manually on-device.

set -euo pipefail
cd "$(dirname "$0")/.."

LOGFS="GblChainloadPkg/Library/LogFsLib"
DBGLIB="GblChainloadPkg/Library/GblDebugLib"
INCLUDE="GblChainloadPkg/Include"
APP="GblChainloadPkg/Application"

fail=0

# ── Check 1: new filename constant present in LogFsLib. ────────────────────
if ! grep -Rq 'gbl-chainload_Boot' "$LOGFS"; then
  echo "FAIL: 'gbl-chainload_Boot' filename constant not found in $LOGFS" >&2
  fail=1
fi

# ── Check 2: old filename constant absent from LogFsLib and headers. ───────
if grep -Rn 'GblChainload_Boot' "$LOGFS" "$INCLUDE" 2>/dev/null; then
  echo "FAIL: old 'GblChainload_Boot' name still present (above)" >&2
  fail=1
fi

# ── Check 3: DebugSink reads gDbgCurrentLevel for level-gated routing. ─────
if ! grep -q 'gDbgCurrentLevel' "$LOGFS/DebugSink.c"; then
  echo "FAIL: DebugSink.c does not reference gDbgCurrentLevel" >&2
  fail=1
fi

if ! grep -q 'DEBUG_ERROR' "$LOGFS/DebugSink.c"; then
  echo "FAIL: DebugSink.c has no DEBUG_ERROR gate" >&2
  fail=1
fi

# ── Check 4: GblDebugLib sets gDbgCurrentLevel before OutputString. ────────
if [[ ! -f "$DBGLIB/GblDebugLib.c" ]]; then
  echo "FAIL: GblDebugLib/GblDebugLib.c not found" >&2
  fail=1
elif ! grep -q 'gDbgCurrentLevel' "$DBGLIB/GblDebugLib.c"; then
  echo "FAIL: GblDebugLib.c does not set gDbgCurrentLevel" >&2
  fail=1
fi

# ── Check 5a: "gbl-chainload entered" marker present at DEBUG_ERROR. ───────
if ! grep -Rq 'gbl-chainload entered' "$APP"; then
  echo "FAIL: 'gbl-chainload entered' boundary marker not found in Application/" >&2
  fail=1
else
  # The DEBUG((DEBUG_ERROR, "gbl-chainload entered...")) call is on one line.
  if ! grep -RnE 'DEBUG_ERROR.*gbl-chainload entered|gbl-chainload entered.*DEBUG_ERROR' "$APP" 2>/dev/null | grep -q .; then
    echo "FAIL: 'gbl-chainload entered' found but not on a DEBUG_ERROR line" >&2
    fail=1
  fi
fi

# ── Check 5b: "gbl-chainload exiting" marker present at DEBUG_ERROR. ───────
if ! grep -Rq 'gbl-chainload exiting' "$APP"; then
  echo "FAIL: 'gbl-chainload exiting' boundary marker not found in Application/" >&2
  fail=1
else
  if ! grep -RnE 'DEBUG_ERROR.*gbl-chainload exiting|gbl-chainload exiting.*DEBUG_ERROR' "$APP" 2>/dev/null | grep -q .; then
    echo "FAIL: 'gbl-chainload exiting' found but not on a DEBUG_ERROR line" >&2
    fail=1
  fi
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "OK: log stream split constants and routing in place."
