/* tools/gbl-pack/gbl-pack.c — CLI for the packer. */
#define _POSIX_C_SOURCE 200809L  /* gmtime_r */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pack.h"

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
    const char *cached = NULL, *source = NULL, *extracted = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cached-abl") && i + 1 < argc)  cached    = argv[++i];
        else if (!strcmp(argv[i], "--source")    && i + 1 < argc)  source    = argv[++i];
        else if (!strcmp(argv[i], "--extracted") && i + 1 < argc)  extracted = argv[++i];
        else if (!strcmp(argv[i], "--out")       && i + 1 < argc)  out       = argv[++i];
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (!cached || !source || !extracted || !out) {
        fprintf(stderr,
            "usage: gbl-pack --cached-abl PE --source RAW --extracted PE --out OUT\n");
        return 2;
    }

    struct gbl_pack_inputs in = {0};
    if (slurp(cached,    (uint8_t **)&in.cached_abl, &in.cached_abl_size)) return 1;
    if (slurp(source,    (uint8_t **)&in.source,      &in.source_size))     return 1;
    if (slurp(extracted, (uint8_t **)&in.extracted,   &in.extracted_size))  return 1;

    in.packer_version = "gbl-pack 1.0.0";
    char ts[32];
    time_t now = time(NULL);
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
