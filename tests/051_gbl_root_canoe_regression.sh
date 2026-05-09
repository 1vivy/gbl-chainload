#!/usr/bin/env bash
# tests/051_gbl_root_canoe_regression.sh — gbl_root_canoe regression fixtures.
#
# Status: survey-only for plan 1.  Future plan should build extractfv, bake
# LinuxLoader.efi fixtures into tests/fixtures/patches-gbl-root-canoe/, port
# representative patches into kRegressionPatches[], and add abl-patcher
# --regression-suite.  See commit message for full survey findings.
#
# Survey date: 2026-05-09  (gbl_root_canoe @ /home/vivy/gbl_root_canoe)
#
# Patches surveyed (tools/patchlib.h + tools/patch_abl.c):
#
#   Patch 1: patch_abl_gbl
#     UTF-16 literal "efisp" -> "nulls" byte scan.  Trivial memcmp walk.
#     Could port to PATCH_DESC + ScanLib directly; no arm64 decoder needed.
#     DEFERRED: fixture extraction blocks (see below).
#
#   Patch 2: patch_adrl_unlocked_to_locked
#     Three consecutive ADRP+ADD pairs whose targets are "unlocked", "locked",
#     "androidboot.vbmeta.device_state".  Rewrites pair-0 to point at "locked".
#     Requires arm64_inst_decoder.h (INST_ADRP, INST_ADD_X_IMM, calc_adrl_file_offset).
#     DEFERRED: needs decoder vendoring.
#
#   Patch 3: patch_abl_bootstate
#     32-byte wildcard pattern (Original[]/Patched[] INT16 arrays) rewrites bytes
#     in place; also captures lock_register_num and anchor_offset for patches 4+5.
#     Plain scan, portable.  Multi-pass dependency (feeds 4 & 5) blocks isolation.
#     DEFERRED: multi-pass chain; anchor_offset coupling to patches 4 & 5.
#
#   Patch 4: find_ldrB_instructio_reverse (LDRB source)
#     Backward walk from anchor_offset tracking register/stack bounces up to 8
#     levels; rewrites the source LDRB as MOV Wt, #1.
#     DEFERRED: depends on patch 3's anchor_offset + arm64 decoder.
#
#   Patch 5: track_forward_patch_strb (STRB sink)
#     Forward walk from LDRB found by patch 4; rewrites the STRB sink so Rt=WZR.
#     DEFERRED: depends on patches 3 & 4; requires arm64 decoder.
#
#   Patch 6: patch_string_jump
#     Scans all branch instructions, follows jump target, checks if it lands at
#     ADRP+ADD that resolves to a string containing "is not allowed in Lock State";
#     if so, NOPs the branch.
#     DEFERRED: requires arm64 decoder (INST_ADRP, get_JUMP_target, etc.).
#
#   Patch 7: patch_orange_state_screen
#     Primary: 4-byte anchor {0x36,0x31,0x88,0x1A} followed by CBZ; rewrites CBZ
#     as unconditional B (rewrite_cbz_as_b).  Fallback: 12-byte wildcard countdown
#     anchor.  Second fixture required (002_infiniti_abl.elf, OnePlus 15 16.0.5.700
#     GLO).
#     DEFERRED: rewrite_cbz_as_b uses arm64 decoder; second fixture not in tree.
#
#   Patch 8: patch_verifiedbootstate_orange
#     24-byte wildcard pattern ending with LDRSW+ADRP+ADD+MOV+ADD+LDR sequence;
#     rewrites first 4 bytes as MOV X8, #1 (0xD2800028).
#     Plain pattern match — most portable.  Feasible for future fast-path import.
#     DEFERRED: fixture extraction still blocks.
#
#   Patch 9: patch_abl_verity_logging
#     Part 1: ADRP+ADD pairs resolving to "enforcing" near "androidboot.veritymode"
#     are rewritten to point at "logging".  Part 2: pointer table scan replaces
#     the "enforcing" raw-offset entry with the "logging" one.
#     DEFERRED: arm64 decoder for part 1; both parts are multi-step.
#
# Why fixture extraction blocks everything:
#   gbl_root_canoe's test fixtures are device-specific ELF blobs
#   (001_myron_abl.elf, 002_infiniti_abl.elf) that must be unwrapped by
#   tools/extractfv (a separate C binary linked against -llzma) before the
#   inner LinuxLoader.efi is available for patching.  Neither the ELFs nor
#   the extracted PEs live in gbl-chainload's tree, and pre-baking them
#   requires building extractfv, running it, then running patch_abl with
#   individual DISABLE_PATCH_* flags to produce expected.bin per patch.
#   Expected outputs are currently stored as MD5 hashes in 004_test_patch.sh,
#   not as binary files.
#
# Why arm64 decoder blocks portability:
#   arm64_inst_decoder.h is a 370-line standalone decoder (ADRP, ADD, LDR, STR,
#   STRB, LDRB, CBZ, MOV, PACIASP, etc.) with its own types and decode_at().
#   Our engine's ScanLib uses a different abstraction.  Vendoring the decoder
#   or rewriting each patch to ScanLib primitives is non-trivial; patches 4 & 5
#   are chained through a shared LocSet and bounce-tracking state that does not
#   map to a single PATCH_DESC Apply() function.
#
# Recommended future plan actions:
#   1. Build extractfv host-side; extract LinuxLoader.efi from both ELF fixtures
#      into tests/fixtures/patches-gbl-root-canoe/{myron,infiniti}/input.bin.
#   2. Vendor arm64_inst_decoder.h into DynamicPatchLib/Internal/ under a thin
#      wrapper so our scan passes can use it.
#   3. Port patch 1 (trivial) and patch 8 (plain pattern) first — these need only
#      the extractor, no decoder.
#   4. Port patch 3 (wildcard bootstate) as a self-contained PATCH_DESC; split
#      patches 4+5 into a combined Apply() that runs the full reverse+forward chain
#      internally, using anchor_offset captured in step 3 via a shared VOID* ctx.
#   5. Add abl-patcher --regression-suite <name> flag to run kRegressionPatches[]
#      instead of the production aggregator; add cmp-based assertions here.

set -euo pipefail
echo "skip 051_gbl_root_canoe_regression (deferred to future plan; see commit message and survey comments in this file)"
exit 0
