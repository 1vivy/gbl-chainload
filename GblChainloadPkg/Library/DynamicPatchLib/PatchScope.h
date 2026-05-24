/* PatchScope.h — runtime patch-scope selection for host callers.
   The on-device EDK2 build uses DynamicPatchLib_EnsureInit() (fixed
   universal + abl_permissive aggregation); host tools (abl-patcher) pick
   OEM and abl_permissive inclusion at invocation time. */
#ifndef GBL_PATCH_SCOPE_H_
#define GBL_PATCH_SCOPE_H_

/* OEM group selector. NONE = universal only. */
typedef enum { GBL_OEM_NONE = 0, GBL_OEM_OPLUS = 1 } GBL_OEM;

/* Aggregate the runtime patch table: universal, then (if oem != NONE) the
   OEM group, then (if include_abl_permissive) the ABL-permissive groups.
   Replaces the compile-time aggregation for host callers. */
void DynamicPatchLib_EnsureInitScoped (GBL_OEM oem, int include_abl_permissive);

#endif
