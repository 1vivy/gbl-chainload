#!/usr/bin/env bash
# tests/host/065_patch_sig_parity.sh — the canonical EFISP UTF-16LE
# pattern lives in the Rust patch-engine crate (PR2 Task 6) and is no
# longer duplicated in a C header (PR2 Task 8 removed
# `tools/shared/patch_signatures.h` along with the rest of the host C
# tool surface).
#
# This test guards against the Rust crate dropping the pattern — the
# in-crate test pins it to the actual 10-byte value; we only check the
# const is still referenced from the named source file.
set -euo pipefail
cd "$(dirname "$0")/../.."

test -f crates/patch-engine/src/retired/block_efisp_recursion.rs \
  || { echo "FAIL: retired module missing from crates/patch-engine"; exit 1; }
grep -q 'EFISP_UTF16_PATTERN' crates/patch-engine/src/retired/block_efisp_recursion.rs \
  || { echo "FAIL: EFISP_UTF16_PATTERN missing from Rust retired module"; exit 1; }

echo "PASS: 065 patch_sig parity"
