//! AVB structure parser — Rust replacement for
//! `GblChainloadPkg/Library/AvbParseLib/AvbParse.c` and
//! `GblChainloadPkg/Library/AvbParseLib/Internal/AvbBigEndian.h`.
//!
//! Two surfaces:
//!
//! 1. Idiomatic Rust API (this module): zero-copy [`VbMeta`] / [`Footer`]
//!    views over an `&[u8]`, [`Descriptor`] enum with strongly-typed
//!    variants for `HASH`, `HASHTREE`, `KERNEL_CMDLINE`,
//!    `CHAIN_PARTITION`, and `PROPERTY`, and a [`VbMeta::descriptors`]
//!    iterator that fails closed on every malformed offset.
//!
//! 2. The [`ffi`] module — `extern "C"` shims preserving the wire ABI
//!    of the deleted `AvbParseLib.h` so the firmware
//!    (`FastbootCmds.c::GblVbmetaLookupDescriptor`, etc.) and host C
//!    tools (`vbmeta-graft`, `mode2-profile`, `tests/avb`) link
//!    `libavb_parse.a` and keep working unchanged.
//!
//! `no_std` when the `std` feature is off (firmware build with
//! `--no-default-features`). Hosted builds keep `std` so cargo's test
//! harness can unwind.
//!
//! ## AVB wire format refresher
//!
//! All multi-byte fields are big-endian on the wire. Layout follows
//! AOSP's `external/avb/libavb/avb_vbmeta_image.h` +
//! `avb_descriptor.h`. The byte-for-byte offsets in [`parse_vbmeta`],
//! [`parse_footer`], and the descriptor decoders mirror what the
//! deleted C code did — `tests/parity.rs` pins both implementations
//! against the same in-tree fixtures.

#![cfg_attr(not(feature = "std"), no_std)]

// In the firmware build libavb_parse.a, libgblp1.a,
// libmode2_profile_core.a, and libpatch_engine.a are all linked into
// the same EDK2 image. Each `#[panic_handler]` lowers to a strong
// `rust_begin_unwind` symbol; the EDK2 link line passes
// `-Wl,--allow-multiple-definition` so the identical `loop {}` panic
// handlers coexist.
#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

pub mod ffi;

// --- Constants -------------------------------------------------------
//
// Ported byte-for-byte from the deleted AvbParseLib.h + AvbParse.c.
// All multi-byte AVB fields are big-endian on the wire.

/// 4-byte magic at offset 0 of every AVB vbmeta blob.
pub const VBMETA_MAGIC: &[u8; 4] = b"AVB0";

/// 4-byte magic at offset 0 of every AVB footer.
pub const FOOTER_MAGIC: &[u8; 4] = b"AVBf";

/// On-disk size of the fixed `AvbVBMetaImageHeader` (libavb).
pub const VBMETA_HEADER_SIZE: usize = 256;

/// On-disk size of the trailing `AvbFooter` (libavb).
pub const FOOTER_SIZE: usize = 64;

/// Minimum on-disk size of an `AvbDescriptor` header (tag + num bytes).
const DESCRIPTOR_HEADER_SIZE: u64 = 16;

// Hash descriptor + chain-partition descriptor body offsets — mirror
// the byte-for-byte field positions used by the deleted
// `AvbParse_HashDescriptor` / `AvbParse_ChainPartitionDescriptor`.
const HASH_DESC_BODY_OFFSET: u64 = 132;
const HASH_DESC_IMAGE_SIZE_OFFSET: usize = 16;
const HASH_DESC_NAME_LEN_OFFSET: usize = 56;
const HASH_DESC_SALT_LEN_OFFSET: usize = 60;
const HASH_DESC_DIGEST_LEN_OFFSET: usize = 64;

const CHAIN_DESC_BODY_OFFSET: u64 = 92;
const CHAIN_DESC_NAME_LEN_OFFSET: usize = 20;
const CHAIN_DESC_PK_LEN_OFFSET: usize = 24;

// --- Error type ------------------------------------------------------

/// Every failure mode the parser can produce.
///
/// Variants map 1:1 onto the EFI status codes the C ABI returns; see
/// [`ffi::avb_status_for_error`].
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum AvbError {
    /// Caller passed an empty slice / NULL pointer / similar.
    InvalidParameter,
    /// AVB magic (`AVB0` or `AVBf`) absent at the expected offset.
    NotFound,
    /// Iterator hit clean end-of-aux-block. Not an error in the C ABI
    /// — surfaces as `EFI_END_OF_MEDIA` so callers can break their walk
    /// loop.
    EndOfMedia,
}

// --- Big-endian helpers ----------------------------------------------
//
// Idiomatic Rust replacements for the deleted AvbBigEndian.h macros.
// All readers are `const fn`-eligible and pure; LLVM lowers them to
// the same `rev` / `ldr` pair the C code produced.

#[inline]
fn read_u32_be(bytes: &[u8], off: usize) -> Option<u32> {
    let end = off.checked_add(4)?;
    if end > bytes.len() {
        return None;
    }
    let mut a = [0u8; 4];
    a.copy_from_slice(&bytes[off..end]);
    Some(u32::from_be_bytes(a))
}

#[inline]
fn read_u64_be(bytes: &[u8], off: usize) -> Option<u64> {
    let end = off.checked_add(8)?;
    if end > bytes.len() {
        return None;
    }
    let mut a = [0u8; 8];
    a.copy_from_slice(&bytes[off..end]);
    Some(u64::from_be_bytes(a))
}

// --- Footer ----------------------------------------------------------

/// Decoded `AvbFooter` — 64 bytes at the tail of a footer'd partition.
///
/// Layout (libavb `avb_footer.h`):
///
/// | offset | size | field |
/// |-------:|-----:|-------|
/// |   0    |  4   | magic "AVBf" |
/// |   4    |  4   | version_major (BE u32) |
/// |   8    |  4   | version_minor (BE u32) |
/// |  12    |  8   | original_image_size (BE u64) |
/// |  20    |  8   | vbmeta_offset (BE u64) |
/// |  28    |  8   | vbmeta_size (BE u64) |
/// |  36    | 28   | reserved |
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Footer {
    pub version_major: u32,
    pub version_minor: u32,
    pub original_image_size: u64,
    pub vbmeta_offset: u64,
    pub vbmeta_size: u64,
}

/// Decode the trailing 64-byte `AvbFooter` from a whole-partition buffer.
///
/// Mirrors `AvbParse_Footer` from the deleted C code: validates the
/// `AVBf` magic, then bounds-checks `vbmeta_offset + vbmeta_size`
/// against `bytes.len()` and `original_image_size` against
/// `bytes.len()`.
pub fn parse_footer(bytes: &[u8]) -> Result<Footer, AvbError> {
    if bytes.len() < FOOTER_SIZE {
        return Err(AvbError::InvalidParameter);
    }
    let footer = &bytes[bytes.len() - FOOTER_SIZE..];
    parse_footer_window(footer, bytes.len() as u64).and_then(|f| {
        if f.vbmeta_offset.checked_add(f.vbmeta_size).map_or(true, |t| t > bytes.len() as u64) {
            return Err(AvbError::InvalidParameter);
        }
        if f.original_image_size > bytes.len() as u64 {
            return Err(AvbError::InvalidParameter);
        }
        Ok(f)
    })
}

/// Decode the `AvbFooter` from a partition-tail window without holding
/// the whole partition in memory. Mirrors `AvbParse_FooterFromTail`.
///
/// `tail` must contain at least the final [`FOOTER_SIZE`] bytes of the
/// partition; the footer is read from `tail[tail.len()-64..]`. The
/// `vbmeta_offset` + `vbmeta_size` fields are bounds-checked against
/// `partition_size`, NOT `tail.len()` — the embedded vbmeta blob the
/// footer points at usually lies outside the tail window.
pub fn parse_footer_from_tail(
    tail: &[u8],
    partition_size: u64,
) -> Result<Footer, AvbError> {
    if tail.len() < FOOTER_SIZE {
        return Err(AvbError::InvalidParameter);
    }
    if partition_size < FOOTER_SIZE as u64 {
        return Err(AvbError::InvalidParameter);
    }
    let footer_buf = &tail[tail.len() - FOOTER_SIZE..];
    let f = parse_footer_window(footer_buf, partition_size)?;
    // Bounds-check against the real partition size (overflow-safe).
    if f.vbmeta_size == 0 {
        return Err(AvbError::InvalidParameter);
    }
    if f.vbmeta_offset >= partition_size {
        return Err(AvbError::InvalidParameter);
    }
    if f.vbmeta_size > partition_size - f.vbmeta_offset {
        return Err(AvbError::InvalidParameter);
    }
    Ok(f)
}

/// Shared 64-byte footer decoder. Validates the `AVBf` magic and pulls
/// the 5 fields out; caller does the bounds checking.
fn parse_footer_window(footer_buf: &[u8], _partition_size: u64) -> Result<Footer, AvbError> {
    if footer_buf.len() < FOOTER_SIZE {
        return Err(AvbError::InvalidParameter);
    }
    if &footer_buf[..4] != FOOTER_MAGIC {
        return Err(AvbError::NotFound);
    }
    Ok(Footer {
        version_major: read_u32_be(footer_buf, 4).ok_or(AvbError::InvalidParameter)?,
        version_minor: read_u32_be(footer_buf, 8).ok_or(AvbError::InvalidParameter)?,
        original_image_size: read_u64_be(footer_buf, 12).ok_or(AvbError::InvalidParameter)?,
        vbmeta_offset: read_u64_be(footer_buf, 20).ok_or(AvbError::InvalidParameter)?,
        vbmeta_size: read_u64_be(footer_buf, 28).ok_or(AvbError::InvalidParameter)?,
    })
}

// --- VbMeta header ---------------------------------------------------

/// Decoded `AvbVBMetaImageHeader` — the fixed 256-byte preamble of
/// every AVB vbmeta blob. Field layout matches AOSP's
/// `external/avb/libavb/avb_vbmeta_image.h`.
#[derive(Debug, Clone, Copy)]
pub struct VbMetaHeader {
    pub major_version: u32,
    pub minor_version: u32,
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

/// Zero-copy view over a parsed vbmeta blob: the decoded header + a
/// borrow of the original bytes (used to walk descriptors).
#[derive(Debug, Clone, Copy)]
pub struct VbMeta<'a> {
    pub header: VbMetaHeader,
    bytes: &'a [u8],
}

impl<'a> VbMeta<'a> {
    /// Borrow the underlying bytes the view was constructed over.
    pub fn bytes(&self) -> &'a [u8] {
        self.bytes
    }

    /// Offset of the auxiliary block inside `bytes`.
    pub fn aux_offset(&self) -> u64 {
        VBMETA_HEADER_SIZE as u64 + self.header.authentication_data_block_size
    }

    /// Auxiliary block length (header field `aux_data_block_size`).
    pub fn aux_size(&self) -> u64 {
        self.header.auxiliary_data_block_size
    }

    /// Borrow the auxiliary block bytes.
    pub fn aux(&self) -> &'a [u8] {
        let off = self.aux_offset() as usize;
        let end = off.saturating_add(self.aux_size() as usize).min(self.bytes.len());
        &self.bytes[off..end]
    }

    /// Borrow the embedded public-key bytes (subset of [`Self::aux`]).
    /// Returns `None` if the header's PK offset/size escape the aux
    /// block — matches what `AvbParse_ChainVerdict` treats as malformed.
    pub fn public_key(&self) -> Option<&'a [u8]> {
        let aux = self.aux();
        let off = self.header.public_key_offset as usize;
        let len = self.header.public_key_size as usize;
        let end = off.checked_add(len)?;
        if end > aux.len() {
            return None;
        }
        Some(&aux[off..end])
    }

    /// Walk descriptors in the aux block. Each iteration step yields
    /// either a strongly-typed [`Descriptor`] view or an [`AvbError`]
    /// pinpointing the offending descriptor.
    ///
    /// The walk respects `descriptors_offset` + `descriptors_size`
    /// from the header — matches the C reference in
    /// `vbmeta-graft.c::walk_descriptors`, where the cursor starts at
    /// `h.DescriptorsOffset` (NOT 0) and stops at
    /// `h.DescriptorsOffset + h.DescriptorsSize`. The aux block may
    /// contain other things (public key, key metadata) outside the
    /// descriptor window, so starting at offset 0 would mis-parse
    /// arbitrary bytes as descriptor headers.
    pub fn descriptors(&self) -> DescriptorIter<'a> {
        let aux = self.aux();
        let off = self.header.descriptors_offset as usize;
        let size = self.header.descriptors_size as usize;
        // Defensive: clamp the descriptor window to the aux block. The
        // header check in `parse_vbmeta` already kept aux inside
        // bytes, but `descriptors_offset` + `descriptors_size` aren't
        // validated upstream (libavb stores them as advisory offsets).
        let end = off.saturating_add(size).min(aux.len());
        let start = off.min(end);
        DescriptorIter {
            aux: &aux[start..end],
            cursor: 0,
        }
    }
}

/// Parse the 256-byte AVB vbmeta header at the start of `bytes` and
/// return a zero-copy view.
///
/// Mirrors `AvbParse_VbmetaHeader`. The overflow-safe sanity check
/// (header + auth + aux <= `bytes.len()`) is applied verbatim from the
/// C code — naive `header + auth + aux > size` is unsound on crafted
/// inputs (wraps `u64`).
pub fn parse_vbmeta(bytes: &[u8]) -> Result<VbMeta<'_>, AvbError> {
    if bytes.len() < VBMETA_HEADER_SIZE {
        return Err(AvbError::InvalidParameter);
    }
    if &bytes[..4] != VBMETA_MAGIC {
        return Err(AvbError::NotFound);
    }
    // Field positions from libavb avb_vbmeta_image.h. The deleted C
    // code's offsets are reproduced byte-for-byte.
    let major_version = read_u32_be(bytes, 4).ok_or(AvbError::InvalidParameter)?;
    let minor_version = read_u32_be(bytes, 8).ok_or(AvbError::InvalidParameter)?;
    let auth = read_u64_be(bytes, 12).ok_or(AvbError::InvalidParameter)?;
    let aux = read_u64_be(bytes, 20).ok_or(AvbError::InvalidParameter)?;
    let algorithm_type = read_u32_be(bytes, 28).ok_or(AvbError::InvalidParameter)?;
    let hash_offset = read_u64_be(bytes, 32).ok_or(AvbError::InvalidParameter)?;
    let hash_size = read_u64_be(bytes, 40).ok_or(AvbError::InvalidParameter)?;
    let signature_offset = read_u64_be(bytes, 48).ok_or(AvbError::InvalidParameter)?;
    let signature_size = read_u64_be(bytes, 56).ok_or(AvbError::InvalidParameter)?;
    let public_key_offset = read_u64_be(bytes, 64).ok_or(AvbError::InvalidParameter)?;
    let public_key_size = read_u64_be(bytes, 72).ok_or(AvbError::InvalidParameter)?;
    let public_key_metadata_offset = read_u64_be(bytes, 80).ok_or(AvbError::InvalidParameter)?;
    let public_key_metadata_size = read_u64_be(bytes, 88).ok_or(AvbError::InvalidParameter)?;
    let descriptors_offset = read_u64_be(bytes, 96).ok_or(AvbError::InvalidParameter)?;
    let descriptors_size = read_u64_be(bytes, 104).ok_or(AvbError::InvalidParameter)?;
    let rollback_index = read_u64_be(bytes, 112).ok_or(AvbError::InvalidParameter)?;
    let flags = read_u32_be(bytes, 120).ok_or(AvbError::InvalidParameter)?;
    let rollback_index_location = read_u32_be(bytes, 124).ok_or(AvbError::InvalidParameter)?;
    let mut release_string = [0u8; 48];
    release_string.copy_from_slice(&bytes[128..128 + 48]);

    // Overflow-safe size check — straight port of the C comment-block:
    // "a crafted vbmeta with huge auth/aux can wrap Total to a small
    //  value and pass the check. Validate each addend against the
    //  remaining budget via subtraction so no operand can overflow."
    if auth > (bytes.len() as u64) - (VBMETA_HEADER_SIZE as u64) {
        return Err(AvbError::InvalidParameter);
    }
    if aux > (bytes.len() as u64) - (VBMETA_HEADER_SIZE as u64) - auth {
        return Err(AvbError::InvalidParameter);
    }

    Ok(VbMeta {
        header: VbMetaHeader {
            major_version,
            minor_version,
            authentication_data_block_size: auth,
            auxiliary_data_block_size: aux,
            algorithm_type,
            hash_offset,
            hash_size,
            signature_offset,
            signature_size,
            public_key_offset,
            public_key_size,
            public_key_metadata_offset,
            public_key_metadata_size,
            descriptors_offset,
            descriptors_size,
            rollback_index,
            flags,
            rollback_index_location,
            release_string,
        },
        bytes,
    })
}

// --- Descriptor enum + iterator -------------------------------------

/// AVB descriptor tag values — match `GBL_AVB_DESCRIPTOR_TAG` from the
/// deleted `AvbParseLib.h` 1:1. The wire encoding is a BE `u64` whose
/// low byte is one of these values.
#[repr(u64)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum DescriptorTag {
    Property = 0,
    Hashtree = 1,
    Hash = 2,
    KernelCmdline = 3,
    ChainPartition = 4,
}

impl DescriptorTag {
    fn from_u64(v: u64) -> Option<Self> {
        match v {
            0 => Some(Self::Property),
            1 => Some(Self::Hashtree),
            2 => Some(Self::Hash),
            3 => Some(Self::KernelCmdline),
            4 => Some(Self::ChainPartition),
            _ => None,
        }
    }
}

/// Strongly-typed view of a single AVB descriptor.
///
/// The `Raw` variant covers tags the parser doesn't recognise (any tag
/// outside the libavb-known 0..=4 range or one of the known tags that
/// fails its body-shape check). Callers walking the iterator should
/// match on `Raw { .. }` to skip unknown descriptors without breaking
/// the iteration.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Descriptor<'a> {
    Hash(HashDescriptor<'a>),
    Hashtree {
        bytes: &'a [u8],
    },
    KernelCmdline {
        bytes: &'a [u8],
    },
    ChainPartition(ChainPartitionDescriptor<'a>),
    Property {
        bytes: &'a [u8],
    },
    /// Tag-not-in-libavb-set / body-shape-mismatch fallback.
    Raw {
        tag: u64,
        bytes: &'a [u8],
    },
}

impl<'a> Descriptor<'a> {
    /// The 16-byte-header tag as a `u64`. For known variants this is
    /// the matching [`DescriptorTag`]'s discriminant.
    pub fn tag(&self) -> u64 {
        match self {
            Descriptor::Hash(_) => DescriptorTag::Hash as u64,
            Descriptor::Hashtree { .. } => DescriptorTag::Hashtree as u64,
            Descriptor::KernelCmdline { .. } => DescriptorTag::KernelCmdline as u64,
            Descriptor::ChainPartition(_) => DescriptorTag::ChainPartition as u64,
            Descriptor::Property { .. } => DescriptorTag::Property as u64,
            Descriptor::Raw { tag, .. } => *tag,
        }
    }

    /// Borrow the raw on-disk bytes (tag header + body).
    pub fn raw_bytes(&self) -> &'a [u8] {
        match self {
            Descriptor::Hash(h) => h.raw,
            Descriptor::Hashtree { bytes } => bytes,
            Descriptor::KernelCmdline { bytes } => bytes,
            Descriptor::ChainPartition(c) => c.raw,
            Descriptor::Property { bytes } => bytes,
            Descriptor::Raw { bytes, .. } => bytes,
        }
    }
}

/// HASH descriptor body, as decoded by the deleted
/// `AvbParse_HashDescriptor`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HashDescriptor<'a> {
    raw: &'a [u8],
    pub image_size: u64,
    pub partition_name: &'a [u8],
    pub salt: &'a [u8],
    pub digest: &'a [u8],
}

/// CHAIN_PARTITION descriptor body, as decoded by the deleted
/// `AvbParse_ChainPartitionDescriptor`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ChainPartitionDescriptor<'a> {
    raw: &'a [u8],
    pub partition_name: &'a [u8],
    pub public_key: &'a [u8],
}

/// Streaming iterator over the descriptors in a vbmeta's aux block.
pub struct DescriptorIter<'a> {
    aux: &'a [u8],
    cursor: u64,
}

impl<'a> Iterator for DescriptorIter<'a> {
    type Item = Result<Descriptor<'a>, AvbError>;

    fn next(&mut self) -> Option<Self::Item> {
        // Mirrors `AvbParse_NextDescriptor`: out-of-bounds / partial
        // descriptor → `Some(Err(InvalidParameter))` + advance to
        // end-of-aux so the next call returns `None`.
        let aux_len = self.aux.len() as u64;
        if self.cursor == aux_len {
            return None;
        }
        if self.cursor > aux_len {
            self.cursor = aux_len;
            return Some(Err(AvbError::InvalidParameter));
        }
        if aux_len - self.cursor < DESCRIPTOR_HEADER_SIZE {
            self.cursor = aux_len;
            return Some(Err(AvbError::InvalidParameter));
        }
        let off = self.cursor as usize;
        let tag = match read_u64_be(self.aux, off) {
            Some(t) => t,
            None => {
                self.cursor = aux_len;
                return Some(Err(AvbError::InvalidParameter));
            }
        };
        let body_len = match read_u64_be(self.aux, off + 8) {
            Some(n) => n,
            None => {
                self.cursor = aux_len;
                return Some(Err(AvbError::InvalidParameter));
            }
        };
        let total = match DESCRIPTOR_HEADER_SIZE.checked_add(body_len) {
            Some(t) => t,
            None => {
                self.cursor = aux_len;
                return Some(Err(AvbError::InvalidParameter));
            }
        };
        if self.cursor.checked_add(total).map_or(true, |e| e > aux_len) {
            self.cursor = aux_len;
            return Some(Err(AvbError::InvalidParameter));
        }
        let end = (self.cursor + total) as usize;
        let raw = &self.aux[off..end];
        self.cursor += total;

        Some(Ok(decode_descriptor(tag, raw)))
    }
}

/// Decode a complete descriptor (header + body). Tags outside the
/// known libavb set or any body that fails its shape check fall back to
/// [`Descriptor::Raw`] / `Descriptor::*{bytes}` rather than failing the
/// whole walk — matches what the C code does in
/// `GblVbmetaLookupDescriptor` (skip + continue).
///
/// Visible to the [`ffi`] module via the public alias
/// [`decode_descriptor_for_ffi`] — `AvbParse_HashDescriptor` /
/// `AvbParse_ChainPartitionDescriptor` need to decode a single
/// caller-supplied descriptor without re-walking the aux block.
pub(crate) fn decode_descriptor_for_ffi<'a>(tag_u64: u64, raw: &'a [u8]) -> Descriptor<'a> {
    decode_descriptor(tag_u64, raw)
}

fn decode_descriptor<'a>(tag_u64: u64, raw: &'a [u8]) -> Descriptor<'a> {
    let tag = DescriptorTag::from_u64(tag_u64);
    let total = raw.len() as u64;
    match tag {
        Some(DescriptorTag::Hash) => {
            if total < HASH_DESC_BODY_OFFSET {
                return Descriptor::Raw {
                    tag: tag_u64,
                    bytes: raw,
                };
            }
            let image_size = match read_u64_be(raw, HASH_DESC_IMAGE_SIZE_OFFSET) {
                Some(v) => v,
                None => return Descriptor::Raw { tag: tag_u64, bytes: raw },
            };
            let name_len = match read_u32_be(raw, HASH_DESC_NAME_LEN_OFFSET) {
                Some(v) => v as u64,
                None => return Descriptor::Raw { tag: tag_u64, bytes: raw },
            };
            let salt_len = match read_u32_be(raw, HASH_DESC_SALT_LEN_OFFSET) {
                Some(v) => v as u64,
                None => return Descriptor::Raw { tag: tag_u64, bytes: raw },
            };
            let digest_len = match read_u32_be(raw, HASH_DESC_DIGEST_LEN_OFFSET) {
                Some(v) => v as u64,
                None => return Descriptor::Raw { tag: tag_u64, bytes: raw },
            };
            let body = HASH_DESC_BODY_OFFSET;
            let need = match body.checked_add(name_len)
                .and_then(|n| n.checked_add(salt_len))
                .and_then(|n| n.checked_add(digest_len))
            {
                Some(n) => n,
                None => return Descriptor::Raw { tag: tag_u64, bytes: raw },
            };
            if need > total {
                return Descriptor::Raw { tag: tag_u64, bytes: raw };
            }
            let body_usize = body as usize;
            let name_end = body_usize + name_len as usize;
            let salt_end = name_end + salt_len as usize;
            let digest_end = salt_end + digest_len as usize;
            Descriptor::Hash(HashDescriptor {
                raw,
                image_size,
                partition_name: &raw[body_usize..name_end],
                salt: &raw[name_end..salt_end],
                digest: &raw[salt_end..digest_end],
            })
        }
        Some(DescriptorTag::ChainPartition) => {
            if total < CHAIN_DESC_BODY_OFFSET {
                return Descriptor::Raw { tag: tag_u64, bytes: raw };
            }
            let name_len = match read_u32_be(raw, CHAIN_DESC_NAME_LEN_OFFSET) {
                Some(v) => v as u64,
                None => return Descriptor::Raw { tag: tag_u64, bytes: raw },
            };
            let pk_len = match read_u32_be(raw, CHAIN_DESC_PK_LEN_OFFSET) {
                Some(v) => v as u64,
                None => return Descriptor::Raw { tag: tag_u64, bytes: raw },
            };
            let body = CHAIN_DESC_BODY_OFFSET;
            let need = match body.checked_add(name_len).and_then(|n| n.checked_add(pk_len)) {
                Some(n) => n,
                None => return Descriptor::Raw { tag: tag_u64, bytes: raw },
            };
            if need > total {
                return Descriptor::Raw { tag: tag_u64, bytes: raw };
            }
            let body_usize = body as usize;
            let name_end = body_usize + name_len as usize;
            let pk_end = name_end + pk_len as usize;
            Descriptor::ChainPartition(ChainPartitionDescriptor {
                raw,
                partition_name: &raw[body_usize..name_end],
                public_key: &raw[name_end..pk_end],
            })
        }
        Some(DescriptorTag::Hashtree) => Descriptor::Hashtree { bytes: raw },
        Some(DescriptorTag::KernelCmdline) => Descriptor::KernelCmdline { bytes: raw },
        Some(DescriptorTag::Property) => Descriptor::Property { bytes: raw },
        None => Descriptor::Raw { tag: tag_u64, bytes: raw },
    }
}

// --- Chain verdict ---------------------------------------------------

/// `AvbParse_ChainVerdict` outcome — mirrors `GBL_AVB_CHAIN_VERDICT`
/// from the deleted C header 1:1.
#[repr(u32)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum ChainVerdict {
    Ok = 0,
    KeyMismatch = 1,
    NoVbmeta = 2,
}

/// Classify a chained partition's embedded vbmeta against the chain
/// descriptor's public key — key-identity check, NOT a sig verify.
/// Mirrors `AvbParse_ChainVerdict`. Returns `NoVbmeta` for any
/// unparseable / malformed input (the conservative "init would see
/// ok_not_signed" bucket).
pub fn chain_verdict(vbmeta: &[u8], chain_pk: Option<&[u8]>) -> ChainVerdict {
    let view = match parse_vbmeta(vbmeta) {
        Ok(v) => v,
        Err(_) => return ChainVerdict::NoVbmeta,
    };
    let pk = match view.public_key() {
        Some(p) => p,
        None => return ChainVerdict::NoVbmeta,
    };
    // No chain key to compare → any parseable vbmeta is a hit.
    let chain_pk = match chain_pk {
        Some(c) if !c.is_empty() => c,
        _ => return ChainVerdict::Ok,
    };
    if pk == chain_pk {
        ChainVerdict::Ok
    } else {
        ChainVerdict::KeyMismatch
    }
}

// --- Tests -----------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn put_u32_be(buf: &mut [u8], off: usize, v: u32) {
        buf[off..off + 4].copy_from_slice(&v.to_be_bytes());
    }

    fn put_u64_be(buf: &mut [u8], off: usize, v: u64) {
        buf[off..off + 8].copy_from_slice(&v.to_be_bytes());
    }

    fn synth_header(auth: u64, aux: u64) -> Vec<u8> {
        let mut h = vec![0u8; VBMETA_HEADER_SIZE];
        h[..4].copy_from_slice(VBMETA_MAGIC);
        put_u32_be(&mut h, 4, 1);
        put_u32_be(&mut h, 8, 1);
        put_u64_be(&mut h, 12, auth);
        put_u64_be(&mut h, 20, aux);
        h
    }

    #[test]
    fn vbmeta_too_small() {
        assert_eq!(parse_vbmeta(&[]).unwrap_err(), AvbError::InvalidParameter);
        let tiny = vec![0u8; 16];
        assert_eq!(parse_vbmeta(&tiny).unwrap_err(), AvbError::InvalidParameter);
    }

    #[test]
    fn vbmeta_bad_magic() {
        let mut h = synth_header(0, 0);
        h[0] = b'X';
        assert_eq!(parse_vbmeta(&h).unwrap_err(), AvbError::NotFound);
    }

    #[test]
    fn vbmeta_overflow_attack_rejected() {
        // huge auth → header + auth wraps u64 if naively summed
        let mut h = synth_header(u64::MAX - 10, 0);
        h.resize(VBMETA_HEADER_SIZE, 0);
        assert_eq!(parse_vbmeta(&h).unwrap_err(), AvbError::InvalidParameter);
    }

    #[test]
    fn vbmeta_ok_zero_blocks() {
        let h = synth_header(0, 0);
        let v = parse_vbmeta(&h).expect("ok");
        assert_eq!(v.header.major_version, 1);
        assert_eq!(v.header.minor_version, 1);
        assert_eq!(v.header.authentication_data_block_size, 0);
        assert_eq!(v.header.auxiliary_data_block_size, 0);
        assert_eq!(v.aux().len(), 0);
        assert!(v.descriptors().next().is_none());
    }

    #[test]
    fn footer_parse_ok() {
        let mut p = vec![0xaa_u8; 1024];
        let off = 1024 - FOOTER_SIZE;
        p[off..off + 4].copy_from_slice(FOOTER_MAGIC);
        put_u32_be(&mut p, off + 4, 1);
        put_u32_be(&mut p, off + 8, 0);
        put_u64_be(&mut p, off + 12, 512); // original_image_size
        put_u64_be(&mut p, off + 20, 300); // vbmeta_offset
        put_u64_be(&mut p, off + 28, 200); // vbmeta_size
        let f = parse_footer(&p).expect("ok");
        assert_eq!(f.version_major, 1);
        assert_eq!(f.original_image_size, 512);
        assert_eq!(f.vbmeta_offset, 300);
        assert_eq!(f.vbmeta_size, 200);
    }

    #[test]
    fn footer_no_magic() {
        let p = vec![0xaa_u8; 1024];
        assert_eq!(parse_footer(&p), Err(AvbError::NotFound));
    }

    #[test]
    fn footer_too_small() {
        let p = vec![0xaa_u8; 32];
        assert_eq!(parse_footer(&p), Err(AvbError::InvalidParameter));
    }

    #[test]
    fn footer_from_tail_no_magic() {
        let tail = vec![0u8; 512];
        assert_eq!(
            parse_footer_from_tail(&tail, 1024 * 1024),
            Err(AvbError::NotFound)
        );
    }

    #[test]
    fn footer_from_tail_ok() {
        // Last 64B of a 512B tail buffer = footer; partition is 1 MiB.
        let mut tail = vec![0u8; 512];
        let off = 512 - FOOTER_SIZE;
        tail[off..off + 4].copy_from_slice(FOOTER_MAGIC);
        put_u32_be(&mut tail, off + 4, 1);
        put_u64_be(&mut tail, off + 12, 4000);
        put_u64_be(&mut tail, off + 20, 8192); // vbmeta_offset
        put_u64_be(&mut tail, off + 28, 1024); // vbmeta_size
        let f = parse_footer_from_tail(&tail, 1024 * 1024).expect("ok");
        assert_eq!(f.vbmeta_offset, 8192);
        assert_eq!(f.vbmeta_size, 1024);
    }

    #[test]
    fn descriptor_walk_synthetic() {
        // Build a 1-hash-1-chain aux block.
        //
        // Hash descriptor on-disk = 132 bytes (16 tag header + 116 body).
        // Chain descriptor on-disk = 92 bytes (16 tag header + 76 body).
        // Aux block total = 132 + 92 = 224.
        let auth = 0u64;
        let aux_size = 132 + 92u64;
        let mut blob = synth_header(auth, aux_size);
        put_u64_be(&mut blob, 96, 0);            // descriptors_offset = 0
        put_u64_be(&mut blob, 104, aux_size);    // descriptors_size

        blob.resize((VBMETA_HEADER_SIZE as u64 + auth + aux_size) as usize, 0);

        // First descriptor: HASH (tag=2, num_bytes_following=132-16=116)
        let aux_off = VBMETA_HEADER_SIZE;
        put_u64_be(&mut blob, aux_off, 2);
        put_u64_be(&mut blob, aux_off + 8, 132 - 16);
        // Image size + zero name/salt/digest lens → empty body lengths.

        // Second descriptor: CHAIN (tag=4, num_bytes_following=92-16=76)
        let aux_off2 = aux_off + 132;
        put_u64_be(&mut blob, aux_off2, 4);
        put_u64_be(&mut blob, aux_off2 + 8, 92 - 16);

        let v = parse_vbmeta(&blob).unwrap();
        let descs: Vec<_> = v.descriptors().collect();
        assert_eq!(descs.len(), 2);
        match &descs[0] {
            Ok(Descriptor::Hash(h)) => {
                assert_eq!(h.partition_name.len(), 0);
                assert_eq!(h.digest.len(), 0);
            }
            other => panic!("expected Hash, got {:?}", other),
        }
        match &descs[1] {
            Ok(Descriptor::ChainPartition(c)) => {
                assert_eq!(c.partition_name.len(), 0);
                assert_eq!(c.public_key.len(), 0);
            }
            other => panic!("expected ChainPartition, got {:?}", other),
        }
    }

    #[test]
    fn descriptor_truncated_header_returns_error() {
        // descriptors window 10B — less than the 16B descriptor header.
        let auth = 0u64;
        let aux_size = 10u64;
        let mut blob = synth_header(auth, aux_size);
        put_u64_be(&mut blob, 96, 0);            // descriptors_offset
        put_u64_be(&mut blob, 104, aux_size);    // descriptors_size = 10
        blob.resize((VBMETA_HEADER_SIZE as u64 + aux_size) as usize, 0);
        let v = parse_vbmeta(&blob).unwrap();
        let mut it = v.descriptors();
        match it.next() {
            Some(Err(AvbError::InvalidParameter)) => {}
            other => panic!("expected InvalidParameter, got {:?}", other),
        }
        // Iterator should now be exhausted.
        assert!(it.next().is_none());
    }

    #[test]
    fn chain_verdict_no_vbmeta_on_garbage() {
        let v = chain_verdict(&[0u8; 16], None);
        assert_eq!(v, ChainVerdict::NoVbmeta);
    }

    #[test]
    fn chain_verdict_ok_no_key() {
        // Build a vbmeta with a non-empty public key area in aux.
        let pk = b"FAKEPK01234567";
        let auth = 0u64;
        let aux_size = (pk.len() as u64).max(8);
        let mut blob = synth_header(auth, aux_size);
        put_u64_be(&mut blob, 64, 0);           // public_key_offset = 0
        put_u64_be(&mut blob, 72, pk.len() as u64); // public_key_size
        blob.resize((VBMETA_HEADER_SIZE as u64 + aux_size) as usize, 0);
        blob[VBMETA_HEADER_SIZE..VBMETA_HEADER_SIZE + pk.len()].copy_from_slice(pk);
        assert_eq!(chain_verdict(&blob, None), ChainVerdict::Ok);
        assert_eq!(chain_verdict(&blob, Some(pk)), ChainVerdict::Ok);
        assert_eq!(
            chain_verdict(&blob, Some(b"DIFFERENTKEY00")),
            ChainVerdict::KeyMismatch
        );
    }

    #[test]
    fn descriptor_tag_discriminants() {
        // Wire ABI commitment — these MUST match the deleted
        // GBL_AVB_DESCRIPTOR_TAG enum.
        assert_eq!(DescriptorTag::Property as u64, 0);
        assert_eq!(DescriptorTag::Hashtree as u64, 1);
        assert_eq!(DescriptorTag::Hash as u64, 2);
        assert_eq!(DescriptorTag::KernelCmdline as u64, 3);
        assert_eq!(DescriptorTag::ChainPartition as u64, 4);
    }

    #[test]
    fn chain_verdict_discriminants() {
        assert_eq!(ChainVerdict::Ok as u32, 0);
        assert_eq!(ChainVerdict::KeyMismatch as u32, 1);
        assert_eq!(ChainVerdict::NoVbmeta as u32, 2);
    }
}
