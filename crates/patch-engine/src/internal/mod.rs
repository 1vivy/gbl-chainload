//! Internal infrastructure ported from
//! `GblChainloadPkg/Library/DynamicPatchLib/Internal/`.
//!
//! - [`scan`] — `ScanFor` / `ScanForBoundedSection` pattern scanners.
//! - [`pe_sections`] — `IsPeFileOffsetInExecutableSection` PE walker.
//! - [`encode`] — AArch64 instruction encoders (CBZ / B / read+write u32).
//! - [`arm64_decode`] — AArch64 branch + ADRP+ADD decoders.

pub mod scan;
pub mod pe_sections;
pub mod encode;
pub mod arm64_decode;
