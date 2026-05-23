//! Retired patches — host-only reference implementations.
//!
//! Port of `GblChainloadPkg/Library/DynamicPatchLib/retired/`. The
//! retired patches are NOT linked into the active patch table; they're
//! kept as documentation + reference implementations only. The
//! `#[allow(dead_code)]` attribute mirrors the C
//! `__attribute__((unused))` on the source side.

pub mod block_efisp_recursion;
