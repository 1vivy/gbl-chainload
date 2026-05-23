//! Parity tests — Rust pe-utils outputs match the C goldens from
//! `tests/host/goldens/`. These cover the same surface as
//! `tests/host/helpers/test_pe_sanity.c` and `test_efisp_scan.c`, plus
//! the real-world PE fixture used by the host integration tests.
//!
//! The contract: any change here that diverges from the C reference is a
//! parity break and must be flagged by these tests.

use pe_utils::{efisp_marker_present, pe_sanity, PeError};
use std::fs;
use std::path::PathBuf;

fn fixtures_dir() -> PathBuf {
    // CARGO_MANIFEST_DIR = crates/pe-utils/. Climb two levels to the repo root.
    let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    p.pop();
    p.pop();
    p.push("tests");
    p.push("images");
    p.push("pe");
    p
}

#[test]
fn pe_sanity_on_real_efi() {
    let path = fixtures_dir().join("infiniti-EU-16.0.5.703.efi");
    let pe = fs::read(&path).expect("read PE fixture");
    assert_eq!(
        pe_sanity(&pe),
        Ok(()),
        "real AArch64 EFI fixture must pass sanity"
    );
}

#[test]
fn efisp_marker_detected_on_unpatched_abl() {
    // The infiniti-EU-16.0.5.703 PE is an unpatched ABL — it carries the
    // UTF-16LE "efisp" partition-name marker that patch10/patch6 would
    // later scrub. gbl-pack uses this signal to warn.
    let path = fixtures_dir().join("infiniti-EU-16.0.5.703.efi");
    let pe = fs::read(&path).expect("read PE fixture");
    assert!(
        efisp_marker_present(&pe),
        "unpatched ABL must contain the UTF-16LE efisp marker"
    );
}

// --- Mirrors tests/host/helpers/test_pe_sanity.c, byte-for-byte. -----

/// Same shape as `build_sane_pe()` in test_pe_sanity.c — a synthetic
/// AArch64 EFI_APPLICATION header that all sanity checks accept.
fn build_sane_pe() -> [u8; 1024] {
    let mut p = [0u8; 1024];
    p[0] = b'M';
    p[1] = b'Z';
    p[0x3C] = 0x80; // e_lfanew = 0x80
    p[0x80] = b'P';
    p[0x81] = b'E';
    p[0x84] = 0x64;
    p[0x85] = 0xAA; // Machine = 0xAA64
    p[0x94] = 0xF0; // SizeOfOptionalHeader
    p[0x98] = 0x0B;
    p[0x99] = 0x02; // PE32+ magic
    p[0xA8] = 0x00;
    p[0xA9] = 0x10; // AddressOfEntryPoint
    p[0xD0] = 0x00;
    p[0xD1] = 0x00;
    p[0xD2] = 0x01;
    p[0xD3] = 0x00; // SizeOfImage
    p[0xDC] = 10; // Subsystem = EFI_APPLICATION
    p
}

#[test]
fn parity_sane_pe_ok() {
    let p = build_sane_pe();
    assert_eq!(pe_sanity(&p), Ok(()));
}

#[test]
fn parity_bad_mz() {
    let mut p = build_sane_pe();
    p[0] = b'X';
    assert_eq!(pe_sanity(&p), Err(PeError::BadDos));
}

#[test]
fn parity_wrong_machine() {
    let mut p = build_sane_pe();
    p[0x84] = 0x64;
    p[0x85] = 0x86; // x64
    assert_eq!(pe_sanity(&p), Err(PeError::BadMachine));
}

#[test]
fn parity_wrong_subsys() {
    let mut p = build_sane_pe();
    p[0xDC] = 3; // WINDOWS_CUI
    assert_eq!(pe_sanity(&p), Err(PeError::BadSubsys));
}

#[test]
fn parity_lfanew_wraparound() {
    let mut p = build_sane_pe();
    p[0x3C] = 0xF0;
    p[0x3D] = 0xFF;
    p[0x3E] = 0xFF;
    p[0x3F] = 0xFF;
    assert_eq!(pe_sanity(&p), Err(PeError::BadLfanew));
}

// --- Mirrors tests/host/helpers/test_efisp_scan.c -------------------

const EFISP_UTF16: [u8; 12] = [
    0x65, 0x00, 0x66, 0x00, 0x69, 0x00, 0x73, 0x00, 0x70, 0x00, 0x00, 0x00,
];

#[test]
fn parity_efisp_at_offset_100() {
    let mut poisoned = [0u8; 256];
    poisoned[100..112].copy_from_slice(&EFISP_UTF16);
    assert!(efisp_marker_present(&poisoned));
}

#[test]
fn parity_efisp_clean_buffer() {
    let clean = [0u8; 256];
    assert!(!efisp_marker_present(&clean));
}

// --- FFI parity --------------------------------------------------------

#[test]
fn ffi_status_discriminants_match_c_enum() {
    // GblPeStatus values must match enum gbl_pe_status in PeSanity.h.
    use pe_utils::ffi::GblPeStatus;
    assert_eq!(GblPeStatus::Ok as u32, 0);
    assert_eq!(GblPeStatus::TooSmall as u32, 1);
    assert_eq!(GblPeStatus::BadDos as u32, 2);
    assert_eq!(GblPeStatus::BadLfanew as u32, 3);
    assert_eq!(GblPeStatus::BadPeMagic as u32, 4);
    assert_eq!(GblPeStatus::BadMachine as u32, 5);
    assert_eq!(GblPeStatus::BadOptMagic as u32, 6);
    assert_eq!(GblPeStatus::BadSubsys as u32, 7);
    assert_eq!(GblPeStatus::EntryOutOfBounds as u32, 8);
}

#[test]
fn ffi_pe_sanity_ok_on_real_efi() {
    use pe_utils::ffi::{gbl_pe_sanity, GblPeStatus};
    let path = fixtures_dir().join("infiniti-EU-16.0.5.703.efi");
    let pe = fs::read(&path).expect("read PE fixture");
    let s = unsafe { gbl_pe_sanity(pe.as_ptr() as *const _, pe.len()) };
    assert_eq!(s, GblPeStatus::Ok);
}

#[test]
fn ffi_pe_sanity_null_buf() {
    use pe_utils::ffi::{gbl_pe_sanity, GblPeStatus};
    let s = unsafe { gbl_pe_sanity(core::ptr::null(), 0) };
    assert_eq!(s, GblPeStatus::TooSmall);
}

#[test]
fn ffi_efisp_marker_via_extern() {
    use pe_utils::ffi::gbl_contains_utf16_efisp;
    let mut poisoned = [0u8; 256];
    poisoned[100..112].copy_from_slice(&EFISP_UTF16);
    let r = unsafe { gbl_contains_utf16_efisp(poisoned.as_ptr() as *const _, poisoned.len()) };
    assert!(r);

    let clean = [0u8; 256];
    let r = unsafe { gbl_contains_utf16_efisp(clean.as_ptr() as *const _, clean.len()) };
    assert!(!r);

    let r = unsafe { gbl_contains_utf16_efisp(core::ptr::null(), 0) };
    assert!(!r);
}
