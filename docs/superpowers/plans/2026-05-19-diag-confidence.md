# diag Pre-Reboot Confidence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `zip/modes/diag` from an environment dump into a no-write pre-reboot EFISP-install confidence check that also collects a `/sdcard/` evidence bundle and reports whether `graft` mode is required, then open a PR.

**Architecture:** One new host tool (`gblp1-inspect`) parses+verifies the GBLP1 container off-line. One new subcommand on the existing `vbmeta-graft` tool (`list-hash`) emits per-partition digest/graft/verdict triples. A rewritten `zip/modes/diag.sh` orchestrates these two tools plus `fv-unwrap`, prints a terse on-screen summary, and tees full state into `/sdcard/gbl-chainload-diag-<ts>/` + a sibling `.tar.gz` for off-device review.

**Tech Stack:** C99 (host + aarch64-Android static via NDK r27), POSIX-sh / busybox-ash for the recovery side, existing `tools/shared/gblp1.h` and `GblChainloadPkg/Library/AvbParseLib` for header/descriptor parsing.

**Spec:** `docs/superpowers/specs/2026-05-19-diag-confidence-design.md`
**Branch / Worktree:** `diag-confidence` at `../gbl-chainload-diag-confidence` (off `main`; already created)

---

## File Structure

Parent repo (`gbl-chainload`):

| File | Change | Responsibility |
|------|--------|----------------|
| `tools/gblp1-inspect/Makefile` | create | host + android-cross build of gblp1-inspect, mirrors `tools/gbl-pack/Makefile` |
| `tools/gblp1-inspect/gblp1-inspect.c` | create | CLI + parse logic: locate GBLP1 magic in a buffer, verify header CRC + every entry SHA-256 + footer, print machine-greppable lines |
| `tools/vbmeta-graft/vbmeta-graft.c` | modify | add `list-hash` subcommand: per-descriptor digest check + graft-natural-offset probe + boot-pass verdict |
| `scripts/build-recovery-tools.sh` | modify | add `gblp1-inspect` to the cross-build tool list |
| `tests/host/084_gblp1_inspect.sh` | create | pack a known overlay, run `gblp1-inspect`, assert lines and exit code on good and corrupted inputs |
| `tests/host/085_vbmeta_descriptor_hash.sh` | create | drive the new `vbmeta-graft list-hash` on `images/grafted-recovery.img` + a perturbed copy, assert verdict columns |
| `tests/host/086_diag_dryrun.sh` | create | run `zip/modes/diag.sh` against a synthetic by-name tree, assert bundle layout + headline tier in HIGH / MEDIUM / LOW / NONE scripted scenarios |

Submodule (`zip/`):

| File | Change | Responsibility |
|------|--------|----------------|
| `zip/modes/diag.sh` | rewrite | orchestrates the checks, tees `ui_print` to `report.txt`, writes the bundle, tar+gzips it |
| `zip/modes/diag.conf` | modify | `MODE_TOOLS="vbmeta-graft fv-unwrap gblp1-inspect"` (was empty) |
| `zip/bin/gblp1-inspect` | added | aarch64-Android static, refreshed by `zip/update-tools.sh` |
| `zip/bin/vbmeta-graft` | refreshed | rebuilt with the new `list-hash` subcommand |
| `zip/bin/MANIFEST` | refreshed | rewritten by `update-tools.sh` after binaries change |

No edits to `scripts/test-device-*.sh`. No edits to EDK2 / on-device chainload code. No edits to install/graft modes.

---

## Task ordering

```
T1 (gblp1-inspect) ──┐
                     ├──> T3 (diag.sh) ──> T4 (build + PR)
T2 (list-hash)   ────┘
```

T1 and T2 are independent and can be implemented in either order or in parallel. T3 consumes the binaries from both. T4 closes out with the cross-built recovery binaries, the refreshed submodule vendoring, the ZIP build, and the PR.

---

### Task 1: gblp1-inspect host tool

**Goal:** A host + aarch64-Android static C tool that parses a GBLP1 container out of an EFISP-style image (base-EFI bytes optionally prepended) and emits machine-greppable per-entry lines with SHA-256 verification.

**Files:**
- Create: `tools/gblp1-inspect/Makefile`
- Create: `tools/gblp1-inspect/gblp1-inspect.c`
- Modify: `scripts/build-recovery-tools.sh`
- Create: `tests/host/084_gblp1_inspect.sh`

**Acceptance Criteria:**
- [ ] `make -C tools/gblp1-inspect` produces a host binary that runs.
- [ ] `make -C tools/gblp1-inspect android` produces a static aarch64-Android binary.
- [ ] Against a valid `payload.bin` (no base EFI prepended), output ends with `result: ok` and exit status 0.
- [ ] Against an `efisp.img`-style file (arbitrary leading bytes followed by a GBLP1 container), the tool finds the GBLP1 magic and validates from there.
- [ ] If the header CRC is wrong, output ends with `result: bad_crc` and exit status non-zero.
- [ ] If any entry's SHA-256 does not match its declared digest, output ends with `result: entry_sha_mismatch` and exit status non-zero.
- [ ] If the input has no `GBLP1\0\0\0` magic anywhere, output is `result: not_a_gblp1` and exit status non-zero.
- [ ] `scripts/build-recovery-tools.sh` now includes `gblp1-inspect` in its cross-build loop, and a re-run produces `dist/recovery/gblp1-inspect`.
- [ ] `bash tests/host/084_gblp1_inspect.sh` prints `PASS: 084 gblp1-inspect`.

**Verify:** `bash tests/host/084_gblp1_inspect.sh` → final line `PASS: 084 gblp1-inspect`.

**Steps:**

- [ ] **Step 1: Create the Makefile.**

Create `tools/gblp1-inspect/Makefile` mirroring `tools/gbl-pack/Makefile`. The tool needs SHA-256 (reuse `GblPayloadLib/Sha256.c`) and CRC-32 (reuse `GblPayloadLib/Crc32.c`), and includes `tools/shared/gblp1.h`. No EDK2 / AVB deps.

```make
# tools/gblp1-inspect/Makefile
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c99
PROJ    := $(realpath ../..)
GPL     := $(PROJ)/GblChainloadPkg/Library/GblPayloadLib

CFLAGS  += -DGBL_HOST_BUILD=1 -I$(GPL)

SRCS    := gblp1-inspect.c $(GPL)/Crc32.c $(GPL)/Sha256.c
HDRS    := ../shared/gblp1.h $(GPL)/Internal/Sha256.h

all: gblp1-inspect

gblp1-inspect: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c99 -DGBL_HOST_BUILD=1 -I$(GPL)

android: gblp1-inspect-android

gblp1-inspect-android: $(SRCS) $(HDRS)
	$(NDK_CC) $(NDK_CFLAGS) -o $@ $(SRCS)

clean:
	rm -f gblp1-inspect gblp1-inspect-android
```

- [ ] **Step 2: Create gblp1-inspect.c.**

Logic (in this order):
1. Slurp the file into a malloc'd buffer.
2. Memscan for `GBLP1\0\0\0` (8 bytes); if not found → print `result: not_a_gblp1` and `return 1`.
3. From the magic offset, read the LE header. Validate `version == 1`, `header_size == 28`, `flags & 1`, `total_size` fits the slurped buffer. Recompute CRC-32 over bytes [magic+0 .. magic+24) and compare to `header_crc32`. On failure print `result: bad_crc` / `result: truncated` / `result: bad_magic` as appropriate and `return 1`.
4. Print one `header:` line summarizing the validated fields.
5. Walk `entry_count` entries (48 bytes each, immediately after the header). For each: print one `entry:` line with type (decoded to text for the three known types and hex otherwise), offset, size, SHA-256 ok/mismatch. Track a fail flag.
6. Verify the 8-byte footer `GBLP1END` at `magic_offset + total_size - 8`. Print `footer:` line.
7. If any entry mismatched → `result: entry_sha_mismatch`, `return 1`. Otherwise `result: ok`, `return 0`.

```c
/* tools/gblp1-inspect/gblp1-inspect.c — GBLP1 container inspector. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../shared/gblp1.h"
#include "Internal/Sha256.h"

extern uint32_t GblCrc32 (const uint8_t *buf, uint32_t len);

static uint16_t rle16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rle32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int slurp(const char *path, uint8_t **out, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fprintf(stderr, "%s: empty\n", path); fclose(f); return 1; }
    uint8_t *b = malloc((size_t)n);
    if (!b) { fclose(f); return 1; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return 1; }
    fclose(f); *out = b; *out_size = (size_t)n; return 0;
}

static const char *type_name(uint16_t t) {
    switch (t) {
        case GBLP1_TYPE_CACHED_ABL:    return "CACHED_ABL";
        case GBLP1_TYPE_SOURCE_META:   return "SOURCE_META";
        case GBLP1_TYPE_MODE2_PROFILE: return "MODE2_PROFILE";
        default:                       return "UNKNOWN";
    }
}

static ssize_t find_magic(const uint8_t *buf, size_t len) {
    if (len < GBLP1_MAGIC_SIZE) return -1;
    for (size_t i = 0; i + GBLP1_MAGIC_SIZE <= len; i++) {
        if (memcmp(buf + i, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) == 0)
            return (ssize_t)i;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: gblp1-inspect <image>\n"); return 2; }
    uint8_t *buf = NULL; size_t blen = 0;
    if (slurp(argv[1], &buf, &blen)) return 1;

    ssize_t mo = find_magic(buf, blen);
    if (mo < 0) { puts("result: not_a_gblp1"); free(buf); return 1; }

    const uint8_t *h = buf + mo;
    size_t avail = blen - (size_t)mo;
    if (avail < GBLP1_HEADER_SIZE) {
        puts("result: truncated"); free(buf); return 1;
    }
    uint16_t version     = rle16(h + 8);
    uint16_t hdr_size    = rle16(h + 10);
    uint32_t flags       = rle32(h + 12);
    uint32_t total_size  = rle32(h + 16);
    uint32_t entry_count = rle32(h + 20);
    uint32_t hdr_crc     = rle32(h + 24);
    if (version != GBLP1_VERSION || hdr_size != GBLP1_HEADER_SIZE
        || (flags & GBLP1_FLAGS_LE) == 0) {
        puts("result: bad_magic"); free(buf); return 1;
    }
    if (total_size > avail || total_size < GBLP1_HEADER_SIZE + GBLP1_FOOTER_SIZE) {
        puts("result: truncated"); free(buf); return 1;
    }
    uint32_t want_crc = GblCrc32(h, 24);
    if (want_crc != hdr_crc) {
        printf("header: magic=ok version=%u header_crc32=BAD(want=%08x got=%08x)\n",
               version, want_crc, hdr_crc);
        puts("result: bad_crc"); free(buf); return 1;
    }
    printf("header: magic=ok version=%u header_crc32=ok total_size=%u entry_count=%u\n",
           version, total_size, entry_count);

    int sha_fail = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = h + GBLP1_HEADER_SIZE + i * GBLP1_ENTRY_SIZE;
        uint16_t type    = rle16(e + 0);
        uint32_t poff    = rle32(e + 4);
        uint32_t psize   = rle32(e + 8);
        const uint8_t *want_sha = e + 16;
        if (poff + psize > total_size) {
            printf("entry: type=0x%04x (%s) offset=0x%x size=%u sha256=OUT_OF_BOUNDS\n",
                   type, type_name(type), poff, psize);
            sha_fail = 1; continue;
        }
        uint8_t got_sha[32];
        GblSha256(h + poff, psize, got_sha);
        int ok = memcmp(got_sha, want_sha, 32) == 0;
        printf("entry: type=0x%04x (%s) offset=0x%x size=%u sha256=%s\n",
               type, type_name(type), poff, psize, ok ? "ok" : "MISMATCH");
        if (!ok) sha_fail = 1;
    }

    const uint8_t *foot = h + total_size - GBLP1_FOOTER_SIZE;
    int footer_ok = memcmp(foot, GBLP1_FOOTER, GBLP1_FOOTER_SIZE) == 0;
    printf("footer: GBLP1END=%s\n", footer_ok ? "ok" : "MISSING");

    free(buf);
    if (!footer_ok) { puts("result: truncated"); return 1; }
    if (sha_fail)   { puts("result: entry_sha_mismatch"); return 1; }
    puts("result: ok");
    return 0;
}
```

(`GblCrc32` and `GblSha256` are exported from `GblPayloadLib/Crc32.c` and `Sha256.c`. Confirm those symbol names by reading the headers before compiling; if the actual symbols differ, adjust the extern declaration and call sites accordingly. The `Internal/Sha256.h` include path is the same one `gbl-pack` uses.)

- [ ] **Step 3: Wire into the cross-build script.**

Edit `scripts/build-recovery-tools.sh`. Change the tool list line:

```sh
  for t in fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile; do
```

to:

```sh
  for t in fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile gblp1-inspect; do
```

- [ ] **Step 4: Create the host test.**

Create `tests/host/084_gblp1_inspect.sh`:

```sh
#!/usr/bin/env bash
# tests/host/084_gblp1_inspect.sh — gblp1-inspect round-trip + failure-mode.
set -euo pipefail
cd "$(dirname "$0")/../.."

PE=images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing" >&2; exit 0; }

make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tools/gblp1-inspect

OUT=tests/host/.last/084
rm -rf "$OUT"; mkdir -p "$OUT"

# Build a valid payload.bin via the same path as 060.
tools/abl-patcher/abl-patcher --in "$PE" --out "$OUT/patched.efi" >/dev/null
tools/gbl-pack/gbl-pack --cached-abl "$OUT/patched.efi" --source "$PE" \
                       --extracted "$PE" --out "$OUT/payload.bin"

# 1. Happy path on a bare payload.
tools/gblp1-inspect/gblp1-inspect "$OUT/payload.bin" > "$OUT/ok.txt"
grep -q '^result: ok$' "$OUT/ok.txt" \
  || { echo "FAIL: clean payload did not produce result: ok"; cat "$OUT/ok.txt"; exit 1; }

# 2. Happy path with arbitrary leading bytes (simulating EFISP = base EFI || GBLP1).
head -c 65536 /dev/urandom > "$OUT/prefix.bin"
cat "$OUT/prefix.bin" "$OUT/payload.bin" > "$OUT/efisp-like.img"
tools/gblp1-inspect/gblp1-inspect "$OUT/efisp-like.img" > "$OUT/ok-prefixed.txt"
grep -q '^result: ok$' "$OUT/ok-prefixed.txt" \
  || { echo "FAIL: prefixed payload did not produce result: ok"; cat "$OUT/ok-prefixed.txt"; exit 1; }

# 3. Corrupt a single entry's payload (flip one byte at offset 0x100, which is inside the
#    first entry payload region given header+entries footprint).
cp "$OUT/payload.bin" "$OUT/corrupt.bin"
python3 -c '
import sys
p=open(sys.argv[1],"r+b"); p.seek(0x100); b=p.read(1); p.seek(0x100); p.write(bytes([b[0]^0xff])); p.close()
' "$OUT/corrupt.bin"
set +e
tools/gblp1-inspect/gblp1-inspect "$OUT/corrupt.bin" > "$OUT/corrupt.txt"
rc=$?
set -e
grep -q '^result: entry_sha_mismatch$' "$OUT/corrupt.txt" \
  || { echo "FAIL: corrupt payload did not produce entry_sha_mismatch"; cat "$OUT/corrupt.txt"; exit 1; }
[ "$rc" != 0 ] \
  || { echo "FAIL: corrupt input returned exit 0"; exit 1; }

# 4. Not-a-gblp1: feed pure random.
head -c 4096 /dev/urandom > "$OUT/random.bin"
set +e
tools/gblp1-inspect/gblp1-inspect "$OUT/random.bin" > "$OUT/random.txt"
rc=$?
set -e
grep -q '^result: not_a_gblp1$' "$OUT/random.txt" \
  || { echo "FAIL: random input did not produce not_a_gblp1"; cat "$OUT/random.txt"; exit 1; }
[ "$rc" != 0 ] \
  || { echo "FAIL: not_a_gblp1 returned exit 0"; exit 1; }

echo "PASS: 084 gblp1-inspect"
```

- [ ] **Step 5: Build + run the test.**

```
make -C tools/gblp1-inspect
bash tests/host/084_gblp1_inspect.sh
```

Expected final line: `PASS: 084 gblp1-inspect`.

- [ ] **Step 6: Commit.**

```
git add tools/gblp1-inspect/ scripts/build-recovery-tools.sh tests/host/084_gblp1_inspect.sh
git commit -m "tools: gblp1-inspect — GBLP1 container inspector for diag"
```

---

### Task 2: vbmeta-graft list-hash subcommand

**Goal:** Add a `list-hash <active-vbmeta> <byname-dir>` subcommand to `vbmeta-graft` that emits per-descriptor lines with three fields — `digest`, `graft`, `verdict` — exactly as specified in §7.4 of the design.

**Files:**
- Modify: `tools/vbmeta-graft/vbmeta-graft.c`
- Create: `tests/host/085_vbmeta_descriptor_hash.sh`

**Acceptance Criteria:**
- [ ] `vbmeta-graft list-hash <self-consistent-vbmeta> <byname-dir>` on a tree where every chained partition matches its descriptor prints `digest=ok ... verdict=match` for hash descriptors and `verdict=match` (with `graft=ok` or `graft=n/a` as appropriate) for chain descriptors that resolve.
- [ ] Perturbing one partition body so its salted SHA-256 no longer matches the descriptor produces `digest=mismatch ... verdict=mismatch` for that line.
- [ ] A chain descriptor whose target has no embedded vbmeta at the natural offset produces `graft=missing verdict=mismatch`.
- [ ] A chain descriptor whose target carries a stock OEM-signed vbmeta at the natural offset produces `graft=ok verdict=match`.
- [ ] Existing `list`, `check`, `graft` subcommands continue to behave as before — `bash tests/host/074_vbmeta_graft.sh` still passes.
- [ ] `bash tests/host/085_vbmeta_descriptor_hash.sh` prints `PASS: 085 vbmeta-graft list-hash`.

**Verify:** `bash tests/host/074_vbmeta_graft.sh && bash tests/host/085_vbmeta_descriptor_hash.sh` → both end with `PASS: …` lines.

**Steps:**

- [ ] **Step 1: Sketch the subcommand structure.**

In `tools/vbmeta-graft/vbmeta-graft.c`, after the existing `cmd_graft` block and before `main`, add:

```c
/* ---- list-hash ------------------------------------------------------ */

struct lh_ctx {
  const char *byname_dir;
  const char *slot_suffix;  /* "a" or "b" derived from active vbmeta path */
  int graft_needed;         /* incremented per partition needing graft mode */
  int fakelock_needed;      /* incremented per direct-hash mismatch */
};

static int cmd_list_hash(const char *mvb_path, const char *byname_dir);
```

And in `main`'s dispatch:

```c
if (strcmp(argv[1], "list-hash") == 0 && argc == 4)
  return cmd_list_hash(argv[2], argv[3]);
```

Update the `usage` block to include the new line:

```
  vbmeta-graft list-hash <active-vbmeta> <byname-dir>
```

- [ ] **Step 2: Implement the per-descriptor callback.**

Walk descriptors of the active main vbmeta (reuse `walk_descriptors`). For each hash descriptor:
- Read `partition_name`, `salt`, `image_size`, `digest`.
- Resolve `<byname_dir>/<partition_name>_<slot_suffix>`. If absent → `digest=missing verdict=mismatch graft=n/a`.
- Compute `SHA-256(salt || partition_bytes[0 .. image_size))` exactly as `libavb` does. Reuse the project's SHA-256 (linked via `AvbParseLib`'s deps; if the lib pulls in `GblPayloadLib/Sha256.c`, use that; if not, link `Sha256.c` directly through the Makefile — confirm during implementation by inspecting the existing Makefile's symbols).
- Compare to the descriptor digest. Emit `digest=ok` / `digest=mismatch`. `graft=n/a` always for plain hash. `verdict` follows `digest`.

For each chain descriptor:
- Read `partition_name`, `rollback_index_location`, `public_key`. `digest=n/a`, `image_size` unknown to us — declared field becomes `declared=-`.
- Resolve `<byname_dir>/<partition_name>_<slot_suffix>`. If absent → `graft=missing verdict=mismatch`.
- Probe for a valid AVB vbmeta blob: walk the file from offset 0 upward in 4 KiB strides starting at `round_up(custom_content_size, 4K)` and read the first `AVB0` magic; for v1 of this tool, simply scan the whole partition for `AVB0` and validate the first hit's signature against the descriptor's `public_key` (the same key the existing `cmd_check` validates against). On success → `graft=ok verdict=match`. On failure → `graft=missing verdict=mismatch`.
- The "custom_content_size" cannot be known from the host without re-reading whatever produced the on-disk image. The pragmatic approximation: scan for an `AVB0`-magic + valid-signature blob anywhere in the partition; if found and the key matches → `graft=ok`. The diag use case is "did some legitimate stock vbmeta end up grafted onto this partition" and that question is answered by signature, not offset.

Emit lines in the format prescribed in §7.4 of the spec, one per descriptor.

Increment `graft_needed` when `verdict=mismatch && graft=missing` on a chain descriptor (or any descriptor where graft applies). Increment `fakelock_needed` when `verdict=mismatch && graft=n/a` on a hash descriptor. Print neither tally on stdout (diag.sh derives them from the lines); return exit 0 unless I/O failed.

- [ ] **Step 3: Derive `slot_suffix` from the active vbmeta path.**

`cmd_list_hash`'s first arg is a file path like `<byname>/vbmeta_a` or a temp copy named `main_vbmeta.img`. To stay robust, accept an explicit env override `GBL_VBMETA_SLOT` (`a` or `b`); otherwise tail-match `_a` / `_b` on the basename; else default to `a` and write a single `note: slot suffix defaulted to 'a'` line on stderr.

- [ ] **Step 4: Create the host test.**

Create `tests/host/085_vbmeta_descriptor_hash.sh`:

```sh
#!/usr/bin/env bash
# tests/host/085_vbmeta_descriptor_hash.sh — vbmeta-graft list-hash.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/vbmeta-graft

FX=images/grafted-recovery.img
[ -f "$FX" ] || { echo "SKIP: $FX absent"; exit 0; }

OUT=tests/host/.last/085
rm -rf "$OUT"; mkdir -p "$OUT/byname"
VG=tools/vbmeta-graft/vbmeta-graft

# Build a synthetic by-name dir: copy the fixture into byname/recovery_a.
# The fixture's embedded vbmeta is what list-hash will both parse and (for
# chain descriptors, if any) probe.
cp "$FX" "$OUT/byname/recovery_a"

# Provide an active vbmeta — for this test we use the fixture itself as the
# main vbmeta target. list-hash walks its descriptors against the byname
# dir. The exact descriptor set depends on the fixture; the test asserts
# only on stable shape, not specific partitions.
GBL_VBMETA_SLOT=a "$VG" list-hash "$FX" "$OUT/byname" > "$OUT/lh.txt" 2>&1 \
  || { echo "FAIL: list-hash exited nonzero"; cat "$OUT/lh.txt"; exit 1; }

# Every emitted line must carry digest=, graft=, verdict= fields.
awk '/^partition=/ {
  if (!/digest=/ || !/graft=/ || !/verdict=/) {
    print "BAD LINE:", $0; exit 1
  }
}' "$OUT/lh.txt" \
  || { echo "FAIL: a partition line is missing required fields"; cat "$OUT/lh.txt"; exit 1; }

# Perturb a body byte and re-run; at least one verdict must flip to mismatch.
cp "$OUT/byname/recovery_a" "$OUT/byname/recovery_a.bak"
python3 -c '
import sys
p=open(sys.argv[1],"r+b"); p.seek(0); p.write(b"\x00\x00\x00\x00"); p.close()
' "$OUT/byname/recovery_a"

GBL_VBMETA_SLOT=a "$VG" list-hash "$FX" "$OUT/byname" > "$OUT/lh-corrupt.txt" 2>&1 \
  || true
grep -q 'verdict=mismatch' "$OUT/lh-corrupt.txt" \
  || { echo "FAIL: perturbed byname did not produce verdict=mismatch"; cat "$OUT/lh-corrupt.txt"; exit 1; }

# Restore.
mv "$OUT/byname/recovery_a.bak" "$OUT/byname/recovery_a"

# Regression: existing list / check / graft must still work — 074 covers that.
bash tests/host/074_vbmeta_graft.sh > "$OUT/074.log" 2>&1 \
  || { echo "FAIL: 074 regressed"; cat "$OUT/074.log"; exit 1; }

echo "PASS: 085 vbmeta-graft list-hash"
```

(If the fixture's descriptors are all hash descriptors and the chain-descriptor branch is uncovered, this test still validates the format and the digest path. The chain-descriptor branch will be exercised on-device; if a synthetic chain-descriptor fixture is cheap to build during implementation, add it — but do not block the task on it. The diag.sh dry-run test in Task 3 also exercises this via mock data.)

- [ ] **Step 5: Build + run the tests.**

```
make -C tools/vbmeta-graft
bash tests/host/074_vbmeta_graft.sh   # regression
bash tests/host/085_vbmeta_descriptor_hash.sh
```

Both must end with `PASS: …`.

- [ ] **Step 6: Commit.**

```
git add tools/vbmeta-graft/vbmeta-graft.c tests/host/085_vbmeta_descriptor_hash.sh
git commit -m "tools(vbmeta-graft): list-hash subcommand — per-descriptor verdict for diag"
```

---

### Task 3: diag.sh rewrite + bundle collection

**Goal:** Rewrite `zip/modes/diag.sh` so it produces the on-screen summary from §4 of the spec, writes the `/sdcard/gbl-chainload-diag-<ts>/` directory + sibling `.tar.gz`, and reflects the four confidence tiers. Refresh `diag.conf`. Add `tests/host/086_diag_dryrun.sh` in the parent repo to exercise the script against synthetic by-name fixtures.

**Files:**
- Modify: `zip/modes/diag.sh` (submodule)
- Modify: `zip/modes/diag.conf` (submodule)
- Create: `tests/host/086_diag_dryrun.sh` (parent)
- Create: `tests/host/helpers/diag_fake_byname.sh` (parent — small helper that builds a synthetic byname tree from `tests/host/.last/*` artifacts and the fixtures)

**Acceptance Criteria:**
- [ ] `diag.sh` produces output matching the example in §4 of the spec (allowing for different per-line values, but the section headings — `EFISP`, `loader-ABL`, `graft needed`, `fakelock req`, `logfs history`, `confidence`, `bundle saved` — must all appear).
- [ ] A bundle directory `gbl-chainload-diag-<ts>/` is created under `/sdcard` (or `$BUNDLE_ROOT` env in tests) containing the exact filenames listed in §5 of the spec.
- [ ] A sibling `gbl-chainload-diag-<ts>.tar.gz` exists and `tar -tzf` lists every bundled file.
- [ ] HIGH tier headline is produced when scripted state has valid GBLP1 + base-EFI match + at least one slot with loader path. MEDIUM when neither slot has the loader path. LOW when GBLP1 is corrupt. NONE when EFISP is not a PE.
- [ ] `diag.conf` declares `MODE_TOOLS="vbmeta-graft fv-unwrap gblp1-inspect"`.
- [ ] `bash tests/host/086_diag_dryrun.sh` prints `PASS: 086 diag dryrun`.

**Verify:** `bash tests/host/086_diag_dryrun.sh` → final line `PASS: 086 diag dryrun`.

**Steps:**

- [ ] **Step 1: Update `zip/modes/diag.conf`.**

```sh
# shellcheck shell=sh
# shellcheck disable=SC2034
# modes/diag.conf — declarative config for the diag mode.
# diag now performs a pre-reboot EFISP-install confidence check and
# writes a state bundle to /sdcard/. Still zero device writes.

MODE_NAME="diag"
MODE_DESC="pre-reboot install-confidence diagnostic (no writes); writes a bundle to /sdcard/"
MODE_WRITES=""
MODE_TOOLS="vbmeta-graft fv-unwrap gblp1-inspect"
MODE_EFI=""
MODE_LIB=""
```

- [ ] **Step 2: Rewrite `zip/modes/diag.sh`.**

Replace the existing file with the structure below. Helpers tee a one-line summary to `ui_print` and write a more verbose dump to a bundle file. Use only POSIX-sh / busybox-ash; no bashisms; no `xxd`; no `sha256sum`.

```sh
# shellcheck shell=sh
# shellcheck disable=SC2154,SC2012
# modes/diag.sh — pre-reboot EFISP-install confidence + state-bundle.
# Zero device writes. Produces /sdcard/gbl-chainload-diag-<ts>/ and a
# sibling .tar.gz that off-device analysis can chew on. See
# docs/superpowers/specs/2026-05-19-diag-confidence-design.md.

TS=$(date +%Y%m%d-%H%M%S 2>/dev/null || echo unknown)
BUNDLE_ROOT="${BUNDLE_ROOT:-/sdcard}"
BUNDLE_DIR="$BUNDLE_ROOT/gbl-chainload-diag-$TS"
BUNDLE_TGZ="$BUNDLE_ROOT/gbl-chainload-diag-$TS.tar.gz"

# Track scalar state for the confidence-tier decision.
EFISP_PE=0           # 1 if EFISP starts with MZ
GBLP1_OK=0           # 1 if gblp1-inspect ended with result: ok
BASE_EFI_MODE=""     # mode-0 / mode-1 / mode-2 / unknown (matches MANIFEST hashes)
LOADER_PATH_A=0      # 1 if abl_a retains loader path
LOADER_PATH_B=0      # 1 if abl_b retains loader path
GRAFT_NEEDED_LIST="" # space-separated partitions needing graft mode
FAKELOCK_NEEDED_LIST="" # space-separated partitions needing mode-1 (hash mismatch)
LOGFS_HISTORY=0      # count of prior GblChainload_BootN.txt
LOGFS_NEWEST=""      # newest filename

# ----- bundle plumbing ------------------------------------------------

prepare_bundle() {
  mkdir -p "$BUNDLE_DIR" || abort "cannot create $BUNDLE_DIR"
  # Free-space gate.
  free_kb=$(df -k "$BUNDLE_ROOT" 2>/dev/null | awk 'NR==2 {print $4}')
  case "$free_kb" in
    ''|*[!0-9]*) ;;
    *) [ "$free_kb" -lt 102400 ] && abort "less than 100 MiB free on $BUNDLE_ROOT" ;;
  esac
  : > "$BUNDLE_DIR/report.txt"
}

# Override ui_print to tee to report.txt while still printing via core.
_orig_ui_print=$(command -v ui_print)
ui_print() {
  echo "$1" >> "$BUNDLE_DIR/report.txt"
  # Defer to core ui_print (recovery I/O).
  echo "ui_print $1
ui_print" >> "/proc/self/fd/$OUTFD" 2>/dev/null || true
}

# ----- env --------------------------------------------------------------

collect_env() {
  {
    echo "boot mode  : $($BOOTMODE && echo 'booted Android' || echo 'recovery')"
    echo "slot       : active=$SLOT inactive=$INACTIVE"
    echo "byname dir : $BYNAME"
    ls -1 "$BYNAME" 2>/dev/null | sort
    echo "---"
    busybox 2>&1 | head -1 || true
  } > "$BUNDLE_DIR/env.txt"
  getprop > "$BUNDLE_DIR/getprop.boot.txt" 2>/dev/null || true
}

# ----- EFISP -----------------------------------------------------------

collect_efisp() {
  efisp=$(byname efisp)
  [ -n "$efisp" ] || { ui_print "  EFISP        : partition not found"; return; }
  dd if="$efisp" of="$BUNDLE_DIR/efisp.img" bs=1M 2>/dev/null
  _mz=$(dd if="$BUNDLE_DIR/efisp.img" bs=1 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
  if [ "$_mz" != "4d5a" ]; then
    ui_print "  EFISP        : not a PE (first 2 bytes = $_mz)"
    return
  fi
  EFISP_PE=1
  gblp1-inspect "$BUNDLE_DIR/efisp.img" > "$BUNDLE_DIR/gblp1-inspect.txt" 2>&1 \
    && GBLP1_OK=1 || GBLP1_OK=0

  # Identify which base EFI is concat-prefixed. Hash the bytes before the
  # GBLP1 magic and compare with the three hashes in zip/bin/MANIFEST.
  # We don't have sha256sum reliably, so re-use gblp1-inspect's parser to
  # learn the magic offset (it emits header: only after locating it). For
  # v1, the simpler heuristic: extract the prefix length from the file
  # size minus total_size minus footer (parsed from the gblp1-inspect
  # output), then compute SHA-256 by feeding the prefix through busybox
  # if available; if not, skip base-EFI fingerprinting and report
  # `base-EFI: unknown (sha256sum unavailable)`.
  if command -v sha256sum >/dev/null 2>&1; then
    # Parse total_size out of gblp1-inspect output.
    _ts=$(awk '/^header:/{ for(i=1;i<=NF;i++) if ($i ~ /^total_size=/) {split($i,a,"="); print a[2]} }' "$BUNDLE_DIR/gblp1-inspect.txt")
    if [ -n "$_ts" ] && [ "$_ts" -gt 0 ]; then
      _total=$(stat -c%s "$BUNDLE_DIR/efisp.img")
      _prefix_len=$((_total - _ts))
      if [ "$_prefix_len" -gt 0 ]; then
        _prefix_sha=$(dd if="$BUNDLE_DIR/efisp.img" bs=1 count=$_prefix_len 2>/dev/null \
                       | sha256sum | awk '{print $1}')
        # Read manifest, find matching mode.
        while IFS= read -r _line; do
          _h=$(echo "$_line" | awk '{print $1}')
          _f=$(echo "$_line" | awk '{print $2}')
          case "$_f" in
            base/mode-0.efi) [ "$_h" = "$_prefix_sha" ] && BASE_EFI_MODE=mode-0 ;;
            base/mode-1.efi) [ "$_h" = "$_prefix_sha" ] && BASE_EFI_MODE=mode-1 ;;
            base/mode-2.efi) [ "$_h" = "$_prefix_sha" ] && BASE_EFI_MODE=mode-2 ;;
          esac
        done < "$WORKDIR/bin/MANIFEST"
      fi
    fi
  fi

  if [ "$GBLP1_OK" = 1 ]; then
    _entries=$(awk -F'=' '/^header:/{ for(i=1;i<=NF;i++) if ($i ~ /entry_count/) {sub(/.*entry_count=/,""); print $0; exit} }' "$BUNDLE_DIR/gblp1-inspect.txt")
    ui_print "  EFISP        : ${BASE_EFI_MODE:-unknown-base} + GBLP1 v1 ok"
  else
    _why=$(awk -F': ' '/^result: /{print $2; exit}' "$BUNDLE_DIR/gblp1-inspect.txt")
    ui_print "  EFISP        : PE present, GBLP1 ${_why:-error}"
  fi
}

# ----- loader-ABL ------------------------------------------------------

# 10-byte UTF-16 LE "efisp" pattern: 65 00 66 00 69 00 73 00 70 00
EFISP_HEX='65006600690073007000'

scan_for_loader_path() {  # $1 = path to unwrapped PE
  od -An -tx1 -v "$1" 2>/dev/null | tr -d ' \n' | grep -q "$EFISP_HEX"
}

collect_abl() {
  for _slot in a b; do
    _dev=$(byname "abl_$_slot")
    [ -n "$_dev" ] || continue
    dd if="$_dev" of="$BUNDLE_DIR/abl_$_slot.img" bs=1M 2>/dev/null
    fv-unwrap "$BUNDLE_DIR/abl_$_slot.img" "$WORKDIR/abl_$_slot.pe" >/dev/null 2>&1 || continue
    if scan_for_loader_path "$WORKDIR/abl_$_slot.pe"; then
      eval "LOADER_PATH_$(echo $_slot | tr a-z A-Z)=1"
      echo "abl_$_slot: retains loader path" >> "$BUNDLE_DIR/loader-abl.txt"
    else
      echo "abl_$_slot: does NOT retain loader path — WON'T LOAD EFISP" >> "$BUNDLE_DIR/loader-abl.txt"
    fi
  done
  _a=$([ "$LOADER_PATH_A" = 1 ] && echo "abl_a retains loader path" || echo "abl_a does NOT — WON'T LOAD EFISP")
  _b=$([ "$LOADER_PATH_B" = 1 ] && echo "abl_b retains loader path" || echo "abl_b does NOT — WON'T LOAD EFISP")
  ui_print "  loader-ABL   : $_a ; $_b"
}

# ----- vbmeta + graft --------------------------------------------------

collect_vbmeta() {
  for _slot in a b; do
    _dev=$(byname "vbmeta_$_slot")
    [ -n "$_dev" ] || continue
    dd if="$_dev" of="$BUNDLE_DIR/vbmeta_$_slot.img" bs=1M 2>/dev/null
  done
  if [ -f "$BUNDLE_DIR/vbmeta_$SLOT.img" ]; then
    vbmeta-graft list "$BUNDLE_DIR/vbmeta_$SLOT.img" > "$BUNDLE_DIR/vbmeta-descriptors.txt" 2>&1 || true
  fi
}

check_graft() {
  [ -f "$BUNDLE_DIR/vbmeta_$SLOT.img" ] || { ui_print "  graft needed : UNKNOWN (no active vbmeta)"; return; }
  GBL_VBMETA_SLOT="$SLOT" vbmeta-graft list-hash \
    "$BUNDLE_DIR/vbmeta_$SLOT.img" "$BYNAME" > "$BUNDLE_DIR/graft-verdict.txt" 2>&1 || true

  # Bucket mismatches: graft=missing -> graft mode; graft=n/a -> fakelock.
  GRAFT_NEEDED_LIST=$(awk '/verdict=mismatch/ && /graft=missing/ { for(i=1;i<=NF;i++) if($i ~ /^partition=/) {split($i,a,"="); printf "%s ",a[2]} }' "$BUNDLE_DIR/graft-verdict.txt" | sed 's/ $//')
  FAKELOCK_NEEDED_LIST=$(awk '/verdict=mismatch/ && /graft=n\/a/ { for(i=1;i<=NF;i++) if($i ~ /^partition=/) {split($i,a,"="); printf "%s ",a[2]} }' "$BUNDLE_DIR/graft-verdict.txt" | sed 's/ $//')

  if [ -n "$GRAFT_NEEDED_LIST" ]; then
    ui_print "  graft needed : YES — $GRAFT_NEEDED_LIST"
  else
    ui_print "  graft needed : NO   (no chained-partition mismatches without a valid graft)"
  fi
  if [ -n "$FAKELOCK_NEEDED_LIST" ]; then
    ui_print "  fakelock req : YES — $FAKELOCK_NEEDED_LIST"
  else
    ui_print "  fakelock req : NO   (no direct-hash mismatches in main vbmeta)"
  fi
}

# ----- logfs history ----------------------------------------------------

collect_logfs() {
  _dev=$(byname logfs)
  if [ -z "$_dev" ]; then
    ui_print "  logfs history: NO logfs partition"
    return
  fi
  dd if="$_dev" of="$BUNDLE_DIR/logfs.img" bs=1M 2>/dev/null
  LOGFS_HISTORY=$(grep -aoE 'GblChainload_Boot[0-9]+\.txt' "$BUNDLE_DIR/logfs.img" 2>/dev/null | sort -u | wc -l)
  LOGFS_NEWEST=$(grep -aoE 'GblChainload_Boot[0-9]+\.txt' "$BUNDLE_DIR/logfs.img" 2>/dev/null \
                  | sort -t'_' -k3 -n | tail -1)
  if [ "$LOGFS_HISTORY" -gt 0 ]; then
    ui_print "  logfs history: $LOGFS_HISTORY prior gbl-chainload boots (newest: $LOGFS_NEWEST)"
  else
    ui_print "  logfs history: no prior gbl-chainload boots recorded"
  fi
}

# ----- confidence tier --------------------------------------------------

decide_tier() {
  if [ "$EFISP_PE" = 0 ]; then
    echo "NONE — EFISP is not a PE"
    return
  fi
  if [ "$GBLP1_OK" = 0 ]; then
    echo "LOW — GBLP1 invalid or unverified"
    return
  fi
  if [ -z "$BASE_EFI_MODE" ]; then
    # Don't refuse to give a tier just because we couldn't fingerprint;
    # treat as MEDIUM at worst, HIGH if a slot has the loader path.
    if [ "$LOADER_PATH_A" = 1 ] || [ "$LOADER_PATH_B" = 1 ]; then
      echo "MEDIUM — GBLP1 valid but base-EFI unknown (sha256sum unavailable?)"
    else
      echo "MEDIUM — GBLP1 valid; neither slot's ABL retains loader path"
    fi
    return
  fi
  if [ "$LOADER_PATH_A" = 1 ] || [ "$LOADER_PATH_B" = 1 ]; then
    echo "HIGH — safe to reboot into chainload"
  else
    echo "MEDIUM — GBLP1 valid; neither slot's ABL retains loader path (EFISP won't be loaded)"
  fi
}

# ----- finalize --------------------------------------------------------

finalize_bundle() {
  (cd "$BUNDLE_ROOT" && tar -czf "$BUNDLE_TGZ" "$(basename "$BUNDLE_DIR")") 2>/dev/null \
    || ui_print "  (tar.gz creation failed — see $BUNDLE_DIR/ directly)"
}

# ----- entry point ------------------------------------------------------

mode_main() {
  prepare_bundle
  ui_print "diag: pre-reboot install confidence"
  collect_env
  collect_efisp
  collect_abl
  collect_vbmeta
  check_graft
  collect_logfs
  ui_print "  confidence   : $(decide_tier)"
  ui_print ""
  finalize_bundle
  ui_print "  bundle saved : $BUNDLE_TGZ"
  ui_print "                 directory:  $BUNDLE_DIR/"
}
```

(Notes — the script intentionally avoids `set -e` because `diag` should keep going through individual probe failures and still emit a partial bundle. Each helper guards its own dependencies and degrades to a clearly-labeled "UNKNOWN"/"failed" line on the screen. The `WORKDIR/bin/MANIFEST` path mirrors how the parent's `build-recovery-zip.sh` lays out the staged ZIP at install time — confirm during implementation by reading `core/*.sh` if the layout has changed.)

- [ ] **Step 3: Create the helper for the dry-run test.**

Create `tests/host/helpers/diag_fake_byname.sh`:

```sh
#!/usr/bin/env bash
# tests/host/helpers/diag_fake_byname.sh — build a synthetic by-name dir
# plus a synthetic recovery environment so diag.sh can run on the host.
#
# Usage: diag_fake_byname.sh <out-root> <scenario>
#   <scenario>: high | medium | low | none
set -euo pipefail

OUT="$1"
SCENARIO="$2"
rm -rf "$OUT"; mkdir -p "$OUT/byname" "$OUT/work" "$OUT/sdcard" "$OUT/zip"

# 1. EFISP fixture — built from tests/host/.last/060/payload.bin with a
#    fake base-EFI prefix matching one of zip/bin/MANIFEST's hashes.
ZIP_ROOT="$(cd "$(dirname "$0")/../../.."; pwd)/zip"
MANIFEST="$ZIP_ROOT/bin/MANIFEST"
BASE_EFI="$ZIP_ROOT/base/mode-1.efi"

case "$SCENARIO" in
  high|medium)
    cat "$BASE_EFI" tests/host/.last/060/payload.bin > "$OUT/byname/efisp"
    ;;
  low)
    cp "$BASE_EFI" "$OUT/byname/efisp"  # PE but no GBLP1
    head -c 64 /dev/urandom >> "$OUT/byname/efisp"
    ;;
  none)
    head -c 4096 /dev/urandom > "$OUT/byname/efisp"  # not a PE
    ;;
esac

# 2. ABL fixtures — for `high` give abl_a the loader-path signature
#    (UTF-16-LE "efisp"), for `medium` give neither slot the signature.
mkabl() {
  out="$1"; with_loader="$2"
  head -c 1048576 /dev/urandom > "$out"
  if [ "$with_loader" = 1 ]; then
    printf 'e\0f\0i\0s\0p\0' > "$OUT/work/efisp_pat.bin"
    dd if="$OUT/work/efisp_pat.bin" of="$out" bs=1 seek=1024 conv=notrunc 2>/dev/null
  fi
}
case "$SCENARIO" in
  high) mkabl "$OUT/byname/abl_a" 1; mkabl "$OUT/byname/abl_b" 0 ;;
  medium|low|none) mkabl "$OUT/byname/abl_a" 0; mkabl "$OUT/byname/abl_b" 0 ;;
esac

# 3. vbmeta + logfs — stub with empty fixtures (descriptors / history
#    not asserted in this test; only file presence is).
head -c 65536 /dev/urandom > "$OUT/byname/vbmeta_a"
head -c 65536 /dev/urandom > "$OUT/byname/vbmeta_b"
head -c 65536 /dev/urandom > "$OUT/byname/logfs"

# 4. Stage the zip/ tree so $WORKDIR/bin/MANIFEST resolves.
cp -r "$ZIP_ROOT"/* "$OUT/zip/"
```

- [ ] **Step 4: Create the dry-run test.**

Create `tests/host/086_diag_dryrun.sh`:

```sh
#!/usr/bin/env bash
# tests/host/086_diag_dryrun.sh — drive zip/modes/diag.sh against a
# synthetic environment in each confidence tier.
set -euo pipefail
cd "$(dirname "$0")/../.."

[ -f tests/host/.last/060/payload.bin ] || bash tests/host/060_pack_roundtrip.sh

# Ensure native tools are built (used via PATH inside diag.sh).
make -s -C tools/gblp1-inspect
make -s -C tools/vbmeta-graft
make -s -C tools/fv-unwrap

OUT=tests/host/.last/086
rm -rf "$OUT"; mkdir -p "$OUT"

run_one() {
  scenario="$1"; expect="$2"
  envdir="$OUT/$scenario"
  bash tests/host/helpers/diag_fake_byname.sh "$envdir" "$scenario"

  # Fake recovery environment.
  export WORKDIR="$envdir/zip"
  export BYNAME="$envdir/byname"
  export BUNDLE_ROOT="$envdir/sdcard"
  export SLOT=a
  export INACTIVE=b
  export OTA_POSTINSTALL=false
  # Cheap byname() shim used by core scripts:
  byname() { for f in "$BYNAME/$1"; do [ -e "$f" ] && echo "$f"; done; }
  export -f byname
  # ui_print writes to OUTFD; create a pipe-stand-in.
  exec 9>"$envdir/screen.txt"; export OUTFD=9
  abort() { echo "ABORT: $*"; exit 1; }
  export -f abort
  BOOTMODE() { return 1; }; export -f BOOTMODE
  # PATH so diag.sh can call our tools by bare name.
  export PATH="$PWD/tools/gblp1-inspect:$PWD/tools/vbmeta-graft:$PWD/tools/fv-unwrap:$PATH"

  # Source diag and call mode_main.
  set +e
  bash -c '. "$WORKDIR/modes/diag.sh"; mode_main' > "$envdir/stdout.txt" 2>&1
  rc=$?
  exec 9>&-
  set -e

  bundle=$(ls -d "$BUNDLE_ROOT"/gbl-chainload-diag-* 2>/dev/null | head -1)
  [ -n "$bundle" ] || { echo "FAIL [$scenario]: no bundle dir"; cat "$envdir/stdout.txt"; exit 1; }
  for f in report.txt env.txt efisp.img abl_a.img abl_b.img \
           vbmeta_a.img vbmeta_b.img logfs.img \
           gblp1-inspect.txt loader-abl.txt vbmeta-descriptors.txt \
           graft-verdict.txt; do
    [ -e "$bundle/$f" ] || { echo "FAIL [$scenario]: missing $f"; exit 1; }
  done
  [ -f "$bundle.tar.gz" ] || { echo "FAIL [$scenario]: tar.gz missing"; exit 1; }
  grep -q "confidence   : $expect" "$bundle/report.txt" \
    || { echo "FAIL [$scenario]: expected tier '$expect' not in report"; cat "$bundle/report.txt"; exit 1; }
  echo "OK [$scenario] -> $expect"
}

run_one high   HIGH
run_one medium MEDIUM
run_one low    LOW
run_one none   NONE

echo "PASS: 086 diag dryrun"
```

(The test stubs `BOOTMODE`, `abort`, `byname`, and the recovery I/O fd. It is reasonable for an implementer to discover during build-out that the diag script depends on additional recovery-environment helpers; if so, stub them here rather than in `diag.sh` itself — `diag.sh` is meant to run in real recovery.)

- [ ] **Step 5: Build, run.**

```
make -C tools/gblp1-inspect
make -C tools/vbmeta-graft
make -C tools/fv-unwrap
bash tests/host/086_diag_dryrun.sh
```

Expected final line: `PASS: 086 diag dryrun`.

- [ ] **Step 6: Commit in the submodule, then in the parent.**

```
cd zip
git add modes/diag.sh modes/diag.conf
git commit -m "diag: pre-reboot EFISP-install confidence + /sdcard/ bundle"
cd ..
git add zip tests/host/086_diag_dryrun.sh tests/host/helpers/diag_fake_byname.sh
git commit -m "diag(submodule-bump): wire diag.sh rewrite + add host dryrun test"
```

(The parent commit also bumps the submodule SHA. If the host helpers live in the parent, they commit in the same parent commit as the submodule bump.)

---

### Task 4: Cross-build, refresh vendored bins, build the ZIP, push, open PR

**Goal:** Produce the aarch64-Android binaries for the two changed tools (and the new tool), refresh `zip/bin/` + `zip/bin/MANIFEST` via `zip/update-tools.sh`, build the diag-mode ZIP, push the branch to the remote, and open a PR against `main`.

**Files:**
- Modify (regenerate): `zip/bin/MANIFEST` (submodule)
- Modify (regenerate): `zip/bin/gblp1-inspect`, `zip/bin/vbmeta-graft` (submodule)
- Generates: `dist/gbl-chainload-diag.zip`

**Acceptance Criteria:**
- [ ] `scripts/build-recovery-tools.sh` succeeds and `dist/recovery/gblp1-inspect` exists.
- [ ] `zip/update-tools.sh` succeeds, `zip/bin/MANIFEST` includes a `gblp1-inspect` line, and the vbmeta-graft binary hash in `MANIFEST` has changed from the previous parent commit.
- [ ] `scripts/build-recovery-zip.sh --mode diag` produces `dist/gbl-chainload-diag.zip` and exits 0.
- [ ] `bash tests/runall.sh` ends with `ALL TESTS PASS` (host suite green, including 084 / 085 / 086).
- [ ] Branch `diag-confidence` is pushed to the remote.
- [ ] `gh pr create` returns a PR URL targeting `main`.

**Verify:** PR URL exists and `gh pr checks <PR#>` reports CI status for the branch.

**Steps:**

- [ ] **Step 1: Cross-build all recovery tools.**

```
bash scripts/build-recovery-tools.sh
```

Confirm `dist/recovery/` lists `fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile gblp1-inspect`.

- [ ] **Step 2: Refresh the vendored binaries in `zip/`.**

```
cd zip
./update-tools.sh
```

Inspect `bin/MANIFEST` to confirm there is a `bin/gblp1-inspect` line and that `bin/vbmeta-graft`'s hash differs from the previously committed manifest.

- [ ] **Step 3: Commit the refreshed binaries inside the submodule.**

```
git add bin/MANIFEST bin/gblp1-inspect bin/vbmeta-graft
git commit -m "bin: refresh vendored binaries — gblp1-inspect + vbmeta-graft list-hash"
cd ..
```

- [ ] **Step 4: Bump the submodule pointer in the parent.**

```
git add zip
git commit -m "diag(submodule-bump): refreshed bin/ for diag-confidence"
```

- [ ] **Step 5: Build the diag ZIP and run the full host suite.**

```
bash scripts/build-recovery-zip.sh --mode diag
bash tests/runall.sh
```

`dist/gbl-chainload-diag.zip` must exist. `tests/runall.sh` final line must read `ALL TESTS PASS`.

- [ ] **Step 6: Push and open the PR.**

```
git push -u origin diag-confidence

gh pr create --base main --title "diag: pre-reboot EFISP-install confidence + /sdcard/ bundle" --body "$(cat <<'EOF'
## Summary
- Rewrite `zip/modes/diag` from an environment dump into a no-write pre-reboot EFISP-install confidence check.
- Add a `/sdcard/gbl-chainload-diag-<ts>/` evidence bundle (+ sibling `.tar.gz`) for off-device analysis.
- Add `tools/gblp1-inspect` (host + aarch64-Android static) for GBLP1 verification.
- Add `vbmeta-graft list-hash` — per-descriptor digest / graft / verdict triples.

## Test plan
- [ ] `bash tests/runall.sh` (includes new 084 / 085 / 086) ends with `ALL TESTS PASS`.
- [ ] On-device: flash `dist/gbl-chainload-diag.zip` after a `mode-N-install`, confirm the on-screen summary reports the expected tier and the bundle lands in `/sdcard/`.

Design: `docs/superpowers/specs/2026-05-19-diag-confidence-design.md`
Plan:   `docs/superpowers/plans/2026-05-19-diag-confidence.md`
EOF
)"
```

- [ ] **Step 7: Record the PR URL.**

Capture the URL from `gh pr create`'s output and report it back as the task's deliverable.

---

## Self-review

- **Spec coverage:** §2 scope — T1+T3 cover EFISP inspection; T2+T3 cover vbmeta walk + graft verdict; T3 covers logfs history + loader-ABL check + bundle. §4 on-screen output — T3 step 2. §4.1 tiers — T3 `decide_tier`. §4.2 graft verdict — T2 emit + T3 bucket. §5 bundle layout — T3 helpers. §6 layout-naming overlap — `getprop.boot.txt` in `collect_env`. §7.3 gblp1-inspect — T1. §7.4 list-hash — T2. §7.5 build + packaging — T4. §8 tests — 084/085/086 in T1/T2/T3. §10 PR plumbing — T4.
- **Placeholder scan:** No TBDs / TODOs. The base-EFI fingerprinting block in `diag.sh` is documented as degrading to "unknown" if `sha256sum` is absent — that is a stated runtime behavior, not a placeholder. The chain-descriptor probe in T2 step 2 calls out that the chain-only synthetic fixture is optional; the test still validates the format on the available fixture.
- **Type consistency:** `BASE_EFI_MODE` / `LOADER_PATH_A` / `LOADER_PATH_B` / `GBLP1_OK` / `EFISP_PE` / `GRAFT_NEEDED_LIST` / `FAKELOCK_NEEDED_LIST` / `LOGFS_HISTORY` / `LOGFS_NEWEST` are the only scalars passed between helpers; all are written by `collect_*` and read by `decide_tier` / `mode_main`. The new `list-hash` subcommand's output schema (`partition= type= declared= digest= graft= verdict=`) is consumed by `diag.sh`'s two awk filters in `check_graft`; both filters reference exactly those field names.
