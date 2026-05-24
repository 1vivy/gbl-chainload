/** @file AvbParseLib.h — Re-export of crates/avb-parse's public C ABI.
 *
 * PR2 Task 7: AVB structure parsing moved into crates/avb-parse
 * (Rust). The struct layouts, enum discriminants, magic-byte sizes,
 * and EFI_STATUS-returning entry points are now defined by
 * `crates/avb-parse/include/avb_parse_ffi.h`. This header exists for
 * source-compat — every in-tree caller's `#include
 * <Library/AvbParseLib.h>` keeps working unchanged.
 *
 * Symbols are exported by `libavb_parse.a`:
 *   - Firmware build: linked via DLINK2_FLAGS in GblChainloadPkg.dsc.
 *   - Host C tools (vbmeta-graft, mode2-profile, tests/avb): linked
 *     via per-tool Makefile (per-cross-target staticlib paths).
 **/
#ifndef AVB_PARSE_LIB_H_
#define AVB_PARSE_LIB_H_

#include "../../../crates/avb-parse/include/avb_parse_ffi.h"

#endif
