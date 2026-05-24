//! Subcommand modules + shared error type.
//!
//! Every subcommand exports a `pub struct Args` (clap-derived) and a
//! `pub fn run(args: Args) -> Result<(), GblError>`. `main.rs` dispatches
//! on the Cli enum and prints whatever the run() returns.

pub mod avb;
pub mod commit;
pub mod inspect;
pub mod mode2;
pub mod pack;
pub mod patch;
pub mod unwrap;

/// Shared error type for every subcommand.
///
/// Carries both an exit code (so the multicall preserves the C tools'
/// distinct exit-code surface — usage errors are 2, runtime errors are 1,
/// verify failures are 3) and an optional pre-formatted message to print
/// to stderr.
///
/// Subcommands that have already printed their own error messages should
/// return `GblError::silent(code)` so we don't double-print.
#[derive(Debug)]
pub struct GblError {
    code: u8,
    msg: Option<String>,
}

impl GblError {
    /// Generic runtime failure with a printable message (exit code 1).
    pub fn runtime<S: Into<String>>(msg: S) -> Self {
        Self { code: 1, msg: Some(msg.into()) }
    }

    /// Usage / bad-argument failure (exit code 2).
    #[allow(dead_code)]
    pub fn usage<S: Into<String>>(msg: S) -> Self {
        Self { code: 2, msg: Some(msg.into()) }
    }

    /// Verify-after-write failure (exit code 3 — `gbl-commit` specific).
    pub fn verify<S: Into<String>>(msg: S) -> Self {
        Self { code: 3, msg: Some(msg.into()) }
    }

    /// Caller has already printed all the diagnostics it wants; the
    /// multicall should just propagate the exit code.
    pub fn silent(code: u8) -> Self {
        Self { code, msg: None }
    }

    pub fn exit_code(&self) -> u8 {
        self.code
    }

    pub fn message(&self) -> Option<&str> {
        self.msg.as_deref()
    }
}

/// Slurp the entire contents of `path` into a `Vec<u8>`. Mirrors the
/// `slurp` / `read_file` helpers from the deleted C tools (block-device
/// aware: falls back to `lseek(SEEK_END)` when `fstat` reports zero size).
pub(crate) fn slurp(path: &std::path::Path) -> Result<Vec<u8>, GblError> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(path).map_err(|e| {
        GblError::runtime(format!("{}: {}", path.display(), e))
    })?;
    let meta_len = f
        .metadata()
        .map(|m| m.len())
        .unwrap_or(0);
    // For block devices `metadata().len()` is 0 on Linux; fall back to
    // lseek-to-end before reading, matching the C tools.
    let want = if meta_len == 0 {
        let end = f.seek(SeekFrom::End(0)).map_err(|e| {
            GblError::runtime(format!("{}: seek: {}", path.display(), e))
        })?;
        f.seek(SeekFrom::Start(0)).map_err(|e| {
            GblError::runtime(format!("{}: seek: {}", path.display(), e))
        })?;
        end
    } else {
        meta_len
    };
    if want == 0 {
        return Err(GblError::runtime(format!(
            "{}: empty or unreadable",
            path.display()
        )));
    }
    let mut buf = Vec::with_capacity(want as usize);
    f.read_to_end(&mut buf).map_err(|e| {
        GblError::runtime(format!("{}: read: {}", path.display(), e))
    })?;
    Ok(buf)
}
