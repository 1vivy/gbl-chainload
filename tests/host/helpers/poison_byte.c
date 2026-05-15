/* tests/host/helpers/poison_byte.c — flip one byte at offset N. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: poison_byte FILE OFFSET XOR\n"); return 2; }
    FILE *f = fopen(argv[1], "r+b");
    if (!f) { perror(argv[1]); return 1; }
    long off = strtol(argv[2], NULL, 0);
    int xor_v = (int)strtol(argv[3], NULL, 0);
    fseek(f, off, SEEK_SET);
    int b = fgetc(f);
    if (b == EOF) { fprintf(stderr, "EOF at offset %ld\n", off); fclose(f); return 1; }
    fseek(f, off, SEEK_SET);
    fputc(b ^ xor_v, f);
    fclose(f);
    return 0;
}
