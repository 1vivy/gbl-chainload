#!/usr/bin/env bash
# 053_cache_abl_lint.sh — host lint for cache-ABL build/runtime plumbing.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

require() {
  local label="$1" pattern="$2" path="$3"
  if ! grep -RnE -- "$pattern" "$path" >/dev/null 2>&1; then
    echo "FAIL: missing $label ($pattern in $path)" >&2
    fail=1
  fi
}

require "build flag" '--cache-abl' scripts/build.sh
require "container cache env" 'GBL_HAS_CACHED_ABL' scripts/build-inside-docker.sh
require "header generator" 'generate-cached-abl-header.py' scripts/build-inside-docker.sh
require "generated header ignore" 'CachedAblBlob\.generated\.h' .gitignore
require "CachedAblLib declaration" 'CachedAblLib\|Include/Library/CachedAblLib.h' GblChainloadPkg/GblChainloadPkg.dec
require "CachedAblLib DSC mapping" 'CachedAblLib\|GblChainloadPkg/Library/CachedAblLib/CachedAblLib.inf' GblChainloadPkg/GblChainloadPkg.dsc
require "BootFlow cached path" 'CachedAbl_IsPresent' GblChainloadPkg/Application/GblChainload/BootFlow.c
require "dynamic patch skip log" 'dynamic patches skipped' GblChainloadPkg/Application/GblChainload/BootFlow.c
require "mode-param host patcher" 'GBL_MODE \?= 1' tools/abl-patcher/Makefile
require "cache recursion safety scan" 'EFISP_UTF16LE' scripts/generate-cached-abl-header.py
require "cache recursion fail closed" 'unsafe cached ABL still contains UTF-16 efisp' scripts/generate-cached-abl-header.py

if [[ ! -x scripts/generate-cached-abl-header.py ]]; then
  echo "FAIL: scripts/generate-cached-abl-header.py must be executable" >&2
  fail=1
fi

[[ $fail -eq 0 ]] && echo "ok 053_cache_abl_lint" || exit 1
