//! Build script for the gbl-commit Rust pilot.
//!
//! Two jobs, both demonstrating the migration pattern the parser tools will
//! reuse:
//!   1. Compile and statically link the *existing* C SHA-256 from
//!      GblChainloadPkg, so the digest is byte-identical to the EFI payload and
//!      the C tools — no reimplementation, no parity drift. (gblp1-inspect /
//!      gbl-pack reuse this for Crc32.c; mode2-profile for AvbParseLib.)
//!   2. Inject GBL_TOOL_VERSION from the top-level VERSION file — the same
//!      single-source-of-truth the C Makefiles use.

use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    // tools-rs/gbl-commit -> repo root is two levels up.
    let root = manifest
        .parent()
        .and_then(|p| p.parent())
        .expect("repo root from tools-rs/gbl-commit");
    let gpl = root.join("GblChainloadPkg/Library/GblPayloadLib");
    let sha_c = gpl.join("Sha256.c");

    cc::Build::new()
        .file(&sha_c)
        .include(&gpl) // resolves #include "Internal/Sha256.h"
        .define("GBL_HOST_BUILD", Some("1"))
        .flag_if_supported("-std=c99")
        .warnings(false)
        .compile("gbl_sha256_c");

    let version = std::fs::read_to_string(root.join("VERSION"))
        .expect("read VERSION");
    let version = version.trim();
    assert!(!version.is_empty(), "VERSION file is empty");
    println!("cargo:rustc-env=GBL_TOOL_VERSION={version}");

    println!("cargo:rerun-if-changed={}", sha_c.display());
    println!("cargo:rerun-if-changed={}", root.join("VERSION").display());
}
