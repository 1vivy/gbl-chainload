//! `gbl avb` — AVB vbmeta tooling (former vbmeta-graft).
//!
//! Replaces `tools/vbmeta-graft/vbmeta-graft.c`. Same 4 subcommands:
//!
//! - `gbl avb list <vbmeta-or-partition-img>` — enumerate descriptors.
//! - `gbl avb check <candidate-part> <main-vbmeta> <part>` — chain-key match.
//! - `gbl avb graft --stock <s> --custom <c> --part-size <n> --out <o>` — graft.
//! - `gbl avb list-hash <active-vbmeta> <byname-dir>` — verify hash + chain
//!   verdicts against an on-device byname directory.

use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use clap::{Args as ClapArgs, Parser, Subcommand};
use sha2::{Digest, Sha256};

use super::{slurp, GblError};

#[derive(Parser, Debug)]
#[command(about = "AVB vbmeta tooling (list / check / graft / list-hash)")]
pub struct Args {
    #[command(subcommand)]
    sub: Sub,
}

#[derive(Subcommand, Debug)]
enum Sub {
    /// Enumerate descriptors in a vbmeta blob or footer'd partition.
    List(ListArgs),
    /// Confirm a candidate partition's embedded vbmeta is chained from
    /// the main vbmeta's <part> descriptor.
    Check(CheckArgs),
    /// Build a partition image grafting stock vbmeta onto custom content.
    Graft(GraftArgs),
    /// Walk hash + chain descriptors against an on-device byname dir.
    ListHash(ListHashArgs),
}

#[derive(ClapArgs, Debug)]
struct ListArgs {
    /// Bare vbmeta or footer'd partition.
    image: PathBuf,
}

#[derive(ClapArgs, Debug)]
struct CheckArgs {
    /// Candidate partition image with embedded vbmeta.
    candidate: PathBuf,
    /// Main vbmeta image containing the chain descriptor.
    main_vbmeta: PathBuf,
    /// Partition name (chain descriptor target).
    part: String,
}

#[derive(ClapArgs, Debug)]
struct GraftArgs {
    #[arg(long)]
    stock: PathBuf,
    #[arg(long)]
    custom: PathBuf,
    #[arg(long)]
    part_size: u64,
    #[arg(long)]
    out: PathBuf,
}

#[derive(ClapArgs, Debug)]
struct ListHashArgs {
    /// Active main vbmeta image (descriptors source).
    active_vbmeta: PathBuf,
    /// On-device-style by-name directory.
    byname_dir: PathBuf,
}

pub fn run(args: Args) -> Result<(), GblError> {
    match args.sub {
        Sub::List(a) => cmd_list(&a.image),
        Sub::Check(a) => cmd_check(&a.candidate, &a.main_vbmeta, &a.part),
        Sub::Graft(a) => cmd_graft(&a.stock, &a.custom, a.part_size, &a.out),
        Sub::ListHash(a) => cmd_list_hash(&a.active_vbmeta, &a.byname_dir),
    }
}

// --- locate_vbmeta ---------------------------------------------------

/// Return `(offset, len)` of the embedded vbmeta within `buf`. If the
/// buffer carries an AvbFooter use it; else treat the whole buffer as a
/// bare vbmeta blob. Mirrors locate_vbmeta() in vbmeta-graft.c.
fn locate_vbmeta(buf: &[u8]) -> Option<(usize, usize)> {
    if let Ok(f) = avb_parse::parse_footer(buf) {
        let off = f.vbmeta_offset as usize;
        let len = f.vbmeta_size as usize;
        if off.checked_add(len)? <= buf.len() {
            return Some((off, len));
        }
    }
    if buf.len() >= 4 && &buf[0..4] == avb_parse::VBMETA_MAGIC {
        return Some((0, buf.len()));
    }
    None
}

// --- list ------------------------------------------------------------

fn cmd_list(path: &Path) -> Result<(), GblError> {
    let buf = slurp(path)?;
    let (off, len) = locate_vbmeta(&buf).ok_or_else(|| {
        GblError::runtime(format!(
            "vbmeta-graft: {}: no vbmeta found",
            path.display()
        ))
    })?;
    let vb = avb_parse::parse_vbmeta(&buf[off..off + len])
        .map_err(|e| GblError::runtime(format!(
            "vbmeta-graft: parse error: {:?}", e
        )))?;
    for d in vb.descriptors().flatten() {
        match d {
            avb_parse::Descriptor::Hash(h) => {
                println!(
                    "partition={} type=hash graftable=yes",
                    String::from_utf8_lossy(h.partition_name)
                );
            }
            avb_parse::Descriptor::ChainPartition(c) => {
                println!(
                    "partition={} type=chain graftable=yes",
                    String::from_utf8_lossy(c.partition_name)
                );
            }
            avb_parse::Descriptor::Hashtree { .. } => {
                println!("descriptor type=hashtree");
            }
            _ => {
                println!("descriptor type=other");
            }
        }
    }
    Ok(())
}

// --- check -----------------------------------------------------------

fn cmd_check(
    cand_path: &Path,
    main_path: &Path,
    part: &str,
) -> Result<(), GblError> {
    let cand = slurp(cand_path)?;
    let mainb = slurp(main_path)?;

    let (coff, clen) = locate_vbmeta(&cand).ok_or_else(|| {
        GblError::runtime("vbmeta-graft: check: unparseable vbmeta".to_string())
    })?;
    let (moff, mlen) = locate_vbmeta(&mainb).ok_or_else(|| {
        GblError::runtime("vbmeta-graft: check: unparseable vbmeta".to_string())
    })?;

    let cvb = avb_parse::parse_vbmeta(&cand[coff..coff + clen])
        .map_err(|_| {
            GblError::runtime(
                "vbmeta-graft: check: bad candidate vbmeta".to_string(),
            )
        })?;
    let cand_pk = cvb.public_key().ok_or_else(|| {
        GblError::runtime(
            "vbmeta-graft: check: candidate public key out of bounds".to_string(),
        )
    })?;
    println!("rollback-index: {}", cvb.header.rollback_index);

    let mvb = avb_parse::parse_vbmeta(&mainb[moff..moff + mlen])
        .map_err(|_| {
            GblError::runtime(
                "vbmeta-graft: check: bad main vbmeta".to_string(),
            )
        })?;
    let mut chain_pk: Option<Vec<u8>> = None;
    for d in mvb.descriptors().flatten() {
        if let avb_parse::Descriptor::ChainPartition(c) = d {
            if c.partition_name == part.as_bytes() {
                chain_pk = Some(c.public_key.to_vec());
                break;
            }
        }
    }
    let chain_pk = match chain_pk {
        Some(p) => p,
        None => {
            // Parsed but unsuitable — exit code 2, matching the C tool.
            return Err(GblError::usage(format!(
                "vbmeta-graft: check: no chain descriptor for '{part}'"
            )));
        }
    };
    if chain_pk == cand_pk {
        println!("suitable: key matches chain descriptor for {part}");
        Ok(())
    } else {
        Err(GblError::usage(format!(
            "vbmeta-graft: check: key mismatch for '{part}'"
        )))
    }
}

// --- graft -----------------------------------------------------------

fn cmd_graft(
    stock_path: &Path,
    custom_path: &Path,
    part_size: u64,
    out_path: &Path,
) -> Result<(), GblError> {
    if part_size < avb_parse::FOOTER_SIZE as u64 {
        return Err(GblError::usage(format!(
            "vbmeta-graft: part-size {} too small (min {})",
            part_size,
            avb_parse::FOOTER_SIZE
        )));
    }
    let stock = slurp(stock_path)?;
    let custom = slurp(custom_path)?;

    let (svb_off, svb_len) = locate_vbmeta(&stock).ok_or_else(|| {
        GblError::runtime("vbmeta-graft: graft: no stock vbmeta".to_string())
    })?;
    let svb = &stock[svb_off..svb_off + svb_len];

    // Content size: AvbFooter's OriginalImageSize if the custom is footer'd
    // (recovery dump or partition-sized custom image); else the file size.
    let content: u64 = match avb_parse::parse_footer(&custom) {
        Ok(f) => f.original_image_size,
        Err(_) => custom.len() as u64,
    };
    let vb_off = (content + 4095) & !4095;
    let footer_at = part_size - avb_parse::FOOTER_SIZE as u64;
    if vb_off + svb_len as u64 > footer_at {
        return Err(GblError::runtime(format!(
            "vbmeta-graft: graft: custom image too large for the partition \
             ({} B content + vbmeta exceeds {} B)",
            content, part_size
        )));
    }

    let mut img = vec![0u8; part_size as usize];
    img[..content as usize].copy_from_slice(&custom[..content as usize]);
    img[vb_off as usize..vb_off as usize + svb_len]
        .copy_from_slice(svb);

    // Footer at partition_end - 64.
    let ft = &mut img[footer_at as usize..(footer_at + 64) as usize];
    ft[..4].copy_from_slice(avb_parse::FOOTER_MAGIC);
    put_u32_be(&mut ft[4..8], 1);                   // major version
    put_u32_be(&mut ft[8..12], 0);                  // minor version
    put_u64_be(&mut ft[12..20], content);           // OriginalImageSize
    put_u64_be(&mut ft[20..28], vb_off);            // VbmetaOffset
    put_u64_be(&mut ft[28..36], svb_len as u64);    // VbmetaSize
    // bytes 36..64 stay zero.

    std::fs::write(out_path, &img).map_err(|e| {
        GblError::runtime(format!(
            "vbmeta-graft: {}: cannot write: {}",
            out_path.display(),
            e
        ))
    })?;
    eprintln!(
        "vbmeta-graft: grafted {} ({} B, vbmeta @ 0x{:x})",
        out_path.display(),
        part_size,
        vb_off
    );
    Ok(())
}

fn put_u32_be(buf: &mut [u8], v: u32) {
    buf[..4].copy_from_slice(&v.to_be_bytes());
}

fn put_u64_be(buf: &mut [u8], v: u64) {
    buf[..8].copy_from_slice(&v.to_be_bytes());
}

// --- list-hash --------------------------------------------------------

fn cmd_list_hash(mvb_path: &Path, byname_dir: &Path) -> Result<(), GblError> {
    let buf = slurp(mvb_path)?;
    let (vb_off, vb_len) = locate_vbmeta(&buf).ok_or_else(|| {
        GblError::runtime(format!(
            "vbmeta-graft: {}: no vbmeta found",
            mvb_path.display()
        ))
    })?;
    let vb = avb_parse::parse_vbmeta(&buf[vb_off..vb_off + vb_len])
        .map_err(|_| GblError::runtime("vbmeta-graft: bad main vbmeta".to_string()))?;

    let slot = derive_slot(mvb_path);

    for d in vb.descriptors().flatten() {
        match d {
            avb_parse::Descriptor::Hash(h) => {
                emit_hash_line(byname_dir, slot, h);
            }
            avb_parse::Descriptor::ChainPartition(c) => {
                emit_chain_line(byname_dir, slot, c);
            }
            _ => {}
        }
    }
    Ok(())
}

/// Slot resolution: env GBL_VBMETA_SLOT > path tail-match (_a/_b) > "a".
fn derive_slot(mvb_path: &Path) -> &'static str {
    if let Ok(env) = std::env::var("GBL_VBMETA_SLOT") {
        if env == "a" {
            return "a";
        }
        if env == "b" {
            return "b";
        }
    }
    if let Some(base) = mvb_path.file_name().and_then(|s| s.to_str()) {
        let bytes = base.as_bytes();
        let len = bytes.len();
        if len >= 2 && bytes[len - 2] == b'_' {
            if bytes[len - 1] == b'a' {
                return "a";
            }
            if bytes[len - 1] == b'b' {
                return "b";
            }
        }
    }
    eprintln!("note: slot suffix defaulted to 'a'");
    "a"
}

fn emit_hash_line(
    byname_dir: &Path,
    slot: &str,
    h: avb_parse::HashDescriptor<'_>,
) {
    let name = String::from_utf8_lossy(h.partition_name).to_string();
    let path = byname_dir.join(format!("{name}_{slot}"));
    let mut digest_status = "missing";
    let mut verdict = "mismatch";
    if let Some(part_sz) = open_size(&path) {
        // SHA-256(salt || partition_bytes[0..image_size))
        let mut read_size = h.image_size;
        if read_size > part_sz {
            read_size = part_sz;
        }
        if let Ok(mut f) = OpenOptions::new().read(true).open(&path) {
            let mut sha = Sha256::new();
            if !h.salt.is_empty() {
                sha.update(h.salt);
            }
            let mut buf = vec![0u8; 1 << 20];
            let mut remaining = read_size;
            let mut read_ok = true;
            while remaining > 0 {
                let want = if remaining > buf.len() as u64 {
                    buf.len()
                } else {
                    remaining as usize
                };
                match f.read(&mut buf[..want]) {
                    Ok(0) => {
                        read_ok = false;
                        break;
                    }
                    Ok(n) => {
                        sha.update(&buf[..n]);
                        remaining -= n as u64;
                    }
                    Err(_) => {
                        read_ok = false;
                        break;
                    }
                }
            }
            if read_ok {
                let got: [u8; 32] = sha.finalize().into();
                if h.digest.len() == 32 && got[..] == h.digest[..] {
                    digest_status = "ok";
                    verdict = "match";
                } else {
                    digest_status = "mismatch";
                    verdict = "mismatch";
                }
            }
        }
    }
    println!(
        "partition={name} type=hash declared={} digest={digest_status} graft=n/a verdict={verdict}",
        h.image_size
    );
}

fn emit_chain_line(
    byname_dir: &Path,
    slot: &str,
    c: avb_parse::ChainPartitionDescriptor<'_>,
) {
    let name = String::from_utf8_lossy(c.partition_name).to_string();
    let path = byname_dir.join(format!("{name}_{slot}"));
    let (graft_status, verdict) = match probe_partition_for_graft(&path, c.public_key) {
        GraftResult::Ok => ("ok", "match"),
        GraftResult::KeyMismatch => ("key_mismatch", "mismatch"),
        GraftResult::NoVbmeta => ("no_vbmeta", "mismatch"),
    };
    println!(
        "partition={name} type=chain declared=- digest=n/a graft={graft_status} verdict={verdict}"
    );
}

enum GraftResult {
    Ok,
    KeyMismatch,
    NoVbmeta,
}

/// `bd_open_size` equivalent — return total byte size of the open path
/// regardless of whether it's a regular file or a block device. For
/// host tests this is always a regular file; on-device the BLKGETSIZE64
/// path the C tool used isn't reachable from this binary (Android target
/// links the same Rust code; if the path is a block device, std's
/// metadata() returns 0 and we lseek-to-end as the slurp helper does).
fn open_size(path: &Path) -> Option<u64> {
    let f = std::fs::File::open(path).ok()?;
    if let Ok(m) = f.metadata() {
        if m.len() > 0 {
            return Some(m.len());
        }
    }
    // Block device — lseek to end.
    let mut f = f;
    let end = f.seek(SeekFrom::End(0)).ok()?;
    if end > 0 {
        Some(end)
    } else {
        None
    }
}

/// Walk the partition's AvbFooter to its vbmeta, then key-match against
/// the chain descriptor's pubkey. Mirrors probe_partition_for_graft().
fn probe_partition_for_graft(path: &Path, chain_pk: &[u8]) -> GraftResult {
    let part_sz = match open_size(path) {
        Some(s) if s >= avb_parse::FOOTER_SIZE as u64 => s,
        _ => return GraftResult::NoVbmeta,
    };
    let mut f = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(_) => return GraftResult::NoVbmeta,
    };
    let footer_off = part_sz - avb_parse::FOOTER_SIZE as u64;
    if f.seek(SeekFrom::Start(footer_off)).is_err() {
        return GraftResult::NoVbmeta;
    }
    let mut footer = [0u8; 64];
    if f.read_exact(&mut footer).is_err() {
        return GraftResult::NoVbmeta;
    }
    let parsed = match avb_parse::parse_footer_from_tail(&footer, part_sz) {
        Ok(p) => p,
        Err(_) => return GraftResult::NoVbmeta,
    };
    if parsed.vbmeta_size == 0
        || parsed.vbmeta_offset >= part_sz
        || parsed.vbmeta_size > part_sz - parsed.vbmeta_offset
    {
        return GraftResult::NoVbmeta;
    }
    let mut vb = vec![0u8; parsed.vbmeta_size as usize];
    if f.seek(SeekFrom::Start(parsed.vbmeta_offset)).is_err() {
        return GraftResult::NoVbmeta;
    }
    if f.read_exact(&mut vb).is_err() {
        return GraftResult::NoVbmeta;
    }

    let view = match avb_parse::parse_vbmeta(&vb) {
        Ok(v) => v,
        Err(_) => return GraftResult::NoVbmeta,
    };
    let pk = match view.public_key() {
        Some(p) => p,
        None => return GraftResult::NoVbmeta,
    };
    if !chain_pk.is_empty() {
        if pk == chain_pk {
            GraftResult::Ok
        } else {
            GraftResult::KeyMismatch
        }
    } else {
        GraftResult::Ok
    }
}

