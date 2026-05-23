//! `gbl` multicall — single binary that replaces the 7 host C tools.
//!
//! Subcommands map 1:1 onto the deleted `tools/<x>/` binaries:
//!
//! | subcommand          | replaced binary    |
//! |---------------------|--------------------|
//! | `gbl commit`        | `gbl-commit`       |
//! | `gbl unwrap`        | `fv-unwrap`        |
//! | `gbl patch`         | `abl-patcher`      |
//! | `gbl pack`          | `gbl-pack`         |
//! | `gbl inspect`       | `gblp1-inspect`    |
//! | `gbl mode2`         | `mode2-profile`    |
//! | `gbl avb`           | `vbmeta-graft`     |
//!
//! Engine-rework deltas (PR1 contract):
//!
//! - `gbl patch` has NO `--no-mode1` flag (PR1 Task 12 dropped it).
//!   `--oem oplus` is canonical; `--oem oneplus` is a deprecation alias
//!   that maps to `Oem::Oplus` and prints a one-time warning.
//! - `gbl pack` has a `--manifest <bits>` flag (PR1 Task 3; PR2 Task 4
//!   wired through the Rust crate).
//! - `gbl inspect` pretty-prints the manifest entry (PR1 Task 4).

use clap::Parser;

mod commands;

/// Top-level argv parser.
#[derive(Parser)]
#[command(
    name = "gbl",
    version = env!("CARGO_PKG_VERSION"),
    about = "gbl-chainload host + recovery multicall tool",
    long_about = "Multicall binary for the gbl-chainload toolchain. \
                  Each subcommand maps 1:1 onto a former standalone host \
                  tool; see `gbl <sub> --help` for per-subcommand flags."
)]
enum Cli {
    /// Copy a file/device with optional backup + uncached SHA-256 verify
    /// (former `gbl-commit`).
    Commit(commands::commit::Args),
    /// Extract a PE32+ image from a Qualcomm ABL/XBL partition image
    /// (former `fv-unwrap`).
    Unwrap(commands::unwrap::Args),
    /// Apply the host patch table to an ABL PE
    /// (former `abl-patcher`; NO `--no-mode1` post-PR1).
    Patch(commands::patch::Args),
    /// Pack a GBLP1 v1 container (former `gbl-pack`).
    Pack(commands::pack::Args),
    /// Inspect a GBLP1 v1 container (former `gblp1-inspect`).
    Inspect(commands::inspect::Args),
    /// Mode-2 profile tooling (former `mode2-profile`).
    Mode2(commands::mode2::Args),
    /// AVB vbmeta tooling (former `vbmeta-graft`).
    Avb(commands::avb::Args),
}

fn main() -> std::process::ExitCode {
    let cli = Cli::parse();
    let result: Result<(), commands::GblError> = match cli {
        Cli::Commit(args) => commands::commit::run(args),
        Cli::Unwrap(args) => commands::unwrap::run(args),
        Cli::Patch(args) => commands::patch::run(args),
        Cli::Pack(args) => commands::pack::run(args),
        Cli::Inspect(args) => commands::inspect::run(args),
        Cli::Mode2(args) => commands::mode2::run(args),
        Cli::Avb(args) => commands::avb::run(args),
    };
    match result {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(e) => {
            let code = e.exit_code();
            if let Some(msg) = e.message() {
                eprintln!("{msg}");
            }
            std::process::ExitCode::from(code)
        }
    }
}
