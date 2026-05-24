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
 * Post-PR2-Task-8: this header is the SOLE C-side source of truth for
 * the 120-byte mode-2 profile wire layout (constants + struct). The
 * legacy `tools/shared/gbl_mode2_profile.h` is gone — the host C tool
 * that consumed it was folded into `gbl mode2`, and the firmware
 * (`ProtocolHookLib`) consumes the definitions from here too.
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
/* EDK2 toolchain leaves <stddef.h> off the include path, so size_t isn't
 * provided by default. avb_sysdeps.h / QcBcc.h in QcomModulePkg also do
 * `typedef UINTN size_t;` — match that exact typedef so the firmware
 * translation units that pull both this header and an avb header don't
 * see a redefinition. _SIZE_T is the libc convention; both UEFI and host
 * libcs respect it. */
# ifndef _SIZE_T
#  define _SIZE_T
   typedef UINTN size_t;
# endif
#endif

/* ---- Wire constants (formerly tools/shared/gbl_mode2_profile.h) -------
 *
 * The 120-byte mode-2 profile payload rides in a GBLP1 0x0010 entry.
 * All multi-byte scalars are little-endian; the struct below is
 * `packed` so existing C consumers can memcpy it 1:1 with the on-disk
 * bytes.
 */
#define GBL_M2P_MAGIC        "GM2P"
#define GBL_M2P_MAGIC_SIZE   4u
#define GBL_M2P_VERSION      0x0001u
#define GBL_M2P_SIZE         120u

/* color field values (KMBootState.Color domain) */
#define GBL_M2P_COLOR_GREEN  0u
#define GBL_M2P_COLOR_YELLOW 1u
#define GBL_M2P_COLOR_ORANGE 2u
#define GBL_M2P_COLOR_RED    3u

/* On-disk profile — packed, little-endian. */
struct gbl_mode2_profile {
    uint8_t  magic[4];          /* "GM2P"                         off 0  */
    uint16_t version;           /* 1                              off 4  */
    uint16_t reserved;          /* 0                              off 6  */
    uint32_t is_unlocked;       /* 0 (locked) — SET_BOOT_STATE    off 8  */
    uint32_t color;             /* 0 = GREEN — SET_BOOT_STATE     off 12 */
    uint32_t system_version;    /* bootloader-domain OS version   off 16 */
    uint32_t system_spl;        /* bootloader-domain SPL          off 20 */
    uint8_t  rot_digest[32];    /* SET_ROT RotDigest              off 24 */
    uint8_t  pubkey_digest[32]; /* SET_BOOT_STATE PublicKey       off 56 */
    uint8_t  vbh[32];           /* SET_VBH Vbh                    off 88 */
} __attribute__((packed));

#ifndef MODE2_PROFILE_FFI_NO_STATIC_ASSERTS
_Static_assert(sizeof(struct gbl_mode2_profile) == GBL_M2P_SIZE,
               "gbl_mode2_profile must be 120 bytes packed");
#endif

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
