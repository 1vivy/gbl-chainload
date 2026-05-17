/* tests/host/helpers/parser_harness.c
   Single host harness that exercises GblPayloadLib's pure-logic parser
   against in-memory bytes. Used by tests 060/061/063/064/067. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/PayloadParse.h"

static int load_file(const char *path, uint8_t **out_buf, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    size_t n = (size_t)sz;
    uint8_t *buf = malloc(n);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, n, f) != n) { free(buf); fclose(f); return -1; }
    fclose(f);
    *out_buf = buf;
    *out_n   = n;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: parser_harness parse-header <file>\n"
                "       parser_harness find-cached-abl <file>\n"
                "       parser_harness scan-cached-abl <file>\n");
        return 2;
    }

    uint8_t *buf = NULL;
    size_t n = 0;
    if (load_file(argv[2], &buf, &n) != 0) return 2;

    if (strcmp(argv[1], "parse-header") == 0) {
        enum gbl_payload_status s = gbl_payload_validate_header(buf, n);
        printf("status=%d\n", s);
        free(buf);
        return s == GBL_PAYLOAD_OK ? 0 : 1;
    }

    if (strcmp(argv[1], "find-cached-abl") == 0) {
        const uint8_t *pe; size_t pe_size;
        enum gbl_payload_status s =
            gbl_payload_find_cached_abl(buf, n, &pe, &pe_size);
        printf("status=%d size=%zu\n", s, s == GBL_PAYLOAD_OK ? pe_size : 0);
        free(buf);
        return s == GBL_PAYLOAD_OK ? 0 : 1;
    }

    if (strcmp(argv[1], "scan-cached-abl") == 0) {
        const uint8_t *pe; size_t pe_size;
        enum gbl_payload_status s =
            gbl_payload_scan_cached_abl(buf, n, &pe, &pe_size);
        printf("status=%d size=%zu\n", s, s == GBL_PAYLOAD_OK ? pe_size : 0);
        free(buf);
        return s == GBL_PAYLOAD_OK ? 0 : 1;
    }

    fprintf(stderr,
            "unknown subcommand '%s'\n"
            "usage: parser_harness parse-header <file>\n"
            "       parser_harness find-cached-abl <file>\n"
            "       parser_harness scan-cached-abl <file>\n",
            argv[1]);
    free(buf);
    return 2;
}
