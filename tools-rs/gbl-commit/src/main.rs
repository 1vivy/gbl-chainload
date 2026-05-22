//! gbl-commit (Rust pilot) — raw write to a file or block device with optional
//! backup-before-write and an *uncached* SHA-256 verify-after-write.
//!
//! Behavioural port of `tools/gbl-commit/gbl-commit.c`: same CLI, same exit
//! codes (0 ok / 1 io / 2 usage / 3 verify-mismatch), same backup→write→
//! fsync+sync→`POSIX_FADV_DONTNEED` uncached read-back→SHA-256 compare→
//! restore-on-mismatch flow. The uncached read-back is the point: a write that
//! returns success but never persists (read-only partition, a bio-dropping
//! kernel write guard such as Baseband Guard, or an unsurfaced writeback error)
//! is caught here instead of being masked by cached-but-correct bytes.
//!
//! SHA-256 is the existing C implementation from the EFI payload, linked via
//! build.rs — byte-identical to the C tools, zero parity drift.

use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;
use std::process::ExitCode;

extern "C" {
    fn gbl_sha256(buf: *const u8, len: usize, out: *mut u8);
}

fn sha256(buf: &[u8]) -> [u8; 32] {
    let mut out = [0u8; 32];
    // SAFETY: `out` is exactly 32 bytes (the C contract); `buf`/len are valid
    // for the call's duration. Empty `buf` yields a non-null dangling ptr with
    // len 0, which the C reads zero bytes from.
    unsafe { gbl_sha256(buf.as_ptr(), buf.len(), out.as_mut_ptr()) };
    out
}

/// Read the entire file/partition. On a block device `metadata().len()` may be
/// 0, but `read_to_end` reads to the device's end regardless. Mirrors the C
/// `read_file`, including its "refuse zero-length" guard (a 0-byte buffer would
/// silently corrupt the destination on restore).
fn read_whole(path: &str) -> std::io::Result<Vec<u8>> {
    let mut f = File::open(path)?;
    let mut buf = Vec::new();
    f.read_to_end(&mut buf)?;
    if buf.is_empty() {
        return Err(std::io::Error::other(format!(
            "gbl-commit: cannot determine size of {path}"
        )));
    }
    Ok(buf)
}

/// Write `buf` to `path`, fsync the fd, then global `sync()`. Mirrors the C
/// `write_file` (O_WRONLY|O_CREAT|O_TRUNC, 0600).
fn write_whole(path: &str, buf: &[u8]) -> std::io::Result<()> {
    let mut f = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true) // ignored by the kernel on block devices, as in C
        .mode(0o600)
        .open(path)?;
    f.write_all(buf)?;
    f.sync_all()?; // fsync
    drop(f);
    // SAFETY: libc::sync() takes no args and cannot fail.
    unsafe { libc::sync() };
    Ok(())
}

/// Restore `backup` onto `dst` on write failure / verify mismatch. Best-effort,
/// matching the C `(void)write_file(...)`.
fn restore_backup(dst: &str, backup: &str) {
    eprintln!("gbl-commit: restoring from {backup}");
    if let Ok(bb) = read_whole(backup) {
        let _ = write_whole(dst, &bb);
    }
}

/// Read the first `need` bytes of `path` bypassing the page cache, so a write
/// that did not truly persist is caught. `POSIX_FADV_DONTNEED` drops the clean
/// (already-fsync'd) pages; the read then repopulates them from the device.
/// Returns the bytes actually read (`< need` signals a short read, a verify
/// failure for the caller). Mirrors the C `read_back_uncached`.
fn read_back_uncached(path: &str, need: usize) -> std::io::Result<Vec<u8>> {
    let mut f = File::open(path)?;
    // SAFETY: fd is valid for the call; advisory, ignored result is fine.
    unsafe { libc::posix_fadvise(f.as_raw_fd(), 0, 0, libc::POSIX_FADV_DONTNEED) };
    let mut buf = vec![0u8; need];
    let mut got = 0usize;
    while got < need {
        match f.read(&mut buf[got..])? {
            0 => break,
            n => got += n,
        }
    }
    buf.truncate(got);
    Ok(buf)
}

const USAGE: &str = "usage: gbl-commit --src FILE --dst PATH \
[--backup BACKUP_PATH] [--verify]";

fn run() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();

    if args.first().map(String::as_str) == Some("--version") {
        println!("gbl-commit {}", env!("GBL_TOOL_VERSION"));
        return ExitCode::SUCCESS;
    }

    let (mut src, mut dst, mut backup) = (None, None, None);
    let mut verify = false;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--src" if i + 1 < args.len() => { src = Some(args[i + 1].clone()); i += 2; }
            "--dst" if i + 1 < args.len() => { dst = Some(args[i + 1].clone()); i += 2; }
            "--backup" if i + 1 < args.len() => { backup = Some(args[i + 1].clone()); i += 2; }
            "--verify" => { verify = true; i += 1; }
            other => {
                eprintln!("unknown arg: {other}");
                return ExitCode::from(2);
            }
        }
    }

    let (src, dst) = match (src, dst) {
        (Some(s), Some(d)) => (s, d),
        _ => {
            eprintln!("gbl-commit {}\n{USAGE}", env!("GBL_TOOL_VERSION"));
            return ExitCode::from(2);
        }
    };

    let src_buf = match read_whole(&src) {
        Ok(b) => b,
        Err(e) => { eprintln!("{e}"); return ExitCode::from(1); }
    };

    if let Some(ref bak) = backup {
        match read_whole(&dst) {
            Ok(dst_buf) => {
                if let Err(e) = write_whole(bak, &dst_buf) {
                    eprintln!("{e}");
                    return ExitCode::from(1);
                }
                eprintln!(
                    "gbl-commit: backed up {dst} -> {bak} ({} bytes)",
                    dst_buf.len()
                );
            }
            Err(e) => { eprintln!("{e}"); return ExitCode::from(1); }
        }
    }

    if let Err(e) = write_whole(&dst, &src_buf) {
        eprintln!("{e}");
        if let Some(ref bak) = backup {
            restore_backup(&dst, bak);
        }
        return ExitCode::from(1);
    }

    if verify {
        let check = match read_back_uncached(&dst, src_buf.len()) {
            Ok(b) => b,
            Err(e) => { eprintln!("{e}"); return ExitCode::from(1); }
        };

        if check.len() < src_buf.len() {
            eprintln!(
                "gbl-commit: verify error: read back {} bytes but wrote {}",
                check.len(),
                src_buf.len()
            );
            if let Some(ref bak) = backup {
                restore_backup(&dst, bak);
            }
            return ExitCode::from(3);
        }

        if sha256(&src_buf) != sha256(&check[..src_buf.len()]) {
            eprintln!(
                "gbl-commit: SHA mismatch after write — device read-back \
differs from what was written; the write was blocked or did not persist \
(write-protected partition or kernel write guard)"
            );
            if let Some(ref bak) = backup {
                restore_backup(&dst, bak);
            }
            return ExitCode::from(3);
        }
        eprintln!("gbl-commit: SHA verify ok (uncached device read-back)");
    }

    ExitCode::SUCCESS
}

fn main() -> ExitCode {
    run()
}
