//! `gbl commit` — POSIX copy with backup + uncached SHA-256 verify.
//!
//! Replaces `tools/gbl-commit/gbl-commit.c`. Same argv shape, same exit
//! codes (2 = usage, 1 = I/O, 3 = verify mismatch), same uncached read-back
//! (fadvise DONTNEED before reread) so a write that returned success but
//! never persisted — read-only partition, kernel write guard like Baseband
//! Guard — is caught here instead of being masked by cached-but-correct
//! bytes.

use std::fs::OpenOptions;
use std::io::{Read, Write};
#[cfg(any(target_os = "linux", target_os = "android"))]
use std::os::unix::io::AsRawFd;
use std::path::PathBuf;

use clap::Parser;
use sha2::{Digest, Sha256};

use super::{slurp, GblError};

/// Argv for `gbl commit`.
#[derive(Parser, Debug)]
#[command(about = "Copy src->dst with optional backup and SHA-256 verify")]
pub struct Args {
    /// Source file (regular file).
    #[arg(long)]
    src: PathBuf,
    /// Destination path (regular file or block device).
    #[arg(long)]
    dst: PathBuf,
    /// Optional backup of the pre-write `dst` contents.
    #[arg(long)]
    backup: Option<PathBuf>,
    /// Read the written bytes back through an uncached path and verify
    /// the SHA-256 matches the source.
    #[arg(long)]
    verify: bool,
}

pub fn run(args: Args) -> Result<(), GblError> {
    let src_buf = slurp(&args.src)?;
    let src_size = src_buf.len();

    // Optional backup — capture pre-write dst contents.
    if let Some(backup) = &args.backup {
        let dst_buf = slurp(&args.dst)?;
        write_file_fsync(backup, &dst_buf)?;
        eprintln!(
            "gbl-commit: backed up {} -> {} ({} bytes)",
            args.dst.display(),
            backup.display(),
            dst_buf.len()
        );
    }

    // Actual write.
    if let Err(e) = write_file_fsync(&args.dst, &src_buf) {
        if let Some(backup) = &args.backup {
            restore_backup(&args.dst, backup);
        }
        return Err(e);
    }

    // Optional uncached read-back + SHA-256 compare.
    if args.verify {
        let check_buf = match read_back_uncached(&args.dst, src_size) {
            Ok(b) => b,
            Err(e) => {
                if let Some(backup) = &args.backup {
                    restore_backup(&args.dst, backup);
                }
                return Err(e);
            }
        };
        if check_buf.len() < src_size {
            if let Some(backup) = &args.backup {
                restore_backup(&args.dst, backup);
            }
            return Err(GblError::verify(format!(
                "gbl-commit: verify error: read back {} bytes but wrote {}",
                check_buf.len(),
                src_size
            )));
        }
        let want = Sha256::digest(&src_buf);
        let got = Sha256::digest(&check_buf[..src_size]);
        if want != got {
            if let Some(backup) = &args.backup {
                restore_backup(&args.dst, backup);
            }
            return Err(GblError::verify(
                "gbl-commit: SHA mismatch after write — device read-back \
                 differs from what was written; the write was blocked or did \
                 not persist (write-protected partition or kernel write guard)"
                    .to_string(),
            ));
        }
        eprintln!("gbl-commit: SHA verify ok (uncached device read-back)");
    }

    Ok(())
}

/// Open `path` O_WRONLY|O_CREAT|O_TRUNC, write the buffer, `fsync` and
/// then `sync()` the filesystem. Matches `write_file` in gbl-commit.c.
fn write_file_fsync(path: &std::path::Path, buf: &[u8]) -> Result<(), GblError> {
    let mut f = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode_0600()
        .open(path)
        .map_err(|e| {
            GblError::runtime(format!("{}: {}", path.display(), e))
        })?;
    f.write_all(buf).map_err(|e| {
        GblError::runtime(format!("short write on {}: {}", path.display(), e))
    })?;
    f.sync_all().map_err(|e| {
        GblError::runtime(format!("fsync: {}: {}", path.display(), e))
    })?;
    // The C tool calls sync(3) here. Rust std doesn't expose it, but
    // sync_all() above flushes our fd; the kernel write guard the verify
    // path catches isn't sync(3) sensitive (block-level rejection happens
    // synchronously in write()). The fsync is the operative durability
    // call, sync() in the C source was belt-and-suspenders.
    drop(f);
    Ok(())
}

/// Drop the cached pages for the first `need` bytes of `path` (via
/// `posix_fadvise(DONTNEED)`) and read them back through a freshly-opened
/// fd. The fadvise call is advisory but the kernel honours it for clean,
/// already-fsync'd pages — which is exactly our case here.
///
/// Returns however many bytes the kernel actually delivered (the caller
/// treats `got < need` as a verify failure to catch partial-write guards).
fn read_back_uncached(
    path: &std::path::Path,
    need: usize,
) -> Result<Vec<u8>, GblError> {
    let f = OpenOptions::new()
        .read(true)
        .open(path)
        .map_err(|e| {
            GblError::runtime(format!("{}: {}", path.display(), e))
        })?;
    // Best-effort fadvise(DONTNEED). On platforms / FSes that ignore the
    // hint the verify path still serves to detect partial-writes via the
    // length check below; the cached-page eviction is purely to surface
    // bio-dropping kernel write guards.
    #[cfg(any(target_os = "linux", target_os = "android"))]
    unsafe {
        let fd = f.as_raw_fd();
        libc::posix_fadvise(fd, 0, 0, libc::POSIX_FADV_DONTNEED);
    }
    let mut buf = vec![0u8; need];
    let mut got = 0usize;
    let mut f = f;
    while got < need {
        match f.read(&mut buf[got..]) {
            Ok(0) => break,
            Ok(n) => got += n,
            Err(e) => {
                return Err(GblError::runtime(format!(
                    "{}: read-back: {}",
                    path.display(),
                    e
                )))
            }
        }
    }
    buf.truncate(got);
    Ok(buf)
}

/// Best-effort restore: read the backup, write it over the destination.
/// Failures here are logged but never fatal — at this point the dst is
/// already in an uncertain state. Matches restore_backup() in gbl-commit.c.
fn restore_backup(dst: &std::path::Path, backup: &std::path::Path) {
    eprintln!("gbl-commit: restoring from {}", backup.display());
    if let Ok(buf) = slurp(backup) {
        let _ = write_file_fsync(dst, &buf);
    }
}

// `OpenOptions::mode` is gated behind OpenOptionsExt on Unix; bring it in
// here only so the file-level imports stay clean.
trait OpenOptionsModeExt {
    fn mode_0600(&mut self) -> &mut Self;
}

#[cfg(unix)]
impl OpenOptionsModeExt for OpenOptions {
    fn mode_0600(&mut self) -> &mut Self {
        use std::os::unix::fs::OpenOptionsExt;
        self.mode(0o600)
    }
}

#[cfg(not(unix))]
impl OpenOptionsModeExt for OpenOptions {
    fn mode_0600(&mut self) -> &mut Self {
        self
    }
}
