//! `gbl pack` — build a GBLP1 v1 container.
//!
//! Replaces `tools/gbl-pack/gbl-pack.c` + `pack.c`. Post-PR1 contract:
//!
//! - `--manifest <bits>` accepts `0x..` / `0..` / decimal. Reserved bits
//!   are rejected with the documented error string before the container is
//!   constructed, mirroring gbl-pack.c.
//! - `SOURCE_DATE_EPOCH` is honored for reproducible timestamps (the
//!   goldens captured against the C tool pin to it).
//! - `cached_abl` still triggers a PE sanity hard-reject and an
//!   informational warning if the UTF-16 "efisp" marker is still present.
//! - Output path is byte-for-byte identical to the C tool — `gblp1::pack`
//!   is the producer.

use std::path::PathBuf;
use std::time::{SystemTime, UNIX_EPOCH};

use clap::Parser;

use super::{slurp, GblError};

#[derive(Parser, Debug)]
#[command(about = "Pack a GBLP1 v1 container")]
pub struct Args {
    /// Patched cached ABL PE (output of `gbl patch`).
    #[arg(long)]
    cached_abl: Option<PathBuf>,
    /// Original ABL partition image (for source_meta).
    #[arg(long)]
    source: Option<PathBuf>,
    /// Unpatched PE (output of `gbl unwrap`) for source_meta.
    #[arg(long)]
    extracted: Option<PathBuf>,
    /// Optional mode-2 profile binary (type 0x0010 entry).
    #[arg(long)]
    mode2_profile: Option<PathBuf>,
    /// Optional manifest entry capability bits (type 0x0020).
    #[arg(long, value_name = "BITS")]
    manifest: Option<String>,
    /// Output container path.
    #[arg(long)]
    out: PathBuf,
}

pub fn run(args: Args) -> Result<(), GblError> {
    if args.cached_abl.is_none()
        && args.mode2_profile.is_none()
        && args.manifest.is_none()
    {
        return Err(GblError::usage(
            "usage: gbl pack --out OUT [--cached-abl PE --source RAW --extracted PE] \
             [--mode2-profile BIN] [--manifest BITS]"
                .to_string(),
        ));
    }
    if args.cached_abl.is_some() && (args.source.is_none() || args.extracted.is_none())
    {
        return Err(GblError::usage(
            "gbl-pack: --cached-abl requires --source and --extracted".to_string(),
        ));
    }

    // --manifest argument parsing — must accept 0x.., 0.., decimal, and
    // reject reserved bits with the documented string.
    let manifest_bits = match &args.manifest {
        None => None,
        Some(s) => Some(parse_manifest_bits(s)?),
    };

    // Slurp every input file (Vec<u8> owns the bytes for the lifetime of
    // the call so the slices we pass into PackInputs stay valid).
    let cached = args.cached_abl.as_deref().map(slurp).transpose()?;
    let source = args.source.as_deref().map(slurp).transpose()?;
    let extracted = args.extracted.as_deref().map(slurp).transpose()?;
    let profile = args.mode2_profile.as_deref().map(slurp).transpose()?;

    // PE sanity hard-reject for cached_abl (the C tool calls
    // gbl_pe_sanity() in pack.c). Note: the firmware's runtime
    // gBS->LoadImage is the operational authority — this catches
    // genuinely malformed inputs that would never boot.
    if let Some(c) = cached.as_deref() {
        pe_utils::pe_sanity(c).map_err(|e| {
            GblError::runtime(format!(
                "gbl-pack: PE sanity failed for cached_abl ({:?})",
                e
            ))
        })?;
        // efisp UTF-16 marker -> informational warning (parity with the C
        // tool's warning path; BlockIoHook is the operational guarantee).
        if pe_utils::efisp_marker_present(c) {
            eprintln!(
                "gbl-pack: warning: cached_abl still contains UTF-16 \"efisp\" \
                 — BlockIoHook gate will handle this, but check that \
                 patch10/patch6 applied as expected"
            );
        }
    }

    // Timestamp string — honor SOURCE_DATE_EPOCH (reproducible-builds).
    let ts = iso8601_utc_now()?;
    // The packer_version string is part of the SOURCE_META entry — its
    // bytes go into the SHA-256 + on-disk blob. To keep the goldens
    // captured against the C tool valid we MUST emit exactly
    // "gbl-pack <VERSION>" with the project-wide VERSION file. Cargo's
    // CARGO_PKG_VERSION is the crate's "0.1.0", not the project's
    // "2.2.2".
    const PROJECT_VERSION: &str =
        include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../VERSION"));
    let packer_version = format!("gbl-pack {}", PROJECT_VERSION.trim());

    let inputs = gblp1::PackInputs {
        cached_abl: cached.as_deref(),
        source: source.as_deref(),
        extracted: extracted.as_deref(),
        mode2_profile: profile.as_deref(),
        manifest_cap_bits: manifest_bits,
        packer_version: Some(&packer_version),
        timestamp_iso8601: Some(&ts),
    };

    let buf = gblp1::pack(&inputs).map_err(|e| {
        GblError::runtime(format!("gbl-pack: pack error: {:?}", e))
    })?;

    std::fs::write(&args.out, &buf).map_err(|e| {
        GblError::runtime(format!("{}: write failed: {}", args.out.display(), e))
    })?;
    eprintln!(
        "gbl-pack: wrote {} ({} bytes)",
        args.out.display(),
        buf.len()
    );
    Ok(())
}

/// Parse `--manifest BITS` — accepts 0x.., 0.., decimal; rejects reserved
/// bits. Error messages are byte-identical to gbl-pack.c so existing
/// goldens / tests keep matching.
fn parse_manifest_bits(s: &str) -> Result<u16, GblError> {
    let n: u64 = parse_unsigned(s).map_err(|_| {
        GblError::usage("gbl-pack: bad --manifest bits (not a number)".to_string())
    })?;
    if n > 0xFFFF {
        return Err(GblError::usage(
            "gbl-pack: bad --manifest bits (must fit in 16 bits)".to_string(),
        ));
    }
    let bits = n as u16;
    if bits & gblp1::GBLP1_MANIFEST_BITS_RESERVED_MASK != 0 {
        return Err(GblError::usage(
            "gbl-pack: bad --manifest bits (reserved bits set)".to_string(),
        ));
    }
    Ok(bits)
}

/// Mirror C `strtoul(s, &end, 0)` semantics: leading `0x`/`0X` → hex,
/// leading `0` → octal, otherwise decimal. Rejects empty / non-numeric.
fn parse_unsigned(s: &str) -> Result<u64, ()> {
    let s = s.trim();
    if s.is_empty() {
        return Err(());
    }
    let (radix, digits) = if let Some(stripped) = s.strip_prefix("0x") {
        (16, stripped)
    } else if let Some(stripped) = s.strip_prefix("0X") {
        (16, stripped)
    } else if let Some(stripped) = s.strip_prefix('0') {
        // Match strtoul-with-base-0: "0" alone is decimal 0; "0123" is octal.
        if stripped.is_empty() {
            return Ok(0);
        } else {
            (8, stripped)
        }
    } else {
        (10, s)
    };
    if digits.is_empty() {
        return Err(());
    }
    u64::from_str_radix(digits, radix).map_err(|_| ())
}

/// `strftime("%Y-%m-%dT%H:%M:%SZ", gmtime_r(&now, ...))` equivalent.
/// Honors `SOURCE_DATE_EPOCH` (reproducible-builds.org convention) so
/// the goldens pin to a fixed timestamp.
fn iso8601_utc_now() -> Result<String, GblError> {
    let secs: i64 = match std::env::var("SOURCE_DATE_EPOCH") {
        Ok(s) => {
            let trimmed = s.trim();
            trimmed.parse::<i64>().map_err(|_| {
                GblError::usage(
                    "gbl-pack: bad SOURCE_DATE_EPOCH (not a non-negative integer)"
                        .to_string(),
                )
            })?
        }
        Err(_) => {
            let now = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_err(|e| GblError::runtime(format!("clock error: {e}")))?;
            now.as_secs() as i64
        }
    };
    if secs < 0 {
        return Err(GblError::usage(
            "gbl-pack: bad SOURCE_DATE_EPOCH (not a non-negative integer)"
                .to_string(),
        ));
    }
    Ok(format_iso8601_utc(secs as u64))
}

/// Format a Unix-epoch seconds count as `YYYY-MM-DDTHH:MM:SSZ`. Pure-Rust
/// gmtime equivalent — no extra deps. Algorithm: civil-from-days (Howard
/// Hinnant). Matches `strftime("%Y-%m-%dT%H:%M:%SZ", gmtime_r(...))`
/// byte-for-byte on every date the C tool ever emitted.
fn format_iso8601_utc(secs: u64) -> String {
    let days = (secs / 86_400) as i64;
    let secs_of_day = secs % 86_400;
    let hour = (secs_of_day / 3600) as u32;
    let minute = ((secs_of_day % 3600) / 60) as u32;
    let second = (secs_of_day % 60) as u32;

    // civil_from_days, Howard Hinnant (date library); valid for all i64.
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64; // [0, 146_096]
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365; // [0, 399]
    let y = (yoe as i64) + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32; // [1, 31]
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32; // [1, 12]
    let y = if m <= 2 { y + 1 } else { y };
    format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
        y, m, d, hour, minute, second
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn iso8601_epoch() {
        assert_eq!(format_iso8601_utc(0), "1970-01-01T00:00:00Z");
    }

    #[test]
    fn iso8601_known() {
        // 2026-05-22T00:00:00Z — verified with `date -u -d ... +%s`.
        let dt = 1_779_408_000u64;
        assert_eq!(format_iso8601_utc(dt), "2026-05-22T00:00:00Z");
    }

    #[test]
    fn parse_unsigned_basics() {
        assert_eq!(parse_unsigned("0").unwrap(), 0);
        assert_eq!(parse_unsigned("1").unwrap(), 1);
        assert_eq!(parse_unsigned("0x10").unwrap(), 16);
        assert_eq!(parse_unsigned("0X10").unwrap(), 16);
        assert_eq!(parse_unsigned("010").unwrap(), 8);
        assert!(parse_unsigned("").is_err());
        assert!(parse_unsigned("foo").is_err());
    }
}
