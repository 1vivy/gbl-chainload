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
#    are present in Application source at DEBUG_INFO level. Visible on
#    screen + UefiLog only under --debug (mask widening); always in logfs.
# 6. SCR_PRINT is not reintroduced anywhere in code paths.
# 7. Application LogFsSetScreenMask calls only widen to ERROR|WARN|INFO,
#    never include DEBUG_VERBOSE or GBL_DBG_LOGFS_ONLY (verbose tier
#    must NEVER reach UefiLog per design).
# 8. GBL_DBG_LOGFS_ONLY (private logfs-only error-level bit) is defined
#    in LogFsLib.h.
# 9. AblUnwrap's high-volume traces use GBL_DBG_LOGFS_ONLY (not
#    DEBUG_VERBOSE), so admitting them via PcdDebugPrintErrorLevel does
#    not also admit QCOM stock DEBUG_VERBOSE spam.
# 10. BootFlow.c does NOT call LogFsRemoveDebugSink before LoadImage.
#     The sink is retained across the chainload handoff so the runtime
#     mask continues to filter the patched ABL's DEBUG output (per-call
#     QSEECOM/SCM/VB traces) by gGblScreenMask — otherwise those
#     emits would leak directly to UefiLog post-handoff regardless of
#     mask state.
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
# Boundary markers stay at INFO. Default mask = DEBUG_ERROR keeps them
# off ConOut on prod; --debug widens to include INFO so they reach
# screen + UefiLog as confirmation. logfs captures them on every build.
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

# ── Check 7: gGblScreenMask widens are constrained to INFO/WARN. ───────────
# Verbose tier (GBL_DBG_LOGFS_ONLY) and DEBUG_VERBOSE MUST NEVER appear
# in a LogFsSetScreenMask call — those tiers are logfs-only by design.
if grep -RnE '\bLogFsSetScreenMask\s*\([^)]*(GBL_DBG_LOGFS_ONLY|DEBUG_VERBOSE)' \
     "$APP" 2>/dev/null | grep -q .; then
  echo "FAIL: LogFsSetScreenMask passes a logfs-only tier (GBL_DBG_LOGFS_ONLY" >&2
  echo "      or DEBUG_VERBOSE). Those bits must NEVER reach UefiLog by design." >&2
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

# ── Check 10: BootFlow.c retains the sink across the LoadImage handoff. ───
# Without this, the patched ABL's runtime hook DEBUG output (qsee-buf
# hex dumps, scm-sip/scm-send traces, vb-fakelock/vb-rwstate) bypasses
# our mask filter and floods UefiLog on every build. The sink stays
# installed; LogFsWrite no-ops cleanly after LogFsClose via the
# !LogFsReady guard, but the screen-mask filter remains active.
BF=GblChainloadPkg/Application/GblChainload/BootFlow.c
if [ -f "$BF" ]; then
  if grep -B2 'gBS->LoadImage' "$BF" | grep -q 'LogFsRemoveDebugSink'; then
    echo "FAIL: BootFlow.c calls LogFsRemoveDebugSink before LoadImage —" >&2
    echo "      the sink must be retained across the chainload handoff so" >&2
    echo "      the screen-mask filter applies to the patched ABL's runtime" >&2
    echo "      DEBUG emits. Drop the LogFsRemoveDebugSink() call." >&2
    fail=1
  fi
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "OK: log stream split constants and routing in place."
