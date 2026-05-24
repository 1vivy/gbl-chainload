//! Minimal AArch64 instruction decoders — port of
//! `GblChainloadPkg/Library/DynamicPatchLib/Internal/Arm64Decode.c`.
//!
//! Only the surfaces the patch engine needs today:
//!   - `B / BL` (unconditional + link)
//!   - `B.cond`
//!   - `CBZ Wn / CBNZ Wn / CBZ Xn / CBNZ Xn`
//!   - `ADRP + ADD` pair (resolve load-address)

use super::encode::read_instr_u32;
use super::pe_sections::is_pe_file_offset_in_executable_section;
use super::scan::ScanResult;

/// Recognized branch kinds.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Arm64InsnKind {
    Other,
    CbzW,
    CbnzW,
    CbzX,
    CbnzX,
    Bcond,
    B,
    Bl,
}

#[inline]
fn sign_extend(value: u32, bits: u32) -> i32 {
    let mask = 1u32 << (bits - 1);
    if (value & mask) != 0 {
        // Set all bits above (bits-1).
        let high_mask = !((1u32 << bits) - 1);
        (value | high_mask) as i32
    } else {
        value as i32
    }
}

/// Decoded view of a recognized AArch64 branch instruction.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub struct DecodedBranch {
    pub kind: Arm64InsnKind,
    pub target_off: u32,
    /// For CBZ/CBNZ — the Rt register (0..=31). For B.cond — the
    /// condition code (0..=15). Undefined for B / BL.
    pub reg_or_cond: u32,
}

/// Decode a single AArch64 branch instruction.
///
/// Returns `None` if the word is not one of the recognized branch kinds.
pub fn decode_branch(insn: u32, insn_off: u32) -> Option<DecodedBranch> {
    let top = (insn >> 24) & 0xFF;
    let top6 = (insn >> 26) & 0x3F;

    // B unconditional: 0b000101_imm26
    if top6 == 0x05 {
        let off = sign_extend(insn & 0x03FF_FFFF, 26) * 4;
        let tgt = (insn_off as i32).wrapping_add(off) as u32;
        return Some(DecodedBranch {
            kind: Arm64InsnKind::B,
            target_off: tgt,
            reg_or_cond: 0,
        });
    }
    // BL: 0b100101_imm26
    if top6 == 0x25 {
        let off = sign_extend(insn & 0x03FF_FFFF, 26) * 4;
        let tgt = (insn_off as i32).wrapping_add(off) as u32;
        return Some(DecodedBranch {
            kind: Arm64InsnKind::Bl,
            target_off: tgt,
            reg_or_cond: 0,
        });
    }
    // B.cond: 0x54 top byte, bit 4 must be 0
    if top == 0x54 && (insn & 0x10) == 0 {
        let off = sign_extend((insn >> 5) & 0x7_FFFF, 19) * 4;
        let tgt = (insn_off as i32).wrapping_add(off) as u32;
        return Some(DecodedBranch {
            kind: Arm64InsnKind::Bcond,
            target_off: tgt,
            reg_or_cond: insn & 0xF,
        });
    }
    // CBZ/CBNZ W/X — top byte 0x34/0x35/0xB4/0xB5
    if top == 0x34 || top == 0x35 || top == 0xB4 || top == 0xB5 {
        let off = sign_extend((insn >> 5) & 0x7_FFFF, 19) * 4;
        let tgt = (insn_off as i32).wrapping_add(off) as u32;
        let kind = match top {
            0x34 => Arm64InsnKind::CbzW,
            0x35 => Arm64InsnKind::CbnzW,
            0xB4 => Arm64InsnKind::CbzX,
            _ => Arm64InsnKind::CbnzX,
        };
        return Some(DecodedBranch {
            kind,
            target_off: tgt,
            reg_or_cond: insn & 0x1F,
        });
    }
    None
}

/// ADRP: `1_xx_10000` top form. Returns `(page_base, rd)`.
fn decode_adrp(insn: u32, adrp_off: u32) -> Option<(u32, u32)> {
    if (insn & 0x9F00_0000) != 0x9000_0000 {
        return None;
    }
    let imm_lo = (insn >> 29) & 0x3;
    let imm_hi = (insn >> 5) & 0x7_FFFF;
    let imm21 = (imm_hi << 2) | imm_lo;
    let signed = sign_extend(imm21, 21);
    // Page-relative: (PC & ~0xFFF) + (imm21 << 12). The C uses uint32
    // arithmetic on signed shift; we mirror that with wrapping_add.
    let page_base = (adrp_off & !0xFFFu32).wrapping_add((signed << 12) as u32);
    let rd = insn & 0x1F;
    Some((page_base, rd))
}

/// ADD imm (sh=0): `1xxx_0010_0010_xxxxxxxxxxxx_xxxxx_xxxxx`. Returns
/// `(imm12, rn, rd)`.
fn decode_add_imm(insn: u32) -> Option<(u32, u32, u32)> {
    if (insn & 0x7F80_0000) != 0x1100_0000 {
        return None;
    }
    let imm12 = (insn >> 10) & 0xFFF;
    let rn = (insn >> 5) & 0x1F;
    let rd = insn & 0x1F;
    Some((imm12, rn, rd))
}

/// Decode an ADRP at `adrp_off` together with an ADD-immediate at
/// `adrp_off + 4`, resolving the combined byte address they load.
///
/// Requires the ADD's `Rd == Rn == ADRP's Rd` — i.e. the canonical
/// `ADD Rd, Rd, #imm12` pointer-construction shape.
pub fn decode_adrp_add(buf: &[u8], adrp_off: u32) -> Option<u32> {
    let adrp_off_u = adrp_off as usize;
    if adrp_off_u + 8 > buf.len() {
        return None;
    }
    let adrp_insn = read_instr_u32(buf, adrp_off);
    let add_insn = read_instr_u32(buf, adrp_off + 4);

    let (page_base, adrp_rd) = decode_adrp(adrp_insn, adrp_off)?;
    let (imm12, add_rn, add_rd) = decode_add_imm(add_insn)?;
    if add_rn != adrp_rd || add_rd != adrp_rd {
        return None;
    }
    Some(page_base + imm12)
}

/// Walk every 4-byte-aligned offset in `buf` and count ADRP+ADD pairs
/// whose decoded target equals `target_addr`. Optionally restricted to
/// pairs whose ADRP location lies inside an executable PE section.
pub fn find_adrp_add_targeting(
    buf: &[u8],
    target_addr: u32,
    restrict_to_exec: bool,
) -> (ScanResult, u32) {
    if buf.len() < 8 {
        return (ScanResult::BadInput, 0);
    }
    let mut found: u32 = 0;
    let mut first_off: u32 = 0;
    let size = buf.len();
    let mut off = 0u32;
    while (off as usize) + 8 <= size {
        if restrict_to_exec
            && !is_pe_file_offset_in_executable_section(buf, off, 8)
        {
            off += 4;
            continue;
        }
        if let Some(resolved) = decode_adrp_add(buf, off) {
            if resolved == target_addr {
                if found == 0 {
                    first_off = off;
                }
                found += 1;
            }
        }
        off += 4;
    }

    match found {
        0 => (ScanResult::NotFound, 0),
        1 => (ScanResult::Found, first_off),
        _ => (ScanResult::Ambiguous, 0),
    }
}

/// Walk every 4-byte-aligned offset in `buf` and count branches that
/// target `target_off`.
///
/// Recognized: `CBZ_W`, `CBNZ_W`, `CBZ_X`, `CBNZ_X`, `Bcond`. `B / BL`
/// are intentionally excluded — patch6 is only interested in conditional
/// gates.
pub fn find_cond_branch_targeting(
    buf: &[u8],
    target_off: u32,
    restrict_to_exec: bool,
) -> (ScanResult, u32) {
    if buf.len() < 4 {
        return (ScanResult::BadInput, 0);
    }
    let mut found: u32 = 0;
    let mut first_off: u32 = 0;
    let size = buf.len();
    let mut off = 0u32;
    while (off as usize) + 4 <= size {
        if restrict_to_exec
            && !is_pe_file_offset_in_executable_section(buf, off, 4)
        {
            off += 4;
            continue;
        }
        let insn = read_instr_u32(buf, off);
        if let Some(decoded) = decode_branch(insn, off) {
            let is_target_kind = matches!(
                decoded.kind,
                Arm64InsnKind::CbzW
                    | Arm64InsnKind::CbnzW
                    | Arm64InsnKind::CbzX
                    | Arm64InsnKind::CbnzX
                    | Arm64InsnKind::Bcond
            );
            if is_target_kind && decoded.target_off == target_off {
                if found == 0 {
                    first_off = off;
                }
                found += 1;
            }
        }
        off += 4;
    }

    match found {
        0 => (ScanResult::NotFound, 0),
        1 => (ScanResult::Found, first_off),
        _ => (ScanResult::Ambiguous, 0),
    }
}
