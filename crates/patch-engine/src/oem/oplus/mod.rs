//! OnePlus / Oppo / Realme (oplus / canoe) family OEM patches.
//!
//! Port of `GblChainloadPkg/Library/DynamicPatchLib/oem/oplus/`. Currently
//! holds patch7 (orange-screen / unlock-warning / 5-second delay gate).
//! Selected at host build time by `abl-patcher --oem oplus`.

pub mod bypass_warning;

use crate::{PatchDesc, PatchScope};

/// All Oplus-family OEM patches.
pub const OEM_OPLUS_PATCHES: &[PatchDesc] = &[
    PatchDesc {
        name: "patch7-orange-screen",
        scope: PatchScope::OemOplus,
        mandatory: false,
        apply: bypass_warning::apply,
    },
];
