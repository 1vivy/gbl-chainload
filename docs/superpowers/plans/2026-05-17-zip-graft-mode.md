# vbmeta graft mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fill the `graft` mode of `zip-gbl-chainload` — a flashable ZIP that grafts stock OEM-signed vbmeta onto a user's custom partition image so it survives mode-1's userspace AVB.

**Architecture:** A new aarch64 recovery tool `vbmeta-graft` (`list`/`check`/`graft`) reuses the existing host-buildable `AvbParseLib` for AVB structure parsing. `modes/graft.sh` resolves a stock-vbmeta candidate (slot-led priority), runs the graft, and writes the result with `commit_verified`. The tool is re-vendored into the submodule like SP3's `fv-unwrap`.

**Tech Stack:** C (`vbmeta-graft` + reused `AvbParseLib`), POSIX `sh` / busybox-`ash` (`graft.{conf,sh}`), Bash (host tests), `git submodule`, `shellcheck`, GitHub Actions, Android NDK r27.

**Spec:** `docs/superpowers/specs/2026-05-17-zip-graft-mode-design.md`
**Branch:** `feature/zip-graft-mode` — PR #26 (off `main`, not stacked).

---

## Spec reconciliations

- The spec's `vbmeta-graft check` says "signed by the right key **and** an acceptable rollback index." This plan implements `check` as the **public-key match** gate only (candidate's vbmeta pubkey == the device main vbmeta's chain-descriptor pubkey for `<part>`). The rollback-index comparison is deferred — the spec's own open question flags it as an implementation detail, and the user's slot pick (priority-1 candidate) is the mechanism for "newest OTA". `check` additionally prints the candidate's `rollback-index:` so a future refinement can use it.
- `vbmeta-graft` reuses `GblChainloadPkg/Library/AvbParseLib/AvbParse.c` (host-buildable with `-D__HOST_BUILD__`, as `tests/avb/` already does) rather than reimplementing an AVB parser.

---

## File Structure

### Parent repo (`gbl-chainload`)

| File | Change | Responsibility |
|------|--------|----------------|
| `tools/vbmeta-graft/vbmeta-graft.c` | Create | The tool: `list` / `check` / `graft`. |
| `tools/vbmeta-graft/Makefile` | Create | Host + `android` (NDK) build, compiling `vbmeta-graft.c` + `AvbParse.c`. |
| `scripts/build-recovery-tools.sh` | Modify | Add `vbmeta-graft` to the toolchain build loop. |
| `tests/host/074_vbmeta_graft.sh` | Create | `vbmeta-graft` `list`/`check`/`graft` tests. |
| `tests/host/075_graft_assembly.sh` | Create | `--mode graft` ZIP-assembly test. |
| `zip` | Modify | Submodule pointer bump. |

### Submodule (`zip-gbl-chainload`, at `zip/`)

| File | Change | Responsibility |
|------|--------|----------------|
| `modes/graft.conf` | Modify (was a stub) | Declarative config for the graft mode. |
| `modes/graft.sh` | Modify (was a stub) | The graft mode body. |
| `modes/diag.sh` | Modify | Add the SP3-deferred vbmeta walk. |
| `update-tools.sh` | Modify | Add `vbmeta-graft` to the vendored set + MANIFEST. |
| `bin/vbmeta-graft`, `bin/MANIFEST` | Modify | Re-vendored tool + refreshed manifest. |

---

## Conventions

- Parent files commit on `feature/zip-graft-mode` (never `main`, never switch branch).
- Submodule files edit under `zip/`, commit with `git -C zip ...` on the submodule's `main`.
- `shellcheck` clean on every script — installer core busybox-`ash` → `shellcheck -s sh`; host tests Bash → plain `shellcheck`. Add a minimal `# shellcheck disable=` only if a warning is genuinely forced; report it.
- Never run `fastboot` flash/oem/flashing of non-HLOS partitions (project safety rule).

---

### Task 1: The `vbmeta-graft` tool

**Files:**
- Create: `tools/vbmeta-graft/vbmeta-graft.c`
- Create: `tools/vbmeta-graft/Makefile`
- Create: `tests/host/074_vbmeta_graft.sh`

- [ ] **Step 1: Write the failing test**

Create `tests/host/074_vbmeta_graft.sh`:

```bash
#!/usr/bin/env bash
# tests/host/074_vbmeta_graft.sh — vbmeta-graft list / check / graft.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/vbmeta-graft

OUT=tests/host/.last/074
rm -rf "$OUT"; mkdir -p "$OUT"
VG=tools/vbmeta-graft/vbmeta-graft

# grafted-recovery.img is a footer'd partition with a real embedded vbmeta.
FX=images/grafted-recovery.img
[ -f "$FX" ] || { echo "SKIP: $FX absent"; exit 0; }

# --- list: enumerates the embedded vbmeta's descriptors ---------------
"$VG" list "$FX" > "$OUT/list.txt" 2>&1 \
  || { echo "FAIL: list exited nonzero"; cat "$OUT/list.txt"; exit 1; }
grep -q 'partition=' "$OUT/list.txt" \
  || { echo "FAIL: list produced no 'partition=' lines"; cat "$OUT/list.txt"; exit 1; }

# --- graft: round-trip an arbitrary custom image ----------------------
head -c 200000 /dev/urandom > "$OUT/custom.img"
PSZ=$(stat -c%s "$FX")
"$VG" graft --stock "$FX" --custom "$OUT/custom.img" --part-size "$PSZ" \
       --out "$OUT/grafted.img" > "$OUT/graft.log" 2>&1 \
  || { echo "FAIL: graft exited nonzero"; cat "$OUT/graft.log"; exit 1; }
[ "$(stat -c%s "$OUT/grafted.img")" = "$PSZ" ] \
  || { echo "FAIL: grafted image is not partition-sized"; exit 1; }
# the grafted image must itself parse: its footer points at a vbmeta
"$VG" list "$OUT/grafted.img" > "$OUT/grafted-list.txt" 2>&1 \
  || { echo "FAIL: grafted image does not re-parse"; cat "$OUT/grafted-list.txt"; exit 1; }
# first 200000 bytes are the custom content verbatim
cmp -n 200000 "$OUT/custom.img" "$OUT/grafted.img" \
  || { echo "FAIL: custom content not preserved at offset 0"; exit 1; }

# --- check: a partition checked against its own vbmeta is suitable ----
# (grafted-recovery's embedded vbmeta is self-consistent for 'recovery')
"$VG" check "$FX" "$FX" recovery > "$OUT/check.log" 2>&1
rc=$?
echo "check rc=$rc" >> "$OUT/check.log"
# rc 0 = suitable, 2 = parsed/mismatch, 1 = unparseable. Accept 0 or 2
# here (the fixture predates the project's key); a hard fail is rc 1.
[ "$rc" != 1 ] || { echo "FAIL: check could not parse the fixture"; cat "$OUT/check.log"; exit 1; }

echo "PASS: 074 vbmeta-graft"
```

- [ ] **Step 2: Run it — verify it fails**

Run: `bash tests/host/074_vbmeta_graft.sh`
Expected: FAIL — `make -s -C tools/vbmeta-graft` errors (`tools/vbmeta-graft/` does not exist yet).

- [ ] **Step 3: Write `tools/vbmeta-graft/vbmeta-graft.c`**

```c
/* tools/vbmeta-graft/vbmeta-graft.c — list / check / graft AVB vbmeta.
 *
 *   vbmeta-graft list  <vbmeta-or-partition-img>
 *   vbmeta-graft check <candidate-partition-img> <main-vbmeta-img> <part>
 *   vbmeta-graft graft --stock <stock-part-img> --custom <custom-img>
 *                      --part-size <bytes> --out <grafted-img>
 *
 * Reuses GblChainloadPkg/Library/AvbParseLib for AVB structure parsing
 * (compiled with -D__HOST_BUILD__; the Makefile builds AvbParse.c too).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* EDK2-type host shim — mirrors tests/avb/test_avbparse.c. */
#define IN
#define OUT
#define EFIAPI
#define STATIC static
#define CONST const
typedef uint8_t  UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef char     CHAR8;
typedef int      EFI_STATUS;
#define EFI_SUCCESS            0
#define EFI_NOT_FOUND          14
#define EFI_INVALID_PARAMETER  2
#define EFI_END_OF_MEDIA       28
#define EFI_ERROR(s)  ((s) != 0)
#include "../../GblChainloadPkg/Include/Library/AvbParseLib.h"

/* slurp: read a whole file into a malloc'd buffer. */
static uint8_t *slurp (const char *path, size_t *len_out) {
  FILE *f = fopen (path, "rb");
  if (!f) { fprintf (stderr, "vbmeta-graft: %s: cannot open\n", path); return NULL; }
  fseek (f, 0, SEEK_END);
  long n = ftell (f);
  fseek (f, 0, SEEK_SET);
  if (n <= 0) { fprintf (stderr, "vbmeta-graft: %s: empty\n", path); fclose (f); return NULL; }
  uint8_t *buf = malloc ((size_t) n);
  if (!buf) { fclose (f); return NULL; }
  if (fread (buf, 1, (size_t) n, f) != (size_t) n) {
    fprintf (stderr, "vbmeta-graft: %s: read error\n", path);
    free (buf); fclose (f); return NULL;
  }
  fclose (f);
  *len_out = (size_t) n;
  return buf;
}

/* locate_vbmeta: point at the vbmeta blob inside a buffer. If the buffer
 * has an AvbFooter (footer'd partition), use it; else treat the whole
 * buffer as a bare vbmeta blob (e.g. the main `vbmeta` partition). */
static int locate_vbmeta (const uint8_t *buf, size_t len,
                          const uint8_t **vb_out, uint64_t *vb_len_out) {
  GBL_AVB_FOOTER footer;
  if (AvbParse_Footer (buf, len, &footer) == EFI_SUCCESS) {
    *vb_out = buf + footer.VbmetaOffset;
    *vb_len_out = footer.VbmetaSize;
    return 0;
  }
  /* No footer: the buffer itself should start with the vbmeta magic. */
  if (len >= 4 && memcmp (buf, GBL_AVB_VBMETA_MAGIC, 4) == 0) {
    *vb_out = buf;
    *vb_len_out = len;
    return 0;
  }
  return -1;
}

/* aux_block: compute the auxiliary block pointer + size from a header. */
static const uint8_t *aux_block (const uint8_t *vb, const GBL_AVB_VBMETA_HEADER *h,
                                 uint64_t *aux_len_out) {
  *aux_len_out = h->AuxiliaryDataBlockSize;
  return vb + GBL_AVB_VBMETA_HEADER_SIZE + h->AuthenticationDataBlockSize;
}

/* descriptor_walk callback type. */
typedef void (*desc_fn) (GBL_AVB_DESCRIPTOR_TAG tag, const uint8_t *desc,
                         uint64_t desc_len, void *ctx);

/* walk every descriptor of a vbmeta blob. Returns 0 on success. */
static int walk_descriptors (const uint8_t *vb, uint64_t vb_len,
                             desc_fn fn, void *ctx) {
  GBL_AVB_VBMETA_HEADER h;
  if (AvbParse_VbmetaHeader (vb, vb_len, &h) != EFI_SUCCESS) return -1;
  uint64_t aux_len;
  const uint8_t *aux = aux_block (vb, &h, &aux_len);
  uint64_t cursor = h.DescriptorsOffset;
  uint64_t end    = h.DescriptorsOffset + h.DescriptorsSize;
  while (cursor < end) {
    GBL_AVB_DESCRIPTOR_TAG tag;
    const uint8_t *desc;
    uint64_t desc_len;
    if (AvbParse_NextDescriptor (aux, aux_len, &cursor, &tag, &desc, &desc_len)
        != EFI_SUCCESS)
      break;
    fn (tag, desc, desc_len, ctx);
  }
  return 0;
}

/* ---- list ----------------------------------------------------------- */

static void list_cb (GBL_AVB_DESCRIPTOR_TAG tag, const uint8_t *desc,
                      uint64_t desc_len, void *ctx) {
  (void) ctx;
  const char *kind = "other";
  const uint8_t *name = NULL; uint32_t name_len = 0;
  if (tag == GblAvbDescHashTag) {
    kind = "hash";
    const uint8_t *digest; uint32_t digest_len;
    AvbParse_HashDescriptor (desc, desc_len, &name, &name_len, &digest, &digest_len);
  } else if (tag == GblAvbDescChainPartitionTag) {
    kind = "chain";
    const uint8_t *pk; uint32_t pk_len;
    AvbParse_ChainPartitionDescriptor (desc, desc_len, &name, &name_len, &pk, &pk_len);
  } else if (tag == GblAvbDescHashtreeTag) {
    kind = "hashtree";
  }
  if (name && name_len) {
    printf ("partition=%.*s type=%s graftable=%s\n",
            (int) name_len, (const char *) name, kind,
            (tag == GblAvbDescHashTag || tag == GblAvbDescChainPartitionTag)
              ? "yes" : "no");
  } else {
    printf ("descriptor type=%s\n", kind);
  }
}

static int cmd_list (const char *path) {
  size_t len; uint8_t *buf = slurp (path, &len);
  if (!buf) return 1;
  const uint8_t *vb; uint64_t vb_len;
  if (locate_vbmeta (buf, len, &vb, &vb_len) != 0) {
    fprintf (stderr, "vbmeta-graft: %s: no vbmeta found\n", path);
    free (buf); return 1;
  }
  int rc = walk_descriptors (vb, vb_len, list_cb, NULL);
  free (buf);
  return rc == 0 ? 0 : 1;
}

/* ---- check ---------------------------------------------------------- */

/* find_chain_pubkey: locate <part>'s chain descriptor in a main vbmeta and
 * copy its public key into a malloc'd buffer. Returns NULL if not found. */
struct chain_ctx { const char *part; uint8_t *pk; uint32_t pk_len; };

static void chain_cb (GBL_AVB_DESCRIPTOR_TAG tag, const uint8_t *desc,
                       uint64_t desc_len, void *vctx) {
  struct chain_ctx *c = vctx;
  if (c->pk || tag != GblAvbDescChainPartitionTag) return;
  const uint8_t *name; uint32_t name_len;
  const uint8_t *pk;   uint32_t pk_len;
  if (AvbParse_ChainPartitionDescriptor (desc, desc_len, &name, &name_len,
                                         &pk, &pk_len) != EFI_SUCCESS)
    return;
  if (name_len == strlen (c->part) &&
      memcmp (name, c->part, name_len) == 0) {
    c->pk = malloc (pk_len);
    if (c->pk) { memcpy (c->pk, pk, pk_len); c->pk_len = pk_len; }
  }
}

static int cmd_check (const char *cand_path, const char *main_path,
                      const char *part) {
  size_t cl, ml;
  uint8_t *cand = slurp (cand_path, &cl);
  if (!cand) return 1;
  uint8_t *mainb = slurp (main_path, &ml);
  if (!mainb) { free (cand); return 1; }

  const uint8_t *cvb; uint64_t cvb_len;
  const uint8_t *mvb; uint64_t mvb_len;
  if (locate_vbmeta (cand, cl, &cvb, &cvb_len) != 0 ||
      locate_vbmeta (mainb, ml, &mvb, &mvb_len) != 0) {
    fprintf (stderr, "vbmeta-graft: check: unparseable vbmeta\n");
    free (cand); free (mainb); return 1;
  }

  /* candidate's own public key (header offsets into its aux block) */
  GBL_AVB_VBMETA_HEADER ch;
  if (AvbParse_VbmetaHeader (cvb, cvb_len, &ch) != EFI_SUCCESS) {
    fprintf (stderr, "vbmeta-graft: check: bad candidate vbmeta\n");
    free (cand); free (mainb); return 1;
  }
  uint64_t caux_len;
  const uint8_t *caux = aux_block (cvb, &ch, &caux_len);
  const uint8_t *cand_pk = caux + ch.PublicKeyOffset;
  uint32_t cand_pk_len = (uint32_t) ch.PublicKeySize;
  printf ("rollback-index: %llu\n", (unsigned long long) ch.RollbackIndex);

  /* the key <part>'s chain descriptor in the main vbmeta names */
  struct chain_ctx cc = { part, NULL, 0 };
  walk_descriptors (mvb, mvb_len, chain_cb, &cc);
  int rc;
  if (!cc.pk) {
    fprintf (stderr, "vbmeta-graft: check: no chain descriptor for '%s'\n", part);
    rc = 2;                      /* parsed, but unsuitable */
  } else if (cc.pk_len == cand_pk_len &&
             memcmp (cc.pk, cand_pk, cand_pk_len) == 0) {
    printf ("suitable: key matches chain descriptor for %s\n", part);
    rc = 0;
  } else {
    fprintf (stderr, "vbmeta-graft: check: key mismatch for '%s'\n", part);
    rc = 2;
  }
  free (cc.pk); free (cand); free (mainb);
  return rc;
}

/* ---- graft ---------------------------------------------------------- */

static void put_u32_be (uint8_t *p, uint32_t v) {
  p[0]=(v>>24)&0xff; p[1]=(v>>16)&0xff; p[2]=(v>>8)&0xff; p[3]=v&0xff;
}
static void put_u64_be (uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = (uint8_t) (v >> (56 - i*8));
}

static int cmd_graft (const char *stock_path, const char *custom_path,
                      uint64_t part_size, const char *out_path) {
  size_t sl, custl;
  uint8_t *stock = slurp (stock_path, &sl);
  if (!stock) return 1;
  uint8_t *custom = slurp (custom_path, &custl);
  if (!custom) { free (stock); return 1; }

  const uint8_t *svb; uint64_t svb_len;
  if (locate_vbmeta (stock, sl, &svb, &svb_len) != 0) {
    fprintf (stderr, "vbmeta-graft: graft: no stock vbmeta\n");
    free (stock); free (custom); return 1;
  }

  uint64_t content   = custl;
  uint64_t vb_off    = (content + 4095) & ~(uint64_t) 4095;   /* round up 4K */
  uint64_t footer_at = part_size - GBL_AVB_FOOTER_SIZE;
  if (vb_off + svb_len > footer_at) {
    fprintf (stderr, "vbmeta-graft: graft: custom image too large for the "
                     "partition (%llu B content + vbmeta exceeds %llu B)\n",
             (unsigned long long) content, (unsigned long long) part_size);
    free (stock); free (custom); return 1;
  }

  uint8_t *img = calloc (1, part_size);
  if (!img) { free (stock); free (custom); return 1; }
  memcpy (img, custom, content);                /* custom content at 0    */
  memcpy (img + vb_off, svb, svb_len);           /* stock vbmeta blob      */

  uint8_t *ft = img + footer_at;                 /* 64-byte AvbFooter      */
  memcpy (ft, GBL_AVB_FOOTER_MAGIC, 4);
  put_u32_be (ft + 4, 1);                        /* footer major version  */
  put_u32_be (ft + 8, 0);                        /* footer minor version  */
  put_u64_be (ft + 12, content);                 /* OriginalImageSize     */
  put_u64_be (ft + 20, vb_off);                  /* VbmetaOffset          */
  put_u64_be (ft + 28, svb_len);                 /* VbmetaSize            */

  FILE *o = fopen (out_path, "wb");
  if (!o) { fprintf (stderr, "vbmeta-graft: %s: cannot write\n", out_path);
            free (img); free (stock); free (custom); return 1; }
  int ok = fwrite (img, 1, part_size, o) == part_size;
  fclose (o);
  free (img); free (stock); free (custom);
  if (!ok) { fprintf (stderr, "vbmeta-graft: graft: short write\n"); return 1; }
  fprintf (stderr, "vbmeta-graft: grafted %s (%llu B, vbmeta @ 0x%llx)\n",
           out_path, (unsigned long long) part_size, (unsigned long long) vb_off);
  return 0;
}

/* ---- main ----------------------------------------------------------- */

static int usage (void) {
  fprintf (stderr,
    "usage:\n"
    "  vbmeta-graft list  <vbmeta-or-partition-img>\n"
    "  vbmeta-graft check <candidate-part-img> <main-vbmeta-img> <part>\n"
    "  vbmeta-graft graft --stock <s> --custom <c> --part-size <N> --out <o>\n");
  return 2;
}

int main (int argc, char **argv) {
  if (argc < 2) return usage ();
  if (strcmp (argv[1], "list") == 0 && argc == 3)
    return cmd_list (argv[2]);
  if (strcmp (argv[1], "check") == 0 && argc == 5)
    return cmd_check (argv[2], argv[3], argv[4]);
  if (strcmp (argv[1], "graft") == 0) {
    const char *stock = NULL, *custom = NULL, *out = NULL;
    uint64_t part_size = 0;
    for (int i = 2; i + 1 < argc; i += 2) {
      if      (strcmp (argv[i], "--stock")     == 0) stock = argv[i+1];
      else if (strcmp (argv[i], "--custom")    == 0) custom = argv[i+1];
      else if (strcmp (argv[i], "--out")       == 0) out = argv[i+1];
      else if (strcmp (argv[i], "--part-size") == 0)
        part_size = strtoull (argv[i+1], NULL, 10);
      else return usage ();
    }
    if (!stock || !custom || !out || part_size < GBL_AVB_FOOTER_SIZE)
      return usage ();
    return cmd_graft (stock, custom, part_size, out);
  }
  return usage ();
}
```

- [ ] **Step 4: Write `tools/vbmeta-graft/Makefile`**

```make
# tools/vbmeta-graft/Makefile
PROJ    := $(realpath ../..)
AVB     := $(PROJ)/GblChainloadPkg/Library/AvbParseLib
INCS    := -I$(AVB)/Internal -I$(PROJ)/GblChainloadPkg/Include/Library
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11
CFLAGS  += -D__HOST_BUILD__ $(INCS)

SRCS := vbmeta-graft.c $(AVB)/AvbParse.c

all: vbmeta-graft

vbmeta-graft: $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

# --- Android cross-compile target (NDK r27) ---
NDK     ?= $(ANDROID_NDK)
NDK_CC   = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang
NDK_CFLAGS = -static -O2 -Wall -Wextra -std=c11 -D__HOST_BUILD__ $(INCS)

android: vbmeta-graft-android

vbmeta-graft-android: $(SRCS)
	$(NDK_CC) $(NDK_CFLAGS) $^ -o $@

clean:
	rm -f vbmeta-graft vbmeta-graft-android
```

- [ ] **Step 5: Build and run the test — verify it passes**

Run: `make -s -C tools/vbmeta-graft && bash tests/host/074_vbmeta_graft.sh`
Expected: final line `PASS: 074 vbmeta-graft`. If the compile fails or the test fails, fix `vbmeta-graft.c` against the `AvbParseLib.h` API until both build clean (`-Wall -Wextra`, no warnings) and `074` passes.

- [ ] **Step 6: Commit (parent)**

```bash
git add tools/vbmeta-graft/vbmeta-graft.c tools/vbmeta-graft/Makefile tests/host/074_vbmeta_graft.sh
git commit -m "tools: vbmeta-graft - list/check/graft AVB vbmeta

Reuses AvbParseLib (AvbParse.c, -D__HOST_BUILD__) for structure parsing.
list walks descriptors; check gates a stock-vbmeta candidate by
public-key match against the device main vbmeta's chain descriptor;
graft assembles custom-content + stock vbmeta + AvbFooter at the
round_up(content,4K) natural offset.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Wire `vbmeta-graft` into the recovery toolchain

**Files:**
- Modify: `scripts/build-recovery-tools.sh`
- Modify: `zip/update-tools.sh`

- [ ] **Step 1: Add `vbmeta-graft` to `build-recovery-tools.sh`**

In `scripts/build-recovery-tools.sh`, change the tool loop line:

```bash
  for t in fv-unwrap abl-patcher gbl-pack gbl-commit; do
```

to:

```bash
  for t in fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft; do
```

- [ ] **Step 2: Add `vbmeta-graft` to `zip/update-tools.sh`**

In `zip/update-tools.sh`, change the copy loop:

```sh
for t in fv-unwrap abl-patcher gbl-pack gbl-commit; do
  cp "$PARENT/dist/recovery/$t" "$SELF_DIR/bin/$t"
done
```

to:

```sh
for t in fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft; do
  cp "$PARENT/dist/recovery/$t" "$SELF_DIR/bin/$t"
done
```

and add `bin/vbmeta-graft` to the `sha256sum` MANIFEST line:

```sh
  ( cd "$SELF_DIR" && sha256sum \
      bin/fv-unwrap bin/abl-patcher bin/gbl-pack bin/gbl-commit \
      bin/vbmeta-graft bin/busybox-arm64 base/mode-1.efi )
```

- [ ] **Step 3: Lint**

Run: `shellcheck scripts/build-recovery-tools.sh zip/update-tools.sh`
Expected: no output, exit 0.

- [ ] **Step 4: Commit (two repos)**

```bash
git -C zip add update-tools.sh
git -C zip commit -m "tools: update-tools.sh - vendor vbmeta-graft"
git add scripts/build-recovery-tools.sh
git commit -m "build: build-recovery-tools.sh - build vbmeta-graft

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: `modes/graft.conf` and `modes/graft.sh`

**Files:**
- Modify: `zip/modes/graft.conf` (was the SP2 abort-stub)
- Modify: `zip/modes/graft.sh` (was the SP2 abort-stub)

- [ ] **Step 1: Replace `zip/modes/graft.conf`**

```sh
# shellcheck shell=sh
# shellcheck disable=SC2034
# modes/graft.conf — declarative config for the graft mode.
# graft writes stock vbmeta onto a custom partition image so it survives
# mode-1 userspace AVB. See the SP4 design spec.

MODE_NAME="graft"
MODE_DESC="graft stock vbmeta onto a custom partition (mode-1 AVB coexistence)"
MODE_WRITES="the selected slot of each grafted partition"
MODE_TOOLS="vbmeta-graft gbl-commit"
MODE_EFI=""
```

- [ ] **Step 2: Replace `zip/modes/graft.sh`**

```sh
# shellcheck shell=sh
# shellcheck disable=SC2154
# modes/graft.sh — graft stock vbmeta onto custom partition image(s).
#
# For each /sdcard/gbl_<part>.img: resolve a suitable stock vbmeta (the
# picked slot first, then /sdcard/stock_<part>.img, then the other slot),
# graft it on, and flash the result to <part>_<slot>. See the SP4 spec.

# vol_key <timeout> -> UP | DOWN | TIMEOUT  (200 events: see zip-methodology A2)
vol_key() {
  _k=$(timeout "$1" getevent -lqc 200 2>/dev/null \
         | grep -m1 -oE 'KEY_(VOLUMEUP|VOLUMEDOWN)' || true)
  case "$_k" in
    KEY_VOLUMEUP)   echo UP ;;
    KEY_VOLUMEDOWN) echo DOWN ;;
    *)              echo TIMEOUT ;;
  esac
}

# pick_slot -> sets SLOT_SUF (a|b). Recovery prompts; BOOTMODE = inactive.
pick_slot() {
  [ -n "$SLOT" ] && [ -n "$INACTIVE" ] || abort "not an A/B device"
  if $BOOTMODE; then
    SLOT_SUF="$INACTIVE"
    ui_print "[*] booted-Android: assuming post-OTA -> slot $SLOT_SUF"
    return 0
  fi
  ui_print "Please select slot to perform graft and flash on:"
  ui_print "  Vol-UP = A    Vol-DOWN = B"
  ui_print "  (If the OTA was flashed from recovery or you know it's on"
  ui_print "   the inactive slot, select that one.)"
  case "$(vol_key 15)" in
    UP)   SLOT_SUF=a ;;
    DOWN) SLOT_SUF=b ;;
    *)    abort "no slot selected" ;;
  esac
  ui_print "[*] target slot: $SLOT_SUF"
}

# graft_one <part> -> graft /sdcard/gbl_<part>.img onto <part>_$SLOT_SUF.
graft_one() {
  _part="$1"
  _custom="/sdcard/gbl_$_part.img"
  _target=$(byname "${_part}_${SLOT_SUF}")
  _mainvb=$(byname "vbmeta_${SLOT_SUF}")
  [ -n "$_target" ] || abort "partition ${_part}_${SLOT_SUF} not found"
  [ -n "$_mainvb" ] || abort "partition vbmeta_${SLOT_SUF} not found"

  ui_print "[*] $_part: selecting a suitable stock vbmeta"
  dd if="$_mainvb" of="$WORKDIR/main_vbmeta.img" bs=1M 2>/dev/null \
    || abort "cannot read vbmeta_${SLOT_SUF}"

  # candidate priority: picked slot, /sdcard/stock, other slot.
  _other=a; [ "$SLOT_SUF" = a ] && _other=b
  _stock=""
  for _cand in "$_target" "/sdcard/stock_$_part.img" "$(byname "${_part}_${_other}")"; do
    [ -n "$_cand" ] && [ -e "$_cand" ] || continue
    dd if="$_cand" of="$WORKDIR/cand.img" bs=1M 2>/dev/null || continue
    if vbmeta-graft check "$WORKDIR/cand.img" "$WORKDIR/main_vbmeta.img" \
         "$_part" >/dev/null 2>&1; then
      cp "$WORKDIR/cand.img" "$WORKDIR/stock_$_part.img"
      _stock="$WORKDIR/stock_$_part.img"
      ui_print "    using stock vbmeta from: $_cand"
      break
    fi
  done
  [ -n "$_stock" ] || abort "$_part: no suitable stock vbmeta candidate"

  _psz=$(blockdev --getsize64 "$_target") || abort "cannot size $_target"
  ui_print "[*] $_part: grafting"
  vbmeta-graft graft --stock "$_stock" --custom "$_custom" \
    --part-size "$_psz" --out "$WORKDIR/grafted_$_part.img" \
    || abort "$_part: vbmeta-graft graft failed"

  ui_print "[*] $_part: writing ${_part}_${SLOT_SUF} (backup + verify)"
  commit_verified "$WORKDIR/grafted_$_part.img" "$_target" \
    "/sdcard/${_part}_${SLOT_SUF}.bak"
}

mode_main() {
  ui_print "graft: vbmeta graft installer"
  ui_print ""

  # collect the custom images present
  _parts=""
  for _f in /sdcard/gbl_*.img; do
    [ -e "$_f" ] || continue
    _b=$(basename "$_f" .img)        # gbl_<part>
    _parts="$_parts ${_b#gbl_}"
  done
  [ -n "$_parts" ] || abort "no /sdcard/gbl_<part>.img found"
  ui_print "[*] custom images:$_parts"

  pick_slot
  for _p in $_parts; do
    graft_one "$_p"
  done

  ui_print ""
  ui_print "graft: done - reboot to use the grafted partition(s)."
}
```

- [ ] **Step 3: Lint**

Run: `shellcheck -s sh zip/modes/graft.conf zip/modes/graft.sh`
Expected: no output, exit 0. If a warning is genuinely forced, add a minimal `# shellcheck disable=` to the header and report it (the SP2/SP3 precedent — e.g. `SC2154` for core-provided vars is already disabled).

- [ ] **Step 4: Commit (submodule)**

```bash
git -C zip add modes/graft.conf modes/graft.sh
git -C zip commit -m "modes: graft - stock-vbmeta graft onto custom partitions"
```

---

### Task 4: `diag` vbmeta walk

**Files:**
- Modify: `zip/modes/diag.sh`

- [ ] **Step 1: Replace the partition-enumeration block**

In `zip/modes/diag.sh`, replace this block:

```sh
    # Static list of the partitions this project touches: efisp (install
    # target), abl (cache source + loader-restore), vbmeta (SP4 graft
    # target). All but efisp are A/B-slotted on the target devices.
    # SP4 adds a vbmeta-descriptor walk here once the on-device
    # vbmeta-parsing tool exists.
    for p in efisp abl_a abl_b vbmeta_a vbmeta_b; do
      if [ -e "$BYNAME/$p" ]; then
        ui_print "    [present] $p"
      else
        ui_print "    [absent ] $p"
      fi
    done
```

with:

```sh
    # Static list of the partitions this project touches: efisp (install
    # target), abl (cache source + loader-restore), vbmeta (graft target).
    for p in efisp abl_a abl_b vbmeta_a vbmeta_b; do
      if [ -e "$BYNAME/$p" ]; then
        ui_print "    [present] $p"
      else
        ui_print "    [absent ] $p"
      fi
    done

    # vbmeta descriptor walk: list the partitions the active slot's main
    # vbmeta covers (handy context for the graft mode).
    if [ -e "$BYNAME/vbmeta_$SLOT" ] && command -v vbmeta-graft >/dev/null 2>&1; then
      dd if="$BYNAME/vbmeta_$SLOT" of="$WORKDIR/diag_vbmeta.img" bs=1M 2>/dev/null
      ui_print "  vbmeta_$SLOT covers:"
      vbmeta-graft list "$WORKDIR/diag_vbmeta.img" 2>/dev/null \
        | while read -r line; do ui_print "    $line"; done
    fi
```

- [ ] **Step 2: Lint**

Run: `shellcheck -s sh zip/modes/diag.sh`
Expected: no output, exit 0.

- [ ] **Step 3: Commit (submodule)**

```bash
git -C zip add modes/diag.sh
git -C zip commit -m "modes: diag - walk the main vbmeta's covered partitions"
```

---

### Task 5: Re-vendor the recovery tools

**Files:**
- Modify: `zip/bin/` (adds `vbmeta-graft`, refreshes `MANIFEST`), `zip/base/`
- Modify: `zip` (parent gitlink)

> **Environment note:** runs `zip/update-tools.sh` — Docker builds (NDK r27 + EDK2). Docker must be available; if not, report BLOCKED.

- [ ] **Step 1: Re-vendor**

```bash
cd /home/vivy/gbl-chainload/zip
./update-tools.sh
cd /home/vivy/gbl-chainload
```

Expected: ends with `==> done. ...`; `zip/bin/vbmeta-graft` now exists; `zip/bin/MANIFEST` lists it with `# parent-dirty: 0`.

- [ ] **Step 2: Verify**

```bash
( cd zip && grep -E '^[0-9a-f]{64}  ' bin/MANIFEST | sha256sum -c )
file zip/bin/vbmeta-graft | grep -o 'ARM aarch64'
```

Expected: `sha256sum -c` prints `OK` for every line (including `bin/vbmeta-graft`); `file` reports `ARM aarch64`.

- [ ] **Step 3: Commit submodule, push, bump pointer**

```bash
git -C zip add bin base
git -C zip commit -m "bin: re-vendor recovery tools incl. vbmeta-graft"
git -C zip push origin HEAD:main
git add zip
git commit -m "build: advance zip submodule - graft mode + vbmeta-graft

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: `tests/host/075_graft_assembly.sh`

**Files:**
- Create: `tests/host/075_graft_assembly.sh`

- [ ] **Step 1: Write the test**

Create `tests/host/075_graft_assembly.sh`:

```bash
#!/usr/bin/env bash
# tests/host/075_graft_assembly.sh — assemble gbl-chainload-graft.zip.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=tests/host/.last/075
rm -rf "$OUT"; mkdir -p "$OUT"

bash scripts/build-recovery-zip.sh --mode graft >/dev/null
ZIP=dist/gbl-chainload-graft.zip
[ -f "$ZIP" ] || { echo "FAIL: $ZIP not produced"; exit 1; }

for e in META-INF/com/google/android/update-binary \
         core/ui.sh core/env.sh core/ota.sh core/busybox.sh core/partition.sh core/safety.sh \
         modes/SELECTED modes/graft.conf modes/graft.sh \
         bin/vbmeta-graft bin/gbl-commit bin/busybox-arm64 SHA256SUMS; do
  unzip -l "$ZIP" | grep -q "[ /]$e\$" || { echo "FAIL: $ZIP missing $e"; exit 1; }
done

unzip -p "$ZIP" modes/SELECTED | grep -qx graft \
  || { echo "FAIL: SELECTED is not 'graft'"; exit 1; }
if unzip -l "$ZIP" | grep -qE 'modes/(diag|install|profile)'; then
  echo "FAIL: non-selected modes were not pruned"; exit 1
fi

unzip -o "$ZIP" -d "$OUT/x" >/dev/null
( cd "$OUT/x" && sha256sum -c --status SHA256SUMS ) \
  || { echo "FAIL: SHA256SUMS mismatch"; exit 1; }
shellcheck -s sh "$OUT/x/modes/graft.sh" \
  || { echo "FAIL: staged graft.sh fails shellcheck"; exit 1; }

echo "PASS: 075 graft assembly"
```

- [ ] **Step 2: Run it and the full suite**

Run: `bash tests/host/075_graft_assembly.sh`
Expected: `PASS: 075 graft assembly`.
Run: `bash tests/runall.sh`
Expected: `074_vbmeta_graft` and `075_graft_assembly` both appear; final line `ALL TESTS PASS`. Slow (Docker build smoke) — generous timeout or background.

- [ ] **Step 3: Commit (parent)**

```bash
git add tests/host/075_graft_assembly.sh
git commit -m "test: 075 graft-mode ZIP assembly"
```

---

### Task 7: Finalize

**Files:** none (integration only)

- [ ] **Step 1: Push the branch**

```bash
git push origin feature/zip-graft-mode
```

- [ ] **Step 2: Confirm CI on PR #26**

Run: `gh pr checks 26 --watch --interval 20`
Expected: the `test` check passes (`tests/runall.sh`, now incl. 074 + 075).

- [ ] **Step 3: On-device acceptance (user-run, manual — not agent-run)**

`scripts/build-recovery-zip.sh --mode graft` builds `dist/gbl-chainload-graft.zip`. With a `/sdcard/gbl_<part>.img` staged, the user flashes it in recovery, picks the slot, and confirms the grafted partition boots and normal Android boot survives mode-1. The agent cannot run this (device prompt + non-HLOS write).

---

## Self-Review

**Spec coverage:**

- `vbmeta-graft` tool (`list`/`check`/`graft`) → Task 1.
- Reuse of `AvbParseLib` → Task 1 (Makefile compiles `AvbParse.c`).
- Toolchain build + vendoring → Task 2, Task 5.
- `modes/graft.{conf,sh}` — slot prompt, BOOTMODE=inactive, file-driven partitions, slot-led candidate priority, `commit_verified` write → Task 3.
- `diag` vbmeta walk (SP3-deferred) → Task 4.
- Tests: `vbmeta-graft` `list`/`check`/`graft` → Task 1 (`074`); `--mode graft` assembly → Task 6 (`075`).
- On-device Layer-3 acceptance → Task 7 Step 3.

**Placeholder scan:** none — `vbmeta-graft.c`, `Makefile`, `graft.{conf,sh}`, both tests, and every edit are shown complete. Task 1 Step 5 explicitly allows iterating `vbmeta-graft.c` against the compiler and test `074` — that is the TDD loop, not a placeholder.

**Type/name consistency:** `vbmeta-graft` subcommands `list <img>` / `check <cand> <main> <part>` / `graft --stock --custom --part-size --out` are identical in `vbmeta-graft.c` (`main`), the Makefile-built binary, `graft.sh` (`graft_one`), `diag.sh`, and tests `074`/`075`. `MODE_TOOLS="vbmeta-graft gbl-commit"` (Task 3) matches the binaries `075` asserts and the `update-tools.sh` vendor list (Task 2). `commit_verified` / `abort` / `byname` / `WORKDIR` / `SLOT` / `INACTIVE` / `BOOTMODE` are the SP2 `core/*.sh` contracts. `AvbParse_Footer` / `AvbParse_VbmetaHeader` / `AvbParse_NextDescriptor` / `AvbParse_HashDescriptor` / `AvbParse_ChainPartitionDescriptor` and the `GBL_AVB_*` structs/constants match `GblChainloadPkg/Include/Library/AvbParseLib.h` exactly.
