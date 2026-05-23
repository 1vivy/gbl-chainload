/* tools/gbl-pack/pack.h */
#ifndef GBL_PACK_H_
#define GBL_PACK_H_

#include <stdint.h>
#include <stddef.h>

struct gbl_pack_inputs {
    const uint8_t *cached_abl;  size_t cached_abl_size;
    const uint8_t *source;      size_t source_size;
    const uint8_t *extracted;   size_t extracted_size;
    const uint8_t *mode2_profile; size_t mode2_profile_size;  /* optional */
    int            have_manifest;     /* 0 = no entry emitted */
    uint16_t       manifest_cap_bits; /* validated against reserved mask */
    const char    *packer_version;   /* ASCII */
    const char    *timestamp_iso8601;/* ASCII */
};

enum gbl_pack_status {
    GBL_PACK_OK = 0,
    /* GBL_PACK_ERR_EFISP_PRESENT removed in Task 10 — BlockIoHook EFISP gate
       supersedes the patch-time rejection.  gbl-pack.c warns at the CLI
       layer instead.  Numeric values of the codes below shifted down by 1
       (status codes are not stable wire format; pack.c is the only emitter). */
    GBL_PACK_ERR_PE_INSANE,
    GBL_PACK_ERR_TOO_LARGE,
    GBL_PACK_ERR_OOM,
    GBL_PACK_ERR_BAD_INPUT,
    GBL_PACK_ERR_PROFILE_BAD,
    GBL_PACK_ERR_MANIFEST_BAD
};

/* Allocates *out_buf with malloc; caller frees. Returns GBL_PACK_OK on
   success and writes the GBLP1 container bytes. */
enum gbl_pack_status
gbl_pack_build(const struct gbl_pack_inputs *in,
               uint8_t **out_buf, size_t *out_size);

#endif
