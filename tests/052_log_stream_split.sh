#!/usr/bin/env bash
# 052_log_stream_split.sh — regression check for cleanup-phase-1 PR-c.
#
# Asserts:
# 1. The gbl-chainload log filename constant in LogFsLib is the new
#    lowercase-hyphen form: gbl-chainload_Boot
# 2. The old form (GblChainload_Boot) is not referenced as an output
#    file path anywhere in LogFsLib source or headers.
# 3. DebugSink.c gates ConOut passthrough via gDbgCurrentLevel against
#    gGblScreenMask, defaulted to DEBUG_ERROR.
# 4. GblDebugLib sets gDbgCurrentLevel before calling ConOut->OutputString.
# 5. Boundary markers ("gbl-chainload entered" / "gbl-chainload exiting")
#    are present in Application source at DEBUG_INFO level — captured
#    by logfs always, never on screen/UefiLog.
# 6. SCR_PRINT is not reintroduced anywhere in code paths.
# 7. gGblScreenMask is never widened at runtime — no Application code
#    calls LogFsSetScreenMask to add any level beyond DEBUG_ERROR. The
#    UefiLog stays clean regardless of build flags.
# 8. GBL_DBG_LOGFS_ONLY (private logfs-only error-level bit) is defined
#    in LogFsLib.h.
# 9. AblUnwrap's high-volume traces use GBL_DBG_LOGFS_ONLY (not
#    DEBUG_VERBOSE), so admitting them via PcdDebugPrintErrorLevel does
#    not also admit QCOM stock DEBUG_VERBOSE spam.
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

# ── Check 3: DebugSink reads gDbgCurrentLevel and gates against the
#            runtime gGblScreenMask (default DEBUG_ERROR). ─────────────────
if ! grep -q 'gDbgCurrentLevel' "$LOGFS/DebugSink.c"; then
  echo "FAIL: DebugSink.c does not reference gDbgCurrentLevel" >&2
  fail=1
fi

if ! grep -q 'gGblScreenMask' "$LOGFS/DebugSink.c"; then
  echo "FAIL: DebugSink.c does not gate via gGblScreenMask" >&2
  fail=1
fi

if ! grep -q 'gGblScreenMask.*DEBUG_ERROR\|DEBUG_ERROR.*gGblScreenMask' \
       "$LOGFS/DebugSink.c"; then
  echo "FAIL: DebugSink.c gGblScreenMask must default to DEBUG_ERROR" >&2
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

# ── Check 5a: "gbl-chainload entered" marker present at DEBUG_INFO. ───────
# Boundary markers stay at INFO level so the runtime mask (DEBUG_ERROR
# only, never widened) keeps them off ConOut and out of UefiLog. The
# logfs hook captures them on every build via LogFsWrite.
if ! grep -Rq 'gbl-chainload entered' "$APP"; then
  echo "FAIL: 'gbl-chainload entered' boundary marker not found in Application/" >&2
  fail=1
else
  if ! grep -RnE 'DEBUG_INFO.*gbl-chainload entered|gbl-chainload entered.*DEBUG_INFO' "$APP" 2>/dev/null | grep -q .; then
    echo "FAIL: 'gbl-chainload entered' found but not on a DEBUG_INFO line" >&2
    fail=1
  fi
fi

# ── Check 5b: "gbl-chainload exiting" marker present at DEBUG_INFO. ───────
if ! grep -Rq 'gbl-chainload exiting' "$APP"; then
  echo "FAIL: 'gbl-chainload exiting' boundary marker not found in Application/" >&2
  fail=1
else
  if ! grep -RnE 'DEBUG_INFO.*gbl-chainload exiting|gbl-chainload exiting.*DEBUG_INFO' "$APP" 2>/dev/null | grep -q .; then
    echo "FAIL: 'gbl-chainload exiting' found but not on a DEBUG_INFO line" >&2
    fail=1
  fi
fi

# ── Check 6: SCR_PRINT was deliberately removed. Catch any reintroduction
#            outside of a comment that documents the removal. ────────────
SCR_HITS=$(grep -RnE '^[^#/]*\bSCR_PRINT\b' "$APP" "$LOGFS" \
            "GblChainloadPkg/Library/GblDebugLib" 2>/dev/null \
            | grep -v ' \* No SCR_PRINT' || true)
if [ -n "$SCR_HITS" ]; then
  echo "FAIL: SCR_PRINT reintroduced — design uses Print/DEBUG_ERROR/DEBUG_INFO directly:" >&2
  echo "$SCR_HITS" >&2
  fail=1
fi

# ── Check 7: gGblScreenMask is never widened at runtime. ──────────────────
# UefiLog should stay clean regardless of build flags. The Application
# layer (Entry.c, BootFlow.c, anything else under GblChainloadPkg/
# Application) must not call LogFsSetScreenMask at all. The function
# itself is defined in DebugSink.c — that definition is allowed; we only
# scan the Application surface.
MASK_HITS=$(grep -RnE '\bLogFsSetScreenMask\s*\(' "$APP" 2>/dev/null || true)
if [ -n "$MASK_HITS" ]; then
  echo "FAIL: Application code calls LogFsSetScreenMask:" >&2
  echo "$MASK_HITS" >&2
  echo "      The mask must never widen at runtime — UefiLog stays clean" >&2
  echo "      on every build, including --debug / --verbose. Use" >&2
  echo "      GBL_DBG_LOGFS_ONLY tier + PcdDebugPrintErrorLevel widening" >&2
  echo "      for high-volume traces instead." >&2
  fail=1
fi

# ── Check 8: GBL_DBG_LOGFS_ONLY error-level bit defined in LogFsLib.h. ────
if ! grep -q 'GBL_DBG_LOGFS_ONLY' "$INCLUDE/Library/LogFsLib.h"; then
  echo "FAIL: GBL_DBG_LOGFS_ONLY not defined in LogFsLib.h —" >&2
  echo "      this is the private logfs-only error-level bit used by" >&2
  echo "      AblUnwrap and any future high-volume tracer." >&2
  fail=1
fi

# ── Check 9: AblUnwrap uses GBL_DBG_LOGFS_ONLY (not DEBUG_VERBOSE) for
#            high-volume traces. Using DEBUG_VERBOSE would collide with
#            QCOM stock code (e.g. PartitionTableUpdate.c:174). ───────────
ABL=GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.c
if [ -f "$ABL" ]; then
  if grep -qE 'DEBUG\s*\(\s*\(\s*DEBUG_VERBOSE' "$ABL"; then
    echo "FAIL: AblUnwrap still uses DEBUG_VERBOSE — re-tag to GBL_DBG_LOGFS_ONLY" >&2
    echo "      so QCOM stock code's EFI_D_VERBOSE doesn't get unlocked alongside." >&2
    fail=1
  fi
  if ! grep -q 'GBL_DBG_LOGFS_ONLY' "$ABL"; then
    echo "FAIL: AblUnwrap doesn't reference GBL_DBG_LOGFS_ONLY — expected" >&2
    echo "      its high-volume traces to use that level." >&2
    fail=1
  fi
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "OK: log stream split constants and routing in place."
