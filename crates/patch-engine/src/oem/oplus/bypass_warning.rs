//! patch7 — orange-screen / unlock-warning / 5-second boot-delay gate.
//!
//! Port of
//! `GblChainloadPkg/Library/DynamicPatchLib/oem/oplus/bypass_warning.c`.
//!
//! `LinuxLoaderEntry` guards an orange-state warning block with a CBZ
//! that skips the block when the device is locked. Rewriting that CBZ
//! as an unconditional B always skips the block, regardless of lock
//! state.
//!
//! String-anchored: the warning text
//! `"Your device has been unlocked and can't be trusted"` is invariant
//! across OTA builds and referenced by exactly one ADRP+ADD. Resolve
//! that pair, then walk backward (window = 0x40 bytes) to the nearest
//! `CBZ Wn` — the lock-state guard — and rewrite it to an unconditional
//! B (branch target preserved).
//!
//! Idempotent: after the rewrite the guard slot is a forward
//! unconditional B; the backward scan treats that as already-applied
//! and returns Ok.
//!
//! Verified on EU-16.0.5.703 (CBZ @0x78F0), IN-16.0.7.201 (@0x76D8) and
//! fairlady-CN-16.0.7.200 (@0x76D8).
//!
//! Non-mandatory — cosmetic only; `Miss` on non-matching ABLs is a
//! clean no-op.

use crate::internal::arm64_decode::{decode_branch, find_adrp_add_targeting, Arm64InsnKind};
use crate::internal::encode::{read_instr_u32, rewrite_b_uncond};
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// Patch7 anchor — the orange-state warning text.
pub const PATCH7_WARN_STR: &[u8] = b"Your device has been unlocked and can't be trusted";

/// Backward-scan window (bytes) from the warning-string ADRP to the
/// guard CBZ. Observed CBZ is at ADRP-0x1C on every oplus build; this
/// window leaves margin while staying inside the warning-render prologue.
pub const PATCH7_BACK_SCAN_WINDOW: u32 = 0x40;

/// patch7 apply function.
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];

    // 1. Locate the warning string. Absent → clean MISS (non-oplus ABL).
    let (r, str_off) = scan_for(active, PATCH7_WARN_STR, None);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return PatchOutcome::Ambiguous,
        _ => return PatchOutcome::Miss,
    }

    // 2. Resolve the unique ADRP+ADD pair that loads the warning string.
    let (r, adrp_off) = find_adrp_add_targeting(active, str_off, true);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return PatchOutcome::Ambiguous,
        _ => return PatchOutcome::Miss,
    }
    if adrp_off < 4 {
        return PatchOutcome::Miss;
    }

    // 3. Walk backward to the lock-state guard.
    //    CBZ Wn               → rewrite to unconditional B (skip warning)
    //    forward unconditional B → already patched (idempotent)
    let lo = if adrp_off > PATCH7_BACK_SCAN_WINDOW {
        adrp_off - PATCH7_BACK_SCAN_WINDOW
    } else {
        4
    };
    let mut probe = adrp_off - 4;
    loop {
        if probe < lo {
            break;
        }
        let word = read_instr_u32(active, probe);
        if let Some(decoded) = decode_branch(word, probe) {
            if decoded.kind == Arm64InsnKind::CbzW {
                rewrite_b_uncond(active, probe, decoded.target_off);
                return PatchOutcome::Ok;
            }
            if decoded.kind == Arm64InsnKind::B && decoded.target_off > probe {
                // Guard already rewritten — idempotent OK.
                return PatchOutcome::Ok;
            }
        }
        if probe < 4 {
            break;
        }
        probe -= 4;
    }

    PatchOutcome::Miss
}
