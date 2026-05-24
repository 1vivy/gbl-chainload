//! C-ABI shim for libmode2_profile_core.a — preserves the wire ABI of
//! the deleted `Internal/Mode2Profile.h` so the firmware and host C
//! callers link unchanged.
//!
//! Public C header: `crates/mode2-profile-core/include/mode2_profile_ffi.h`.
//!
//! The `enum gbl_m2p_status` discriminants are frozen at the values
//! declared by the legacy header and asserted by the
//! `status_discriminants_match_c_header` unit test below. Adding or
//! reordering variants is a wire-ABI break.

use core::ptr;

use crate::{parse, ParseError, GBL_M2P_SIZE};

#[cfg(feature = "std")]
use crate::{compile, derive, CompileError, DeriveError};

/// Mirror of `enum gbl_m2p_status` from the old
/// `Internal/Mode2Profile.h`. Numbered explicitly to make the wire-ABI
/// commitment visible at every call site.
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Mode2Status {
    Ok = 0,
    TooSmall = 1,
    BadMagic = 2,
    BadVersion = 3,
    BadReserved = 4,
    /// `is_unlocked > 1` or `color > 3`. Also: NULL pointer arguments
    /// (matches the C parser's `b == NULL || out == NULL` early-out).
    BadField = 5,
}

impl From<ParseError> for Mode2Status {
    fn from(e: ParseError) -> Self {
        match e {
            ParseError::TooSmall => Mode2Status::TooSmall,
            ParseError::BadMagic => Mode2Status::BadMagic,
            ParseError::BadVersion => Mode2Status::BadVersion,
            ParseError::BadReserved => Mode2Status::BadReserved,
            ParseError::BadField => Mode2Status::BadField,
        }
    }
}

/// Wire-form of `struct gbl_mode2_profile` (packed, little-endian).
///
/// Layout is identical to the C `struct gbl_mode2_profile` declared in
/// `tools/shared/gbl_mode2_profile.h`. Use this type from `extern "C"`
/// callers; the idiomatic Rust [`Profile`](crate::Profile) struct has
/// the same `repr(C, packed)` layout so the two are interchangeable.
#[repr(C, packed)]
pub struct GblMode2ProfileWire {
    pub magic: [u8; 4],
    pub version: u16,
    pub reserved: u16,
    pub is_unlocked: u32,
    pub color: u32,
    pub system_version: u32,
    pub system_spl: u32,
    pub rot_digest: [u8; 32],
    pub pubkey_digest: [u8; 32],
    pub vbh: [u8; 32],
}

const _: () = {
    if core::mem::size_of::<GblMode2ProfileWire>() != GBL_M2P_SIZE {
        panic!("GblMode2ProfileWire must be 120 bytes packed");
    }
};

unsafe fn slice_or_empty<'a>(bytes: *const u8, size: usize) -> &'a [u8] {
    if bytes.is_null() {
        &[]
    } else {
        core::slice::from_raw_parts(bytes, size)
    }
}

/// C ABI: parse + validate a 120-byte mode2 profile.
///
/// On `Ok`, writes the parsed profile into `*out`. The wire layout of
/// `*out` is identical to the C `struct gbl_mode2_profile` byte-for-byte.
///
/// # Safety
/// `bytes` must be NULL or point to at least `size` readable bytes.
/// `out` must point to a writable `gbl_mode2_profile` (i.e. at least
/// 120 bytes, packed). Both pointers are NULL-checked at the boundary:
/// NULL `out` returns `BadField` to match the legacy C parser.
#[no_mangle]
pub unsafe extern "C" fn gbl_mode2_profile_parse(
    bytes: *const u8,
    size: usize,
    out: *mut GblMode2ProfileWire,
) -> Mode2Status {
    if out.is_null() {
        return Mode2Status::BadField;
    }
    let buf = slice_or_empty(bytes, size);
    match parse(buf) {
        Ok(p) => {
            // Profile and GblMode2ProfileWire have identical
            // repr(C, packed) layouts; serialize through the wire form
            // to avoid any aliasing-violation hazard with packed
            // fields. The 120-byte buffer is overwritten as a unit.
            let bytes_out = p.to_bytes();
            ptr::copy_nonoverlapping(
                bytes_out.as_ptr(),
                out as *mut u8,
                GBL_M2P_SIZE,
            );
            Mode2Status::Ok
        }
        Err(e) => e.into(),
    }
}

// --- Host-only: compile + derive --------------------------------------
//
// These entry points need the `std` + `alloc` features (toml crate,
// String, Vec). They are gated out of firmware staticlibs by the same
// feature flag that gates the function bodies in lib.rs.

/// C ABI: TOML profile -> 120-byte wire binary.
///
/// `toml_str` is a NUL-terminated UTF-8 string. On `Ok`, writes exactly
/// 120 bytes to `out_bin` and `120` to `*out_size`.
///
/// On error, returns one of the negative status codes below and leaves
/// `*out_size` untouched. The mapping is stable so host callers can
/// switch on it; the Rust `CompileError` enum is internal.
///
/// Wire status (in addition to the parse statuses above; >= 100 is a
/// compile-side error):
///   - 100: malformed TOML
///   - 101: missing key or wrong type
///   - 102: integer key out of legal range
///   - 103: digest field not 64 lowercase-hex
///   - 104: unknown top-level key
///
/// # Safety
/// `toml_str` must be NULL or point to a NUL-terminated readable
/// string. `out_bin` must point to at least 120 writable bytes.
/// `out_size` may be NULL.
#[cfg(feature = "std")]
#[no_mangle]
pub unsafe extern "C" fn gbl_mode2_profile_compile(
    toml_str: *const core::ffi::c_char,
    out_bin: *mut u8,
    out_size: *mut usize,
) -> core::ffi::c_int {
    if toml_str.is_null() || out_bin.is_null() {
        return 101; // missing/null input
    }
    let cstr = core::ffi::CStr::from_ptr(toml_str);
    let s = match cstr.to_str() {
        Ok(s) => s,
        Err(_) => return 100,
    };
    match compile(s) {
        Ok(bin) => {
            debug_assert_eq!(bin.len(), GBL_M2P_SIZE);
            ptr::copy_nonoverlapping(bin.as_ptr(), out_bin, GBL_M2P_SIZE);
            if !out_size.is_null() {
                *out_size = GBL_M2P_SIZE;
            }
            0
        }
        Err(e) => match e {
            CompileError::MalformedToml(_) => 100,
            CompileError::MissingOrBadType(_) => 101,
            CompileError::OutOfRange { .. } => 102,
            CompileError::BadDigest(_) => 103,
            CompileError::UnknownKey(_) => 104,
        },
    }
}

/// C ABI: stock vbmeta -> wire `gbl_mode2_profile`.
///
/// On `Ok`, writes a 120-byte profile to `*out` whose fields encode the
/// pubkey-derived digests + os_version + security_patch from the input.
/// `is_unlocked` is set to 0 and `color` to 0 (GREEN) — matches the
/// Python / C tool default. Callers that want a different boot-state
/// trio edit the resulting TOML, not the binary.
///
/// Wire status (>= 200 is a derive-side error):
///   - 200: vbmeta too small (< 256 bytes)
///   - 201: bad AVB magic
///   - 202: malformed header (auth/aux overflows file)
///   - 203: vbmeta has no public key (unsigned)
///   - 204: public key extends past aux block
///   - 205: descriptors region extends past aux block
///   - 206: no os_version property descriptor
///   - 207: no security_patch property descriptor
///   - 208: os_version out of range
///   - 209: spl out of range
///   - 210: spl malformed (not YYYY-MM-DD)
///
/// # Safety
/// `vbmeta` must be NULL or point to `vbmeta_size` readable bytes.
/// `out` must point to a writable `gbl_mode2_profile` (120 bytes).
#[cfg(feature = "std")]
#[no_mangle]
pub unsafe extern "C" fn gbl_mode2_profile_derive(
    vbmeta: *const u8,
    vbmeta_size: usize,
    out: *mut GblMode2ProfileWire,
) -> core::ffi::c_int {
    if out.is_null() {
        return 200;
    }
    let buf = slice_or_empty(vbmeta, vbmeta_size);
    match derive(buf) {
        Ok(p) => {
            let bytes_out = p.to_bytes();
            ptr::copy_nonoverlapping(bytes_out.as_ptr(), out as *mut u8, GBL_M2P_SIZE);
            0
        }
        Err(e) => match e {
            DeriveError::TooSmall => 200,
            DeriveError::BadMagic => 201,
            DeriveError::MalformedHeader => 202,
            DeriveError::NoPublicKey => 203,
            DeriveError::PublicKeyPastAux => 204,
            DeriveError::DescriptorsPastAux => 205,
            DeriveError::NoOsVersionProperty => 206,
            DeriveError::NoSecurityPatchProperty => 207,
            DeriveError::OsVersionOutOfRange => 208,
            DeriveError::SplOutOfRange => 209,
            DeriveError::SplMalformed => 210,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_discriminants_match_c_header() {
        // These numeric values are the wire ABI — they must match
        // `enum gbl_m2p_status` in
        // `crates/mode2-profile-core/include/mode2_profile_ffi.h`
        // (which itself replaces the deleted Internal/Mode2Profile.h).
        assert_eq!(Mode2Status::Ok as i32, 0);
        assert_eq!(Mode2Status::TooSmall as i32, 1);
        assert_eq!(Mode2Status::BadMagic as i32, 2);
        assert_eq!(Mode2Status::BadVersion as i32, 3);
        assert_eq!(Mode2Status::BadReserved as i32, 4);
        assert_eq!(Mode2Status::BadField as i32, 5);
    }

    #[test]
    fn parse_via_ffi_round_trip() {
        // Synthesize a known-good 120-byte payload, then parse via the
        // FFI shim and verify the field copy lands correctly.
        let mut bin = [0u8; GBL_M2P_SIZE];
        bin[0..4].copy_from_slice(b"GM2P");
        bin[4..6].copy_from_slice(&1u16.to_le_bytes());
        // is_unlocked = 0, color = 0
        bin[16..20].copy_from_slice(&0xDEAD_BEEFu32.to_le_bytes()); // sysver
        bin[20..24].copy_from_slice(&0x9A4u32.to_le_bytes()); // spl
        for i in 0..32 {
            bin[24 + i] = 0xA0 + i as u8;
        }

        let mut out: GblMode2ProfileWire = unsafe { core::mem::zeroed() };
        let st = unsafe { gbl_mode2_profile_parse(bin.as_ptr(), bin.len(), &mut out) };
        assert_eq!(st, Mode2Status::Ok);
        // Read packed field through a copy to avoid an unaligned reference.
        let sysver = unsafe { core::ptr::addr_of!(out.system_version).read_unaligned() };
        assert_eq!(sysver, 0xDEAD_BEEF);
    }

    #[test]
    fn parse_via_ffi_null_out() {
        let bin = [0u8; GBL_M2P_SIZE];
        let st = unsafe { gbl_mode2_profile_parse(bin.as_ptr(), bin.len(), ptr::null_mut()) };
        assert_eq!(st, Mode2Status::BadField);
    }
}
