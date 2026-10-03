//! Host-only Mode 2 patch: make both ABL BCC population paths use Normal DICE mode.
//!
//! The source has three choices in `PopulateBccParams` and a separate Debug
//! assignment in `SetDummyBccParams`. In the supported ABL PEs the former is
//! compiled as a conditional select, not a writable data table. Anchor each
//! site through its unique DEBUG string and validate the surrounding code;
//! refuse an unfamiliar build rather than changing an unrelated immediate.

use crate::internal::arm64_decode::find_adrp_add_targeting;
use crate::internal::pe_sections::is_pe_file_offset_in_executable_section;
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

const MAIN_LOG: &[u8] = b"VB: PopulateBccParams: Parameter receivedis NULL\0";
const DUMMY_LOG: &[u8] = b"VB: Setting Dummy DICE params\n\0";
const SELECT_MODE: u32 = 0x1a89_0108; // csel w8, w8, w9, eq
const NORMAL_MODE: u32 = 0x5280_0028; // mov w8, #1
const DEBUG_MODE: u32 = 0x5280_0048; // mov w8, #2
const NOP: u32 = 0xd503_201f;

fn word(buf: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(buf[off..off + 4].try_into().unwrap())
}

fn log_reference(buf: &[u8], text: &[u8]) -> Result<usize, PatchOutcome> {
    let (status, string) = scan_for(buf, text, None);
    match status {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return Err(PatchOutcome::Ambiguous),
        _ => return Err(PatchOutcome::Miss),
    }
    let (status, reference) = find_adrp_add_targeting(buf, string, true);
    match status {
        ScanResult::Found => Ok(reference as usize),
        ScanResult::Ambiguous => Err(PatchOutcome::Ambiguous),
        _ => Err(PatchOutcome::Miss),
    }
}

fn main_site(buf: &[u8]) -> Result<usize, PatchOutcome> {
    let reference = log_reference(buf, MAIN_LOG)?;
    let start = reference.saturating_sub(0x200);
    let mut site = None;
    for off in (start..reference).step_by(4) {
        if off < 0x30 || off + 20 > buf.len()
            || !is_pe_file_offset_in_executable_section(buf, off as u32, 4)
            || !matches!(word(buf, off), SELECT_MODE | NORMAL_MODE)
        {
            continue;
        }
        // The select follows `cinc w8,w10,ne` and `mov w9,#3`; the selected
        // value is then stored into the BCC params global. Instruction
        // scheduling differs slightly between builds, hence bounded windows.
        let mut before = (off - 0x30..off).step_by(4);
        if !before.clone().any(|p| word(buf, p) == 0x1a8a_0548)
            || !before.any(|p| word(buf, p) == 0x5280_0069)
            || !(off + 4..off + 20).step_by(4).any(|p| {
                let w = word(buf, p);
                w & 0xffc0_03ff == 0xb900_0128 // str w8, [x9, #imm]
            })
        {
            continue;
        }
        if site.replace(off).is_some() {
            return Err(PatchOutcome::Ambiguous);
        }
    }
    site.ok_or(PatchOutcome::Miss)
}

fn dummy_site(buf: &[u8]) -> Result<usize, PatchOutcome> {
    let reference = log_reference(buf, DUMMY_LOG)?;
    let end = (reference + 0x38).min(buf.len().saturating_sub(4));
    let mut site = None;
    for off in (reference + 8..end).step_by(4) {
        if off < 4 || off + 8 > buf.len()
            || !is_pe_file_offset_in_executable_section(buf, (off - 4) as u32, 12)
            || !matches!(word(buf, off), DEBUG_MODE | NORMAL_MODE)
            || !matches!(word(buf, off - 4), 0x3400_0068 | NOP)
            || word(buf, off + 4) != 0xb900_6268 // str w8, [x19,#0x60]
        {
            continue;
        }
        if site.replace(off).is_some() {
            return Err(PatchOutcome::Ambiguous);
        }
    }
    site.ok_or(PatchOutcome::Miss)
}

/// Patch both paths atomically, leaving unknown or ambiguous PEs unchanged.
/// The host ABL patch table applies this for every prepared image.
pub fn apply(buf: &mut [u8]) -> PatchOutcome {
    let main = match main_site(buf) {
        Ok(off) => off,
        Err(outcome) => return outcome,
    };
    let dummy = match dummy_site(buf) {
        Ok(off) => off,
        Err(outcome) => return outcome,
    };
    buf[main..main + 4].copy_from_slice(&NORMAL_MODE.to_le_bytes());
    // The dummy struct is memset to zero; its old CBZ skipped the mode
    // store when locked. Remove that guard so even the dummy path is Normal.
    buf[dummy - 4..dummy].copy_from_slice(&NOP.to_le_bytes());
    buf[dummy..dummy + 4].copy_from_slice(&NORMAL_MODE.to_le_bytes());
    PatchOutcome::Ok
}
