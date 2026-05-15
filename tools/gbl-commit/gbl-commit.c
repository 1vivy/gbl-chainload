/* tools/gbl-commit/gbl-commit.c
   POSIX raw write to a target path (file or block device) with optional
   backup-before-write and SHA-256 verify-after-write. Same code on host
   (writes regular files) and Android (writes /dev/block/by-name/efisp). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include "../../GblChainloadPkg/Library/GblPayloadLib/Internal/Sha256.h"

static int read_file(const char *p, uint8_t **out, size_t *out_size) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) { perror(p); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return -1; }
    size_t n = (size_t)st.st_size;
    /* For block devices, fstat may report 0; fall back to lseek-to-end. */
    if (n == 0) {
        off_t cur = lseek(fd, 0, SEEK_END);
        if (cur > 0) n = (size_t)cur;
        lseek(fd, 0, SEEK_SET);
    }
    /* Guard: if size is still 0 after the lseek fallback (some kernels don't
       report block-device size via lseek either), refuse to proceed — a
       zero-length buffer would silently corrupt the destination on restore. */
    if (n == 0) {
        fprintf(stderr, "gbl-commit: cannot determine size of %s\n", p);
        close(fd);
        return -1;
    }
    uint8_t *b = malloc(n);
    if (!b) { close(fd); return -1; }
    ssize_t r = 0; size_t got = 0;
    while (got < n && (r = read(fd, b + got, n - got)) > 0) got += (size_t)r;
    close(fd);
    if (got != n) { fprintf(stderr, "short read on %s\n", p); free(b); return -1; }
    *out = b; *out_size = n;
    return 0;
}

static int write_file(const char *p, const uint8_t *buf, size_t n) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror(p); return -1; }
    ssize_t w = 0; size_t put = 0;
    while (put < n && (w = write(fd, buf + put, n - put)) > 0) put += (size_t)w;
    if (put != n) { fprintf(stderr, "short write on %s\n", p); close(fd); return -1; }
    if (fsync(fd) < 0) { perror("fsync"); close(fd); return -1; }
    close(fd);
    sync();
    return 0;
}

/* Restore backup to dst; used on write failure or SHA mismatch. */
static void restore_backup(const char *dst, const char *backup) {
    fprintf(stderr, "gbl-commit: restoring from %s\n", backup);
    uint8_t *bb = NULL; size_t bs = 0;
    if (read_file(backup, &bb, &bs) == 0) {
        (void)write_file(dst, bb, bs);
        free(bb);
    }
}

int main(int argc, char **argv) {
    const char *src = NULL, *dst = NULL, *backup = NULL;
    int verify = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--src") && i+1 < argc) src = argv[++i];
        else if (!strcmp(argv[i], "--dst") && i+1 < argc) dst = argv[++i];
        else if (!strcmp(argv[i], "--backup") && i+1 < argc) backup = argv[++i];
        else if (!strcmp(argv[i], "--verify")) verify = 1;
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (!src || !dst) {
        fprintf(stderr,
            "usage: gbl-commit --src FILE --dst PATH "
            "[--backup BACKUP_PATH] [--verify]\n");
        return 2;
    }

    uint8_t *src_buf = NULL; size_t src_size = 0;
    if (read_file(src, &src_buf, &src_size) < 0) return 1;

    if (backup) {
        uint8_t *dst_buf = NULL; size_t dst_size = 0;
        if (read_file(dst, &dst_buf, &dst_size) < 0) { free(src_buf); return 1; }
        if (write_file(backup, dst_buf, dst_size) < 0) {
            free(dst_buf); free(src_buf); return 1;
        }
        free(dst_buf);
        fprintf(stderr, "gbl-commit: backed up %s -> %s (%zu bytes)\n",
                dst, backup, dst_size);
    }

    if (write_file(dst, src_buf, src_size) < 0) {
        if (backup) restore_backup(dst, backup);
        free(src_buf);
        return 1;
    }

    if (verify) {
        uint8_t *check_buf = NULL; size_t check_size = 0;
        if (read_file(dst, &check_buf, &check_size) < 0) { free(src_buf); return 1; }

        /* Guard: partition/file must be at least as large as what we wrote. */
        if (check_size < src_size) {
            fprintf(stderr,
                "gbl-commit: verify error: read back %zu bytes but wrote %zu\n",
                check_size, src_size);
            free(check_buf);
            if (backup) restore_backup(dst, backup);
            free(src_buf);
            return 3;
        }

        uint8_t want[32], got[32];
        gbl_sha256(src_buf, src_size, want);
        /* Hash only the first src_size bytes of what was read back. */
        gbl_sha256(check_buf, src_size, got);
        free(check_buf);

        if (memcmp(want, got, 32) != 0) {
            fprintf(stderr, "gbl-commit: SHA mismatch after write\n");
            if (backup) restore_backup(dst, backup);
            free(src_buf);
            return 3;
        }
        fprintf(stderr, "gbl-commit: SHA verify ok\n");
    }

    free(src_buf);
    return 0;
}
