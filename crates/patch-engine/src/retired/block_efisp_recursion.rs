//! Retired patch1 — EFISP recursion fix.
//!
//! Port of
//! `GblChainloadPkg/Library/DynamicPatchLib/retired/block_efisp_recursion.c`.
//!
//! **RETIRED 2026-05-22** — superseded by `BlockIoHook` EFISP gate. The
//! BlockIo hook refuses reads/writes against the efisp partition handle
//! at the protocol layer, which is the operational guarantee against
//! the second-stage-ABL recursion. The byte-scan implementation below
//! is preserved as a reference only; it is **not** included in any
//! active patch table.
//!
//! ## Historical context
//!
//! After gbl-chainload is loaded by stock ABL, it LoadImages an
//! unwrapped copy of ABL from the abl partition. That second-stage
//! ABL, if it sees the `efisp` partition label in its own search, will
//! load whatever is there as the next-stage GBL — i.e. us — and recurse
//! forever (hard brick on the watchdog). The original patch scanned the
//! in-memory PE for the UTF-16LE bytes of `"efisp"` and rewrote to
//! `"nulls"`.

use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// The 10-byte UTF-16LE pattern that ABL searches for when probing
/// EFISP for the GBL chainload app. Matches `kEfispUtf16Pattern` in
/// `tools/shared/patch_signatures.h` byte-for-byte. Exported so PR2
/// Task 8 (the multicall tooling collapse) can consume it from one
/// canonical Rust location.
pub const EFISP_UTF16_PATTERN: [u8; 10] = [
    0x65, 0x00, // e
    0x66, 0x00, // f
    0x69, 0x00, // i
    0x73, 0x00, // s
    0x70, 0x00, // p
];

/// patch1 (retired) apply function — preserved as documentation.
#[allow(dead_code)]
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];
    let (r, off) = scan_for(active, &EFISP_UTF16_PATTERN, None);
    match r {
        ScanResult::NotFound => return PatchOutcome::Miss,
        ScanResult::Ambiguous => return PatchOutcome::Ambiguous,
        ScanResult::Found => {}
        _ => return PatchOutcome::Miss,
    }
    // "efisp" -> "nulls", in UTF-16LE. Each char occupies 2 bytes; the
    // high byte is already 0 from the original string.
    let i = off as usize;
    active[i] = b'n';
    active[i + 2] = b'u';
    active[i + 4] = b'l';
    active[i + 6] = b'l';
    active[i + 8] = b's';
    PatchOutcome::Ok
}
