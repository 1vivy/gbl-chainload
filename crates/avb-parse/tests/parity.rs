//! Parity tests — Rust avb-parse outputs match the C reference on
//! tracked real-device fixtures.
//!
//! Inputs:
//!   - `tests/images/vbmeta-infiniti-IN-16.0.7.201.img` — stock OEM
//!     vbmeta partition dump from infiniti (OnePlus 15).
//!   - `tests/images/grafted-recovery.img` — post-graft recovery
//!     partition with an embedded vbmeta blob (footer'd image).
//!
//! Goldens for the host C-tool surface (074 / 091 list output, 090
//! list-hash output) live under `tests/host/goldens/{074,090,091}/`
//! and are enforced by the host shell tests. This file pins the
//! invariants the Rust API surface promises — descriptor count + types
//! + bounds — on the same fixtures.

use std::fs;
use std::path::PathBuf;

use avb_parse::{
    chain_verdict, parse_footer, parse_footer_from_tail, parse_vbmeta, ChainVerdict, Descriptor,
    DescriptorTag, FOOTER_SIZE,
};

fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR = crates/avb-parse/. Climb two levels.
    let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    p.pop();
    p.pop();
    p
}

fn read_fixture(rel: &str) -> Option<Vec<u8>> {
    let p = repo_root().join(rel);
    if !p.exists() {
        return None;
    }
    Some(fs::read(&p).expect("read fixture"))
}

// --- infiniti vbmeta partition (bare vbmeta blob) --------------------

#[test]
fn infiniti_vbmeta_parses() {
    let bytes = match read_fixture("tests/images/vbmeta-infiniti-IN-16.0.7.201.img") {
        Some(b) => b,
        None => {
            eprintln!("SKIP: infiniti vbmeta fixture missing");
            return;
        }
    };
    let v = parse_vbmeta(&bytes).expect("vbmeta header parses");
    assert_eq!(v.header.major_version, 1, "AVB major version mismatch");
    // Aux block must fit inside the blob.
    assert!(
        v.aux_offset() + v.aux_size() <= bytes.len() as u64,
        "aux block escapes buffer"
    );
    // Embedded pubkey must lie inside the aux block.
    let pk = v.public_key().expect("pubkey within aux");
    assert!(!pk.is_empty(), "no embedded public key");
}

#[test]
fn infiniti_vbmeta_descriptor_set() {
    let bytes = match read_fixture("tests/images/vbmeta-infiniti-IN-16.0.7.201.img") {
        Some(b) => b,
        None => {
            eprintln!("SKIP: infiniti vbmeta fixture missing");
            return;
        }
    };
    let v = parse_vbmeta(&bytes).unwrap();

    let mut hash_count = 0usize;
    let mut chain_count = 0usize;
    let mut prop_count = 0usize;
    let mut other_count = 0usize;
    for d in v.descriptors() {
        let d = d.expect("descriptor walk fail-closed");
        match d {
            Descriptor::Hash(h) => {
                hash_count += 1;
                // Sanity-check name bytes are printable ASCII — protects
                // against an off-by-one in body offset math.
                assert!(h.partition_name.iter().all(|&b| b.is_ascii_graphic()));
                // Digest length is either 32 (sha256) or 64 (sha512).
                assert!(h.digest.len() == 32 || h.digest.len() == 64);
            }
            Descriptor::ChainPartition(c) => {
                chain_count += 1;
                assert!(c.partition_name.iter().all(|&b| b.is_ascii_graphic()));
                assert!(!c.public_key.is_empty());
            }
            Descriptor::Property { .. } => prop_count += 1,
            _ => other_count += 1,
        }
    }
    // Real infiniti vbmeta has at least one chain (for boot) + some
    // hash/property descriptors. Pin loosely — exact counts depend on
    // build version. The 091 golden locks the C tool's textual output.
    assert!(
        hash_count > 0 || chain_count > 0,
        "no hash or chain descriptors — vbmeta looks empty"
    );
    let _ = (prop_count, other_count); // silence unused if all zero
}

#[test]
fn infiniti_vbmeta_chain_verdict_unkeyed() {
    // Chain verdict on the vbmeta blob with no chain key passed — any
    // parseable vbmeta should land in `Ok`.
    let bytes = match read_fixture("tests/images/vbmeta-infiniti-IN-16.0.7.201.img") {
        Some(b) => b,
        None => {
            eprintln!("SKIP: infiniti vbmeta fixture missing");
            return;
        }
    };
    assert_eq!(chain_verdict(&bytes, None), ChainVerdict::Ok);
}

// --- grafted-recovery: footer'd partition with embedded vbmeta ------

#[test]
fn grafted_recovery_footer() {
    let bytes = match read_fixture("tests/images/grafted-recovery.img") {
        Some(b) => b,
        None => {
            eprintln!("SKIP: grafted-recovery fixture missing");
            return;
        }
    };
    let f = parse_footer(&bytes).expect("footer parses");
    assert!(f.vbmeta_offset > 0);
    assert!(f.vbmeta_size > 0);
    assert!(f.vbmeta_offset + f.vbmeta_size <= bytes.len() as u64);
    assert!(f.original_image_size > 0);
    assert!(f.original_image_size <= bytes.len() as u64);
}

#[test]
fn grafted_recovery_embedded_vbmeta_descriptors() {
    let bytes = match read_fixture("tests/images/grafted-recovery.img") {
        Some(b) => b,
        None => {
            eprintln!("SKIP: grafted-recovery fixture missing");
            return;
        }
    };
    let f = parse_footer(&bytes).unwrap();
    let off = f.vbmeta_offset as usize;
    let end = off + f.vbmeta_size as usize;
    let vb = &bytes[off..end];

    let v = parse_vbmeta(vb).expect("embedded vbmeta parses");

    // The 074 list golden documents this fixture's embedded set:
    //   partition=recovery type=hash graftable=yes
    //   descriptor type=other
    //
    // i.e. one hash descriptor named "recovery" and one descriptor
    // outside the (hash, chain, hashtree) printable set in the C tool
    // — confirm by counting types.
    let mut saw_recovery_hash = false;
    let mut total = 0usize;
    for d in v.descriptors() {
        let d = d.expect("descriptor walk fail-closed");
        total += 1;
        if let Descriptor::Hash(h) = d {
            if h.partition_name == b"recovery" {
                saw_recovery_hash = true;
            }
        }
    }
    assert!(saw_recovery_hash, "expected hash descriptor for 'recovery'");
    assert!(total >= 1);
}

#[test]
fn grafted_recovery_footer_from_tail() {
    let bytes = match read_fixture("tests/images/grafted-recovery.img") {
        Some(b) => b,
        None => {
            eprintln!("SKIP: grafted-recovery fixture missing");
            return;
        }
    };
    // Hand the parser just the last 4 KiB — like the firmware would,
    // reading a single block off the partition end.
    let tail_len = 4096usize.min(bytes.len());
    let tail = &bytes[bytes.len() - tail_len..];
    let f = parse_footer_from_tail(tail, bytes.len() as u64).expect("tail decode");
    let f_full = parse_footer(&bytes).expect("full decode");
    assert_eq!(f.vbmeta_offset, f_full.vbmeta_offset);
    assert_eq!(f.vbmeta_size, f_full.vbmeta_size);
    assert_eq!(f.original_image_size, f_full.original_image_size);
}

#[test]
fn no_footer_returns_not_found_on_random_buffer() {
    let v: Vec<u8> = (0..1024).map(|i| (i as u8) ^ 0xa5).collect();
    // Random data is statistically certain not to have AVBf at the tail.
    let r = parse_footer(&v);
    assert!(
        r.is_err(),
        "random buffer should not parse as a footer'd partition"
    );
}

#[test]
fn footer_constants_match_c_header() {
    // Lock the on-disk size constants.
    assert_eq!(FOOTER_SIZE, 64);
    assert_eq!(avb_parse::VBMETA_HEADER_SIZE, 256);
    assert_eq!(avb_parse::FOOTER_MAGIC, b"AVBf");
    assert_eq!(avb_parse::VBMETA_MAGIC, b"AVB0");
    // Sanity-check descriptor tag wire values one more time.
    assert_eq!(DescriptorTag::Hash as u64, 2);
    assert_eq!(DescriptorTag::ChainPartition as u64, 4);
}
