/* GblChainloadPkg/Include/Library/DynamicPatchLib.h
 *
 * PR2 Task 6: the patch engine moved to crates/patch-engine. This
 * header is now a thin re-export of the Rust crate's FFI header so
 * existing #include <Library/DynamicPatchLib.h> call sites keep
 * working without source changes.
 *
 * The wire-ABI commitments (PATCH_OUTCOME / PATCH_WORST / PATCH_RESULT
 * / GBL_OEM discriminants) live in patch_engine_ffi.h — see the Rust
 * shim at crates/patch-engine/src/ffi.rs for the parity assertions.
 */
#ifndef DYNAMIC_PATCH_LIB_H_
#define DYNAMIC_PATCH_LIB_H_

#include "../../../crates/patch-engine/include/patch_engine_ffi.h"

#endif /* DYNAMIC_PATCH_LIB_H_ */
