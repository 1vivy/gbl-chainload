# Engine Rework (PR1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Collapse the three compile-time-mode EFIs into one capability-driven EFI binary whose runtime hook installation is gated by a 16-byte GBLP1 manifest entry, retire the `patch1` UTF-16 EFISP rewrite (replaced by an always-on `BlockIoHook` EFISP gate), and restructure `DynamicPatchLib` into effect-named patch groups (`abl_permissive/`, `oem/oplus/`, `retired/`) with OEM patches host-only.

**Architecture:** Engine reads a new `GBLP1_TYPE_MANIFEST` entry (0x0020, magic `'GMAN'`) into `gManifest`; `ProtocolHook_InstallAll` and per-hook mutation paths gate on `gManifest.WantFakelockHook` / `gManifest.WantProfileSpoof` instead of `#if (GBL_MODE == N)`. When no `cached_abl` is present, the boot-time fallback (`RunDynamicPatchOnSlotAbl`) applies the `abl_permissive` group only — OEM patches are excluded from the firmware build and only applied at host packing time. Three modes survive only as host-side install presets that drive the manifest bits and the abl-patcher invocation; on-device there is one EFI and one manifest-defined behavior surface.

**Tech Stack:** EDK2 (C, AARCH64), GBLP1 binary format, fastboot stage + oem boot-efi for on-device test, host C tools (gbl-pack / abl-patcher / gblp1-inspect), Python (efisp-package.py), busybox shell (zip/modes/), bash (tests/host/).

**Spec references:**
- Combined coordination + naming + manifest refinements: `docs/superpowers/specs/2026-05-22-engine-rework-and-rust-consolidation-design.md` (§5 = PR1 scope).
- Original engine-rework design, authoritative for unchanged sections: `docs/superpowers/specs/2026-05-22-engine-rework-design.md`.

**Worktree:** `.claude/worktrees/engine-rework` (branch `engine-rework` off `main`).

**Dependency note:** All tasks land on the `engine-rework` branch. PR2 (Rust consolidation) is rebased on this branch; do not force-push existing commits to avoid breaking PR2's rebases. Add new commits per iteration.

---

### Task 1: GBLP1 manifest entry — wire format + host-testable parser

**Goal:** Define the `GBLP1_TYPE_MANIFEST = 0x0020` entry constants in the shared header, add a parser to `GblPayloadLib/PayloadParse.c` that validates and decodes its 16-byte payload, and cover absence / present / malformed cases with a host shell test. This is the foundation for every later task.

**Files:**
- Modify: `tools/shared/gblp1.h` (add manifest constants)
- Modify: `GblChainloadPkg/Library/GblPayloadLib/Internal/PayloadParse.h` (add error codes + struct + decl)
- Modify: `GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c` (add parser fn)
- Create: `tests/host/093_manifest_parse.sh` (host parser test driver)
- Create: `tests/host/helpers/test_manifest_parse.c` (calls parser directly)
- Modify: `tests/host/helpers/Makefile` (build the new helper)

**Acceptance Criteria:**
- [ ] `gblp1.h` defines `GBLP1_TYPE_MANIFEST = 0x0020u`, `GBLP1_MANIFEST_MAGIC = "GMAN"` (4 bytes), `GBLP1_MANIFEST_SIZE = 16u`, `GBLP1_MANIFEST_SCHEMA_VERSION = 1u`, `GBLP1_MANIFEST_BIT_FAKELOCK_HOOK = 0x0001u`, `GBLP1_MANIFEST_BIT_PROFILE_SPOOF = 0x0002u`, `GBLP1_MANIFEST_BITS_RESERVED_MASK = 0xFFFCu`.
- [ ] `PayloadParse.h` declares `struct gbl_manifest { uint16_t cap_bits; }` and `enum gbl_payload_status gbl_payload_find_manifest(const uint8_t *bytes, size_t size, struct gbl_manifest *out, int *out_present)`.
- [ ] New error codes added: `GBL_PAYLOAD_NO_MANIFEST`, `GBL_PAYLOAD_BAD_MANIFEST_MAGIC`, `GBL_PAYLOAD_BAD_MANIFEST_SCHEMA`, `GBL_PAYLOAD_BAD_MANIFEST_RESERVED`, `GBL_PAYLOAD_BAD_MANIFEST_SIZE`.
- [ ] Parser validates: payload size == 16, magic == `'GMAN'`, schema_version == 1, reserved bits (bits 2–15) == 0, reserved_pad[8] == all zero. Sets `*out_present = 0` and returns `GBL_PAYLOAD_OK` when no manifest entry exists (absence is not an error).
- [ ] `093_manifest_parse.sh` exercises absence, valid mode-0 (bits 0x0000), valid mode-1 (0x0001), valid mode-2 (0x0002), bad magic, bad schema, unknown reserved bit set (0x0004), non-zero pad. All 8 cases pass.

**Verify:** `bash tests/host/093_manifest_parse.sh` → prints `093_manifest_parse: OK (8/8)` and exits 0.

**Steps:**

- [ ] **Step 1: Write the failing test driver.** Create `tests/host/helpers/test_manifest_parse.c`. This is a small standalone host binary that hand-crafts GBLP1 containers in memory, calls `gbl_payload_find_manifest`, and asserts results. Build it with `-DGBL_HOST_BUILD` so PayloadParse.c uses `<string.h>` not EDK2 BaseMemoryLib.

```c
/* tests/host/helpers/test_manifest_parse.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/PayloadParse.h"
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Crc32.h"
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Sha256.h"
#include "../../../tools/shared/gblp1.h"

static void wle16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

/* Build a minimal valid GBLP1 container with one manifest entry whose
   payload bytes are caller-supplied (`payload`/`payload_size`). Returns
   alloc'd buffer + size; caller frees. */
static uint8_t *make_container(const uint8_t *payload, size_t payload_size, size_t *out_size) {
    uint32_t entries_end = GBLP1_HEADER_SIZE + GBLP1_ENTRY_SIZE;
    uint32_t off = (entries_end + GBLP1_PAYLOAD_ALIGN - 1) & ~(GBLP1_PAYLOAD_ALIGN - 1);
    uint32_t total = off + (uint32_t)payload_size + GBLP1_FOOTER_SIZE;
    /* Pad payload region to align footer cleanly. */
    total = (total + GBLP1_PAYLOAD_ALIGN - 1) & ~(GBLP1_PAYLOAD_ALIGN - 1);
    uint8_t *buf = calloc(1, total);
    memcpy(buf, GBLP1_MAGIC, GBLP1_MAGIC_SIZE);
    wle16(buf + 8,  GBLP1_VERSION);
    wle16(buf + 10, GBLP1_HEADER_SIZE);
    wle32(buf + 12, GBLP1_FLAGS_LE);
    wle32(buf + 16, total);
    wle32(buf + 20, 1);
    uint8_t *e = buf + GBLP1_HEADER_SIZE;
    wle16(e + 0,  GBLP1_TYPE_MANIFEST);
    wle32(e + 4,  off);
    wle32(e + 8,  (uint32_t)payload_size);
    if (payload) memcpy(buf + off, payload, payload_size);
    gbl_sha256(buf + off, payload_size, e + 16);
    memcpy(buf + total - GBLP1_FOOTER_SIZE, GBLP1_FOOTER, GBLP1_FOOTER_SIZE);
    wle32(buf + 24, gbl_crc32(buf, 24));
    *out_size = total;
    return buf;
}

/* Build a valid 16-byte manifest payload. */
static void make_payload(uint8_t *out16, uint16_t cap_bits, uint16_t schema_ver,
                         int bad_magic, int bad_pad) {
    memset(out16, 0, 16);
    memcpy(out16, bad_magic ? "BAD!" : GBLP1_MANIFEST_MAGIC, 4);
    wle16(out16 + 4, schema_ver);
    wle16(out16 + 6, cap_bits);
    if (bad_pad) out16[15] = 0xff;
}

#define CASE(label, expected_status, expected_present, expected_bits, ...) do { \
    uint8_t pl[16]; make_payload(pl, __VA_ARGS__); \
    size_t n; uint8_t *b = make_container(pl, sizeof(pl), &n); \
    struct gbl_manifest m = {0}; int present = -1; \
    enum gbl_payload_status s = gbl_payload_find_manifest(b, n, &m, &present); \
    if (s != (expected_status) || present != (expected_present) || \
        (present && m.cap_bits != (expected_bits))) { \
        fprintf(stderr, "FAIL %s: status=%d present=%d bits=0x%x\n", \
                label, (int)s, present, m.cap_bits); free(b); return 1; \
    } \
    free(b); pass++; \
} while (0)

int main(void) {
    int pass = 0;
    CASE("mode-0",   GBL_PAYLOAD_OK,                  1, 0x0000, 0x0000, 1, 0, 0);
    CASE("mode-1",   GBL_PAYLOAD_OK,                  1, 0x0001, 0x0001, 1, 0, 0);
    CASE("mode-2",   GBL_PAYLOAD_OK,                  1, 0x0002, 0x0002, 1, 0, 0);
    CASE("bad-magic", GBL_PAYLOAD_BAD_MANIFEST_MAGIC, 1, 0x0000, 0x0000, 1, 1, 0);
    CASE("bad-sch",  GBL_PAYLOAD_BAD_MANIFEST_SCHEMA, 1, 0x0000, 0x0000, 2, 0, 0);
    CASE("bad-bit",  GBL_PAYLOAD_BAD_MANIFEST_RESERVED, 1, 0x0004, 0x0004, 1, 0, 0);
    CASE("bad-pad",  GBL_PAYLOAD_BAD_MANIFEST_RESERVED, 1, 0x0000, 0x0000, 1, 0, 1);
    /* Absence case: an empty container with no manifest entry. */
    {
        /* Re-use make_container with another type by post-editing the
           entry-type field — keeps test driver short. */
        uint8_t pl[16] = {0}; size_t n; uint8_t *b = make_container(pl, 16, &n);
        uint8_t *e = b + GBLP1_HEADER_SIZE;
        wle16(e + 0, GBLP1_TYPE_CACHED_ABL);  /* not manifest */
        gbl_sha256(b + (e[4] | (e[5]<<8) | (e[6]<<16) | (e[7]<<24)), 16, e + 16);
        wle32(b + 24, gbl_crc32(b, 24));
        struct gbl_manifest m = {0}; int present = -1;
        enum gbl_payload_status s = gbl_payload_find_manifest(b, n, &m, &present);
        if (s != GBL_PAYLOAD_OK || present != 0) {
            fprintf(stderr, "FAIL absence: status=%d present=%d\n", (int)s, present);
            free(b); return 1;
        }
        free(b); pass++;
    }
    printf("093_manifest_parse: OK (%d/8)\n", pass);
    return pass == 8 ? 0 : 1;
}
```

- [ ] **Step 2: Add the shell test wrapper.** Create `tests/host/093_manifest_parse.sh`:

```bash
#!/usr/bin/env bash
# tests/host/093_manifest_parse.sh — manifest entry parse coverage.
set -euo pipefail
cd "$(dirname "$0")/../.."
make -s -C tests/host/helpers test_manifest_parse
exec tests/host/helpers/test_manifest_parse
```

Make it executable: `chmod +x tests/host/093_manifest_parse.sh`.

- [ ] **Step 3: Run the test to verify it fails.** `bash tests/host/093_manifest_parse.sh` → expect compile error (`gbl_payload_find_manifest` not declared) or undefined symbols.

- [ ] **Step 4: Add manifest constants to `tools/shared/gblp1.h`.** Append after the existing entry-type defines (after the `GBLP1_TYPE_MODE2_PROFILE` line):

```c
#define GBLP1_TYPE_MANIFEST       0x0020u  /* engine capability manifest (GMAN) */

/* Manifest payload (16 bytes; little-endian). */
#define GBLP1_MANIFEST_MAGIC                "GMAN"
#define GBLP1_MANIFEST_MAGIC_SIZE           4u
#define GBLP1_MANIFEST_SIZE                 16u
#define GBLP1_MANIFEST_SCHEMA_VERSION       1u
#define GBLP1_MANIFEST_BIT_FAKELOCK_HOOK    0x0001u
#define GBLP1_MANIFEST_BIT_PROFILE_SPOOF    0x0002u
#define GBLP1_MANIFEST_BITS_RESERVED_MASK   0xFFFCu  /* bits 2..15 must be 0 */
```

- [ ] **Step 5: Add the struct + error codes + decl to `PayloadParse.h`.** Insert into `enum gbl_payload_status`:

```c
    GBL_PAYLOAD_NO_MANIFEST,
    GBL_PAYLOAD_BAD_MANIFEST_MAGIC,
    GBL_PAYLOAD_BAD_MANIFEST_SCHEMA,
    GBL_PAYLOAD_BAD_MANIFEST_RESERVED,
    GBL_PAYLOAD_BAD_MANIFEST_SIZE
```

Append at the end of the header, before `#endif`:

```c
/* Engine capability manifest. cap_bits is the raw bit field from the wire,
   not pre-validated — callers should compare against the GBLP1_MANIFEST_BIT_*
   constants. */
struct gbl_manifest { uint16_t cap_bits; };

/* Locate + validate the unique GBLP1_TYPE_MANIFEST entry. On
   GBL_PAYLOAD_OK with *out_present == 1: *out is filled. On
   *out_present == 0: no manifest entry in the container (NOT an error;
   caller treats as all-zero capabilities). On any other return: parse
   or validation failed; *out is undefined. */
enum gbl_payload_status
gbl_payload_find_manifest(const uint8_t *bytes, size_t size,
                          struct gbl_manifest *out, int *out_present);
```

- [ ] **Step 6: Implement the parser in `PayloadParse.c`.** Append at the end of the file (after `gbl_payload_scan_cached_abl`):

```c
enum gbl_payload_status
gbl_payload_find_manifest(const uint8_t *b, size_t n,
                          struct gbl_manifest *out, int *out_present) {
    const uint8_t *p = NULL; size_t sz = 0;
    enum gbl_payload_status s =
        gbl_payload_find_entry(b, n, GBLP1_TYPE_MANIFEST, &p, &sz);
    if (s != GBL_PAYLOAD_OK) return s;
    if (p == NULL) { *out_present = 0; return GBL_PAYLOAD_OK; }
    if (sz != GBLP1_MANIFEST_SIZE) return GBL_PAYLOAD_BAD_MANIFEST_SIZE;
    if (memcmp(p, GBLP1_MANIFEST_MAGIC, GBLP1_MANIFEST_MAGIC_SIZE) != 0)
        return GBL_PAYLOAD_BAD_MANIFEST_MAGIC;
    uint16_t schema = le16(p + 4);
    if (schema != GBLP1_MANIFEST_SCHEMA_VERSION)
        return GBL_PAYLOAD_BAD_MANIFEST_SCHEMA;
    uint16_t bits = le16(p + 6);
    if (bits & GBLP1_MANIFEST_BITS_RESERVED_MASK)
        return GBL_PAYLOAD_BAD_MANIFEST_RESERVED;
    /* Reserved pad: bytes 8..15 must all be zero. */
    for (int i = 8; i < 16; ++i) if (p[i] != 0)
        return GBL_PAYLOAD_BAD_MANIFEST_RESERVED;
    out->cap_bits = bits;
    *out_present = 1;
    return GBL_PAYLOAD_OK;
}
```

- [ ] **Step 7: Update `tests/host/helpers/Makefile`** to build the new helper. Look at the existing pattern (e.g., for `test_pe_sanity` if present). Add:

```makefile
test_manifest_parse: test_manifest_parse.c \
    ../../../GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c \
    ../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Crc32.c \
    ../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Sha256.c
	$(CC) $(CFLAGS) -DGBL_HOST_BUILD -o $@ $^
```

If the Makefile uses `all:` aggregation, add `test_manifest_parse` to it.

- [ ] **Step 8: Run the test to verify it passes.** `bash tests/host/093_manifest_parse.sh` → `093_manifest_parse: OK (8/8)`.

- [ ] **Step 9: Commit.**

```bash
git add tools/shared/gblp1.h \
        GblChainloadPkg/Library/GblPayloadLib/{Internal/PayloadParse.h,PayloadParse.c} \
        tests/host/{093_manifest_parse.sh,helpers/test_manifest_parse.c,helpers/Makefile}
git commit -m "GblPayloadLib: parse new GBLP1_TYPE_MANIFEST entry (0x0020, GMAN)"
```

---

### Task 2: `GblPayload_LoadManifest()` + `gManifest` global; load in BootFlow

**Goal:** Expose the parser through `GblPayloadLib`'s firmware-facing API as `GblPayload_LoadManifest()` (mirroring `GblPayload_LoadMode2Profile`), wire it into `BootFlow.c`, and define the firmware-side `gManifest` struct that `ProtocolHookLib` will consume in Task 8. Profile spoof gating in BootFlow.c switches from `#if (GBL_MODE == 2)` to runtime.

**Files:**
- Modify: `GblChainloadPkg/Include/Library/GblPayloadLib.h` (API decl + struct)
- Modify: `GblChainloadPkg/Library/GblPayloadLib/GblPayload.c` (impl + `gManifest` definition)
- Modify: `GblChainloadPkg/Application/GblChainload/BootFlow.c` (call site + runtime gate)

**Acceptance Criteria:**
- [ ] `GblPayloadLib.h` declares `struct GblManifest { BOOLEAN WantFakelockHook; BOOLEAN WantProfileSpoof; }` and `EFI_STATUS EFIAPI GblPayload_LoadManifest (IN EFI_HANDLE ImageHandle, OUT struct GblManifest *Manifest)`.
- [ ] `GblPayload.c` exposes `struct GblManifest gManifest = {0};` (global) and implements `GblPayload_LoadManifest` translating wire `cap_bits` to in-struct booleans. EFI_SUCCESS on present-and-valid; EFI_NOT_FOUND on absence (sets fields to FALSE); EFI_LOAD_ERROR on malformed.
- [ ] `BootFlow.c` calls `GblPayload_LoadManifest(gImageHandle, &gManifest)` after `GblPayload_LoadCachedAbl` / `RunDynamicPatchOnSlotAbl`, before any hook install.
- [ ] The `#if (GBL_MODE == 2)` block around `Mode2_SetProfile` becomes `if (gManifest.WantProfileSpoof)` — unconditional include of `Mode2Overlay.h`, unconditional `extern VOID GblFastbootSetMode2Warning` declaration.
- [ ] Firmware compiles cleanly (the existing tests/host suite is unaffected by this task).

**Verify:** `bash scripts/build.sh -m 1` (or whatever single-mode invocation builds an EFI today; collapse happens in Task 11) → produces a `.efi` artifact without errors.

**Steps:**

- [ ] **Step 1: Update `GblPayloadLib.h`** — add the struct + API decl before the closing `#endif`:

```c
/* Engine capability manifest, populated by GblPayload_LoadManifest from the
   GBLP1_TYPE_MANIFEST entry. ProtocolHookLib + BootFlow consume this to
   gate runtime mutations. Absent / invalid entry → all FALSE = mode-0. */
struct GblManifest {
  BOOLEAN  WantFakelockHook;
  BOOLEAN  WantProfileSpoof;
};

/* Single firmware-wide instance. Defined in GblPayload.c. */
extern struct GblManifest gManifest;

/* Locate the GBLP1 overlay, find the manifest (0x0020) entry, validate,
   and decode into *Manifest. Returns:
     EFI_SUCCESS    — manifest present and valid; *Manifest populated.
     EFI_NOT_FOUND  — no overlay or no manifest entry. *Manifest cleared
                      to all-FALSE so caller may use it as effective mode-0.
     EFI_LOAD_ERROR — overlay present but manifest malformed. *Manifest
                      cleared to all-FALSE. */
EFI_STATUS EFIAPI
GblPayload_LoadManifest (IN  EFI_HANDLE             ImageHandle,
                         OUT struct GblManifest    *Manifest);
```

- [ ] **Step 2: Implement in `GblPayload.c`** — append after `GblPayload_LoadMode2Profile`:

```c
struct GblManifest gManifest = {0};

EFI_STATUS EFIAPI
GblPayload_LoadManifest (IN  EFI_HANDLE          ImageHandle,
                         OUT struct GblManifest *Manifest) {
  VOID *Bytes = NULL; UINTN Size = 0;
  if (Manifest == NULL) return EFI_INVALID_PARAMETER;
  Manifest->WantFakelockHook = FALSE;
  Manifest->WantProfileSpoof = FALSE;

  EFI_STATUS Status = LocateOverlayBytes(&Bytes, &Size);
  if (EFI_ERROR(Status)) {
    GBL_INFO("gbl-payload: manifest — no overlay bytes (%r)\n", Status);
    return EFI_NOT_FOUND;
  }

  CONST UINT8 *B = (CONST UINT8 *)Bytes;
  enum gbl_payload_status PS = GBL_PAYLOAD_BAD_MAGIC;
  struct gbl_manifest WireM = {0}; int Present = 0;
  for (UINTN i = 0; i + GBLP1_MAGIC_SIZE <= Size; i++) {
    if (CompareMem(B + i, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) != 0) continue;
    PS = gbl_payload_find_manifest(B + i, Size - i, &WireM, &Present);
    if (PS == GBL_PAYLOAD_OK) break;
  }
  if (PS == GBL_PAYLOAD_BAD_MAGIC) {
    GBL_INFO("gbl-payload: manifest — no GBLP1 magic in overlay\n");
    return EFI_NOT_FOUND;
  }
  if (PS != GBL_PAYLOAD_OK) {
    GBL_INFO("gbl-payload: manifest — invalid (status=%d)\n", (int)PS);
    return EFI_LOAD_ERROR;
  }
  if (!Present) {
    GBL_INFO("gbl-payload: manifest — absent, effective mode-0\n");
    return EFI_NOT_FOUND;
  }
  Manifest->WantFakelockHook = (WireM.cap_bits & GBLP1_MANIFEST_BIT_FAKELOCK_HOOK) != 0;
  Manifest->WantProfileSpoof = (WireM.cap_bits & GBLP1_MANIFEST_BIT_PROFILE_SPOOF) != 0;
  GBL_INFO("gbl-payload: manifest — fakelock_hook=%u profile_spoof=%u\n",
           (UINT32)Manifest->WantFakelockHook, (UINT32)Manifest->WantProfileSpoof);
  return EFI_SUCCESS;
}
```

- [ ] **Step 3: Modify `BootFlow.c`** — drop the `#if (GBL_MODE == 2)` include at the top, add the manifest load, swap the mode-2 block's `#if` for a runtime if:

Replace (lines 30-37 region):
```c
#if (GBL_MODE == 2)
#include "../../Library/ProtocolHookLib/Mode2Overlay.h"
extern VOID GblFastbootSetMode2Warning (IN CONST CHAR8 *Warning);
#endif

#ifndef GBL_MODE
# error "GBL_MODE must be defined"
#endif
```

With:
```c
#include "../../Library/ProtocolHookLib/Mode2Overlay.h"
extern VOID GblFastbootSetMode2Warning (IN CONST CHAR8 *Warning);
```

(`GBL_MODE` requirement removed because it's no longer a thing. The `Mode2Overlay.h` rename to `ProfileOverlay.h` lands in Task 7; for now keep the existing header path.)

After the `BootFlow: ABL loaded via %a` log (line ~157), insert manifest load:

```c
  /* Load the engine capability manifest. Absence is non-fatal — gManifest
     stays all-FALSE (effective mode-0). */
  {
    EFI_STATUS MStatus = GblPayload_LoadManifest (gImageHandle, &gManifest);
    if (EFI_ERROR (MStatus) && MStatus != EFI_NOT_FOUND) {
      GBL_INFO ("BootFlow: manifest load returned %r — falling back to mode-0\n",
                MStatus);
    }
  }
```

Replace the `#if (GBL_MODE == 2) { ... } #endif` block (lines 159-185) with a runtime gate:

```c
  if (gManifest.WantProfileSpoof) {
    struct gbl_mode2_profile Mode2Profile;
    EFI_STATUS M2Status =
        GblPayload_LoadMode2Profile (gImageHandle, &Mode2Profile);
    if (!EFI_ERROR (M2Status)) {
      Mode2_SetProfile (&Mode2Profile);
      GBL_INFO ("BootFlow: profile spoof active\n");
    } else {
      GBL_INFO ("BootFlow: profile spoof requested but profile unavailable (%r) — honest boot\n",
                M2Status);
      Print (
        (M2Status == EFI_NOT_FOUND)
          ? L"GBL: MODE-2 PROFILE MISSING — booting honest, attestation will fail\n"
          : L"GBL: MODE-2 PROFILE INVALID — booting honest, attestation will fail\n");
      GblFastbootSetMode2Warning (
        (M2Status == EFI_NOT_FOUND)
          ? "MODE-2 PROFILE MISSING - booting honest, attestation will fail"
          : "MODE-2 PROFILE INVALID - booting honest, attestation will fail");
    }
  }
```

Replace `GBL_INFO ("BootFlow: start (mode=%d)\n", (int)GBL_MODE);` with:
```c
  GBL_INFO ("BootFlow: start\n");
```

Note: this task keeps the per-hook `#if (GBL_MODE == N)` gates inside InstallAll / per-hook bodies intact. They're swapped to runtime in Task 8 once `gManifest` exists and the renames have happened.

- [ ] **Step 4: Verify firmware still builds.** Run the existing build invocation (matches current docker pipeline). It must compile against `GBL_MODE=1` or `GBL_MODE=2` and produce a valid EFI for now — `gManifest` is defined but consumed only by BootFlow's `WantProfileSpoof` check (which will mirror the old `#if` for mode-2 builds where the overlay sets the bit).

- [ ] **Step 5: Commit.**

```bash
git add GblChainloadPkg/Include/Library/GblPayloadLib.h \
        GblChainloadPkg/Library/GblPayloadLib/GblPayload.c \
        GblChainloadPkg/Application/GblChainload/BootFlow.c
git commit -m "BootFlow: load manifest into gManifest; profile-spoof gates on bit"
```

---

### Task 3: `gbl-pack --manifest <bits>` emits the 16-byte manifest entry

**Goal:** Teach the host packer to emit a `GBLP1_TYPE_MANIFEST` entry alongside the existing cached_abl / source_meta / mode2_profile entries when `--manifest <bits>` is passed. Round-trip tested against the Task 1 parser.

**Files:**
- Modify: `tools/gbl-pack/pack.h` (add `cap_bits` + `have_manifest` to inputs)
- Modify: `tools/gbl-pack/pack.c` (emit manifest entry when requested)
- Modify: `tools/gbl-pack/gbl-pack.c` (parse `--manifest`)
- Create: `tests/host/094_gbl_pack_manifest.sh`

**Acceptance Criteria:**
- [ ] `gbl-pack --manifest 0x01 --out OUT` (with `--cached-abl` etc. also satisfied) produces a container that, when parsed by `gbl_payload_find_manifest`, returns `present=1, cap_bits=0x0001`.
- [ ] `gbl-pack` without `--manifest` produces a container with no manifest entry (`present=0`).
- [ ] `--manifest` accepts decimal (`1`, `2`) or hex (`0x1`, `0x02`) syntax.
- [ ] `--manifest` value with reserved bits set (e.g., `0x04`) is rejected at packing time with `gbl-pack: bad --manifest bits (reserved bits set)` and exit code 2 — defense in depth before the on-device parser sees it.

**Verify:** `bash tests/host/094_gbl_pack_manifest.sh` → exits 0.

**Steps:**

- [ ] **Step 1: Write the failing test.** Create `tests/host/094_gbl_pack_manifest.sh`:

```bash
#!/usr/bin/env bash
# tests/host/094_gbl_pack_manifest.sh — gbl-pack --manifest round-trip.
set -euo pipefail
cd "$(dirname "$0")/../.."
make -s -C tools/gbl-pack
make -s -C tools/abl-patcher  # need a sane patched.efi as cached-abl input
PE=tests/images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing"; exit 0; }
OUT=tests/host/.last/094
mkdir -p "$OUT"

# Run abl-patcher to get a sane patched PE (cached_abl input).
tools/abl-patcher/abl-patcher --in "$PE" --out "$OUT/patched.efi" >/dev/null

pack() {
    tools/gbl-pack/gbl-pack \
        --cached-abl "$OUT/patched.efi" \
        --source     "$OUT/patched.efi" \
        --extracted  "$OUT/patched.efi" \
        --out        "$OUT/$1.bin" "$@" \
    2>"$OUT/$1.err" || { echo "FAIL pack $1:"; cat "$OUT/$1.err"; exit 1; }
}

# Case 1: with --manifest 0x01.
pack mode1 --manifest 0x01
# Verify by grepping for 'GMAN' + bit 0 set inside.
if ! grep -aq 'GMAN' "$OUT/mode1.bin"; then echo "FAIL: GMAN absent"; exit 1; fi

# Case 2: without --manifest — no GMAN should appear.
pack none
if grep -aq 'GMAN' "$OUT/none.bin"; then echo "FAIL: GMAN unexpected"; exit 1; fi

# Case 3: reserved-bit rejected.
if tools/gbl-pack/gbl-pack \
        --cached-abl "$OUT/patched.efi" --source "$OUT/patched.efi" \
        --extracted "$OUT/patched.efi" --out "$OUT/bad.bin" \
        --manifest 0x04 2>"$OUT/bad.err"; then
    echo "FAIL: --manifest 0x04 should have been rejected"; exit 1
fi
grep -q 'reserved bits' "$OUT/bad.err" || \
    { echo "FAIL: error message missing"; cat "$OUT/bad.err"; exit 1; }

echo "094_gbl_pack_manifest: OK"
```

`chmod +x tests/host/094_gbl_pack_manifest.sh`. Run → fail (no `--manifest` flag yet).

- [ ] **Step 2: Update `pack.h`.** Add fields:

```c
struct gbl_pack_inputs {
    const uint8_t *cached_abl;  size_t cached_abl_size;
    const uint8_t *source;      size_t source_size;
    const uint8_t *extracted;   size_t extracted_size;
    const uint8_t *mode2_profile; size_t mode2_profile_size;
    int            have_manifest;     /* 0/1 */
    uint16_t       manifest_cap_bits; /* used only when have_manifest */
    const char    *packer_version;
    const char    *timestamp_iso8601;
};
```

Add to `enum gbl_pack_status`:
```c
    GBL_PACK_ERR_MANIFEST_BAD
```

- [ ] **Step 3: Emit the manifest entry in `pack.c`.** In `gbl_pack_build`, after the `have_profile` checks add manifest validation:

```c
    int have_manifest = in->have_manifest != 0;
    if (have_manifest && (in->manifest_cap_bits & GBLP1_MANIFEST_BITS_RESERVED_MASK))
        return GBL_PACK_ERR_MANIFEST_BAD;
```

Extend the entry-descriptor array size from `[3]` to `[4]` and add manifest emission after the profile block:

```c
    if (have_manifest) {
        ents[ec].type = GBLP1_TYPE_MANIFEST;
        ents[ec].data = NULL;          ents[ec].size = GBLP1_MANIFEST_SIZE; ec++;
    }
```

In the per-entry payload writer (the loop building `ents[i].type == GBLP1_TYPE_SOURCE_META` block), add a sibling `else if`:

```c
        } else if (ents[i].type == GBLP1_TYPE_MANIFEST) {
            uint8_t *m = buf + payload_off[i];
            memcpy(m, GBLP1_MANIFEST_MAGIC, GBLP1_MANIFEST_MAGIC_SIZE);
            wle16(m + 4, GBLP1_MANIFEST_SCHEMA_VERSION);
            wle16(m + 6, in->manifest_cap_bits);
            /* m + 8..15 left zero by calloc. */
        }
```

Also extend the array bound check (the `ents[3]` arrays — bump to `[4]`).

- [ ] **Step 4: Parse `--manifest` in `gbl-pack.c`.** Add a parse case in the argv loop:

```c
        else if (!strcmp(argv[i], "--manifest") && i + 1 < argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 0);  /* 0 = auto-detect 0x prefix */
            if (!end || *end || v > 0xFFFFu) {
                fprintf(stderr, "gbl-pack: bad --manifest bits (parse)\n");
                return 2;
            }
            in.have_manifest = 1;
            in.manifest_cap_bits = (uint16_t)v;
        }
```

Update the usage line to include `[--manifest BITS]`.

Map `GBL_PACK_ERR_MANIFEST_BAD` to a friendlier message:

```c
    if (s == GBL_PACK_ERR_MANIFEST_BAD) {
        fprintf(stderr, "gbl-pack: bad --manifest bits (reserved bits set)\n");
        return 2;
    }
```

- [ ] **Step 5: Run the test to verify it passes.** `bash tests/host/094_gbl_pack_manifest.sh` → `094_gbl_pack_manifest: OK`.

- [ ] **Step 6: Commit.**

```bash
git add tools/gbl-pack/{pack.h,pack.c,gbl-pack.c} tests/host/094_gbl_pack_manifest.sh
git commit -m "gbl-pack: --manifest <bits> emits GBLP1_TYPE_MANIFEST entry"
```

---

### Task 4: `gblp1-inspect` pretty-prints the manifest entry

**Goal:** Add a manifest-aware case to `gblp1-inspect` so users debugging an EFISP payload can see capability bits without hex-diving.

**Files:**
- Modify: `tools/gblp1-inspect/gblp1-inspect.c` (add manifest case to type-printer)
- Modify: `tests/host/089_gblp1_inspect.sh` (assert manifest line on a packed sample)

**Acceptance Criteria:**
- [ ] Running `gblp1-inspect <pack-with-manifest-0x01.bin>` prints a section like:
  ```
  entry: type=0x0020 (manifest) off=0x... size=16
    magic=GMAN schema=1 fakelock_hook=yes profile_spoof=no
  ```
- [ ] Test `089_gblp1_inspect.sh` asserts the `fakelock_hook=yes profile_spoof=no` line.

**Verify:** `bash tests/host/089_gblp1_inspect.sh` → exits 0.

**Steps:**

- [ ] **Step 1: Read the existing `gblp1-inspect.c`** to understand its entry-walking pattern. (One small file; absorb the dispatch on `GBLP1_TYPE_*`.)

- [ ] **Step 2: Add a manifest case.** In the entry-type switch / if-chain, add:

```c
case GBLP1_TYPE_MANIFEST: {
    printf("  type=0x0020 (manifest) off=0x%x size=%u\n", off, sz);
    if (sz != GBLP1_MANIFEST_SIZE) { printf("    (bad size, expected %u)\n", GBLP1_MANIFEST_SIZE); break; }
    const uint8_t *m = buf + off;
    char magic[5] = {m[0],m[1],m[2],m[3],0};
    uint16_t schema = (uint16_t)(m[4] | (m[5]<<8));
    uint16_t bits   = (uint16_t)(m[6] | (m[7]<<8));
    printf("    magic=%s schema=%u fakelock_hook=%s profile_spoof=%s\n",
           magic, schema,
           (bits & GBLP1_MANIFEST_BIT_FAKELOCK_HOOK) ? "yes" : "no",
           (bits & GBLP1_MANIFEST_BIT_PROFILE_SPOOF) ? "yes" : "no");
    break;
}
```

If the file uses a generic "unknown type" fallthrough today, place the new case before it. Match the existing print style for whitespace.

- [ ] **Step 3: Extend `089_gblp1_inspect.sh`** to pack a manifest-bearing sample and grep the inspect output:

```bash
# inside existing 089 test, after the existing checks:
"$PACK" --cached-abl "$PATCHED" --source "$RAW" --extracted "$PATCHED" \
        --manifest 0x01 --out "$OUT/m1.bin"
"$INSPECT" "$OUT/m1.bin" > "$OUT/m1.inspect"
grep -q "fakelock_hook=yes profile_spoof=no" "$OUT/m1.inspect" \
    || { echo "FAIL: manifest line missing"; cat "$OUT/m1.inspect"; exit 1; }
```

- [ ] **Step 4: Run the test.** `bash tests/host/089_gblp1_inspect.sh` passes.

- [ ] **Step 5: Commit.**

```bash
git add tools/gblp1-inspect/gblp1-inspect.c tests/host/089_gblp1_inspect.sh
git commit -m "gblp1-inspect: pretty-print manifest capability bits"
```

---

### Task 5: Restructure `DynamicPatchLib/` to `abl_permissive/` + `oem/oplus/` + `retired/`; split per-patch

**Goal:** Pure filesystem and symbol restructure of `DynamicPatchLib`. `mode_1/mode_1.c` splits into per-patch files under `abl_permissive/`; `oem/oneplus_canoe.c` moves under `oem/oplus/`; `universal/universal.c` (carrying retired patch1) moves to `retired/`. Patch contents, anchor logic, and behavior are unchanged. Tests pass before and after.

**Files:**
- Move: `GblChainloadPkg/Library/DynamicPatchLib/mode_1/Signatures.h` → `abl_permissive/Signatures.h`
- Move + split: `mode_1/mode_1.c` → `abl_permissive/libavb_force_success.c` + `abl_permissive/fastboot_lock_gates.c`
- Move: `oem/oneplus_canoe.c` → `oem/oplus/bypass_warning.c`; `oem/Signatures.h` → `oem/oplus/Signatures.h`
- Move: `universal/universal.c` → `retired/block_efisp_recursion.c`; `universal/Signatures.h` → `retired/Signatures.h`
- Modify: `DynamicPatchLib.inf` `[Sources]` to reflect new paths
- Modify: `tools/abl-patcher/Makefile`, `tools/gbl-pack/Makefile` if they reference the old paths
- Modify: `tests/host/088_patch7_multi_abl.sh` path-only edits to match new tree
- Modify: any other test that compiles against DynamicPatchLib paths

**Acceptance Criteria:**
- [ ] Directory `DynamicPatchLib/mode_1/`, `DynamicPatchLib/oem/oneplus_canoe.c`, `DynamicPatchLib/universal/` no longer exist.
- [ ] `abl_permissive/libavb_force_success.c` declares `CONST PATCH_DESC kAblPermissiveLibavbPatches[]` + `kAblPermissiveLibavbPatchesCount` (one entry, the patch10 logic).
- [ ] `abl_permissive/fastboot_lock_gates.c` declares `CONST PATCH_DESC kAblPermissiveFastbootGatePatches[]` + `kAblPermissiveFastbootGatePatchesCount` (one entry, the patch6 logic).
- [ ] `oem/oplus/bypass_warning.c` declares `CONST PATCH_DESC kOemOplusPatches[]` + `kOemOplusPatchesCount` (one entry, the patch7 logic).
- [ ] `retired/block_efisp_recursion.c` is in tree, top-of-file comment notes `RETIRED 2026-05-22 — superseded by BlockIoHook EFISP gate (Task 9). Reference implementation only.` Array still declared `kUniversalPatches[]` (single entry) — drop from active table happens in Task 10.
- [ ] All existing host tests still pass (`bash tests/host/run-all.sh` or per-test invocations).
- [ ] EDK2 firmware build still succeeds.

**Verify:** `bash tests/host/run-all.sh` exits 0 (or, if scoped: `bash tests/host/088_patch7_multi_abl.sh && bash tests/host/065_patch_sig_parity.sh && bash tests/host/083_abl_patcher_oem.sh` all pass).

**Steps:**

- [ ] **Step 1: Rename + split `mode_1/mode_1.c`.** First do the directory move with `git mv`:

```bash
cd .claude/worktrees/engine-rework
git mv GblChainloadPkg/Library/DynamicPatchLib/mode_1 \
       GblChainloadPkg/Library/DynamicPatchLib/abl_permissive
```

Then create `abl_permissive/libavb_force_success.c` by copying the patch10 portion (`ApplyAvbForceSuccess` fn + the `kMode1Patches[]` entry for patch10 turned into `kAblPermissiveLibavbPatches[]`). The header preamble + `#include` block goes in both files:

```c
/** @file libavb_force_success.c — patch10: libavb force-AVB-success.

    [copy the patch10 docstring from mode_1.c verbatim]
**/
#include "../../../Include/Library/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "../Internal/Arm64Decode.h"
#include "Signatures.h"

STATIC PATCH_OUTCOME
ApplyAvbForceSuccess (IN OUT UINT8 *Buf, IN UINT32 Size)
{
  /* body verbatim from mode_1.c */
}

CONST PATCH_DESC kAblPermissiveLibavbPatches[] = {
  { .Name = "patch10-libavb-force-avb-success",
    .Scope = SCOPE_ABL_PERMISSIVE,     /* enum rename lands in Task 6 */
    .Mandatory = TRUE,
    .Apply = ApplyAvbForceSuccess },
};
CONST UINTN kAblPermissiveLibavbPatchesCount =
  sizeof (kAblPermissiveLibavbPatches) / sizeof (kAblPermissiveLibavbPatches[0]);
```

Same shape for `fastboot_lock_gates.c` with `ApplyLockStateFastbootGate` + `RewriteOneLockStateGate` and `kAblPermissiveFastbootGatePatches[]`.

Delete the original `mode_1/mode_1.c` (now `abl_permissive/mode_1.c` after rename) by `git rm` (after staging the two new files). The end state: `abl_permissive/{libavb_force_success.c, fastboot_lock_gates.c, Signatures.h}`.

(Note: `SCOPE_ABL_PERMISSIVE` is the post-rename enum. Until Task 6 renames the enum, use the current `SCOPE_MODE_1`; PatchScope.h's enum is renamed there. Mark this with a TODO in your local notes if the order matters; otherwise apply Task 5 + Task 6 back-to-back.)

- [ ] **Step 2: Rename OEM directory.**

```bash
git mv GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c \
       GblChainloadPkg/Library/DynamicPatchLib/oem/oplus/bypass_warning.c
git mv GblChainloadPkg/Library/DynamicPatchLib/oem/Signatures.h \
       GblChainloadPkg/Library/DynamicPatchLib/oem/oplus/Signatures.h
```

(The `mkdir -p` happens implicitly via `git mv`; if not, create `oem/oplus/` first.)

Edit `oem/oplus/bypass_warning.c`:
- Top-of-file `@file` line: update to `bypass_warning.c — patch7: orange-state warning bypass`.
- Rename `kOemOneplusPatches` → `kOemOplusPatches` and `kOemOneplusPatchesCount` similarly.
- Rename `SCOPE_OEM_ONEPLUS` references → `SCOPE_OEM_OPLUS` (enum rename in Task 6).
- Update the `.Name` field: `"patch7-orange-screen"` → `"patch7-orange-screen"` (unchanged — `kPatches[]` name strings stay numbered per spec; only file/group renames apply in PR1).

Update `oem/oplus/Signatures.h`:
- Top `@file` block: change `DPL_OEM_ONEPLUS_CANOE_SIGNATURES_H_` guard to `DPL_OEM_OPLUS_SIGNATURES_H_`.
- Path comments mentioning `oneplus_canoe` → `oplus`.

- [ ] **Step 3: Rename universal/ to retired/.**

```bash
git mv GblChainloadPkg/Library/DynamicPatchLib/universal \
       GblChainloadPkg/Library/DynamicPatchLib/retired
git mv GblChainloadPkg/Library/DynamicPatchLib/retired/universal.c \
       GblChainloadPkg/Library/DynamicPatchLib/retired/block_efisp_recursion.c
```

Edit `retired/block_efisp_recursion.c`:
- Replace top-of-file comment with:

```c
/** @file block_efisp_recursion.c — RETIRED 2026-05-22.

    Originally patch1: the EFISP UTF-16 byte rewrite that stopped a
    second-stage ABL from re-loading us out of EFISP. Superseded by the
    `BlockIoHook` EFISP gate that returns EFI_NO_MEDIA on EFISP reads
    (see Task 9 in the engine-rework plan).

    Kept in tree as documentation / reference implementation of the
    UTF-16 byte-pattern-anchor style. NOT registered in any patch table
    after Task 10. Reverting to this patch would require re-adding its
    entry to `kAblPermissivePatches[]` (or a new universal table) and
    re-disabling the BlockIoHook EFISP gate.
**/
```

The `kUniversalPatches[]` and `kUniversalPatchesCount` declarations stay (still consumed by `PatchTable.c` in this task). Task 10 drops them from the active table.

`retired/Signatures.h`'s include guard becomes `DPL_RETIRED_SIGNATURES_H_`.

- [ ] **Step 4: Update `DynamicPatchLib.inf` `[Sources]`.** Replace the existing list:

```ini
[Sources]
  Internal/ScanLib.c
  Internal/PeSections.c
  Internal/Encode.c
  Internal/Arm64Decode.c
  Internal/PatchEngine.c
  PatchTable.c
  retired/block_efisp_recursion.c
  abl_permissive/libavb_force_success.c
  abl_permissive/fastboot_lock_gates.c
  # OEM patches are host-only — see PatchTable.c's __HOST_BUILD__ gate.
  # oem/oplus/bypass_warning.c is consumed by host tool Makefiles only.
```

- [ ] **Step 5: Update host tool Makefiles.** `tools/abl-patcher/Makefile` and `tools/gbl-pack/Makefile` (if they list DynamicPatchLib source files individually) must point at the new paths. Look for occurrences of:

```
DynamicPatchLib/mode_1/mode_1.c
DynamicPatchLib/oem/oneplus_canoe.c
DynamicPatchLib/universal/universal.c
```

Replace with the new paths (the two split abl_permissive files, the oplus path, and the retired path). The OEM source file remains compiled into host tools.

- [ ] **Step 6: Update `PatchTable.c`** to reference the new symbol names — this is preview-only for Task 5. The full enum + symbol rename happens in Task 6, but the file path references can update now:

```c
extern CONST PATCH_DESC kUniversalPatches[];        /* retired/block_efisp_recursion.c — drops in Task 10 */
extern CONST UINTN      kUniversalPatchesCount;
extern CONST PATCH_DESC kOemOplusPatches[];          /* was kOemOneplusPatches */
extern CONST UINTN      kOemOplusPatchesCount;

#if (GBL_MODE >= 1) || defined(__HOST_BUILD__)
extern CONST PATCH_DESC kAblPermissiveLibavbPatches[];    /* was kMode1Patches[] (half of it) */
extern CONST UINTN      kAblPermissiveLibavbPatchesCount;
extern CONST PATCH_DESC kAblPermissiveFastbootGatePatches[];
extern CONST UINTN      kAblPermissiveFastbootGatePatchesCount;
#endif
```

Update the `InitAggregate` body to copy from the two split arrays where `kMode1Patches` was iterated.

Update the `EnsureInitScoped` body (host-only) similarly. Rename param `include_mode1` → `include_abl_permissive` and rename `oem == GBL_OEM_ONEPLUS` → `oem == GBL_OEM_OPLUS` (these enum renames also happen in Task 6 — coordinate).

- [ ] **Step 7: Update tests that reference renamed paths or symbols.** `tests/host/088_patch7_multi_abl.sh` should pass without changes (it only invokes `abl-patcher` and asserts patch names — and `patch7-orange-screen` name string is preserved per spec). `tests/host/065_patch_sig_parity.sh` may reference signature filenames — adjust paths.

- [ ] **Step 8: Run all tests.** `bash tests/host/run-all.sh`. All pre-existing tests still pass (only paths changed, no behavior).

- [ ] **Step 9: Commit.**

```bash
git add -A GblChainloadPkg/Library/DynamicPatchLib/ \
           tools/abl-patcher/Makefile tools/gbl-pack/Makefile \
           tests/host/
git commit -m "DynamicPatchLib: rename to abl_permissive/oem/oplus/retired; split per-patch"
```

---

### Task 6: `PatchScope` enum rename + exclude OEM from firmware

**Goal:** Rename `GBL_OEM_ONEPLUS` → `GBL_OEM_OPLUS`, `SCOPE_MODE_1` → `SCOPE_ABL_PERMISSIVE`, `SCOPE_OEM_ONEPLUS` → `SCOPE_OEM_OPLUS`, and finish the `PatchTable.c` cleanup. Confirm OEM patches are NOT in the firmware build (the `[Sources]` change in Task 5 already does this; this task verifies and tightens the `#if defined(__HOST_BUILD__)` guards in `PatchTable.c`).

**Files:**
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/PatchScope.h`
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/PatchTable.c`
- Modify: `GblChainloadPkg/Include/Library/PatchDesc.h` (if `PATCH_SCOPE` enum lives there)
- Modify: `tools/abl-patcher/abl-patcher.c` (calls `EnsureInitScoped` with renamed enum/param)
- Modify: any other consumer (`tests/host/helpers/`, etc.)

**Acceptance Criteria:**
- [ ] `PatchScope.h` defines `GBL_OEM` enum: `GBL_OEM_NONE = 0, GBL_OEM_OPLUS = 1`. (Wire value `1` unchanged — only the symbol renames.)
- [ ] `EnsureInitScoped` signature: `void DynamicPatchLib_EnsureInitScoped(GBL_OEM oem, int include_abl_permissive)`.
- [ ] `SCOPE_ABL_PERMISSIVE` + `SCOPE_OEM_OPLUS` defined; old names removed.
- [ ] Firmware `PatchTable.c` `InitAggregate` no longer references OEM arrays. Wrapping `#if defined(__HOST_BUILD__)` around the OEM extern + the OEM copy loop in `EnsureInitScoped`.
- [ ] `abl-patcher` builds with the new enum + param names.
- [ ] `grep -rn 'GBL_OEM_ONEPLUS\|SCOPE_MODE_1\|SCOPE_OEM_ONEPLUS\|include_mode1' GblChainloadPkg tools tests scripts` returns no matches (deprecation alias for `--oem oneplus` CLI input is handled in Task 12 inside `abl-patcher.c`, not via a leftover enum value).

**Verify:** `make -s -C tools/abl-patcher && bash tests/host/083_abl_patcher_oem.sh` → both pass.

**Steps:**

- [ ] **Step 1: Update `PatchScope.h`.** Replace contents:

```c
/* PatchScope.h — runtime patch-scope selection.
   On-device firmware: uses DynamicPatchLib_EnsureInit() with the compile-
   time `__HOST_BUILD__`-free path (universal + abl_permissive only).
   Host tools: use EnsureInitScoped for runtime selection. */
#ifndef GBL_PATCH_SCOPE_H_
#define GBL_PATCH_SCOPE_H_

typedef enum { GBL_OEM_NONE = 0, GBL_OEM_OPLUS = 1 } GBL_OEM;

void DynamicPatchLib_EnsureInitScoped (GBL_OEM oem, int include_abl_permissive);

#endif
```

- [ ] **Step 2: Find + rename `SCOPE_*` enum values.** Locate `PATCH_SCOPE` (or whatever names hold `SCOPE_MODE_1` / `SCOPE_OEM_ONEPLUS` / `SCOPE_UNIVERSAL`) — most likely `GblChainloadPkg/Include/Library/PatchDesc.h`. Rename:

```c
typedef enum {
  SCOPE_ABL_PERMISSIVE = 1,
  SCOPE_OEM_OPLUS,
  /* SCOPE_UNIVERSAL retained for retired/block_efisp_recursion.c's
     existing PATCH_DESC entry only; not in any active table. */
  SCOPE_UNIVERSAL,
} PATCH_SCOPE;
```

(Order is illustrative — preserve original numeric values if any tests pin them.)

- [ ] **Step 3: Update `PatchTable.c`.** Final shape:

```c
extern CONST PATCH_DESC kAblPermissiveLibavbPatches[];
extern CONST UINTN      kAblPermissiveLibavbPatchesCount;
extern CONST PATCH_DESC kAblPermissiveFastbootGatePatches[];
extern CONST UINTN      kAblPermissiveFastbootGatePatchesCount;

#ifdef __HOST_BUILD__
extern CONST PATCH_DESC kOemOplusPatches[];
extern CONST UINTN      kOemOplusPatchesCount;
#endif

STATIC VOID InitAggregate (VOID) {
  UINTN n = 0, i;
  for (i = 0; i < kAblPermissiveLibavbPatchesCount && n < MAX_PATCHES; ++i)
    gAggregated[n++] = kAblPermissiveLibavbPatches[i];
  for (i = 0; i < kAblPermissiveFastbootGatePatchesCount && n < MAX_PATCHES; ++i)
    gAggregated[n++] = kAblPermissiveFastbootGatePatches[i];
  gAggregatedLen = n;
  gPatchTable    = gAggregated;
  gPatchTableLen = n;
  gAggregateInit = TRUE;
}

VOID DynamicPatchLib_EnsureInit (VOID) {
  if (!gAggregateInit) InitAggregate ();
}

#ifdef __HOST_BUILD__
void DynamicPatchLib_EnsureInitScoped (GBL_OEM oem, int include_abl_permissive) {
  UINTN n = 0, i;
  if (include_abl_permissive) {
    for (i = 0; i < kAblPermissiveLibavbPatchesCount && n < MAX_PATCHES; ++i)
      gAggregated[n++] = kAblPermissiveLibavbPatches[i];
    for (i = 0; i < kAblPermissiveFastbootGatePatchesCount && n < MAX_PATCHES; ++i)
      gAggregated[n++] = kAblPermissiveFastbootGatePatches[i];
  }
  if (oem == GBL_OEM_OPLUS) {
    for (i = 0; i < kOemOplusPatchesCount && n < MAX_PATCHES; ++i)
      gAggregated[n++] = kOemOplusPatches[i];
  }
  gAggregatedLen = n;
  gPatchTable    = gAggregated;
  gPatchTableLen = n;
  gAggregateInit = TRUE;
}
#endif
```

Note: `kUniversalPatches[]` is no longer referenced. Task 10 will drop the symbol declarations from `retired/block_efisp_recursion.c` since they're now orphaned.

- [ ] **Step 4: Update callers.** `tools/abl-patcher/abl-patcher.c` calls `EnsureInitScoped` — update enum + param. Also update the patch group .c files (`abl_permissive/libavb_force_success.c`, `abl_permissive/fastboot_lock_gates.c`, `oem/oplus/bypass_warning.c`) to use `SCOPE_ABL_PERMISSIVE` and `SCOPE_OEM_OPLUS` in their `kPatches[]` `.Scope` fields.

- [ ] **Step 5: Verify no stale references.** `grep -rn 'GBL_OEM_ONEPLUS\|SCOPE_MODE_1\|SCOPE_OEM_ONEPLUS\|include_mode1\|kMode1Patches\|kOemOneplusPatches' GblChainloadPkg tools tests scripts zip` — should return nothing.

- [ ] **Step 6: Build + run tests.** `make -s -C tools/abl-patcher` builds; `bash tests/host/083_abl_patcher_oem.sh` passes (still uses old `--no-mode1` CLI flag — that drops in Task 12; for now the flag still works because Task 12 will rewrite `abl-patcher.c`).

- [ ] **Step 7: Commit.**

```bash
git add GblChainloadPkg/Library/DynamicPatchLib/{PatchScope.h,PatchTable.c} \
        GblChainloadPkg/Include/Library/PatchDesc.h \
        GblChainloadPkg/Library/DynamicPatchLib/{abl_permissive,oem/oplus,retired}/*.c \
        tools/abl-patcher/abl-patcher.c
git commit -m "DynamicPatchLib: rename scope enum (SCOPE_ABL_PERMISSIVE/OPLUS); host-only OEM"
```

---

### Task 7: Rename `Mode1Overlay` → `FakelockOverlay`; `Mode2{Rewrite,Overlay}` → `Profile{Rewrite,Overlay}`

**Goal:** Pure C-side rename of the mutation helpers. Compile-time `#if (GBL_MODE == N)` gates are PRESERVED — they get swapped to runtime in Task 8. This task is a mechanical rename so that Task 8's diff is purely about gating, not naming.

**Files:**
- Move: `Mode1Overlay.{c,h}` → `FakelockOverlay.{c,h}`
- Move: `Mode2Rewrite.{c,h}` → `ProfileRewrite.{c,h}`
- Move: `Mode2Overlay.{c,h}` → `ProfileOverlay.{c,h}`
- Modify: `ProtocolHookLib.inf` `[Sources]`
- Modify: `VerifiedBootHook.c`, `QseecomHook.c`, `SpssHook.c`, `BootFlow.c` — symbol references
- Modify: callers' includes — `#include "Mode1Overlay.h"` etc.

**Acceptance Criteria:**
- [ ] Symbol renames (case-sensitive, exact-match):
  - `Mode1Policy_OnVbReadConfig_Post` → `FakelockOverlay_OnVbReadConfig_Post`
  - `Mode1Policy_OnVbDeviceInit_PrePost` → `FakelockOverlay_OnVbDeviceInit_PrePost`
  - `Mode1Policy_OnVbWriteConfig` → `FakelockOverlay_OnVbWriteConfig`
  - `Mode1Policy_OnVbReset` → `FakelockOverlay_OnVbReset`
  - `Mode1Policy_ShouldDropQseeOplusSec` → `FakelockOverlay_ShouldDropQseeOplusSec`
  - `Mode2_SetProfile` → `ProfileOverlay_SetProfile`
  - `Mode2Policy_RewriteKmSend` → `ProfileOverlay_RewriteKmSend`
  - `Mode2Policy_RewriteSpss` → `ProfileOverlay_RewriteSpss`
  - `gbl_m2_rewrite_km` → `gbl_profile_rewrite_km` (pure-logic side in ProfileRewrite.c)
  - `gbl_m2_rewrite_spss` → `gbl_profile_rewrite_spss`
  - `GBL_KM_*` constants unchanged (wire constants — no rename needed)
- [ ] `#include` paths updated everywhere.
- [ ] `grep -rn 'Mode1Overlay\|Mode2Overlay\|Mode2Rewrite\|Mode1Policy_\|Mode2Policy_\|Mode2_SetProfile\|gbl_m2_rewrite' GblChainloadPkg` returns no matches.
- [ ] Firmware build + host tests still pass.

**Verify:** `bash tests/host/078_mode2_rewrite.sh` passes (file probably gets renamed too — see Step 4). Firmware EDK2 build succeeds.

**Steps:**

- [ ] **Step 1: Rename the four C/header pairs.**

```bash
git mv GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c \
       GblChainloadPkg/Library/ProtocolHookLib/FakelockOverlay.c
git mv GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.h \
       GblChainloadPkg/Library/ProtocolHookLib/FakelockOverlay.h
git mv GblChainloadPkg/Library/ProtocolHookLib/Mode2Overlay.c \
       GblChainloadPkg/Library/ProtocolHookLib/ProfileOverlay.c
git mv GblChainloadPkg/Library/ProtocolHookLib/Mode2Overlay.h \
       GblChainloadPkg/Library/ProtocolHookLib/ProfileOverlay.h
git mv GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.c \
       GblChainloadPkg/Library/ProtocolHookLib/ProfileRewrite.c
git mv GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.h \
       GblChainloadPkg/Library/ProtocolHookLib/ProfileRewrite.h
```

- [ ] **Step 2: Update header guards + file-level comments** in each renamed file. E.g., `MODE1_OVERLAY_H_` → `FAKELOCK_OVERLAY_H_`, etc. The `@file` lines mention the file name — update.

- [ ] **Step 3: Symbol-rename inside the renamed files.** Use sed-style replacements. Example for FakelockOverlay.h:

```bash
sed -i \
  -e 's/Mode1Overlay/FakelockOverlay/g' \
  -e 's/Mode1Policy_/FakelockOverlay_/g' \
  GblChainloadPkg/Library/ProtocolHookLib/FakelockOverlay.{c,h}

sed -i \
  -e 's/Mode2Overlay/ProfileOverlay/g' \
  -e 's/Mode2Policy_/ProfileOverlay_/g' \
  -e 's/Mode2_SetProfile/ProfileOverlay_SetProfile/g' \
  GblChainloadPkg/Library/ProtocolHookLib/ProfileOverlay.{c,h}

sed -i \
  -e 's/Mode2Rewrite/ProfileRewrite/g' \
  -e 's/gbl_m2_rewrite_/gbl_profile_rewrite_/g' \
  GblChainloadPkg/Library/ProtocolHookLib/ProfileRewrite.{c,h}
```

- [ ] **Step 4: Update callers.** Files that include or reference the renamed symbols:
  - `VerifiedBootHook.c` — includes `Mode1Overlay.h` and calls `Mode1Policy_*`. Bulk-replace.
  - `QseecomHook.c` — calls `Mode1Policy_ShouldDropQseeOplusSec` AND `Mode2Policy_RewriteKmSend`. Both renames apply.
  - `SpssHook.c` — calls `Mode2Policy_RewriteSpss`. Rename.
  - `BootFlow.c` — references `Mode2Overlay.h` + `Mode2_SetProfile`. Rename to `ProfileOverlay.h` + `ProfileOverlay_SetProfile`.

```bash
for f in GblChainloadPkg/Library/ProtocolHookLib/{VerifiedBootHook,QseecomHook,SpssHook}.c \
         GblChainloadPkg/Application/GblChainload/BootFlow.c; do
  sed -i \
    -e 's/Mode1Overlay/FakelockOverlay/g' \
    -e 's/Mode1Policy_/FakelockOverlay_/g' \
    -e 's/Mode2Overlay/ProfileOverlay/g' \
    -e 's/Mode2Policy_/ProfileOverlay_/g' \
    -e 's/Mode2Rewrite/ProfileRewrite/g' \
    -e 's/Mode2_SetProfile/ProfileOverlay_SetProfile/g' \
    "$f"
done
```

- [ ] **Step 5: Update `ProtocolHookLib.inf`** `[Sources]` listing.

- [ ] **Step 6: Update host tests that reference Mode2Rewrite directly.** `tests/host/078_mode2_rewrite.sh` and any test driver under `tests/host/helpers/` that includes `Mode2Rewrite.h`. Consider renaming the test file too:

```bash
git mv tests/host/078_mode2_rewrite.sh tests/host/078_profile_rewrite.sh
```

(Optional; the file name doesn't have to match exactly, but matching makes find-by-grep easier later.)

- [ ] **Step 7: Verify no leftovers.** `grep -rn 'Mode1Overlay\|Mode2Overlay\|Mode2Rewrite\|Mode1Policy_\|Mode2Policy_\|Mode2_SetProfile\|gbl_m2_rewrite' GblChainloadPkg tools tests` returns 0 matches.

- [ ] **Step 8: Build + test.** Firmware build succeeds; `bash tests/host/078_profile_rewrite.sh` passes; full `bash tests/host/run-all.sh` passes.

- [ ] **Step 9: Commit.**

```bash
git add -A GblChainloadPkg tests/host
git commit -m "ProtocolHookLib: rename Mode1Overlay→FakelockOverlay, Mode2*→Profile*"
```

---

### Task 8: Replace compile-time `#if (GBL_MODE == N)` with runtime `gManifest` gates

**Goal:** Switch every per-hook + InstallAll mode gate from compile-time to runtime. `gManifest.WantFakelockHook` replaces `#if (GBL_MODE == 1)`; `gManifest.WantProfileSpoof` replaces `#if (GBL_MODE == 2)`; the mode-1-or-mode-2 disjunctions become `||`. Mutation helper headers (FakelockOverlay.h, ProfileOverlay.h) drop their `#if (GBL_MODE == N)` declaration guards — the functions are always declared and compiled now.

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/FakelockOverlay.h` (drop `#if (GBL_MODE == 1)` guard)
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ProfileOverlay.h` (drop `#if (GBL_MODE == 2)` guard if present)
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ProfileRewrite.h` (same)

**Acceptance Criteria:**
- [ ] `grep -rn 'GBL_MODE' GblChainloadPkg/Library/ProtocolHookLib GblChainloadPkg/Application` returns 0 matches.
- [ ] `InstallAll.c`'s required-status gates:
  - VerifiedBoot required when `gManifest.WantFakelockHook`.
  - Qseecom required when `gManifest.WantFakelockHook || gManifest.WantProfileSpoof`.
  - SPSS required when `gManifest.WantProfileSpoof`.
- [ ] Per-call mutation gates inside hook wrappers swap from `#if` to `if` (gManifest reads).
- [ ] Firmware builds for the legacy `-D GBL_MODE=0` invocation (still defined in DSC for now, drops in Task 11) — manifest unset → all mutations skipped — behavior identical to today's mode-0 build.
- [ ] Same for `GBL_MODE=1` (when a manifest with `WantFakelockHook=1` is packed) — behavior identical to today's mode-1.
- [ ] Same for `GBL_MODE=2` (when a manifest with `WantProfileSpoof=1` is packed).

**Verify:** Three firmware builds with three differently-packed overlays produce three different runtime behaviors; the existing `088_patch7_multi_abl.sh` and `078_profile_rewrite.sh` host tests still pass.

**Steps:**

- [ ] **Step 1: `InstallAll.c`.** Replace contents starting from the `#include <Library/ProtocolHookLib.h>` line. Final body:

```c
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/GblLog.h>

#include <Library/ProtocolHookLib.h>
#include <Library/GblPayloadLib.h>   /* for gManifest */

#include "HookCommon.h"

EFI_STATUS EFIAPI
ProtocolHook_InstallAll (OUT HOOK_INSTALL_RESULT *Result) {
  EFI_STATUS Status;
  BOOLEAN    WantVb   = gManifest.WantFakelockHook;
  BOOLEAN    WantQsee = gManifest.WantFakelockHook || gManifest.WantProfileSpoof;
  BOOLEAN    WantSpss = gManifest.WantProfileSpoof;

  if (Result == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem (Result, sizeof (*Result));

  /* 1. VerifiedBoot — required iff fakelock hook requested. */
  Status = InstallVerifiedBootHook ();
  if (EFI_ERROR (Status)) {
    if (WantVb) {
      Print (L"ProtocolHookLib: FATAL — VerifiedBoot install failed (%r), aborting\n", Status);
      return Status;
    }
    Print (L"ProtocolHookLib: VerifiedBoot install failed (%r) — continuing (observation-only)\n", Status);
  } else {
    Result->VbInstalledSlots = 1;
  }
  Result->VbExpectedSlots = 1;

  /* 2. SCM — always required (safety hook). */
  Status = InstallScmHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: FATAL — SCM install failed (%r), aborting\n", Status);
    return Status;
  }
  Result->ScmInstalledSlots = Result->ScmExpectedSlots = 1;

  /* 3. Qseecom — required for fakelock OR profile spoof. */
  Status = InstallQseecomHook ();
  if (EFI_ERROR (Status)) {
    if (WantQsee) {
      Print (L"ProtocolHookLib: FATAL — Qseecom install failed (%r), aborting\n", Status);
      return Status;
    }
    Print (L"ProtocolHookLib: Qseecom install failed (%r) — continuing (observation-only)\n", Status);
  } else {
    Result->QseecomInstalledSlots = 1;
  }
  Result->QseecomExpectedSlots = 1;

  /* 4. SPSS — required for profile spoof. */
  Status = InstallSpssHook ();
  if (EFI_ERROR (Status)) {
    if (WantSpss) {
      Print (L"ProtocolHookLib: FATAL — SPSS install failed (%r), aborting\n", Status);
      return Status;
    }
    Print (L"ProtocolHookLib: SPSS install failed (%r) — continuing (observation-only)\n", Status);
  } else {
    Result->SpssInstalledSlots = 1;
  }
  Result->SpssExpectedSlots = 1;

  /* 5. BlockIo — always required (safety hook). */
  Status = InstallBlockIoHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: FATAL — BlockIo install failed (%r), aborting\n", Status);
    return Status;
  }
  Result->BlockIoInstalledSlots = Result->BlockIoExpectedSlots = 1;

  Result->UniversalRequiredOk =
    (Result->ScmInstalledSlots > 0 && Result->BlockIoInstalledSlots > 0);
  if (!Result->UniversalRequiredOk) {
    Print (L"ProtocolHookLib: FATAL — universal baseline incomplete\n");
    return EFI_NOT_READY;
  }
  Result->ModeOverlayOk = TRUE;

  GBL_INFO (
    "ProtocolHookLib: installed (fakelock=%u profile_spoof=%u "
    "vb=%u/%u scm=%u/%u qsee=%u/%u spss=%u/%u blockio=%u/%u)\n",
    (UINT32)gManifest.WantFakelockHook, (UINT32)gManifest.WantProfileSpoof,
    Result->VbInstalledSlots,      Result->VbExpectedSlots,
    Result->ScmInstalledSlots,     Result->ScmExpectedSlots,
    Result->QseecomInstalledSlots, Result->QseecomExpectedSlots,
    Result->SpssInstalledSlots,    Result->SpssExpectedSlots,
    Result->BlockIoInstalledSlots, Result->BlockIoExpectedSlots);
  return EFI_SUCCESS;
}
```

- [ ] **Step 2: `VerifiedBootHook.c`.** For each of the 14 `#if (GBL_MODE == 1) ... #endif` blocks (at lines 131, 141, 160, 196, 201, 208, 214, 339, 348, 485, 508, 517, 544, 573 per the earlier grep), replace with `if (gManifest.WantFakelockHook) { ... }`. Add `#include <Library/GblPayloadLib.h>` at the top for `gManifest`. Read each block first to determine whether the `#if` wraps a statement vs. a decl; statements use `if (...) {}`, declarations are simply unguarded now.

For declarations that were `#if`-guarded (e.g., the `Mode1Policy_*` extern decls), drop the guard — `FakelockOverlay.h` now declares them unconditionally (per Step 4 below).

- [ ] **Step 3: `QseecomHook.c`.** Two `#if` blocks (line 511 = mode-1, line 523 = mode-2). Replace:

```c
#if (GBL_MODE == 1)
    if (FakelockOverlay_ShouldDropQseeOplusSec (CmdId, &FakeStatus)) { ... }
#endif
```

becomes:

```c
    if (gManifest.WantFakelockHook &&
        FakelockOverlay_ShouldDropQseeOplusSec (CmdId, &FakeStatus)) { ... }
```

And:

```c
#if (GBL_MODE == 2)
    ProfileOverlay_RewriteKmSend (CmdId, SendBuf, SendLen);
#endif
```

becomes:

```c
    if (gManifest.WantProfileSpoof) {
      ProfileOverlay_RewriteKmSend (CmdId, SendBuf, SendLen);
    }
```

Add `#include <Library/GblPayloadLib.h>` at the top.

- [ ] **Step 4: `SpssHook.c`.** Single `#if (GBL_MODE == 2)` block at line 87. Replace with `if (gManifest.WantProfileSpoof) { ProfileOverlay_RewriteSpss (...); }`. Add include.

- [ ] **Step 5: Drop the `#if (GBL_MODE == 1)` declaration guard in `FakelockOverlay.h`.** All five `FakelockOverlay_*` declarations are unconditional now.

Same for `ProfileOverlay.h` if it has a `#if (GBL_MODE == 2)` guard.

- [ ] **Step 6: Build for `GBL_MODE=0`, `=1`, `=2`.** Each succeeds. Behavior is determined by the packed manifest now (Tasks 11/13/14 finish the build-system collapse so all three are the same binary).

- [ ] **Step 7: Commit.**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/*.{c,h}
git commit -m "ProtocolHookLib: gate hooks on runtime gManifest instead of GBL_MODE"
```

---

### Task 9: `BlockIoHook` EFISP gate — return `EFI_NO_MEDIA`

**Goal:** Add an `IsEfisp` flag to `BLOCK_IO_HOOK_RECORD`, detect the EFISP partition by GPT name at install time, and short-circuit `HookedReadBlocks` / `HookedWriteBlocks` with `EFI_NO_MEDIA` on those records. This makes `patch1` redundant for its core purpose (stopping a second-stage ABL from re-loading us out of EFISP).

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/BlockIoHook.c`

**Acceptance Criteria:**
- [ ] `BLOCK_IO_HOOK_RECORD` has a new `BOOLEAN IsEfisp;` field.
- [ ] At install time, partitions whose GPT name (case-insensitive) matches `efisp` get `IsEfisp = TRUE` on their record.
- [ ] `HookedReadBlocks` / `HookedWriteBlocks` short-circuit on EFISP records with `GBL_INFO ("blockio: refused %a on EFISP (lba=0x%Lx)", "read"/"write", Lba)` and return `EFI_NO_MEDIA`.
- [ ] Other partitions (oplusreserve1 logic unchanged) behave as today.
- [ ] On-device test (T16): second-stage patched ABL boots cleanly without recursing through EFISP.

**Verify:** EDK2 firmware build succeeds; manual on-device confirmation is part of T16.

**Steps:**

- [ ] **Step 1: Read `BlockIoHook.c` around the record struct definition and the install loop** to find the GPT-name-match site. The existing `IsOplusReservePartitionName` pattern (line 88) is the template; `PartitionNameMatches (Name, L"efisp")` is the predicate.

- [ ] **Step 2: Add `IsEfisp` field** to `BLOCK_IO_HOOK_RECORD` (the struct lives near the top of the file; if it lives in a header, edit there).

- [ ] **Step 3: At install time** (around line 369 where `BlockIo->ReadBlocks = HookedReadBlocks`), set `IsEfisp`:

```c
      Record->IsEfisp = PartitionNameMatches (Name, L"efisp");
```

- [ ] **Step 4: Short-circuit reads/writes.** In `HookedReadBlocks` (around line 222) add at the top of the function, after `FindRecordByBlockIo`:

```c
  if (Record != NULL && Record->IsEfisp) {
    GBL_INFO ("blockio: refused read on EFISP (lba=0x%Lx, bytes=%u)\n",
              (UINT64)Lba, (UINT32)BufferSize);
    return EFI_NO_MEDIA;
  }
```

Same in `HookedWriteBlocks`:

```c
  if (Record != NULL && Record->IsEfisp) {
    GBL_INFO ("blockio: refused write on EFISP (lba=0x%Lx, bytes=%u)\n",
              (UINT64)Lba, (UINT32)BufferSize);
    return EFI_NO_MEDIA;
  }
```

- [ ] **Step 5: Build the firmware.** Compiles cleanly. Stage + boot-efi on a test device (this is partially T16's job — verify here that the build is sane; actual on-device validation in T16).

- [ ] **Step 6: Commit.**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/BlockIoHook.c
git commit -m "BlockIoHook: refuse EFISP reads/writes (replaces patch1 recursion guard)"
```

---

### Task 10: Retire patch1 + drop `PatchEngine.c` post-patch efisp invariant scan

**Goal:** Drop the patch1 entry from `kUniversalPatches[]` (array becomes empty), drop the post-patch efisp invariant scan in `PatchEngine.c`, downgrade `efisp_scan` in `gbl-pack` from rejection to a warning, and delete the now-orphaned test `062_efisp_scan_gate.sh`.

**Files:**
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/retired/block_efisp_recursion.c` (drop `kUniversalPatches[]` array — the patch fn stays as documentation)
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/Internal/PatchEngine.c` (drop lines 101-142 efisp invariant scan, per engine-rework spec §6)
- Modify: `tools/gbl-pack/pack.c` (`GBL_PACK_ERR_EFISP_PRESENT` → stderr warning, no error)
- Delete: `tests/host/062_efisp_scan_gate.sh`
- Delete: `tools/shared/efisp_scan.h` if no callers remain (verify via grep)

**Acceptance Criteria:**
- [ ] `retired/block_efisp_recursion.c` does NOT declare `kUniversalPatches[]` or `kUniversalPatchesCount` — the apply fn is `STATIC` (or wholly unused) for documentation only.
- [ ] `PatchTable.c` no longer references `kUniversalPatches` (it stopped in Task 6 already; this task confirms the dangling symbols are gone).
- [ ] `PatchEngine.c` no longer scans for the efisp UTF-16 pattern after applying patches.
- [ ] `gbl-pack` with a cached_abl that contains the efisp UTF-16 pattern PRINTS `gbl-pack: warning: cached_abl still contains UTF-16 "efisp" — BlockIoHook gate will handle this, but check that patch10/patch6 applied as expected` to stderr but EXITS 0 (no longer fails).
- [ ] `tests/host/062_efisp_scan_gate.sh` no longer exists.

**Verify:** `bash tests/host/run-all.sh` exits 0; no `062_efisp_scan_gate` in test output.

**Steps:**

- [ ] **Step 1: Drop the patch-table entries in `retired/block_efisp_recursion.c`.** Replace the bottom of the file (the `CONST PATCH_DESC kUniversalPatches[] = { ... };` block + its count) with:

```c
/* RETIRED — kUniversalPatches[] intentionally omitted. The patch fn above
   is preserved as a reference implementation only; the BlockIoHook EFISP
   gate (Task 9) supersedes it operationally. */
```

The `STATIC PATCH_OUTCOME ApplyEfispRecursion(...)` body stays, but mark it `STATIC` if it isn't already, to silence unused-function warnings via a void-cast at end of file:

```c
/* Pin the symbol for documentation continuity; suppresses unused-static warnings. */
(void)ApplyEfispRecursion;
```

Actually — for unused static fns, the cleanest path is to keep the function in scope without referencing it. If the compiler complains, mark `__attribute__((unused))` on the fn:

```c
STATIC __attribute__((unused)) PATCH_OUTCOME ApplyEfispRecursion(...) { ... }
```

- [ ] **Step 2: Remove the source from `DynamicPatchLib.inf`** if its sole purpose was documentation:

Actually keep it in `[Sources]` — the file still wants to compile cleanly so the documentation stays compileable. Otherwise it bit-rots.

- [ ] **Step 3: Drop the post-patch efisp invariant scan in `PatchEngine.c`.** Read `PatchEngine.c:101-142` and delete the scan block. Replace with a one-line comment:

```c
/* Post-patch efisp invariant scan retired — BlockIoHook EFISP gate is now
   the operational guarantee (see GblChainloadPkg/Library/ProtocolHookLib/
   BlockIoHook.c). */
```

- [ ] **Step 4: Downgrade `gbl-pack` efisp check.** In `tools/gbl-pack/pack.c`, the existing block:

```c
if (gbl_contains_utf16_efisp(in->cached_abl, in->cached_abl_size))
    return GBL_PACK_ERR_EFISP_PRESENT;
```

The pure-logic pack lib can't print to stderr — instead, return a new status `GBL_PACK_WARN_EFISP_PRESENT` (or repurpose the existing one) and have the CLI in `gbl-pack.c` print + continue. Actually simpler: pull the check out of `pack.c` into the CLI in `gbl-pack.c`:

In `pack.c`, delete the `gbl_contains_utf16_efisp` check entirely. Also delete the `GBL_PACK_ERR_EFISP_PRESENT` enum value (or repurpose for the warning).

In `gbl-pack.c`, after slurping `cached`, add:

```c
#include "../shared/efisp_scan.h"
...
if (cached) {
    if (gbl_contains_utf16_efisp((const uint8_t *)in.cached_abl, in.cached_abl_size)) {
        fprintf(stderr,
            "gbl-pack: warning: cached_abl still contains UTF-16 \"efisp\" — "
            "BlockIoHook gate will handle this, but check that patch10/patch6 "
            "applied as expected\n");
    }
}
```

(This keeps `tools/shared/efisp_scan.h` alive as long as the CLI still wants the warning. The header can be retired in PR2 when the Rust port replaces it.)

- [ ] **Step 5: Delete the test.**

```bash
git rm tests/host/062_efisp_scan_gate.sh
```

- [ ] **Step 6: Verify firmware + host tests.** `bash tests/host/run-all.sh` passes; `make -s -C tools/gbl-pack` builds.

- [ ] **Step 7: Commit.**

```bash
git add -A GblChainloadPkg/Library/DynamicPatchLib/retired \
           GblChainloadPkg/Library/DynamicPatchLib/Internal/PatchEngine.c \
           tools/gbl-pack/{pack.c,gbl-pack.c}
git rm tests/host/062_efisp_scan_gate.sh
git commit -m "retire patch1: drop universal patch entry + post-patch scan; warn-only in gbl-pack"
```

---

### Task 11: Drop `GBL_MODE` compile-time flag — collapse to single EFI

**Goal:** Eliminate `GBL_MODE` from the build entirely. `GblChainloadPkg.dsc` no longer DEFINEs it; `[BuildOptions]` no longer emits `-DGBL_MODE=$(GBL_MODE)`; `scripts/build*.sh` collapse the per-mode build loop to a single invocation producing `dist/gbl-chainload.efi`.

**Files:**
- Modify: `GblChainloadPkg/GblChainloadPkg.dsc`
- Modify: `GblChainloadPkg/Application/GblChainload/GblChainload.inf`
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/DynamicPatchLib.inf`
- Modify: `scripts/build.sh`
- Modify: `scripts/build-inside-docker.sh`
- Modify: any other build script under `scripts/` that iterates per-mode (`build-cross-tools.sh`, `build-recovery-tools.sh` if they have a mode loop)
- Modify: `dist/` layout — produce `gbl-chainload.efi` instead of `mode-{0,1,2}.efi`
- Modify: GitHub Actions / release workflow if applicable (search `.github/workflows/`)

**Acceptance Criteria:**
- [ ] `grep -rn 'GBL_MODE\|mode-0.efi\|mode-1.efi\|mode-2.efi' GblChainloadPkg scripts .github 2>/dev/null` returns no matches (excluding string literals in `tools/README.md` that get cleaned in Task 12 / docs).
- [ ] `scripts/build.sh` builds one EFI and emits it to `dist/gbl-chainload.efi`.
- [ ] `GblChainloadPkg.dsc` `[Defines]` no longer has `GBL_MODE`, `GBL_AUTO`, etc. — or, if some survive for legitimate reasons (verbose/debug knobs), they stay. `GBL_MODE` specifically is gone.
- [ ] `GblChainloadPkg.dsc` `[BuildOptions]` removes the `-DGBL_MODE=...` line.
- [ ] On-device boot from the single EFI still works (validation lives in T16).

**Verify:** `bash scripts/build.sh` produces `dist/gbl-chainload.efi` without errors. Old mode-{0,1,2}.efi artifacts are absent (CI/release packaging may need a follow-up Task 14 adjustment).

**Steps:**

- [ ] **Step 1: Read `scripts/build.sh`** to find the per-mode loop. Replace it with a single build invocation passing no GBL_MODE.

- [ ] **Step 2: Edit `GblChainloadPkg.dsc`** — remove `DEFINE GBL_MODE = 1` (line 63) and the `-DGBL_MODE=$(GBL_MODE)` line in BuildOptions (line 156). Keep `GBL_DEBUG`, `GBL_VERBOSE`, `GBL_BUILD_NAME` if they have other consumers. Update `GBL_BUILD_NAME` default to `gbl-chainload` (drop `mode-unknown`).

- [ ] **Step 3: Edit `scripts/build-inside-docker.sh`** — remove `GBL_MODE` env var, remove `-D GBL_MODE="$GBL_MODE"` from build args, update the log banner.

- [ ] **Step 4: Edit `DynamicPatchLib.inf` `[BuildOptions]`** — remove `*_*_AARCH64_CC_FLAGS = -DGBL_MODE=$(GBL_MODE)`.

- [ ] **Step 5: Remove `#ifndef GBL_MODE / #error` from `BootFlow.c`** — already done in Task 2; verify.

- [ ] **Step 6: Remove `#if (GBL_MODE >= 1)` from `PatchTable.c`** — the unconditional include path now applies to all builds. The `#ifdef __HOST_BUILD__` guards stay (they govern OEM patches).

- [ ] **Step 7: Update output filenames.** `scripts/build.sh` or `scripts/build-inside-docker.sh` copies the EDK2 build output (typically `Build/GblChainloadPkg/RELEASE_GCC.../AARCH64/GblChainload.efi`) to `dist/gbl-chainload.efi`. Adjust accordingly.

- [ ] **Step 8: Search GitHub Actions** for mode iteration. Likely places: `.github/workflows/release.yml`, `.github/workflows/ci.yml`. Collapse any `matrix.mode: [0, 1, 2]` to single artifact emission.

- [ ] **Step 9: Run a clean build.** `bash scripts/build.sh` (inside docker) produces `dist/gbl-chainload.efi`. No GBL_MODE references remain in build output paths.

- [ ] **Step 10: Commit.**

```bash
git add -A GblChainloadPkg/GblChainloadPkg.dsc \
           GblChainloadPkg/Application/GblChainload/GblChainload.inf \
           GblChainloadPkg/Library/DynamicPatchLib/DynamicPatchLib.inf \
           GblChainloadPkg/Library/DynamicPatchLib/PatchTable.c \
           scripts/build*.sh \
           .github/workflows/
git commit -m "build: drop GBL_MODE compile flag; one dist/gbl-chainload.efi"
```

---

### Task 12: `abl-patcher` — drop `--no-mode1`; `--oem oplus` canonical with deprecation alias

**Goal:** Always-apply abl_permissive at host packing. Drop `--no-mode1` / `--no-libavb-bypass` entirely. `--oem oplus` is canonical; `--oem oneplus` accepts with a one-line stderr deprecation note for one release.

**Files:**
- Modify: `tools/abl-patcher/abl-patcher.c`
- Modify: `tools/abl-patcher/Makefile` (drop `-DGBL_MODE=1` from CFLAGS)
- Modify: `tests/host/083_abl_patcher_oem.sh`

**Acceptance Criteria:**
- [ ] `abl-patcher --help` (or no-args usage) does not mention `--no-mode1` or `--no-libavb-bypass`.
- [ ] `abl-patcher --in <pe> --out <out>` always applies abl_permissive (`include_abl_permissive=1` to `EnsureInitScoped`).
- [ ] `abl-patcher --oem oplus ...` succeeds; `abl-patcher --oem oneplus ...` succeeds AND prints `abl-patcher: --oem oneplus is deprecated; use --oem oplus (accepted for compatibility, will be removed in a future release)` to stderr.
- [ ] `tests/host/083_abl_patcher_oem.sh` updated: drops `--no-mode1` cases (flag is gone), keeps `--oem` cases; adds an `--oem oplus` happy path and an `--oem oneplus` deprecation-message assertion.

**Verify:** `bash tests/host/083_abl_patcher_oem.sh` passes.

**Steps:**

- [ ] **Step 1: Update test first.** `tests/host/083_abl_patcher_oem.sh`:

```bash
# ---- Test 1: --oem oplus (canonical) ---------------------------------------
"$PATCHER" --in "$PE" --oem oplus --out "$OUT/oplus.efi" >"$OUT/oplus.log" 2>&1 \
    || { echo "FAIL: --oem oplus returned non-zero"; cat "$OUT/oplus.log"; exit 1; }
grep -q "patch7-orange-screen .*OK" "$OUT/oplus.log" \
    || { echo "FAIL: patch7 did not apply under --oem oplus"; exit 1; }

# ---- Test 2: --oem oneplus (deprecation alias) -----------------------------
"$PATCHER" --in "$PE" --oem oneplus --out "$OUT/oneplus.efi" >"$OUT/oneplus.log" 2>&1 \
    || { echo "FAIL: --oem oneplus returned non-zero"; cat "$OUT/oneplus.log"; exit 1; }
grep -q "deprecated; use --oem oplus" "$OUT/oneplus.log" \
    || { echo "FAIL: deprecation message missing"; cat "$OUT/oneplus.log"; exit 1; }
grep -q "patch7-orange-screen .*OK" "$OUT/oneplus.log" \
    || { echo "FAIL: patch7 did not apply under --oem oneplus alias"; exit 1; }

# ---- Test 3: plain invocation applies abl_permissive ------------------------
"$PATCHER" --in "$PE" --out "$OUT/plain.efi" >"$OUT/plain.log" 2>&1
grep -q "patch10-libavb-force-avb-success .*OK" "$OUT/plain.log" \
    || { echo "FAIL: patch10 missing from plain run"; exit 1; }
grep -q "patch6-lock-state-fastboot-gate .*OK" "$OUT/plain.log" \
    || { echo "FAIL: patch6 missing from plain run"; exit 1; }

# ---- Test 4: bad --oem still rejected --------------------------------------
"$PATCHER" --in "$PE" --oem bad --out "$OUT/bad.efi" >"$OUT/bad.log" 2>&1 && \
    { echo "FAIL: --oem bad accepted"; exit 1; }
grep -q "unknown --oem" "$OUT/bad.log" \
    || { echo "FAIL: unknown-oem error missing"; cat "$OUT/bad.log"; exit 1; }
echo "  ok: abl-patcher --oem behavior verified"
```

- [ ] **Step 2: Run the test — verify it fails.** Today's binary still has `--no-mode1`; the new test cases for `--oem oplus` and the deprecation message will fail.

- [ ] **Step 3: Rewrite `abl-patcher.c`'s argv handling.** Delete `--no-mode1` parsing entirely. Add `--oem oplus` and `--oem oneplus` (alias):

```c
int main (int argc, char **argv)
{
  /* ... existing input/output handling ... */
  GBL_OEM Oem = GBL_OEM_NONE;
  /* --no-mode1 is removed — abl_permissive is always applied. */

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--oem") && i + 1 < argc) {
      const char *v = argv[++i];
      if (!strcmp(v, "oplus")) {
        Oem = GBL_OEM_OPLUS;
      } else if (!strcmp(v, "oneplus")) {
        fprintf(stderr,
          "abl-patcher: --oem oneplus is deprecated; use --oem oplus "
          "(accepted for compatibility, will be removed in a future release)\n");
        Oem = GBL_OEM_OPLUS;
      } else if (!strcmp(v, "none")) {
        Oem = GBL_OEM_NONE;
      } else {
        fprintf(stderr, "abl-patcher: unknown --oem '%s'\n", v);
        return 2;
      }
    }
    /* ... existing --in/--out handling ... */
  }

  DynamicPatchLib_EnsureInitScoped (Oem, /*include_abl_permissive=*/1);
  /* ... existing apply-and-write ... */
}
```

Update the usage string at the top of the file to drop `--no-mode1`.

- [ ] **Step 4: Drop `-DGBL_MODE=1` from `tools/abl-patcher/Makefile`** CFLAGS (lines 6, 30, 60 per the earlier grep). The host build no longer needs it — patch group selection is via `EnsureInitScoped` only.

- [ ] **Step 5: Run the test to verify it passes.** `bash tests/host/083_abl_patcher_oem.sh` → ok.

- [ ] **Step 6: Commit.**

```bash
git add tools/abl-patcher/{abl-patcher.c,Makefile} tests/host/083_abl_patcher_oem.sh
git commit -m "abl-patcher: drop --no-mode1; --oem oplus canonical, oneplus deprecation alias"
```

---

### Task 13: `efisp-package.py` — single base EFI, `--manifest`, decouple `--oem`

**Goal:** Switch the host packaging script to the single-EFI world: `--efi` points at `gbl-chainload.efi`; `--oem` is allowed in any mode (not gated to mode-2); `gbl-pack --manifest <bits>` is derived from `--mode`.

**Files:**
- Modify: `scripts/efisp-package.py`
- Modify: `tests/host/085_efisp_package.sh`

**Acceptance Criteria:**
- [ ] `efisp-package.py --mode 0 --oem oplus ...` succeeds (today this aborts with "--oem only valid for --mode 2").
- [ ] `efisp-package.py --mode N ...` invokes `gbl-pack --manifest BITS` where BITS is `0x00` for N=0, `0x01` for N=1, `0x02` for N=2.
- [ ] No `--no-mode1` flag is passed to `abl-patcher` (Task 12 removed it).
- [ ] `--efi` argument expects a path; default name conventions for `mode-N.efi` no longer apply — `gbl-chainload.efi` works for every mode.
- [ ] `tests/host/085_efisp_package.sh` updated for the new behavior.

**Verify:** `bash tests/host/085_efisp_package.sh` passes.

**Steps:**

- [ ] **Step 1: Edit `efisp-package.py`.** Replace the relevant blocks:

Around line 123–130 (the mode-2-only gate on `--oem`):

```python
    if args.mode == "2":
        if not args.stock_vbmeta:
            die("--mode 2 requires --stock-vbmeta")
        if not os.path.isfile(args.stock_vbmeta):
            die(f"input not found: {args.stock_vbmeta}")
    elif args.stock_vbmeta:
        die("--stock-vbmeta is only valid for --mode 2")
    # --oem is allowed in any mode now; abl-patcher always applies abl_permissive.
```

Replace lines 152–157 (`patch_argv` composition):

```python
        # 2. patch — abl_permissive always applies; oem optional.
        patch_argv = [patch, "--in", extracted, "--out", patched]
        if args.oem:
            patch_argv += ["--oem", args.oem]
        run(patch_argv, "abl-patcher")
```

Add manifest derivation + injection into the gbl-pack invocation:

```python
        # Derive manifest bits from --mode.
        manifest_bits = {"0": "0x00", "1": "0x01", "2": "0x02"}[args.mode]

        # 4. pack the GBLP1 overlay (including the manifest entry).
        run([pack, "--cached-abl", patched, "--source", args.abl,
             "--extracted", extracted, *pack_extra,
             "--manifest", manifest_bits,
             "--out", payload],
            "gbl-pack")
```

- [ ] **Step 2: Update test.** `tests/host/085_efisp_package.sh` likely asserts argv shape. Replace `--no-mode1` assertions with `--manifest 0x0N` assertions and add a `--mode 0 --oem oplus` case (which today would abort).

- [ ] **Step 3: Run the test.** Passes.

- [ ] **Step 4: Commit.**

```bash
git add scripts/efisp-package.py tests/host/085_efisp_package.sh
git commit -m "efisp-package.py: decouple --oem from --mode; emit --manifest bits"
```

---

### Task 14: install scripts — single base EFI, `M_MANIFEST_BITS`, `detect_oem` moved

**Goal:** `zip/modes/mode-{0,1,2}-install.sh` stop selecting per-mode base EFIs. `zip/modes/install-common.sh`'s `build_payload` derives manifest bits from a `M_MANIFEST_BITS` per-mode parameter and passes them to `gbl-pack`. `detect_oem` (today in mode-2-install.sh) moves into install-common.sh so it can be invoked by any mode that needs it (mode-2 still its sole caller).

**Files:**
- Modify: `zip/modes/install-common.sh`
- Modify: `zip/modes/mode-0-install.sh`
- Modify: `zip/modes/mode-1-install.sh`
- Modify: `zip/modes/mode-2-install.sh`
- Modify: `zip/update-tools.sh` (collapse mode-N.efi artifact references)
- Modify: `tests/host/071_zip_assembly.sh`, `tests/host/073_install_assembly.sh` if they grep for `mode-N.efi`

**Acceptance Criteria:**
- [ ] Each `mode-{0,1,2}-install.sh` sets `M_EFI=gbl-chainload.efi` and `M_MANIFEST_BITS=0xNN` per its mode (0x00 / 0x01 / 0x02). `M_PATCHER_ARGS` for mode-0 = empty (no `--no-mode1`); mode-1 = empty; mode-2 = `--oem $OEM_ID`.
- [ ] `install-common.sh::build_payload` adds `--manifest "$M_MANIFEST_BITS"` to the `gbl-pack` invocation.
- [ ] `detect_oem` is defined in `install-common.sh` (the function body moves verbatim from `mode-2-install.sh`); also normalize `OEM_ID` to `oplus` (was `oneplus` previously).
- [ ] `mode-2-install.sh::mode_prepare` calls `detect_oem` via the shared definition (deletes the local copy).
- [ ] `zip/update-tools.sh` lists `gbl-chainload.efi` as the single base EFI instead of three per-mode files.
- [ ] Existing zip-assembly tests pass.

**Verify:** `bash tests/host/071_zip_assembly.sh && bash tests/host/073_install_assembly.sh` both pass.

**Steps:**

- [ ] **Step 1: Move `detect_oem` to `install-common.sh`.** Cut from `mode-2-install.sh` (the body between `# detect_oem ...` and the closing brace), paste into `install-common.sh` above `# mode_preflight`. Update the OEM mapping:

```sh
detect_oem() {
  # ... existing logic ...
  case "$_mfr" in
    *oneplus*|*oppo*|*oplus*|*realme*) OEM_ID=oplus ;;
    *) abort "unsupported OEM (manufacturer='$_mfr')" ;;
  esac
  ui_print "[*] OEM detected: $OEM_ID (manufacturer=$_mfr)"
}
```

- [ ] **Step 2: Update each mode-N-install.sh.**

`mode-0-install.sh`:
```sh
. "$WORKDIR/modes/install-common.sh"

M_EFI=gbl-chainload.efi
M_LABEL=mode-0-install
M_PATCHER_ARGS=""
M_PACK_ARGS=""
M_MANIFEST_BITS=0x00
```

`mode-1-install.sh`:
```sh
. "$WORKDIR/modes/graft-common.sh"
. "$WORKDIR/modes/install-common.sh"

M_EFI=gbl-chainload.efi
M_LABEL=mode-1-install
M_PATCHER_ARGS=""
M_PACK_ARGS=""
M_MANIFEST_BITS=0x01

mode_preflight() { ... existing ... }
mode_preinstall_write() { ... existing ... }
```

`mode-2-install.sh`:
```sh
. "$WORKDIR/modes/install-common.sh"

M_EFI=gbl-chainload.efi
M_LABEL=mode-2-install
M_WANT_PROFILE=1
M_MANIFEST_BITS=0x02

STOCK_VBMETA=$GBL_STATE_DIR/mode-2/stock_vbmeta.img
PROFILE_TOML=$GBL_STATE_DIR/mode-2/profile.toml

# detect_oem definition deleted — now in install-common.sh.

build_profile() { ... unchanged ... }

mode_prepare() {
  detect_oem                    # uses shared definition
  build_profile
  M_PATCHER_ARGS="--oem $OEM_ID"  # no more --no-mode1
  M_PACK_ARGS="--mode2-profile $WORKDIR/profile.bin"
}

mode_preflight() { ... existing ... }
```

- [ ] **Step 3: Update `install-common.sh::build_payload`.** Add `--manifest $M_MANIFEST_BITS` to the `gbl-pack` invocation:

```sh
  # shellcheck disable=SC2086
  gbl-pack --cached-abl "$WORKDIR/patched.efi" \
           --source "$WORKDIR/cache_abl.img" \
           --extracted "$WORKDIR/extracted.efi" \
           $M_PACK_ARGS \
           --manifest "$M_MANIFEST_BITS" \
           --out "$WORKDIR/payload.bin" \
    || abort "gbl-pack failed"
```

- [ ] **Step 4: Update `zip/update-tools.sh`.** Whatever currently copies `mode-{0,1,2}.efi` into the ZIP's `base/` directory should now copy `gbl-chainload.efi` once.

- [ ] **Step 5: Update install-assembly tests** if they grep `mode-N.efi` names.

- [ ] **Step 6: Run tests.** Zip + install assembly suites pass.

- [ ] **Step 7: Commit.**

```bash
git add zip/modes/{install-common.sh,mode-0-install.sh,mode-1-install.sh,mode-2-install.sh} \
        zip/update-tools.sh tests/host/
git commit -m "install: single base EFI + M_MANIFEST_BITS; detect_oem to install-common"
```

---

### Task 15: Test housekeeping (remaining cleanups + path edits)

**Goal:** All test failures from upstream renames/refactors are addressed. Specifically: `088_patch7_multi_abl.sh` argv updates, any other tests that grep for retired symbols/paths, and a final `run-all.sh` green.

**Files:**
- Modify: `tests/host/088_patch7_multi_abl.sh`
- Modify: any test that asserts on `Mode1`/`Mode2`/`mode_1`/`oneplus_canoe`/`universal/` paths or symbols

**Acceptance Criteria:**
- [ ] `tests/host/088_patch7_multi_abl.sh` drops `--no-mode1` from its `abl-patcher` invocations (per Task 12 the flag is gone — leaving it in causes the test to fail at exec time).
- [ ] `bash tests/host/run-all.sh` exits 0 with all per-test PASS lines.
- [ ] No test file references retired symbols/paths.

**Verify:** `bash tests/host/run-all.sh` → all PASS, exit 0.

**Steps:**

- [ ] **Step 1: Update `088_patch7_multi_abl.sh`.** Remove `--no-mode1` from the three `abl-patcher` invocations (lines 53, 61, 82 per the earlier grep). Keep `--oem oneplus` since the deprecation alias still works (or update to `oplus` if you want to clean as you go).

- [ ] **Step 2: Sweep for stragglers.** `grep -rn 'mode_1\|oneplus_canoe\|universal/universal\|kMode1Patches\|kOemOneplusPatches\|kUniversalPatches' tests/ scripts/ tools/ zip/` — anything that pops up gets fixed.

- [ ] **Step 3: Run all tests.** `bash tests/host/run-all.sh` → green.

- [ ] **Step 4: Commit.**

```bash
git add tests/host/
git commit -m "tests: update for engine-rework renames; full suite green"
```

---

### Task 16: On-device boot validation — mode 0/1/2 against single EFI (USER GATE)

**USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Goal:** Validate end-to-end that the single-EFI build, packed with each of the three install presets (mode 0 / mode 1 / mode 2), boots cleanly on the infiniti test device via `fastboot stage` + `oem boot-efi`. This is the merge-gate.

**Files:** none — this is a runtime verification gate.

**Acceptance Criteria:**
- [ ] **Mode-0 boot.** Pack a mode-0 overlay (`efisp-package.py --mode 0`), `fastboot stage <out.efi>`, `fastboot oem boot-efi`. Device boots through to HLOS. HLOS reports real lock state (unlocked → orange warning if Oplus). Capture: console banner mentions `fakelock=0 profile_spoof=0`; HLOS boots; `getprop ro.boot.flash.locked` returns `0`; no `vb-fakelock` lines in the chainload log.
- [ ] **Mode-1 boot.** Pack a mode-1 overlay (`efisp-package.py --mode 1`), stage, boot-efi. HLOS boots. HLOS reports locked (`getprop ro.boot.flash.locked = 1`, `verifiedbootstate = green`). Capture: console banner mentions `fakelock=1 profile_spoof=0`.
- [ ] **Mode-2 boot.** Pack a mode-2 overlay (`efisp-package.py --mode 2 --stock-vbmeta <vbmeta>`), stage, boot-efi. HLOS boots. Attestation indicators per `mode2_validated_on_device` memory (2026-05-18 standards): key attestation passes, RKP / Widevine / Strongbox / SOTER all healthy, ABL stays honest (0 `vb-fakelock` lines). Capture: console banner mentions `fakelock=0 profile_spoof=1`.
- [ ] **EFISP recursion gate verified.** During each boot, the chainload log includes a `blockio: refused read on EFISP` line emitted by the second-stage ABL probe (proves the BlockIoHook gate fires before recursive load).
- [ ] **No-cached_abl fallback verified (optional but recommended).** Manually corrupt the EFISP cached_abl entry on one slot, reboot, observe the engine apply abl_permissive dynamically and complete a clean honest boot.

**Verify:** For each mode-N, the verify command is:

```
fastboot stage dist/efisp-payload/<dev>-modeN.efi
fastboot oem boot-efi
# then: read /proc/bootloader_log or the chainload log via diag dump, confirm
# the expected banner + behavior per AC above.
```

Evidence to capture per acceptance criterion: the chainload log excerpt showing the banner line for that mode, and a `getprop` snapshot from the booted HLOS for the lock-state assertion.

**Steps:**

- [ ] **Step 1: Build the single EFI.** `bash scripts/build.sh` → `dist/gbl-chainload.efi`.

- [ ] **Step 2: Pack three overlays.**

```bash
scripts/efisp-package.py --mode 0 \
  --abl <dump>.img --efi dist/gbl-chainload.efi \
  --out dist/efisp-payload/infiniti-mode0.efi

scripts/efisp-package.py --mode 1 \
  --abl <dump>.img --efi dist/gbl-chainload.efi \
  --out dist/efisp-payload/infiniti-mode1.efi

scripts/efisp-package.py --mode 2 --oem oplus \
  --abl <dump>.img --stock-vbmeta <stock_vbmeta>.img \
  --efi dist/gbl-chainload.efi \
  --out dist/efisp-payload/infiniti-mode2.efi
```

- [ ] **Step 3: For each mode, stage + boot-efi.**

```bash
fastboot stage dist/efisp-payload/infiniti-modeN.efi
fastboot oem boot-efi
```

Observe boot. Once HLOS is up: capture `getprop ro.boot.flash.locked`, `getprop ro.boot.verifiedbootstate`, and the chainload log (via `/proc/bootloader_log` or diag tool).

- [ ] **Step 4: Verify each acceptance criterion** against the captured evidence. If any AC fails, do NOT close the gate — investigate, fix in code, re-pack, re-test.

- [ ] **Step 5: Verify the BlockIoHook EFISP gate** by grepping the chainload log for `blockio: refused .* EFISP`.

- [ ] **Step 6: (Optional, recommended) corrupt cached_abl** and confirm the dynamic-patch fallback boots cleanly with abl_permissive applied at boot. This isn't strictly in the AC but exercises the §4 fallback path of the spec.

- [ ] **Step 7: Capture all evidence into the task's close comment.** Per the user-gate banner: every AC requires captured output, not a verbal "yes."

- [ ] **Step 8: Commit any final tweaks** from validation. Open the PR.

```bash
git push -u origin engine-rework
gh pr create --base main --title "Engine rework: capability-driven single EFI" --body "..."
```
