/* crates/patch-engine/include/patch_engine_ffi.h — C ABI for
 * libpatch_engine.a.
 *
 * Replaces the deleted DynamicPatchLib C headers:
 *   - GblChainloadPkg/Include/Library/DynamicPatchLib.h
 *   - GblChainloadPkg/Include/Library/PatchDesc.h
 *   - GblChainloadPkg/Library/DynamicPatchLib/PatchScope.h
 *
 * Backed by crates/patch-engine (Rust). Symbols are exported by the
 * libpatch_engine.a staticlib that cargo builds; each host or firmware
 * consumer links the matching target's staticlib.
 *
 * Wire-ABI commitment: every enum below pins its discriminants to the
 * exact values the deleted C headers carried. The Rust shim
 * (crates/patch-engine/src/ffi.rs) asserts these in a unit test.
 */
#ifndef PATCH_ENGINE_FFI_H_
#define PATCH_ENGINE_FFI_H_

#ifdef GBL_HOST_BUILD
# include <stdint.h>
# include <stddef.h>
#else
# include <Uefi.h>
# ifndef GBL_COMPAT_TYPES_DEFINED
#  define GBL_COMPAT_TYPES_DEFINED
   typedef UINT8  uint8_t;
   typedef UINT16 uint16_t;
   typedef UINT32 uint32_t;
   typedef INT32  int32_t;
# endif
# ifndef _SIZE_T
#  define _SIZE_T
   typedef UINTN size_t;
# endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- enum PATCH_OUTCOME -----------------------------------------------
 * Per-patch outcome. Matches the deleted PatchDesc.h enum 1:1. */
enum {
    PATCH_OK         = 0,
    PATCH_MISS       = 1,
    PATCH_AMBIGUOUS  = 2,
};
typedef int PATCH_OUTCOME;

/* ---- enum PATCH_WORST -------------------------------------------------
 * Aggregate worst-case outcome — what BootFlow.c keys off to abort. */
enum {
    PATCH_RESULT_OK              = 0,
    PATCH_RESULT_OPTIONAL_MISS   = 1,
    PATCH_RESULT_MANDATORY_MISS  = 2,
};
typedef int PATCH_WORST;

/* ---- struct PATCH_RESULT ----------------------------------------------
 * Aggregate result returned by DynamicPatch_Apply. Layout is fixed at
 * { u32, u32, enum } = 12 bytes packed; the Rust shim asserts this. */
typedef struct {
    uint32_t     AppliedCount;
    uint32_t     MissedCount;
    PATCH_WORST  WorstOutcome;
} PATCH_RESULT;

/* ---- enum GBL_OEM -----------------------------------------------------
 * OEM group selector. NONE = abl_permissive only (no OEM patches). */
enum {
    GBL_OEM_NONE  = 0,
    GBL_OEM_OPLUS = 1,
};
typedef int GBL_OEM;

/* ---- Engine entry points ---------------------------------------------- */

/* Firmware path: populate the active patch table with abl_permissive
 * (patch6 + patch10). Call once before DynamicPatch_Apply.
 *
 * Replaces DynamicPatchLib_EnsureInit() from the deleted PatchTable.c. */
void DynamicPatchLib_EnsureInit(void);

/* Host path: runtime scope selector. Aggregates the active table from:
 *   - (if oem == GBL_OEM_OPLUS) the OEM oplus group, then
 *   - (if include_abl_permissive != 0) the abl_permissive group.
 *
 * Replaces DynamicPatchLib_EnsureInitScoped() from the deleted
 * PatchTable.c. Symbol only exported on host builds. */
void DynamicPatchLib_EnsureInitScoped(GBL_OEM oem, int include_abl_permissive);

/* Walk the active table, edit `buf` in place, write the aggregate
 * outcome to *result. Result is zeroed even when there are no patches
 * to apply (matches the C semantics).
 *
 * Replaces DynamicPatch_Apply() from the deleted PatchEngine.c. */
void DynamicPatch_Apply(uint8_t *buf, uint32_t size, PATCH_RESULT *result);

#ifdef __cplusplus
}
#endif

#endif /* PATCH_ENGINE_FFI_H_ */
