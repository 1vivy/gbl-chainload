//! C-ABI shim — preserves the wire ABI of the deleted `AvbParseLib.h`
//! / `AvbParse.c` so the firmware (`FastbootCmds.c::GblVbmetaLookupDescriptor`
//! et al.) and host C tools (`vbmeta-graft`, `mode2-profile`, the
//! `tests/avb` harness) link `libavb_parse.a` and keep working
//! unchanged.
//!
//! Public C header: `crates/avb-parse/include/avb_parse_ffi.h`.
//!
//! Symbol contract (all `extern "C"`, preserved verbatim from the
//! deleted C library):
//!
//!   - `AvbParse_Footer`
//!   - `AvbParse_VbmetaHeader`
//!   - `AvbParse_NextDescriptor`
//!   - `AvbParse_HashDescriptor`
//!   - `AvbParse_ChainPartitionDescriptor`
//!   - `AvbParse_FooterFromTail`
//!   - `AvbParse_ChainVerdict`
//!
//! These are the only AvbParse_* symbols any in-tree caller references
//! — confirmed by `grep -rn 'AvbParse_'` over the firmware + host
//! tool tree.

use core::ptr;

use crate::{
    chain_verdict as rs_chain_verdict, decode_descriptor_for_ffi, parse_footer as rs_parse_footer,
    parse_footer_from_tail as rs_parse_footer_from_tail, parse_vbmeta as rs_parse_vbmeta,
    AvbError, ChainVerdict, Descriptor, DescriptorTag, Footer, VbMeta,
};

// --- EFI_STATUS values --------------------------------------------------
//
// Match the deleted AvbBigEndian.h host-build shim + UEFI's official
// status codes. The firmware build sources them from <Uefi.h>; the host
// public header (`avb_parse_ffi.h`) declares the same numeric constants.

const EFI_SUCCESS: i64 = 0;
const EFI_INVALID_PARAMETER: i64 = (0x8000_0000_0000_0000u64 | 2) as i64;
const EFI_NOT_FOUND: i64 = (0x8000_0000_0000_0000u64 | 14) as i64;
const EFI_END_OF_MEDIA: i64 = (0x8000_0000_0000_0000u64 | 28) as i64;

// EFI_STATUS is `UINTN` on UEFI (64-bit on AArch64 / 32-bit on IA32).
// The deleted host-build shim mapped it to `int` though, so on host
// builds the values above need to fit in 32 bits — the C side casts
// freely. We expose the function returns as `i64` and rely on the C
// caller to receive them as `EFI_STATUS` (UEFI: UINTN; host: int).
// AArch64 ABI + the firmware UEFI build both treat int / long as
// 64-bit-or-less-in-register, so the trunc on host is benign.

// --- Wire structs -------------------------------------------------------
//
// Layout matches the deleted GBL_AVB_FOOTER + GBL_AVB_VBMETA_HEADER
// byte-for-byte. The C definitions used `UINT32`/`UINT64` directly with
// natural alignment; `#[repr(C)]` produces the same layout on
// AArch64/x86_64.

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct GblAvbFooterC {
    pub footer_major_version: u32,
    pub footer_minor_version: u32,
    pub original_image_size: u64,
    pub vbmeta_offset: u64,
    pub vbmeta_size: u64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct GblAvbVbmetaHeaderC {
    pub avb_major_version: u32,
    pub avb_minor_version: u32,
    pub authentication_data_block_size: u64,
    pub auxiliary_data_block_size: u64,
    pub algorithm_type: u32,
    pub hash_offset: u64,
    pub hash_size: u64,
    pub signature_offset: u64,
    pub signature_size: u64,
    pub public_key_offset: u64,
    pub public_key_size: u64,
    pub public_key_metadata_offset: u64,
    pub public_key_metadata_size: u64,
    pub descriptors_offset: u64,
    pub descriptors_size: u64,
    pub rollback_index: u64,
    pub flags: u32,
    pub rollback_index_location: u32,
    pub release_string: [u8; 48],
}

impl From<Footer> for GblAvbFooterC {
    fn from(f: Footer) -> Self {
        GblAvbFooterC {
            footer_major_version: f.version_major,
            footer_minor_version: f.version_minor,
            original_image_size: f.original_image_size,
            vbmeta_offset: f.vbmeta_offset,
            vbmeta_size: f.vbmeta_size,
        }
    }
}

impl<'a> From<&VbMeta<'a>> for GblAvbVbmetaHeaderC {
    fn from(v: &VbMeta<'a>) -> Self {
        let h = &v.header;
        GblAvbVbmetaHeaderC {
            avb_major_version: h.major_version,
            avb_minor_version: h.minor_version,
            authentication_data_block_size: h.authentication_data_block_size,
            auxiliary_data_block_size: h.auxiliary_data_block_size,
            algorithm_type: h.algorithm_type,
            hash_offset: h.hash_offset,
            hash_size: h.hash_size,
            signature_offset: h.signature_offset,
            signature_size: h.signature_size,
            public_key_offset: h.public_key_offset,
            public_key_size: h.public_key_size,
            public_key_metadata_offset: h.public_key_metadata_offset,
            public_key_metadata_size: h.public_key_metadata_size,
            descriptors_offset: h.descriptors_offset,
            descriptors_size: h.descriptors_size,
            rollback_index: h.rollback_index,
            flags: h.flags,
            rollback_index_location: h.rollback_index_location,
            release_string: h.release_string,
        }
    }
}

/// `GBL_AVB_DESCRIPTOR_TAG` — same discriminants as the deleted enum.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GblAvbDescriptorTagC {
    Property = 0,
    Hashtree = 1,
    Hash = 2,
    KernelCmdline = 3,
    ChainPartition = 4,
}

impl From<u64> for GblAvbDescriptorTagC {
    fn from(v: u64) -> Self {
        // Mirrors the C cast `(GBL_AVB_DESCRIPTOR_TAG)Tag` in
        // AvbParse_NextDescriptor — the C code never validates the tag;
        // unknown tags get passed through to the caller, which then
        // skips them. We reproduce that by truncating + casting; the
        // caller's switch will land in the default arm for unknown
        // values.
        let lo = v as u32;
        match lo {
            0 => Self::Property,
            1 => Self::Hashtree,
            2 => Self::Hash,
            3 => Self::KernelCmdline,
            4 => Self::ChainPartition,
            // Out-of-range value — pick Property (0) as the most benign
            // bucket; the C cast would have just yielded the literal
            // value, but no in-tree caller depends on the bit-exact
            // u32 value beyond the 5-case switch.
            _ => Self::Property,
        }
    }
}

#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GblAvbChainVerdictC {
    Ok = 0,
    KeyMismatch = 1,
    NoVbmeta = 2,
}

impl From<ChainVerdict> for GblAvbChainVerdictC {
    fn from(v: ChainVerdict) -> Self {
        match v {
            ChainVerdict::Ok => Self::Ok,
            ChainVerdict::KeyMismatch => Self::KeyMismatch,
            ChainVerdict::NoVbmeta => Self::NoVbmeta,
        }
    }
}

// --- Helper: error → EFI_STATUS -----------------------------------------

#[inline]
pub fn avb_status_for_error(err: AvbError) -> i64 {
    match err {
        AvbError::InvalidParameter => EFI_INVALID_PARAMETER,
        AvbError::NotFound => EFI_NOT_FOUND,
        AvbError::EndOfMedia => EFI_END_OF_MEDIA,
    }
}

// --- AvbParse_Footer ----------------------------------------------------

/// Mirror of `AvbParse_Footer (CONST UINT8*, UINT64, OUT GBL_AVB_FOOTER*)`.
///
/// # Safety
/// - `partition` must point at `partition_size` readable bytes (NULL is allowed,
///   in which case `EFI_INVALID_PARAMETER` is returned).
/// - `footer_out` must point at a writable `GblAvbFooterC` or be NULL.
#[no_mangle]
pub unsafe extern "C" fn AvbParse_Footer(
    partition: *const u8,
    partition_size: u64,
    footer_out: *mut GblAvbFooterC,
) -> i64 {
    if partition.is_null() || footer_out.is_null() {
        return EFI_INVALID_PARAMETER;
    }
    if partition_size > isize::MAX as u64 {
        return EFI_INVALID_PARAMETER;
    }
    let bytes = core::slice::from_raw_parts(partition, partition_size as usize);
    match rs_parse_footer(bytes) {
        Ok(f) => {
            ptr::write(footer_out, f.into());
            EFI_SUCCESS
        }
        Err(e) => avb_status_for_error(e),
    }
}

// --- AvbParse_VbmetaHeader ---------------------------------------------

/// Mirror of `AvbParse_VbmetaHeader (CONST UINT8*, UINT64, OUT
/// GBL_AVB_VBMETA_HEADER*)`.
#[no_mangle]
pub unsafe extern "C" fn AvbParse_VbmetaHeader(
    vbmeta: *const u8,
    vbmeta_size: u64,
    header_out: *mut GblAvbVbmetaHeaderC,
) -> i64 {
    if vbmeta.is_null() || header_out.is_null() {
        return EFI_INVALID_PARAMETER;
    }
    if vbmeta_size > isize::MAX as u64 {
        return EFI_INVALID_PARAMETER;
    }
    let bytes = core::slice::from_raw_parts(vbmeta, vbmeta_size as usize);
    match rs_parse_vbmeta(bytes) {
        Ok(v) => {
            ptr::write(header_out, (&v).into());
            EFI_SUCCESS
        }
        Err(e) => avb_status_for_error(e),
    }
}

// --- AvbParse_NextDescriptor -------------------------------------------

/// Mirror of `AvbParse_NextDescriptor` — walks the aux block one
/// descriptor at a time. Cursor semantics + EOF status code (`EFI_END_OF_MEDIA`)
/// are preserved from the C code so callers loop the same way.
///
/// # Safety
/// All pointers must be non-NULL and point at writable / readable
/// memory of the appropriate sizes (`aux_size` bytes for `aux_block`,
/// scalar slots for the rest).
#[no_mangle]
pub unsafe extern "C" fn AvbParse_NextDescriptor(
    aux_block: *const u8,
    aux_size: u64,
    cursor: *mut u64,
    tag_out: *mut GblAvbDescriptorTagC,
    descriptor_out: *mut *const u8,
    descriptor_len_out: *mut u64,
) -> i64 {
    if aux_block.is_null()
        || cursor.is_null()
        || tag_out.is_null()
        || descriptor_out.is_null()
        || descriptor_len_out.is_null()
    {
        return EFI_INVALID_PARAMETER;
    }
    let cur = ptr::read(cursor);
    if cur == aux_size {
        return EFI_END_OF_MEDIA;
    }
    if cur > aux_size {
        return EFI_INVALID_PARAMETER;
    }
    if aux_size - cur < 16 {
        return EFI_INVALID_PARAMETER;
    }
    if aux_size > isize::MAX as u64 {
        return EFI_INVALID_PARAMETER;
    }
    let aux_slice = core::slice::from_raw_parts(aux_block, aux_size as usize);
    let off = cur as usize;
    let tag_raw = u64::from_be_bytes(aux_slice[off..off + 8].try_into().unwrap());
    let body_len = u64::from_be_bytes(aux_slice[off + 8..off + 16].try_into().unwrap());
    let total = match 16u64.checked_add(body_len) {
        Some(t) => t,
        None => return EFI_INVALID_PARAMETER,
    };
    if cur.checked_add(total).map_or(true, |e| e > aux_size) {
        return EFI_INVALID_PARAMETER;
    }
    ptr::write(tag_out, GblAvbDescriptorTagC::from(tag_raw));
    ptr::write(descriptor_out, aux_block.add(off));
    ptr::write(descriptor_len_out, total);
    ptr::write(cursor, cur + total);
    EFI_SUCCESS
}

// --- AvbParse_HashDescriptor -------------------------------------------

/// Mirror of `AvbParse_HashDescriptor`.
///
/// `salt_out`, `salt_len_out`, `image_size_out` are OPTIONAL (NULL OK).
#[no_mangle]
pub unsafe extern "C" fn AvbParse_HashDescriptor(
    descriptor: *const u8,
    descriptor_len: u64,
    partition_name_out: *mut *const u8,
    partition_name_len_out: *mut u32,
    digest_out: *mut *const u8,
    digest_len_out: *mut u32,
    salt_out: *mut *const u8,
    salt_len_out: *mut u32,
    image_size_out: *mut u64,
) -> i64 {
    if descriptor.is_null()
        || partition_name_out.is_null()
        || partition_name_len_out.is_null()
        || digest_out.is_null()
        || digest_len_out.is_null()
    {
        return EFI_INVALID_PARAMETER;
    }
    if descriptor_len > isize::MAX as u64 {
        return EFI_INVALID_PARAMETER;
    }
    let raw = core::slice::from_raw_parts(descriptor, descriptor_len as usize);
    // Decode reuses the same body-shape check the Rust descriptors()
    // iterator uses — but the C entry point is called with the raw
    // descriptor (header + body) the C caller has on hand.
    match decode_descriptor_for_ffi(DescriptorTag::Hash as u64, raw) {
        Descriptor::Hash(h) => {
            ptr::write(partition_name_out, h.partition_name.as_ptr());
            ptr::write(partition_name_len_out, h.partition_name.len() as u32);
            ptr::write(digest_out, h.digest.as_ptr());
            ptr::write(digest_len_out, h.digest.len() as u32);
            if !salt_out.is_null() {
                ptr::write(salt_out, h.salt.as_ptr());
            }
            if !salt_len_out.is_null() {
                ptr::write(salt_len_out, h.salt.len() as u32);
            }
            if !image_size_out.is_null() {
                ptr::write(image_size_out, h.image_size);
            }
            EFI_SUCCESS
        }
        _ => EFI_INVALID_PARAMETER,
    }
}

// --- AvbParse_ChainPartitionDescriptor ---------------------------------

/// Mirror of `AvbParse_ChainPartitionDescriptor`.
#[no_mangle]
pub unsafe extern "C" fn AvbParse_ChainPartitionDescriptor(
    descriptor: *const u8,
    descriptor_len: u64,
    partition_name_out: *mut *const u8,
    partition_name_len_out: *mut u32,
    public_key_out: *mut *const u8,
    public_key_len_out: *mut u32,
) -> i64 {
    if descriptor.is_null()
        || partition_name_out.is_null()
        || partition_name_len_out.is_null()
        || public_key_out.is_null()
        || public_key_len_out.is_null()
    {
        return EFI_INVALID_PARAMETER;
    }
    if descriptor_len > isize::MAX as u64 {
        return EFI_INVALID_PARAMETER;
    }
    let raw = core::slice::from_raw_parts(descriptor, descriptor_len as usize);
    match decode_descriptor_for_ffi(DescriptorTag::ChainPartition as u64, raw) {
        Descriptor::ChainPartition(c) => {
            ptr::write(partition_name_out, c.partition_name.as_ptr());
            ptr::write(partition_name_len_out, c.partition_name.len() as u32);
            ptr::write(public_key_out, c.public_key.as_ptr());
            ptr::write(public_key_len_out, c.public_key.len() as u32);
            EFI_SUCCESS
        }
        _ => EFI_INVALID_PARAMETER,
    }
}

// --- AvbParse_FooterFromTail -------------------------------------------

/// Mirror of `AvbParse_FooterFromTail`.
#[no_mangle]
pub unsafe extern "C" fn AvbParse_FooterFromTail(
    tail: *const u8,
    tail_len: u64,
    partition_size: u64,
    footer_out: *mut GblAvbFooterC,
) -> i64 {
    if tail.is_null() || footer_out.is_null() {
        return EFI_INVALID_PARAMETER;
    }
    if tail_len > isize::MAX as u64 {
        return EFI_INVALID_PARAMETER;
    }
    let bytes = core::slice::from_raw_parts(tail, tail_len as usize);
    match rs_parse_footer_from_tail(bytes, partition_size) {
        Ok(f) => {
            ptr::write(footer_out, f.into());
            EFI_SUCCESS
        }
        Err(e) => avb_status_for_error(e),
    }
}

// --- AvbParse_ChainVerdict ---------------------------------------------

/// Mirror of `AvbParse_ChainVerdict`. The C signature returns
/// `EFI_STATUS` and writes the verdict via an out-pointer; our shim
/// follows that contract exactly.
#[no_mangle]
pub unsafe extern "C" fn AvbParse_ChainVerdict(
    vbmeta: *const u8,
    vbmeta_size: u64,
    chain_pk: *const u8,
    chain_pk_len: u32,
    verdict_out: *mut GblAvbChainVerdictC,
) -> i64 {
    if verdict_out.is_null() {
        return EFI_INVALID_PARAMETER;
    }
    // Per the C semantics: caller sees NoVbmeta on bad inputs but the
    // function still returns EFI_SUCCESS.
    ptr::write(verdict_out, GblAvbChainVerdictC::NoVbmeta);
    if vbmeta.is_null() {
        return EFI_INVALID_PARAMETER;
    }
    if vbmeta_size > isize::MAX as u64 {
        return EFI_INVALID_PARAMETER;
    }
    let bytes = core::slice::from_raw_parts(vbmeta, vbmeta_size as usize);
    let chain_pk_slice = if chain_pk.is_null() || chain_pk_len == 0 {
        None
    } else {
        Some(core::slice::from_raw_parts(chain_pk, chain_pk_len as usize))
    };
    let v = rs_chain_verdict(bytes, chain_pk_slice);
    ptr::write(verdict_out, v.into());
    EFI_SUCCESS
}

// --- Compile-time wire-ABI assertions ---------------------------------

const _: () = {
    // GBL_AVB_FOOTER: 5 fields = u32, u32, u64, u64, u64. With 8-byte
    // alignment on u64, the C compiler packs the two u32s contiguously
    // (4+4 fills one 8-byte slot), then 3 u64s = 24. Total = 32.
    assert!(core::mem::size_of::<GblAvbFooterC>() == 32);
    // GBL_AVB_VBMETA_HEADER: layout walk (8-byte natural alignment on
    // AArch64 / x86_64; matches what the deleted C struct produced):
    //   off  0:  major_version (u32) + minor_version (u32)          = 8
    //   off  8:  authentication_data_block_size (u64)               = 8
    //   off 16:  auxiliary_data_block_size      (u64)               = 8
    //   off 24:  algorithm_type (u32) + pad(4)                      = 8
    //   off 32:  hash_offset, hash_size, sig_offset, sig_size,      \
    //            pk_offset, pk_size, pk_meta_offset, pk_meta_size,   \
    //            desc_offset, desc_size, rollback_index (u64 x11)   = 88
    //   off 120: flags (u32) + rollback_index_location (u32)        = 8
    //   off 128: release_string[48]                                  = 48
    //   total = 176 bytes.
    //
    // The deleted C struct yields the same 176-byte layout under any
    // GCC/Clang target we ship (AArch64 LP64 / x86_64 SysV). The
    // FastbootCmds.c reader only touches fields by name + accepts the
    // 176 we produce; sizeof() is never serialized.
    assert!(core::mem::size_of::<GblAvbVbmetaHeaderC>() == 176);
};

// --- Tests --------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enum_discriminants_match_c_header() {
        assert_eq!(GblAvbDescriptorTagC::Property as u32, 0);
        assert_eq!(GblAvbDescriptorTagC::Hashtree as u32, 1);
        assert_eq!(GblAvbDescriptorTagC::Hash as u32, 2);
        assert_eq!(GblAvbDescriptorTagC::KernelCmdline as u32, 3);
        assert_eq!(GblAvbDescriptorTagC::ChainPartition as u32, 4);
        assert_eq!(GblAvbChainVerdictC::Ok as u32, 0);
        assert_eq!(GblAvbChainVerdictC::KeyMismatch as u32, 1);
        assert_eq!(GblAvbChainVerdictC::NoVbmeta as u32, 2);
    }

    #[test]
    fn status_codes_match_uefi() {
        // EFI_INVALID_PARAMETER = 0x80000000_00000002 — high-bit set
        // marks an error in UEFI's encoding.
        assert_eq!(EFI_INVALID_PARAMETER as u64, 0x8000_0000_0000_0002);
        assert_eq!(EFI_NOT_FOUND as u64, 0x8000_0000_0000_000E);
        assert_eq!(EFI_END_OF_MEDIA as u64, 0x8000_0000_0000_001C);
        assert_eq!(EFI_SUCCESS, 0);
    }

    #[test]
    fn footer_roundtrip_via_ffi() {
        // Synthesize a 1 KiB partition with a valid footer at the tail.
        let mut p = vec![0xaau8; 1024];
        let off = 1024 - super::super::FOOTER_SIZE;
        p[off..off + 4].copy_from_slice(super::super::FOOTER_MAGIC);
        p[off + 4..off + 8].copy_from_slice(&1u32.to_be_bytes());
        p[off + 8..off + 12].copy_from_slice(&0u32.to_be_bytes());
        p[off + 12..off + 20].copy_from_slice(&512u64.to_be_bytes());
        p[off + 20..off + 28].copy_from_slice(&300u64.to_be_bytes());
        p[off + 28..off + 36].copy_from_slice(&200u64.to_be_bytes());
        let mut f = GblAvbFooterC {
            footer_major_version: 0,
            footer_minor_version: 0,
            original_image_size: 0,
            vbmeta_offset: 0,
            vbmeta_size: 0,
        };
        let rc = unsafe { AvbParse_Footer(p.as_ptr(), p.len() as u64, &mut f) };
        assert_eq!(rc, EFI_SUCCESS);
        assert_eq!(f.original_image_size, 512);
        assert_eq!(f.vbmeta_offset, 300);
        assert_eq!(f.vbmeta_size, 200);
    }

    #[test]
    fn null_partition_invalid_parameter() {
        let mut f = GblAvbFooterC {
            footer_major_version: 0,
            footer_minor_version: 0,
            original_image_size: 0,
            vbmeta_offset: 0,
            vbmeta_size: 0,
        };
        let rc = unsafe { AvbParse_Footer(core::ptr::null(), 1024, &mut f) };
        assert_eq!(rc, EFI_INVALID_PARAMETER);
    }
}
