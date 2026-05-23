//! C-ABI shim — preserves the wire ABI of `PeSanity.h` / `efisp_scan.h`
//! so existing C call sites link unchanged against `libpe_utils.a`.
//!
//! The discriminants of [`GblPeStatus`] match `enum gbl_pe_status` in
//! `GblChainloadPkg/Library/GblPayloadLib/Internal/PeSanity.h`
//! one-for-one (in declaration order, starting at 0). Adding or
//! reordering variants is a wire-ABI break.

use core::ffi::c_void;

use crate::{efisp_marker_present, pe_sanity, PeError};

/// Mirror of `enum gbl_pe_status` from `PeSanity.h`.
///
/// `#[repr(C)]` plus explicit discriminants give us a byte-identical
/// ABI to the C enum so legacy callers (gbl-pack, fv-unwrap, the
/// `test_pe_sanity` host helper) can keep calling `gbl_pe_sanity` with
/// no source changes.
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum GblPeStatus {
    Ok = 0,
    TooSmall = 1,
    BadDos = 2,
    BadLfanew = 3,
    BadPeMagic = 4,
    BadMachine = 5,
    BadOptMagic = 6,
    BadSubsys = 7,
    EntryOutOfBounds = 8,
}

impl From<Result<(), PeError>> for GblPeStatus {
    fn from(r: Result<(), PeError>) -> Self {
        match r {
            Ok(()) => GblPeStatus::Ok,
            Err(PeError::TooSmall) => GblPeStatus::TooSmall,
            Err(PeError::BadDos) => GblPeStatus::BadDos,
            Err(PeError::BadLfanew) => GblPeStatus::BadLfanew,
            Err(PeError::BadPeMagic) => GblPeStatus::BadPeMagic,
            Err(PeError::BadMachine) => GblPeStatus::BadMachine,
            Err(PeError::BadOptMagic) => GblPeStatus::BadOptMagic,
            Err(PeError::BadSubsys) => GblPeStatus::BadSubsys,
            Err(PeError::EntryOutOfBounds) => GblPeStatus::EntryOutOfBounds,
        }
    }
}

/// C ABI: PE sanity check.
///
/// `buf` must point to `len` readable bytes (or be NULL with `len == 0`).
/// Returns one of the `GblPeStatus` discriminants — `Ok` (0) means the
/// header looks well-formed.
///
/// # Safety
/// Caller must ensure `buf` is either NULL or points to at least `len`
/// bytes of readable memory and that the memory is not mutated for the
/// duration of the call.
#[no_mangle]
pub unsafe extern "C" fn gbl_pe_sanity(buf: *const c_void, len: usize) -> GblPeStatus {
    if buf.is_null() {
        // Match C behavior for an empty/missing buffer — TooSmall is what
        // gbl_pe_sanity returns for size < 0x200 today.
        return GblPeStatus::TooSmall;
    }
    let slice = core::slice::from_raw_parts(buf as *const u8, len);
    pe_sanity(slice).into()
}

/// C ABI: detect the 12-byte UTF-16LE "efisp\0" marker.
///
/// Returns `true` (any non-zero `bool` representation) when the marker
/// is present.
///
/// # Safety
/// Caller must ensure `buf` is either NULL or points to at least `len`
/// bytes of readable memory and that the memory is not mutated for the
/// duration of the call.
#[no_mangle]
pub unsafe extern "C" fn gbl_contains_utf16_efisp(buf: *const c_void, len: usize) -> bool {
    if buf.is_null() {
        return false;
    }
    let slice = core::slice::from_raw_parts(buf as *const u8, len);
    efisp_marker_present(slice)
}
