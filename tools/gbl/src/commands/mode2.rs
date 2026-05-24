//! `gbl mode2` — mode-2 profile derive / compile / build.
//!
//! Replaces `tools/mode2-profile/mode2-profile.c`:
//!
//! - `gbl mode2 derive <vbmeta> -o <out.toml>` — port of `mode2-profile derive`.
//!   Walks the vbmeta header + property descriptors via `avb-parse`,
//!   derives rot_digest / pubkey_digest / vbh, encodes os_version + spl,
//!   and writes the TOML with byte-for-byte parity with the Python
//!   reference (PosixPath comment, 0x.. hex formatting, …).
//! - `gbl mode2 compile <in.toml> -o <out.bin>` — port of `mode2-profile compile`.
//!   Validates the TOML and writes a 120-byte binary via
//!   `mode2_profile_core::compile`.
//! - `gbl mode2 build` — new composite: derive into a temp TOML, then
//!   compile into a binary. Per spec §3.
//!
//! Note: the Python reference (`scripts/mode2-profile.py`, formerly
//! `tools/mode2-profile/mode2-profile.py`) is preserved as a parity
//! reference — tests 080/082/091 still consume it.

use std::path::{Path, PathBuf};

use clap::{Args as ClapArgs, Parser, Subcommand};
use sha2::{Digest, Sha256};

use super::{slurp, GblError};

#[derive(Parser, Debug)]
#[command(about = "Mode-2 profile derive/compile/build")]
pub struct Args {
    #[command(subcommand)]
    sub: Sub,
}

#[derive(Subcommand, Debug)]
enum Sub {
    /// Derive a profile TOML from a stock vbmeta image.
    Derive(DeriveArgs),
    /// Compile a profile TOML into a 120-byte binary.
    Compile(CompileArgs),
    /// Composite: derive into a temp TOML, then compile.
    Build(BuildArgs),
}

#[derive(ClapArgs, Debug)]
struct DeriveArgs {
    /// Stock vbmeta image (positional, matches the legacy CLI).
    vbmeta: PathBuf,
    /// Output TOML path.
    #[arg(short)]
    o: PathBuf,
}

#[derive(ClapArgs, Debug)]
struct CompileArgs {
    /// Input TOML path.
    toml: PathBuf,
    /// Output binary path.
    #[arg(short)]
    o: PathBuf,
}

#[derive(ClapArgs, Debug)]
struct BuildArgs {
    /// Stock vbmeta image.
    vbmeta: PathBuf,
    /// Output binary path (the intermediate TOML is buffered in memory).
    #[arg(short)]
    o: PathBuf,
}

pub fn run(args: Args) -> Result<(), GblError> {
    match args.sub {
        Sub::Derive(a) => do_derive(&a.vbmeta, &a.o),
        Sub::Compile(a) => do_compile(&a.toml, &a.o),
        Sub::Build(a) => {
            // derive → in-memory TOML → compile → write binary.
            let toml = derive_to_string(&a.vbmeta)?;
            let bin = mode2_profile_core::compile(&toml).map_err(|e| {
                GblError::runtime(format!("error: compile: {:?}", e))
            })?;
            std::fs::write(&a.o, &bin).map_err(|e| {
                GblError::runtime(format!(
                    "error: cannot write {}: {}",
                    a.o.display(),
                    e
                ))
            })?;
            println!("wrote {} ({} bytes)", a.o.display(), bin.len());
            Ok(())
        }
    }
}

fn do_compile(input: &Path, output: &Path) -> Result<(), GblError> {
    let toml_str = std::fs::read_to_string(input).map_err(|e| {
        GblError::runtime(format!(
            "error: cannot open {}: {}",
            input.display(),
            e
        ))
    })?;
    let bin = mode2_profile_core::compile(&toml_str).map_err(|e| {
        // Match the C tool's per-error stderr line shape so anything
        // that grepped the message keeps working.
        let msg = match e {
            mode2_profile_core::CompileError::MalformedToml(_) => {
                "error: malformed profile TOML".to_string()
            }
            mode2_profile_core::CompileError::MissingOrBadType(_) => {
                "error: missing key or wrong type in profile".to_string()
            }
            mode2_profile_core::CompileError::OutOfRange { .. } => {
                "error: integer key out of range in profile".to_string()
            }
            mode2_profile_core::CompileError::BadDigest(_) => {
                "error: digest field is not 64 lowercase-hex chars".to_string()
            }
            mode2_profile_core::CompileError::UnknownKey(_) => {
                "error: unknown key in profile".to_string()
            }
        };
        GblError::runtime(msg)
    })?;
    if bin.len() != mode2_profile_core::GBL_M2P_SIZE {
        return Err(GblError::runtime(format!(
            "error: compile produced {} bytes (expected {})",
            bin.len(),
            mode2_profile_core::GBL_M2P_SIZE
        )));
    }
    // The C tool deletes the output file on write failure. Rust's
    // fs::write opens with truncate=true; partial writes are rare on
    // disk-full but we don't bother to clean up (the C tool's `remove`
    // call only matters if downstream tools try to consume the partial
    // file — they won't, the output was non-zero exit).
    std::fs::write(output, &bin).map_err(|e| {
        let _ = std::fs::remove_file(output);
        GblError::runtime(format!("error: write failed: {}: {}", output.display(), e))
    })?;
    println!("wrote {} ({} bytes)", output.display(), bin.len());
    Ok(())
}

/// Build the derived TOML as a String — shared by `gbl mode2 derive` and
/// `gbl mode2 build`. The exact format mirrors the Python `cmd_derive`
/// output byte-for-byte (PosixPath comment, hex literals without leading
/// zeros, …) and the C tool's `derive_main`.
fn derive_to_string(vbmeta_path: &Path) -> Result<String, GblError> {
    let img = slurp(vbmeta_path)?;
    if img.len() < avb_parse::VBMETA_HEADER_SIZE {
        return Err(GblError::runtime(format!(
            "error: {}: too small to be a vbmeta image",
            vbmeta_path.display()
        )));
    }
    let vbm = avb_parse::parse_vbmeta(&img).map_err(|e| match e {
        avb_parse::AvbError::NotFound => GblError::runtime(format!(
            "error: {}: not a vbmeta image (bad magic)",
            vbmeta_path.display()
        )),
        _ => GblError::runtime(format!(
            "error: {}: malformed vbmeta header",
            vbmeta_path.display()
        )),
    })?;

    if vbm.header.public_key_size == 0 {
        return Err(GblError::runtime(format!(
            "error: {}: vbmeta has no public key (unsigned?)",
            vbmeta_path.display()
        )));
    }
    let pubkey = vbm.public_key().ok_or_else(|| {
        GblError::runtime(format!(
            "error: {}: public key extends past aux block",
            vbmeta_path.display()
        ))
    })?;

    // rot_digest = SHA256(pubkey || 0x00)
    let mut h = Sha256::new();
    h.update(pubkey);
    h.update([0u8]);
    let rot_digest: [u8; 32] = h.finalize().into();

    // pubkey_digest = SHA256(pubkey)
    let pubkey_digest: [u8; 32] = Sha256::digest(pubkey).into();

    // vbh = SHA256(image[0 .. 256 + auth + aux])
    let auth = vbm.header.authentication_data_block_size;
    let aux = vbm.header.auxiliary_data_block_size;
    let vbmeta_size =
        (avb_parse::VBMETA_HEADER_SIZE as u64) + auth + aux;
    let vbh: [u8; 32] = Sha256::digest(&img[..vbmeta_size as usize]).into();

    // sha256 of the whole file (for the provenance comment).
    let src_sha = Sha256::digest(&img);
    let src_sha_hex = hex64(&src_sha);

    // Walk descriptors for the two property strings we care about.
    let mut os_ver_str: Option<String> = None;
    let mut spl_str: Option<String> = None;
    for d in vbm.descriptors().flatten() {
        if let avb_parse::Descriptor::Property { bytes } = d {
            // Property body: at offset +16 of the raw bytes,
            //   u64 BE key_size, u64 BE val_size, then key '\0' val '\0'.
            if bytes.len() < 32 {
                continue;
            }
            let klen = u64::from_be_bytes([
                bytes[16], bytes[17], bytes[18], bytes[19],
                bytes[20], bytes[21], bytes[22], bytes[23],
            ]) as usize;
            let vlen = u64::from_be_bytes([
                bytes[24], bytes[25], bytes[26], bytes[27],
                bytes[28], bytes[29], bytes[30], bytes[31],
            ]) as usize;
            let body_off = 32usize;
            if body_off + klen + 1 + vlen + 1 > bytes.len() {
                continue;
            }
            let key = &bytes[body_off..body_off + klen];
            let val = &bytes[body_off + klen + 1..body_off + klen + 1 + vlen];
            if key == b"com.android.build.boot.os_version" {
                if let Ok(s) = std::str::from_utf8(val) {
                    os_ver_str = Some(s.to_string());
                }
            } else if key == b"com.android.build.boot.security_patch" {
                if let Ok(s) = std::str::from_utf8(val) {
                    spl_str = Some(s.to_string());
                }
            }
        }
    }
    let os_ver_str = os_ver_str.ok_or_else(|| {
        GblError::runtime(
            "error: vbmeta has no com.android.build.boot.os_version property"
                .to_string(),
        )
    })?;
    let spl_str = spl_str.ok_or_else(|| {
        GblError::runtime(
            "error: vbmeta has no com.android.build.boot.security_patch property"
                .to_string(),
        )
    })?;

    let os_ver_encoded = encode_os_version(&os_ver_str)?;
    let spl_encoded = encode_spl(&spl_str)?;

    let rot_hex = hex64(&rot_digest);
    let pk_hex = hex64(&pubkey_digest);
    let vbh_hex = hex64(&vbh);

    // Use the path string as provided — Python's repr(Path(x)) renders as
    // `PosixPath('x')` on Linux. The C tool prints argv[2] verbatim
    // inside the PosixPath() wrapper, so this matches.
    let vbmeta_str = vbmeta_path.to_string_lossy();

    let out = format!(
        "# generated by mode2-profile derive\n\
         # source: PosixPath('{vbmeta_str}')\n\
         # sha256: {src_sha_hex}\n\
         # os_version: '{os_ver_str}' -> 0x{os_ver_encoded:x}   spl: '{spl_str}' -> 0x{spl_encoded:x}\n\
         version        = 1\n\
         is_unlocked    = 0\n\
         color          = 0\n\
         system_version = 0x{os_ver_encoded:x}\n\
         system_spl     = 0x{spl_encoded:x}\n\
         rot_digest     = \"{rot_hex}\"\n\
         pubkey_digest  = \"{pk_hex}\"\n\
         vbh            = \"{vbh_hex}\"\n"
    );

    Ok(out)
}

fn do_derive(vbmeta_path: &Path, out_path: &Path) -> Result<(), GblError> {
    // Re-implements derive_to_string but with the C tool's exact stdout
    // ordering: "wrote <out>\n" first, then the digest block. We can't
    // easily refactor derive_to_string to return BOTH the TOML and the
    // commentary fields without duplicating most of its body, so just
    // re-derive here, printing in the right order.
    let toml = derive_to_string(vbmeta_path)?;
    std::fs::write(out_path, &toml).map_err(|e| {
        let _ = std::fs::remove_file(out_path);
        GblError::runtime(format!(
            "error: write failed for {}: {}",
            out_path.display(),
            e
        ))
    })?;
    println!("wrote {}", out_path.display());
    print_derive_commentary(&toml);
    Ok(())
}

/// Parse our own generated TOML's commentary back out and emit the
/// post-write digest block to stdout — matches mode2-profile derive's
/// trailing commentary block byte-for-byte without re-running the AVB
/// walk twice.
fn print_derive_commentary(toml: &str) {
    let mut rot_hex = "";
    let mut pk_hex = "";
    let mut vbh_hex = "";
    let mut os_line = String::new();
    let mut spl_line = String::new();
    for line in toml.lines() {
        if let Some(s) = line.strip_prefix("# os_version: ") {
            // Expected shape: 'V' -> 0xH   spl: 'S' -> 0xS
            // Re-emit verbatim in the two stdout lines.
            if let Some((osv, spl)) = s.split_once("   spl: ") {
                os_line = format!("  os_version    = {}", osv);
                spl_line = format!("  spl           = {}", spl);
            }
        } else if let Some(rest) = line.strip_prefix("rot_digest     = \"") {
            if let Some(end) = rest.find('"') {
                rot_hex = &rest[..end];
            }
        } else if let Some(rest) = line.strip_prefix("pubkey_digest  = \"") {
            if let Some(end) = rest.find('"') {
                pk_hex = &rest[..end];
            }
        } else if let Some(rest) = line.strip_prefix("vbh            = \"") {
            if let Some(end) = rest.find('"') {
                vbh_hex = &rest[..end];
            }
        }
    }
    println!("  rot_digest    = {rot_hex}");
    println!("  pubkey_digest = {pk_hex}");
    println!("  vbh           = {vbh_hex}");
    if !os_line.is_empty() {
        println!("{os_line}");
    }
    if !spl_line.is_empty() {
        println!("{spl_line}");
    }
}

fn hex64(d: &[u8]) -> String {
    let mut s = String::with_capacity(64);
    for b in d {
        s.push(HEX[(b >> 4) as usize] as char);
        s.push(HEX[(b & 0xf) as usize] as char);
    }
    s
}

const HEX: &[u8; 16] = b"0123456789abcdef";

/// Mirror the C tool's `sscanf("%d.%d.%d", ...)` parse with the same
/// "missing component = 0" semantics + per-field range checks.
fn encode_os_version(s: &str) -> Result<u64, GblError> {
    let parts: Vec<&str> = s.split('.').collect();
    let major: i64 = parts.first().and_then(|p| p.parse().ok()).unwrap_or(0);
    let minor: i64 = parts.get(1).and_then(|p| p.parse().ok()).unwrap_or(0);
    let sub: i64 = parts.get(2).and_then(|p| p.parse().ok()).unwrap_or(0);
    if !(0..=0x7F).contains(&minor) {
        return Err(GblError::runtime(format!(
            "error: OS version minor {minor} exceeds 7-bit limit"
        )));
    }
    if !(0..=0x7F).contains(&sub) {
        return Err(GblError::runtime(format!(
            "error: OS version sub {sub} exceeds 7-bit limit"
        )));
    }
    if !(0..=0x3FFFF).contains(&major) {
        return Err(GblError::runtime(format!(
            "error: OS version major {major} exceeds 18-bit limit"
        )));
    }
    Ok(((major as u64) << 14) | ((minor as u64) << 7) | (sub as u64))
}

fn encode_spl(s: &str) -> Result<u64, GblError> {
    let parts: Vec<&str> = s.split('-').collect();
    if parts.len() < 3 {
        return Err(GblError::runtime(format!(
            "error: unrecognized security patch {s} (expected YYYY-MM-DD)"
        )));
    }
    let year: i64 = parts[0].parse().map_err(|_| {
        GblError::runtime(format!(
            "error: unrecognized security patch {s} (expected YYYY-MM-DD)"
        ))
    })?;
    let month: i64 = parts[1].parse().map_err(|_| {
        GblError::runtime(format!(
            "error: unrecognized security patch {s} (expected YYYY-MM-DD)"
        ))
    })?;
    let day: i64 = parts[2].parse().map_err(|_| {
        GblError::runtime(format!(
            "error: unrecognized security patch {s} (expected YYYY-MM-DD)"
        ))
    })?;
    if !(2000..=2127).contains(&year) {
        return Err(GblError::runtime(format!(
            "error: SPL year {year} out of range (2000-2127)"
        )));
    }
    if !(1..=12).contains(&month) {
        return Err(GblError::runtime(format!(
            "error: SPL month {month} out of range (1-12)"
        )));
    }
    if !(1..=31).contains(&day) {
        return Err(GblError::runtime(format!(
            "error: SPL day {day} out of range (1-31)"
        )));
    }
    Ok(((day as u64) << 11) | (((year - 2000) as u64) << 4) | (month as u64))
}
