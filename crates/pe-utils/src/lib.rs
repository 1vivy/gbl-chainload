//! PE sanity + UTF-16LE "efisp" marker scan.
//!
//! Ports `GblChainloadPkg/Library/GblPayloadLib/PeSanity.c` (host-side PE
//! header sanity check consumed by `tools/gbl-pack`) and
//! `tools/shared/efisp_scan.h` (the 12-byte UTF-16LE "efisp\0" marker scan
//! used by `gbl-pack`'s warning path and `fv-unwrap`'s diagnostics).
//!
//! Two layers:
//!
//! - The idiomatic Rust API exposed at the crate root (`pe_sanity`,
//!   `efisp_marker_present`).
//! - The `ffi` module: `extern "C"` shims (`gbl_pe_sanity`,
//!   `gbl_contains_utf16_efisp`) preserving the C wire ABI so existing C
//!   call sites continue to link unchanged via the staticlib.
//!
//! `no_std` when targeting UEFI (`target_os = "uefi"`) so the crate can be
//! linked into `aarch64-unknown-uefi`. On hosted targets (Linux/macOS/
//! Windows), `std` stays in scope — the host staticlib build is only a
//! convenience artifact (gbl-pack et al. link the host .a) and Cargo's
//! test harness needs `std` to unwind.

#![cfg_attr(target_os = "uefi", no_std)]

// On UEFI there's no runtime to fall back on — supply a tiny abort-loop
// panic handler. Hosted builds pull this in from `std`.
#[cfg(target_os = "uefi")]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

pub mod ffi;

/// PE sanity-check failure modes.
///
/// Mirrors `enum gbl_pe_status` in `PeSanity.h` minus the `OK` variant
/// (we use `Result<(), PeError>` instead of a status enum). The
/// numeric ordering of the underlying C enum is preserved in the FFI
/// shim — see [`ffi::GblPeStatus`].
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum PeError {
    /// Buffer below the 0x200-byte minimum the C check enforces.
    TooSmall,
    /// Missing "MZ" DOS magic at offset 0.
    BadDos,
    /// `e_lfanew` out of range (or arithmetic overflow when computing
    /// the PE-header end offset).
    BadLfanew,
    /// PE signature ("PE\0\0") wrong at `e_lfanew`.
    BadPeMagic,
    /// COFF Machine field not `IMAGE_FILE_MACHINE_ARM64` (0xAA64).
    BadMachine,
    /// PE32+ optional-header magic (0x020B) not present.
    BadOptMagic,
    /// Subsystem not `EFI_APPLICATION` (10).
    BadSubsys,
    /// AddressOfEntryPoint is zero or beyond `SizeOfImage`.
    EntryOutOfBounds,
}

// --- PE-header constants (mirror PeSanity.c) -------------------------

const DOS_E_LFANEW: usize = 0x3C;
const COFF_OPT_HDR_SIZE_OFF: usize = 0x10; // SizeOfOptionalHeader in COFF
const OPT_MAGIC_OFF: usize = 0x00; // relative to OptionalHeader start
const OPT_ENTRY_POINT_OFF: usize = 0x10;
const OPT_SIZE_OF_IMAGE_OFF: usize = 0x38; // PE32+
const OPT_SUBSYSTEM_OFF: usize = 0x44; // PE32+

const PE_MAGIC_BYTES: u32 = 0x0000_4550; // "PE\0\0"
const MACHINE_AARCH64: u16 = 0xAA64;
const OPT_MAGIC_PE32P: u16 = 0x020B;
const SUBSYSTEM_EFI_APP: u16 = 10;

const MIN_SIZE: usize = 0x200;

#[inline]
fn le16(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}

#[inline]
fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

/// Host-side AArch64 EFI_APPLICATION PE sanity check.
///
/// Validates the small set of fields that `gBS->LoadImage` would later
/// reject if wrong, plus the defensive checks the PE/COFF spec calls
/// out (entry point in bounds, etc.). It does NOT load or relocate the
/// image — at boot, the firmware's image loader is the runtime
/// authority.
///
/// Byte-for-byte parity with `gbl_pe_sanity` in `PeSanity.c`.
pub fn pe_sanity(pe: &[u8]) -> Result<(), PeError> {
    let size = pe.len();
    if size < MIN_SIZE {
        return Err(PeError::TooSmall);
    }
    if pe[0] != b'M' || pe[1] != b'Z' {
        return Err(PeError::BadDos);
    }

    let lfanew = le32(&pe[DOS_E_LFANEW..DOS_E_LFANEW + 4]) as usize;
    // 0x18 = PE sig (4) + COFF header up through SizeOfOptionalHeader (0x14)
    // plus enough to read SizeOfOptionalHeader itself. Then 0x10 covers the
    // start of the OptionalHeader. Use checked_add to refuse uint32 wrap.
    let pe_hdr_end = lfanew
        .checked_add(0x18)
        .and_then(|v| v.checked_add(COFF_OPT_HDR_SIZE_OFF));
    let pe_hdr_end = match pe_hdr_end {
        Some(v) if v <= size => v,
        _ => return Err(PeError::BadLfanew),
    };
    let _ = pe_hdr_end; // bounds check only

    if le32(&pe[lfanew..lfanew + 4]) != PE_MAGIC_BYTES {
        return Err(PeError::BadPeMagic);
    }

    // COFF header sits right after the PE\0\0 signature.
    let coff = lfanew + 4;
    if le16(&pe[coff..coff + 2]) != MACHINE_AARCH64 {
        return Err(PeError::BadMachine);
    }

    let opt_size = le16(&pe[coff + COFF_OPT_HDR_SIZE_OFF..coff + COFF_OPT_HDR_SIZE_OFF + 2]) as usize;
    // Full optional-header span must fit. (lfanew + 4 (PE sig) + 0x14 (COFF) + opt_size).
    let opt_end = lfanew
        .checked_add(4)
        .and_then(|v| v.checked_add(0x14))
        .and_then(|v| v.checked_add(opt_size));
    match opt_end {
        Some(v) if v <= size => {}
        _ => return Err(PeError::BadLfanew),
    }

    // OptionalHeader starts at coff + 0x14 (COFF header is 0x14 bytes).
    let opt = coff + 0x14;
    if le16(&pe[opt + OPT_MAGIC_OFF..opt + OPT_MAGIC_OFF + 2]) != OPT_MAGIC_PE32P {
        return Err(PeError::BadOptMagic);
    }
    if le16(&pe[opt + OPT_SUBSYSTEM_OFF..opt + OPT_SUBSYSTEM_OFF + 2]) != SUBSYSTEM_EFI_APP {
        return Err(PeError::BadSubsys);
    }

    let entry = le32(&pe[opt + OPT_ENTRY_POINT_OFF..opt + OPT_ENTRY_POINT_OFF + 4]);
    let soi = le32(&pe[opt + OPT_SIZE_OF_IMAGE_OFF..opt + OPT_SIZE_OF_IMAGE_OFF + 4]);
    if entry == 0 || entry >= soi {
        return Err(PeError::EntryOutOfBounds);
    }

    Ok(())
}

/// 12-byte UTF-16LE "efisp\0" marker pattern.
const EFISP_UTF16: [u8; 12] = [
    b'e', 0, b'f', 0, b'i', 0, b's', 0, b'p', 0, 0, 0,
];

/// Detect the UTF-16LE "efisp\0" marker (12 bytes) anywhere in `buf`.
///
/// Used by `gbl-pack` to warn when a `cached_abl` still carries the
/// EFISP partition-name marker (patch10/patch6 should have scrubbed it)
/// and by `fv-unwrap`'s diagnostic output. Byte-for-byte parity with
/// `gbl_contains_utf16_efisp` in `tools/shared/efisp_scan.h`.
#[inline]
pub fn efisp_marker_present(buf: &[u8]) -> bool {
    buf.windows(EFISP_UTF16.len()).any(|w| w == EFISP_UTF16)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mirror of tests/host/helpers/test_pe_sanity.c's `build_sane_pe`.
    fn build_sane_pe() -> [u8; 1024] {
        let mut p = [0u8; 1024];
        p[0] = b'M';
        p[1] = b'Z';
        p[0x3C] = 0x80; // e_lfanew = 0x80
        p[0x80] = b'P';
        p[0x81] = b'E';
        // COFF header at 0x84
        p[0x84] = 0x64;
        p[0x85] = 0xAA; // Machine = 0xAA64
        p[0x94] = 0xF0; // SizeOfOptionalHeader = 0xF0
        // Optional header at 0x98
        p[0x98] = 0x0B;
        p[0x99] = 0x02; // PE32+ magic
        p[0xA8] = 0x00;
        p[0xA9] = 0x10; // AddressOfEntryPoint = 0x1000
        p[0xD0] = 0x00;
        p[0xD1] = 0x00;
        p[0xD2] = 0x01;
        p[0xD3] = 0x00; // SizeOfImage = 0x10000
        p[0xDC] = 10; // Subsystem = EFI_APPLICATION
        p
    }

    #[test]
    fn sane_pe_passes() {
        let p = build_sane_pe();
        assert_eq!(pe_sanity(&p), Ok(()));
    }

    #[test]
    fn rejects_too_small() {
        assert_eq!(pe_sanity(&[]), Err(PeError::TooSmall));
        let tiny = [0u8; 0x1FF];
        assert_eq!(pe_sanity(&tiny), Err(PeError::TooSmall));
    }

    #[test]
    fn rejects_bad_dos() {
        let mut p = build_sane_pe();
        p[0] = b'X';
        assert_eq!(pe_sanity(&p), Err(PeError::BadDos));
    }

    #[test]
    fn rejects_bad_machine() {
        let mut p = build_sane_pe();
        p[0x84] = 0x64;
        p[0x85] = 0x86; // 0x8664 = x64
        assert_eq!(pe_sanity(&p), Err(PeError::BadMachine));
    }

    #[test]
    fn rejects_bad_subsys() {
        let mut p = build_sane_pe();
        p[0xDC] = 3; // WINDOWS_CUI
        assert_eq!(pe_sanity(&p), Err(PeError::BadSubsys));
    }

    #[test]
    fn rejects_lfanew_wraparound() {
        let mut p = build_sane_pe();
        // e_lfanew = 0xFFFFFFF0 — would wrap if checked with naive uint32 math.
        p[0x3C] = 0xF0;
        p[0x3D] = 0xFF;
        p[0x3E] = 0xFF;
        p[0x3F] = 0xFF;
        assert_eq!(pe_sanity(&p), Err(PeError::BadLfanew));
    }

    #[test]
    fn efisp_marker_unit() {
        // Embedded at offset 100 — same shape as test_efisp_scan.c.
        let mut poisoned = [0u8; 256];
        poisoned[100..112].copy_from_slice(&EFISP_UTF16);
        assert!(efisp_marker_present(&poisoned));

        let clean = [0u8; 256];
        assert!(!efisp_marker_present(&clean));

        // Marker straddling end-of-buffer must NOT match (windows() iterator
        // only emits full-length slices).
        let short: [u8; 11] = [b'e', 0, b'f', 0, b'i', 0, b's', 0, b'p', 0, 0];
        assert!(!efisp_marker_present(&short));
    }

    #[test]
    fn efisp_at_start_and_end() {
        let mut at_start = [0u8; 64];
        at_start[..12].copy_from_slice(&EFISP_UTF16);
        assert!(efisp_marker_present(&at_start));

        let mut at_end = [0u8; 64];
        at_end[52..64].copy_from_slice(&EFISP_UTF16);
        assert!(efisp_marker_present(&at_end));
    }
}
