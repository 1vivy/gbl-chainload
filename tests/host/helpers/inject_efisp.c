/* tests/host/helpers/inject_efisp.c — write UTF-16 LE "efisp\0" at offset. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: inject_efisp FILE OFFSET\n"); return 2; }
    FILE *f = fopen(argv[1], "r+b");
    if (!f) { perror(argv[1]); return 1; }
    static const unsigned char pat[12] = {
        0x65,0,0x66,0,0x69,0,0x73,0,0x70,0,0,0
    };
    long off = strtol(argv[2], NULL, 0);
    fseek(f, off, SEEK_SET);
    fwrite(pat, 1, sizeof(pat), f);
    fclose(f);
    return 0;
}
