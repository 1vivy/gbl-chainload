//! patch6 — lock-state fastboot-gate.
//!
//! Port of
//! `GblChainloadPkg/Library/DynamicPatchLib/abl_permissive/fastboot_lock_gates.c`.
//!
//! ABL-permissive fakelocks the VerifiedBoot view; ABL's in-fastboot command
//! dispatcher then refuses flash / erase / slot-change / snapshot-cancel.
//! For each of those four refusal strings, locate the ADRP+ADD pair in
//! .text that loads the string pointer and rewrite the preceding gate:
//!
//! - **Pattern A** — `CBZ Wn, L_error` jumps INTO the error block. NOP
//!   the CBZ.
//! - **Pattern B** — `B.NE skip_error` jumps PAST the error block.
//!   Rewrite to unconditional `B skip_error` with the same target.
//!
//! See `docs/project/re-findings.md`.

use crate::internal::arm64_decode::{
    decode_branch, find_adrp_add_targeting, find_cond_branch_targeting, Arm64InsnKind,
};
use crate::internal::encode::{read_instr_u32, rewrite_b_uncond, write_instr_u32};
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// The four refusal strings each lock-state gate references.
const REFUSAL_STRS: &[&[u8]] = &[
    b"Flashing is not allowed in Lock State",
    b"Erase is not allowed in Lock State",
    b"Slot Change is not allowed in Lock State\n",
    b"Snapshot Cancel is not allowed in Lock State",
];

/// AArch64 NOP.
const ARM64_NOP: u32 = 0xD503_201F;

fn rewrite_one_gate(buf: &mut [u8], str_bytes: &[u8]) -> (PatchOutcome, bool) {
    // Locate refusal string. Must be unique. If absent, this OEM doesn't
    // ship that gate — treated as OK (the loop's Found counter decides
    // whether at least one gate was present).
    let (r, str_off) = scan_for(buf, str_bytes, None);
    match r {
        ScanResult::NotFound => return (PatchOutcome::Ok, false),
        ScanResult::Found => {}
        ScanResult::Ambiguous => return (PatchOutcome::Ambiguous, false),
        _ => return (PatchOutcome::Miss, false),
    }

    // ADRP+ADD pair in .text whose target equals str_off.
    let (r, adrp_off) = find_adrp_add_targeting(buf, str_off, true);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return (PatchOutcome::Ambiguous, true),
        _ => return (PatchOutcome::Miss, true),
    }

    if adrp_off < 4 {
        return (PatchOutcome::Miss, true);
    }

    // Pattern B: B.NE at ADRP-4 skipping past the error block.
    let prior_word = read_instr_u32(buf, adrp_off - 4);
    if let Some(decoded) = decode_branch(prior_word, adrp_off - 4) {
        if decoded.kind == Arm64InsnKind::Bcond && decoded.reg_or_cond == 0x1 {
            if !rewrite_b_uncond(buf, adrp_off - 4, decoded.target_off) {
                return (PatchOutcome::Miss, true);
            }
            return (PatchOutcome::Ok, true);
        }
    }

    // Pattern A: an upstream conditional branch jumps INTO the ADRP+ADD.
    // NOP it.
    let (r, branch_off) = find_cond_branch_targeting(buf, adrp_off, true);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return (PatchOutcome::Ambiguous, true),
        _ => return (PatchOutcome::Miss, true),
    }
    write_instr_u32(buf, branch_off, ARM64_NOP);
    (PatchOutcome::Ok, true)
}

/// patch6 apply function.
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];
    let mut found = 0u32;
    for &s in REFUSAL_STRS {
        let (outcome, gate_found) = rewrite_one_gate(active, s);
        if gate_found {
            found += 1;
        }
        if outcome != PatchOutcome::Ok {
            return outcome;
        }
    }
    if found == 0 {
        // No supported refusal strings present — not an oplus/oppo ABL.
        return PatchOutcome::Miss;
    }
    PatchOutcome::Ok
}
