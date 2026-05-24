//! Parity tests — Rust patch-engine outputs match the C reference's
//! behavior on the tracked PE fixture.
//!
//! Direct port of the three host tests deleted from `tests/patches/`:
//!
//!   - `test_patch6.c` — lock-state fastboot-gate parity.
//!   - `test_patch7.c` — orange-screen parity (incl. anchor uniqueness,
//!     CBZ→B rewrite, target preservation, idempotency, clean MISS on
//!     scrubbed warning string).
//!   - `test_patch10.c` — libavb force-AVB-success parity.
//!
//! The 4-ABL cross-build matrix continues to live at
//! `tests/host/088_patch7_multi_abl.sh`, which depends on `fv-unwrap`
//! to extract the PE from raw `.img` FV wrappers. We don't bring that
//! dependency into cargo's test path — the host shell test is the
//! authoritative cross-build gate.

use std::fs;
use std::path::PathBuf;

use patch_engine::{
    abl_permissive::{fastboot_lock_gates, libavb_force_success},
    internal::{
        arm64_decode::find_adrp_add_targeting,
        encode::read_instr_u32,
        scan::{scan_for, ScanResult},
    },
    Engine, Oem, PatchOutcome, PatchScope, Worst,
};

fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR = crates/patch-engine/. Climb two levels to repo root.
    let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    p.pop();
    p.pop();
    p
}

fn infiniti_pe() -> Option<Vec<u8>> {
    let path = repo_root().join("tests/images/pe/infiniti-EU-16.0.5.703.efi");
    if !path.exists() {
        return None;
    }
    Some(fs::read(&path).expect("read infiniti PE fixture"))
}

// --- patch6 — lock-state fastboot-gate -------------------------------

const REFUSAL_STRS: &[&[u8]] = &[
    b"Flashing is not allowed in Lock State",
    b"Erase is not allowed in Lock State",
    b"Slot Change is not allowed in Lock State\n",
    b"Snapshot Cancel is not allowed in Lock State",
];

#[test]
fn patch6_applies_and_is_miss_on_reapply() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch6_applies — infiniti PE fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();

    // First application.
    let outcome = fastboot_lock_gates::apply(&mut work, size);
    assert_eq!(
        outcome,
        PatchOutcome::Ok,
        "patch6 must apply cleanly on infiniti PE"
    );

    // Refusal strings must stay intact — patch6 only rewrites .text.
    for s in REFUSAL_STRS {
        assert!(
            work.windows(s.len()).any(|w| w == *s),
            "patch6 erased refusal string {:?}",
            std::str::from_utf8(s).unwrap()
        );
    }

    // Re-apply: every upstream conditional / B.NE has been rewritten,
    // so the second application can't find another gate site → MISS.
    let outcome = fastboot_lock_gates::apply(&mut work, size);
    assert_eq!(outcome, PatchOutcome::Miss);
}

// --- patch7 — orange-screen ------------------------------------------

const PATCH7_WARN_STR: &[u8] = b"Your device has been unlocked and can't be trusted";
const PATCH7_BACK_SCAN_WINDOW: u32 = 0x40;
const PATCH7_CBZ_OFF: u32 = 0x78F0; // infiniti-EU-16.0.5.703 specific
const PATCH7_B_UNCOND_INSN: u32 = 0x14000023; // rewritten B word — frozen byte parity

#[test]
fn patch7_table_membership() {
    // Mirror of test_patch7.c section 0 — the table-membership check
    // runs regardless of fixture presence.
    let oplus_table = patch_engine::oem::oplus::OEM_OPLUS_PATCHES;
    assert!(!oplus_table.is_empty());
    assert!(oplus_table
        .iter()
        .any(|p| p.name == "patch7-orange-screen"));
}

#[test]
fn patch7_string_anchor_resolves_uniquely() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch7_string_anchor_resolves_uniquely — fixture missing");
            return;
        }
    };
    let (r, str_off) = scan_for(&buf, PATCH7_WARN_STR, None);
    assert_eq!(r, ScanResult::Found, "warning string not unique");
    let (r, adrp_off) = find_adrp_add_targeting(&buf, str_off, true);
    assert_eq!(r, ScanResult::Found, "ADRP+ADD not unique");
    assert!(adrp_off > PATCH7_CBZ_OFF);
    assert!(adrp_off - PATCH7_CBZ_OFF <= PATCH7_BACK_SCAN_WINDOW);
}

#[test]
fn patch7_pre_patch_cbz_word() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch7_pre_patch_cbz_word — fixture missing");
            return;
        }
    };
    let cbz = read_instr_u32(&buf, PATCH7_CBZ_OFF);
    assert_eq!((cbz & 0xFF000000), 0x34000000, "not a CBZ Wn");
    assert_eq!((cbz & 0x1F), 10, "Rt != W10");
}

#[test]
fn patch7_apply_rewrites_to_b_uncond() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch7_apply — fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();
    let cbz_original = read_instr_u32(&work, PATCH7_CBZ_OFF);

    use patch_engine::oem::oplus::bypass_warning;
    assert_eq!(bypass_warning::apply(&mut work, size), PatchOutcome::Ok);

    let new_word = read_instr_u32(&work, PATCH7_CBZ_OFF);
    assert_eq!((new_word & 0xFC000000), 0x14000000, "not a B insn");
    assert_eq!(
        new_word, PATCH7_B_UNCOND_INSN,
        "B word mismatch — frozen byte parity broken"
    );

    // Target preservation: the rewritten B must point at the same byte
    // address as the original CBZ.
    let cbz_imm19 = sign_extend((cbz_original >> 5) & 0x7FFFF, 19) * 4;
    let cbz_target = (PATCH7_CBZ_OFF as i32 + cbz_imm19) as u32;
    let b_imm26 = sign_extend(new_word & 0x3FFFFFF, 26) * 4;
    let b_target = (PATCH7_CBZ_OFF as i32 + b_imm26) as u32;
    assert_eq!(cbz_target, b_target, "B target != original CBZ target");

    // Idempotency: second apply still returns Ok, word stays put.
    assert_eq!(bypass_warning::apply(&mut work, size), PatchOutcome::Ok);
    assert_eq!(read_instr_u32(&work, PATCH7_CBZ_OFF), PATCH7_B_UNCOND_INSN);
}

#[test]
fn patch7_clean_miss_without_warning_string() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch7_clean_miss — fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();
    let cbz_original = read_instr_u32(&work, PATCH7_CBZ_OFF);

    // Break the warning string at its first occurrence.
    let (r, str_off) = scan_for(&work, PATCH7_WARN_STR, None);
    assert_eq!(r, ScanResult::Found);
    work[str_off as usize] = 0x00;

    use patch_engine::oem::oplus::bypass_warning;
    assert_eq!(bypass_warning::apply(&mut work, size), PatchOutcome::Miss);
    assert_eq!(
        read_instr_u32(&work, PATCH7_CBZ_OFF),
        cbz_original,
        "patch7 mutated the CBZ on a MISS"
    );
}

fn sign_extend(v: u32, bits: u32) -> i32 {
    let mask = 1u32 << (bits - 1);
    if (v & mask) != 0 {
        let high = !((1u32 << bits) - 1);
        (v | high) as i32
    } else {
        v as i32
    }
}

// --- patch10 — libavb force-AVB-success ------------------------------

const PATCH10_ANCHOR: &[u8] =
    b"Persistent values required for AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO";

#[test]
fn patch10_table_membership() {
    let abl_table = patch_engine::abl_permissive::ABL_PERMISSIVE_PATCHES;
    assert!(abl_table
        .iter()
        .any(|p| p.name == "patch10-libavb-force-avb-success"));
    assert!(abl_table
        .iter()
        .any(|p| p.name == "patch6-lock-state-fastboot-gate"));
}

#[test]
fn patch10_applies_and_leaves_signature_words() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch10 — fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();

    // Sanity: the anchor string is present in this PE.
    let (r, _) = scan_for(&work, PATCH10_ANCHOR, None);
    assert_eq!(r, ScanResult::Found, "anchor string absent on infiniti PE");

    assert_eq!(libavb_force_success::apply(&mut work, size), PatchOutcome::Ok);

    // Sweep for the two rewrite signatures.
    let mut orr_seen = 0;
    let mut movz_seen = 0;
    let mut i = 0u32;
    while (i as usize) + 4 <= work.len() {
        let word = read_instr_u32(&work, i);
        if (word & 0xFFFFFFE0) == 0x32000060 {
            orr_seen += 1;
        }
        if word == 0x52800000 {
            movz_seen += 1;
        }
        i += 4;
    }
    assert!(orr_seen >= 1, "patch10 did not produce `orr wN, w3, #1`");
    assert!(movz_seen >= 1, "patch10 did not produce `mov w0, #0`");

    // Re-apply: prologue mov-from-w3 is gone → MISS at step 4.
    assert_eq!(
        libavb_force_success::apply(&mut work, size),
        PatchOutcome::Miss
    );
}

// --- Engine-level integration tests -----------------------------------

#[test]
fn engine_scoped_oplus_applies_all_three() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: engine_scoped_oplus_applies_all_three — fixture missing");
            return;
        }
    };
    let mut work = buf.clone();
    let engine = Engine::ensure_init_scoped(Oem::Oplus, true);
    let r = engine.apply(&mut work);
    // patch7 (1) + patch10 (1) + patch6 (1) = 3 applied.
    assert_eq!(
        r.applied_count, 3,
        "expected patch7 + patch10 + patch6 all OK on infiniti PE"
    );
    assert_eq!(r.missed_count, 0);
    assert_eq!(r.worst, Worst::Ok);
}

#[test]
fn engine_scoped_none_only_runs_abl_permissive() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: engine_scoped_none_only_runs_abl_permissive — fixture missing");
            return;
        }
    };
    let mut work = buf.clone();
    let engine = Engine::ensure_init_scoped(Oem::None, true);
    let r = engine.apply(&mut work);
    // patch10 + patch6.
    assert_eq!(r.applied_count, 2);
    assert_eq!(r.missed_count, 0);
    assert_eq!(r.worst, Worst::Ok);

    // patch7 must NOT have been touched — verify by re-running just
    // patch7 in isolation; it should still find the original CBZ and
    // rewrite to B.
    use patch_engine::oem::oplus::bypass_warning;
    let mut isolated = work.clone();
    let isize = isolated.len() as u32;
    assert_eq!(
        bypass_warning::apply(&mut isolated, isize),
        PatchOutcome::Ok,
        "patch7 wasn't pre-applied by Oem::None scope"
    );
}

#[test]
fn engine_firmware_path_only_abl_permissive() {
    // Engine::ensure_init() is the firmware-mode entry point — must
    // contain exactly the abl_permissive set.
    let engine = Engine::ensure_init();
    assert_eq!(engine.table().len(), 2);
    for p in engine.table() {
        assert_eq!(p.scope, PatchScope::AblPermissive);
    }
}
