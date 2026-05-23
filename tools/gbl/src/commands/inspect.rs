//! `gbl inspect` — GBLP1 container inspector.
//!
//! Replaces `tools/gblp1-inspect/gblp1-inspect.c` byte-for-byte. Same
//! find-container logic (skip embedded-string false-matches, distinguish
//! truncated/bad-CRC from "not_a_gblp1"), same line shapes, same exit
//! semantics. Includes the PR1 Task 4 manifest pretty-print.

use std::path::PathBuf;

use clap::Parser;

use super::{slurp, GblError};

#[derive(Parser, Debug)]
#[command(about = "Inspect a GBLP1 v1 container")]
pub struct Args {
    /// Container image (base EFI prefix allowed; scanner finds the
    /// embedded GBLP1).
    image: PathBuf,
}

pub fn run(args: Args) -> Result<(), GblError> {
    let buf = slurp(&args.image)?;
    match find_container(&buf) {
        FindResult::NoCandidate => {
            println!("result: not_a_gblp1");
            Err(GblError::silent(1))
        }
        FindResult::Corrupt { reason, .. } => {
            println!("result: {reason}");
            Err(GblError::silent(1))
        }
        FindResult::Ok { offset } => emit_valid(&buf, offset),
    }
}

enum FindResult {
    Ok { offset: usize },
    /// `offset` is kept for diagnostics symmetry with the C tool even
    /// though our printer only consumes `reason`; suppress the dead-code
    /// warning since the field exists for future use.
    Corrupt {
        #[allow(dead_code)]
        offset: usize,
        reason: &'static str,
    },
    NoCandidate,
}

/// Mirror find_container() in gblp1-inspect.c:
///   * Plausible = magic match + version/header_size/flags v1 values.
///   * Corrupt   = plausible but total_size or CRC invalid.
///   * NoCandidate = nothing matched even the basic shape check.
fn find_container(buf: &[u8]) -> FindResult {
    if buf.len() < gblp1::GBLP1_HEADER_SIZE {
        return FindResult::NoCandidate;
    }
    let mut best_corrupt: Option<(usize, &'static str)> = None;
    let mut i = 0usize;
    while i + gblp1::GBLP1_HEADER_SIZE <= buf.len() {
        if buf[i..i + gblp1::GBLP1_MAGIC_SIZE] != gblp1::GBLP1_MAGIC[..] {
            i += 1;
            continue;
        }
        let h = &buf[i..];
        let version = u16::from_le_bytes([h[8], h[9]]);
        let hdr_size = u16::from_le_bytes([h[10], h[11]]);
        let flags = u32::from_le_bytes([h[12], h[13], h[14], h[15]]);
        if version != gblp1::GBLP1_VERSION
            || hdr_size as usize != gblp1::GBLP1_HEADER_SIZE
            || flags & gblp1::GBLP1_FLAGS_LE == 0
        {
            i += 1;
            continue;
        }
        let total_size =
            u32::from_le_bytes([h[16], h[17], h[18], h[19]]) as usize;
        let hdr_crc = u32::from_le_bytes([h[24], h[25], h[26], h[27]]);
        let avail = buf.len() - i;
        if total_size > gblp1::GBLP1_TOTAL_SIZE_CAP
            || total_size > avail
            || total_size < gblp1::GBLP1_HEADER_SIZE + gblp1::GBLP1_FOOTER_SIZE
        {
            if best_corrupt.is_none() {
                best_corrupt = Some((i, "truncated"));
            }
            i += 1;
            continue;
        }
        if gblp1::crc32(&h[..24]) != hdr_crc {
            if best_corrupt.is_none() {
                best_corrupt = Some((i, "bad_crc"));
            }
            i += 1;
            continue;
        }
        return FindResult::Ok { offset: i };
    }
    match best_corrupt {
        Some((offset, reason)) => FindResult::Corrupt { offset, reason },
        None => FindResult::NoCandidate,
    }
}

fn type_name(t: u16) -> &'static str {
    match t {
        gblp1::GBLP1_TYPE_CACHED_ABL => "CACHED_ABL",
        gblp1::GBLP1_TYPE_SOURCE_META => "SOURCE_META",
        gblp1::GBLP1_TYPE_MODE2_PROFILE => "MODE2_PROFILE",
        gblp1::GBLP1_TYPE_MANIFEST => "manifest",
        _ => "UNKNOWN",
    }
}

fn emit_valid(buf: &[u8], mo: usize) -> Result<(), GblError> {
    let h = &buf[mo..];
    let version = u16::from_le_bytes([h[8], h[9]]);
    let total_size =
        u32::from_le_bytes([h[16], h[17], h[18], h[19]]);
    let entry_count =
        u32::from_le_bytes([h[20], h[21], h[22], h[23]]);
    println!(
        "header: magic=ok version={} header_crc32=ok total_size={} entry_count={}",
        version, total_size, entry_count
    );

    // Overflow-safe entries-end + footer fit-in-total check.
    let entries_end = (gblp1::GBLP1_HEADER_SIZE as u64)
        + (entry_count as u64) * (gblp1::GBLP1_ENTRY_SIZE as u64);
    if entries_end + gblp1::GBLP1_FOOTER_SIZE as u64 > total_size as u64 {
        println!("result: truncated");
        return Err(GblError::silent(1));
    }

    let mut sha_fail = false;
    for i in 0..entry_count as usize {
        let e = &h[gblp1::GBLP1_HEADER_SIZE + i * gblp1::GBLP1_ENTRY_SIZE..];
        let t = u16::from_le_bytes([e[0], e[1]]);
        let poff = u32::from_le_bytes([e[4], e[5], e[6], e[7]]);
        let psize = u32::from_le_bytes([e[8], e[9], e[10], e[11]]);
        let want_sha = &e[16..48];

        if psize as u64 > total_size as u64
            || poff as u64 > (total_size as u64).saturating_sub(psize as u64)
        {
            println!(
                "entry: type=0x{:04x} ({}) offset=0x{:x} size={} sha256=OUT_OF_BOUNDS",
                t,
                type_name(t),
                poff,
                psize
            );
            sha_fail = true;
            continue;
        }
        let payload = &h[poff as usize..(poff as usize + psize as usize)];
        let got = gblp1::sha256(payload);
        let ok = got[..] == *want_sha;
        println!(
            "entry: type=0x{:04x} ({}) offset=0x{:x} size={} sha256={}",
            t,
            type_name(t),
            poff,
            psize,
            if ok { "ok" } else { "MISMATCH" }
        );
        if !ok {
            sha_fail = true;
        }
        if t == gblp1::GBLP1_TYPE_MANIFEST {
            print_manifest_payload(payload);
        }
    }

    let foot_start = total_size as usize - gblp1::GBLP1_FOOTER_SIZE;
    let footer_ok =
        h[foot_start..total_size as usize] == gblp1::GBLP1_FOOTER[..];
    println!(
        "footer: GBLP1END={}",
        if footer_ok { "ok" } else { "MISSING" }
    );

    if !footer_ok {
        println!("result: truncated");
        return Err(GblError::silent(1));
    }
    if sha_fail {
        println!("result: entry_sha_mismatch");
        return Err(GblError::silent(1));
    }
    println!("result: ok");
    Ok(())
}

/// Pretty-print a GBLP1_TYPE_MANIFEST (0x0020) payload — mirrors the
/// print_manifest_payload() helper from gblp1-inspect.c.
fn print_manifest_payload(p: &[u8]) {
    if p.len() != gblp1::GBLP1_MANIFEST_SIZE {
        println!(
            "  manifest: bad size={} (expected {})",
            p.len(),
            gblp1::GBLP1_MANIFEST_SIZE
        );
        return;
    }
    let magic_bytes = &p[..gblp1::GBLP1_MANIFEST_MAGIC_SIZE];
    let magic_ok = magic_bytes == gblp1::GBLP1_MANIFEST_MAGIC.as_slice();
    let magic_str: String = if magic_ok {
        std::str::from_utf8(magic_bytes).unwrap_or("BAD").to_string()
    } else {
        "BAD".to_string()
    };
    let schema = u16::from_le_bytes([p[4], p[5]]);
    let bits = u16::from_le_bytes([p[6], p[7]]);
    let fakelock = if bits & gblp1::GBLP1_MANIFEST_BIT_FAKELOCK_HOOK != 0 {
        "yes"
    } else {
        "no"
    };
    let spoof = if bits & gblp1::GBLP1_MANIFEST_BIT_PROFILE_SPOOF != 0 {
        "yes"
    } else {
        "no"
    };
    println!(
        "  magic={magic_str} schema={schema} fakelock_hook={fakelock} profile_spoof={spoof}"
    );
}
