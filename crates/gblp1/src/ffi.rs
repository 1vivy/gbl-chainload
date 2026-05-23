//! C-ABI shim — preserves the wire ABI of the deleted
//! `Internal/PayloadParse.h`, `Internal/Sha256.h`, and
//! `Internal/Crc32.h` so existing C call sites link unchanged against
//! `libgblp1.a`.
//!
//! The new public C header is `crates/gblp1/include/gblp1_ffi.h` —
//! callers include that single header and link the staticlib.
//!
//! `enum GblPayloadStatus` discriminants must match
//! `enum gbl_payload_status` from the old PayloadParse.h one-for-one
//! (declaration order, starting at 0). Adding or reordering variants
//! is a wire-ABI break.

use core::ffi::c_int;
use core::mem::MaybeUninit;
use core::ptr;

use crate::{parse, sha256, validate_header, ManifestError, ParseError};

// --- Status enum -----------------------------------------------------

/// Mirror of `enum gbl_payload_status` from the old
/// `Internal/PayloadParse.h`. Discriminants frozen — re-numbering any
/// variant is a wire-ABI break (host C callers switch on these).
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum GblPayloadStatus {
    Ok = 0,
    TooSmall = 1,
    BadMagic = 2,
    BadVersion = 3,
    BadHeaderSize = 4,
    BadFlags = 5,
    BadTotalSize = 6,
    BadEntryCount = 7,
    HeaderCrcMismatch = 8,
    FooterMismatch = 9,
    EntryBadType = 10,
    EntryBadFlags = 11,
    EntryBadReserved = 12,
    EntryBadOffset = 13,
    EntryBadSize = 14,
    EntryShaMismatch = 15,
    NoCachedAbl = 16,
    NoMode2Profile = 17,
    /// Reserved; never returned. Manifest absence signaled via
    /// `Ok + *out_present == 0`. Variant kept for source-compat with
    /// the legacy enum.
    NoManifest = 18,
    BadManifestMagic = 19,
    BadManifestSchema = 20,
    BadManifestReserved = 21,
    BadManifestSize = 22,
}

impl From<ParseError> for GblPayloadStatus {
    fn from(e: ParseError) -> Self {
        match e {
            ParseError::TooSmall => GblPayloadStatus::TooSmall,
            ParseError::BadMagic => GblPayloadStatus::BadMagic,
            ParseError::BadVersion => GblPayloadStatus::BadVersion,
            ParseError::BadHeaderSize => GblPayloadStatus::BadHeaderSize,
            ParseError::BadFlags => GblPayloadStatus::BadFlags,
            ParseError::BadTotalSize => GblPayloadStatus::BadTotalSize,
            ParseError::BadEntryCount => GblPayloadStatus::BadEntryCount,
            ParseError::HeaderCrcMismatch => GblPayloadStatus::HeaderCrcMismatch,
            ParseError::FooterMismatch => GblPayloadStatus::FooterMismatch,
            ParseError::EntryBadType => GblPayloadStatus::EntryBadType,
            ParseError::EntryBadFlags => GblPayloadStatus::EntryBadFlags,
            ParseError::EntryBadReserved => GblPayloadStatus::EntryBadReserved,
            ParseError::EntryBadOffset => GblPayloadStatus::EntryBadOffset,
            ParseError::EntryBadSize => GblPayloadStatus::EntryBadSize,
            ParseError::EntryShaMismatch => GblPayloadStatus::EntryShaMismatch,
            ParseError::NoCachedAbl => GblPayloadStatus::NoCachedAbl,
            ParseError::NoMode2Profile => GblPayloadStatus::NoMode2Profile,
        }
    }
}

impl From<ManifestError> for GblPayloadStatus {
    fn from(e: ManifestError) -> Self {
        match e {
            ManifestError::BadSize => GblPayloadStatus::BadManifestSize,
            ManifestError::BadMagic => GblPayloadStatus::BadManifestMagic,
            ManifestError::BadSchema => GblPayloadStatus::BadManifestSchema,
            ManifestError::BadReserved => GblPayloadStatus::BadManifestReserved,
        }
    }
}

/// Wire-form of `struct gbl_manifest`.
#[repr(C)]
pub struct GblManifestWire {
    pub cap_bits: u16,
}

// --- Payload-parser FFI ----------------------------------------------

unsafe fn slice_or_empty<'a>(bytes: *const u8, size: usize) -> &'a [u8] {
    if bytes.is_null() {
        // Match C behavior: NULL + size 0 → empty; NULL + size > 0 is
        // undefined, but mapping to empty lets us return TooSmall
        // cleanly instead of UB.
        &[]
    } else {
        core::slice::from_raw_parts(bytes, size)
    }
}

/// C ABI: validate the GBLP1 header + footer only (no entry walk).
///
/// # Safety
/// `bytes` must be NULL or point to at least `size` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn gbl_payload_validate_header(
    bytes: *const u8,
    size: usize,
) -> GblPayloadStatus {
    let buf = slice_or_empty(bytes, size);
    match validate_header(buf) {
        Ok(()) => GblPayloadStatus::Ok,
        Err(e) => e.into(),
    }
}

/// C ABI: parse + find the unique cached_abl entry.
///
/// On `Ok`, writes the borrowed pointer/size into `out_pe`/`out_pe_size`
/// (must be non-NULL); the pointer aliases `bytes`. Returns
/// `NoCachedAbl` if the container is valid but lacks a cached_abl entry.
///
/// # Safety
/// `bytes` must be NULL or point to `size` readable bytes.
/// `out_pe` and `out_pe_size` must be non-NULL.
#[no_mangle]
pub unsafe extern "C" fn gbl_payload_find_cached_abl(
    bytes: *const u8,
    size: usize,
    out_pe: *mut *const u8,
    out_pe_size: *mut usize,
) -> GblPayloadStatus {
    let buf = slice_or_empty(bytes, size);
    let container = match parse(buf) {
        Ok(c) => c,
        Err(e) => return e.into(),
    };
    match container.find_cached_abl() {
        Some(pe) => {
            if !out_pe.is_null() {
                *out_pe = pe.as_ptr();
            }
            if !out_pe_size.is_null() {
                *out_pe_size = pe.len();
            }
            GblPayloadStatus::Ok
        }
        None => GblPayloadStatus::NoCachedAbl,
    }
}

/// C ABI: parse + find the unique mode2_profile entry.
///
/// # Safety
/// As [`gbl_payload_find_cached_abl`].
#[no_mangle]
pub unsafe extern "C" fn gbl_payload_find_mode2_profile(
    bytes: *const u8,
    size: usize,
    out_profile: *mut *const u8,
    out_size: *mut usize,
) -> GblPayloadStatus {
    let buf = slice_or_empty(bytes, size);
    let container = match parse(buf) {
        Ok(c) => c,
        Err(e) => return e.into(),
    };
    match container.find_mode2_profile() {
        Some(p) => {
            if !out_profile.is_null() {
                *out_profile = p.as_ptr();
            }
            if !out_size.is_null() {
                *out_size = p.len();
            }
            GblPayloadStatus::Ok
        }
        None => GblPayloadStatus::NoMode2Profile,
    }
}

/// C ABI: scan for a GBLP1 container, tolerating stray copies of the
/// magic, then locate the cached_abl entry.
///
/// Returns the result of the first occurrence that fully validates.
/// If the magic is seen at one or more offsets but none validates,
/// returns the last non-`Ok` status from `gbl_payload_find_cached_abl`.
/// If the magic is never found, returns `BadMagic`.
///
/// # Safety
/// As [`gbl_payload_find_cached_abl`].
#[no_mangle]
pub unsafe extern "C" fn gbl_payload_scan_cached_abl(
    bytes: *const u8,
    size: usize,
    out_pe: *mut *const u8,
    out_pe_size: *mut usize,
) -> GblPayloadStatus {
    let buf = slice_or_empty(bytes, size);
    let mut last = GblPayloadStatus::BadMagic;
    let magic = &crate::GBLP1_MAGIC[..];
    let mut i = 0;
    while i + crate::GBLP1_MAGIC_SIZE <= buf.len() {
        if buf[i] == b'G' && buf[i..i + crate::GBLP1_MAGIC_SIZE] == *magic {
            let st = gbl_payload_find_cached_abl(
                buf.as_ptr().add(i),
                buf.len() - i,
                out_pe,
                out_pe_size,
            );
            if matches!(st, GblPayloadStatus::Ok) {
                return GblPayloadStatus::Ok;
            }
            last = st;
        }
        i += 1;
    }
    last
}

/// C ABI: locate + validate the unique manifest entry.
///
/// - `Ok` + `*out_present == 1`: manifest filled in.
/// - `Ok` + `*out_present == 0`: container valid, no manifest entry —
///   caller treats as all-zero capabilities.
/// - other return: parse or manifest-validation failure; `*out` is
///   undefined.
///
/// # Safety
/// `bytes` must be NULL or point to `size` readable bytes. `out` and
/// `out_present` must be non-NULL.
#[no_mangle]
pub unsafe extern "C" fn gbl_payload_find_manifest(
    bytes: *const u8,
    size: usize,
    out: *mut GblManifestWire,
    out_present: *mut c_int,
) -> GblPayloadStatus {
    let buf = slice_or_empty(bytes, size);
    let container = match parse(buf) {
        Ok(c) => c,
        Err(e) => return e.into(),
    };
    match container.find_manifest() {
        Ok(Some(m)) => {
            if !out.is_null() {
                (*out).cap_bits = m.cap_bits;
            }
            if !out_present.is_null() {
                *out_present = 1;
            }
            GblPayloadStatus::Ok
        }
        Ok(None) => {
            if !out_present.is_null() {
                *out_present = 0;
            }
            GblPayloadStatus::Ok
        }
        Err(e) => e.into(),
    }
}

// --- SHA-256 FFI ------------------------------------------------------
//
// The legacy `gbl_sha256_ctx` was a transparent B-Con state struct
// (data[64], datalen, bitlen, state[8]) totaling 112 bytes. The Rust
// crate uses `sha2::Sha256`, whose internal layout is not stable across
// versions, so we expose the C-side context as an opaque blob big
// enough to hold any reasonable streaming context with safe headroom
// (`GBL_SHA256_CTX_SIZE` bytes, 8-byte aligned). The first 8 bytes are
// a tag that the FFI uses to detect uninitialized contexts. See
// `crates/gblp1/include/gblp1_ffi.h`.

/// Size of the opaque `gbl_sha256_ctx` blob. Must match the C header.
pub const GBL_SHA256_CTX_SIZE: usize = 256;

const TAG_INIT: u64 = 0x6762_6c70_3173_6831; // "gblp1sh1"

#[repr(C, align(8))]
pub struct GblSha256Ctx {
    /// Tag (8 bytes), then a `MaybeUninit<sha2::Sha256>` packed into
    /// the remainder. We hand-place fields with offsets to avoid
    /// depending on sha2's exposed size at link time.
    pub bytes: [u8; GBL_SHA256_CTX_SIZE],
}

#[inline]
fn ctx_tag(c: &GblSha256Ctx) -> u64 {
    u64::from_le_bytes([
        c.bytes[0], c.bytes[1], c.bytes[2], c.bytes[3], c.bytes[4], c.bytes[5], c.bytes[6],
        c.bytes[7],
    ])
}

#[inline]
fn set_ctx_tag(c: &mut GblSha256Ctx, tag: u64) {
    let t = tag.to_le_bytes();
    c.bytes[0..8].copy_from_slice(&t);
}

#[inline]
unsafe fn inner_sha_ptr(c: *mut GblSha256Ctx) -> *mut sha2::Sha256 {
    // Skip the 8-byte tag; the cargo align(8) on GblSha256Ctx + the
    // 8-byte tag together preserve 8-byte alignment for the inner
    // sha2::Sha256 (which has align_of == 8 in sha2 0.10.x).
    (c as *mut u8).add(8) as *mut sha2::Sha256
}

const _: () = {
    // Compile-time guard: the blob must be large enough for the tag
    // plus a sha2::Sha256, with room to spare for future versions.
    if GBL_SHA256_CTX_SIZE < 8 + core::mem::size_of::<sha2::Sha256>() {
        panic!("GBL_SHA256_CTX_SIZE too small for sha2::Sha256");
    }
};

/// C ABI: single-shot SHA-256.
///
/// # Safety
/// `buf` must be NULL or point to `len` readable bytes. `out` must
/// point to 32 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gbl_sha256(buf: *const u8, len: usize, out: *mut u8) {
    if out.is_null() {
        return;
    }
    let slice = slice_or_empty(buf, len);
    let d = sha256(slice);
    ptr::copy_nonoverlapping(d.as_ptr(), out, 32);
}

/// C ABI: initialize a streaming SHA-256 context.
///
/// # Safety
/// `ctx` must point to a writable [`GblSha256Ctx`] (i.e. at least
/// `GBL_SHA256_CTX_SIZE` bytes, 8-byte aligned).
#[no_mangle]
pub unsafe extern "C" fn gbl_sha256_init(ctx: *mut GblSha256Ctx) {
    if ctx.is_null() {
        return;
    }
    use sha2::Digest;
    // Zero the blob (defensive; the C struct was uninitialized stack
    // data and the B-Con impl init'd from scratch).
    ptr::write_bytes(ctx as *mut u8, 0, GBL_SHA256_CTX_SIZE);
    let sha_ptr = inner_sha_ptr(ctx);
    let fresh: MaybeUninit<sha2::Sha256> = MaybeUninit::new(sha2::Sha256::new());
    ptr::write(sha_ptr, fresh.assume_init());
    set_ctx_tag(&mut *ctx, TAG_INIT);
}

/// C ABI: feed bytes to a streaming SHA-256 context.
///
/// # Safety
/// `ctx` must point to a context previously initialized by
/// [`gbl_sha256_init`]. `data` must be NULL or point to `len` readable
/// bytes.
#[no_mangle]
pub unsafe extern "C" fn gbl_sha256_update(
    ctx: *mut GblSha256Ctx,
    data: *const u8,
    len: usize,
) {
    if ctx.is_null() || ctx_tag(&*ctx) != TAG_INIT {
        return;
    }
    use sha2::Digest;
    let slice = slice_or_empty(data, len);
    let sha = &mut *inner_sha_ptr(ctx);
    sha.update(slice);
}

/// C ABI: finalize a streaming SHA-256 context, writing 32 bytes to
/// `out`. The context is consumed (re-init before re-use).
///
/// # Safety
/// `ctx` must point to a context previously initialized by
/// [`gbl_sha256_init`]. `out` must point to 32 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gbl_sha256_final(ctx: *mut GblSha256Ctx, out: *mut u8) {
    if ctx.is_null() || out.is_null() || ctx_tag(&*ctx) != TAG_INIT {
        return;
    }
    use sha2::Digest;
    let sha = ptr::read(inner_sha_ptr(ctx));
    let digest = sha.finalize();
    ptr::copy_nonoverlapping(digest.as_ptr(), out, 32);
    // Invalidate the tag so accidental re-use is a no-op rather than
    // a use-after-finalize on the moved sha2 context.
    set_ctx_tag(&mut *ctx, 0);
}

// --- CRC-32 FFI -------------------------------------------------------

/// C ABI: IEEE-802.3 CRC-32 (zlib-compatible).
///
/// # Safety
/// `buf` must be NULL or point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn gbl_crc32(buf: *const u8, len: usize) -> u32 {
    let slice = slice_or_empty(buf, len);
    crate::crc32(slice)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_discriminants_match_c_header() {
        // These numeric values are the wire ABI — they must match
        // `enum gbl_payload_status` in
        // `crates/gblp1/include/gblp1_ffi.h` (which itself replaces
        // the deleted `Internal/PayloadParse.h`).
        assert_eq!(GblPayloadStatus::Ok as i32, 0);
        assert_eq!(GblPayloadStatus::TooSmall as i32, 1);
        assert_eq!(GblPayloadStatus::BadMagic as i32, 2);
        assert_eq!(GblPayloadStatus::BadVersion as i32, 3);
        assert_eq!(GblPayloadStatus::BadHeaderSize as i32, 4);
        assert_eq!(GblPayloadStatus::BadFlags as i32, 5);
        assert_eq!(GblPayloadStatus::BadTotalSize as i32, 6);
        assert_eq!(GblPayloadStatus::BadEntryCount as i32, 7);
        assert_eq!(GblPayloadStatus::HeaderCrcMismatch as i32, 8);
        assert_eq!(GblPayloadStatus::FooterMismatch as i32, 9);
        assert_eq!(GblPayloadStatus::EntryBadType as i32, 10);
        assert_eq!(GblPayloadStatus::EntryBadFlags as i32, 11);
        assert_eq!(GblPayloadStatus::EntryBadReserved as i32, 12);
        assert_eq!(GblPayloadStatus::EntryBadOffset as i32, 13);
        assert_eq!(GblPayloadStatus::EntryBadSize as i32, 14);
        assert_eq!(GblPayloadStatus::EntryShaMismatch as i32, 15);
        assert_eq!(GblPayloadStatus::NoCachedAbl as i32, 16);
        assert_eq!(GblPayloadStatus::NoMode2Profile as i32, 17);
        assert_eq!(GblPayloadStatus::NoManifest as i32, 18);
        assert_eq!(GblPayloadStatus::BadManifestMagic as i32, 19);
        assert_eq!(GblPayloadStatus::BadManifestSchema as i32, 20);
        assert_eq!(GblPayloadStatus::BadManifestReserved as i32, 21);
        assert_eq!(GblPayloadStatus::BadManifestSize as i32, 22);
    }
}
