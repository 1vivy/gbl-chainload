/* tests/host/helpers/parser_harness.c
   Single host harness that exercises GblPayloadLib's pure-logic parser
   against in-memory bytes. Used by tests 060/061/063/064/067. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/PayloadParse.h"

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "parse-header") != 0) {
        fprintf(stderr, "usage: parser_harness parse-header <file>\n");
        return 2;
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END);
    size_t n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != n) { free(buf); fclose(f); return 2; }
    fclose(f);

    enum gbl_payload_status s = gbl_payload_validate_header(buf, n);
    printf("status=%d\n", s);
    free(buf);
    return s == GBL_PAYLOAD_OK ? 0 : 1;
}
