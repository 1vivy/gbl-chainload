//! mode2_profile parser + host-side derive/compile.
//!
//! Replaces:
//!
//! - `GblChainloadPkg/Library/GblPayloadLib/Mode2Profile.c` — the
//!   firmware-side 120-byte payload parser.
//! - `GblChainloadPkg/Library/GblPayloadLib/Internal/Mode2Profile.h` —
//!   the matching C header (status enum + parse signature).
//!
//! The wire constants and the `struct gbl_mode2_profile` definition stay
//! in `tools/shared/gbl_mode2_profile.h` until PR2 Task 8 folds the host
//! tools into the `gbl` multicall. They are duplicated here as Rust
//! constants and the [`Profile`] struct; the `repr(C, packed)` layout
//! matches the C struct byte-for-byte.
//!
//! Two layers, same shape as `crates/gblp1`:
//!
//! 1. Idiomatic Rust API: [`parse`], [`compile`], [`derive`], the
//!    [`Profile`] struct, and the [`ParseError`] / [`CompileError`] /
//!    [`DeriveError`] enums.
//! 2. The [`ffi`] module: `extern "C"` shims preserving the C wire ABI
//!    so the firmware and the existing host C tool
//!    (`tools/mode2-profile`) can link `libmode2_profile_core.a` and
//!    keep working unchanged at the call sites.
//!
//! Firmware builds (`target_os = "uefi"` or bare-metal `aarch64-
//! unknown-none`) deselect all default features; only `parse` is reachable
//! in those configurations.

// no_std on bare-metal / firmware targets. The host build keeps std for
// the toml crate, cargo's test harness, and the feature-gated `derive` /
// `compile` paths.
#![cfg_attr(not(feature = "std"), no_std)]

// In the firmware build both `libgblp1.a` and `libmode2_profile_core.a`
// are linked into the same EDK2 image. Each `#[panic_handler]` lowers
// to a strong `rust_begin_unwind` symbol — two staticlibs both
// defining it would duplicate at final link. GblChainloadPkg.dsc
// resolves this by passing `--allow-multiple-definition` to ld; the
// two handlers are identical `loop {}` bodies so "first wins" is safe.
#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

#[cfg(feature = "alloc")]
extern crate alloc;

#[cfg(feature = "alloc")]
use alloc::string::String;
#[cfg(feature = "alloc")]
use alloc::vec::Vec;

pub mod ffi;

// --- Wire constants ---------------------------------------------------
//
// Port byte-for-byte from `tools/shared/gbl_mode2_profile.h`. The header
// still exists for host C tools that PR2 Task 8 collapses into the
// multicall; keep these in sync until that task lands.

pub const GBL_M2P_MAGIC: &[u8; 4] = b"GM2P";
pub const GBL_M2P_MAGIC_SIZE: usize = 4;
pub const GBL_M2P_VERSION: u16 = 0x0001;
pub const GBL_M2P_SIZE: usize = 120;

/// `color` field values — mirror the KMBootState.Color domain.
pub const GBL_M2P_COLOR_GREEN: u32 = 0;
pub const GBL_M2P_COLOR_YELLOW: u32 = 1;
pub const GBL_M2P_COLOR_ORANGE: u32 = 2;
pub const GBL_M2P_COLOR_RED: u32 = 3;

// --- Profile struct ---------------------------------------------------

/// 120-byte mode2 profile payload, host-endian.
///
/// This is the idiomatic Rust view of the wire `struct gbl_mode2_profile`
/// from `tools/shared/gbl_mode2_profile.h`. All scalar fields are
/// stored host-endian; the LE conversion happens at [`parse`] /
/// [`Profile::to_bytes`]. C callers go through the wire-shaped
/// [`ffi::GblMode2ProfileWire`] (which IS `repr(C, packed)` and matches
/// the C struct byte-for-byte).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Profile {
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

impl Profile {
    /// Serialize the profile to its 120-byte wire form.
    pub fn to_bytes(&self) -> [u8; GBL_M2P_SIZE] {
        let mut b = [0u8; GBL_M2P_SIZE];
        b[0..4].copy_from_slice(&self.magic);
        b[4..6].copy_from_slice(&self.version.to_le_bytes());
        b[6..8].copy_from_slice(&self.reserved.to_le_bytes());
        b[8..12].copy_from_slice(&self.is_unlocked.to_le_bytes());
        b[12..16].copy_from_slice(&self.color.to_le_bytes());
        b[16..20].copy_from_slice(&self.system_version.to_le_bytes());
        b[20..24].copy_from_slice(&self.system_spl.to_le_bytes());
        b[24..56].copy_from_slice(&self.rot_digest);
        b[56..88].copy_from_slice(&self.pubkey_digest);
        b[88..120].copy_from_slice(&self.vbh);
        b
    }
}

// --- Errors -----------------------------------------------------------

/// Reasons a mode2 profile binary can fail validation.
///
/// Variants are listed in the same declaration order as
/// `enum gbl_m2p_status` in the legacy `Internal/Mode2Profile.h`; the
/// FFI shim maps them to that enum's exact discriminants — see
/// [`ffi::Mode2Status`].
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum ParseError {
    /// `size != GBL_M2P_SIZE`.
    TooSmall,
    BadMagic,
    BadVersion,
    /// Reserved (offset 6) is non-zero.
    BadReserved,
    /// `is_unlocked > 1` or `color > GBL_M2P_COLOR_RED`.
    BadField,
}

/// Parse + validate a 120-byte mode2 profile payload.
///
/// On success, the returned [`Profile`] holds host-endian field values
/// — the wire is little-endian, the struct is `repr(C, packed)`, and
/// the parser does an explicit LE read for the multi-byte scalars.
pub fn parse(bytes: &[u8]) -> Result<Profile, ParseError> {
    if bytes.len() != GBL_M2P_SIZE {
        return Err(ParseError::TooSmall);
    }
    if &bytes[0..4] != &GBL_M2P_MAGIC[..] {
        return Err(ParseError::BadMagic);
    }
    let version = u16::from_le_bytes([bytes[4], bytes[5]]);
    if version != GBL_M2P_VERSION {
        return Err(ParseError::BadVersion);
    }
    let reserved = u16::from_le_bytes([bytes[6], bytes[7]]);
    if reserved != 0 {
        return Err(ParseError::BadReserved);
    }
    let is_unlocked = u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]);
    let color = u32::from_le_bytes([bytes[12], bytes[13], bytes[14], bytes[15]]);
    if is_unlocked > 1 {
        return Err(ParseError::BadField);
    }
    if color > GBL_M2P_COLOR_RED {
        return Err(ParseError::BadField);
    }
    let system_version = u32::from_le_bytes([bytes[16], bytes[17], bytes[18], bytes[19]]);
    let system_spl = u32::from_le_bytes([bytes[20], bytes[21], bytes[22], bytes[23]]);
    let mut rot_digest = [0u8; 32];
    rot_digest.copy_from_slice(&bytes[24..56]);
    let mut pubkey_digest = [0u8; 32];
    pubkey_digest.copy_from_slice(&bytes[56..88]);
    let mut vbh = [0u8; 32];
    vbh.copy_from_slice(&bytes[88..120]);
    Ok(Profile {
        magic: *GBL_M2P_MAGIC,
        version,
        reserved: 0,
        is_unlocked,
        color,
        system_version,
        system_spl,
        rot_digest,
        pubkey_digest,
        vbh,
    })
}

// --- Compile (TOML -> binary) -----------------------------------------

#[cfg(feature = "std")]
#[derive(Debug, PartialEq, Eq, Clone)]
pub enum CompileError {
    /// `toml::from_str` rejected the input.
    MalformedToml(String),
    /// A required key is missing or has the wrong type.
    MissingOrBadType(&'static str),
    /// An integer key is outside the legal range.
    OutOfRange { key: &'static str, value: i64 },
    /// A digest key is not exactly 64 lowercase-hex chars.
    BadDigest(&'static str),
    /// An unrecognized top-level key.
    UnknownKey(String),
}

/// TOML schema accepted by [`compile`].
///
/// Mirrors the Python `cmd_compile` accepted-keys set:
///   { version, is_unlocked, color, system_version, system_spl,
///     rot_digest, pubkey_digest, vbh }
///
/// Unknown keys are rejected (matches the C tool's `known[]` table).
#[cfg(feature = "std")]
#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct ProfileToml {
    version: i64,
    is_unlocked: i64,
    color: i64,
    system_version: i64,
    system_spl: i64,
    rot_digest: String,
    pubkey_digest: String,
    vbh: String,
}

#[cfg(feature = "std")]
fn parse_digest_hex(s: &str, name: &'static str) -> Result<[u8; 32], CompileError> {
    if s.len() != 64 {
        return Err(CompileError::BadDigest(name));
    }
    let bytes = s.as_bytes();
    let mut out = [0u8; 32];
    for i in 0..32 {
        let hi = bytes[2 * i];
        let lo = bytes[2 * i + 1];
        let hv = hex_nibble(hi).ok_or(CompileError::BadDigest(name))?;
        let lv = hex_nibble(lo).ok_or(CompileError::BadDigest(name))?;
        out[i] = (hv << 4) | lv;
    }
    Ok(out)
}

#[cfg(feature = "std")]
fn hex_nibble(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        _ => None,
    }
}

#[cfg(feature = "std")]
fn range_check(key: &'static str, v: i64, lo: i64, hi: i64) -> Result<i64, CompileError> {
    if v < lo || v > hi {
        Err(CompileError::OutOfRange { key, value: v })
    } else {
        Ok(v)
    }
}

/// Compile a profile TOML string to its 120-byte wire form.
///
/// Validation rules mirror the C tool (`tools/mode2-profile/mode2-profile.c`)
/// and the Python reference (`tools/mode2-profile/mode2-profile.py`):
///
/// - `version` must equal 1.
/// - `is_unlocked` in `[0, 1]`.
/// - `color` in `[0, 3]`.
/// - `system_version`, `system_spl` in `[0, 0xFFFFFFFF]`.
/// - The three digest fields are exactly 64 lowercase-hex chars.
/// - Unknown top-level keys are rejected.
#[cfg(feature = "std")]
pub fn compile(toml_str: &str) -> Result<Vec<u8>, CompileError> {
    let t: ProfileToml = toml::from_str(toml_str).map_err(|e| {
        // Unknown-key errors surface here via serde's `deny_unknown_fields`;
        // toml 0.8 wraps the inner serde message in a positioned error
        // like "TOML parse error at line N, column M\n...\nunknown field
        // `foo`, expected ...". We search for the `unknown field` marker
        // anywhere in the formatted message — it's a stable serde phrase.
        let msg = e.to_string();
        const UF: &str = "unknown field `";
        if let Some(start) = msg.find(UF) {
            let rest = &msg[start + UF.len()..];
            if let Some(end) = rest.find('`') {
                return CompileError::UnknownKey(rest[..end].to_string());
            }
        }
        CompileError::MalformedToml(msg)
    })?;

    range_check("version", t.version, 1, 1)?;
    let is_unlocked = range_check("is_unlocked", t.is_unlocked, 0, 1)? as u32;
    let color = range_check("color", t.color, 0, 3)? as u32;
    let system_version =
        range_check("system_version", t.system_version, 0, 0xFFFF_FFFF)? as u32;
    let system_spl = range_check("system_spl", t.system_spl, 0, 0xFFFF_FFFF)? as u32;
    let rot_digest = parse_digest_hex(&t.rot_digest, "rot_digest")?;
    let pubkey_digest = parse_digest_hex(&t.pubkey_digest, "pubkey_digest")?;
    let vbh = parse_digest_hex(&t.vbh, "vbh")?;

    let profile = Profile {
        magic: *GBL_M2P_MAGIC,
        version: GBL_M2P_VERSION,
        reserved: 0,
        is_unlocked,
        color,
        system_version,
        system_spl,
        rot_digest,
        pubkey_digest,
        vbh,
    };
    Ok(profile.to_bytes().to_vec())
}

// --- Derive (stock vbmeta -> Profile) ---------------------------------
//
// This is a minimal AVB header walker — just enough to extract the
// public key blob and the {os_version, security_patch} property
// descriptors. PR2 Task 6 lands `crates/avb-parse` which will replace
// this inline parser; until then we keep it self-contained to avoid an
// inter-crate dep on a placeholder.

#[cfg(feature = "std")]
#[derive(Debug, PartialEq, Eq, Clone)]
pub enum DeriveError {
    TooSmall,
    BadMagic,
    MalformedHeader,
    NoPublicKey,
    PublicKeyPastAux,
    DescriptorsPastAux,
    NoOsVersionProperty,
    NoSecurityPatchProperty,
    /// e.g. "16.0.7" -> minor or sub doesn't fit 7 bits.
    OsVersionOutOfRange,
    /// "YYYY-MM-DD" -> year not 2000-2127, month 1-12, day 1-31.
    SplOutOfRange,
    SplMalformed,
}

#[cfg(feature = "std")]
const AVB_VBMETA_MAGIC: &[u8; 4] = b"AVB0";
#[cfg(feature = "std")]
const AVB_VBMETA_HEADER_SIZE: usize = 256;
#[cfg(feature = "std")]
const AVB_DESC_TAG_PROPERTY: u64 = 0;

#[cfg(feature = "std")]
fn read_u64_be(b: &[u8]) -> u64 {
    u64::from_be_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]])
}

/// Derive a mode2 [`Profile`] from a stock vbmeta image.
///
/// The profile carries:
/// - `rot_digest = SHA256(pubkey || 0x00)`
/// - `pubkey_digest = SHA256(pubkey)`
/// - `vbh = SHA256(vbmeta[0..header+auth+aux])`
/// - `system_version` / `system_spl` encoded from the
///   `com.android.build.boot.os_version` and
///   `com.android.build.boot.security_patch` property descriptors.
///
/// `is_unlocked` defaults to 0, `color` defaults to 0 (GREEN) — matches
/// the Python reference (`tools/mode2-profile/mode2-profile.py
/// cmd_derive`) and the host C tool. Callers that need a different
/// boot-state profile edit the resulting TOML before [`compile`].
#[cfg(feature = "std")]
pub fn derive(vbmeta: &[u8]) -> Result<Profile, DeriveError> {
    use sha2::{Digest, Sha256};

    if vbmeta.len() < AVB_VBMETA_HEADER_SIZE {
        return Err(DeriveError::TooSmall);
    }
    if &vbmeta[0..4] != &AVB_VBMETA_MAGIC[..] {
        return Err(DeriveError::BadMagic);
    }

    // Header layout (big-endian, libavb avb_vbmeta_image.h; field offsets
    // mirror AvbParse.c's AvbParse_VbmetaHeader):
    //   off   4..  8 : major version (u32 BE)
    //   off   8.. 12 : minor version (u32 BE)
    //   off  12.. 20 : authentication_data_block_size (u64 BE)
    //   off  20.. 28 : auxiliary_data_block_size      (u64 BE)
    //   off  28.. 32 : algorithm_type (u32 BE)         — unused
    //   off  32.. 40 : hash_offset (u64 BE)            — unused
    //   off  40.. 48 : hash_size                       — unused
    //   off  48.. 56 : signature_offset                — unused
    //   off  56.. 64 : signature_size                  — unused
    //   off  64.. 72 : public_key_offset (u64 BE)
    //   off  72.. 80 : public_key_size (u64 BE)
    //   off  80.. 88 : public_key_metadata_offset     — unused
    //   off  88.. 96 : public_key_metadata_size       — unused
    //   off  96..104 : descriptors_offset (u64 BE)
    //   off 104..112 : descriptors_size (u64 BE)
    let auth_size = read_u64_be(&vbmeta[12..20]);
    let aux_size = read_u64_be(&vbmeta[20..28]);
    let pk_off = read_u64_be(&vbmeta[64..72]);
    let pk_size = read_u64_be(&vbmeta[72..80]);
    let desc_off = read_u64_be(&vbmeta[96..104]);
    let desc_size = read_u64_be(&vbmeta[104..112]);

    let header_size = AVB_VBMETA_HEADER_SIZE as u64;
    let vbmeta_total = header_size
        .checked_add(auth_size)
        .and_then(|x| x.checked_add(aux_size))
        .ok_or(DeriveError::MalformedHeader)?;
    if vbmeta_total > vbmeta.len() as u64 {
        return Err(DeriveError::MalformedHeader);
    }
    if pk_size == 0 {
        return Err(DeriveError::NoPublicKey);
    }
    let aux_off = header_size + auth_size;
    if pk_off > aux_size || pk_size > aux_size.saturating_sub(pk_off) {
        return Err(DeriveError::PublicKeyPastAux);
    }
    if desc_off > aux_size || desc_size > aux_size.saturating_sub(desc_off) {
        return Err(DeriveError::DescriptorsPastAux);
    }

    let pubkey_start = (aux_off + pk_off) as usize;
    let pubkey_end = pubkey_start + pk_size as usize;
    let pubkey = &vbmeta[pubkey_start..pubkey_end];

    // rot_digest = SHA256(pubkey || 0x00)
    let mut h = Sha256::new();
    h.update(pubkey);
    h.update([0u8]);
    let rot_digest: [u8; 32] = h.finalize().into();

    // pubkey_digest = SHA256(pubkey)
    let pubkey_digest: [u8; 32] = Sha256::digest(pubkey).into();

    // vbh = SHA256(vbmeta[0..header+auth+aux])
    let vbh: [u8; 32] = Sha256::digest(&vbmeta[..vbmeta_total as usize]).into();

    // Walk property descriptors in [desc_off, desc_off+desc_size).
    let aux_block_start = aux_off as usize;
    let mut cursor = desc_off as usize;
    let walk_end = (desc_off + desc_size) as usize;
    let mut os_ver_str: Option<&str> = None;
    let mut spl_str: Option<&str> = None;

    // Descriptor header: tag(u64 BE), num_bytes_following(u64 BE).
    while cursor + 16 <= walk_end {
        let h_start = aux_block_start + cursor;
        let tag = read_u64_be(&vbmeta[h_start..h_start + 8]);
        let nbf = read_u64_be(&vbmeta[h_start + 8..h_start + 16]);
        // Total descriptor size = 16-byte header + num_bytes_following,
        // rounded up to 8 (libavb's AVB_DESCRIPTOR_ALIGNMENT). We
        // don't strictly need the rounding here — we advance the cursor
        // by the exact body size — but bounds-check matches libavb.
        let body_start = cursor + 16;
        let body_end = body_start.checked_add(nbf as usize).ok_or(DeriveError::MalformedHeader)?;
        if body_end > walk_end {
            break;
        }

        if tag == AVB_DESC_TAG_PROPERTY && nbf >= 16 {
            // Property descriptor body:
            //   u64 BE key_num_bytes (klen)
            //   u64 BE value_num_bytes (vlen)
            //   key[klen] '\0' value[vlen] '\0'
            let body = &vbmeta[aux_block_start + body_start..aux_block_start + body_end];
            let klen = read_u64_be(&body[0..8]) as usize;
            let vlen = read_u64_be(&body[8..16]) as usize;
            // After the two u64 lengths we need klen + 1 + vlen + 1 bytes.
            let need = klen.checked_add(1)
                .and_then(|x| x.checked_add(vlen))
                .and_then(|x| x.checked_add(1));
            if let Some(need) = need {
                if 16 + need <= body.len() {
                    let key = &body[16..16 + klen];
                    let val = &body[16 + klen + 1..16 + klen + 1 + vlen];
                    if key == b"com.android.build.boot.os_version" {
                        if let Ok(s) = core::str::from_utf8(val) {
                            os_ver_str = Some(s);
                        }
                    } else if key == b"com.android.build.boot.security_patch" {
                        if let Ok(s) = core::str::from_utf8(val) {
                            spl_str = Some(s);
                        }
                    }
                }
            }
        }
        cursor = body_end;
    }

    let os_ver_str = os_ver_str.ok_or(DeriveError::NoOsVersionProperty)?;
    let spl_str = spl_str.ok_or(DeriveError::NoSecurityPatchProperty)?;
    let system_version = encode_os_version(os_ver_str)?;
    let system_spl = encode_spl(spl_str)?;

    Ok(Profile {
        magic: *GBL_M2P_MAGIC,
        version: GBL_M2P_VERSION,
        reserved: 0,
        is_unlocked: 0,
        color: 0,
        system_version,
        system_spl,
        rot_digest,
        pubkey_digest,
        vbh,
    })
}

#[cfg(feature = "std")]
fn encode_os_version(s: &str) -> Result<u32, DeriveError> {
    // Parse "M[.N[.P]]" — missing components default to 0. Matches the
    // C tool's sscanf behavior and the Python reference.
    let parts: Vec<&str> = s.split('.').collect();
    let major: i64 = parts.first().and_then(|p| p.parse().ok()).unwrap_or(0);
    let minor: i64 = parts.get(1).and_then(|p| p.parse().ok()).unwrap_or(0);
    let sub: i64 = parts.get(2).and_then(|p| p.parse().ok()).unwrap_or(0);
    if !(0..=0x7F).contains(&minor) || !(0..=0x7F).contains(&sub) {
        return Err(DeriveError::OsVersionOutOfRange);
    }
    if !(0..=0x3FFFF).contains(&major) {
        return Err(DeriveError::OsVersionOutOfRange);
    }
    Ok(((major as u32) << 14) | ((minor as u32) << 7) | (sub as u32))
}

#[cfg(feature = "std")]
fn encode_spl(s: &str) -> Result<u32, DeriveError> {
    // Parse "YYYY-MM-DD". Matches the C tool's sscanf >= 3 check.
    let parts: Vec<&str> = s.split('-').collect();
    if parts.len() < 3 {
        return Err(DeriveError::SplMalformed);
    }
    let year: i64 = parts[0].parse().map_err(|_| DeriveError::SplMalformed)?;
    let month: i64 = parts[1].parse().map_err(|_| DeriveError::SplMalformed)?;
    let day: i64 = parts[2].parse().map_err(|_| DeriveError::SplMalformed)?;
    if !(2000..=2127).contains(&year) {
        return Err(DeriveError::SplOutOfRange);
    }
    if !(1..=12).contains(&month) {
        return Err(DeriveError::SplOutOfRange);
    }
    if !(1..=31).contains(&day) {
        return Err(DeriveError::SplOutOfRange);
    }
    Ok(((day as u32) << 11) | (((year - 2000) as u32) << 4) | (month as u32))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn good_profile_bytes() -> Vec<u8> {
        // Mirror the fixture from tests/host/076_mode2_profile_parse.sh.
        let mut p = Vec::with_capacity(GBL_M2P_SIZE);
        p.extend_from_slice(GBL_M2P_MAGIC);
        p.extend_from_slice(&1u16.to_le_bytes()); // version
        p.extend_from_slice(&0u16.to_le_bytes()); // reserved
        p.extend_from_slice(&0u32.to_le_bytes()); // is_unlocked
        p.extend_from_slice(&0u32.to_le_bytes()); // color
        p.extend_from_slice(&0x40000u32.to_le_bytes()); // sysver
        p.extend_from_slice(&0x9A4u32.to_le_bytes()); // spl
        p.extend((0..32).map(|b| b as u8));
        p.extend((32..64).map(|b| b as u8));
        p.extend((64..96).map(|b| b as u8));
        assert_eq!(p.len(), GBL_M2P_SIZE);
        p
    }

    #[test]
    fn parse_good() {
        let b = good_profile_bytes();
        let p = parse(&b).expect("good profile parses");
        assert_eq!(p.version, 1);
        assert_eq!(p.system_version, 0x40000);
        assert_eq!(p.system_spl, 0x9A4);
    }

    #[test]
    fn parse_bad_magic() {
        let mut b = good_profile_bytes();
        b[0] = b'X';
        assert_eq!(parse(&b), Err(ParseError::BadMagic));
    }

    #[test]
    fn parse_too_small() {
        let b = good_profile_bytes();
        assert_eq!(parse(&b[..119]), Err(ParseError::TooSmall));
    }

    #[test]
    fn parse_bad_color() {
        let mut b = good_profile_bytes();
        b[12..16].copy_from_slice(&9u32.to_le_bytes());
        assert_eq!(parse(&b), Err(ParseError::BadField));
    }

    #[test]
    fn parse_bad_version() {
        let mut b = good_profile_bytes();
        b[4..6].copy_from_slice(&2u16.to_le_bytes());
        assert_eq!(parse(&b), Err(ParseError::BadVersion));
    }

    #[test]
    fn parse_bad_reserved() {
        let mut b = good_profile_bytes();
        b[6..8].copy_from_slice(&1u16.to_le_bytes());
        assert_eq!(parse(&b), Err(ParseError::BadReserved));
    }

    #[test]
    fn parse_bad_is_unlocked() {
        let mut b = good_profile_bytes();
        b[8..12].copy_from_slice(&5u32.to_le_bytes());
        assert_eq!(parse(&b), Err(ParseError::BadField));
    }

    #[test]
    fn compile_round_trip() {
        let t = "version        = 1\n\
                 is_unlocked    = 0\n\
                 color          = 0\n\
                 system_version = 0x40000\n\
                 system_spl     = 0x9A4\n\
                 rot_digest     = \"1111111111111111111111111111111111111111111111111111111111111111\"\n\
                 pubkey_digest  = \"2222222222222222222222222222222222222222222222222222222222222222\"\n\
                 vbh            = \"3333333333333333333333333333333333333333333333333333333333333333\"\n";
        let bin = compile(t).expect("good TOML compiles");
        assert_eq!(bin.len(), GBL_M2P_SIZE);
        let p = parse(&bin).expect("compiled binary parses");
        assert_eq!(p.system_version, 0x40000);
        assert_eq!(p.rot_digest[0], 0x11);
        assert_eq!(p.pubkey_digest[0], 0x22);
        assert_eq!(p.vbh[0], 0x33);
    }

    #[test]
    fn compile_bad_version() {
        let t = "version = 2\nis_unlocked = 0\ncolor = 0\nsystem_version = 0\nsystem_spl = 0\n\
                 rot_digest = \"00\"\npubkey_digest = \"00\"\nvbh = \"00\"\n";
        assert!(matches!(compile(t), Err(CompileError::OutOfRange { key: "version", .. })));
    }

    #[test]
    fn compile_bad_color() {
        let t = "version = 1\nis_unlocked = 0\ncolor = 9\nsystem_version = 0\nsystem_spl = 0\n\
                 rot_digest = \"1111111111111111111111111111111111111111111111111111111111111111\"\n\
                 pubkey_digest = \"2222222222222222222222222222222222222222222222222222222222222222\"\n\
                 vbh = \"3333333333333333333333333333333333333333333333333333333333333333\"\n";
        assert!(matches!(compile(t), Err(CompileError::OutOfRange { key: "color", .. })));
    }

    #[test]
    fn compile_bad_digest() {
        // 2 hex chars, not 64
        let t = "version = 1\nis_unlocked = 0\ncolor = 0\nsystem_version = 0\nsystem_spl = 0\n\
                 rot_digest = \"11\"\npubkey_digest = \"22\"\nvbh = \"33\"\n";
        assert!(matches!(compile(t), Err(CompileError::BadDigest(_))));
    }

    #[test]
    fn compile_unknown_key() {
        let t = "version = 1\nis_unlocked = 0\ncolor = 0\nsystem_version = 0\nsystem_spl = 0\n\
                 rot_digest = \"1111111111111111111111111111111111111111111111111111111111111111\"\n\
                 pubkey_digest = \"2222222222222222222222222222222222222222222222222222222222222222\"\n\
                 vbh = \"3333333333333333333333333333333333333333333333333333333333333333\"\n\
                 unknownkey = 1\n";
        assert!(matches!(compile(t), Err(CompileError::UnknownKey(_))));
    }

    #[test]
    fn encode_os_version_matches_python() {
        // Python: "16.0.7" -> (16<<14)|(0<<7)|7 = 0x40007
        assert_eq!(encode_os_version("16.0.7").unwrap(), (16 << 14) | (0 << 7) | 7);
        // Single-component: "16" -> minor=0 sub=0
        assert_eq!(encode_os_version("16").unwrap(), 16 << 14);
        // Boundary: 0.127.127
        assert_eq!(encode_os_version("0.127.127").unwrap(), (127 << 7) | 127);
        // Out of range
        assert_eq!(encode_os_version("16.128.0"), Err(DeriveError::OsVersionOutOfRange));
    }

    #[test]
    fn encode_spl_matches_python() {
        // Python: "2026-05-01" -> day=1<<11 | (year-2000=26)<<4 | month=5
        //                       = 0x800 | 0x1A0 | 0x5 = 0x9A5
        assert_eq!(encode_spl("2026-05-01").unwrap(), (1 << 11) | (26 << 4) | 5);
        // Out of range — year
        assert_eq!(encode_spl("1999-01-01"), Err(DeriveError::SplOutOfRange));
        // Out of range — month
        assert_eq!(encode_spl("2026-13-01"), Err(DeriveError::SplOutOfRange));
        // Malformed
        assert_eq!(encode_spl("notadate"), Err(DeriveError::SplMalformed));
    }
}
