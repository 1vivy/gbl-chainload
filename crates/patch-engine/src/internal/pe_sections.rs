//! PE/COFF section walker — port of
//! `GblChainloadPkg/Library/DynamicPatchLib/Internal/PeSections.c`.
//!
//! Used by [`super::scan::scan_for_bounded_section`] and the ADRP+ADD
//! scanners to refuse anchors that land in `.rodata`, `.data`, or any
//! other non-executable section.

/// Read a little-endian u16 from an unaligned slice.
#[inline]
fn read_u16_le(b: &[u8]) -> u32 {
    (b[0] as u32) | ((b[1] as u32) << 8)
}

/// Read a little-endian u32 from an unaligned slice.
#[inline]
fn read_u32_le(b: &[u8]) -> u32 {
    (b[0] as u32)
        | ((b[1] as u32) << 8)
        | ((b[2] as u32) << 16)
        | ((b[3] as u32) << 24)
}

/// Return `true` if `[off, off+len)` lies entirely within an executable
/// PE section in `buf`.
///
/// Byte-for-byte parity with `IsPeFileOffsetInExecutableSection` in
/// `Internal/PeSections.c`.
pub fn is_pe_file_offset_in_executable_section(buf: &[u8], off: u32, len: u32) -> bool {
    let size = buf.len();
    let off_u = off as usize;
    let len_u = len as usize;
    if size < 0x100 || len == 0 {
        return false;
    }
    // Equivalent of C's overflow check `off + len < off || off + len > size`.
    let end = match off_u.checked_add(len_u) {
        Some(v) => v,
        None => return false,
    };
    if end > size {
        return false;
    }
    if buf[0] != b'M' || buf[1] != b'Z' {
        return false;
    }

    let pe_off = read_u32_le(&buf[0x3C..0x40]) as usize;
    if pe_off > size || pe_off.saturating_add(0x18) > size {
        return false;
    }
    if &buf[pe_off..pe_off + 4] != b"PE\0\0" {
        return false;
    }

    let number_of_sections = read_u16_le(&buf[pe_off + 6..pe_off + 8]);
    let opt_hdr_size = read_u16_le(&buf[pe_off + 20..pe_off + 22]) as usize;
    let section_off = pe_off + 24 + opt_hdr_size;
    if number_of_sections == 0 || section_off > size {
        return false;
    }

    for i in 0..(number_of_sections as usize) {
        let sh = section_off + i * 40;
        if sh + 40 > size {
            return false;
        }
        let virtual_size = read_u32_le(&buf[sh + 8..sh + 12]) as usize;
        let size_of_raw_data = read_u32_le(&buf[sh + 16..sh + 20]) as usize;
        let pointer_to_raw_data = read_u32_le(&buf[sh + 20..sh + 24]) as usize;
        let characteristics = read_u32_le(&buf[sh + 36..sh + 40]);
        // IMAGE_SCN_MEM_EXECUTE
        if (characteristics & 0x2000_0000) == 0 {
            continue;
        }
        // Overflow check from C: PointerToRawData + SizeOfRawData < PointerToRawData
        let raw_end = match pointer_to_raw_data.checked_add(size_of_raw_data) {
            Some(v) => v,
            None => continue,
        };
        if off_u < pointer_to_raw_data || off_u + len_u > raw_end {
            continue;
        }
        let rel_off = off_u - pointer_to_raw_data;
        let mapped_limit = if virtual_size != 0 {
            virtual_size
        } else {
            size_of_raw_data
        };
        let rel_end = match rel_off.checked_add(len_u) {
            Some(v) => v,
            None => continue,
        };
        if rel_end > mapped_limit {
            continue;
        }
        return true;
    }
    false
}
