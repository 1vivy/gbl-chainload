//! Parity tests against PR1's 9-case manifest matrix
//! (`tests/host/helpers/test_manifest_parse.c`), plus cached_abl
//! scan + mode2 lookup happy-path coverage.

use gblp1::*;

// --- minimal container builder ---------------------------------------
//
// Mirrors the C `make_container` in test_manifest_parse.c: builds a
// single-entry GBLP1 container around a caller-supplied payload, with
// the entry size deliberately decoupled from the buffer size so we can
// exercise the BadSize path.

fn le16(p: &mut [u8], v: u16) {
    p[0] = v as u8;
    p[1] = (v >> 8) as u8;
}
fn le32(p: &mut [u8], v: u32) {
    p[0] = v as u8;
    p[1] = (v >> 8) as u8;
    p[2] = (v >> 16) as u8;
    p[3] = (v >> 24) as u8;
}

fn make_container(payload: &[u8], entry_size: usize, entry_type: u16) -> Vec<u8> {
    let entries_end = GBLP1_HEADER_SIZE + GBLP1_ENTRY_SIZE;
    let off = (entries_end + GBLP1_PAYLOAD_ALIGN - 1) & !(GBLP1_PAYLOAD_ALIGN - 1);
    let mut total = off + entry_size + GBLP1_FOOTER_SIZE;
    total = (total + GBLP1_PAYLOAD_ALIGN - 1) & !(GBLP1_PAYLOAD_ALIGN - 1);

    let mut buf = vec![0u8; total];
    buf[0..GBLP1_MAGIC_SIZE].copy_from_slice(&GBLP1_MAGIC[..]);
    le16(&mut buf[8..10], GBLP1_VERSION);
    le16(&mut buf[10..12], GBLP1_HEADER_SIZE as u16);
    le32(&mut buf[12..16], GBLP1_FLAGS_LE);
    le32(&mut buf[16..20], total as u32);
    le32(&mut buf[20..24], 1);

    let e = GBLP1_HEADER_SIZE;
    le16(&mut buf[e..e + 2], entry_type);
    le16(&mut buf[e + 2..e + 4], 0); // flags
    le32(&mut buf[e + 4..e + 8], off as u32);
    le32(&mut buf[e + 8..e + 12], entry_size as u32);
    le32(&mut buf[e + 12..e + 16], 0); // reserved

    let copy = payload.len().min(entry_size);
    if copy > 0 {
        buf[off..off + copy].copy_from_slice(&payload[..copy]);
    }
    // SHA over the recorded entry_size, even if the buffer it points
    // to is shorter — matches the C test's behavior.
    let sha = sha256(&buf[off..off + entry_size]);
    buf[e + 16..e + 48].copy_from_slice(&sha);

    buf[total - GBLP1_FOOTER_SIZE..total].copy_from_slice(&GBLP1_FOOTER[..]);
    let crc = crc32(&buf[0..24]);
    le32(&mut buf[24..28], crc);
    buf
}

fn make_manifest_payload(
    cap_bits: u16,
    schema: u16,
    bad_magic: bool,
    bad_pad: bool,
) -> [u8; 16] {
    let mut out = [0u8; 16];
    out[0..4].copy_from_slice(if bad_magic { b"BAD!" } else { GBLP1_MANIFEST_MAGIC });
    le16(&mut out[4..6], schema);
    le16(&mut out[6..8], cap_bits);
    if bad_pad {
        out[15] = 0xff;
    }
    out
}

// --- 9-case manifest matrix (ported from test_manifest_parse.c) ------

#[test]
fn manifest_mode_0() {
    let pl = make_manifest_payload(0x0000, 1, false, false);
    let b = make_container(&pl, 16, GBLP1_TYPE_MANIFEST);
    let c = parse(&b).expect("container parses");
    let m = c.find_manifest().expect("manifest valid");
    assert_eq!(m.unwrap().cap_bits, 0x0000);
}

#[test]
fn manifest_mode_1() {
    let pl = make_manifest_payload(0x0001, 1, false, false);
    let b = make_container(&pl, 16, GBLP1_TYPE_MANIFEST);
    let c = parse(&b).unwrap();
    let m = c.find_manifest().unwrap().unwrap();
    assert_eq!(m.cap_bits, 0x0001);
}

#[test]
fn manifest_mode_2() {
    let pl = make_manifest_payload(0x0002, 1, false, false);
    let b = make_container(&pl, 16, GBLP1_TYPE_MANIFEST);
    let c = parse(&b).unwrap();
    let m = c.find_manifest().unwrap().unwrap();
    assert_eq!(m.cap_bits, 0x0002);
}

#[test]
fn manifest_bad_magic() {
    let pl = make_manifest_payload(0x0000, 1, true, false);
    let b = make_container(&pl, 16, GBLP1_TYPE_MANIFEST);
    let c = parse(&b).unwrap();
    assert_eq!(c.find_manifest(), Err(ManifestError::BadMagic));
}

#[test]
fn manifest_bad_schema() {
    let pl = make_manifest_payload(0x0000, 2, false, false);
    let b = make_container(&pl, 16, GBLP1_TYPE_MANIFEST);
    let c = parse(&b).unwrap();
    assert_eq!(c.find_manifest(), Err(ManifestError::BadSchema));
}

#[test]
fn manifest_bad_reserved_bit() {
    let pl = make_manifest_payload(0x0004, 1, false, false);
    let b = make_container(&pl, 16, GBLP1_TYPE_MANIFEST);
    let c = parse(&b).unwrap();
    assert_eq!(c.find_manifest(), Err(ManifestError::BadReserved));
}

#[test]
fn manifest_bad_reserved_pad() {
    let pl = make_manifest_payload(0x0000, 1, false, true);
    let b = make_container(&pl, 16, GBLP1_TYPE_MANIFEST);
    let c = parse(&b).unwrap();
    assert_eq!(c.find_manifest(), Err(ManifestError::BadReserved));
}

#[test]
fn manifest_absent_in_container_with_other_entry() {
    // Container with a single cached_abl entry; manifest lookup must
    // return `Ok(None)`.
    let pl = [0u8; 16];
    let b = make_container(&pl, 16, GBLP1_TYPE_CACHED_ABL);
    let c = parse(&b).unwrap();
    assert_eq!(c.find_manifest(), Ok(None));
}

#[test]
fn manifest_bad_size() {
    // Entry recorded as 15 bytes (spec requires 16).
    let pl = make_manifest_payload(0x0000, 1, false, false);
    let b = make_container(&pl, 15, GBLP1_TYPE_MANIFEST);
    let c = parse(&b).unwrap();
    assert_eq!(c.find_manifest(), Err(ManifestError::BadSize));
}

// --- FFI parity (manifest path mirrors the C signature) --------------

#[test]
fn ffi_find_manifest_signals_absence_via_present_flag() {
    let pl = [0u8; 16];
    let b = make_container(&pl, 16, GBLP1_TYPE_CACHED_ABL);
    let mut wire = gblp1::ffi::GblManifestWire { cap_bits: 0xDEAD };
    let mut present: core::ffi::c_int = -1;
    let st = unsafe {
        gblp1::ffi::gbl_payload_find_manifest(
            b.as_ptr(),
            b.len(),
            &mut wire,
            &mut present,
        )
    };
    assert_eq!(st, gblp1::ffi::GblPayloadStatus::Ok);
    assert_eq!(present, 0);
}

#[test]
fn ffi_find_manifest_populates_caps_when_present() {
    let pl = make_manifest_payload(0x0003, 1, false, false);
    let b = make_container(&pl, 16, GBLP1_TYPE_MANIFEST);
    let mut wire = gblp1::ffi::GblManifestWire { cap_bits: 0xDEAD };
    let mut present: core::ffi::c_int = -1;
    let st = unsafe {
        gblp1::ffi::gbl_payload_find_manifest(
            b.as_ptr(),
            b.len(),
            &mut wire,
            &mut present,
        )
    };
    assert_eq!(st, gblp1::ffi::GblPayloadStatus::Ok);
    assert_eq!(present, 1);
    assert_eq!(wire.cap_bits, 0x0003);
}

// --- cached_abl scan / find ------------------------------------------

#[test]
fn cached_abl_round_trip_via_pack_then_parse() {
    // Build a fake PE-shaped payload (sanity isn't checked in this
    // crate). Round-trip through pack() and verify the parser finds
    // the same bytes.
    let pe = vec![0xABu8; 1024];
    let inputs = PackInputs {
        cached_abl: Some(&pe),
        packer_version: Some("gblp1-test 0.1"),
        timestamp_iso8601: Some("2026-05-23T00:00:00Z"),
        ..Default::default()
    };
    let buf = pack(&inputs).expect("pack");
    let c = parse(&buf).expect("parse");
    let got = c.find_cached_abl().unwrap();
    assert_eq!(got, &pe[..]);
}

#[test]
fn scan_tolerates_stray_magic_prefix() {
    // Build a valid container with a stray copy of GBLP1\0\0\0 in front
    // of it. scan_for_container must skip past the stray and land on
    // the real container.
    let pe = vec![0x42u8; 512];
    let inputs = PackInputs {
        cached_abl: Some(&pe),
        ..Default::default()
    };
    let mut buf = pack(&inputs).unwrap();
    let mut prefixed = Vec::new();
    prefixed.extend_from_slice(b"GBLP1\0\0\0junk");
    prefixed.append(&mut buf);
    let c = scan_for_container(&prefixed).expect("scan finds real container");
    assert_eq!(c.find_cached_abl().unwrap(), &pe[..]);
}

#[test]
fn scan_returns_none_when_magic_absent() {
    let junk = vec![0u8; 4096];
    assert!(scan_for_container(&junk).is_none());
}

#[test]
fn ffi_scan_cached_abl_returns_bad_magic_when_absent() {
    let junk = vec![0u8; 4096];
    let mut pe: *const u8 = core::ptr::null();
    let mut pe_size: usize = 0;
    let st = unsafe {
        gblp1::ffi::gbl_payload_scan_cached_abl(
            junk.as_ptr(),
            junk.len(),
            &mut pe,
            &mut pe_size,
        )
    };
    assert_eq!(st, gblp1::ffi::GblPayloadStatus::BadMagic);
}

// --- mode2_profile lookup --------------------------------------------

#[test]
fn mode2_profile_present_and_absent() {
    // Synthesize a 120-byte GM2P-prefixed blob to satisfy the packer's
    // structural check on the profile entry. 120 is GBL_M2P_SIZE — the
    // wire size of the mode-2 profile struct (see tools/shared/gbl_mode2_profile.h
    // and `mode2_profile_core`). Task 8 corrected this constant in
    // lib.rs from 256 to 120; Task 10 brought this stale test fixture
    // into alignment with that fix.
    let mut profile = vec![0u8; 120];
    profile[0..4].copy_from_slice(b"GM2P");
    let pe = vec![0xCDu8; 256];
    let inputs = PackInputs {
        cached_abl: Some(&pe),
        mode2_profile: Some(&profile),
        ..Default::default()
    };
    let buf = pack(&inputs).unwrap();
    let c = parse(&buf).unwrap();
    assert_eq!(c.find_mode2_profile().unwrap(), &profile[..]);

    // Same payload, no profile this time.
    let inputs2 = PackInputs {
        cached_abl: Some(&pe),
        ..Default::default()
    };
    let buf2 = pack(&inputs2).unwrap();
    let c2 = parse(&buf2).unwrap();
    assert!(c2.find_mode2_profile().is_none());
}

// --- streaming SHA via the FFI ---------------------------------------

#[test]
fn streaming_sha_matches_single_shot() {
    use gblp1::ffi::*;
    let data: Vec<u8> = (0..1024u32).map(|i| i as u8).collect();
    let want = sha256(&data);

    let mut ctx = GblSha256Ctx {
        bytes: [0u8; GBL_SHA256_CTX_SIZE],
    };
    let mut got = [0u8; 32];
    unsafe {
        gbl_sha256_init(&mut ctx);
        gbl_sha256_update(&mut ctx, data.as_ptr(), 100);
        gbl_sha256_update(&mut ctx, data.as_ptr().add(100), 924);
        gbl_sha256_final(&mut ctx, got.as_mut_ptr());
    }
    assert_eq!(got, want);
}
