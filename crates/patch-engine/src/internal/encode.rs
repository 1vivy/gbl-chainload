//! AArch64 instruction encoding helpers — port of
//! `GblChainloadPkg/Library/DynamicPatchLib/Internal/Encode.c`.
//!
//! Just enough to rewrite the conditional branches patches 6/7 emit:
//! `CBZ Wn, target`, `B target`, and a raw 32-bit instruction read+write.

/// Write a 32-bit instruction word at `file_off` in `buf` (little-endian).
#[inline]
pub fn write_instr_u32(buf: &mut [u8], file_off: u32, insn: u32) {
    let off = file_off as usize;
    buf[off] = (insn & 0xFF) as u8;
    buf[off + 1] = ((insn >> 8) & 0xFF) as u8;
    buf[off + 2] = ((insn >> 16) & 0xFF) as u8;
    buf[off + 3] = ((insn >> 24) & 0xFF) as u8;
}

/// Read a 32-bit instruction word at `file_off` in `buf` (little-endian).
#[inline]
pub fn read_instr_u32(buf: &[u8], file_off: u32) -> u32 {
    let off = file_off as usize;
    (buf[off] as u32)
        | ((buf[off + 1] as u32) << 8)
        | ((buf[off + 2] as u32) << 16)
        | ((buf[off + 3] as u32) << 24)
}

/// Encode a `CBZ Wn, target` instruction word.
///
/// Returns `Some(insn)` on success, `None` if the displacement is
/// out-of-range (19-bit signed instruction delta), unaligned, or
/// `reg > 31`. Byte-for-byte parity with `EncodeCbz` in
/// `Internal/Encode.c`.
pub fn encode_cbz(insn_off: u32, target_off: u32, reg: u32) -> Option<u32> {
    if reg > 31 {
        return None;
    }
    // Signed byte delta. Wrapping_sub matches `(INT32)(TargetOff - InsnOff)`
    // on the C side (u32 difference reinterpreted as i32).
    let delta_bytes = target_off.wrapping_sub(insn_off) as i32;
    if (delta_bytes & 3) != 0 {
        return None;
    }
    let delta_instr = delta_bytes >> 2;
    if delta_instr < -(1 << 18) || delta_instr >= (1 << 18) {
        return None;
    }
    // CBZ Wn: 0x34000000 | (imm19 << 5) | Rt
    let insn = 0x3400_0000u32
        | (((delta_instr as u32) & 0x7_FFFF) << 5)
        | (reg & 0x1F);
    Some(insn)
}

/// Encode + write CBZ at `insn_off` targeting `target_off`. Returns
/// `true` on successful rewrite, `false` on out-of-range / unaligned /
/// bad register.
pub fn rewrite_cbz(buf: &mut [u8], insn_off: u32, reg: u32, target_off: u32) -> bool {
    match encode_cbz(insn_off, target_off, reg) {
        Some(insn) => {
            write_instr_u32(buf, insn_off, insn);
            true
        }
        None => false,
    }
}

/// Encode an unconditional `B target` word.
///
/// Returns `Some(insn)` on success, `None` if the displacement is
/// out-of-range (26-bit signed instruction delta) or unaligned.
/// Byte-for-byte parity with `EncodeBUncond` in `Internal/Encode.c`.
pub fn encode_b_uncond(insn_off: u32, target_off: u32) -> Option<u32> {
    let delta_bytes = target_off.wrapping_sub(insn_off) as i32;
    if (delta_bytes & 3) != 0 {
        return None;
    }
    let delta_instr = delta_bytes >> 2;
    if delta_instr < -(1 << 25) || delta_instr >= (1 << 25) {
        return None;
    }
    let insn = 0x1400_0000u32 | ((delta_instr as u32) & 0x03FF_FFFF);
    Some(insn)
}

/// Encode + write an unconditional B at `insn_off` targeting `target_off`.
pub fn rewrite_b_uncond(buf: &mut [u8], insn_off: u32, target_off: u32) -> bool {
    match encode_b_uncond(insn_off, target_off) {
        Some(insn) => {
            write_instr_u32(buf, insn_off, insn);
            true
        }
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mirror of tests/scan/test_encode.c.

    #[test]
    fn write_read_roundtrip() {
        let mut buf = [0u8; 32];
        write_instr_u32(&mut buf, 12, 0xDEAD_BEEF);
        assert_eq!(buf[12], 0xEF);
        assert_eq!(buf[13], 0xBE);
        assert_eq!(buf[14], 0xAD);
        assert_eq!(buf[15], 0xDE);
        assert_eq!(read_instr_u32(&buf, 12), 0xDEAD_BEEF);
    }

    #[test]
    fn cbz_forward() {
        // cbz w24, +0x4 -> 0x34000038
        let insn = encode_cbz(0, 4, 24).unwrap();
        assert_eq!(insn, 0x3400_0038);
    }

    #[test]
    fn cbz_backward() {
        // cbz w0, -0x4 at 0x100 -> 0x34FFFFE0
        let insn = encode_cbz(0x100, 0xFC, 0).unwrap();
        assert_eq!(insn, 0x34FF_FFE0);
    }

    #[test]
    fn cbz_misaligned() {
        assert_eq!(encode_cbz(0, 5, 0), None);
    }

    #[test]
    fn cbz_out_of_range() {
        assert_eq!(encode_cbz(0, 0x10_0000, 0), None);
    }

    #[test]
    fn cbz_bad_reg() {
        assert_eq!(encode_cbz(0, 4, 32), None);
    }

    #[test]
    fn rewrite_cbz_buffer() {
        let mut buf = [0xCCu8; 256];
        assert!(rewrite_cbz(&mut buf, 16, 24, 20));
        assert_eq!(read_instr_u32(&buf, 16), 0x3400_0038);
        assert_eq!(buf[15], 0xCC);
        assert_eq!(buf[20], 0xCC);
    }
}
