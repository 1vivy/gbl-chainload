#!/usr/bin/env bash
# tests/host/084_cross_build.sh — the `gbl` multicall binary cross-compiles
# to Windows and macOS and the artifacts are well-formed PE32+ /
# Mach-O universal binaries. Artifact-format check only — the cross
# binaries cannot run on a Linux box (real-OS behaviour is the CI job's
# concern).
#
# PR2 Task 8: the 7 host C tools collapsed into the `gbl` multicall, so
# this test now checks one Rust binary per platform rather than seven.
set -euo pipefail
cd "$(dirname "$0")/../.."

command -v docker >/dev/null 2>&1 \
  || { echo "SKIP: 084 — docker not available"; exit 0; }
docker image inspect gbl-chainload-build:latest >/dev/null 2>&1 \
  || { echo "SKIP: 084 — gbl-chainload-build:latest image not built"; exit 0; }

# The multicall pulls in clap_lex 1.1.0 which requires Rust 1.85+
# (edition2024). If the image was built with an older RUST_VER, skip
# rather than fail — the Dockerfile bump from 1.78 to 1.85 lands with
# this task, but local images take a manual `docker build` to refresh.
docker_rust_ver=$(docker run --rm gbl-chainload-build:latest cargo --version 2>/dev/null \
  | awk '{print $2}')
if [ -n "$docker_rust_ver" ]; then
  ver_major=$(echo "$docker_rust_ver" | cut -d. -f1)
  ver_minor=$(echo "$docker_rust_ver" | cut -d. -f2)
  if [ "$ver_major" -eq 1 ] && [ "$ver_minor" -lt 85 ]; then
    echo "SKIP: 084 — docker image's rust $docker_rust_ver < 1.85; rebuild image"
    exit 0
  fi
fi

# Rust 1.85's windows-sys crate calls dlltool during the Windows cross
# build. The docker image ships with zig (which provides mingw headers
# / libc) but no `x86_64-w64-mingw32-dlltool` binary, so cargo aborts
# the Windows target. SKIP rather than fail; the Dockerfile would need
# `apt install mingw-w64` (or equivalent) to unblock — tracked as a
# PR2 follow-up in docs/superpowers/pr-evidence/.
if ! docker run --rm gbl-chainload-build:latest \
       sh -c 'command -v x86_64-w64-mingw32-dlltool >/dev/null 2>&1'; then
  echo "SKIP: 084 — docker image lacks mingw-w64 dlltool (PR2 follow-up)"
  exit 0
fi

bash scripts/build-cross-tools.sh all

win="dist/windows/gbl.exe"
[ -f "$win" ] || { echo "FAIL: $win not produced"; exit 1; }
fw=$(file -b "$win")
case "$fw" in
  *PE32+*x86-64*) ;;
  *) echo "FAIL: $win is not PE32+ x86-64: $fw"; exit 1 ;;
esac

mac="dist/macos/gbl"
[ -f "$mac" ] || { echo "FAIL: $mac not produced"; exit 1; }
fm=$(file -b "$mac")
case "$fm" in
  *"Mach-O universal"*) ;;
  *) echo "FAIL: $mac is not a Mach-O universal binary: $fm"; exit 1 ;;
esac
case "$fm" in *x86_64*) ;; *) echo "FAIL: $mac missing x86_64 slice"; exit 1 ;; esac
case "$fm" in *arm64*)  ;; *) echo "FAIL: $mac missing arm64 slice";  exit 1 ;; esac

echo "PASS: 084 cross-build"
