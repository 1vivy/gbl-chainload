//! Pattern scanner — port of
//! `GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.c`.
//!
//! Always scans the entire buffer to detect ambiguity (so patches refuse
//! to anchor when their byte pattern matches multiple sites). Stateless;
//! the caller owns the buffer.

use super::pe_sections::is_pe_file_offset_in_executable_section;

/// Outcome of a pattern scan. Discriminants match the C `SCAN_RESULT`
/// enum 1:1 — see `Include/Library/ScanLib.h`.
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum ScanResult {
    /// Exactly one match in the scanned domain.
    Found = 0,
    NotFound = 1,
    /// Multiple matches — the patch refuses to anchor on this pattern.
    Ambiguous = 2,
    /// NULL pointer or zero-length input.
    BadInput = 3,
}

/// Mask-aware byte comparator. `mask[i] = 0xFF` matches exactly, `0x00`
/// is a wildcard; bytes in between are partial matches (used by some
/// instruction-template scans).
#[inline]
fn match_at(buf: &[u8], pattern: &[u8], mask: Option<&[u8]>) -> bool {
    debug_assert!(buf.len() >= pattern.len());
    for i in 0..pattern.len() {
        let b = buf[i] ^ pattern[i];
        let m = mask.map(|m| m[i]).unwrap_or(0xFF);
        if (b & m) != 0 {
            return false;
        }
    }
    true
}

/// Scan the whole buffer for exactly one occurrence of `pattern`.
///
/// Returns `(ScanResult::Found, off)` on a unique match. Always walks
/// the entire buffer (no first-match exit) so ambiguity is detectable.
///
/// Byte-for-byte parity with `ScanFor` in `Internal/ScanLib.c`.
pub fn scan_for(buf: &[u8], pattern: &[u8], mask: Option<&[u8]>) -> (ScanResult, u32) {
    if buf.is_empty() || pattern.is_empty() || buf.len() < pattern.len() {
        return (ScanResult::BadInput, 0);
    }

    let mut found: u32 = 0;
    let mut first_off: u32 = 0;
    let plen = pattern.len();

    let size = buf.len();
    let mut i = 0usize;
    while i + plen <= size {
        if match_at(&buf[i..i + plen], pattern, mask) {
            if found == 0 {
                first_off = i as u32;
            }
            found += 1;
        }
        i += 1;
    }

    match found {
        0 => (ScanResult::NotFound, 0),
        1 => (ScanResult::Found, first_off),
        _ => (ScanResult::Ambiguous, 0),
    }
}

/// Same as [`scan_for`], but restricted to file-offsets that lie inside
/// an executable PE section when `exec_only` is true.
///
/// Byte-for-byte parity with `ScanForBoundedSection` in
/// `Internal/ScanLib.c`.
pub fn scan_for_bounded_section(
    buf: &[u8],
    exec_only: bool,
    pattern: &[u8],
    mask: Option<&[u8]>,
) -> (ScanResult, u32) {
    if buf.is_empty() || pattern.is_empty() || buf.len() < pattern.len() {
        return (ScanResult::BadInput, 0);
    }

    let mut found: u32 = 0;
    let mut first_off: u32 = 0;
    let plen = pattern.len();

    let size = buf.len();
    let mut i = 0usize;
    while i + plen <= size {
        if exec_only
            && !is_pe_file_offset_in_executable_section(buf, i as u32, plen as u32)
        {
            i += 1;
            continue;
        }
        if match_at(&buf[i..i + plen], pattern, mask) {
            if found == 0 {
                first_off = i as u32;
            }
            found += 1;
        }
        i += 1;
    }

    match found {
        0 => (ScanResult::NotFound, 0),
        1 => (ScanResult::Found, first_off),
        _ => (ScanResult::Ambiguous, 0),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mirror of tests/scan/test_scanfor.c.

    #[test]
    fn scan_unique_match() {
        let mut buf = [0xAAu8; 64];
        buf[20..24].copy_from_slice(&[0xDE, 0xAD, 0xBE, 0xEF]);
        let (r, off) = scan_for(&buf, &[0xDE, 0xAD, 0xBE, 0xEF], None);
        assert_eq!(r, ScanResult::Found);
        assert_eq!(off, 20);
    }

    #[test]
    fn scan_not_found() {
        let buf = [0xAAu8; 64];
        let (r, _) = scan_for(&buf, &[0xDE, 0xAD, 0xBE, 0xEF], None);
        assert_eq!(r, ScanResult::NotFound);
    }

    #[test]
    fn scan_ambiguous() {
        let mut buf = [0xAAu8; 64];
        buf[10..12].copy_from_slice(&[0xDE, 0xAD]);
        buf[40..42].copy_from_slice(&[0xDE, 0xAD]);
        let (r, _) = scan_for(&buf, &[0xDE, 0xAD], None);
        assert_eq!(r, ScanResult::Ambiguous);
    }

    #[test]
    fn scan_with_mask() {
        let mut buf = [0xAAu8; 64];
        buf[30..34].copy_from_slice(&[0xDE, 0xAD, 0x12, 0xEF]);
        let pat = [0xDE, 0xAD, 0x00, 0xEF];
        let mask = [0xFF, 0xFF, 0x00, 0xFF];
        let (r, off) = scan_for(&buf, &pat, Some(&mask));
        assert_eq!(r, ScanResult::Found);
        assert_eq!(off, 30);
    }

    #[test]
    fn scan_bad_input() {
        let (r, _) = scan_for(&[], &[0x01], None);
        assert_eq!(r, ScanResult::BadInput);
    }
}
