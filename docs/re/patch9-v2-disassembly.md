# Patch9 v2 disassembly findings

Date: 2026-05-09
Plan: docs/superpowers/plans/2026-05-09-mode0-logfs-fix-and-patch9-v2.md

Tool: `llvm-objdump 18.1.3` with `--triple=aarch64-unknown-unknown` on raw COFF-arm64 PE files.

## Per-fixture sites

### infiniti (gbl\_root\_canoe, reference)

- Site V (cset for AllowVerificationError): file offset `0x00025388`
- Instruction at Site V: `f8079f1a` (`cset w24, ne`)
- 32 bytes preceding Site V: `680080527d174f39ff4f00f9ffa300f9e00306adbf030071e00307ade00308ad`
- Site G (post-libavb cbz on AllowVerificationError): file offset `0x00025a64`
- Instruction at Site G: `bd170034` (`cbz w29, 0x25d58`)
- 32 bytes preceding Site G: `58f6ff97e1c30291e4630291e00315aae20316aae303172a8b140094f803002a`

Assembly context at Site V:
```
25368: 52800068   mov  w8, #0x3
2536c: 394f177d   ldrb w29, [x27, #0x3c5]    ; IsUnlocked() result
25370: f9004fff   str  xzr, [sp, #0x98]
25374: f900a3ff   str  xzr, [sp, #0x140]
25378: ad0603e0   stp  q0, q0, [sp, #0xc0]
2537c: 710003bf   cmp  w29, #0x0
25380: ad0703e0   stp  q0, q0, [sp, #0xe0]
25384: ad0803e0   stp  q0, q0, [sp, #0x100]
25388: 1a9f07f8   cset w24, ne               ; AllowVerificationError = (w29 != 0)
```

Assembly context at Site G:
```
25a50: aa1503e0   mov  x0, x21
25a54: aa1603e2   mov  x2, x22
25a58: 2a1703e3   mov  w3, w23
25a5c: 9400148b   bl   0x2ac88                ; avb_slot_verify call
25a60: 2a0003f8   mov  w24, w0                ; Result = avb_slot_verify(...)
25a64: 340017bd   cbz  w29, 0x25d58           ; if (!AllowVerificationError) goto fatal
25a68: f00001b9   adrp x25, 0x5c000
25a70: 7100171f   cmp  w24, #0x5
25a74: 54001788   b.hi 0x25d64               ; if (Result > 5) goto fatal
```

### infiniti-EU-16.0.5.703

- Site V offset: `0x000253dc`
- Instruction at Site V: `f8079f1a` (`cset w24, ne`)
- 32 bytes preceding Site V: `680080527d174f39ff4f00f9ffa300f9e00306adbf030071e00307ade00308ad`
- Site G offset: `0x00025ab8`
- Instruction at Site G: `bd170034` (`cbz w29, 0x25dac`)
- 32 bytes preceding Site G: `58f6ff97e1c30291e4630291e00315aae20316aae303172a8b140094f803002a`

Notes: Code is byte-for-byte identical to reference at both sites. The function sits
at a +0x54 byte offset within the binary (consistent global shift, not a site-local
difference). Anchor-match verified exact.

### infiniti-IN-16.0.7.201

- Site V offset: `0x000238c4`
- Instruction at Site V: `f8079f1a` (`cset w24, ne`)
- 32 bytes preceding Site V: `e00306ad083804b9e002009000702a9128a140f97f030071e00307ade00308ad`
- Site G offset: `0x00023ff4`
- Instruction at Site G: `7b170034` (`cbz w27, 0x242e0`)
- 32 bytes preceding Site G: `cbf9ff97e1c30291e4630291e00315aae20316aae303172af3150094f803002a`

Notes: Register allocation differs from reference: AllowVerificationError is held in
`w27` instead of `w29`. The pre-Site-V sequence diverges from reference/EU in the
first 20 bytes (stack layout, adrp targets, ldrb source offset differs: `[x8, #0x3a5]`
vs reference `[x27, #0x3c5]`). Bytes 21-31 of the pre-32 window are identical to
reference. Exact 32-byte anchor does not match but the 15-byte Site V anchor (last
11 bytes before cset + cset) does match uniquely.

Assembly context at Site V:
```
23898: 394e951b   ldrb w27, [x8, #0x3a5]     ; IsUnlocked() result, different reg
2389c: 52800068   mov  w8, #0x3
238b8: 7100037f   cmp  w27, #0x0
238bc: ad0703e0   stp  q0, q0, [sp, #0xe0]
238c0: ad0803e0   stp  q0, q0, [sp, #0x100]
238c4: 1a9f07f8   cset w24, ne               ; AllowVerificationError (still w24)
```

Assembly context at Site G:
```
23fec: 940015f3   bl   0x297b8               ; avb_slot_verify call
23ff0: 2a0003f8   mov  w24, w0               ; Result
23ff4: 3400177b   cbz  w27, 0x242e0          ; cbz w27 (not w29)
24000: 7100171f   cmp  w24, #0x5
```

### fairlady-CN-16.0.7.200

- Site V offset: `0x00023654`
- Instruction at Site V: `f8079f1a` (`cset w24, ne`)
- 32 bytes preceding Site V: `e00306ad083804b9e002009000f02e9128a140f97f030071e00307ade00308ad`
- Site G offset: `0x00023d84`
- Instruction at Site G: `7b170034` (`cbz w27, 0x24070`)
- 32 bytes preceding Site G: `cbf9ff97e1c30291e4630291e00315aae20316aae303172af6150094f803002a`

Notes: Same register pattern as IN (w27). Pre-Site-V bytes 0-19 differ from IN in
one word (ldrb source offset: `[x8, #0x4c5]` vs `[x8, #0x3a5]` in IN). Bytes 20-31
are stable across all 4 fixtures. Site G pre-32 is nearly identical to IN with one
word difference at position 24 (`f6150094` vs `f3150094` — different bl target).

Assembly context at Site V:
```
23628: 3953151b   ldrb w27, [x8, #0x4c5]     ; IsUnlocked(), offset differs from IN
2362c: 52800068   mov  w8, #0x3
23648: 7100037f   cmp  w27, #0x0
2364c: ad0703e0   stp  q0, q0, [sp, #0xe0]
23650: ad0803e0   stp  q0, q0, [sp, #0x100]
23654: 1a9f07f8   cset w24, ne
```

Assembly context at Site G:
```
23d7c: 940015f6   bl   0x29554               ; avb_slot_verify
23d80: 2a0003f8   mov  w24, w0
23d84: 3400177b   cbz  w27, 0x24070
23d90: 7100171f   cmp  w24, #0x5
```

### myron

- Site V offset: NOT PRESENT
- Site G offset: NOT PRESENT

Notes: `myron.efi` has two occurrences of the `cset w24, ne` instruction
(`0x56d8c`, `0x56e58`). Both have the pattern `blr x8 / cmp w0, #0 / cset w24, ne`
(vtable dispatch result check), not the `ldrb [locked_state_var] / cmp / cset`
pattern that identifies the IsUnlocked site. Neither the Site V 15-byte anchor nor
the Site G 22-byte anchor matches anywhere in myron. The binary does not appear to
contain the GBL+AVB libavb call path; it is likely a different product model without
the standard VerifiedBoot flow, or the libavb path has been removed/refactored.
PATCH\_MISS for both sites.

## Cross-fixture comparison

### Site V preceding-32 bytes

| Position (0-31) | infiniti | EU-16.0.5.703 | IN-16.0.7.201 | fairlady-CN | Stable? |
|---|---|---|---|---|---|
| 0-9   | `680080527d174f39ff4f` | `680080527d174f39ff4f` | `e00306ad083804b9e002` | `e00306ad083804b9e002` | NO |
| 10    | `00`                   | `00`                   | `00`                   | `00`                   | YES |
| 11-20 | `f9ffa300f9e00306adbf` | `f9ffa300f9e00306adbf` | `009000702a9128a140f9` | `009000f02e9128a140f9` | NO |
| 21-31 | `030071e00307ade00308ad` | `030071e00307ade00308ad` | `030071e00307ade00308ad` | `030071e00307ade00308ad` | YES |

Stable bytes (12/32): positions 10 and 21-31.
Best contiguous stable run: positions 21-31 (11 bytes): `030071e00307ade00308ad`

### Site G preceding-32 bytes

| Position (0-31) | infiniti | EU-16.0.5.703 | IN-16.0.7.201 | fairlady-CN | Stable? |
|---|---|---|---|---|---|
| 0-1   | `58f6` | `58f6` | `cbf9` | `cbf9` | NO |
| 2-23  | `ff97e1c30291e4630291e00315aae20316aae303172a` | (same) | (same) | (same) | YES |
| 24-25 | `8b14` | `8b14` | `f315` | `f615` | NO |
| 26-31 | `0094f803002a` | `0094f803002a` | `0094f803002a` | `0094f803002a` | YES |

Stable bytes (28/32): positions 2-23 and 26-31.
Best contiguous stable run: positions 2-23 (22 bytes): `ff97e1c30291e4630291e00315aae20316aae303172a`

## Anchor candidates

### Site V anchor

- **Anchor bytes (15 bytes = 11-byte pre-cset tail + cset instruction):**
  `030071e00307ade00308adf8079f1a`
- Derived from: bytes at `[sv_off - 11 .. sv_off + 3]` inclusive
- Mask: `ffffffffffffffffffffffffffffff` (all bytes exact — no wildcards needed)
- Rewrite delta from anchor start to patch target: `+11` bytes (the cset is at anchor[11..14])
- Replacement instruction: `38008052` (`mov w24, #1`) — forces AllowVerificationError=TRUE,
  preserves w24 register convention used at Site G
- Notes: The 4 fixtures with libavb path all match uniquely. The 11-byte prefix is
  stable because it encodes `cmp wN, #0 / stp q0,q0 / stp q0,q0` which is invariant
  regardless of which register holds the IsUnlocked result.

### Site G anchor

- **Anchor bytes (22 bytes from pre-32 positions 2-23):**
  `ff97e1c30291e4630291e00315aae20316aae303172a`
- Derived from: bytes at `[sg_off - 30 .. sg_off - 9]` inclusive
- Mask: `ffffffffffffffffffffffffffffffffffffffffffff` (all bytes exact)
- Rewrite delta from anchor start to patch target: `+30` bytes (cbz is at anchor\_start + 30)
- Replacement instruction: `1f2003d5` (`nop`) — turns the cbz into a no-op so control
  falls through to the recoverable-continue path; works regardless of whether the
  register is w29 (reference/EU) or w27 (IN/fairlady)
- Notes: The 22-byte anchor encodes the argument-setup sequence before `bl avb_slot_verify`
  plus `mov w24, w0` (capturing the result). This sequence is highly stable because it
  reflects the fixed ABI of the avb_slot_verify call. The two variable words (positions
  0-1 and 24-25) are the bl displacement and the preceding bl displacement — both
  call-target-relative and fixture-specific.

## Cross-binary anchor uniqueness check

| Fixture | Site V anchor (15 B) | Site G anchor (22 B) |
|---|---|---|
| infiniti (reference) | 1 — PATCH\_OK | 1 — PATCH\_OK |
| EU-16.0.5.703 | 1 — PATCH\_OK | 1 — PATCH\_OK |
| IN-16.0.7.201 | 1 — PATCH\_OK | 1 — PATCH\_OK |
| fairlady-CN-16.0.7.200 | 1 — PATCH\_OK | 1 — PATCH\_OK |
| myron | 0 — PATCH\_MISS | 0 — PATCH\_MISS |

Both anchors are unique (count=1) in all 4 fixtures that contain the libavb call
path. Myron lacks the path entirely; the patch engine should detect PATCH\_MISS and
skip both sites for that binary.

## Approach decision

**Selected: Approach A — Site V (cset rewrite) + Site G (cbz → nop).**

### Rationale

Approach A's anchor stability is excellent:

- Site V anchor (15 bytes, exact): unique match on all 4 fixtures with libavb.
- Site G anchor (22 bytes, exact): unique match on all 4 fixtures with libavb.
- Both replacements are register-stable (mov w24, #1) or register-agnostic (nop).
- Stop-line satisfied: 4-of-5 PATCH_OK ≥ 3-of-5 minimum (myron lacks libavb path; expected MISS).

Approach B (libavb-internal return-value remap) was deemed unnecessary:

- Approach A already produces a clean two-rewrite patch with maximally durable anchors.
- Approach B's premise (single-rewrite at libavb's return path) would have been preferred only if Approach A's anchors lacked cross-binary stability. They don't.
- Adding Approach B would add complexity (intercepting libavb's return-value setup at a single point in a function with multiple return paths) without measurable benefit.

### Implementation specifics for Approach A

**Site V (cset rewrite):**
- Anchor (15 bytes, exact match): `030071e00307ade00308adf8079f1a`
- Match offset → cset instruction: anchor_start + 11 bytes
- Replacement instruction: `38008052` (little-endian for `mov w24, #1` = `0x52800038`)

**Site G (cbz → nop):**
- Anchor (22 bytes, exact match): `ff97e1c30291e4630291e00315aae20316aae303172a`
- Match offset → cbz instruction: anchor_start + 30 bytes
- Replacement instruction: `1f2003d5` (little-endian for `nop` = `0xD503201F`)

### Per-fixture expected outcomes

| Fixture | Site V offset | Site G offset | Site C offset | patch9 v2 outcome |
|---|---|---|---|---|
| infiniti (reference) | 0x25388 | 0x25A64 | 0x25C44 | PATCH_OK |
| infiniti-EU-16.0.5.703 | 0x253DC | 0x25AB8 | 0x25C98 | PATCH_OK |
| infiniti-IN-16.0.7.201 | 0x238C4 | 0x23FF4 | 0x241D4 | PATCH_OK |
| fairlady-CN-16.0.7.200 | 0x23654 | 0x23D84 | 0x23F64 | PATCH_OK |
| myron | not present | not present | not present | PATCH_MISS (expected — myron has no libavb call path; not a defect) |

### Approach B (not selected) — reference for future iterations

If, in a future ABL build, Approach A's anchors lose unique-match stability and shorter masked anchors don't suffice, Approach B becomes the alternative:

- Locate `avb_slot_verify_full` (or `avb_slot_verify`) by string-anchoring on libavb error strings (`OK_NOT_SIGNED`, `Hash of data does not match descriptor`, etc.).
- Find the return-value-setup site for the recoverable-error case (the `mov w0, #4` for OK_NOT_SIGNED, where 4 is the AvbSlotVerifyResult enum value).
- Rewrite to `mov w0, #0` (OK).
- Single-site rewrite. ABL sees Result=OK always when SlotData is populated, takes the OK fast path naturally — no Site G rewrite needed.

This is documented as a fallback architecture, not selected for plan 2.
