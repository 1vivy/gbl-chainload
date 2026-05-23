//! Parity tests for `crates/mode2-profile-core`.
//!
//! Pins:
//!   1. `compile()` output for the canonical `good.toml` matches the
//!      frozen `tests/host/goldens/082/c.bin` (the byte string the C
//!      mode2-profile tool produced before the migration).
//!   2. `derive()` output for the infiniti vbmeta fixture, when
//!      re-encoded to TOML in the same shape the Python / C tools emit,
//!      matches `tools/mode2-profile/tests/baseline.toml.golden`.
//!   3. `parse()` accepts every golden it produces (round-trip
//!      via the wire form).
//!
//! Run with `cargo test -p mode2-profile-core --test parity`.

use std::path::PathBuf;

fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR is the crate directory; the workspace root is
    // two levels up (crates/<name> -> ../..).
    let crate_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    crate_dir.parent().unwrap().parent().unwrap().to_path_buf()
}

#[test]
fn compile_golden_082_byte_match() {
    // The frozen byte string is what the C mode2-profile tool produced
    // for tests/host/.last/082/good.toml — see
    // tests/host/082_mode2_profile_parity.sh:
    //
    //   version        = 1
    //   is_unlocked    = 0
    //   color          = 0
    //   system_version = 0x40000
    //   system_spl     = 0x9A4
    //   rot_digest     = "11...11"
    //   pubkey_digest  = "22...22"
    //   vbh            = "33...33"
    let toml = "version        = 1\n\
                is_unlocked    = 0\n\
                color          = 0\n\
                system_version = 0x40000\n\
                system_spl     = 0x9A4\n\
                rot_digest     = \"1111111111111111111111111111111111111111111111111111111111111111\"\n\
                pubkey_digest  = \"2222222222222222222222222222222222222222222222222222222222222222\"\n\
                vbh            = \"3333333333333333333333333333333333333333333333333333333333333333\"\n";

    let bin = mode2_profile_core::compile(toml).expect("good.toml compiles");
    let golden = std::fs::read(repo_root().join("tests/host/goldens/082/c.bin"))
        .expect("082 golden present");
    assert_eq!(bin, golden, "Rust compile() diverged from frozen 082 golden");
}

#[test]
fn compile_rejects_color_9() {
    let toml = "version        = 1\n\
                is_unlocked    = 0\n\
                color          = 9\n\
                system_version = 0x40000\n\
                system_spl     = 0x9A4\n\
                rot_digest     = \"1111111111111111111111111111111111111111111111111111111111111111\"\n\
                pubkey_digest  = \"2222222222222222222222222222222222222222222222222222222222222222\"\n\
                vbh            = \"3333333333333333333333333333333333333333333333333333333333333333\"\n";
    assert!(matches!(
        mode2_profile_core::compile(toml),
        Err(mode2_profile_core::CompileError::OutOfRange { key: "color", .. })
    ));
}

#[test]
fn derive_infiniti_vbmeta_matches_golden() {
    // The fixture is only present in dev trees with the OEM bundle
    // unpacked; skip if absent (matches the host-test SKIP pattern in
    // tests/host/079_mode2_profile_derive.sh).
    let vbmeta_path = repo_root().join("tests/images/vbmeta-infiniti-IN-16.0.7.201.img");
    if !vbmeta_path.exists() {
        eprintln!("SKIP: derive_infiniti_vbmeta_matches_golden — fixture {} missing",
                  vbmeta_path.display());
        return;
    }
    let vbmeta = std::fs::read(&vbmeta_path).expect("vbmeta read");
    let profile = mode2_profile_core::derive(&vbmeta).expect("derive succeeds");

    // Cross-check the key derived fields against the captured golden.
    // (The golden TOML is the textual form the C tool emitted; we check
    // the underlying binary values here. The TOML byte-identity test is
    // tests/host/087, which compares against the same fixture.)
    //
    // Path note (Task 10): Task 8 deleted tools/mode2-profile/ and
    // relocated the captured-pre-Rust C-tool TOML golden under
    // tests/host/goldens/087/baseline.toml. This test now reads from
    // the relocated path.
    let golden_toml = std::fs::read_to_string(
        repo_root().join("tests/host/goldens/087/baseline.toml"),
    )
    .expect("golden TOML read");

    // Pull the rot_digest / pubkey_digest / vbh + the system_version /
    // system_spl values out of the TOML and compare.
    let expected_rot = grep_hex(&golden_toml, "rot_digest");
    let expected_pk = grep_hex(&golden_toml, "pubkey_digest");
    let expected_vbh = grep_hex(&golden_toml, "vbh");
    let expected_sv = grep_u32(&golden_toml, "system_version");
    let expected_spl = grep_u32(&golden_toml, "system_spl");

    assert_eq!(hex::encode(profile.rot_digest), expected_rot, "rot_digest");
    assert_eq!(hex::encode(profile.pubkey_digest), expected_pk, "pubkey_digest");
    assert_eq!(hex::encode(profile.vbh), expected_vbh, "vbh");
    assert_eq!(profile.system_version, expected_sv, "system_version");
    assert_eq!(profile.system_spl, expected_spl, "system_spl");
    assert_eq!(profile.is_unlocked, 0);
    assert_eq!(profile.color, 0);
}

#[test]
fn parse_round_trip_on_compile_output() {
    let toml = "version        = 1\n\
                is_unlocked    = 1\n\
                color          = 3\n\
                system_version = 0x40000\n\
                system_spl     = 0x9A5\n\
                rot_digest     = \"4444444444444444444444444444444444444444444444444444444444444444\"\n\
                pubkey_digest  = \"5555555555555555555555555555555555555555555555555555555555555555\"\n\
                vbh            = \"6666666666666666666666666666666666666666666666666666666666666666\"\n";
    let bin = mode2_profile_core::compile(toml).expect("compile");
    let p = mode2_profile_core::parse(&bin).expect("round-trip parse");
    assert_eq!(p.version, 1);
    assert_eq!(p.is_unlocked, 1);
    assert_eq!(p.color, 3);
    assert_eq!(p.system_version, 0x40000);
    assert_eq!(p.system_spl, 0x9A5);
    assert_eq!(p.rot_digest[0], 0x44);
    assert_eq!(p.pubkey_digest[0], 0x55);
    assert_eq!(p.vbh[0], 0x66);
}

// --- Tiny TOML helpers --------------------------------------------
//
// Used by derive_infiniti_vbmeta_matches_golden — we parse the golden
// by hand to keep the parity test free of an extra serde dep. The
// golden has a known fixed shape.

fn grep_hex(s: &str, key: &str) -> String {
    let prefix = format!("{}", key);
    for line in s.lines() {
        let line = line.trim();
        if line.starts_with(&prefix) {
            // `key            = "..."` — grab between the quotes.
            let start = line.find('"').expect("opening quote") + 1;
            let end = line.rfind('"').expect("closing quote");
            return line[start..end].to_string();
        }
    }
    panic!("key {} not found in golden TOML", key)
}

fn grep_u32(s: &str, key: &str) -> u32 {
    let prefix = format!("{}", key);
    for line in s.lines() {
        let line = line.trim();
        if line.starts_with(&prefix) {
            // `key            = 0xNN` or decimal.
            let eq = line.find('=').expect("=");
            let v = line[eq + 1..].trim();
            return if let Some(hex) = v.strip_prefix("0x") {
                u32::from_str_radix(hex, 16).expect("hex")
            } else {
                v.parse().expect("decimal")
            };
        }
    }
    panic!("key {} not found in golden TOML", key)
}

mod hex {
    // Tiny lowercase-hex encoder. Avoids pulling in the `hex` crate
    // just for one assertion.
    pub fn encode(bytes: impl AsRef<[u8]>) -> String {
        const HEX: &[u8; 16] = b"0123456789abcdef";
        let bytes = bytes.as_ref();
        let mut out = String::with_capacity(bytes.len() * 2);
        for b in bytes {
            out.push(HEX[(b >> 4) as usize] as char);
            out.push(HEX[(b & 0xf) as usize] as char);
        }
        out
    }
}
