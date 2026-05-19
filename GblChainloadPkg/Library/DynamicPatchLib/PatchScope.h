/* PatchScope.h — runtime patch-scope selection for host callers.
   The on-device EDK2 build uses DynamicPatchLib_EnsureInit() (GBL_MODE
   compile-time aggregation); host tools (abl-patcher) select at runtime. */
#ifndef GBL_PATCH_SCOPE_H_
#define GBL_PATCH_SCOPE_H_

/* OEM group selector. NONE = universal only. */
typedef enum { GBL_OEM_NONE = 0, GBL_OEM_ONEPLUS = 1 } GBL_OEM;

/* Aggregate the runtime patch table: universal, then (if oem != NONE) the
   OEM group, then (if include_mode1) the mode_1 group. Replaces the
   compile-time aggregation for host callers. */
void DynamicPatchLib_EnsureInitScoped (GBL_OEM oem, int include_mode1);

#endif
