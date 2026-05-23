/* crates/mode2-profile-core/include/mode2_profile_ffi.h — C ABI for
 * libmode2_profile_core.a.
 *
 * Replaces the deleted Internal/Mode2Profile.h:
 *   - enum gbl_m2p_status (parse-side wire ABI)
 *   - gbl_mode2_profile_parse() signature
 *
 * Adds two host-only entry points for the mode2-profile multicall:
 *   - gbl_mode2_profile_compile()  (TOML -> 120 bytes)
 *   - gbl_mode2_profile_derive()   (stock vbmeta -> wire profile)
 *
 * The 120-byte payload layout (`struct gbl_mode2_profile`) still lives
 * in tools/shared/gbl_mode2_profile.h until PR2 Task 8 collapses the
 * host tools into the `gbl` multicall binary. Include both headers
 * where you need both surfaces.
 *
 * Backed by crates/mode2-profile-core (Rust). Symbols are exported by
 * the libmode2_profile_core.a staticlib that cargo builds; each host
 * or firmware consumer links the matching target's staticlib.
 */
#ifndef MODE2_PROFILE_FFI_H_
#define MODE2_PROFILE_FFI_H_

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
   typedef __SIZE_TYPE__ size_t;
# endif
#endif

/* The wire `struct gbl_mode2_profile` definition lives in the shared
 * header; pull it in so callers only need to include this one. */
#include "../../../tools/shared/gbl_mode2_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Parse-side status enum -------------------------------------------
 *
 * Numeric values match the legacy `enum gbl_m2p_status` from
 * Internal/Mode2Profile.h one-for-one. The Rust shim (`Mode2Status`
 * in crates/mode2-profile-core/src/ffi.rs) asserts these discriminants
 * in a unit test. Re-numbering any variant is a wire-ABI break.
 */
enum gbl_m2p_status {
    GBL_M2P_OK             = 0,
    GBL_M2P_TOO_SMALL      = 1,
    GBL_M2P_BAD_MAGIC      = 2,
    GBL_M2P_BAD_VERSION    = 3,
    GBL_M2P_BAD_RESERVED   = 4,
    /* is_unlocked > 1 or color > 3, or NULL out pointer. */
    GBL_M2P_BAD_FIELD      = 5
};

/* Parse + validate a 120-byte mode2 profile. On GBL_M2P_OK, *out is
 * filled with host-endian field values. */
enum gbl_m2p_status
gbl_mode2_profile_parse(const uint8_t *bytes, size_t size,
                        struct gbl_mode2_profile *out);

/* ---- Host-only: compile + derive ---------------------------------------
 *
 * Both functions return 0 on success and a positive status on failure.
 * The compile-side errors are >= 100; the derive-side errors are >= 200.
 * Callers that want a typed error switch on these numeric ranges; the
 * Rust enums are not exposed across the FFI.
 */

/* Compile-side error codes (in addition to GBL_M2P_OK = 0). */
#define GBL_M2P_COMPILE_MALFORMED_TOML   100
#define GBL_M2P_COMPILE_MISSING_OR_TYPE  101
#define GBL_M2P_COMPILE_OUT_OF_RANGE     102
#define GBL_M2P_COMPILE_BAD_DIGEST       103
#define GBL_M2P_COMPILE_UNKNOWN_KEY      104

/* Compile a NUL-terminated profile TOML string to its 120-byte wire
 * binary. `out_bin` must point to at least 120 writable bytes; on OK
 * the byte count (always 120) is written to *out_size (which may be
 * NULL if the caller doesn't need it). */
int gbl_mode2_profile_compile(const char *toml_str,
                              uint8_t *out_bin, size_t *out_size);

/* Derive-side error codes. */
#define GBL_M2P_DERIVE_TOO_SMALL          200
#define GBL_M2P_DERIVE_BAD_MAGIC          201
#define GBL_M2P_DERIVE_MALFORMED_HEADER   202
#define GBL_M2P_DERIVE_NO_PUBLIC_KEY      203
#define GBL_M2P_DERIVE_PK_PAST_AUX        204
#define GBL_M2P_DERIVE_DESC_PAST_AUX      205
#define GBL_M2P_DERIVE_NO_OS_VERSION      206
#define GBL_M2P_DERIVE_NO_SPL             207
#define GBL_M2P_DERIVE_OS_OUT_OF_RANGE    208
#define GBL_M2P_DERIVE_SPL_OUT_OF_RANGE   209
#define GBL_M2P_DERIVE_SPL_MALFORMED      210

/* Derive a wire profile from a stock vbmeta image. `is_unlocked` is set
 * to 0 and `color` to 0 (GREEN); callers wanting a different boot-state
 * profile edit the resulting TOML before recompiling. */
int gbl_mode2_profile_derive(const uint8_t *vbmeta, size_t vbmeta_size,
                             struct gbl_mode2_profile *out);

#ifdef __cplusplus
}
#endif

#endif /* MODE2_PROFILE_FFI_H_ */
