//! `gbl unwrap` — Extract PE32+ image from a Qualcomm ABL/XBL partition.
//!
//! Replaces `tools/fv-unwrap/fv-unwrap.c` byte-for-byte. Same FFS / GUID-
//! defined section / FV-image walk; LZMA-alone decompression is now via
//! `lzma-rs` instead of the cross-built `liblzma` the Dockerfile carried.
//!
//! On stdout we print exactly one diagnostic the C tool printed —
//!   `efisp-marker: present` / `efisp-marker: absent`
//! — using `pe_utils::efisp_marker_present` for byte-identical parity with
//! the C tool's `gbl_contains_utf16_efisp`. Every other diagnostic (FV
//! offsets, LZMA payload sizes, etc.) goes to stderr.

use std::io::Read;
use std::path::PathBuf;

use clap::Parser;
use lzma_rs::lzma_decompress_with_options;

use super::{slurp, GblError};

#[derive(Parser, Debug)]
#[command(about = "Extract PE32+ from a Qualcomm-style ABL/XBL image")]
pub struct Args {
    /// Raw partition image (LZMA-FV wrapped ABL/XBL).
    input: PathBuf,
    /// Output path for the extracted PE32+ blob.
    output: PathBuf,
}

pub fn run(args: Args) -> Result<(), GblError> {
    let buf = slurp(&args.input)?;
    let pe = extract_from_partition(&buf).ok_or_else(|| {
        GblError::runtime(format!("{}: no PE32 found", args.input.display()))
    })?;

    std::fs::write(&args.output, &pe).map_err(|e| {
        GblError::runtime(format!("{}: {}", args.output.display(), e))
    })?;
    eprintln!(
        "wrote 0x{:x} ({}) bytes to {}",
        pe.len(),
        pe.len(),
        args.output.display()
    );

    // efisp-marker diagnostic (stdout, byte-for-byte parity with C tool).
    let marker = if pe_utils::efisp_marker_present(&pe) {
        "present"
    } else {
        "absent"
    };
    println!("efisp-marker: {marker}");
    Ok(())
}

// --- PE / FV / FFS walker --------------------------------------------
//
// All the layouts below mirror fv-unwrap.c byte-for-byte. Header offsets
// are reproduced from the comment block at the top of the C source.

/// Scan `buf` for the first valid PE32+ image and return a heap-owned
/// copy. Mirrors `find_and_extract_pe`.
fn find_and_extract_pe(buf: &[u8]) -> Option<Vec<u8>> {
    if buf.len() < 0x80 + 4 {
        return None;
    }
    let mut i = 0usize;
    while i + 0x80 + 4 < buf.len() {
        if buf[i] != b'M' || buf[i + 1] != b'Z' {
            i += 1;
            continue;
        }
        let e_lfanew = u32::from_le_bytes([
            buf[i + 0x3C],
            buf[i + 0x3D],
            buf[i + 0x3E],
            buf[i + 0x3F],
        ]) as usize;
        let peoff = match i.checked_add(e_lfanew) {
            Some(v) => v,
            None => {
                i += 1;
                continue;
            }
        };
        if peoff + 4 >= buf.len() {
            i += 1;
            continue;
        }
        if buf[peoff] != b'P'
            || buf[peoff + 1] != b'E'
            || buf[peoff + 2] != 0
            || buf[peoff + 3] != 0
        {
            i += 1;
            continue;
        }
        // OptHdr starts at PE+4+20; SizeOfImage at OptHdr+56.
        let opthdr = peoff + 4 + 20;
        if opthdr + 60 >= buf.len() {
            i += 1;
            continue;
        }
        let mut sz = u32::from_le_bytes([
            buf[opthdr + 56],
            buf[opthdr + 57],
            buf[opthdr + 58],
            buf[opthdr + 59],
        ]) as usize;
        if sz == 0 || i + sz > buf.len() {
            sz = buf.len() - i;
        }
        eprintln!("  PE32: MZ at +0x{:x} SizeOfImage=0x{:x}", i, sz);
        return Some(buf[i..i + sz].to_vec());
    }
    None
}

/// EDK2 LZMA section-data GUID stored little-endian:
/// EE4E5898-3914-4259-9D6E-DC7BD79403CF.
const LZMA_GUID: [u8; 16] = [
    0x98, 0x58, 0x4E, 0xEE, 0x14, 0x39, 0x59, 0x42, 0x9D, 0x6E, 0xDC, 0x7B,
    0xD7, 0x94, 0x03, 0xCF,
];

const EFI_SECTION_COMPRESSION: u8 = 0x01;
const EFI_SECTION_GUID_DEFINED: u8 = 0x02;
const EFI_SECTION_PE32: u8 = 0x10;
const EFI_SECTION_FV_IMAGE: u8 = 0x17;

#[inline]
fn u24le(p: &[u8]) -> u32 {
    (p[0] as u32) | ((p[1] as u32) << 8) | ((p[2] as u32) << 16)
}

/// LZMA_ALONE decompression via `lzma-rs`. The C tool used `liblzma`'s
/// `lzma_alone_decoder` with `memlimit = UINT64_MAX`; lzma-rs' alone
/// decoder is the same on-disk format (5-byte properties + 8-byte
/// uncompressed-size + payload). Streams that end with `LZMA_BUF_ERROR`
/// (EDK2 emits these in practice) still produce useful output up to the
/// last whole-block boundary — match the C behaviour and return the
/// decompressed bytes anyway, letting the FV walker re-validate downstream.
fn lzma_alone_decompress(input: &[u8]) -> Option<Vec<u8>> {
    if input.len() < 13 {
        return None;
    }
    let mut reader = std::io::Cursor::new(input);
    let mut out: Vec<u8> = Vec::new();
    let opts = lzma_rs::decompress::Options {
        unpacked_size: lzma_rs::decompress::UnpackedSize::ReadFromHeader,
        memlimit: None,
        allow_incomplete: true,
    };
    match lzma_decompress_with_options(&mut reader, &mut out, &opts) {
        Ok(()) => Some(out),
        Err(e) => {
            // Trailing-buf-error tolerance — emit the partial output the
            // walker may still extract a PE from.
            eprintln!("  lzma decompress: {} (decoded {} bytes)", e, out.len());
            if out.is_empty() {
                None
            } else {
                Some(out)
            }
        }
    }
}

/// Walk an FFS body's section list at `ffs_body[..body_sz]`. Mirrors
/// `walk_sections` in fv-unwrap.c — same align-up-to-4 rule, same section
/// type dispatch, same indented diagnostic logging.
fn walk_sections(ffs_body: &[u8], depth: i32) -> Option<Vec<u8>> {
    let body_sz = ffs_body.len();
    let mut off = 0usize;
    while off + 4 <= body_sz {
        if off & 3 != 0 {
            off = (off + 3) & !3usize;
            continue;
        }
        let sz = u24le(&ffs_body[off..off + 3]) as usize;
        let stype = ffs_body[off + 3];
        if sz < 4 || off + sz > body_sz {
            break;
        }

        eprintln!(
            "{:indent$}SEC type=0x{:02x} size=0x{:x} @ body+0x{:x}",
            "",
            stype,
            sz,
            off,
            indent = (depth * 2) as usize
        );

        match stype {
            EFI_SECTION_PE32 => {
                if let Some(pe) = find_and_extract_pe(&ffs_body[off + 4..off + sz]) {
                    return Some(pe);
                }
            }
            EFI_SECTION_GUID_DEFINED => {
                // GUID_DEFINED: GUID(16) + DataOffset(2) + Attributes(2) after the 4-byte section hdr.
                if sz < 4 + 20 {
                    off += sz;
                    continue;
                }
                let guid = &ffs_body[off + 4..off + 4 + 16];
                let data_off = u16::from_le_bytes([
                    ffs_body[off + 20],
                    ffs_body[off + 21],
                ]) as usize;
                if guid == LZMA_GUID {
                    eprintln!(
                        "{:indent$}LZMA payload at sec+0x{:x}",
                        "",
                        data_off,
                        indent = (depth * 2) as usize
                    );
                    let payload = &ffs_body[off + data_off..off + sz];
                    if let Some(dec) = lzma_alone_decompress(payload) {
                        eprintln!(
                            "{:indent$}Decompressed: {} bytes",
                            "",
                            dec.len(),
                            indent = (depth * 2) as usize
                        );
                        // Try nested FV first, then bare PE scan.
                        if let Some(r) = fv_extract_pe(&dec, depth + 1) {
                            return Some(r);
                        }
                        if let Some(r) = find_and_extract_pe(&dec) {
                            return Some(r);
                        }
                    }
                }
            }
            EFI_SECTION_COMPRESSION => {
                eprintln!(
                    "{:indent$}COMPRESSION section (tiano?) — skipping",
                    "",
                    indent = (depth * 2) as usize
                );
            }
            EFI_SECTION_FV_IMAGE => {
                if let Some(r) = fv_extract_pe(&ffs_body[off + 4..off + sz], depth + 1) {
                    return Some(r);
                }
            }
            _ => {}
        }
        off += sz;
    }
    None
}

/// Walk an FV (Firmware Volume): for each FFS file inside it, walk that
/// FFS body's section list. Mirrors `fv_extract_pe` in fv-unwrap.c.
fn fv_extract_pe(fv: &[u8], depth: i32) -> Option<Vec<u8>> {
    let fv_sz = fv.len();
    if fv_sz < 0x48 {
        return None;
    }
    if &fv[40..44] != b"_FVH" {
        eprintln!(
            "{:indent$}FV sig mismatch at depth {}",
            "",
            depth,
            indent = (depth * 2) as usize
        );
        return None;
    }
    let mut fv_len = u64::from_le_bytes([
        fv[32], fv[33], fv[34], fv[35], fv[36], fv[37], fv[38], fv[39],
    ]) as usize;
    let hdr_len = u16::from_le_bytes([fv[48], fv[49]]) as usize;
    if fv_len > fv_sz {
        fv_len = fv_sz;
    }
    eprintln!(
        "{:indent$}FV: len=0x{:x} hdr=0x{:x}",
        "",
        fv_len,
        hdr_len,
        indent = (depth * 2) as usize
    );

    let mut off = hdr_len;
    while off + 24 <= fv_len {
        if off & 7 != 0 {
            off = (off + 7) & !7usize;
            continue;
        }
        // FFS file header: GUID(16) + integrity(2) + type(1) + attrs(1) +
        // size3(3) + state(1).
        let fsz = u24le(&fv[off + 20..off + 23]) as usize;
        let ftype = fv[off + 18];
        if fsz == 0 || fsz == 0xFF_FFFF {
            break;
        }
        if off + fsz > fv_len {
            break;
        }
        eprintln!(
            "{:indent$}FFS type=0x{:02x} size=0x{:x} @ fv+0x{:x}",
            "",
            ftype,
            fsz,
            off,
            indent = (depth * 2) as usize
        );
        // Body starts after the 24-byte FFS header.
        if let Some(pe) = walk_sections(&fv[off + 24..off + fsz], depth + 1) {
            return Some(pe);
        }
        off += fsz;
    }
    None
}

/// Locate an FV inside an ELF-wrapped raw partition image and walk it.
/// Mirrors `extract_from_partition`.
fn extract_from_partition(buf: &[u8]) -> Option<Vec<u8>> {
    if buf.len() < 48 {
        return None;
    }
    let mut i = 40usize;
    while i + 8 < buf.len() {
        if buf[i] == b'_'
            && buf[i + 1] == b'F'
            && buf[i + 2] == b'V'
            && buf[i + 3] == b'H'
        {
            let fv_start = i - 40;
            eprintln!("FV candidate at 0x{:x}", fv_start);
            if let Some(r) = fv_extract_pe(&buf[fv_start..], 0) {
                return Some(r);
            }
        }
        i += 1;
    }
    eprintln!("No FV found — trying bare MZ/PE scan");
    find_and_extract_pe(buf)
}

// Suppress unused-import lint when stdin path goes unused (we don't read
// stdin, but keeping Read in scope makes the file resilient to future
// adds).
#[allow(dead_code)]
fn _force_read_in_scope(mut r: impl Read) -> std::io::Result<()> {
    let mut b = [0u8; 0];
    r.read_exact(&mut b)
}
