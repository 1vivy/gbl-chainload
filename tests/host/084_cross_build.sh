#!/usr/bin/env bash
# tests/host/084_cross_build.sh — the six host-side tools cross-compile to
# Windows and macOS and the artifacts are well-formed PE32+ / Mach-O
# universal binaries. Artifact-format check only — the cross binaries
# cannot run on a Linux box (real-OS behaviour is the CI job's concern).
set -euo pipefail
cd "$(dirname "$0")/../.."

command -v docker >/dev/null 2>&1 \
  || { echo "SKIP: 084 — docker not available"; exit 0; }
docker image inspect gbl-chainload-build:latest >/dev/null 2>&1 \
  || { echo "SKIP: 084 — gbl-chainload-build:latest image not built"; exit 0; }

bash scripts/build-cross-tools.sh all

TOOLS="fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile"
for t in $TOOLS; do
  win="dist/windows/$t.exe"
  [ -f "$win" ] || { echo "FAIL: $win not produced"; exit 1; }
  fw=$(file -b "$win")
  case "$fw" in
    *PE32+*x86-64*) ;;
    *) echo "FAIL: $win is not PE32+ x86-64: $fw"; exit 1 ;;
  esac

  mac="dist/macos/$t"
  [ -f "$mac" ] || { echo "FAIL: $mac not produced"; exit 1; }
  fm=$(file -b "$mac")
  case "$fm" in
    *"Mach-O universal"*) ;;
    *) echo "FAIL: $mac is not a Mach-O universal binary: $fm"; exit 1 ;;
  esac
  case "$fm" in *x86_64*) ;; *) echo "FAIL: $mac missing x86_64 slice"; exit 1 ;; esac
  case "$fm" in *arm64*)  ;; *) echo "FAIL: $mac missing arm64 slice";  exit 1 ;; esac
done

echo "PASS: 084 cross-build"
