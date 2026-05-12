# ABL fastboot Lock-State gate — patch6 RE findings

**Date:** 2026-05-11
**Fixtures:** `LinuxLoader_infiniti.efi` (infiniti — OnePlus 15) and
`LinuxLoader.efi` (myron — Xiaomi Redmi K90 Pro Max / POCO F8 Ultra;
identified by literal `myron,end` string in the PE body).
**Ghidra project:** `gbl_root_canoe` — bookmarks under category `patch6`.
The project name is aspirational/historical (canoe = kaanapali, sm8850 — not
a fixture we currently exercise). The binary opened as `LinuxLoader.efi` in
that project is myron's extracted PE; the cross-OEM split (OnePlus +
Xiaomi) below confirms patch6's anchor strategy isn't OnePlus-specific.

## Goal

Locate and characterize every ABL fastboot lock-state-refusal site so we can
implement `patch6-lock-state-fastboot-gate` for mode-1.

Under mode-1 the VerifiedBoot view is fakelocked: `IsUnlocked()` returns
FALSE, so ABL's in-fastboot commands that gate on lock-state (flash, erase,
slot change, snapshot-cancel) refuse with messages like `"Flashing is not
allowed in Lock State"`. Patch6 NOPs/rewrites those gates so the commands
proceed while leaving the rest of the fakelock contract intact (KM 0x208,
RoT triple, AVB state).

## String anchors (both fixtures)

| String | Infiniti VA | Myron VA | Note |
|--------|-------------|----------|------|
| `Flashing is not allowed in Lock State`        | `0x584CF` | `0x65EEE` | |
| `Erase is not allowed in Lock State`           | `0x5B002` | `0x68A79` | |
| `Slot Change is not allowed in Lock State\n`   | `0x5BA13` | `0x6959E` | trailing `\n` |
| `Snapshot Cancel is not allowed in Lock State` | `0x5C663` | `0x6A307` | |

Both images use `image_base = 0x00000000`, so Ghidra address == file offset
== runtime VA inside the PE's mapped image. Each string has exactly one
ADRP+ADD pair that loads its pointer.

## Gate pattern split

The conditional that precedes the ADRP comes in **two structurally different
shapes** across the four sites. Both fixtures share the same split.

### Pattern A — `CBZ Wx, L_error` jumps INTO the error block

The error block (ADRP+ADD+BL printer) sits OFF the success path; control
reaches it only via an explicit `CBZ`.

| Site | Infiniti CBZ | Decoded | Myron CBZ | Decoded |
|------|--------------|---------|-----------|---------|
| Flashing | `0x1C3E4: a8 03 00 34` | `CBZ w8, #+0x74 → 0x1C458` | `0x5C080: 68 21 00 34` | `CBZ w8, #+0x42C → 0x5C4AC` |
| Erase    | `0x1DF68: a8 02 00 34` | `CBZ w8, #+0x54 → 0x1DFBC` | `0x5DDE0: a8 0a 00 34` | `CBZ w8, #+0x154 → 0x5DF34` |

`w8` is the zero-extended return of the `BL` immediately above each CBZ —
empirically an `IsUnlocked`-class probe (0 = locked, non-zero = unlocked).
CBZ-taken == locked == enter error block.

**Rewrite:** NOP the CBZ → execution always falls through to the success
path. Both CBZ sites become `1f 20 03 d5` (AArch64 NOP `0xD503201F`).

The offsets differ between fixtures because the error-block layout differs;
the *opcode form* (top byte `0x34`, Rt=`w8`) is stable.

### Pattern B — `B.NE skip_error` jumps PAST the error block

The error block is fall-through; the conditional skips past it when the
device is unlocked. `BL <unlock-checker>` immediately above sets the flags
consumed by `B.NE`.

| Site | Infiniti B.NE | Decoded | Myron B.NE | Decoded |
|------|---------------|---------|------------|---------|
| Slot Change     | `0x1AA54: c1 21 00 54` | `B.NE #+0x438 → 0x1AE8C` | `0x5A1E4: e1 21 00 54` | `B.NE #+0x43C → 0x5A620` |
| Snapshot Cancel | `0x1C1EC: a1 04 00 54` | `B.NE #+0x94  → 0x1C280` | `0x5B8A4: a1 04 00 54` | `B.NE #+0x94  → 0x5B938` |

**Rewrite:** convert `B.NE off` to unconditional `B off` (same target).
The skip is then always taken, error block never executes.

Encoding mechanics (AArch64):
- `B.cond`: `0x54000000 | (imm19 << 5) | cond`
- `B`:      `0x14000000 | imm26`
- Same byte distance → `imm26 = imm19` for positive forward skips.

Concrete byte rewrites for the four B.NE sites:

| Site | Original word | Patched word | Patched bytes |
|------|---------------|--------------|---------------|
| Infiniti Slot Change     | `0x540021C1` | `0x1400010E` | `0e 01 00 14` |
| Infiniti Snapshot Cancel | `0x540004A1` | `0x14000025` | `25 00 00 14` |
| Myron Slot Change        | `0x540021E1` | `0x1400010F` | `0f 01 00 14` |
| Myron Snapshot Cancel    | `0x540004A1` | `0x14000025` | `25 00 00 14` |

## Why patch6 is mode-1 only

Under modes 2 and 3 ABL stays honest (per [`docs/superpowers/specs/2026-05-09-gbl-chainload-v2-three-mode-rewrite-design.md`](../superpowers/specs/2026-05-09-gbl-chainload-v2-three-mode-rewrite-design.md)):
`IsUnlocked()` returns TRUE, so on Pattern A the CBZ never branches into the
error block, and on Pattern B the `B.NE skip_error` always takes the skip.
The gates are dead branches under modes 2/3 — patch6 has no useful effect
and need not run.

Mode-1 fakelocks the VerifiedBoot view, so the unlock-checker BLs return
"locked" — the gates fire and refuse fastboot commands. Patch6 lives at
`GblChainloadPkg/Library/DynamicPatchLib/mode_1/` alongside patch9.
`SCOPE_MODE_1`, `Mandatory=TRUE`. Non-OnePlus/Oppo fixtures don't carry
the anchor strings; mode-1 isn't shipped for them.

## FBE-safety: KM hidden-set unchanged

Patch6 operates entirely inside ABL's fastboot command dispatcher. It does
not touch KM/Keymint, KM 0x201 `SET_ROT`, KM 0x208 `SET_BOOT_STATE`, the
VerifiedBoot protocol device-info, or RPMB boot-info.

Specifically: KM 0x208 continues to receive identical bytes
(`isUnlocked=0, color=0, pubKey=<stockOEM>`) on every mode-1 boot, so the
`(verified_boot_key, verified_boot_state, device_locked)` triple bound into
the QSEE KM hidden-set is byte-stable across boots. FBE class keys wrap and
unwrap correctly.

This is the explicit reason we did not pursue the alternative "detect
fastboot session and present real-unlocked RoT only then" — that would flip
the hidden-set triple between boots and brick `/data`. The field-by-field
divergence across keymint TAs (Trusty C++ / QSEE / Samsung TEEGRIS /
AOSP-Rust kmr-ta) is treated in external research (not committed here): the
canonical pointers are `BuildHiddenAuthorizations` in `system/keymaster` /
`trusty/app/keymaster` and `kmr-ta/src/keys.rs` `hidden()` in AOSP, plus
Shakevsky/Ronen/Wool USENIX'22 for the Samsung blob, and Beniamini 2016 /
NCC 2019 for QSEE. Infiniti runs QSEE-KMv3.0.3 (KM2+ class, confirmed by the
2026-05-08 device capture in `.re-notes/sessions/2026-05-08-km-revalidation.md`)
so its hidden-set is the three-field form `(verified_boot_key, verified_boot_state,
device_locked)`; `verified_boot_hash` is attestation-only and not bound to
the KEK — which is why grafted vbmeta footers do not brick `/data` today
and why patch6's no-touch contract is sufficient. (Myron's KM class is not
yet directly measured here; if its hidden-set turns out to also bind
`verified_boot_hash` like the AOSP-Rust path, patch6 stays safe but mode-1's
vbmeta-graft assumption needs re-verification on myron — separate
follow-up.)

## Scan/anchor strategy for the engine v2 patch

Implementation outline (single pass per fixture):

```c
for each KNOWN_LOCK_STATE_STRING:
  str_off = ScanFor(buf, size, str_bytes, /*Mask=*/NULL)
  if (str_off == NOT_FOUND) continue   // not this OEM

  // Find ADRP+ADD pair whose computed target == str_off.
  Pair = FindAdrpAddTargeting(buf, size, str_off)
  if (!Pair.found) return PATCH_MISS

  prior = ReadU32LE(buf, Pair.adrp_off - 4)
  if ((prior >> 24) == 0x54 && (prior & 0xF) == 0x1) {
    // Pattern B (B.NE skip-error)
    imm19 = (prior >> 5) & 0x7FFFF
    if (imm19 & 0x40000) return PATCH_MISS   // refuse negative skips
    new = 0x14000000 | (imm19 & 0x3FFFFFF)
    WriteU32LE(buf, Pair.adrp_off - 4, new)
  } else {
    // Pattern A (CBZ-into-error)
    branch_off = ScanForBranchTargeting(buf, size, Pair.adrp_off,
                                        /*accept_cbz=*/TRUE,
                                        /*accept_cbnz=*/FALSE,
                                        /*accept_bcond=*/FALSE)
    if (!branch_off) return PATCH_MISS
    WriteU32LE(buf, branch_off, 0xD503201F)   // NOP
  }
```

`FindAdrpAddTargeting` and `ScanForBranchTargeting` are small AArch64
helpers — the same decoder vendoring effort listed in
`tests/051_gbl_root_canoe_regression.sh` for patches 3, 4, 5, 7-fallback,
8, 9-part-1. One-time gate-opener.

**CI gate:** each known string present in a fixture must yield exactly one
match. Total patch-site count == count of supported strings present. If
either is unique-but-zero or count is wrong, `tools/abl-patcher --check-anchors-only`
fails and CI fails.

## Cross-fixture stability summary

| Aspect | Infiniti | Myron | Stable? |
|--------|----------|-------|---------|
| Strings present | 4 of 4 | 4 of 4 | yes |
| Pattern split (which gate uses A vs B) | Flash/Erase A; Slot/Snap B | identical | **yes** |
| Number of gates per pattern | 2 + 2 | 2 + 2 | yes |
| Branch offset values | per-gate | differ | NO — anchor on string, not on branch bytes |
| ADD imm12 (= string suffix) | `0xCF / 0x02 / 0xA13 / 0x663` | `0xEEE / 0x79 / 0x59E / 0x307` | NO — same reason |

Conclusion: anchor on the string content (universal across both fixtures),
derive the ADRP+ADD location from a backward search, then dispatch A vs B
on the preceding word's opcode bits.

## Ghidra bookmarks

All four gate sites bookmarked under category `patch6` on both programs in
the `gbl_root_canoe` project: `LinuxLoader_infiniti.efi` and
`LinuxLoader.efi`.

## Open items (separate plan)

1. Vendor `arm64_inst_decoder.h` (or a minimal subset covering ADRP, ADD,
   CBZ, B.cond, B unconditional, and offset resolvers) into
   `GblChainloadPkg/Library/DynamicPatchLib/Internal/`. Same gate-opener
   that patches 3, 4, 5, 6, 8, 9-part-1 all need per the
   `tests/051_gbl_root_canoe_regression.sh` survey.
2. Implement `patch6-lock-state-fastboot-gate` (`SCOPE_MODE_1`,
   `Mandatory=TRUE`) in `GblChainloadPkg/Library/DynamicPatchLib/mode_1/`.
3. Add `tests/fixtures/patches-patch6/{infiniti,myron}/{input,expected}.bin`
   and a byte-diff regression test wired into
   `tests/042_dynamic_patch_harness.sh`.
4. Stage+`oem boot-efi` smoke: confirm
   `DynamicPatch: patch6-lock-state-fastboot-gate OK` in the boot log,
   then user-driven `fastboot flash <HLOS partition>` from inside mode-1
   to confirm the command is accepted (per CLAUDE.md, non-HLOS flashes
   remain user-initiated only).
