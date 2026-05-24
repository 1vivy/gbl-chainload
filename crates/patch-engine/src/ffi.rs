//! C-ABI shim — preserves the wire ABI of the deleted DynamicPatchLib
//! C headers (`DynamicPatchLib.h`, `PatchScope.h`, `PatchDesc.h`) so the
//! firmware (`BootFlow.c`) and host tools (`abl-patcher`) keep linking
//! `libpatch_engine.a` unchanged.
//!
//! Public C header: `crates/patch-engine/include/patch_engine_ffi.h`.
//!
//! The shim holds a single engine instance in static storage and walks
//! it from [`DynamicPatch_Apply`]. The split into `ensure_init` /
//! `apply` matches the C two-call protocol BootFlow.c uses.

use core::ptr;

use crate::{Engine, PatchOutcome, PatchResult, Worst};

#[cfg(feature = "host")]
use crate::Oem;

/// Mirror of `enum PATCH_OUTCOME` (C `PATCH_DESC::Apply` return type).
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum CPatchOutcome {
    Ok = 0,
    Miss = 1,
    Ambiguous = 2,
}

impl From<PatchOutcome> for CPatchOutcome {
    fn from(o: PatchOutcome) -> Self {
        match o {
            PatchOutcome::Ok => CPatchOutcome::Ok,
            PatchOutcome::Miss => CPatchOutcome::Miss,
            PatchOutcome::Ambiguous => CPatchOutcome::Ambiguous,
        }
    }
}

/// Mirror of `enum PATCH_WORST`.
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum CPatchWorst {
    Ok = 0,
    OptionalMiss = 1,
    MandatoryMiss = 2,
}

impl From<Worst> for CPatchWorst {
    fn from(w: Worst) -> Self {
        match w {
            Worst::Ok => CPatchWorst::Ok,
            Worst::OptionalMiss => CPatchWorst::OptionalMiss,
            Worst::MandatoryMiss => CPatchWorst::MandatoryMiss,
        }
    }
}

/// Mirror of `enum GBL_OEM`.
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum CGblOem {
    None = 0,
    Oplus = 1,
}

/// Mirror of `struct PATCH_RESULT` byte-for-byte.
///
/// Field order + sizes: `applied_count: u32`, `missed_count: u32`,
/// `worst_outcome: enum (u32 on AArch64/x86_64 + UEFI / SysV)`.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CPatchResult {
    pub applied_count: u32,
    pub missed_count: u32,
    pub worst_outcome: CPatchWorst,
}

/// Static slot for the active engine. Set by
/// `DynamicPatchLib_EnsureInit` / `DynamicPatchLib_EnsureInitScoped`,
/// read by `DynamicPatch_Apply`. Single-threaded by construction (same
/// as the C `gAggregateInit` global).
static mut ACTIVE: Option<Engine> = None;

/// Replace `DynamicPatchLib_EnsureInit()` from the deleted
/// `PatchTable.c`. Firmware-only entry point — selects abl_permissive.
#[no_mangle]
pub extern "C" fn DynamicPatchLib_EnsureInit() {
    unsafe {
        #[allow(static_mut_refs)]
        {
            ACTIVE = Some(Engine::ensure_init());
        }
    }
}

/// Replace `DynamicPatchLib_EnsureInitScoped()` from the deleted
/// `PatchTable.c`. Host tool entry point — selects OEM group + ABL
/// permissive inclusion at runtime.
///
/// `include_abl_permissive` is a C `int` (0 / non-zero).
#[cfg(feature = "host")]
#[no_mangle]
pub extern "C" fn DynamicPatchLib_EnsureInitScoped(
    oem: CGblOem,
    include_abl_permissive: core::ffi::c_int,
) {
    let oem_rs = match oem {
        CGblOem::None => Oem::None,
        CGblOem::Oplus => Oem::Oplus,
    };
    let include = include_abl_permissive != 0;
    unsafe {
        #[allow(static_mut_refs)]
        {
            ACTIVE = Some(Engine::ensure_init_scoped(oem_rs, include));
        }
    }
}

/// Replace `DynamicPatch_Apply()` from the deleted
/// `Internal/PatchEngine.c`. Walks the active table, in-place edits
/// `buf`, writes the aggregate result to `*result`.
///
/// # Safety
/// - `buf` must point to `size` writable bytes (or NULL with `size == 0`).
/// - `result` must be non-NULL and point to a writable `CPatchResult`.
/// - `DynamicPatchLib_EnsureInit*` must have been called first (the C
///   protocol). If not, the function returns vacuously OK — matching
///   the C `gPatchTable == NULL` branch.
#[no_mangle]
pub unsafe extern "C" fn DynamicPatch_Apply(
    buf: *mut u8,
    size: u32,
    result: *mut CPatchResult,
) {
    if result.is_null() {
        return;
    }
    // Default-zero the result — matches the C path even on the early-out.
    ptr::write(
        result,
        CPatchResult {
            applied_count: 0,
            missed_count: 0,
            worst_outcome: CPatchWorst::Ok,
        },
    );

    #[allow(static_mut_refs)]
    let engine = match ACTIVE.as_ref() {
        Some(e) => e,
        None => return,
    };
    if buf.is_null() || size == 0 {
        return;
    }
    let slice = core::slice::from_raw_parts_mut(buf, size as usize);
    let r: PatchResult = engine.apply(slice);
    ptr::write(
        result,
        CPatchResult {
            applied_count: r.applied_count,
            missed_count: r.missed_count,
            worst_outcome: r.worst.into(),
        },
    );
}

// --- Compile-time wire-ABI assertions --------------------------------

const _: () = {
    // PATCH_RESULT layout: two u32 + one enum. Total must be 12 bytes
    // packed (no padding) on every supported target.
    assert!(core::mem::size_of::<CPatchResult>() == 12);
    assert!(core::mem::align_of::<CPatchResult>() == 4);
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enum_discriminants_match_c_header() {
        // CPatchOutcome
        assert_eq!(CPatchOutcome::Ok as u32, 0);
        assert_eq!(CPatchOutcome::Miss as u32, 1);
        assert_eq!(CPatchOutcome::Ambiguous as u32, 2);
        // CPatchWorst
        assert_eq!(CPatchWorst::Ok as u32, 0);
        assert_eq!(CPatchWorst::OptionalMiss as u32, 1);
        assert_eq!(CPatchWorst::MandatoryMiss as u32, 2);
        // CGblOem
        assert_eq!(CGblOem::None as u32, 0);
        assert_eq!(CGblOem::Oplus as u32, 1);
    }

    #[cfg(feature = "host")]
    #[test]
    fn apply_via_ffi_round_trip() {
        // Build a CPatchResult, call EnsureInit + Apply on a tiny
        // buffer that won't match any anchors, and verify the FFI shim
        // wrote the expected aggregate (2 patches, both mandatory MISS).
        let mut buf = vec![0u8; 4096];
        let mut r = CPatchResult {
            applied_count: 0,
            missed_count: 0,
            worst_outcome: CPatchWorst::Ok,
        };
        DynamicPatchLib_EnsureInitScoped(CGblOem::None, 1);
        unsafe {
            DynamicPatch_Apply(buf.as_mut_ptr(), buf.len() as u32, &mut r);
        }
        // abl_permissive = patch10 + patch6, both mandatory.
        assert_eq!(r.applied_count, 0);
        assert_eq!(r.missed_count, 2);
        assert_eq!(r.worst_outcome, CPatchWorst::MandatoryMiss);
    }
}
