/* tools/gbl-pack/gbl-pack.c — CLI for the packer. */
#define _POSIX_C_SOURCE 200809L  /* gmtime_r */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "pack.h"
#include "../shared/gblp1.h"

/* PR2 Task 3: `gbl_contains_utf16_efisp` now lives in `crates/pe-utils`
   (Rust). Linked from target/<triple>/release/libpe_utils.a. */
extern bool gbl_contains_utf16_efisp(const void *buf, size_t len);

static int slurp(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fprintf(stderr, "%s: empty or unreadable\n", path); fclose(f); return 1; }
    uint8_t *b = malloc((size_t)n);
    if (!b) { fprintf(stderr, "%s: OOM\n", path); fclose(f); return 1; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "%s: read failed\n", path); fclose(f); free(b); return 1;
    }
    fclose(f);
    *out = b;
    *out_size = (size_t)n;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("gbl-pack %s\n", GBL_TOOL_VERSION);
        return 0;
    }
    const char *cached = NULL, *source = NULL, *extracted = NULL,
               *out = NULL, *profile = NULL, *manifest_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cached-abl") && i + 1 < argc)  cached    = argv[++i];
        else if (!strcmp(argv[i], "--source")    && i + 1 < argc)  source    = argv[++i];
        else if (!strcmp(argv[i], "--extracted") && i + 1 < argc)  extracted = argv[++i];
        else if (!strcmp(argv[i], "--mode2-profile") && i + 1 < argc) profile = argv[++i];
        else if (!strcmp(argv[i], "--manifest")  && i + 1 < argc)  manifest_arg = argv[++i];
        else if (!strcmp(argv[i], "--out")       && i + 1 < argc)  out       = argv[++i];
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (!out || (!cached && !profile && !manifest_arg)) {
        fprintf(stderr,
            "gbl-pack %s\n"
            "usage: gbl-pack --out OUT "
            "[--cached-abl PE --source RAW --extracted PE] "
            "[--mode2-profile BIN] "
            "[--manifest BITS]\n", GBL_TOOL_VERSION);
        return 2;
    }
    if (cached && (!source || !extracted)) {
        fprintf(stderr,
            "gbl-pack: --cached-abl requires --source and --extracted\n");
        return 2;
    }

    int have_manifest = 0;
    uint16_t manifest_cap_bits = 0;
    if (manifest_arg) {
        errno = 0;
        char *end = NULL;
        unsigned long v = strtoul(manifest_arg, &end, 0);  /* auto 0x/0/decimal */
        if (errno != 0 || !end || *end != '\0' || end == manifest_arg) {
            fprintf(stderr, "gbl-pack: bad --manifest bits (not a number)\n");
            return 2;
        }
        if (v > 0xFFFFu) {
            fprintf(stderr,
                "gbl-pack: bad --manifest bits (must fit in 16 bits)\n");
            return 2;
        }
        if ((uint16_t)v & GBLP1_MANIFEST_BITS_RESERVED_MASK) {
            fprintf(stderr,
                "gbl-pack: bad --manifest bits (reserved bits set)\n");
            return 2;
        }
        manifest_cap_bits = (uint16_t)v;
        have_manifest = 1;
    }

    struct gbl_pack_inputs in = {0};
    if (cached) {
        if (slurp(cached,    (uint8_t **)&in.cached_abl, &in.cached_abl_size)) return 1;
        if (slurp(source,    (uint8_t **)&in.source,      &in.source_size))     return 1;
        if (slurp(extracted, (uint8_t **)&in.extracted,   &in.extracted_size))  return 1;
        /* Task 10: efisp UTF-16 rejection retired from the packer; warn only.
           The BlockIoHook EFISP gate is the runtime guarantee, so the packer
           accepts cached_abl containing the literal pattern.  Still useful
           as a signal that patch10/patch6 may have missed. */
        if (gbl_contains_utf16_efisp(in.cached_abl, in.cached_abl_size)) {
            fprintf(stderr,
                "gbl-pack: warning: cached_abl still contains UTF-16 \"efisp\" "
                "— BlockIoHook gate will handle this, but check that "
                "patch10/patch6 applied as expected\n");
        }
    }
    if (profile) {
        if (slurp(profile, (uint8_t **)&in.mode2_profile, &in.mode2_profile_size))
            return 1;
    }

    in.have_manifest     = have_manifest;
    in.manifest_cap_bits = manifest_cap_bits;

    in.packer_version = "gbl-pack " GBL_TOOL_VERSION;
    char ts[32];
    /* SOURCE_DATE_EPOCH support — reproducible-builds.org convention.
       Lets goldens (tests/host/goldens/) capture byte-exact output. */
    time_t now;
    const char *sde = getenv("SOURCE_DATE_EPOCH");
    if (sde && *sde) {
        errno = 0;
        char *end = NULL;
        unsigned long long v = strtoull(sde, &end, 10);
        if (errno != 0 || !end || *end != '\0' || end == sde) {
            fprintf(stderr,
                "gbl-pack: bad SOURCE_DATE_EPOCH (not a non-negative integer)\n");
            return 2;
        }
        now = (time_t)v;
    } else {
        now = time(NULL);
    }
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    in.timestamp_iso8601 = ts;

    uint8_t *buf = NULL;
    size_t size = 0;
    enum gbl_pack_status s = gbl_pack_build(&in, &buf, &size);
    if (s != GBL_PACK_OK) {
        fprintf(stderr, "gbl-pack: status=%d\n", (int)s);
        return 1;
    }
    FILE *o = fopen(out, "wb");
    if (!o) { perror(out); free(buf); return 1; }
    if (fwrite(buf, 1, size, o) != size) {
        fprintf(stderr, "%s: write failed\n", out); fclose(o); free(buf); return 1;
    }
    fclose(o);
    free(buf);
    fprintf(stderr, "gbl-pack: wrote %s (%zu bytes)\n", out, size);
    return 0;
}
