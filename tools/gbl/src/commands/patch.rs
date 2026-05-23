//! `gbl patch` — host-side ABL patcher.
//!
//! Replaces `tools/abl-patcher/abl-patcher.c`. Post-PR1 contract:
//!
//! - `--no-mode1` is gone (PR1 Task 12 dropped it; `abl_permissive` is
//!   always applied at host packing time).
//! - `--oem oplus` is canonical. `--oem oneplus` is a deprecation alias
//!   that maps to `Oem::Oplus` and prints a one-time warning.
//! - `--check-anchors-only` re-uses the engine's apply loop and only
//!   surfaces a mandatory miss (matching the C source comment about
//!   AMBIGUOUS/MISS being lumped together).

use std::path::PathBuf;

use clap::Parser;
use patch_engine::{Engine, Oem, Worst};

use super::{slurp, GblError};

#[derive(Parser, Debug)]
#[command(about = "Apply the host patch table to an ABL PE")]
pub struct Args {
    /// Input PE32+ blob (post fv-unwrap output).
    #[arg(long)]
    r#in: PathBuf,
    /// Output path; omit to skip writing (still applies in-memory).
    #[arg(long)]
    out: Option<PathBuf>,
    /// Anchor-uniqueness probe mode — fail only on a mandatory miss.
    #[arg(long)]
    check_anchors_only: bool,
    /// OEM patch group to apply. `oplus` is canonical; `oneplus` is a
    /// deprecation alias; `none` skips OEM patches.
    #[arg(long, value_name = "ID")]
    oem: Option<String>,
}

pub fn run(args: Args) -> Result<(), GblError> {
    // OEM resolution — mirror abl-patcher.c's `if (OemStr != NULL)` block.
    let oem = match args.oem.as_deref() {
        None => Oem::None,
        Some("oplus") => Oem::Oplus,
        Some("oneplus") => {
            eprintln!(
                "abl-patcher: --oem oneplus is deprecated; use --oem oplus \
                 (accepted for compatibility, will be removed in a future release)"
            );
            Oem::Oplus
        }
        Some("none") => Oem::None,
        Some(other) => {
            return Err(GblError::usage(format!(
                "abl-patcher: unknown --oem '{other}'"
            )));
        }
    };

    // abl_permissive is always on at host packing time.
    let engine = Engine::ensure_init_scoped(oem, true);

    let mut buf = slurp(&args.r#in)?;
    let sz = buf.len();
    let result = engine.apply(&mut buf);

    // Aggregate line — byte-for-byte match abl-patcher.c.
    eprintln!(
        "{}: applied={} missed={} worst={} (0=ok 1=optional-miss 2=mandatory-miss)",
        args.r#in.display(),
        result.applied_count,
        result.missed_count,
        worst_as_int(result.worst),
    );

    if args.check_anchors_only {
        if result.worst == Worst::MandatoryMiss {
            eprintln!(
                "FAIL: mandatory patch missed/ambiguous on {}",
                args.r#in.display()
            );
            return Err(GblError::silent(1));
        }
        eprintln!("ok check-anchors-only on {}", args.r#in.display());
        return Ok(());
    }

    if result.worst == Worst::MandatoryMiss {
        eprintln!(
            "ERROR: mandatory patch missed on {}",
            args.r#in.display()
        );
        return Err(GblError::silent(1));
    }

    if let Some(out) = &args.out {
        std::fs::write(out, &buf).map_err(|e| {
            GblError::runtime(format!("{}: write failed: {}", out.display(), e))
        })?;
        eprintln!("wrote {} bytes to {}", sz, out.display());
    }

    Ok(())
}

fn worst_as_int(w: Worst) -> i32 {
    match w {
        Worst::Ok => 0,
        Worst::OptionalMiss => 1,
        Worst::MandatoryMiss => 2,
    }
}
