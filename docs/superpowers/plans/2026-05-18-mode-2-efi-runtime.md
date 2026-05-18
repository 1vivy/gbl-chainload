# Mode-2 EFI Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the EFI-side of mode-2 — a `GBL_MODE == 2` build that reads a per-OTA profile from a GBLP1 `0x0010` overlay entry and rewrites the KeyMaster (`SET_ROT`/`SET_VERSION`/`SET_BOOT_STATE`/`SET_VBH`) and SPSS payloads in flight so the secure world latches a coherent locked/green state.

**Architecture:** Mirrors the existing mode-0/mode-1 split. A new `mode2_profile` binary struct rides in the already-reserved GBLP1 type `0x0010` entry, parsed by pure-logic code shared with host tooling. A new `Mode2Overlay` (gated `#if (GBL_MODE == 2)`) holds the profile and the rewrite policy; `QseecomHook`/`SpssHook` gain `GBL_MODE == 2` blocks that call it. ABL stays honest (no `VerifiedBoot` device-state mutation). Profile missing/invalid → honest boot + a fastboot-screen warning line.

**Tech Stack:** EDK2 / C (AArch64 UEFI), Docker build (`scripts/build.sh`), host C unit harnesses under `tests/host/`.

**Scope:** This plan covers spec slices 1–2 (`docs/superpowers/specs/2026-05-17-mode-2-design.md` §8). Slices 3–5 — the vbmeta→profile tooling + `gbl-pack` XML→`0x0010` compiler, the `images/`-drop orchestration, and the mode-2 ZIP — are deferred to follow-up plans. This plan's tests build `0x0010` entries by hand in C, so it is fully testable without the slice-3 compiler.

---

## File structure

New files:

- `tools/shared/gbl_mode2_profile.h` — the `mode2_profile` binary layout + constants, shared between EDK2 and host tools (sibling of `tools/shared/gblp1.h`).
- `GblChainloadPkg/Library/GblPayloadLib/Internal/Mode2Profile.h` — pure-logic profile-parser API.
- `GblChainloadPkg/Library/GblPayloadLib/Mode2Profile.c` — pure-logic profile parser/validator.
- `GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.h` / `Mode2Rewrite.c` — pure-logic, host-testable KM/SPSS buffer rewrite (sibling-style to `PayloadParse.c`: `GBL_HOST_BUILD`-aware).
- `GblChainloadPkg/Library/ProtocolHookLib/Mode2Overlay.h` / `Mode2Overlay.c` — EDK2 glue: profile holder + policy entrypoints, gated `#if (GBL_MODE == 2)`.
- `tests/host/helpers/mode2_harness.c` — host harness exercising the profile parser + rewrite.
- `tests/host/076_mode2_profile_parse.sh`, `tests/host/077_gblp1_find_mode2_profile.sh`, `tests/host/078_mode2_rewrite.sh` — host tests.

Modified files:

- `GblChainloadPkg/Library/GblPayloadLib/Internal/PayloadParse.h` / `PayloadParse.c` — generic entry finder + `gbl_payload_find_mode2_profile`.
- `GblChainloadPkg/Include/Library/GblPayloadLib.h` / `GblPayloadLib/GblPayload.c` / `GblPayloadLib.inf` — `GblPayload_LoadMode2Profile` public API.
- `tests/host/helpers/parser_harness.c` / `tests/host/helpers/Makefile` — `find-mode2-profile` subcommand + `mode2_harness` target.
- `scripts/build.sh`, `GblChainloadPkg/GblChainloadPkg.dsc`, `README.md`, `tests/045_mode_taxonomy_lint.sh` — `--mode 2`.
- `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c` / `SpssHook.c` / `ProtocolHookLib.inf` — `GBL_MODE == 2` rewrite blocks.
- `GblChainloadPkg/Application/GblChainload/BootFlow.c`, `GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c` — profile load + required-hook handling.
- `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`, `edk2/QcomModulePkg/Library/BootLib/FastbootMenu.c` — mode-2 profile warning line.

---

### Task 1: `mode2_profile` binary format + pure-logic parser

**Goal:** Define the `mode2_profile` binary struct and a host-testable parser that validates it and decodes its fields.

**Files:**
- Create: `tools/shared/gbl_mode2_profile.h`
- Create: `GblChainloadPkg/Library/GblPayloadLib/Internal/Mode2Profile.h`
- Create: `GblChainloadPkg/Library/GblPayloadLib/Mode2Profile.c`
- Create: `tests/host/helpers/mode2_harness.c`
- Modify: `tests/host/helpers/Makefile`
- Create: `tests/host/076_mode2_profile_parse.sh`

**Acceptance Criteria:**
- [ ] `struct gbl_mode2_profile` is exactly 120 bytes, enforced by `_Static_assert`.
- [ ] `gbl_mode2_profile_parse` rejects wrong size, bad magic, bad version, non-zero reserved, `is_unlocked > 1`, `color > 3`, and accepts a well-formed profile decoding all fields little-endian.
- [ ] `tests/host/076_mode2_profile_parse.sh` passes.

**Verify:** `bash tests/host/076_mode2_profile_parse.sh` → final line `PASS: 076 mode2 profile parse`

**Steps:**

- [ ] **Step 1: Write the shared format header**

Create `tools/shared/gbl_mode2_profile.h`:

```c
/* tools/shared/gbl_mode2_profile.h — mode2_profile binary layout (LE).
   Rides in the GBLP1 container as the type 0x0010 entry payload.
   Shared between EDK2 GblPayloadLib and host tools. */
#ifndef GBL_MODE2_PROFILE_H_
#define GBL_MODE2_PROFILE_H_

#include <stdint.h>

#define GBL_M2P_MAGIC        "GM2P"
#define GBL_M2P_MAGIC_SIZE   4u
#define GBL_M2P_VERSION      0x0001u
#define GBL_M2P_SIZE         120u

/* color field values (KMBootState.Color domain) */
#define GBL_M2P_COLOR_GREEN  0u
#define GBL_M2P_COLOR_YELLOW 1u
#define GBL_M2P_COLOR_ORANGE 2u
#define GBL_M2P_COLOR_RED    3u

/* On-disk profile — packed, little-endian. Field offsets are naturally
   aligned; `packed` is belt-and-suspenders against odd ABIs. */
struct gbl_mode2_profile {
    uint8_t  magic[4];          /* "GM2P"                         off 0  */
    uint16_t version;           /* 1                              off 4  */
    uint16_t reserved;          /* 0                              off 6  */
    uint32_t is_unlocked;       /* 0 (locked) — SET_BOOT_STATE    off 8  */
    uint32_t color;             /* 0 = GREEN — SET_BOOT_STATE     off 12 */
    uint32_t system_version;    /* bootloader-domain OS version   off 16 */
    uint32_t system_spl;        /* bootloader-domain SPL          off 20 */
    uint8_t  rot_digest[32];    /* SET_ROT RotDigest              off 24 */
    uint8_t  pubkey_digest[32]; /* SET_BOOT_STATE PublicKey       off 56 */
    uint8_t  vbh[32];           /* SET_VBH Vbh                    off 88 */
} __attribute__((packed));

_Static_assert(sizeof(struct gbl_mode2_profile) == GBL_M2P_SIZE,
               "gbl_mode2_profile must be 120 bytes packed");

#endif /* GBL_MODE2_PROFILE_H_ */
```

- [ ] **Step 2: Write the parser API header**

Create `GblChainloadPkg/Library/GblPayloadLib/Internal/Mode2Profile.h`:

```c
#ifndef GBL_MODE2_PROFILE_PARSE_H_
#define GBL_MODE2_PROFILE_PARSE_H_

#ifdef GBL_HOST_BUILD
# include <stdint.h>
# include <stddef.h>
#else
# include <Uefi.h>
# ifndef GBL_COMPAT_TYPES_DEFINED
#  define GBL_COMPAT_TYPES_DEFINED
   typedef UINT8  uint8_t;
   typedef UINT16 uint16_t;
   typedef UINT32 uint32_t;
   typedef INT32  int32_t;
# endif
# ifndef _SIZE_T
#  define _SIZE_T
   typedef __SIZE_TYPE__ size_t;
# endif
#endif

#include "../../../../tools/shared/gbl_mode2_profile.h"

enum gbl_m2p_status {
    GBL_M2P_OK = 0,
    GBL_M2P_TOO_SMALL,
    GBL_M2P_BAD_MAGIC,
    GBL_M2P_BAD_VERSION,
    GBL_M2P_BAD_RESERVED,
    GBL_M2P_BAD_FIELD       /* is_unlocked > 1 or color > 3 */
};

/* Parse and validate a mode2_profile payload. `bytes` is the 0x0010
   entry payload, `size` its byte length. On GBL_M2P_OK, *out is filled
   with host-endian field values. */
enum gbl_m2p_status
gbl_mode2_profile_parse(const uint8_t *bytes, size_t size,
                        struct gbl_mode2_profile *out);

#endif
```

- [ ] **Step 3: Write the parser implementation**

Create `GblChainloadPkg/Library/GblPayloadLib/Mode2Profile.c`:

```c
/* GblChainloadPkg/Library/GblPayloadLib/Mode2Profile.c — pure-logic
   mode2_profile parser. No EDK2 / libc dependency beyond the byte
   helpers below, so it builds host-side (GBL_HOST_BUILD) and in EDK2. */
#include "Internal/Mode2Profile.h"

static uint16_t m2p_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t m2p_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int m2p_memeq(const uint8_t *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}
static void m2p_copy(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

enum gbl_m2p_status
gbl_mode2_profile_parse(const uint8_t *b, size_t n,
                        struct gbl_mode2_profile *out) {
    if (b == NULL || out == NULL)            return GBL_M2P_BAD_FIELD;
    if (n != (size_t)GBL_M2P_SIZE)           return GBL_M2P_TOO_SMALL;
    if (!m2p_memeq(b, GBL_M2P_MAGIC, GBL_M2P_MAGIC_SIZE))
                                             return GBL_M2P_BAD_MAGIC;
    if (m2p_le16(b + 4) != GBL_M2P_VERSION)  return GBL_M2P_BAD_VERSION;
    if (m2p_le16(b + 6) != 0)                return GBL_M2P_BAD_RESERVED;

    uint32_t is_unlocked = m2p_le32(b + 8);
    uint32_t color       = m2p_le32(b + 12);
    if (is_unlocked > 1u)                    return GBL_M2P_BAD_FIELD;
    if (color > GBL_M2P_COLOR_RED)           return GBL_M2P_BAD_FIELD;

    m2p_copy(out->magic, b + 0, 4);
    out->version        = m2p_le16(b + 4);
    out->reserved       = 0;
    out->is_unlocked    = is_unlocked;
    out->color          = color;
    out->system_version = m2p_le32(b + 16);
    out->system_spl     = m2p_le32(b + 20);
    m2p_copy(out->rot_digest,    b + 24, 32);
    m2p_copy(out->pubkey_digest, b + 56, 32);
    m2p_copy(out->vbh,           b + 88, 32);
    return GBL_M2P_OK;
}
```

- [ ] **Step 4: Write the host harness**

Create `tests/host/helpers/mode2_harness.c`:

```c
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
```

- [ ] **Step 5: Add Makefile targets for the harness**

Read `tests/host/helpers/Makefile` to match its style, then add a `mode2_harness` target. It must compile `mode2_harness.c` together with the parser source, defining `GBL_HOST_BUILD`, with the include path reaching `GblChainloadPkg/Library/GblPayloadLib`. Append:

```make
MODE2_SRC = ../../../GblChainloadPkg/Library/GblPayloadLib/Mode2Profile.c

mode2_harness: mode2_harness.c $(MODE2_SRC)
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD \
	  -I../../../GblChainloadPkg/Library/GblPayloadLib \
	  -o $@ mode2_harness.c $(MODE2_SRC)
```

(Use the existing `CC`/`CFLAGS` variables from that Makefile; if it does not define them, add `CC ?= cc` and `CFLAGS ?= -std=c11 -Wall -Wextra -O1` near the top.)

- [ ] **Step 6: Write the failing test**

Create `tests/host/076_mode2_profile_parse.sh`:

```bash
#!/usr/bin/env bash
# tests/host/076_mode2_profile_parse.sh — mode2_profile parser unit test.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers mode2_harness
H=tests/host/helpers/mode2_harness
OUT=tests/host/.last/076
mkdir -p "$OUT"

# Build a well-formed 120-byte profile: magic GM2P, ver 1, reserved 0,
# is_unlocked 0, color 0, sysver 0x40000, spl 0x9A4, then 3x 32B digests.
python3 - "$OUT/good.bin" <<'PY'
import struct, sys
p  = b"GM2P" + struct.pack("<HHIIII", 1, 0, 0, 0, 0x40000, 0x9A4)
p += bytes(range(32)) + bytes(range(32,64)) + bytes(range(64,96))
assert len(p) == 120, len(p)
open(sys.argv[1], "wb").write(p)
PY

# good -> status=0
"$H" profile-parse "$OUT/good.bin" | grep -q 'status=0' \
  || { echo "FAIL: well-formed profile rejected"; exit 1; }

# bad magic -> non-zero
python3 - "$OUT/badmagic.bin" "$OUT/good.bin" <<'PY'
import sys
b = bytearray(open(sys.argv[2],"rb").read()); b[0]=ord('X')
open(sys.argv[1],"wb").write(b)
PY
"$H" profile-parse "$OUT/badmagic.bin" | grep -q 'status=0' \
  && { echo "FAIL: bad magic accepted"; exit 1; } || true

# wrong size -> non-zero
head -c 119 "$OUT/good.bin" > "$OUT/short.bin"
"$H" profile-parse "$OUT/short.bin" | grep -q 'status=0' \
  && { echo "FAIL: short profile accepted"; exit 1; } || true

# color out of range (color=9 at offset 12) -> non-zero
python3 - "$OUT/badcolor.bin" "$OUT/good.bin" <<'PY'
import sys, struct
b = bytearray(open(sys.argv[2],"rb").read())
b[12:16] = struct.pack("<I", 9)
open(sys.argv[1],"wb").write(b)
PY
"$H" profile-parse "$OUT/badcolor.bin" | grep -q 'status=0' \
  && { echo "FAIL: bad color accepted"; exit 1; } || true

echo "PASS: 076 mode2 profile parse"
```

- [ ] **Step 7: Run the test, confirm it fails then passes**

Run: `bash tests/host/076_mode2_profile_parse.sh`
Expected before Steps 1–6 are complete: FAIL (missing harness/sources). After: `PASS: 076 mode2 profile parse`.

- [ ] **Step 8: Commit**

```bash
git add tools/shared/gbl_mode2_profile.h \
  GblChainloadPkg/Library/GblPayloadLib/Internal/Mode2Profile.h \
  GblChainloadPkg/Library/GblPayloadLib/Mode2Profile.c \
  tests/host/helpers/mode2_harness.c tests/host/helpers/Makefile \
  tests/host/076_mode2_profile_parse.sh
git commit -m "feat(mode-2): mode2_profile binary format + parser"
```

---

### Task 2: GBLP1 parser — locate the `0x0010` entry

**Goal:** Extend the GBLP1 parser so it can return the `mode2_profile` (type `0x0010`) entry payload, reusing the existing entry-walk integrity checks.

**Files:**
- Modify: `GblChainloadPkg/Library/GblPayloadLib/Internal/PayloadParse.h`
- Modify: `GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c`
- Modify: `tests/host/helpers/parser_harness.c`
- Create: `tests/host/077_gblp1_find_mode2_profile.sh`

**Acceptance Criteria:**
- [ ] `gbl_payload_find_mode2_profile` returns `GBL_PAYLOAD_OK` and the payload slice for a container holding a `0x0010` entry, `GBL_PAYLOAD_NO_MODE2_PROFILE` when absent, and the existing per-entry integrity errors (SHA mismatch, bad offset) on a corrupt container.
- [ ] `gbl_payload_find_cached_abl` behavior is unchanged (existing tests 060–069 still pass).
- [ ] `tests/host/077_gblp1_find_mode2_profile.sh` passes.

**Verify:** `bash tests/host/077_gblp1_find_mode2_profile.sh` → `PASS: 077 gblp1 find mode2_profile`

**Steps:**

- [ ] **Step 1: Add the new status code and API to `PayloadParse.h`**

In `Internal/PayloadParse.h`, add `GBL_PAYLOAD_NO_MODE2_PROFILE` to `enum gbl_payload_status` (append after `GBL_PAYLOAD_NO_CACHED_ABL`), and declare the new function after `gbl_payload_find_cached_abl`:

```c
/* Validates header + walks every entry exactly as
   gbl_payload_find_cached_abl, then locates the unique
   GBLP1_TYPE_MODE2_PROFILE (0x0010) entry. On GBL_PAYLOAD_OK,
   *out_profile points into `bytes` and *out_size is its length.
   Returns GBL_PAYLOAD_NO_MODE2_PROFILE if no 0x0010 entry exists. */
enum gbl_payload_status
gbl_payload_find_mode2_profile(const uint8_t *bytes, size_t size,
                               const uint8_t **out_profile,
                               size_t *out_size);
```

- [ ] **Step 2: Refactor the entry walk into a shared finder in `PayloadParse.c`**

`gbl_payload_find_cached_abl` already walks and integrity-checks every entry. Extract that walk into a static helper that takes a target type, then make both public finders thin wrappers. Replace the body of `gbl_payload_find_cached_abl` (lines ~51–110) with:

```c
/* Walk + integrity-check every entry; return the unique entry whose
   type == want_type. Returns GBL_PAYLOAD_OK with *out/*out_size set,
   or an integrity error, or GBL_PAYLOAD_OK with *out == NULL when no
   entry of want_type exists (caller maps that to its own "missing"). */
static enum gbl_payload_status
gbl_payload_find_entry(const uint8_t *b, size_t n, uint16_t want_type,
                       const uint8_t **out, size_t *out_size) {
    enum gbl_payload_status s = gbl_payload_validate_header(b, n);
    if (s != GBL_PAYLOAD_OK) return s;

    uint32_t total = le32(b + 16);
    uint32_t ec = le32(b + 20);
    const uint8_t *entries = b + GBLP1_HEADER_SIZE;
    size_t payload_region_start =
        GBLP1_HEADER_SIZE + (size_t)ec * GBLP1_ENTRY_SIZE;
    payload_region_start = (payload_region_start + GBLP1_PAYLOAD_ALIGN - 1)
                           & ~((size_t)GBLP1_PAYLOAD_ALIGN - 1);

    int found = 0;
    const uint8_t *found_pe = NULL;
    size_t found_size = 0;

    for (uint32_t i = 0; i < ec; i++) {
        const uint8_t *e = entries + (size_t)i * GBLP1_ENTRY_SIZE;
        uint16_t type     = le16(e + 0);
        uint16_t flags    = le16(e + 2);
        uint32_t off      = le32(e + 4);
        uint32_t sz       = le32(e + 8);
        uint32_t reserved = le32(e + 12);
        const uint8_t *recorded_sha = e + 16;

        if (type == 0)     return GBL_PAYLOAD_ENTRY_BAD_TYPE;
        if (flags != 0)    return GBL_PAYLOAD_ENTRY_BAD_FLAGS;
        if (reserved != 0) return GBL_PAYLOAD_ENTRY_BAD_RESERVED;
        if (off < payload_region_start ||
            (off & (GBLP1_PAYLOAD_ALIGN - 1)) != 0)
            return GBL_PAYLOAD_ENTRY_BAD_OFFSET;
        if ((size_t)off + sz + GBLP1_FOOTER_SIZE > (size_t)total)
            return GBL_PAYLOAD_ENTRY_BAD_SIZE;

        uint8_t got[32];
        gbl_sha256(b + off, sz, got);
        if (memcmp(got, recorded_sha, 32) != 0)
            return GBL_PAYLOAD_ENTRY_SHA_MISMATCH;

        if (type == want_type) {
            if (found) return GBL_PAYLOAD_ENTRY_BAD_TYPE; /* duplicate */
            found = 1;
            found_pe = b + off;
            found_size = sz;
        }
    }

    *out      = found ? found_pe : NULL;
    *out_size = found_size;
    return GBL_PAYLOAD_OK;
}

enum gbl_payload_status
gbl_payload_find_cached_abl(const uint8_t *b, size_t n,
                            const uint8_t **out_pe, size_t *out_size) {
    const uint8_t *pe = NULL; size_t sz = 0;
    enum gbl_payload_status s =
        gbl_payload_find_entry(b, n, GBLP1_TYPE_CACHED_ABL, &pe, &sz);
    if (s != GBL_PAYLOAD_OK) return s;
    if (pe == NULL) return GBL_PAYLOAD_NO_CACHED_ABL;
    *out_pe = pe; *out_size = sz;
    return GBL_PAYLOAD_OK;
}

enum gbl_payload_status
gbl_payload_find_mode2_profile(const uint8_t *b, size_t n,
                               const uint8_t **out_profile,
                               size_t *out_size) {
    const uint8_t *p = NULL; size_t sz = 0;
    enum gbl_payload_status s =
        gbl_payload_find_entry(b, n, GBLP1_TYPE_MODE2_PROFILE, &p, &sz);
    if (s != GBL_PAYLOAD_OK) return s;
    if (p == NULL) return GBL_PAYLOAD_NO_MODE2_PROFILE;
    *out_profile = p; *out_size = sz;
    return GBL_PAYLOAD_OK;
}
```

Keep the `#include "Internal/Sha256.h"` line that currently sits above `gbl_payload_find_cached_abl`. `gbl_payload_scan_cached_abl` below is unchanged.

- [ ] **Step 3: Add a `find-mode2-profile` subcommand to `parser_harness.c`**

Read `tests/host/helpers/parser_harness.c`. It dispatches on `argv[1]` (`parse-header`, `find-cached-abl`). Add a branch mirroring `find-cached-abl` that calls `gbl_payload_find_mode2_profile` and prints `status=<n>` and, on status 0, `size=<n>`.

- [ ] **Step 4: Write the test**

Create `tests/host/077_gblp1_find_mode2_profile.sh`:

```bash
#!/usr/bin/env bash
# tests/host/077_gblp1_find_mode2_profile.sh — locate a 0x0010 entry.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers parser_harness
H=tests/host/helpers/parser_harness
OUT=tests/host/.last/077
mkdir -p "$OUT"

# Build a GBLP1 container with one 0x0010 (mode2_profile) entry: a
# 120-byte profile payload. Mirrors the on-disk layout in
# tools/shared/gblp1.h (header 28, entry 48, payload 16-aligned, footer 8).
python3 - "$OUT/with_profile.bin" "$OUT/no_profile.bin" <<'PY'
import struct, sys, zlib, hashlib

def container(entry_type):
    profile = b"GM2P" + struct.pack("<HHIIII",1,0,0,0,0x40000,0x9A4)
    profile += bytes(96)
    assert len(profile) == 120
    hdr_size, ent_size, ftr = 28, 48, b"GBLP1END"
    pay_off = (hdr_size + ent_size + 15) & ~15
    total = pay_off + len(profile)
    total = (total + 15) & ~15
    total += len(ftr)
    buf = bytearray(total)
    buf[pay_off:pay_off+len(profile)] = profile
    buf[total-8:total] = ftr
    # entry
    ent = struct.pack("<HHIII", entry_type, 0, pay_off, len(profile), 0)
    ent += hashlib.sha256(profile).digest()
    buf[hdr_size:hdr_size+ent_size] = ent
    # header: magic,ver,hdrsize,flags,total,entry_count, then crc32[0..24)
    head = b"GBLP1\0\0\0" + struct.pack("<HHIII",1,28,1,total,1)
    buf[0:24] = head
    buf[24:28] = struct.pack("<I", zlib.crc32(bytes(buf[0:24])) & 0xffffffff)
    return bytes(buf)

open(sys.argv[1],"wb").write(container(0x0010))  # has mode2_profile
open(sys.argv[2],"wb").write(container(0x0001))  # cached_abl only
PY

# container WITH a 0x0010 entry -> status=0
"$H" find-mode2-profile "$OUT/with_profile.bin" | grep -q 'status=0' \
  || { echo "FAIL: 0x0010 entry not found"; exit 1; }

# container WITHOUT one -> non-zero (GBL_PAYLOAD_NO_MODE2_PROFILE)
"$H" find-mode2-profile "$OUT/no_profile.bin" | grep -q 'status=0' \
  && { echo "FAIL: missing 0x0010 reported as found"; exit 1; } || true

echo "PASS: 077 gblp1 find mode2_profile"
```

- [ ] **Step 5: Run the test + regression-check cached_abl**

Run: `bash tests/host/077_gblp1_find_mode2_profile.sh` → expect `PASS: 077 ...`
Run: `bash tests/host/063_pe_sanity.sh` and `bash tests/host/061_parser_fuzz.sh` → expect their existing PASS lines (confirms the refactor did not regress `find_cached_abl`).

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/Library/GblPayloadLib/Internal/PayloadParse.h \
  GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c \
  tests/host/helpers/parser_harness.c \
  tests/host/077_gblp1_find_mode2_profile.sh
git commit -m "feat(mode-2): locate GBLP1 0x0010 mode2_profile entry"
```

---

### Task 3: `GblPayloadLib` EDK2 reader — `GblPayload_LoadMode2Profile`

**Goal:** Expose a public EDK2 API that locates the GBLP1 overlay, finds the `0x0010` entry, parses it, and returns a validated `struct gbl_mode2_profile`.

**Files:**
- Modify: `GblChainloadPkg/Include/Library/GblPayloadLib.h`
- Modify: `GblChainloadPkg/Library/GblPayloadLib/GblPayload.c`
- Modify: `GblChainloadPkg/Library/GblPayloadLib/GblPayloadLib.inf`

**Acceptance Criteria:**
- [ ] `GblPayload_LoadMode2Profile` returns `EFI_SUCCESS` + a filled profile when a valid `0x0010` entry exists, `EFI_NOT_FOUND` when the overlay or entry is absent, and `EFI_LOAD_ERROR` when an entry exists but fails GBLP1 or profile validation.
- [ ] `dist/mode-1.efi` still builds (the new source compiles into the lib without disturbing existing modes).

**Verify:** `./scripts/build.sh --mode 1` → `==> Built dist/mode-1.efi (<N> bytes)`

**Steps:**

- [ ] **Step 1: Declare the API**

In `GblChainloadPkg/Include/Library/GblPayloadLib.h`, add before `#endif`:

```c
#include "../../../tools/shared/gbl_mode2_profile.h"

/* Locate the GBLP1 overlay, find the mode2_profile (0x0010) entry, and
   parse it. Returns:
     EFI_SUCCESS    — *Profile filled with a validated profile
     EFI_NOT_FOUND  — no overlay, or no 0x0010 entry in the overlay
     EFI_LOAD_ERROR — overlay/entry present but failed validation */
EFI_STATUS EFIAPI
GblPayload_LoadMode2Profile (IN  EFI_HANDLE                ImageHandle,
                             OUT struct gbl_mode2_profile *Profile);
```

- [ ] **Step 2: Implement it in `GblPayload.c`**

Add `#include "Internal/Mode2Profile.h"` to the includes, then append:

```c
EFI_STATUS EFIAPI
GblPayload_LoadMode2Profile (IN  EFI_HANDLE                ImageHandle,
                             OUT struct gbl_mode2_profile *Profile) {
  VOID *Bytes = NULL; UINTN Size = 0;
  if (Profile == NULL) return EFI_INVALID_PARAMETER;

  EFI_STATUS Status = LocateOverlayBytes(&Bytes, &Size);
  if (EFI_ERROR(Status)) {
    GBL_INFO("gbl-payload: mode2 — no overlay bytes (%r)\n", Status);
    return EFI_NOT_FOUND;
  }

  /* Scan for the GBLP1 magic, tolerating stray copies, then locate the
     0x0010 entry within the first fully-valid container. */
  CONST UINT8 *B = (CONST UINT8 *)Bytes;
  enum gbl_payload_status PS = GBL_PAYLOAD_BAD_MAGIC;
  CONST UINT8 *ProfBytes = NULL; size_t ProfSize = 0;
  for (UINTN i = 0; i + GBLP1_MAGIC_SIZE <= Size; i++) {
    if (CompareMem(B + i, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) != 0) continue;
    PS = gbl_payload_find_mode2_profile(B + i, Size - i,
                                        &ProfBytes, &ProfSize);
    if (PS == GBL_PAYLOAD_OK || PS == GBL_PAYLOAD_NO_MODE2_PROFILE) break;
  }

  if (PS == GBL_PAYLOAD_NO_MODE2_PROFILE || PS == GBL_PAYLOAD_BAD_MAGIC) {
    GBL_INFO("gbl-payload: mode2 — no 0x0010 entry\n");
    return EFI_NOT_FOUND;
  }
  if (PS != GBL_PAYLOAD_OK) {
    GBL_INFO("gbl-payload: mode2 — container invalid (status=%d)\n", (int)PS);
    return EFI_LOAD_ERROR;
  }

  enum gbl_m2p_status MS =
      gbl_mode2_profile_parse(ProfBytes, ProfSize, Profile);
  if (MS != GBL_M2P_OK) {
    GBL_INFO("gbl-payload: mode2 — profile invalid (status=%d)\n", (int)MS);
    return EFI_LOAD_ERROR;
  }
  GBL_INFO("gbl-payload: mode2 — profile loaded (ver=%u color=%u)\n",
           Profile->version, Profile->color);
  return EFI_SUCCESS;
}
```

- [ ] **Step 3: Add `Mode2Profile.c` to the lib INF**

In `GblChainloadPkg/Library/GblPayloadLib/GblPayloadLib.inf`, under `[Sources]`, add `Mode2Profile.c` after `PayloadParse.c`.

- [ ] **Step 4: Build to verify the lib compiles**

Run: `./scripts/build.sh --mode 1`
Expected: `==> Built dist/mode-1.efi (<N> bytes)` and exit 0.

- [ ] **Step 5: Commit**

```bash
git add GblChainloadPkg/Include/Library/GblPayloadLib.h \
  GblChainloadPkg/Library/GblPayloadLib/GblPayload.c \
  GblChainloadPkg/Library/GblPayloadLib/GblPayloadLib.inf
git commit -m "feat(mode-2): GblPayload_LoadMode2Profile EDK2 reader"
```

---

### Task 4: `--mode 2` build wiring

**Goal:** Make `scripts/build.sh --mode 2` a valid build that produces `dist/mode-2.efi`, and update mode taxonomy in docs + lint.

**Files:**
- Modify: `scripts/build.sh`
- Modify: `GblChainloadPkg/GblChainloadPkg.dsc`
- Modify: `README.md`
- Modify: `tests/045_mode_taxonomy_lint.sh`

**Acceptance Criteria:**
- [ ] `./scripts/build.sh --mode 2` builds and produces `dist/mode-2.efi`.
- [ ] `./scripts/build.sh --mode 3` still rejects with a clear error.
- [ ] `tests/045_mode_taxonomy_lint.sh` passes.

**Verify:** `./scripts/build.sh --mode 2` → `==> Built dist/mode-2.efi (<N> bytes)`

**Steps:**

- [ ] **Step 1: Accept mode 2 in `build.sh`**

In `scripts/build.sh`, change the mode-validation `case` from `0|1)` to `0|1|2)`, and update the two usage strings (`--mode {0|1}` → `--mode {0|1|2}`) and the help text to add `Mode 2: TA-payload spoof at QSEE/SPSS boundaries (custom-ROM mode); ABL stays honest.`

- [ ] **Step 2: Confirm the `.dsc` needs no per-mode change**

`GblChainloadPkg.dsc` already passes `GBL_MODE` through unconditionally (`DEFINE GBL_MODE = 1`, `-DGBL_MODE=$(GBL_MODE)`). No change needed beyond confirming `GBL_MODE=2` flows from the `-e GBL_MODE` env var. No edit unless a hard-coded `0|1` check exists — grep `GBL_MODE` in the `.dsc` and remove any such guard.

- [ ] **Step 3: Update `README.md`**

In `README.md`, change the mode-2 bullet from `*(not yet implemented)*` to a description matching the spec, and add `./scripts/build.sh --mode 2` to the Build block.

- [ ] **Step 4: Extend the mode taxonomy lint**

`tests/045_mode_taxonomy_lint.sh` asserts the patch-scope taxonomy. Add an assertion that `build.sh` accepts mode 2:

```bash
# 6. build.sh accepts --mode 2.
grep -q '0|1|2)' scripts/build.sh \
  || { echo "FAIL: build.sh must accept --mode 2"; exit 1; }
```

- [ ] **Step 5: Verify**

Run: `./scripts/build.sh --mode 2` → expect `==> Built dist/mode-2.efi`.
Run: `./scripts/build.sh --mode 3` → expect non-zero exit with `--mode 3 not yet supported` (text may now read `valid: 0, 1, 2`).
Run: `bash tests/045_mode_taxonomy_lint.sh` → expect its PASS output.

- [ ] **Step 6: Commit**

```bash
git add scripts/build.sh GblChainloadPkg/GblChainloadPkg.dsc \
  README.md tests/045_mode_taxonomy_lint.sh
git commit -m "feat(mode-2): --mode 2 build wiring"
```

---

### Task 5: Mode-2 rewrite logic (pure, host-testable)

**Goal:** Implement the pure-logic KM/SPSS buffer rewrites — given a profile and a send buffer, overwrite the relevant fields — in a host-testable source file.

**Files:**
- Create: `GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.h`
- Create: `GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.c`
- Modify: `tests/host/helpers/mode2_harness.c`
- Modify: `tests/host/helpers/Makefile`
- Create: `tests/host/078_mode2_rewrite.sh`

**Acceptance Criteria:**
- [ ] `gbl_m2_rewrite_km` rewrites only the four target cmd-ids (`0x201`/`0x207`/`0x208`/`0x211`), only when `SendLen` matches the expected wire size, returns 1 when it rewrote and 0 otherwise, and never writes out of bounds.
- [ ] After a `0x208` rewrite the buffer's `IsUnlocked`/`PublicKey`/`Color`/`SystemVersion`/`SystemSecurityLevel` fields equal the profile's values.
- [ ] `tests/host/078_mode2_rewrite.sh` passes.

**Verify:** `bash tests/host/078_mode2_rewrite.sh` → `PASS: 078 mode2 rewrite`

**Steps:**

- [ ] **Step 1: Write `Mode2Rewrite.h`**

```c
/* GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.h — pure-logic
   KM/SPSS buffer rewrite. No EDK2 dependency; host-testable. */
#ifndef GBL_MODE2_REWRITE_H_
#define GBL_MODE2_REWRITE_H_

#ifdef GBL_HOST_BUILD
# include <stdint.h>
# include <stddef.h>
#else
# include <Uefi.h>
# ifndef GBL_COMPAT_TYPES_DEFINED
#  define GBL_COMPAT_TYPES_DEFINED
   typedef UINT8  uint8_t;
   typedef UINT16 uint16_t;
   typedef UINT32 uint32_t;
   typedef INT32  int32_t;
# endif
# ifndef _SIZE_T
#  define _SIZE_T
   typedef __SIZE_TYPE__ size_t;
# endif
#endif

#include "../../../tools/shared/gbl_mode2_profile.h"

/* KeyMaster wire sizes (KEYMASTER_UTILS_CMD_ID = 0x200 + N). */
#define GBL_KM_CMD_SET_ROT          0x00000201u
#define GBL_KM_CMD_SET_VERSION      0x00000207u
#define GBL_KM_CMD_SET_BOOT_STATE   0x00000208u
#define GBL_KM_CMD_SET_VBH          0x00000211u
#define GBL_KM_LEN_SET_ROT          44u
#define GBL_KM_LEN_SET_VERSION      12u
#define GBL_KM_LEN_SET_BOOT_STATE   64u
#define GBL_KM_LEN_SET_VBH          36u

/* Rewrite a KM send buffer in place from `p`. `cmd_id` is the leading
   u32 of the buffer; `buf`/`len` the send buffer. Rewrites only the
   four spoof-target cmd-ids and only when `len` matches the wire size.
   Returns 1 if a rewrite happened, 0 otherwise. Safe on NULL/short buf. */
int gbl_m2_rewrite_km(uint32_t cmd_id, uint8_t *buf, uint32_t len,
                      const struct gbl_mode2_profile *p);

/* SPSS ShareKeyMintInfo carries a packed
   { KmSetRotReqWire(44), KmSetBootStateReqWire(64), KmSetVbhReqWire(36) }.
   Rewrite all three sub-structs in place. `info`/`info_len` is the whole
   packed struct (>= 144 bytes). Returns 1 if rewritten, 0 otherwise. */
#define GBL_SPSS_INFO_LEN  144u
int gbl_m2_rewrite_spss(uint8_t *info, uint32_t info_len,
                        const struct gbl_mode2_profile *p);

#endif
```

- [ ] **Step 2: Write `Mode2Rewrite.c`**

```c
/* GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.c — pure-logic
   KM/SPSS rewrite. Field offsets cross-checked against
   ~/gbl_root_canoe/tools/keymaster_wire.h and QseecomHook.c's decoder. */
#include "Mode2Rewrite.h"

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wrbytes(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

/* SET_ROT (44B):  {cmd@0, RotOffset@4, RotSize@8, RotDigest[32]@12} */
static void rewrite_set_rot(uint8_t *b, const struct gbl_mode2_profile *p) {
    wrbytes(b + 12, p->rot_digest, 32);
}
/* SET_VERSION (12B): {cmd@0, OsVersion@4, OsPatchLevel@8} */
static void rewrite_set_version(uint8_t *b, const struct gbl_mode2_profile *p) {
    wr32(b + 4, p->system_version);
    wr32(b + 8, p->system_spl);
}
/* SET_BOOT_STATE (64B): {cmd@0, Version@4, Offset@8, Size@12,
     BootState{IsUnlocked@16, PublicKey[32]@20, Color@52,
               SystemVersion@56, SystemSecurityLevel@60}} */
static void rewrite_set_boot_state(uint8_t *b,
                                   const struct gbl_mode2_profile *p) {
    wr32(b + 16, p->is_unlocked);
    wrbytes(b + 20, p->pubkey_digest, 32);
    wr32(b + 52, p->color);
    wr32(b + 56, p->system_version);
    wr32(b + 60, p->system_spl);
}
/* SET_VBH (36B): {cmd@0, Vbh[32]@4} */
static void rewrite_set_vbh(uint8_t *b, const struct gbl_mode2_profile *p) {
    wrbytes(b + 4, p->vbh, 32);
}

int gbl_m2_rewrite_km(uint32_t cmd_id, uint8_t *buf, uint32_t len,
                      const struct gbl_mode2_profile *p) {
    if (buf == NULL || p == NULL) return 0;
    switch (cmd_id) {
        case GBL_KM_CMD_SET_ROT:
            if (len != GBL_KM_LEN_SET_ROT) return 0;
            rewrite_set_rot(buf, p); return 1;
        case GBL_KM_CMD_SET_VERSION:
            if (len != GBL_KM_LEN_SET_VERSION) return 0;
            rewrite_set_version(buf, p); return 1;
        case GBL_KM_CMD_SET_BOOT_STATE:
            if (len != GBL_KM_LEN_SET_BOOT_STATE) return 0;
            rewrite_set_boot_state(buf, p); return 1;
        case GBL_KM_CMD_SET_VBH:
            if (len != GBL_KM_LEN_SET_VBH) return 0;
            rewrite_set_vbh(buf, p); return 1;
        default:
            return 0;
    }
}

int gbl_m2_rewrite_spss(uint8_t *info, uint32_t info_len,
                        const struct gbl_mode2_profile *p) {
    if (info == NULL || p == NULL || info_len < GBL_SPSS_INFO_LEN) return 0;
    rewrite_set_rot(info + 0, p);          /* RoT sub-struct  @0  (44) */
    rewrite_set_boot_state(info + 44, p);  /* BootState       @44 (64) */
    rewrite_set_vbh(info + 108, p);        /* Vbh             @108(36) */
    return 1;
}
```

- [ ] **Step 3: Extend `mode2_harness.c` with the `rewrite` subcommand**

In `tests/host/helpers/mode2_harness.c`, add `#include "Mode2Rewrite.h"` and a branch:

```c
    if (argc >= 5 && strcmp(argv[1], "rewrite") == 0) {
        uint32_t cmd = (uint32_t)strtoul(argv[2], NULL, 16);
        size_t pn, bn;
        unsigned char *pb = slurp(argv[3], &pn);
        unsigned char *bb = slurp(argv[4], &bn);
        struct gbl_mode2_profile prof;
        if (gbl_mode2_profile_parse(pb, pn, &prof) != GBL_M2P_OK) {
            printf("rewrote=0\n"); return 0;
        }
        int r = gbl_m2_rewrite_km(cmd, bb, (uint32_t)bn, &prof);
        printf("rewrote=%d\n", r);
        for (size_t i = 0; i < bn; i++) printf("%02x", bb[i]);
        printf("\n");
        return 0;
    }
```

- [ ] **Step 4: Update the harness Makefile target**

Update the `mode2_harness` target so it also compiles `Mode2Rewrite.c` and reaches the ProtocolHookLib include path:

```make
MODE2_REWRITE_SRC = ../../../GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.c

mode2_harness: mode2_harness.c $(MODE2_SRC) $(MODE2_REWRITE_SRC)
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD \
	  -I../../../GblChainloadPkg/Library/GblPayloadLib \
	  -I../../../GblChainloadPkg/Library/ProtocolHookLib \
	  -o $@ mode2_harness.c $(MODE2_SRC) $(MODE2_REWRITE_SRC)
```

- [ ] **Step 5: Write the test**

Create `tests/host/078_mode2_rewrite.sh`:

```bash
#!/usr/bin/env bash
# tests/host/078_mode2_rewrite.sh — mode-2 KM rewrite unit test.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers mode2_harness
H=tests/host/helpers/mode2_harness
OUT=tests/host/.last/078
mkdir -p "$OUT"

# Profile: is_unlocked=0, color=0, sysver=0x40000, spl=0x9A4,
# rot_digest=0x11*32, pubkey_digest=0x22*32, vbh=0x33*32.
python3 - "$OUT/profile.bin" <<'PY'
import struct, sys
p  = b"GM2P" + struct.pack("<HHIIII",1,0,0,0,0x40000,0x9A4)
p += b"\x11"*32 + b"\x22"*32 + b"\x33"*32
open(sys.argv[1],"wb").write(p)
PY

# SET_BOOT_STATE buffer (64B): cmd=0x208, rest = honest/unlocked junk.
python3 - "$OUT/bs.bin" <<'PY'
import struct, sys
b  = struct.pack("<IIII", 0x208, 0, 16, 48)        # cmd,Version,Offset,Size
b += struct.pack("<I", 1)                          # IsUnlocked = 1 (honest)
b += b"\xAA"*32                                    # PublicKey (custom)
b += struct.pack("<III", 2, 0, 0)                  # Color=ORANGE,sysver,spl
assert len(b) == 64, len(b)
open(sys.argv[1],"wb").write(b)
PY

OUT_HEX=$("$H" rewrite 0x208 "$OUT/profile.bin" "$OUT/bs.bin")
echo "$OUT_HEX" | grep -q 'rewrote=1' \
  || { echo "FAIL: SET_BOOT_STATE not rewritten"; echo "$OUT_HEX"; exit 1; }

# Last line is the rewritten buffer hex. Verify the spoofed fields:
HEX=$(echo "$OUT_HEX" | tail -1)
# IsUnlocked @16 (bytes 32..39 of hex) must now be 00000000
[ "${HEX:32:8}" = "00000000" ] \
  || { echo "FAIL: IsUnlocked not zeroed (${HEX:32:8})"; exit 1; }
# PublicKey @20 (hex 40..103) must be 0x22 * 32
[ "${HEX:40:64}" = "$(printf '22%.0s' {1..32})" ] \
  || { echo "FAIL: PublicKey not rewritten"; exit 1; }
# Color @52 (hex 104..111) must be 00000000 (GREEN)
[ "${HEX:104:8}" = "00000000" ] \
  || { echo "FAIL: Color not GREEN (${HEX:104:8})"; exit 1; }

# Wrong length must be rejected: a 63-byte SET_BOOT_STATE -> rewrote=0.
head -c 63 "$OUT/bs.bin" > "$OUT/bs_short.bin"
"$H" rewrite 0x208 "$OUT/profile.bin" "$OUT/bs_short.bin" | grep -q 'rewrote=0' \
  || { echo "FAIL: short SET_BOOT_STATE was rewritten"; exit 1; }

# Non-target cmd-id (0x219) must never be rewritten.
"$H" rewrite 0x219 "$OUT/profile.bin" "$OUT/bs.bin" | grep -q 'rewrote=0' \
  || { echo "FAIL: 0x219 was rewritten"; exit 1; }

echo "PASS: 078 mode2 rewrite"
```

- [ ] **Step 6: Run the test**

Run: `bash tests/host/078_mode2_rewrite.sh` → expect `PASS: 078 mode2 rewrite`.

- [ ] **Step 7: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.h \
  GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.c \
  tests/host/helpers/mode2_harness.c tests/host/helpers/Makefile \
  tests/host/078_mode2_rewrite.sh
git commit -m "feat(mode-2): pure-logic KM/SPSS rewrite"
```

---

### Task 6: `Mode2Overlay` — EDK2 glue

**Goal:** Add the `GBL_MODE == 2` overlay module: a process-global validated-profile holder and the policy entrypoints that `QseecomHook`/`SpssHook` will call.

**Files:**
- Create: `GblChainloadPkg/Library/ProtocolHookLib/Mode2Overlay.h`
- Create: `GblChainloadPkg/Library/ProtocolHookLib/Mode2Overlay.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf`

**Acceptance Criteria:**
- [ ] `Mode2Overlay.{c,h}` compile only under `GBL_MODE == 2` (whole body inside the guard, like `Mode1Overlay`).
- [ ] `Mode2_SetProfile` stores a copy of the profile and flips `Mode2_HasProfile()` to `TRUE`; before it is called, policy functions are no-ops.
- [ ] `dist/mode-2.efi` builds with the new sources in the INF.

**Verify:** `./scripts/build.sh --mode 2` → `==> Built dist/mode-2.efi`

**Steps:**

- [ ] **Step 1: Write `Mode2Overlay.h`**

```c
/** @file Mode2Overlay.h — mode-2-scope hook policy declarations.
    Only active when GBL_MODE == 2. **/
#ifndef MODE2_OVERLAY_H_
#define MODE2_OVERLAY_H_

#include <Uefi.h>

#if (GBL_MODE == 2)

#include "../../../tools/shared/gbl_mode2_profile.h"

/* Store a validated profile. Copies *Profile into module state and
   makes Mode2_HasProfile() return TRUE. Called once by BootFlow. */
VOID EFIAPI Mode2_SetProfile (IN CONST struct gbl_mode2_profile *Profile);

/* TRUE once Mode2_SetProfile has been called with a valid profile. */
BOOLEAN EFIAPI Mode2_HasProfile (VOID);

/* QseecomSendCmd policy: rewrite a KM send buffer in place from the
   stored profile. No-op (returns FALSE) if no profile is stored or the
   cmd-id is not a spoof target. Emits a GBL_INFO line on a rewrite. */
BOOLEAN EFIAPI
Mode2Policy_RewriteKmSend (IN     UINT32  CmdId,
                           IN OUT UINT8  *SendBuf,
                           IN     UINT32  SendLen);

/* SPSS ShareKeyMintInfo policy: rewrite the packed RoT/BootState/Vbh
   struct in place from the stored profile. No-op if no profile. */
BOOLEAN EFIAPI
Mode2Policy_RewriteSpss (IN OUT VOID   *Info,
                         IN     UINT32  InfoLen);

#endif /* GBL_MODE == 2 */
#endif /* MODE2_OVERLAY_H_ */
```

- [ ] **Step 2: Write `Mode2Overlay.c`**

```c
/** @file Mode2Overlay.c — mode-2-scope hook policy implementation.
    Holds the validated profile and applies the QSEE/SPSS rewrites.
    Compiled out entirely in non-mode-2 builds via the GBL_MODE guard. **/
#include "Mode2Overlay.h"

#if (GBL_MODE == 2)

#include <Library/BaseMemoryLib.h>
#include <Library/GblLog.h>
#include "Mode2Rewrite.h"

STATIC struct gbl_mode2_profile  gMode2Profile;
STATIC BOOLEAN                   gMode2HasProfile = FALSE;

VOID EFIAPI
Mode2_SetProfile (IN CONST struct gbl_mode2_profile *Profile) {
  if (Profile == NULL) return;
  CopyMem (&gMode2Profile, Profile, sizeof (gMode2Profile));
  gMode2HasProfile = TRUE;
  GBL_INFO ("mode2 | profile set (ver=%u color=%u isUnlocked=%u)\n",
            Profile->version, Profile->color, Profile->is_unlocked);
}

BOOLEAN EFIAPI
Mode2_HasProfile (VOID) {
  return gMode2HasProfile;
}

BOOLEAN EFIAPI
Mode2Policy_RewriteKmSend (IN UINT32 CmdId, IN OUT UINT8 *SendBuf,
                           IN UINT32 SendLen) {
  if (!gMode2HasProfile) return FALSE;
  int Rewrote = gbl_m2_rewrite_km (CmdId, SendBuf, SendLen, &gMode2Profile);
  if (Rewrote) {
    GBL_INFO ("mode2 | km-rewrite | cmd=0x%08x | len=%u\n", CmdId, SendLen);
  }
  return Rewrote ? TRUE : FALSE;
}

BOOLEAN EFIAPI
Mode2Policy_RewriteSpss (IN OUT VOID *Info, IN UINT32 InfoLen) {
  if (!gMode2HasProfile || Info == NULL) return FALSE;
  int Rewrote = gbl_m2_rewrite_spss ((UINT8 *)Info, InfoLen, &gMode2Profile);
  if (Rewrote) {
    GBL_INFO ("mode2 | spss-rewrite | len=%u\n", InfoLen);
  }
  return Rewrote ? TRUE : FALSE;
}

#endif /* GBL_MODE == 2 */
```

- [ ] **Step 3: Add the sources to `ProtocolHookLib.inf`**

In `GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf`, under `[Sources]`, add `Mode2Overlay.c` and `Mode2Rewrite.c` after `Mode1Overlay.c`.

- [ ] **Step 4: Build mode-2 and mode-1 to confirm guards**

Run: `./scripts/build.sh --mode 2` → expect `==> Built dist/mode-2.efi`.
Run: `./scripts/build.sh --mode 1` → expect `==> Built dist/mode-1.efi` (confirms the new sources compile to nothing harmful when `GBL_MODE != 2`).

- [ ] **Step 5: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/Mode2Overlay.h \
  GblChainloadPkg/Library/ProtocolHookLib/Mode2Overlay.c \
  GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf
git commit -m "feat(mode-2): Mode2Overlay profile holder + policy entrypoints"
```

---

### Task 7: Wire the rewrite into `QseecomHook` + `SpssHook`

**Goal:** Call the mode-2 rewrite policy from the live hooks — KM send buffers in `QseecomHook`, the SPSS struct in `SpssHook` — under `#if (GBL_MODE == 2)`.

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c`

**Acceptance Criteria:**
- [ ] In a mode-2 build, `HookedSendCmd` calls `Mode2Policy_RewriteKmSend` on the first-entry path after `CmdId` is read and before `gOriginalSendCmd` is invoked.
- [ ] In a mode-2 build, `HookedShareKeyMintInfo` calls `Mode2Policy_RewriteSpss` before `gOrigShareKeyMintInfo`.
- [ ] `dist/mode-1.efi` and `dist/mode-0.efi` still build (the `GBL_MODE == 2` blocks are absent there).

**Verify:** `./scripts/build.sh --mode 2 && ./scripts/build.sh --mode 1` → both print their `==> Built ...` line.

**Steps:**

- [ ] **Step 1: Add the mode-2 block to `QseecomHook.c`**

`QseecomHook.c` already `#include "Mode1Overlay.h"`. Add alongside it:

```c
#include "Mode2Overlay.h"
```

In `HookedSendCmd`, the buffer's `CmdId` is read into a local near the top. The existing `#if (GBL_MODE == 1)` OplusSec-drop block sits right after that. Add a sibling block immediately after it:

```c
#if (GBL_MODE == 2)
  /* Mode-2 policy: rewrite KM SET_ROT/SET_VERSION/SET_BOOT_STATE/SET_VBH
     send buffers in place from the loaded profile before forwarding to
     TZ. Applies on the first-entry path only; reentrant calls below
     forward the (already-rewritten) buffer untouched. */
  if (First && SendBuf != NULL) {
    Mode2Policy_RewriteKmSend (CmdId, SendBuf, SendLen);
  }
#endif
```

Place this AFTER the `First = HookEnter(...)` call and the `gOriginalSendCmd == NULL` early-return, and BEFORE the `if (!First)` reentrant forward — so a rewrite happens exactly once, on the outermost call, and reentrant forwards carry the rewritten bytes. If the current code computes `First` after `CmdId`, keep this block after both.

- [ ] **Step 2: Add the mode-2 block to `SpssHook.c`**

In `SpssHook.c`, add `#include "Mode2Overlay.h"`. In `HookedShareKeyMintInfo`, after the `First`/`Info == NULL` guard and before the value is logged + forwarded to `gOrigShareKeyMintInfo`, add:

```c
#if (GBL_MODE == 2)
  /* Mode-2: rewrite the packed RoT/BootState/Vbh mirror before it
     reaches the SPU. KeymintSharedInfoStruct is the packed wire form. */
  if (First && Info != NULL) {
    Mode2Policy_RewriteSpss (Info, (UINT32)sizeof (KeymintSharedInfoStruct));
  }
#endif
```

(If `sizeof (KeymintSharedInfoStruct)` is not in scope, use the literal `GBL_SPSS_INFO_LEN` from `Mode2Rewrite.h` — the packed mirror is 144 bytes. The rewrite function bounds-checks against `GBL_SPSS_INFO_LEN` regardless.)

- [ ] **Step 3: Build all three modes**

Run: `./scripts/build.sh --mode 2` → `==> Built dist/mode-2.efi`
Run: `./scripts/build.sh --mode 1` → `==> Built dist/mode-1.efi`
Run: `./scripts/build.sh --mode 0` → `==> Built dist/mode-0.efi`

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c \
  GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c
git commit -m "feat(mode-2): wire KM/SPSS rewrite into the live hooks"
```

---

### Task 8: `BootFlow` profile load + `InstallAll` required hooks

**Goal:** In a mode-2 build, load the profile before chain-load and hand it to `Mode2Overlay`; make the Qseecom + SPSS hooks required for mode-2; record a warning state when the profile is missing/invalid.

**Files:**
- Modify: `GblChainloadPkg/Application/GblChainload/BootFlow.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c`

**Acceptance Criteria:**
- [ ] In a mode-2 build, `BootFlow` calls `GblPayload_LoadMode2Profile`; on success it calls `Mode2_SetProfile`; on `EFI_NOT_FOUND`/`EFI_LOAD_ERROR` it logs and continues (honest boot) and sets the warning state via `GblFastbootSetMode2Warning` (Task 9).
- [ ] In a mode-2 build, `InstallAll` treats a Qseecom or SPSS install failure as FATAL (aborts chain-load), matching how mode-1 treats Qseecom.
- [ ] mode-0 / mode-1 `InstallAll` behavior is unchanged.

**Verify:** `./scripts/build.sh --mode 2` → `==> Built dist/mode-2.efi`; the log shows `BootFlow: mode-2 profile ...` on a boot.

**Steps:**

- [ ] **Step 1: Load the profile in `BootFlow.c`**

`BootFlow.c` already `#include <Library/GblPayloadLib.h>`. After the Tier-1/Tier-2 block sets `Pe`/`PeSize` and before `ProtocolHook_InstallAll`, add:

```c
#if (GBL_MODE == 2)
  {
    struct gbl_mode2_profile Mode2Profile;
    EFI_STATUS M2Status =
        GblPayload_LoadMode2Profile (gImageHandle, &Mode2Profile);
    if (!EFI_ERROR (M2Status)) {
      Mode2_SetProfile (&Mode2Profile);
      GBL_INFO ("BootFlow: mode-2 profile loaded — spoof active\n");
    } else {
      GBL_INFO ("BootFlow: mode-2 profile unavailable (%r) — honest boot\n",
                M2Status);
      GblFastbootSetMode2Warning (
        (M2Status == EFI_NOT_FOUND)
          ? "MODE-2 PROFILE MISSING - booting honest, attestation will fail"
          : "MODE-2 PROFILE INVALID - booting honest, attestation will fail");
    }
  }
#endif
```

Add the includes near the top of `BootFlow.c`:

```c
#if (GBL_MODE == 2)
#include "../../Library/ProtocolHookLib/Mode2Overlay.h"
#endif
```

`GblFastbootSetMode2Warning` is declared in Task 9; if Task 9 is implemented after this task, the symbol resolves at link time once both are present. (Subagent-driven execution: implement Task 9 before final build.)

- [ ] **Step 2: Make Qseecom + SPSS required for mode-2 in `InstallAll.c`**

In `InstallAll.c`, the Qseecom install currently does `#if (GBL_MODE == 1)` → FATAL/return, `#else` → continue. Change the guard to include mode 2:

```c
#if (GBL_MODE == 1) || (GBL_MODE == 2)
    Print (L"ProtocolHookLib: FATAL — Qseecom install failed (%r), aborting chain-load\n",
           Status);
    return Status;
#else
    Print (L"ProtocolHookLib: Qseecom install failed (%r) - continuing (mode-0 observation-only)\n",
           Status);
    Result->QseecomInstalledSlots = 0;
#endif
```

For SPSS — currently optional for all modes. Wrap the SPSS failure handling so mode-2 makes it FATAL:

```c
  Status = InstallSpssHook ();
  if (EFI_ERROR (Status)) {
#if (GBL_MODE == 2)
    Print (L"ProtocolHookLib: FATAL — SPSS install failed (%r), aborting chain-load (mode-2)\n",
           Status);
    return Status;
#else
    Print (L"ProtocolHookLib: SPSS install failed (%r) - continuing (observation-only)\n",
           Status);
    Result->SpssInstalledSlots = 0;
#endif
  } else {
    Result->SpssInstalledSlots = 1;
  }
  Result->SpssExpectedSlots = 1;
```

Also update the `VerifiedBoot` install comment block — mode-2 does NOT need VerifiedBoot fatal (ABL stays honest). Leave the existing `#if (GBL_MODE == 1)` guard on VerifiedBoot unchanged so mode-2 falls into the `#else` (observation-only) branch.

- [ ] **Step 3: Build mode-2**

Run: `./scripts/build.sh --mode 2` → expect `==> Built dist/mode-2.efi` (this will only fully link once Task 9 provides `GblFastbootSetMode2Warning`; if building this task in isolation, temporarily stub the call — but under subagent-driven execution, Task 9 lands first or in the same batch).

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Application/GblChainload/BootFlow.c \
  GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c
git commit -m "feat(mode-2): BootFlow profile load + required QSEE/SPSS hooks"
```

---

### Task 9: FastbootLib mode-2 profile warning line

**Goal:** Add a red mode-2-profile warning line to gbl-chainload's fastboot screen, modeled on the existing `AVB WARNING - ...` line.

**Files:**
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`
- Modify: `edk2/QcomModulePkg/Library/BootLib/FastbootMenu.c`

**Acceptance Criteria:**
- [ ] `GblFastbootSetMode2Warning(const CHAR8 *)` stores a warning string; `GblFastbootGetMode2Warning(OUT CHAR8 *, IN UINTN)` retrieves it (empty when never set).
- [ ] `FastbootMenu.c` renders a `MODE-2 - <warning>` line in the same red style as `AVB WARNING - ...`, only when the warning is non-empty.
- [ ] `dist/mode-2.efi` builds and links cleanly.

**Verify:** `./scripts/build.sh --mode 2` → `==> Built dist/mode-2.efi`

**Steps:**

- [ ] **Step 1: Add the setter/getter to `FastbootCmds.c`**

Read `FastbootCmds.c` around the existing `mVbmetaWarning` global and `GblFastbootGetAvbWarning` (near line 5055–5066). Mirror them — add next to `mVbmetaWarning`:

```c
STATIC CHAR8 mMode2Warning[MAX_RSP_SIZE] = "";

VOID
GblFastbootSetMode2Warning (IN CONST CHAR8 *Warning)
{
  if (Warning == NULL) {
    mMode2Warning[0] = '\0';
    return;
  }
  AsciiStrnCpyS (mMode2Warning, sizeof (mMode2Warning),
                 Warning, sizeof (mMode2Warning) - 1);
}

VOID
GblFastbootGetMode2Warning (OUT CHAR8 *Out, IN UINTN OutCap)
{
  if (Out == NULL || OutCap == 0) {
    return;
  }
  AsciiStrnCpyS (Out, OutCap, mMode2Warning, OutCap - 1);
}
```

(Use whatever buffer-size constant and `AsciiStrnCpyS`/`AsciiStrCpyS` form the surrounding `GblFastbootGetAvbWarning` code uses — match it exactly.)

- [ ] **Step 2: Render the line in `FastbootMenu.c`**

Read `FastbootMenu.c` around line 309 (the `"AVB WARNING - "` menu entry) and 427–435 (where `GblFastbootGetAvbWarning` is called and the line conditionally drawn). Add a parallel `extern` near line 85:

```c
extern VOID GblFastbootGetMode2Warning (OUT CHAR8 *Out, IN UINTN OutCap);
```

Add a `MODE-2 - ` slot to the menu-message table next to the `AVB WARNING - ` entry, and a parallel render block next to the AVB one:

```c
  CHAR8 Mode2Warning[MAX_MSG_SIZE] = "";
  GblFastbootGetMode2Warning (Mode2Warning, sizeof (Mode2Warning));
  if (Mode2Warning[0] != '\0') {
    /* draw "MODE-2 - <Mode2Warning>" in the same red style as the
       AVB WARNING line above — copy that block's draw call verbatim,
       substituting the "MODE-2 - " prefix and Mode2Warning text. */
  }
```

Replace the comment with the actual draw call copied from the `AVB WARNING - ` block immediately above it (same colour argument, same `AsciiStrLen` pattern).

- [ ] **Step 3: Declare the setter for `BootFlow.c`**

So `BootFlow.c` (Task 8) can call `GblFastbootSetMode2Warning`, add a declaration. Put it in the GBL header `BootFlow.c` already includes — add to `GblChainloadPkg/Include/Library/GblLog.h` (or whichever GBL-owned header is shared) is wrong scope; instead add an `extern` directly in `BootFlow.c`'s mode-2 include block:

```c
#if (GBL_MODE == 2)
#include "../../Library/ProtocolHookLib/Mode2Overlay.h"
extern VOID GblFastbootSetMode2Warning (IN CONST CHAR8 *Warning);
#endif
```

(Update the Task 8 Step-1 include block to include this `extern` — they are the same block.)

- [ ] **Step 4: Build mode-2**

Run: `./scripts/build.sh --mode 2` → expect `==> Built dist/mode-2.efi`.

- [ ] **Step 5: Commit**

```bash
git add edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c \
  edk2/QcomModulePkg/Library/BootLib/FastbootMenu.c
git commit -m "feat(mode-2): fastboot-screen profile-missing warning line"
```

---

## Final verification

After all tasks, run the host suite and the build smoke:

```bash
for t in 076 077 078; do bash tests/host/${t}_*.sh; done
bash tests/host/061_parser_fuzz.sh
bash tests/host/063_pe_sanity.sh
bash tests/045_mode_taxonomy_lint.sh
./scripts/build.sh --mode 0
./scripts/build.sh --mode 1
./scripts/build.sh --mode 2
```

Expected: every host test prints its `PASS:` line; all three builds print `==> Built dist/mode-<N>.efi`.

On-device validation (manual, user-driven — not part of automated execution): stage `dist/mode-2.efi` with a hand-built GBLP1 overlay carrying a real profile, `fastboot stage` + `fastboot oem boot-efi`, and confirm the logfs shows `mode2 | km-rewrite | cmd=0x...` lines for `0x201`/`0x208` and `mode2 | spss-rewrite`.

## Follow-up plans (out of scope here)

- Slice 3 — vbmeta→profile helper (host Python + device aarch64) and the `gbl-pack` XML→`0x0010` compiler.
- Slice 4 — the `images/`-drop build orchestration.
- Slice 5 — the mode-2 ZIP with `build.prop` OEM-patch selection + profile staleness validation.
