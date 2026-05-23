//! GBLP1 v1 container parser + packer.
//!
//! Replaces three sources at once:
//!
//! - `GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c` — the
//!   pure-logic header+entry validator the EDK2 shim and host tools
//!   share, plus PR1's manifest support (`gbl_payload_find_manifest`).
//! - `GblChainloadPkg/Library/GblPayloadLib/Sha256.c` and
//!   `Internal/Sha256.h` — the vendored B-Con SHA-256. Now backed by
//!   the `sha2` crate, but the C-ABI streaming context is preserved as
//!   an opaque blob so existing callers (vbmeta-graft) keep working.
//! - `GblChainloadPkg/Library/GblPayloadLib/Crc32.c` +
//!   `Internal/Crc32.h` — the vendored IEEE-802.3 CRC-32. Now backed
//!   by `crc32fast`.
//!
//! Two layers:
//!
//! 1. Idiomatic Rust API at the crate root: [`parse`], [`Container`],
//!    [`pack`], plus the wire constants.
//! 2. The [`ffi`] module: `extern "C"` shims preserving the C wire ABI
//!    so the EDK2 firmware and every host C tool that previously linked
//!    the deleted C sources can link `libgblp1.a` instead. The shims'
//!    enum discriminants and struct layouts match
//!    `crates/gblp1/include/gblp1_ffi.h` byte-for-byte.
//!
//! `no_std` when targeting UEFI (`target_os = "uefi"`); the firmware
//! build links the `aarch64-unknown-uefi` staticlib. Hosted builds keep
//! `std` so cargo's test harness can unwind, and `pack()` (which needs
//! `alloc`) is available there.

// no_std on bare-metal / firmware targets. The host build keeps std
// for cargo's test harness and the alloc-feature-gated `pack()` path.
#![cfg_attr(not(feature = "std"), no_std)]

// On no_std targets (firmware + bare-metal) there is no runtime to
// fall back to — supply a tiny abort-loop panic handler. EDK2's
// linker strips this with --gc-sections if no panic is reachable.
#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

// `pack()` needs `Vec`. When the `alloc` feature is on we either pull
// it from std (hosted) or from the `alloc` crate (no_std + alloc —
// not exercised by firmware today, but the gate is in place for
// future use). The firmware build deselects all features so this
// block is gated out and the staticlib has no allocator dependency.
#[cfg(all(feature = "alloc", not(feature = "std")))]
extern crate alloc as core_alloc;
#[cfg(all(feature = "alloc", not(feature = "std")))]
use core_alloc::vec::Vec;

pub mod ffi;

// --- Wire constants ---------------------------------------------------
//
// Port byte-for-byte from `tools/shared/gblp1.h`. The header still
// exists for host C tools that PR2 Task 8 collapses into the multicall;
// keep these in sync until that task lands.

pub const GBLP1_MAGIC: &[u8; 8] = b"GBLP1\0\0\0";
pub const GBLP1_MAGIC_SIZE: usize = 8;
pub const GBLP1_VERSION: u16 = 0x0001;
pub const GBLP1_HEADER_SIZE: usize = 28;
pub const GBLP1_FLAGS_LE: u32 = 0x0000_0001;
pub const GBLP1_FOOTER: &[u8; 8] = b"GBLP1END";
pub const GBLP1_FOOTER_SIZE: usize = 8;
pub const GBLP1_TOTAL_SIZE_CAP: usize = 16 * 1024 * 1024;
pub const GBLP1_PAYLOAD_ALIGN: usize = 16;
pub const GBLP1_ENTRY_SIZE: usize = 48;

pub const GBLP1_TYPE_CACHED_ABL: u16 = 0x0001;
pub const GBLP1_TYPE_SOURCE_META: u16 = 0x0002;
pub const GBLP1_TYPE_MODE2_PROFILE: u16 = 0x0010;
pub const GBLP1_TYPE_MANIFEST: u16 = 0x0020;

pub const GBLP1_MANIFEST_MAGIC: &[u8; 4] = b"GMAN";
pub const GBLP1_MANIFEST_MAGIC_SIZE: usize = 4;
pub const GBLP1_MANIFEST_SIZE: usize = 16;
pub const GBLP1_MANIFEST_SCHEMA_VERSION: u16 = 1;
pub const GBLP1_MANIFEST_BIT_FAKELOCK_HOOK: u16 = 0x0001;
pub const GBLP1_MANIFEST_BIT_PROFILE_SPOOF: u16 = 0x0002;
pub const GBLP1_MANIFEST_BITS_RESERVED_MASK: u16 = 0xFFFC;

// --- Errors -----------------------------------------------------------

/// Reasons a GBLP1 container can fail header / entry validation.
///
/// Variants are listed in the same declaration order as
/// `enum gbl_payload_status` in the legacy `PayloadParse.h`; the FFI
/// shim maps them to that enum's exact discriminants — see
/// [`ffi::GblPayloadStatus`].
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum ParseError {
    TooSmall,
    BadMagic,
    BadVersion,
    BadHeaderSize,
    BadFlags,
    BadTotalSize,
    BadEntryCount,
    HeaderCrcMismatch,
    FooterMismatch,
    EntryBadType,
    EntryBadFlags,
    EntryBadReserved,
    EntryBadOffset,
    EntryBadSize,
    EntryShaMismatch,
    NoCachedAbl,
    NoMode2Profile,
}

/// Reasons a manifest entry (type 0x0020) can fail validation.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum ManifestError {
    BadSize,
    BadMagic,
    BadSchema,
    BadReserved,
}

// --- Parsed-form types -------------------------------------------------

/// Parsed manifest entry.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub struct Manifest {
    /// Raw 16-bit capability bitfield from the wire. Already validated
    /// against [`GBLP1_MANIFEST_BITS_RESERVED_MASK`]; callers compare
    /// against the `GBLP1_MANIFEST_BIT_*` constants.
    pub cap_bits: u16,
}

#[derive(Debug, Clone, Copy)]
struct Entry {
    type_: u16,
    offset: u32,
    size: u32,
}

/// A validated GBLP1 container, ready to query for typed entries.
///
/// Borrows the underlying byte buffer for the container's lifetime — no
/// allocation, no copy. Mirrors the C parser which returns pointers
/// into the input buffer.
pub struct Container<'a> {
    bytes: &'a [u8],
    entries: [Option<Entry>; 64],
    entry_count: usize,
}

impl<'a> Container<'a> {
    /// Find the unique `cached_abl` entry (type 0x0001).
    ///
    /// Returns `None` when the container has no such entry — caller
    /// maps this to `NoCachedAbl` if absence is an error in context.
    pub fn find_cached_abl(&self) -> Option<&'a [u8]> {
        self.find(GBLP1_TYPE_CACHED_ABL)
    }

    /// Find the unique `mode2_profile` entry (type 0x0010).
    pub fn find_mode2_profile(&self) -> Option<&'a [u8]> {
        self.find(GBLP1_TYPE_MODE2_PROFILE)
    }

    /// Find + validate the unique `manifest` entry (type 0x0020).
    ///
    /// - `Ok(None)`: no manifest entry in the container; caller treats
    ///   as all-zero capabilities (mode-0 default).
    /// - `Ok(Some(m))`: manifest present and well-formed.
    /// - `Err(_)`: manifest entry present but malformed.
    pub fn find_manifest(&self) -> Result<Option<Manifest>, ManifestError> {
        let bytes = match self.find(GBLP1_TYPE_MANIFEST) {
            Some(b) => b,
            None => return Ok(None),
        };
        if bytes.len() != GBLP1_MANIFEST_SIZE {
            return Err(ManifestError::BadSize);
        }
        if bytes[0..GBLP1_MANIFEST_MAGIC_SIZE] != GBLP1_MANIFEST_MAGIC[..] {
            return Err(ManifestError::BadMagic);
        }
        let schema = le16(&bytes[4..6]);
        if schema != GBLP1_MANIFEST_SCHEMA_VERSION {
            return Err(ManifestError::BadSchema);
        }
        let bits = le16(&bytes[6..8]);
        if bits & GBLP1_MANIFEST_BITS_RESERVED_MASK != 0 {
            return Err(ManifestError::BadReserved);
        }
        // Reserved pad: bytes 8..16 must be zero.
        for i in 8..16 {
            if bytes[i] != 0 {
                return Err(ManifestError::BadReserved);
            }
        }
        Ok(Some(Manifest { cap_bits: bits }))
    }

    fn find(&self, want: u16) -> Option<&'a [u8]> {
        for slot in self.entries.iter().take(self.entry_count) {
            if let Some(e) = slot {
                if e.type_ == want {
                    let off = e.offset as usize;
                    let sz = e.size as usize;
                    return Some(&self.bytes[off..off + sz]);
                }
            }
        }
        None
    }
}

// --- Parsing ----------------------------------------------------------

#[inline]
fn le16(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}

#[inline]
fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

/// Validate only the GBLP1 header + footer layout. Mirrors the C
/// `gbl_payload_validate_header`: does not walk entries or hash
/// payloads.
pub fn validate_header(b: &[u8]) -> Result<(), ParseError> {
    if b.len() < GBLP1_HEADER_SIZE + GBLP1_ENTRY_SIZE + GBLP1_FOOTER_SIZE {
        return Err(ParseError::TooSmall);
    }
    if b[0..GBLP1_MAGIC_SIZE] != GBLP1_MAGIC[..] {
        return Err(ParseError::BadMagic);
    }
    if le16(&b[8..10]) != GBLP1_VERSION {
        return Err(ParseError::BadVersion);
    }
    if le16(&b[10..12]) as usize != GBLP1_HEADER_SIZE {
        return Err(ParseError::BadHeaderSize);
    }
    let flags = le32(&b[12..16]);
    if flags & GBLP1_FLAGS_LE == 0 || flags & !GBLP1_FLAGS_LE != 0 {
        return Err(ParseError::BadFlags);
    }
    let total = le32(&b[16..20]) as usize;
    if total > GBLP1_TOTAL_SIZE_CAP || total > b.len() {
        return Err(ParseError::BadTotalSize);
    }
    let ec = le32(&b[20..24]);
    if ec < 1 {
        return Err(ParseError::BadEntryCount);
    }
    if GBLP1_HEADER_SIZE + (ec as usize) * GBLP1_ENTRY_SIZE + GBLP1_FOOTER_SIZE > total {
        return Err(ParseError::BadEntryCount);
    }
    if crc32(&b[0..24]) != le32(&b[24..28]) {
        return Err(ParseError::HeaderCrcMismatch);
    }
    if b[total - GBLP1_FOOTER_SIZE..total] != GBLP1_FOOTER[..] {
        return Err(ParseError::FooterMismatch);
    }
    Ok(())
}

/// Parse + validate the GBLP1 container at `bytes`.
///
/// Validates the header, walks every entry (type/flags/reserved/
/// offset/size + per-entry SHA-256), and indexes the entries for
/// later `find_*` queries. Per the C parser, unknown entry types are
/// silently skipped — only their structural checks (offset/size/SHA)
/// run.
///
/// Duplicate `cached_abl`, `mode2_profile`, or `manifest` entries
/// produce `EntryBadType` to mirror the C parser.
pub fn parse(bytes: &[u8]) -> Result<Container<'_>, ParseError> {
    validate_header(bytes)?;
    let total = le32(&bytes[16..20]) as usize;
    let _ = total; // total bounds-checked in validate_header
    let ec = le32(&bytes[20..24]) as usize;

    let entries_end = GBLP1_HEADER_SIZE + ec * GBLP1_ENTRY_SIZE;
    let payload_region_start =
        (entries_end + GBLP1_PAYLOAD_ALIGN - 1) & !(GBLP1_PAYLOAD_ALIGN - 1);

    if ec > 64 {
        // Defensive cap. The C parser has no such cap, but in practice
        // gbl-pack only emits ~4 entries; 64 is generous and keeps the
        // Container struct stack-allocatable.
        return Err(ParseError::BadEntryCount);
    }

    let mut entries: [Option<Entry>; 64] = [None; 64];
    let mut seen_types: [u16; 64] = [0; 64];
    let mut seen_count = 0usize;
    let total = le32(&bytes[16..20]) as usize;

    for i in 0..ec {
        let e = &bytes[GBLP1_HEADER_SIZE + i * GBLP1_ENTRY_SIZE..];
        let type_ = le16(&e[0..2]);
        let flags = le16(&e[2..4]);
        let off = le32(&e[4..8]);
        let sz = le32(&e[8..12]);
        let reserved = le32(&e[12..16]);
        let recorded_sha = &e[16..48];

        if type_ == 0 {
            return Err(ParseError::EntryBadType);
        }
        if flags != 0 {
            return Err(ParseError::EntryBadFlags);
        }
        if reserved != 0 {
            return Err(ParseError::EntryBadReserved);
        }
        if (off as usize) < payload_region_start
            || (off as usize) & (GBLP1_PAYLOAD_ALIGN - 1) != 0
        {
            return Err(ParseError::EntryBadOffset);
        }
        if (off as usize) + (sz as usize) + GBLP1_FOOTER_SIZE > total {
            return Err(ParseError::EntryBadSize);
        }

        let payload = &bytes[off as usize..off as usize + sz as usize];
        let got = sha256(payload);
        if got[..] != *recorded_sha {
            return Err(ParseError::EntryShaMismatch);
        }

        // Duplicate detection for typed entries the parser exposes by
        // type. Mirrors the C `if (type == want_type) { if (found)
        // return ENTRY_BAD_TYPE; }` check.
        if matches!(
            type_,
            GBLP1_TYPE_CACHED_ABL | GBLP1_TYPE_MODE2_PROFILE | GBLP1_TYPE_MANIFEST
        ) {
            for j in 0..seen_count {
                if seen_types[j] == type_ {
                    return Err(ParseError::EntryBadType);
                }
            }
            seen_types[seen_count] = type_;
            seen_count += 1;
        }

        entries[i] = Some(Entry {
            type_,
            offset: off,
            size: sz,
        });
    }

    Ok(Container {
        bytes,
        entries,
        entry_count: ec,
    })
}

/// Scan a byte buffer for a GBLP1 container, tolerating stray copies
/// of the 8-byte magic.
///
/// At each occurrence of [`GBLP1_MAGIC`], attempt to parse from that
/// offset; return the first occurrence that fully validates. Mirrors
/// `gbl_payload_scan_cached_abl` but produces a typed container for
/// any consumer (cached_abl, mode2, manifest) rather than narrowing to
/// the cached_abl entry.
///
/// Returns `None` when no occurrence of the magic produces a valid
/// container. The C-ABI shim distinguishes "magic never seen" from
/// "magic seen but never validated" — see [`ffi::gbl_payload_scan_cached_abl`].
pub fn scan_for_container(bytes: &[u8]) -> Option<Container<'_>> {
    let mut i = 0;
    while i + GBLP1_MAGIC_SIZE <= bytes.len() {
        if bytes[i] == b'G' && bytes[i..i + GBLP1_MAGIC_SIZE] == GBLP1_MAGIC[..] {
            if let Ok(c) = parse(&bytes[i..]) {
                return Some(c);
            }
        }
        i += 1;
    }
    None
}

// --- Hash + CRC helpers ----------------------------------------------

/// Single-shot SHA-256. Backed by `sha2`.
pub fn sha256(buf: &[u8]) -> [u8; 32] {
    use sha2::Digest;
    let mut h = sha2::Sha256::new();
    h.update(buf);
    let out = h.finalize();
    let mut r = [0u8; 32];
    r.copy_from_slice(&out);
    r
}

/// IEEE-802.3 CRC-32 (reflected polynomial 0xEDB88320, init/final-XOR
/// 0xFFFFFFFF). Backed by `crc32fast`.
pub fn crc32(buf: &[u8]) -> u32 {
    let mut h = crc32fast::Hasher::new();
    h.update(buf);
    h.finalize()
}

// --- Packing ----------------------------------------------------------

/// Inputs to [`pack`]. Mirrors `struct gbl_pack_inputs` in
/// `tools/gbl-pack/pack.h` — Task 4 keeps the C packer as the only
/// pack-time caller (it forward-decls the helpers below). Reproducing
/// it here lets in-workspace Rust call sites build containers without
/// depending on the C source. Host-only (uses `Vec`).
#[cfg(feature = "alloc")]
#[derive(Debug, Default)]
pub struct PackInputs<'a> {
    pub cached_abl: Option<&'a [u8]>,
    pub source: Option<&'a [u8]>,
    pub extracted: Option<&'a [u8]>,
    pub mode2_profile: Option<&'a [u8]>,
    pub manifest_cap_bits: Option<u16>,
    pub packer_version: Option<&'a str>,
    pub timestamp_iso8601: Option<&'a str>,
}

/// Reasons [`pack`] can refuse to build a container.
#[cfg(feature = "alloc")]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum PackError {
    BadInput,
    ProfileBad,
    ManifestBad,
    TooLarge,
}

/// Build a GBLP1 v1 container.
///
/// Mirrors `gbl_pack_build` in `tools/gbl-pack/pack.c`, minus the
/// PE-sanity check (callers can run [`pe_utils::pe_sanity`] themselves
/// before calling). Host-only; the firmware never packs.
#[cfg(feature = "alloc")]
pub fn pack(inputs: &PackInputs<'_>) -> Result<Vec<u8>, PackError> {
    let have_cached = matches!(inputs.cached_abl, Some(b) if !b.is_empty());
    let have_profile = matches!(inputs.mode2_profile, Some(b) if !b.is_empty());
    let have_manifest = inputs.manifest_cap_bits.is_some();
    if !have_cached && !have_profile && !have_manifest {
        return Err(PackError::BadInput);
    }
    if have_profile {
        let p = inputs.mode2_profile.unwrap();
        // gbl-pack additionally validates the 4-byte GM2P magic; the
        // crate stays magic-agnostic here because mode2-profile-core
        // owns that check and the Rust packer is the caller's
        // responsibility to feed valid bytes. Replicate the size check
        // only — it's structural for the container.
        const GBL_M2P_SIZE: usize = 120; // see tools/shared/gbl_mode2_profile.h
        if p.len() != GBL_M2P_SIZE {
            return Err(PackError::ProfileBad);
        }
        if p[0..4] != *b"GM2P" {
            return Err(PackError::ProfileBad);
        }
    }
    if let Some(bits) = inputs.manifest_cap_bits {
        if bits & GBLP1_MANIFEST_BITS_RESERVED_MASK != 0 {
            return Err(PackError::ManifestBad);
        }
    }

    let pv_len = inputs.packer_version.map(str::len).unwrap_or(0);
    let ts_len = inputs.timestamp_iso8601.map(str::len).unwrap_or(0);
    // source_meta payload: 3 × (u32 size + 32 SHA), then u32 + pv, u32 + ts.
    let meta_size = 3 * (4 + 32) + 4 + pv_len + 4 + ts_len;

    #[derive(Clone, Copy)]
    struct EntPlan {
        type_: u16,
        size: usize,
    }
    let mut ents: [EntPlan; 4] = [EntPlan { type_: 0, size: 0 }; 4];
    let mut ec = 0usize;
    if have_cached {
        ents[ec] = EntPlan {
            type_: GBLP1_TYPE_CACHED_ABL,
            size: inputs.cached_abl.unwrap().len(),
        };
        ec += 1;
        ents[ec] = EntPlan {
            type_: GBLP1_TYPE_SOURCE_META,
            size: meta_size,
        };
        ec += 1;
    }
    if have_profile {
        ents[ec] = EntPlan {
            type_: GBLP1_TYPE_MODE2_PROFILE,
            size: inputs.mode2_profile.unwrap().len(),
        };
        ec += 1;
    }
    if have_manifest {
        ents[ec] = EntPlan {
            type_: GBLP1_TYPE_MANIFEST,
            size: GBLP1_MANIFEST_SIZE,
        };
        ec += 1;
    }

    let entries_end = GBLP1_HEADER_SIZE + ec * GBLP1_ENTRY_SIZE;
    let mut off = align_up(entries_end, GBLP1_PAYLOAD_ALIGN);
    let mut payload_off: [usize; 4] = [0; 4];
    for i in 0..ec {
        payload_off[i] = off;
        off = align_up(off + ents[i].size, GBLP1_PAYLOAD_ALIGN);
    }
    let total = off + GBLP1_FOOTER_SIZE;
    if total > GBLP1_TOTAL_SIZE_CAP {
        return Err(PackError::TooLarge);
    }

    let mut buf = alloc_vec(total);

    // Header.
    buf[0..GBLP1_MAGIC_SIZE].copy_from_slice(&GBLP1_MAGIC[..]);
    write_le16(&mut buf[8..10], GBLP1_VERSION);
    write_le16(&mut buf[10..12], GBLP1_HEADER_SIZE as u16);
    write_le32(&mut buf[12..16], GBLP1_FLAGS_LE);
    write_le32(&mut buf[16..20], total as u32);
    write_le32(&mut buf[20..24], ec as u32);

    // Footer.
    buf[total - GBLP1_FOOTER_SIZE..total].copy_from_slice(&GBLP1_FOOTER[..]);

    // Entry table + payloads.
    for i in 0..ec {
        // Build payload.
        let poff = payload_off[i];
        match ents[i].type_ {
            GBLP1_TYPE_CACHED_ABL => {
                let src = inputs.cached_abl.unwrap();
                buf[poff..poff + src.len()].copy_from_slice(src);
            }
            GBLP1_TYPE_SOURCE_META => {
                let mut m = poff;
                let src_size = inputs.source.map(|b| b.len()).unwrap_or(0);
                write_le32(&mut buf[m..m + 4], src_size as u32);
                m += 4;
                if let Some(s) = inputs.source {
                    let d = sha256(s);
                    buf[m..m + 32].copy_from_slice(&d);
                }
                m += 32;
                let ext_size = inputs.extracted.map(|b| b.len()).unwrap_or(0);
                write_le32(&mut buf[m..m + 4], ext_size as u32);
                m += 4;
                if let Some(s) = inputs.extracted {
                    let d = sha256(s);
                    buf[m..m + 32].copy_from_slice(&d);
                }
                m += 32;
                let cached = inputs.cached_abl.unwrap();
                write_le32(&mut buf[m..m + 4], cached.len() as u32);
                m += 4;
                let d = sha256(cached);
                buf[m..m + 32].copy_from_slice(&d);
                m += 32;
                write_le32(&mut buf[m..m + 4], pv_len as u32);
                m += 4;
                if let Some(s) = inputs.packer_version {
                    buf[m..m + pv_len].copy_from_slice(s.as_bytes());
                }
                m += pv_len;
                write_le32(&mut buf[m..m + 4], ts_len as u32);
                m += 4;
                if let Some(s) = inputs.timestamp_iso8601 {
                    buf[m..m + ts_len].copy_from_slice(s.as_bytes());
                }
            }
            GBLP1_TYPE_MODE2_PROFILE => {
                let src = inputs.mode2_profile.unwrap();
                buf[poff..poff + src.len()].copy_from_slice(src);
            }
            GBLP1_TYPE_MANIFEST => {
                buf[poff..poff + GBLP1_MANIFEST_MAGIC_SIZE]
                    .copy_from_slice(&GBLP1_MANIFEST_MAGIC[..]);
                write_le16(
                    &mut buf[poff + 4..poff + 6],
                    GBLP1_MANIFEST_SCHEMA_VERSION,
                );
                write_le16(
                    &mut buf[poff + 6..poff + 8],
                    inputs.manifest_cap_bits.unwrap(),
                );
                // bytes [poff+8..poff+16] stay zero (calloc-like vec).
            }
            _ => unreachable!(),
        }
        // Compute SHA over the payload range before writing the
        // entry-table region (no overlap, but read-then-write is
        // clearer).
        let payload_sha = sha256(&buf[poff..poff + ents[i].size]);
        let e = GBLP1_HEADER_SIZE + i * GBLP1_ENTRY_SIZE;
        write_le16(&mut buf[e..e + 2], ents[i].type_);
        write_le16(&mut buf[e + 2..e + 4], 0);
        write_le32(&mut buf[e + 4..e + 8], poff as u32);
        write_le32(&mut buf[e + 8..e + 12], ents[i].size as u32);
        write_le32(&mut buf[e + 12..e + 16], 0);
        buf[e + 16..e + 48].copy_from_slice(&payload_sha);
    }

    // Header CRC last (over bytes [0..24)).
    let crc = crc32(&buf[0..24]);
    write_le32(&mut buf[24..28], crc);

    Ok(buf)
}

#[cfg(feature = "alloc")]
fn alloc_vec(n: usize) -> Vec<u8> {
    let mut v = Vec::with_capacity(n);
    v.resize(n, 0);
    v
}

#[cfg(feature = "alloc")]
#[inline]
fn align_up(v: usize, a: usize) -> usize {
    (v + a - 1) & !(a - 1)
}

#[cfg(feature = "alloc")]
#[inline]
fn write_le16(b: &mut [u8], v: u16) {
    b[0] = v as u8;
    b[1] = (v >> 8) as u8;
}

#[cfg(feature = "alloc")]
#[inline]
fn write_le32(b: &mut [u8], v: u32) {
    b[0] = v as u8;
    b[1] = (v >> 8) as u8;
    b[2] = (v >> 16) as u8;
    b[3] = (v >> 24) as u8;
}

// --- Unit tests --------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_known_answer() {
        // FIPS 180-2 vector: SHA-256("abc")
        let got = sha256(b"abc");
        let want: [u8; 32] = [
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae,
            0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61,
            0xf2, 0x00, 0x15, 0xad,
        ];
        assert_eq!(got, want);
    }

    #[test]
    fn crc32_known_answer() {
        assert_eq!(crc32(b"123456789"), 0xCBF43926);
    }
}
