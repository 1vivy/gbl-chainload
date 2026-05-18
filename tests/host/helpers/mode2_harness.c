/* tests/host/helpers/mode2_harness.c — host driver for the mode2_profile
   parser and the mode-2 rewrite logic.
   Usage:
     mode2_harness profile-parse <file>     -> prints "status=<n>"
     mode2_harness rewrite <cmd-hex> <profile-file> <buf-file>
        -> rewrites buf in place from profile, prints "rewrote=<0|1>"
           and the new buffer hex on stdout.
   The `rewrite` subcommand is exercised by Task 5; `profile-parse` here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Internal/Mode2Profile.h"

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz ? sz : 1);
    if (sz && fread(buf, 1, sz, f) != (size_t)sz) { exit(2); }
    fclose(f); *n = (size_t)sz; return buf;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "profile-parse") == 0) {
        size_t n; unsigned char *b = slurp(argv[2], &n);
        struct gbl_mode2_profile p;
        enum gbl_m2p_status s = gbl_mode2_profile_parse(b, n, &p);
        printf("status=%d\n", (int)s);
        return 0;
    }
    fprintf(stderr, "usage: mode2_harness profile-parse <file>\n");
    return 2;
}
