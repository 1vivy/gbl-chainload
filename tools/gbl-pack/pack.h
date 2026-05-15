/* tools/gbl-pack/pack.h */
#ifndef GBL_PACK_H_
#define GBL_PACK_H_

#include <stdint.h>
#include <stddef.h>

struct gbl_pack_inputs {
    const uint8_t *cached_abl;  size_t cached_abl_size;
    const uint8_t *source;      size_t source_size;
    const uint8_t *extracted;   size_t extracted_size;
    const char    *packer_version;   /* ASCII */
    const char    *timestamp_iso8601;/* ASCII */
};

enum gbl_pack_status {
    GBL_PACK_OK = 0,
    GBL_PACK_ERR_EFISP_PRESENT,
    GBL_PACK_ERR_PE_INSANE,
    GBL_PACK_ERR_TOO_LARGE,
    GBL_PACK_ERR_OOM,
    GBL_PACK_ERR_BAD_INPUT
};

/* Allocates *out_buf with malloc; caller frees. Returns GBL_PACK_OK on
   success and writes the GBLP1 container bytes. */
enum gbl_pack_status
gbl_pack_build(const struct gbl_pack_inputs *in,
               uint8_t **out_buf, size_t *out_size);

#endif
