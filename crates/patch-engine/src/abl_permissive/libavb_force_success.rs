//! patch10 — libavb force-AVB-success.
//!
//! Port of
//! `GblChainloadPkg/Library/DynamicPatchLib/abl_permissive/libavb_force_success.c`.
//!
//! Anchor on the unique libavb string
//! `"Persistent values required for AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO"`
//! (verbatim AOSP source text in `avb_slot_verify.c`). From the string xref,
//! locate the enclosing function entry via PACIASP backscan, then apply two
//! rewrites inside `avb_slot_verify`:
//!
//! - **10a** — entry-prologue `mov wN, w3` → `orr wN, w3, #1` (any Rd).
//!   Forces bit 0 of `flags`
//!   (`AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR`) high in the
//!   saved-flags register. All ~7 downstream
//!   `!allow_verification_error` gates inside libavb pass through.
//! - **10c** — exit return-value `mov w0, wM` → `mov w0, #0`. Forces the
//!   function to return `AVB_SLOT_VERIFY_RESULT_OK` regardless of
//!   internal `ret` state.
//!
//! See `docs/project/re-findings.md`.

use crate::internal::arm64_decode::find_adrp_add_targeting;
use crate::internal::encode::{read_instr_u32, write_instr_u32};
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// patch10 anchor string (verbatim AOSP source text from
/// `avb_slot_verify.c:1466`). Used solely as a unique function-locator.
const PATCH10_ANCHOR: &[u8] =
    b"Persistent values required for AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO";

/// PACIASP — function-entry marker on AArch64 PAC builds.
const ARM64_PACIASP_WORD: u32 = 0xD503_233F;
/// RET.
const ARM64_RET_WORD: u32 = 0xD65F_03C0;
/// `mov w0, #0` (MOVZ w0, #0).
const ARM64_MOV_W0_ZERO: u32 = 0x5280_0000;

/// `mov wN, w3` template (any Rd in low 5 bits).
const ARM64_MOV_FROM_W3_MASK: u32 = 0xFFFF_FFE0;
const ARM64_MOV_FROM_W3_PAT: u32 = 0x2A03_03E0;

/// `orr wN, w3, #1` template — OR in Rd into the low 5 bits.
const ARM64_ORR_W3_ONE_BASE: u32 = 0x3200_0060;

/// `mov w0, wM` template — match on Rd=0, Rn=WZR; Rm wildcarded.
const ARM64_MOV_TO_W0_MASK: u32 = 0xFFE0_FFFF;
const ARM64_MOV_TO_W0_PAT: u32 = 0x2A00_03E0;

/// patch10 apply function.
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];

    // 1. Find the unique libavb anchor string.
    let (r, str_off) = scan_for(active, PATCH10_ANCHOR, None);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return PatchOutcome::Ambiguous,
        _ => return PatchOutcome::Miss,
    }

    // 2. Find the ADRP+ADD pair in .text that loads the string pointer.
    let (r, adrp_off) = find_adrp_add_targeting(active, str_off, true);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return PatchOutcome::Ambiguous,
        _ => return PatchOutcome::Miss,
    }

    // 3. Walk backward from ADRP to the nearest PACIASP (function entry).
    let mut func_entry: u32 = 0;
    let mut probe = adrp_off;
    while probe >= 4 {
        let word = read_instr_u32(active, probe - 4);
        if word == ARM64_PACIASP_WORD {
            func_entry = probe - 4;
            break;
        }
        probe -= 4;
    }
    if func_entry == 0 {
        return PatchOutcome::Miss;
    }

    // 4. Forward from func_entry, scan ~30 instructions for `mov wN, w3`.
    let mut mov_from_w3_off: u32 = 0;
    let scan_limit = func_entry.saturating_add(30 * 4);
    let mut p = func_entry;
    while p + 4 <= scan_limit && (p as usize) + 4 <= active.len() {
        let word = read_instr_u32(active, p);
        if (word & ARM64_MOV_FROM_W3_MASK) == ARM64_MOV_FROM_W3_PAT {
            mov_from_w3_off = p;
            break;
        }
        p += 4;
    }
    if mov_from_w3_off == 0 {
        return PatchOutcome::Miss;
    }

    // 5. Forward from func_entry, scan until first `ret`.
    let mut ret_off: u32 = 0;
    let mut p = func_entry;
    while (p as usize) + 4 <= active.len() {
        if read_instr_u32(active, p) == ARM64_RET_WORD {
            ret_off = p;
            break;
        }
        p += 4;
    }
    if ret_off == 0 {
        return PatchOutcome::Miss;
    }

    // 6. Walk backward from ret (up to 0x40 bytes) for `mov w0, wM`.
    let mut mov_to_w0_off: u32 = 0;
    let window_floor = ret_off.saturating_sub(0x40);
    let mut p = ret_off;
    while p > func_entry && p > window_floor {
        let word = read_instr_u32(active, p - 4);
        if (word & ARM64_MOV_TO_W0_MASK) == ARM64_MOV_TO_W0_PAT {
            mov_to_w0_off = p - 4;
            break;
        }
        if p < 4 {
            break;
        }
        p -= 4;
    }
    if mov_to_w0_off == 0 {
        return PatchOutcome::Miss;
    }

    // 7. Apply both rewrites — preserve Rd from the mov-from-w3.
    let rd = read_instr_u32(active, mov_from_w3_off) & 0x1F;
    let orr_insn = ARM64_ORR_W3_ONE_BASE | rd;
    write_instr_u32(active, mov_from_w3_off, orr_insn);
    write_instr_u32(active, mov_to_w0_off, ARM64_MOV_W0_ZERO);

    PatchOutcome::Ok
}
