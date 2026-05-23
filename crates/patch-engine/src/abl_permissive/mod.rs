//! ABL-permissive patches — always compiled (firmware + host).
//!
//! Port of `GblChainloadPkg/Library/DynamicPatchLib/abl_permissive/`.
//! Two patches:
//!
//! - [`libavb_force_success`] — patch10. Forces libavb's
//!   `avb_slot_verify` to always return OK with populated SlotData,
//!   bypassing every `!allow_verification_error` gate.
//! - [`fastboot_lock_gates`] — patch6. Rewrites the four lock-state
//!   refusal gates in ABL's fastboot command dispatcher.

pub mod libavb_force_success;
pub mod fastboot_lock_gates;

use crate::{PatchDesc, PatchScope};

/// All ABL-permissive patches. Always linked into both firmware and
/// host staticlibs.
pub const ABL_PERMISSIVE_PATCHES: &[PatchDesc] = &[
    PatchDesc {
        name: "patch10-libavb-force-avb-success",
        scope: PatchScope::AblPermissive,
        mandatory: true,
        apply: libavb_force_success::apply,
    },
    PatchDesc {
        name: "patch6-lock-state-fastboot-gate",
        scope: PatchScope::AblPermissive,
        mandatory: true,
        apply: fastboot_lock_gates::apply,
    },
];
