# patch10 — libavb force-AVB-success — RE findings

**Date:** 2026-05-12
**Replaces:** patch9 v2 (`docs/re/patch9-v2-disassembly.md`)
**Fixtures:** `LinuxLoader_infiniti.efi` (OnePlus 15), `LinuxLoader.efi`
(myron — Xiaomi Redmi K90 Pro Max / POCO F8 Ultra).
**Ghidra project:** `gbl_root_canoe` — bookmarks under category `patch10`.

## Goal

Make mode-1 fakelock work end-to-end for any custom payload — including
the cases patch9 couldn't reach. Specifically: fix `OK_NOT_SIGNED` /
`ERROR_INVALID_METADATA` from a custom recovery on the **primary Android
boot path** (not just recovery-mode), which patch9 left fatal because
`result_should_continue(INVALID_METADATA)` is `false` and ABL's post-call
classifier (`cmp w24, #5; b.hi fatal`) treats it as fatal too.

## Why patch9 was insufficient

patch9 v2 patched ABL's wrapper `LoadImageAndAuthVB2` (in QcomModulePkg),
not libavb itself. It forced ABL's local `AllowVerificationError` to TRUE
(Site V) and NOP'd ABL's post-call cbz gates (Site G, Site C). That suffices
when libavb returns a result class that ABL's classifier considers
recoverable (OK / ERROR_VERIFICATION / ROLLBACK_INDEX / PUBLIC_KEY_REJECTED).

But two failure shapes leak through:

1. **`AVB_SLOT_VERIFY_RESULT_ERROR_INVALID_METADATA`** — what libavb returns
   when a chained vbmeta is structurally invalid (missing footer, corrupt
   header). Common for OrangeFox-style custom recoveries that strip vbmeta
   entirely. libavb bails at its own internal gate
   (`!allow_verification_error || !result_should_continue(ret)`, source
   line 1032) BEFORE patch9's Site G/C even get to vote. ABL receives
   `Result = INVALID_METADATA`, the classifier `cmp w24, #5; b.hi fatal`
   sees 7 > 5, takes the b.hi fatal path. Red screen.

2. **`PATCH_MISS` on myron** — patch9's Site V/G anchors hard-coded bytes
   reflecting one compiler's register choice (`cset w24, ne`). Xiaomi's
   miui_codes2 build chose `cset w26, ne` instead, so the 15-byte Site V
   anchor (`f8 07 9f 1a` for `cset w24, ne`) missed entirely. This was
   already documented in `docs/re/patch9-v2-disassembly.md`'s myron section
   as "myron lacks libavb path" — that's wrong, see "Myron has libavb"
   below.

## Strategy: rewrite libavb itself

Anchor inside libavb's `avb_slot_verify` (in `edk2/QcomModulePkg/Library/avb/libavb/avb_slot_verify.c`),
not in QcomModulePkg's wrapper. libavb is identical AOSP source across all
Qualcomm OEM compiles — same code, different compilers picking different
register numbers. Anchor on the source-text strings + function-prologue
shape, not byte-exact instruction encodings.

### Two rewrites, one anchor

**10a — Force AVE bit high at function entry.** The compiler stashes the
`flags` argument (w3 per AArch64 ABI) into a callee-saved register at
function entry, e.g. `mov w26, w3`. Rewrite to `orr w26, w3, #1`. Bit 0 of
`flags` is `AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR`; forcing it
high inside the saved register means every downstream
`if (!allow_verification_error)` check (seven of them at L1453, L808, L863,
L915, L941, L1032, L1453 in the source) reads bit 0 = 1 → never bails.

**10c — Force OK return at function exit.** The compiler materializes the
final return value via `mov w0, w28` (or `mov w0, wM` for whatever
register holds the local `ret`) immediately before the stack-teardown +
`ret`. Rewrite to `mov w0, #0`. The function always returns
`AVB_SLOT_VERIFY_RESULT_OK`, regardless of internal `ret` state.

Together: libavb processes everything permissively (10a), populates
`SlotData` fully even past recoverable errors, then returns OK (10c). ABL
sees clean OK + populated SlotData → success branch → green/locked cmdline.

### Why not also patch result_should_continue (10b)?

We considered patching `result_should_continue` (or the L1032 gate directly)
to neutralize the non-recoverable error-class veto. We dropped it because
10a + 10c together are sufficient:

- For recoverable classes (ERROR_VERIFICATION, ROLLBACK_INDEX,
  PUBLIC_KEY_REJECTED, OK): 10a forces libavb past the gate, SlotData is
  fully populated, 10c forces OK. Clean.
- For non-recoverable classes (INVALID_METADATA, OOM, IO, etc.): libavb
  bails at L1032 with partial SlotData. 10c forces OK return. ABL sees OK
  with possibly-partial SlotData. **In practice this works** because main
  bootpath doesn't strictly require every per-partition vbmeta digest in
  `androidboot.vbmeta.*.digest` — Android tolerates absent ones. Risk:
  silent acceptance of genuinely corrupted images (no red screen). Mode-1
  scope contains the blast radius; user accepts the trade-off.

If a future case exposes a need for full SlotData on INVALID_METADATA, a
follow-up `patch10b` can rewrite `result_should_continue` to always
return TRUE.

## Anchor details

### String anchor

```
"Persistent values required for AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO"
```

Verbatim source text from `avb_slot_verify.c:1466` (the error message when
`AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO` is requested but
`AvbOps->{read,write}_persistent_value` aren't implemented). Used solely
as a function-locator — the patch never executes the path that emits it.

Cross-fixture uniqueness verified:
- infiniti: 1 match at `0x587BE`
- myron: 1 match at `0x72E4A` (different offset, same content)

### Function entry

Walk backward from the string's ADRP+ADD pair to the first PACIASP
instruction (`d503233f` LE: `3f 23 03 d5`). PACIASP is the standard ARMv8.3
pointer-auth function-entry marker; AArch64 PAC-equipped compilers emit it
as the first instruction of every function.

| Fixture | Function entry | String at |
|---|---|---|
| infiniti | `0x2AC88` | `0x587BE` |
| myron    | `0x290C4` | `0x72E4A` |

### 10a site (entry `mov wN, w3`)

Scan forward from function entry up to 30 instructions for the pattern
`(word & 0xFFFFFFE0) == 0x2A0303E0` — that's `mov wN, w3` (`ORR Wd, WZR,
W3`, alias for `mov`) with any Rd. Extract Rd from low 5 bits.

| Fixture | 10a site | Original | Patched (orr wRd, w3, #1) |
|---|---|---|---|
| infiniti | `0x2ACA0` (entry +0x18) | `0x2A0303FA` = `mov w26, w3` | `0x3200007A` |
| myron    | `0x290DC` (entry +0x18) | `0x2A0303FA` = `mov w26, w3` | `0x3200007A` |

Both compilers happened to choose w26 for the stash; the patch handles any
Rd via the masked-pattern scan.

Encoding for `orr wRd, w3, #1`:
- Top byte = `0x32` (ORR-immediate)
- byte 1 = 0x00, byte 2 = 0x00 (N=0, immr=0, imms=0 → bitmask `#1`)
- byte 0 low 3 bits = Rn[2:0] for Rn=w3 = `011`
- byte 0 low 5 bits = (3 << 5) | Rd lower 5 bits → `0x60 | Rd`
- Combined word: `0x32000060 | Rd`

### 10c site (exit `mov w0, wM`)

Scan forward from function entry for the first `ret` (`D65F03C0` LE:
`c0 03 5f d6`). Then walk backward up to ~16 instructions for
`(word & 0xFFE0FFFF) == 0x2A0003E0` — that's `mov w0, wM` (`ORR W0, WZR,
Wm`, mov-alias). Rewrite that word to `mov w0, #0` (`0x52800000`,
MOVZ W0, #0 alias).

| Fixture | 10c site | Original | Patched |
|---|---|---|---|
| infiniti | `0x2AE8C` (ret -0x14) | `0x2A1C03E0` = `mov w0, w28` | `0x52800000` = `mov w0, #0` |
| myron    | `0x292C8` (ret -0x14) | `0x2A1C03E0` = `mov w0, w28` | `0x52800000` = `mov w0, #0` |

Same offset from ret on both fixtures. Same Rm (w28). Same rewrite.

## Cross-fixture stability summary

| Aspect | Infiniti | Myron | Stable? |
|---|---|---|---|
| Anchor string present | yes (`0x587BE`) | yes (`0x72E4A`) | yes |
| PACIASP at function entry | yes | yes | yes |
| 10a pattern matches at +0x18 from entry | yes | yes | yes |
| 10c pattern matches at -0x14 from ret | yes | yes | yes |
| Compiler-chosen Rd for AVE stash | w26 | w26 | yes (coincidence; patch handles any) |
| Compiler-chosen Rm for ret value | w28 | w28 | yes (coincidence; patch handles any) |

The patch implementation does not depend on these coincidences — Rd and Rm
are extracted from the matched word at runtime.

## Myron has libavb (correction to prior RE)

`docs/re/patch9-v2-disassembly.md` concluded myron lacked the libavb path.
That conclusion was based on the 15-byte Site V anchor not matching and
the assumption that no other libavb sites would either. Re-RE 2026-05-12
showed myron has the full libavb path:

- `OK_NOT_SIGNED` string at `0x7B4A2`
- `avb_slot_verify.c` source-path string at `0x7228D` (Xiaomi miui_codes2
  build of the same upstream file)
- `Hash of data does not match digest in descriptor.\n` at `0x7C07B`
- `AvbSlotVerifyAndBuildCmdline` function (= `avb_slot_verify` after
  Ghidra auto-naming) at `0x290C4`

The original conclusion was wrong; the byte-level anchors patch9 used
were just too narrow. patch10's string-based anchor sees through compiler
variation and works on both.

## Validation

Host-side (`tools/abl-patcher` + `tests/patches/test_patch10`):
- infiniti: `applied=3 missed=0 worst=0`. Byte-equivalent rewrites at the
  documented offsets. Idempotency-via-miss confirmed.
- myron: `applied=3 missed=0 worst=0`. Same.

Pending device-side smoke (mode-1 EFI staged + `oem boot-efi` + custom
recovery on primary Android boot path) — tracked separately.

## Open items (separate plans)

- If `INVALID_METADATA` cases hit partial-SlotData crashes in practice, add
  patch10b: rewrite `result_should_continue` to always return TRUE (locate
  via callee xref from L1032).
- Suppress `avb_errorv` log lines in bootloader_log — purely cosmetic;
  doesn't affect bootflow but clutters captures.
- Drop the legacy patch9 Site V/G/C signatures from the tree (still
  referenced from `docs/re/patch9-v2-disassembly.md` for historical
  context; removing them is a documentation cleanup).
