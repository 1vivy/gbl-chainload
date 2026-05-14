#!/usr/bin/env bash
# 052_log_minimal.sh — regression check for the minimal logging design.
#
# Asserts:
# 1. GblLog.h exists at the canonical path.
# 2. GBL_INFO and VERBOSE macros are defined.
# 3. The header gates the macros on GBL_DEBUG / GBL_VERBOSE compile flags.
# 4. GBL_INFO uses both AsciiPrint (visible branch) AND DEBUG((DEBUG_INFO,...))
#    (silent branch) per the swap-mechanism design.
# 5. No GBL_DBG_LOGFS_ONLY references anywhere (legacy from PR #17 design).
# 6. No GblDebugLib source remains (legacy from PR #17 — should not be
#    present on this branch which was cut from main).
# 7. BootFlow.c calls LogFsClose() before gBS->LoadImage(...).
# 8. LogFsLib.h still exports its core API: LogFsInit / LogFsWrite /
#    LogFsFlush / LogFsClose / LogFsIsReady.
#
# Host-side only; on-device verification is manual per CLAUDE.md.

set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

# ── Check 1: GblLog.h present ──────────────────────────────────────────────
HDR=GblChainloadPkg/Include/Library/GblLog.h
if [ ! -f "$HDR" ]; then
  echo "FAIL: $HDR not found" >&2
  fail=1
fi

# ── Check 2: macros defined ────────────────────────────────────────────────
if [ -f "$HDR" ]; then
  if ! grep -qE 'define[[:space:]]+GBL_INFO' "$HDR"; then
    echo "FAIL: GBL_INFO macro not defined in GblLog.h" >&2
    fail=1
  fi
  if ! grep -qE 'define[[:space:]]+VERBOSE' "$HDR"; then
    echo "FAIL: VERBOSE macro not defined in GblLog.h" >&2
    fail=1
  fi
fi

# ── Check 3: compile-time gates present ────────────────────────────────────
if [ -f "$HDR" ]; then
  if ! grep -qE '^[[:space:]]*#[[:space:]]*if[[:space:]]*\(GBL_DEBUG' "$HDR"; then
    echo "FAIL: GblLog.h missing #if (GBL_DEBUG ...) gate" >&2
    fail=1
  fi
  if ! grep -qE '^[[:space:]]*#[[:space:]]*if[[:space:]]*\(GBL_VERBOSE' "$HDR"; then
    echo "FAIL: GblLog.h missing #if (GBL_VERBOSE ...) gate" >&2
    fail=1
  fi
fi

# ── Check 4: GBL_INFO swap-mechanism — uses BOTH AsciiPrint and DEBUG(DEBUG_INFO ──
if [ -f "$HDR" ]; then
  if ! grep -q 'AsciiPrint' "$HDR"; then
    echo "FAIL: GblLog.h missing AsciiPrint reference (visible branch)" >&2
    fail=1
  fi
  if ! grep -qE 'DEBUG[[:space:]]*\(\([[:space:]]*DEBUG_INFO' "$HDR"; then
    echo "FAIL: GblLog.h missing DEBUG((DEBUG_INFO, ...)) reference (silent branch)" >&2
    fail=1
  fi
fi

# ── Check 5: GBL_DBG_LOGFS_ONLY fully absent ───────────────────────────────
if grep -rn 'GBL_DBG_LOGFS_ONLY' GblChainloadPkg 2>/dev/null | grep -v 'Binary file'; then
  echo "FAIL: GBL_DBG_LOGFS_ONLY references found (legacy PR #17 — should be absent)" >&2
  fail=1
fi

# ── Check 6: no GblDebugLib source dir ─────────────────────────────────────
if [ -d GblChainloadPkg/Library/GblDebugLib ]; then
  echo "FAIL: GblChainloadPkg/Library/GblDebugLib/ exists — legacy PR #17 should be absent" >&2
  fail=1
fi

# ── Check 7: BootFlow.c LogFsClose before LoadImage ────────────────────────
BF=GblChainloadPkg/Application/GblChainload/BootFlow.c
if [ -f "$BF" ]; then
  if ! awk '/gBS->LoadImage/{exit} {print}' "$BF" | grep -q 'LogFsClose'; then
    echo "FAIL: BootFlow.c must call LogFsClose() before gBS->LoadImage" >&2
    fail=1
  fi
else
  echo "FAIL: $BF not found" >&2
  fail=1
fi

# ── Check 8: LogFsLib.h public API intact ──────────────────────────────────
LF=GblChainloadPkg/Include/Library/LogFsLib.h
if [ -f "$LF" ]; then
  for sym in LogFsInit LogFsWrite LogFsFlush LogFsClose LogFsIsReady; do
    if ! grep -q "$sym" "$LF"; then
      echo "FAIL: LogFsLib.h missing $sym" >&2
      fail=1
    fi
  done
else
  echo "FAIL: $LF not found" >&2
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "OK: minimal logging design constants and structure in place."
