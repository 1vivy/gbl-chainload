# AVB façade graft + libavb-status fastboot interface — Implementation Plan

> **Status: SUPERSEDED — 2026-05-12.** Shipped per this plan but proved
> insufficient on the primary Android bootpath: stock's `expected_digest`
> baked into the transplanted hash descriptor mismatches the custom
> partition's actual content, and userspace `libfs_avb` (post AOSP
> `ec10d3c`) re-validates the on-disk vbmeta independently of the
> bootloader's claim. Graft worked for system-boot only because patch10
> swallowed the mismatch at the ABL layer; recovery-boot broke at a
> parallel check that patch10 doesn't cover.
>
> **Replaced by:** [`scripts/synthesize-vbmeta.py`](../../../scripts/synthesize-vbmeta.py)
> and the upcoming `fastboot oem synthesize-and-flash` command. Synthesize
> derives `expected_digest` from the partition's actual bytes, removing the
> mismatch by construction.
>
> The graft host scripts (`scripts/graft-vbmeta.py`, `tools/avb-graft-recovery/`,
> `zip/avb-graft-installer/`) and `tests/052_graft_vbmeta_roundtrip.sh` were
> deleted. `AvbParseLib` is retained — its parsers are reusable for the
> diagnostic / `getvar` surface this plan defined. The fastboot
> `CmdOemGraftAndFlash` removal lands with the synthesize command in PR C.
>
> Body below preserved for historical context and the design vocabulary
> (three-track tooling shape, vbmeta-status getvar surface) that synthesize
> will reuse.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore custom-recovery + system-boot under mode-1 by transplanting stock recovery's embedded vbmeta into the user's custom recovery image; ship three tooling tracks (host script, on-device fastboot oem, flashable ZIP) plus an `AvbParseLib` for our own AVB structure parsing and a `getvar`/`oem` fastboot interface for vbmeta diagnostics.

**Status:** ~~completed~~ **superseded** (commit `4d7eddd` introduced `AvbParseLib`; `scripts/graft-vbmeta.py`, `tools/avb-graft-recovery/`, `zip/avb-graft-installer/`, and `tests/052_graft_vbmeta_roundtrip.sh` all landed alongside — and have since been removed).

**Architecture:** New `GblChainloadPkg/Library/AvbParseLib/` provides minimal AVB structure parsing (footer, header, descriptors) with no link dependency on libavb. New `scripts/graft-vbmeta.py` is a host-side Python tool (Track 1). New fastboot oem commands `graft-and-flash` and `vbmeta-status` plus per-vbmeta `getvar` handlers in the edk2 fork's `FastbootCmds.c` reuse `AvbParseLib`. Flashable ZIP under `zip/avb-graft-installer/` ports the graft logic to a static-linked aarch64 C binary (Track 3). All code is unconditionally compiled — no build flag gating in this plan.

**Tech Stack:** EDK-II (UEFI Application + Library, AArch64), C99, Python 3 stdlib, AnyKernel3-style flashable ZIP, BaseCryptLib's SHA256 (already available in EDK-II), `aarch64-linux-gnu-gcc` cross-compile for the recovery-side static binary.

**Spec reference:** `docs/superpowers/specs/2026-05-10-avb-facade-graft-and-status-design.md`

**Predecessor:** Plan 2 (`v2.0.0-plan2-mode0-logfs-patch9v2`). Repo at `/home/vivy/gbl-chainload`, edk2 submodule at `1vivy/edk2-gbl-chainload`.

---

## File Structure

```
gbl-chainload/
├── GblChainloadPkg/
│   ├── Include/Library/
│   │   └── AvbParseLib.h                      # NEW — public surface
│   └── Library/
│       └── AvbParseLib/                       # NEW
│           ├── AvbParse.c                     #   parser implementation
│           ├── AvbParseLib.inf                #   .inf
│           └── Internal/
│               └── AvbBigEndian.h             #   big-endian read helpers
├── scripts/
│   └── graft-vbmeta.py                        # NEW — Track 1
├── tests/
│   └── avb/                                   # NEW
│       ├── Makefile
│       └── test_avbparse.c                    #   AvbParseLib host tests (TDD)
├── tools/
│   └── avb-graft-recovery/                    # NEW — Track 3 static binary
│       ├── graft-vbmeta.c                     #   small C, host-cross-compiled
│       └── Makefile
├── zip/
│   └── avb-graft-installer/                   # NEW — Track 3 ZIP template
│       ├── META-INF/com/google/android/
│       │   ├── update-binary                  #   AnyKernel3-style bash entry
│       │   └── updater-script                 #   ASSERT lines only
│       └── README.md                          #   user-assembly instructions
edk2-gbl-chainload/                             # submodule, separate repo
└── QcomModulePkg/Library/FastbootLib/
    └── FastbootCmds.c                         # MODIFY — add oem graft-and-flash, getvar vbmeta:*, oem vbmeta-status handlers
```

---

## Task 1: `AvbParseLib` skeleton + `AvbFooter` parser (TDD)

**Files:**
- Create: `GblChainloadPkg/Include/Library/AvbParseLib.h`
- Create: `GblChainloadPkg/Library/AvbParseLib/AvbParse.c`
- Create: `GblChainloadPkg/Library/AvbParseLib/AvbParseLib.inf`
- Create: `GblChainloadPkg/Library/AvbParseLib/Internal/AvbBigEndian.h`
- Create: `tests/avb/test_avbparse.c`
- Create: `tests/avb/Makefile`

### Step 1: Write the public header

Create `GblChainloadPkg/Include/Library/AvbParseLib.h`:

```c
/** @file AvbParseLib.h — minimal AVB structure parser.

    Parses Android Verified Boot 2.0 structures: AvbFooter (last 64 bytes
    of a partition that has an embedded vbmeta), AvbVBMetaImageHeader
    (start of the embedded vbmeta region, magic 'AVB0'), and the descriptor
    iterator for the auxiliary data block.

    Big-endian on disk; readers convert to native uint.

    No link dependency on external/avb/libavb. Field layouts derived from
    libavb headers as reference.
**/
#ifndef AVB_PARSE_LIB_H_
#define AVB_PARSE_LIB_H_

#include <Uefi.h>

#define GBL_AVB_FOOTER_MAGIC        "AVBf"
#define GBL_AVB_VBMETA_MAGIC        "AVB0"
#define GBL_AVB_FOOTER_SIZE         64
#define GBL_AVB_VBMETA_HEADER_SIZE  256

typedef struct {
  UINT32  FooterMajorVersion;
  UINT32  FooterMinorVersion;
  UINT64  OriginalImageSize;
  UINT64  VbmetaOffset;        /* bytes from start of partition */
  UINT64  VbmetaSize;
} GBL_AVB_FOOTER;

typedef struct {
  UINT32  AvbMajorVersion;
  UINT32  AvbMinorVersion;
  UINT64  AuthenticationDataBlockSize;
  UINT64  AuxiliaryDataBlockSize;
  UINT32  AlgorithmType;
  UINT64  HashOffset;
  UINT64  HashSize;
  UINT64  SignatureOffset;
  UINT64  SignatureSize;
  UINT64  PublicKeyOffset;
  UINT64  PublicKeySize;
  UINT64  PublicKeyMetadataOffset;
  UINT64  PublicKeyMetadataSize;
  UINT64  DescriptorsOffset;
  UINT64  DescriptorsSize;
  UINT64  RollbackIndex;
  UINT32  Flags;
  UINT32  RollbackIndexLocation;
  CHAR8   ReleaseString[48];
} GBL_AVB_VBMETA_HEADER;

typedef enum {
  GblAvbDescPropertyTag        = 0,
  GblAvbDescHashtreeTag        = 1,
  GblAvbDescHashTag            = 2,
  GblAvbDescKernelCmdlineTag   = 3,
  GblAvbDescChainPartitionTag  = 4,
} GBL_AVB_DESCRIPTOR_TAG;

/** Parse the AvbFooter from the last 64 bytes of a partition buffer.
    Returns EFI_NOT_FOUND if magic 'AVBf' missing.
    Returns EFI_INVALID_PARAMETER if version unsupported or fields out of range. **/
EFI_STATUS
EFIAPI
AvbParse_Footer (
  IN  CONST UINT8        *Partition,
  IN  UINT64              PartitionSize,
  OUT GBL_AVB_FOOTER     *FooterOut
  );

/** Parse AvbVBMetaImageHeader from a vbmeta byte region.
    Returns EFI_NOT_FOUND if magic 'AVB0' missing.
    Returns EFI_INVALID_PARAMETER if header would extend past VbmetaSize. **/
EFI_STATUS
EFIAPI
AvbParse_VbmetaHeader (
  IN  CONST UINT8                *Vbmeta,
  IN  UINT64                      VbmetaSize,
  OUT GBL_AVB_VBMETA_HEADER      *HeaderOut
  );

/** Iterator over descriptors in the auxiliary data block.
    On first call, pass *Cursor = 0. Subsequent calls advance Cursor.
    Returns EFI_END_OF_MEDIA when no more descriptors. **/
EFI_STATUS
EFIAPI
AvbParse_NextDescriptor (
  IN     CONST UINT8                *AuxBlock,
  IN     UINT64                      AuxSize,
  IN OUT UINT64                     *Cursor,
  OUT    GBL_AVB_DESCRIPTOR_TAG     *TagOut,
  OUT    CONST UINT8               **DescriptorOut,
  OUT    UINT64                     *DescriptorLenOut
  );

/** Parse a hash descriptor's fields. **/
EFI_STATUS
EFIAPI
AvbParse_HashDescriptor (
  IN  CONST UINT8  *Descriptor,
  IN  UINT64        DescriptorLen,
  OUT CONST UINT8 **PartitionNameOut,
  OUT UINT32       *PartitionNameLenOut,
  OUT CONST UINT8 **DigestOut,
  OUT UINT32       *DigestLenOut
  );

/** Parse a chain_partition descriptor's fields. **/
EFI_STATUS
EFIAPI
AvbParse_ChainPartitionDescriptor (
  IN  CONST UINT8  *Descriptor,
  IN  UINT64        DescriptorLen,
  OUT CONST UINT8 **PartitionNameOut,
  OUT UINT32       *PartitionNameLenOut,
  OUT CONST UINT8 **PublicKeyOut,
  OUT UINT32       *PublicKeyLenOut
  );

#endif /* AVB_PARSE_LIB_H_ */
```

- [ ] **Step 2: Write the big-endian helper header**

Create `GblChainloadPkg/Library/AvbParseLib/Internal/AvbBigEndian.h`:

```c
/** @file AvbBigEndian.h — big-endian on-disk integer readers. **/
#ifndef AVB_BIG_ENDIAN_H_
#define AVB_BIG_ENDIAN_H_

#ifdef __HOST_BUILD__
#include <stdint.h>
#include <stddef.h>
typedef uint8_t  UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#define EFIAPI
#define IN
#define OUT
#define STATIC static
#define CONST const
#else
#include <Uefi.h>
#endif

STATIC UINT32
AvbReadU32Be (
  CONST UINT8 *Buf
  )
{
  return ((UINT32)Buf[0] << 24)
       | ((UINT32)Buf[1] << 16)
       | ((UINT32)Buf[2] << 8)
       |  (UINT32)Buf[3];
}

STATIC UINT64
AvbReadU64Be (
  CONST UINT8 *Buf
  )
{
  return ((UINT64)Buf[0] << 56)
       | ((UINT64)Buf[1] << 48)
       | ((UINT64)Buf[2] << 40)
       | ((UINT64)Buf[3] << 32)
       | ((UINT64)Buf[4] << 24)
       | ((UINT64)Buf[5] << 16)
       | ((UINT64)Buf[6] << 8)
       |  (UINT64)Buf[7];
}

#endif
```

- [ ] **Step 3: Write the failing host test for `AvbParse_Footer`**

Create `tests/avb/test_avbparse.c`:

```c
/* Host-compiled tests for AvbParseLib.  Uses synthetic AVB bytes,
   no real partition needed. */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#define IN
#define OUT
#define EFIAPI
#define STATIC static
#define CONST const
typedef uint8_t  UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int      EFI_STATUS;
#define EFI_SUCCESS                0
#define EFI_NOT_FOUND              14
#define EFI_INVALID_PARAMETER      2
#define EFI_END_OF_MEDIA           28
#define EFI_ERROR(s)  ((s) != 0)

#include "../../GblChainloadPkg/Include/Library/AvbParseLib.h"

/* Build a synthetic AvbFooter (64 bytes) at the end of a partition buffer.
   Layout (big-endian, from libavb avb_footer.h):
     magic[4]                = "AVBf"
     version_major (u32)     = 1
     version_minor (u32)     = 0
     original_image_size(u64)
     vbmeta_offset (u64)
     vbmeta_size (u64)
     reserved[28]
*/
static void make_footer (UINT8 *footer64,
                         UINT64 orig_size, UINT64 vbm_off, UINT64 vbm_sz) {
  memset (footer64, 0, 64);
  memcpy (footer64, "AVBf", 4);
  /* version: 1.0 */
  footer64[4]=0; footer64[5]=0; footer64[6]=0; footer64[7]=1;  /* major */
  footer64[8]=0; footer64[9]=0; footer64[10]=0; footer64[11]=0; /* minor */
  /* original_image_size, vbmeta_offset, vbmeta_size — all big-endian u64 */
  for (int i = 0; i < 8; ++i) footer64[12+i] = (orig_size >> (56 - i*8)) & 0xff;
  for (int i = 0; i < 8; ++i) footer64[20+i] = (vbm_off  >> (56 - i*8)) & 0xff;
  for (int i = 0; i < 8; ++i) footer64[28+i] = (vbm_sz   >> (56 - i*8)) & 0xff;
}

static void test_footer_parse_ok (void) {
  UINT8 partition[1024];
  memset (partition, 0xAA, sizeof (partition));
  /* Place footer at offset partition_size - 64 */
  make_footer (partition + 1024 - 64,
               /*orig=*/512, /*vbm_off=*/300, /*vbm_sz=*/200);

  GBL_AVB_FOOTER footer = {0};
  EFI_STATUS s = AvbParse_Footer (partition, 1024, &footer);
  assert (s == EFI_SUCCESS);
  assert (footer.FooterMajorVersion == 1);
  assert (footer.FooterMinorVersion == 0);
  assert (footer.OriginalImageSize  == 512);
  assert (footer.VbmetaOffset       == 300);
  assert (footer.VbmetaSize         == 200);
  printf ("ok test_footer_parse_ok\n");
}

static void test_footer_no_magic (void) {
  UINT8 partition[1024];
  memset (partition, 0xAA, sizeof (partition));
  GBL_AVB_FOOTER footer = {0};
  EFI_STATUS s = AvbParse_Footer (partition, 1024, &footer);
  assert (s == EFI_NOT_FOUND);
  printf ("ok test_footer_no_magic\n");
}

static void test_footer_partition_too_small (void) {
  UINT8 partition[32];
  GBL_AVB_FOOTER footer = {0};
  EFI_STATUS s = AvbParse_Footer (partition, 32, &footer);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_footer_partition_too_small\n");
}

int main (void) {
  test_footer_parse_ok ();
  test_footer_no_magic ();
  test_footer_partition_too_small ();
  printf ("ALL PASS\n");
  return 0;
}
```

- [ ] **Step 4: Write the test Makefile**

Create `tests/avb/Makefile`:

```makefile
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -D__HOST_BUILD__
PROJ    := $(realpath ../..)

TESTS   := test_avbparse

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_avbparse: test_avbparse.c $(PROJ)/GblChainloadPkg/Library/AvbParseLib/AvbParse.c
	$(CC) $(CFLAGS) \
	  -I$(PROJ)/GblChainloadPkg/Library/AvbParseLib/Internal \
	  -I$(PROJ)/GblChainloadPkg/Include/Library \
	  $^ -o $@

clean:
	rm -f $(TESTS) *.o
```

- [ ] **Step 5: Run the test — must FAIL (no impl yet)**

```bash
cd /home/vivy/gbl-chainload/tests/avb && make
```

Expected: build error (`AvbParse.c not found` or undefined reference).

- [ ] **Step 6: Implement `AvbParse_Footer` in `AvbParse.c`**

Create `GblChainloadPkg/Library/AvbParseLib/AvbParse.c`:

```c
/** @file AvbParse.c — AVB structure parser. **/
#include "../../Include/Library/AvbParseLib.h"
#include "Internal/AvbBigEndian.h"

EFI_STATUS
EFIAPI
AvbParse_Footer (
  IN  CONST UINT8        *Partition,
  IN  UINT64              PartitionSize,
  OUT GBL_AVB_FOOTER     *FooterOut
  )
{
  CONST UINT8 *Footer;

  if (Partition == NULL || FooterOut == NULL)        return EFI_INVALID_PARAMETER;
  if (PartitionSize < GBL_AVB_FOOTER_SIZE)           return EFI_INVALID_PARAMETER;

  Footer = Partition + PartitionSize - GBL_AVB_FOOTER_SIZE;

  if (Footer[0] != 'A' || Footer[1] != 'V'
      || Footer[2] != 'B' || Footer[3] != 'f') {
    return EFI_NOT_FOUND;
  }

  FooterOut->FooterMajorVersion  = AvbReadU32Be (Footer + 4);
  FooterOut->FooterMinorVersion  = AvbReadU32Be (Footer + 8);
  FooterOut->OriginalImageSize   = AvbReadU64Be (Footer + 12);
  FooterOut->VbmetaOffset        = AvbReadU64Be (Footer + 20);
  FooterOut->VbmetaSize          = AvbReadU64Be (Footer + 28);

  /* Sanity: vbmeta_offset + vbmeta_size must be <= partition size. */
  if (FooterOut->VbmetaOffset + FooterOut->VbmetaSize > PartitionSize) {
    return EFI_INVALID_PARAMETER;
  }
  /* Sanity: original_image_size <= partition size. */
  if (FooterOut->OriginalImageSize > PartitionSize) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/* Stubs for later tasks — return EFI_UNSUPPORTED so test binary links. */
EFI_STATUS EFIAPI AvbParse_VbmetaHeader  (CONST UINT8 *V, UINT64 S, GBL_AVB_VBMETA_HEADER *H) { return -1; }
EFI_STATUS EFIAPI AvbParse_NextDescriptor (CONST UINT8 *A, UINT64 S, UINT64 *C, GBL_AVB_DESCRIPTOR_TAG *T, CONST UINT8 **D, UINT64 *L) { return -1; }
EFI_STATUS EFIAPI AvbParse_HashDescriptor (CONST UINT8 *D, UINT64 L, CONST UINT8 **N, UINT32 *NL, CONST UINT8 **DG, UINT32 *DGL) { return -1; }
EFI_STATUS EFIAPI AvbParse_ChainPartitionDescriptor (CONST UINT8 *D, UINT64 L, CONST UINT8 **N, UINT32 *NL, CONST UINT8 **PK, UINT32 *PKL) { return -1; }
```

(The unsigned `EFI_STATUS = -1` works in host build because `EFI_STATUS` is `int`. EDK-II builds will replace these with proper implementations in Tasks 2-3.)

- [ ] **Step 7: Run tests — must PASS (3 footer tests)**

```bash
cd /home/vivy/gbl-chainload/tests/avb && make clean && make
```

Expected:
```
ok test_footer_parse_ok
ok test_footer_no_magic
ok test_footer_partition_too_small
ALL PASS
```

- [ ] **Step 8: Author `AvbParseLib.inf`**

Create `GblChainloadPkg/Library/AvbParseLib/AvbParseLib.inf`:

```ini
[Defines]
  INF_VERSION    = 0x00010005
  BASE_NAME      = AvbParseLib
  FILE_GUID      = a8c4b021-cccc-4321-d000-fedcba012345
  MODULE_TYPE    = UEFI_APPLICATION
  VERSION_STRING = 1.0
  LIBRARY_CLASS  = AvbParseLib

[Sources]
  AvbParse.c

[Packages]
  MdePkg/MdePkg.dec
  GblChainloadPkg/GblChainloadPkg.dec

[LibraryClasses]
  BaseLib
  BaseMemoryLib
```

Generate a fresh GUID with `uuidgen`; replace the placeholder above.

- [ ] **Step 9: Add `AvbParseLib` to the package's DSC**

Edit `GblChainloadPkg/GblChainloadPkg.dsc`. Find `[LibraryClasses]` (the same block that maps `DynamicPatchLib`, `LogFsLib`, etc.) and add:

```ini
  AvbParseLib|GblChainloadPkg/Library/AvbParseLib/AvbParseLib.inf
```

Verify the build still works:

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 2>&1 | tail -10
```

Expected: clean build (no link errors yet — AvbParseLib isn't called by anything).

- [ ] **Step 10: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Include/Library/AvbParseLib.h \
            GblChainloadPkg/Library/AvbParseLib \
            tests/avb \
  && git commit -m "AvbParseLib: skeleton + AvbFooter parser with TDD (3 tests)"
```

---

## Task 2: `AvbParse_VbmetaHeader` (TDD)

**Files:**
- Modify: `GblChainloadPkg/Library/AvbParseLib/AvbParse.c`
- Modify: `tests/avb/test_avbparse.c`

- [ ] **Step 1: Add the failing tests**

Append to `tests/avb/test_avbparse.c` (before `int main`):

```c
/* Synthetic AvbVBMetaImageHeader (big-endian).
   Layout from external/avb/libavb/avb_vbmeta_image.h:
     magic[4]          = "AVB0"
     required_libavb_version_major (u32)
     required_libavb_version_minor (u32)
     authentication_data_block_size (u64)
     auxiliary_data_block_size (u64)
     algorithm_type (u32)
     hash_offset (u64)
     hash_size (u64)
     signature_offset (u64)
     signature_size (u64)
     public_key_offset (u64)
     public_key_size (u64)
     public_key_metadata_offset (u64)
     public_key_metadata_size (u64)
     descriptors_offset (u64)
     descriptors_size (u64)
     rollback_index (u64)
     flags (u32)
     rollback_index_location (u32)
     release_string[48]
     reserved[80]
*/
static void put_u32_be (UINT8 *p, UINT32 v) {
  p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff;
  p[2] = (v >> 8) & 0xff;  p[3] = v & 0xff;
}
static void put_u64_be (UINT8 *p, UINT64 v) {
  for (int i = 0; i < 8; ++i) p[i] = (v >> (56 - i*8)) & 0xff;
}

static void make_vbmeta_header (UINT8 *out256,
                                 UINT64 auth_size, UINT64 aux_size,
                                 UINT32 algo, UINT64 desc_off, UINT64 desc_size,
                                 UINT32 flags) {
  memset (out256, 0, 256);
  memcpy (out256, "AVB0", 4);
  put_u32_be (out256 + 4, 1);                 /* major */
  put_u32_be (out256 + 8, 1);                 /* minor */
  put_u64_be (out256 + 12, auth_size);
  put_u64_be (out256 + 20, aux_size);
  put_u32_be (out256 + 28, algo);
  put_u64_be (out256 + 32, 0);                /* hash_offset */
  put_u64_be (out256 + 40, 32);               /* hash_size */
  put_u64_be (out256 + 48, 32);               /* signature_offset */
  put_u64_be (out256 + 56, 256);              /* signature_size */
  put_u64_be (out256 + 64, 288);              /* pubkey_offset */
  put_u64_be (out256 + 72, 1024);             /* pubkey_size */
  put_u64_be (out256 + 80, 1312);             /* pubkey_meta_offset */
  put_u64_be (out256 + 88, 0);                /* pubkey_meta_size */
  put_u64_be (out256 + 96, desc_off);         /* descriptors_offset */
  put_u64_be (out256 + 104, desc_size);
  put_u64_be (out256 + 112, 0);               /* rollback_index */
  put_u32_be (out256 + 120, flags);
  put_u32_be (out256 + 124, 0);               /* rollback_index_location */
  /* release_string at offset 128, 48 bytes.  Leave zero. */
}

static void test_header_parse_ok (void) {
  UINT8 region[2048];
  memset (region, 0, sizeof (region));
  make_vbmeta_header (region, /*auth=*/256, /*aux=*/512, /*algo=*/1,
                      /*desc_off=*/1312, /*desc_size=*/256, /*flags=*/0);

  GBL_AVB_VBMETA_HEADER hdr = {0};
  EFI_STATUS s = AvbParse_VbmetaHeader (region, sizeof (region), &hdr);
  assert (s == EFI_SUCCESS);
  assert (hdr.AvbMajorVersion == 1);
  assert (hdr.AvbMinorVersion == 1);
  assert (hdr.AuthenticationDataBlockSize == 256);
  assert (hdr.AuxiliaryDataBlockSize == 512);
  assert (hdr.AlgorithmType == 1);
  assert (hdr.DescriptorsOffset == 1312);
  assert (hdr.DescriptorsSize == 256);
  printf ("ok test_header_parse_ok\n");
}

static void test_header_no_magic (void) {
  UINT8 region[256];
  memset (region, 0xCC, sizeof (region));
  GBL_AVB_VBMETA_HEADER hdr = {0};
  EFI_STATUS s = AvbParse_VbmetaHeader (region, sizeof (region), &hdr);
  assert (s == EFI_NOT_FOUND);
  printf ("ok test_header_no_magic\n");
}

static void test_header_too_small (void) {
  UINT8 region[100];
  memset (region, 0, sizeof (region));
  memcpy (region, "AVB0", 4);
  GBL_AVB_VBMETA_HEADER hdr = {0};
  EFI_STATUS s = AvbParse_VbmetaHeader (region, sizeof (region), &hdr);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_header_too_small\n");
}
```

In `main ()`, call the new tests after the footer ones:

```c
  test_header_parse_ok ();
  test_header_no_magic ();
  test_header_too_small ();
```

- [ ] **Step 2: Run tests — header tests must FAIL**

```bash
cd /home/vivy/gbl-chainload/tests/avb && make clean && make
```

Expected: 3 footer tests pass; 3 header tests fail (stub returns -1 not EFI_SUCCESS, assertion fires).

- [ ] **Step 3: Implement `AvbParse_VbmetaHeader`**

Replace the stub `AvbParse_VbmetaHeader` in `AvbParse.c` with:

```c
EFI_STATUS
EFIAPI
AvbParse_VbmetaHeader (
  IN  CONST UINT8                *Vbmeta,
  IN  UINT64                      VbmetaSize,
  OUT GBL_AVB_VBMETA_HEADER      *HeaderOut
  )
{
  if (Vbmeta == NULL || HeaderOut == NULL)         return EFI_INVALID_PARAMETER;
  if (VbmetaSize < GBL_AVB_VBMETA_HEADER_SIZE)     return EFI_INVALID_PARAMETER;

  if (Vbmeta[0] != 'A' || Vbmeta[1] != 'V'
      || Vbmeta[2] != 'B' || Vbmeta[3] != '0') {
    return EFI_NOT_FOUND;
  }

  HeaderOut->AvbMajorVersion              = AvbReadU32Be (Vbmeta + 4);
  HeaderOut->AvbMinorVersion              = AvbReadU32Be (Vbmeta + 8);
  HeaderOut->AuthenticationDataBlockSize  = AvbReadU64Be (Vbmeta + 12);
  HeaderOut->AuxiliaryDataBlockSize       = AvbReadU64Be (Vbmeta + 20);
  HeaderOut->AlgorithmType                = AvbReadU32Be (Vbmeta + 28);
  HeaderOut->HashOffset                   = AvbReadU64Be (Vbmeta + 32);
  HeaderOut->HashSize                     = AvbReadU64Be (Vbmeta + 40);
  HeaderOut->SignatureOffset              = AvbReadU64Be (Vbmeta + 48);
  HeaderOut->SignatureSize                = AvbReadU64Be (Vbmeta + 56);
  HeaderOut->PublicKeyOffset              = AvbReadU64Be (Vbmeta + 64);
  HeaderOut->PublicKeySize                = AvbReadU64Be (Vbmeta + 72);
  HeaderOut->PublicKeyMetadataOffset      = AvbReadU64Be (Vbmeta + 80);
  HeaderOut->PublicKeyMetadataSize        = AvbReadU64Be (Vbmeta + 88);
  HeaderOut->DescriptorsOffset            = AvbReadU64Be (Vbmeta + 96);
  HeaderOut->DescriptorsSize              = AvbReadU64Be (Vbmeta + 104);
  HeaderOut->RollbackIndex                = AvbReadU64Be (Vbmeta + 112);
  HeaderOut->Flags                        = AvbReadU32Be (Vbmeta + 120);
  HeaderOut->RollbackIndexLocation        = AvbReadU32Be (Vbmeta + 124);
  /* release_string is at offset 128, length 48. */
  for (int i = 0; i < 48; ++i) HeaderOut->ReleaseString[i] = (CHAR8)Vbmeta[128 + i];

  /* Sanity: header + auth + aux <= VbmetaSize. */
  UINT64 Total = (UINT64)GBL_AVB_VBMETA_HEADER_SIZE
                + HeaderOut->AuthenticationDataBlockSize
                + HeaderOut->AuxiliaryDataBlockSize;
  if (Total > VbmetaSize) return EFI_INVALID_PARAMETER;

  return EFI_SUCCESS;
}
```

`CHAR8` may need `typedef char CHAR8;` in the host shim — it's already in our project's host shim per ScanLib pattern. Add to `AvbBigEndian.h`'s host block if missing:

```c
#ifdef __HOST_BUILD__
#ifndef CHAR8
typedef char CHAR8;
#endif
#endif
```

- [ ] **Step 4: Run tests — all 6 must pass**

```bash
cd /home/vivy/gbl-chainload/tests/avb && make clean && make
```

Expected: 3 footer + 3 header = 6 passes. ALL PASS.

- [ ] **Step 5: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Library/AvbParseLib/AvbParse.c \
            GblChainloadPkg/Library/AvbParseLib/Internal/AvbBigEndian.h \
            tests/avb/test_avbparse.c \
  && git commit -m "AvbParseLib: AvbVBMetaImageHeader parser with TDD (3 tests)"
```

---

## Task 3: Descriptor iterator + Hash + Chain parsers (TDD)

**Files:**
- Modify: `GblChainloadPkg/Library/AvbParseLib/AvbParse.c`
- Modify: `tests/avb/test_avbparse.c`

- [ ] **Step 1: Add failing tests**

Append to `tests/avb/test_avbparse.c`:

```c
/* AvbDescriptor common header (big-endian):
     tag (u64)
     num_bytes_following (u64)
   Then descriptor-specific fields.

   AvbHashDescriptor (tag=2):
     header (16 bytes)
     image_size (u64)
     hash_algorithm[32]
     partition_name_len (u32)
     salt_len (u32)
     digest_len (u32)
     flags (u32)
     reserved[60]
     partition_name[partition_name_len]
     salt[salt_len]
     digest[digest_len]
     padding to 8-byte alignment

   AvbChainPartitionDescriptor (tag=4):
     header (16 bytes)
     rollback_index_location (u32)
     partition_name_len (u32)
     public_key_len (u32)
     reserved[64]
     partition_name[partition_name_len]
     public_key[public_key_len]
     padding
*/

static void put_desc_header (UINT8 *p, UINT64 tag, UINT64 num_bytes_following) {
  put_u64_be (p, tag);
  put_u64_be (p + 8, num_bytes_following);
}

static UINT64 align8 (UINT64 v) { return (v + 7) & ~(UINT64)7; }

static UINT64 build_hash_descriptor (UINT8 *out, CONST char *name, CONST UINT8 *digest, UINT32 digest_len) {
  UINT32 name_len = (UINT32)strlen (name);
  UINT64 body_size = 16 + 32 + 4 + 4 + 4 + 4 + 60 + name_len + 0 + digest_len; /* salt_len=0 */
  body_size = align8 (body_size);
  UINT64 num_bytes_following = body_size - 16;
  put_desc_header (out, 2, num_bytes_following);  /* tag=2 hash */
  put_u64_be (out + 16, 1024);                /* image_size */
  memcpy (out + 24, "sha256", 6);             /* hash_algorithm */
  put_u32_be (out + 56, name_len);
  put_u32_be (out + 60, 0);                   /* salt_len */
  put_u32_be (out + 64, digest_len);
  put_u32_be (out + 68, 0);                   /* flags */
  /* reserved[60] at offset 72 */
  memcpy (out + 132, name, name_len);
  memcpy (out + 132 + name_len + 0, digest, digest_len);
  return body_size;
}

static UINT64 build_chain_descriptor (UINT8 *out, CONST char *name, CONST UINT8 *pk, UINT32 pk_len) {
  UINT32 name_len = (UINT32)strlen (name);
  UINT64 body_size = 16 + 4 + 4 + 4 + 4 + 60 + name_len + pk_len;
  body_size = align8 (body_size);
  UINT64 num_bytes_following = body_size - 16;
  put_desc_header (out, 4, num_bytes_following);  /* tag=4 chain */
  put_u32_be (out + 16, 0);                   /* rollback_index_location */
  put_u32_be (out + 20, name_len);
  put_u32_be (out + 24, pk_len);
  put_u32_be (out + 28, 0);                   /* flags */
  /* reserved[60] at offset 32 */
  memcpy (out + 92, name, name_len);
  memcpy (out + 92 + name_len, pk, pk_len);
  return body_size;
}

static void test_descriptor_iterator (void) {
  UINT8 aux[2048];
  memset (aux, 0, sizeof (aux));
  UINT8 dummy_digest[32]; memset (dummy_digest, 0xDD, 32);
  UINT8 dummy_pk[64];      memset (dummy_pk, 0xEE, 64);

  UINT64 d1 = build_hash_descriptor  (aux + 0, "init_boot", dummy_digest, 32);
  UINT64 d2 = build_chain_descriptor (aux + d1, "recovery", dummy_pk, 64);
  UINT64 total = d1 + d2;

  UINT64 cursor = 0;
  GBL_AVB_DESCRIPTOR_TAG tag;
  CONST UINT8 *desc;
  UINT64 desc_len;

  EFI_STATUS s = AvbParse_NextDescriptor (aux, total, &cursor, &tag, &desc, &desc_len);
  assert (s == EFI_SUCCESS);
  assert (tag == GblAvbDescHashTag);
  assert (desc_len == d1);
  printf ("ok test_descriptor_iter first=hash\n");

  s = AvbParse_NextDescriptor (aux, total, &cursor, &tag, &desc, &desc_len);
  assert (s == EFI_SUCCESS);
  assert (tag == GblAvbDescChainPartitionTag);
  assert (desc_len == d2);
  printf ("ok test_descriptor_iter second=chain\n");

  s = AvbParse_NextDescriptor (aux, total, &cursor, &tag, &desc, &desc_len);
  assert (s == EFI_END_OF_MEDIA);
  printf ("ok test_descriptor_iter end\n");
}

static void test_parse_hash_descriptor (void) {
  UINT8 desc[256];
  memset (desc, 0, sizeof (desc));
  UINT8 digest[32]; memset (digest, 0xAB, 32);
  UINT64 dlen = build_hash_descriptor (desc, "init_boot", digest, 32);

  CONST UINT8 *name; UINT32 name_len;
  CONST UINT8 *out_digest; UINT32 out_dlen;
  EFI_STATUS s = AvbParse_HashDescriptor (desc, dlen, &name, &name_len, &out_digest, &out_dlen);
  assert (s == EFI_SUCCESS);
  assert (name_len == 9);
  assert (memcmp (name, "init_boot", 9) == 0);
  assert (out_dlen == 32);
  assert (memcmp (out_digest, digest, 32) == 0);
  printf ("ok test_parse_hash_descriptor\n");
}

static void test_parse_chain_descriptor (void) {
  UINT8 desc[256];
  memset (desc, 0, sizeof (desc));
  UINT8 pk[64]; memset (pk, 0xCD, 64);
  UINT64 dlen = build_chain_descriptor (desc, "recovery", pk, 64);

  CONST UINT8 *name; UINT32 name_len;
  CONST UINT8 *out_pk; UINT32 out_pk_len;
  EFI_STATUS s = AvbParse_ChainPartitionDescriptor (desc, dlen, &name, &name_len, &out_pk, &out_pk_len);
  assert (s == EFI_SUCCESS);
  assert (name_len == 8);
  assert (memcmp (name, "recovery", 8) == 0);
  assert (out_pk_len == 64);
  assert (memcmp (out_pk, pk, 64) == 0);
  printf ("ok test_parse_chain_descriptor\n");
}
```

In `main ()`, append:

```c
  test_descriptor_iterator ();
  test_parse_hash_descriptor ();
  test_parse_chain_descriptor ();
```

- [ ] **Step 2: Run — descriptor tests must FAIL**

```bash
cd /home/vivy/gbl-chainload/tests/avb && make clean && make
```

Expected: prior 6 pass; new 5 fail.

- [ ] **Step 3: Implement the descriptor iterator + parsers**

Replace the three stubs in `AvbParse.c` with:

```c
EFI_STATUS
EFIAPI
AvbParse_NextDescriptor (
  IN     CONST UINT8                *AuxBlock,
  IN     UINT64                      AuxSize,
  IN OUT UINT64                     *Cursor,
  OUT    GBL_AVB_DESCRIPTOR_TAG     *TagOut,
  OUT    CONST UINT8               **DescriptorOut,
  OUT    UINT64                     *DescriptorLenOut
  )
{
  UINT64 Tag, NumBytesFollowing, Total;

  if (AuxBlock == NULL || Cursor == NULL || TagOut == NULL
      || DescriptorOut == NULL || DescriptorLenOut == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (*Cursor >= AuxSize)                 return EFI_END_OF_MEDIA;
  if (AuxSize - *Cursor < 16)             return EFI_END_OF_MEDIA;

  Tag                = AvbReadU64Be (AuxBlock + *Cursor);
  NumBytesFollowing  = AvbReadU64Be (AuxBlock + *Cursor + 8);
  Total              = 16 + NumBytesFollowing;

  if (*Cursor + Total > AuxSize)          return EFI_INVALID_PARAMETER;

  *TagOut            = (GBL_AVB_DESCRIPTOR_TAG)Tag;
  *DescriptorOut     = AuxBlock + *Cursor;
  *DescriptorLenOut  = Total;
  *Cursor           += Total;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AvbParse_HashDescriptor (
  IN  CONST UINT8  *Descriptor,
  IN  UINT64        DescriptorLen,
  OUT CONST UINT8 **PartitionNameOut,
  OUT UINT32       *PartitionNameLenOut,
  OUT CONST UINT8 **DigestOut,
  OUT UINT32       *DigestLenOut
  )
{
  UINT32 NameLen, SaltLen, DigestLen;
  UINT64 BodyStart;

  if (Descriptor == NULL || PartitionNameOut == NULL || PartitionNameLenOut == NULL
      || DigestOut == NULL || DigestLenOut == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (DescriptorLen < 132)                return EFI_INVALID_PARAMETER;

  /* Header at +0 (16 bytes). image_size at +16. hash_algorithm at +24..56.
     partition_name_len at +56, salt_len at +60, digest_len at +64,
     flags at +68, reserved at +72..132, then variable-length fields. */
  NameLen   = AvbReadU32Be (Descriptor + 56);
  SaltLen   = AvbReadU32Be (Descriptor + 60);
  DigestLen = AvbReadU32Be (Descriptor + 64);

  BodyStart = 132;

  if ((UINT64)NameLen + SaltLen + DigestLen + BodyStart > DescriptorLen) {
    return EFI_INVALID_PARAMETER;
  }

  *PartitionNameOut    = Descriptor + BodyStart;
  *PartitionNameLenOut = NameLen;
  *DigestOut           = Descriptor + BodyStart + NameLen + SaltLen;
  *DigestLenOut        = DigestLen;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AvbParse_ChainPartitionDescriptor (
  IN  CONST UINT8  *Descriptor,
  IN  UINT64        DescriptorLen,
  OUT CONST UINT8 **PartitionNameOut,
  OUT UINT32       *PartitionNameLenOut,
  OUT CONST UINT8 **PublicKeyOut,
  OUT UINT32       *PublicKeyLenOut
  )
{
  UINT32 NameLen, PkLen;
  UINT64 BodyStart;

  if (Descriptor == NULL || PartitionNameOut == NULL || PartitionNameLenOut == NULL
      || PublicKeyOut == NULL || PublicKeyLenOut == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (DescriptorLen < 92)                 return EFI_INVALID_PARAMETER;

  /* Header at +0 (16 bytes). rollback_idx_loc at +16, partition_name_len at +20,
     public_key_len at +24, flags at +28, reserved at +32..92, then variable. */
  NameLen = AvbReadU32Be (Descriptor + 20);
  PkLen   = AvbReadU32Be (Descriptor + 24);
  BodyStart = 92;

  if ((UINT64)NameLen + PkLen + BodyStart > DescriptorLen) {
    return EFI_INVALID_PARAMETER;
  }

  *PartitionNameOut    = Descriptor + BodyStart;
  *PartitionNameLenOut = NameLen;
  *PublicKeyOut        = Descriptor + BodyStart + NameLen;
  *PublicKeyLenOut     = PkLen;

  return EFI_SUCCESS;
}
```

- [ ] **Step 4: Run — all 11 tests pass**

```bash
cd /home/vivy/gbl-chainload/tests/avb && make clean && make
```

Expected:
```
ok test_footer_parse_ok
ok test_footer_no_magic
ok test_footer_partition_too_small
ok test_header_parse_ok
ok test_header_no_magic
ok test_header_too_small
ok test_descriptor_iter first=hash
ok test_descriptor_iter second=chain
ok test_descriptor_iter end
ok test_parse_hash_descriptor
ok test_parse_chain_descriptor
ALL PASS
```

- [ ] **Step 5: Run repo-wide runall — must still pass**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -10
```

Expected: ALL TESTS PASS (existing scan/encode/engine + new avb tests).

If `tests/runall.sh` doesn't include `tests/avb/`, add it:

```bash
# in tests/runall.sh, near the other test dirs:
echo "== tests/avb =="
make -C tests/avb clean >/dev/null
make -C tests/avb
```

- [ ] **Step 6: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Library/AvbParseLib/AvbParse.c \
            tests/avb/test_avbparse.c \
            tests/runall.sh \
  && git commit -m "AvbParseLib: descriptor iterator + Hash/Chain parsers with TDD (5 tests)"
```

---

## Task 4: `scripts/graft-vbmeta.py` — Track 1 host script

**Files:**
- Create: `scripts/graft-vbmeta.py`
- Create: `tests/052_graft_vbmeta_roundtrip.sh`

- [ ] **Step 1: Write the host script**

Create `scripts/graft-vbmeta.py`:

```python
#!/usr/bin/env python3
"""graft-vbmeta.py — transplant stock partition's embedded vbmeta region into a custom image.

Reads stock partition's AvbFooter (last 64 bytes), parses it, copies the
[partition_size - vbmeta_offset - vbmeta_size, partition_size] byte range
from stock onto the corresponding range of custom, and writes the result.

Usage:
    graft-vbmeta.py --partition recovery --stock STOCK.img --custom CUSTOM.img --out OUT.img
"""
import argparse
import struct
import sys
from pathlib import Path


AVB_FOOTER_MAGIC = b"AVBf"
AVB_FOOTER_SIZE  = 64
AVB_VBMETA_MAGIC = b"AVB0"


class GraftError(RuntimeError):
    pass


def parse_footer(partition: bytes) -> dict:
    if len(partition) < AVB_FOOTER_SIZE:
        raise GraftError(f"partition too small ({len(partition)} bytes) for AvbFooter")
    footer = partition[-AVB_FOOTER_SIZE:]
    if footer[:4] != AVB_FOOTER_MAGIC:
        raise GraftError(f"AvbFooter magic missing (got {footer[:4]!r}, want {AVB_FOOTER_MAGIC!r})")
    # Big-endian: major (u32), minor (u32), original_image_size (u64), vbmeta_offset (u64), vbmeta_size (u64)
    major, minor = struct.unpack(">II", footer[4:12])
    orig_size, vbm_off, vbm_size = struct.unpack(">QQQ", footer[12:36])
    if vbm_off + vbm_size > len(partition):
        raise GraftError(f"AvbFooter inconsistent: vbmeta_offset={vbm_off} + size={vbm_size} > partition={len(partition)}")
    return {
        "major": major, "minor": minor,
        "original_image_size": orig_size,
        "vbmeta_offset": vbm_off,
        "vbmeta_size": vbm_size,
    }


def parse_vbmeta_header(vbmeta: bytes) -> dict:
    if len(vbmeta) < 256:
        raise GraftError(f"vbmeta region too small ({len(vbmeta)} bytes) for AvbVBMetaImageHeader")
    if vbmeta[:4] != AVB_VBMETA_MAGIC:
        raise GraftError(f"AvbVBMetaImageHeader magic missing (got {vbmeta[:4]!r}, want {AVB_VBMETA_MAGIC!r})")
    major, minor = struct.unpack(">II", vbmeta[4:12])
    auth_sz, aux_sz = struct.unpack(">QQ", vbmeta[12:28])
    algo, = struct.unpack(">I", vbmeta[28:32])
    return {
        "major": major, "minor": minor,
        "auth_size": auth_sz, "aux_size": aux_sz,
        "algorithm_type": algo,
    }


def graft(partition_label: str, stock_path: Path, custom_path: Path, out_path: Path) -> None:
    stock = stock_path.read_bytes()
    custom = custom_path.read_bytes()

    if len(stock) != len(custom):
        # Permit different sizes only if custom <= stock; we'll pad custom with zeros up to stock size.
        if len(custom) > len(stock):
            raise GraftError(f"custom image larger ({len(custom)}) than stock partition size ({len(stock)})")
        custom = custom + bytes(len(stock) - len(custom))
        print(f"NOTE: padded custom from {custom_path.stat().st_size} to {len(custom)} bytes "
              f"to match stock partition size", file=sys.stderr)

    footer = parse_footer(stock)
    print(f"donor footer: vbmeta_offset={footer['vbmeta_offset']} size={footer['vbmeta_size']} "
          f"orig_image={footer['original_image_size']}")

    # The vbmeta region in stock is at [vbmeta_offset, vbmeta_offset + vbmeta_size).
    # The footer itself is at [partition_size - 64, partition_size).
    # Both regions are at the END of the partition; we transplant both.
    vbm_start = footer["vbmeta_offset"]
    vbm_end   = vbm_start + footer["vbmeta_size"]
    if vbm_end > len(stock):
        raise GraftError(f"vbmeta region extends beyond partition (end={vbm_end}, partition={len(stock)})")

    # Sanity: parse the vbmeta header from stock to confirm it's valid.
    header = parse_vbmeta_header(stock[vbm_start:vbm_end])
    print(f"donor vbmeta header: avb={header['major']}.{header['minor']} algo={header['algorithm_type']} "
          f"auth_size={header['auth_size']} aux_size={header['aux_size']}")

    # Compose grafted output.  Layout: custom[0:vbm_start] + stock[vbm_start:vbm_end] + stock[-64:] (footer).
    # But the regions are contiguous: vbmeta_size + 64 bytes at the tail in stock.
    # Replace the corresponding tail of custom with stock's tail.
    tail_size = (len(stock) - vbm_start)  # vbmeta region + footer + any padding before footer
    grafted = custom[:vbm_start] + stock[vbm_start:]

    if len(grafted) != len(stock):
        raise GraftError(f"graft size mismatch (got {len(grafted)}, want {len(stock)})")

    # Verify the grafted output's footer parses cleanly.
    re_footer = parse_footer(grafted)
    assert re_footer == footer, "round-trip footer mismatch"

    out_path.write_bytes(grafted)
    print(f"wrote {out_path} ({len(grafted)} bytes); replaced last {tail_size} bytes "
          f"with donor's vbmeta region + footer")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--partition", required=True, help="recovery, dtbo, etc. (informational)")
    ap.add_argument("--stock",  required=True, type=Path)
    ap.add_argument("--custom", required=True, type=Path)
    ap.add_argument("--out",    required=True, type=Path)
    args = ap.parse_args()
    try:
        graft(args.partition, args.stock, args.custom, args.out)
    except GraftError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

`chmod +x scripts/graft-vbmeta.py`.

- [ ] **Step 2: Write a roundtrip test**

Create `tests/052_graft_vbmeta_roundtrip.sh`:

```bash
#!/usr/bin/env bash
# 052_graft_vbmeta_roundtrip.sh — verify graft script round-trips correctly.
# Construct synthetic stock + custom images; graft; assert output's vbmeta
# region equals stock's; assert content above the vbmeta region equals
# custom's.
set -euo pipefail
cd "$(dirname "$0")/.."

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 <<'PY' "$TMP"
import os, sys, struct
tmp = sys.argv[1]
SIZE = 4096

# Build stock partition: 0xAA padding + AvbVBMetaImageHeader at 3000 + AvbFooter.
stock = bytearray(SIZE)
for i in range(SIZE):
    stock[i] = 0xAA

# Vbmeta header at offset 3000, size 256.
vbm_off = 3000
vbm_size = 256
hdr = bytearray(256)
hdr[0:4] = b"AVB0"
struct.pack_into(">II", hdr, 4, 1, 1)        # major, minor
struct.pack_into(">QQ", hdr, 12, 64, 128)    # auth, aux
struct.pack_into(">I",  hdr, 28, 1)          # algo
stock[vbm_off:vbm_off+vbm_size] = hdr

# AvbFooter at last 64 bytes.
footer = bytearray(64)
footer[0:4] = b"AVBf"
struct.pack_into(">II", footer, 4, 1, 0)     # major, minor
struct.pack_into(">QQQ", footer, 12, SIZE, vbm_off, vbm_size)
stock[SIZE-64:] = footer

# Custom partition: all 0xCC; no AVB structures at all.
custom = bytearray(SIZE)
for i in range(SIZE):
    custom[i] = 0xCC

with open(os.path.join(tmp, "stock.img"),  "wb") as f: f.write(stock)
with open(os.path.join(tmp, "custom.img"), "wb") as f: f.write(custom)
print("synthetic images written")
PY

python3 scripts/graft-vbmeta.py \
  --partition recovery \
  --stock "$TMP/stock.img" \
  --custom "$TMP/custom.img" \
  --out "$TMP/grafted.img"

# Verify output: bytes [0..3000) == custom; bytes [3000..end] == stock.
python3 <<PY "$TMP"
import sys
tmp = sys.argv[1]
stock   = open(f"{tmp}/stock.img",  "rb").read()
custom  = open(f"{tmp}/custom.img", "rb").read()
grafted = open(f"{tmp}/grafted.img","rb").read()
assert len(grafted) == len(stock), f"size mismatch {len(grafted)} != {len(stock)}"
assert grafted[:3000] == custom[:3000],  "above vbmeta: should be from custom"
assert grafted[3000:] == stock[3000:],   "vbmeta + footer: should be from stock"
print("roundtrip OK")
PY

echo "ok 052_graft_vbmeta_roundtrip"
```

`chmod +x tests/052_graft_vbmeta_roundtrip.sh`.

- [ ] **Step 3: Run the test**

```bash
cd /home/vivy/gbl-chainload && bash tests/052_graft_vbmeta_roundtrip.sh
```

Expected: `roundtrip OK` and `ok 052_graft_vbmeta_roundtrip`.

- [ ] **Step 4: Add to runall.sh**

In `tests/runall.sh`, add (alongside the other test scripts):

```bash
echo "== 052_graft_vbmeta_roundtrip =="
bash tests/052_graft_vbmeta_roundtrip.sh
```

Run runall:

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -10
```

Expected: ALL TESTS PASS.

- [ ] **Step 5: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add scripts/graft-vbmeta.py \
            tests/052_graft_vbmeta_roundtrip.sh \
            tests/runall.sh \
  && git commit -m "scripts/graft-vbmeta: Track 1 host Python script + roundtrip test"
```

---

## Task 5: `fastboot oem graft-and-flash` — Track 2 (edk2 fork)

**Files:**
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` (in our edk2-gbl-chainload submodule)

This task adds the on-device fastboot oem command. Unlike the other tasks, the changes land in the edk2 submodule (separate repo), and the gbl-chainload repo gets a submodule pointer bump at the end.

- [ ] **Step 1: Inspect the existing FastbootCmds.c structure**

```bash
cd /home/vivy/gbl-chainload/edk2 \
  && grep -nE 'CmdOemEscape|GBL_EXPERIMENTAL_FASTBOOT_CMDS' \
  QcomModulePkg/Library/FastbootLib/FastbootCmds.c | head -20
```

Find the `cmd_list[]` (or equivalent) where commands like `CmdOemEscape` are registered. New commands go alongside.

- [ ] **Step 2: Add `CmdOemGraftAndFlash` handler**

Edit `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`. Add the handler function inside the `#ifdef GBL_EXPERIMENTAL_FASTBOOT_CMDS` block (so it's gated alongside `CmdOemEscape`):

```c
#ifdef GBL_EXPERIMENTAL_FASTBOOT_CMDS

#include <Library/AvbParseLib.h>

/* Helper: locate active-slot partition handle by name root.
   "recovery" → recovery_a or recovery_b based on active slot.
   "dtbo"     → dtbo, dtbo_a, or dtbo_b. */
STATIC EFI_STATUS
LocatePartitionForGraft (
  IN  CONST CHAR8                *NameRoot,
  OUT EFI_BLOCK_IO_PROTOCOL     **BlockIoOut,
  OUT EFI_HANDLE                 *HandleOut,
  OUT CHAR16                     *NameOut,
  IN  UINTN                       NameOutCap
  )
{
  /* Implementation: use existing PartitionLib helpers in QcomModulePkg.
     Pseudocode:
       Slot active = GetCurrentSlotSuffix();
       Try NameRoot + active.Suffix (e.g. "recovery_a").
       Fall back to NameRoot alone (e.g. "dtbo").
       LocateHandle by GPT name.
       OpenProtocol BlockIo on handle.  */
  /* See examples in same file: CmdFlashCmd, OemBootEfi for patterns. */
  EFI_STATUS Status;
  CHAR16 Tmp[MAX_GPT_NAME_SIZE];
  UnicodeSPrintAscii (Tmp, sizeof (Tmp), "%a%s", NameRoot, GetCurrentSlotSuffix ().Suffix);
  Status = PartitionGetInfo (Tmp, BlockIoOut, HandleOut);
  if (EFI_ERROR (Status)) {
    UnicodeSPrintAscii (Tmp, sizeof (Tmp), "%a", NameRoot);
    Status = PartitionGetInfo (Tmp, BlockIoOut, HandleOut);
  }
  if (EFI_ERROR (Status)) return Status;
  StrnCpyS (NameOut, NameOutCap, Tmp, StrLen (Tmp));
  return EFI_SUCCESS;
}

STATIC VOID
CmdOemGraftAndFlash (
  IN CONST CHAR8 *Arg,
  IN VOID        *Data,
  IN UINT32       Size
  )
{
  CHAR8           PartName[16];
  CHAR8           CommitArg[16];
  BOOLEAN         IsCommit;
  EFI_STATUS      Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
  EFI_HANDLE      Handle  = NULL;
  CHAR16          PartLabel[MAX_GPT_NAME_SIZE];
  UINT8          *PartBuf = NULL;
  UINT64          PartSize;
  GBL_AVB_FOOTER  Footer;
  GBL_AVB_VBMETA_HEADER Header;
  UINT8          *Staged = (UINT8 *)mFlashDataBuffer;  /* fastboot stage target buffer */
  UINT64          StagedSize = mFlashNumDataBytes;
  CHAR8           Resp[256];

  /* Parse args: "<partition>" or "<partition> commit" */
  ZeroMem (PartName,  sizeof (PartName));
  ZeroMem (CommitArg, sizeof (CommitArg));
  AsciiSScanf (Arg, "%15s %15s", PartName, CommitArg);
  IsCommit = (AsciiStrCmp (CommitArg, "commit") == 0);

  if (PartName[0] == 0) {
    FastbootFail ("missing partition name (recovery|dtbo)");
    return;
  }

  /* 1. Locate target partition. */
  Status = LocatePartitionForGraft (PartName, &BlockIo, &Handle,
                                     PartLabel, MAX_GPT_NAME_SIZE);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Resp, sizeof (Resp), "partition %a not found (%r)", PartName, Status);
    FastbootFail (Resp); return;
  }

  /* 2. Read partition contents. */
  PartSize = MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize);
  PartBuf = AllocatePool (PartSize);
  if (PartBuf == NULL) { FastbootFail ("alloc"); return; }
  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, PartSize, PartBuf);
  if (EFI_ERROR (Status)) { FreePool (PartBuf); FastbootFail ("read"); return; }

  /* 3. Source validation: parse footer. */
  Status = AvbParse_Footer (PartBuf, PartSize, &Footer);
  if (EFI_ERROR (Status)) {
    FreePool (PartBuf);
    AsciiSPrint (Resp, sizeof (Resp),
                 "AvbFooter parse failed on %s (%r) — partition lacks stock vbmeta",
                 PartLabel, Status);
    FastbootFail (Resp);
    return;
  }

  /* 4. Header validation: parse vbmeta header. */
  Status = AvbParse_VbmetaHeader (PartBuf + Footer.VbmetaOffset, Footer.VbmetaSize, &Header);
  if (EFI_ERROR (Status)) {
    FreePool (PartBuf);
    AsciiSPrint (Resp, sizeof (Resp),
                 "AvbVBMetaImageHeader parse failed on %s (%r)", PartLabel, Status);
    FastbootFail (Resp);
    return;
  }

  /* 5. Staged-image validation: must be at least PartSize bytes. */
  if (StagedSize < PartSize) {
    FreePool (PartBuf);
    AsciiSPrint (Resp, sizeof (Resp),
                 "staged size %ld < partition size %ld",
                 (long)StagedSize, (long)PartSize);
    FastbootFail (Resp);
    return;
  }

  /* 6. Staged-image magic check. */
  if (AsciiStrCmp (PartName, "recovery") == 0) {
    /* Android boot image magic "ANDROID!" at offset 0. */
    if (CompareMem (Staged, "ANDROID!", 8) != 0) {
      FreePool (PartBuf);
      FastbootFail ("staged buffer does not look like an Android boot image (no ANDROID! magic)");
      return;
    }
  } else if (AsciiStrCmp (PartName, "dtbo") == 0) {
    /* fdt magic 0xD00DFEED at offset 0 (or DTBO container header). */
    UINT32 Magic = (UINT32)Staged[0] << 24 | (UINT32)Staged[1] << 16
                 | (UINT32)Staged[2] << 8  | (UINT32)Staged[3];
    UINT32 DtboMagic = 0xD00DFEED; /* fdt */
    UINT32 DtboHdrMagic = 0xD7B7AB1E; /* DTBO header */
    if (Magic != DtboMagic && Magic != DtboHdrMagic) {
      FreePool (PartBuf);
      FastbootFail ("staged buffer does not look like a dtbo (no fdt or DTBO magic)");
      return;
    }
  } else {
    FreePool (PartBuf);
    AsciiSPrint (Resp, sizeof (Resp), "unsupported partition '%a' (recovery|dtbo only)", PartName);
    FastbootFail (Resp);
    return;
  }

  /* 7. Print diagnostic INFO lines. */
  AsciiSPrint (Resp, sizeof (Resp),
    "donor footer: vbmeta_offset=%lu size=%lu orig_image_size=%lu",
    (unsigned long)Footer.VbmetaOffset, (unsigned long)Footer.VbmetaSize,
    (unsigned long)Footer.OriginalImageSize);
  FastbootInfo (Resp);

  AsciiSPrint (Resp, sizeof (Resp),
    "donor header: avb=%u.%u algo=%u auth_size=%lu aux_size=%lu",
    Header.AvbMajorVersion, Header.AvbMinorVersion, Header.AlgorithmType,
    (unsigned long)Header.AuthenticationDataBlockSize,
    (unsigned long)Header.AuxiliaryDataBlockSize);
  FastbootInfo (Resp);

  AsciiSPrint (Resp, sizeof (Resp),
    "target partition %s: %lu bytes; staged: %lu bytes",
    PartLabel, (unsigned long)PartSize, (unsigned long)StagedSize);
  FastbootInfo (Resp);

  if (!IsCommit) {
    FreePool (PartBuf);
    FastbootInfo ("DRY RUN — re-issue with 'commit' to write");
    FastbootOkay ("");
    return;
  }

  /* 8. Atomic write: graft donor's tail bytes onto staged buffer, write to partition. */
  CopyMem (Staged + Footer.VbmetaOffset,
           PartBuf + Footer.VbmetaOffset,
           PartSize - Footer.VbmetaOffset);

  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0,
                                  PartSize, Staged);
  FreePool (PartBuf);

  if (EFI_ERROR (Status)) {
    AsciiSPrint (Resp, sizeof (Resp), "WriteBlocks failed (%r)", Status);
    FastbootFail (Resp);
    return;
  }

  AsciiSPrint (Resp, sizeof (Resp),
    "grafted %s: replaced last %lu bytes (vbmeta+footer) with donor's region",
    PartLabel, (unsigned long)(PartSize - Footer.VbmetaOffset));
  FastbootOkay (Resp);
}

#endif /* GBL_EXPERIMENTAL_FASTBOOT_CMDS */
```

(The exact `FastbootInfo`/`FastbootFail`/`FastbootOkay` API + `mFlashDataBuffer` symbol names follow the conventions already in `FastbootCmds.c`. If they differ, mirror the patterns from `CmdOemEscape` or `CmdFlashCmd`. The handler above is the design intent — adapt to the file's existing helpers.)

- [ ] **Step 3: Register the command**

Find the `cmd_list[]` definition (where `CmdOemEscape` is registered). Add (inside the existing `#ifdef GBL_EXPERIMENTAL_FASTBOOT_CMDS` block):

```c
  { "oem graft-and-flash", CmdOemGraftAndFlash },
```

- [ ] **Step 4: Build and verify it compiles**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -10
```

Expected: clean build. The new function uses `AvbParse_*` from AvbParseLib (already linked into our payload via DSC LibraryClasses).

If link errors surface re: AvbParseLib, check that `GblChainloadPkg.dsc`'s `[LibraryClasses]` includes the AvbParseLib mapping (added in Task 1 Step 9), and that the FastbootLib.inf in edk2 lists `AvbParseLib` in its `[LibraryClasses]`. If not, add to the FastbootLib.inf:

```ini
[LibraryClasses]
  AvbParseLib
```

- [ ] **Step 5: Commit (in edk2 submodule)**

```bash
cd /home/vivy/gbl-chainload/edk2 \
  && git add QcomModulePkg/Library/FastbootLib/FastbootCmds.c \
            QcomModulePkg/Library/FastbootLib/FastbootCmds.inf \
  && git commit -m "FastbootCmds: add 'oem graft-and-flash <partition> [commit]' (Track 2)" \
  && git push origin main
```

- [ ] **Step 6: Bump submodule pointer in gbl-chainload**

```bash
cd /home/vivy/gbl-chainload \
  && git add edk2 \
  && git commit -m "edk2: bump submodule (oem graft-and-flash command)"
```

(Don't push yet; controller batches.)

---

## Task 6: `fastboot getvar vbmeta:*` handlers — Plan 3d (edk2 fork)

**Files:**
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` (or wherever getvar handlers live; possibly `FastbootCommands.c` or a separate file)

- [ ] **Step 1: Locate the getvar dispatcher**

```bash
cd /home/vivy/gbl-chainload/edk2 \
  && grep -nE 'getvar|HandleGetVarCmd|VarHandlers' \
  QcomModulePkg/Library/FastbootLib/*.c | head -20
```

The QcomModulePkg fastboot library typically has a `getvar` dispatcher that matches variable names against handlers. Add new handler entries.

- [ ] **Step 2: Implement vbmeta:* getvar handlers**

Add a helper file or extend `FastbootCmds.c`:

```c
#include <Library/AvbParseLib.h>

/* Per-partition descriptor lookup.  Returns descriptor type for a partition
   name from the active-slot vbmeta. */
typedef enum {
  PartDescNone = 0,
  PartDescHash,
  PartDescChain,
  PartDescHashtree,
} PART_DESC_TYPE;

STATIC EFI_STATUS
GetVbmetaForActiveSlot (
  OUT UINT8  **VbmetaBufOut,
  OUT UINT64  *VbmetaSizeOut
  )
{
  /* Use the same partition lookup as graft-and-flash, but for "vbmeta". */
  EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
  EFI_HANDLE Handle;
  CHAR16 PartLabel[MAX_GPT_NAME_SIZE];
  EFI_STATUS Status;
  UINT64 Size;
  UINT8 *Buf;

  Status = LocatePartitionForGraft ("vbmeta", &BlockIo, &Handle, PartLabel, MAX_GPT_NAME_SIZE);
  if (EFI_ERROR (Status)) return Status;
  Size = MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize);
  Buf = AllocatePool (Size);
  if (Buf == NULL) return EFI_OUT_OF_RESOURCES;
  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, Size, Buf);
  if (EFI_ERROR (Status)) { FreePool (Buf); return Status; }
  *VbmetaBufOut = Buf;
  *VbmetaSizeOut = Size;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
LookupDescriptorForPartition (
  IN  CONST UINT8 *VbmetaBuf,
  IN  UINT64       VbmetaSize,
  IN  CONST CHAR8 *PartName,
  OUT PART_DESC_TYPE *TypeOut,
  OUT CONST UINT8 **DigestOut,    /* hash descriptor: digest bytes; chain: pubkey bytes */
  OUT UINT32      *DigestLenOut
  )
{
  GBL_AVB_VBMETA_HEADER Header;
  EFI_STATUS Status;
  CONST UINT8 *Aux;
  UINT64 AuxLen, Cursor = 0;
  GBL_AVB_DESCRIPTOR_TAG Tag;
  CONST UINT8 *Desc;
  UINT64 DescLen;

  *TypeOut = PartDescNone;
  *DigestOut = NULL;
  *DigestLenOut = 0;

  /* The main vbmeta partition has no AvbFooter; the vbmeta header IS at
     offset 0 of the partition. */
  Status = AvbParse_VbmetaHeader (VbmetaBuf, VbmetaSize, &Header);
  if (EFI_ERROR (Status)) return Status;

  Aux = VbmetaBuf + GBL_AVB_VBMETA_HEADER_SIZE + Header.AuthenticationDataBlockSize;
  AuxLen = Header.AuxiliaryDataBlockSize;

  while (TRUE) {
    Status = AvbParse_NextDescriptor (Aux, AuxLen, &Cursor, &Tag, &Desc, &DescLen);
    if (Status == EFI_END_OF_MEDIA) break;
    if (EFI_ERROR (Status)) return Status;

    CONST UINT8 *Name; UINT32 NameLen;
    if (Tag == GblAvbDescHashTag) {
      CONST UINT8 *Digest; UINT32 DigestLen;
      Status = AvbParse_HashDescriptor (Desc, DescLen, &Name, &NameLen, &Digest, &DigestLen);
      if (EFI_ERROR (Status)) continue;
      if (NameLen == AsciiStrLen (PartName) && CompareMem (Name, PartName, NameLen) == 0) {
        *TypeOut = PartDescHash;
        *DigestOut = Digest;
        *DigestLenOut = DigestLen;
        return EFI_SUCCESS;
      }
    } else if (Tag == GblAvbDescChainPartitionTag) {
      CONST UINT8 *Pk; UINT32 PkLen;
      Status = AvbParse_ChainPartitionDescriptor (Desc, DescLen, &Name, &NameLen, &Pk, &PkLen);
      if (EFI_ERROR (Status)) continue;
      if (NameLen == AsciiStrLen (PartName) && CompareMem (Name, PartName, NameLen) == 0) {
        *TypeOut = PartDescChain;
        *DigestOut = Pk;
        *DigestLenOut = PkLen;
        return EFI_SUCCESS;
      }
    }
  }

  return EFI_NOT_FOUND;
}

STATIC VOID
HexDump (CONST UINT8 *Bytes, UINT32 Len, CHAR8 *Out, UINTN OutCap) {
  UINTN i; UINTN p = 0;
  for (i = 0; i < Len && p + 2 < OutCap; ++i) {
    AsciiSPrint (Out + p, OutCap - p, "%02x", Bytes[i]);
    p += 2;
  }
  Out[p] = 0;
}

/* Handler signature follows the QcomModulePkg getvar pattern.  Adapt to
   actual conventions in the file. */
STATIC VOID
GetVarVbmetaDigest (
  OUT CHAR8 *Out,
  IN  UINTN  OutCap
  )
{
  UINT8 *Buf = NULL;
  UINT64 Size;
  EFI_STATUS Status = GetVbmetaForActiveSlot (&Buf, &Size);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Out, OutCap, "n/a");
    return;
  }
  /* SHA256 the entire vbmeta partition content. */
  UINT8 Hash[32];
  /* Use Sha256HashAll from BaseCryptLib if available; otherwise inline a small SHA256.
     For now, expect BaseCryptLib in our edk2 fork. */
  Sha256HashAll (Buf, Size, Hash);
  HexDump (Hash, 32, Out, OutCap);
  FreePool (Buf);
}

STATIC VOID
GetVarVbmetaPartStatus (
  IN  CONST CHAR8 *PartName,
  OUT CHAR8       *Out,
  IN  UINTN        OutCap
  )
{
  UINT8 *VbmBuf = NULL;
  UINT64 VbmSize;
  EFI_STATUS Status = GetVbmetaForActiveSlot (&VbmBuf, &VbmSize);
  if (EFI_ERROR (Status)) { AsciiSPrint (Out, OutCap, "n/a"); return; }

  PART_DESC_TYPE Type;
  CONST UINT8 *Stored; UINT32 StoredLen;
  Status = LookupDescriptorForPartition (VbmBuf, VbmSize, PartName, &Type, &Stored, &StoredLen);
  if (Status == EFI_NOT_FOUND) { AsciiSPrint (Out, OutCap, "n/a"); FreePool (VbmBuf); return; }
  if (EFI_ERROR (Status))      { AsciiSPrint (Out, OutCap, "error"); FreePool (VbmBuf); return; }

  if (Type == PartDescChain) {
    /* Read the chained partition's footer; check that AvbFooter parses cleanly. */
    EFI_BLOCK_IO_PROTOCOL *Bio; EFI_HANDLE H; CHAR16 PartLabel[MAX_GPT_NAME_SIZE];
    Status = LocatePartitionForGraft (PartName, &Bio, &H, PartLabel, MAX_GPT_NAME_SIZE);
    if (EFI_ERROR (Status)) { AsciiSPrint (Out, OutCap, "n/a"); FreePool (VbmBuf); return; }
    UINT64 PSize = MultU64x32 (Bio->Media->LastBlock + 1, Bio->Media->BlockSize);
    UINT8 *PBuf = AllocatePool (PSize);
    if (PBuf == NULL)        { AsciiSPrint (Out, OutCap, "error"); FreePool (VbmBuf); return; }
    Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, PSize, PBuf);
    if (EFI_ERROR (Status))  { FreePool (PBuf); FreePool (VbmBuf); AsciiSPrint (Out, OutCap, "error"); return; }
    GBL_AVB_FOOTER F;
    Status = AvbParse_Footer (PBuf, PSize, &F);
    if (Status == EFI_NOT_FOUND) AsciiSPrint (Out, OutCap, "unsigned");
    else if (EFI_ERROR (Status)) AsciiSPrint (Out, OutCap, "error");
    else                         AsciiSPrint (Out, OutCap, "ok");
    FreePool (PBuf);
  } else if (Type == PartDescHash) {
    /* Hash descriptor: compute SHA256 over partition content; compare to stored. */
    EFI_BLOCK_IO_PROTOCOL *Bio; EFI_HANDLE H; CHAR16 PartLabel[MAX_GPT_NAME_SIZE];
    Status = LocatePartitionForGraft (PartName, &Bio, &H, PartLabel, MAX_GPT_NAME_SIZE);
    if (EFI_ERROR (Status)) { AsciiSPrint (Out, OutCap, "n/a"); FreePool (VbmBuf); return; }
    UINT64 PSize = MultU64x32 (Bio->Media->LastBlock + 1, Bio->Media->BlockSize);
    UINT8 *PBuf = AllocatePool (PSize);
    if (PBuf == NULL)        { AsciiSPrint (Out, OutCap, "error"); FreePool (VbmBuf); return; }
    Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, PSize, PBuf);
    if (EFI_ERROR (Status))  { FreePool (PBuf); FreePool (VbmBuf); AsciiSPrint (Out, OutCap, "error"); return; }
    UINT8 Computed[32];
    Sha256HashAll (PBuf, PSize, Computed);
    if (StoredLen == 32 && CompareMem (Computed, Stored, 32) == 0) {
      AsciiSPrint (Out, OutCap, "ok");
    } else {
      AsciiSPrint (Out, OutCap, "mismatch");
    }
    FreePool (PBuf);
  } else {
    AsciiSPrint (Out, OutCap, "n/a");
  }
  FreePool (VbmBuf);
}
```

- [ ] **Step 3: Wire into the getvar dispatcher**

In FastbootCmds.c's getvar dispatcher, add (or in a `[LookupTable]` of var names → handlers):

```c
/* Pseudocode — adapt to actual dispatcher pattern: */
if (AsciiStrCmp (VarName, "vbmeta:digest") == 0) {
  GetVarVbmetaDigest (Resp, RespSize); return;
}
if (AsciiStrCmp (VarName, "vbmeta:slot") == 0) {
  AsciiStrCpyS (Resp, RespSize, GetCurrentSlotSuffix ().Suffix + 1);  /* "_a" → "a" */
  return;
}
{
  CHAR8 PartName[16];
  if (AsciiSScanf (VarName, "vbmeta:%15[^:]:status", PartName) == 1) {
    GetVarVbmetaPartStatus (PartName, Resp, RespSize); return;
  }
  /* Add similar parsing for :descriptor-type, :expected, :computed (use the same
     LookupDescriptorForPartition path; format Stored or Computed bytes as hex). */
}
```

For brevity, the descriptor-type / expected / computed handlers follow the same shape as `:status` — they call `LookupDescriptorForPartition` and format the result differently. Implementer fills in following the same patterns.

- [ ] **Step 4: Build + verify it compiles**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -10
```

Expected: clean build. If `Sha256HashAll` is missing, add `BaseCryptLib` to FastbootLib's `.inf` `[LibraryClasses]`.

- [ ] **Step 5: Commit (edk2 + bump)**

```bash
cd /home/vivy/gbl-chainload/edk2 \
  && git add QcomModulePkg/Library/FastbootLib/FastbootCmds.c \
            QcomModulePkg/Library/FastbootLib/FastbootCmds.inf \
  && git commit -m "FastbootCmds: getvar vbmeta:* handlers (digest, slot, per-partition status)" \
  && git push origin main
cd /home/vivy/gbl-chainload \
  && git add edk2 \
  && git commit -m "edk2: bump submodule (getvar vbmeta:* handlers)"
```

---

## Task 7: `fastboot oem vbmeta-status` — Plan 3d multi-line report (edk2 fork)

**Files:**
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`

- [ ] **Step 1: Add `CmdOemVbmetaStatus` handler**

```c
STATIC VOID
CmdOemVbmetaStatus (
  IN CONST CHAR8 *Arg,
  IN VOID        *Data,
  IN UINT32       Size
  )
{
  /* Per-OEM partition list — partitions we expect to find descriptors for. */
  STATIC CONST CHAR8 *KnownParts[] = {
    "boot", "init_boot", "vendor_boot", "recovery", "dtbo",
    "vbmeta_system", "vbmeta_vendor",
    NULL,
  };

  CHAR8 Line[256];

  FastbootInfo ("partition       descriptor    expected                                                          computed                                                          status");

  UINT8 *VbmBuf = NULL;
  UINT64 VbmSize;
  EFI_STATUS Status = GetVbmetaForActiveSlot (&VbmBuf, &VbmSize);
  if (EFI_ERROR (Status)) {
    FastbootFail ("failed to read main vbmeta");
    return;
  }

  for (UINTN i = 0; KnownParts[i] != NULL; ++i) {
    PART_DESC_TYPE Type;
    CONST UINT8 *Stored;
    UINT32 StoredLen;
    Status = LookupDescriptorForPartition (VbmBuf, VbmSize, KnownParts[i], &Type, &Stored, &StoredLen);
    if (Status == EFI_NOT_FOUND || Type == PartDescNone) continue;

    CHAR8 ExpectedHex[129] = {0};
    CHAR8 ComputedHex[129] = {0};
    CHAR8 StatusStr[16] = "n/a";

    if (Type == PartDescHash) {
      HexDump (Stored, StoredLen, ExpectedHex, sizeof (ExpectedHex));
      /* Compute live hash. */
      EFI_BLOCK_IO_PROTOCOL *Bio; EFI_HANDLE H; CHAR16 PartLabel[MAX_GPT_NAME_SIZE];
      if (LocatePartitionForGraft (KnownParts[i], &Bio, &H, PartLabel, MAX_GPT_NAME_SIZE) == EFI_SUCCESS) {
        UINT64 PSize = MultU64x32 (Bio->Media->LastBlock + 1, Bio->Media->BlockSize);
        UINT8 *PBuf = AllocatePool (PSize);
        if (PBuf != NULL && Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, PSize, PBuf) == EFI_SUCCESS) {
          UINT8 Computed[32];
          Sha256HashAll (PBuf, PSize, Computed);
          HexDump (Computed, 32, ComputedHex, sizeof (ComputedHex));
          if (CompareMem (Computed, Stored, StoredLen) == 0) AsciiStrCpyS (StatusStr, 16, "ok");
          else                                                AsciiStrCpyS (StatusStr, 16, "mismatch");
        }
        if (PBuf != NULL) FreePool (PBuf);
      }
      AsciiSPrint (Line, sizeof (Line), "%-16a %-13a %-65a %-65a %a",
                   KnownParts[i], "hash", ExpectedHex, ComputedHex, StatusStr);
    } else if (Type == PartDescChain) {
      HexDump (Stored, StoredLen, ExpectedHex, sizeof (ExpectedHex));
      /* Status: chain partition's footer parses → ok; else unsigned. */
      EFI_BLOCK_IO_PROTOCOL *Bio; EFI_HANDLE H; CHAR16 PartLabel[MAX_GPT_NAME_SIZE];
      if (LocatePartitionForGraft (KnownParts[i], &Bio, &H, PartLabel, MAX_GPT_NAME_SIZE) == EFI_SUCCESS) {
        UINT64 PSize = MultU64x32 (Bio->Media->LastBlock + 1, Bio->Media->BlockSize);
        UINT8 *PBuf = AllocatePool (PSize);
        if (PBuf != NULL && Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, PSize, PBuf) == EFI_SUCCESS) {
          GBL_AVB_FOOTER F;
          if (AvbParse_Footer (PBuf, PSize, &F) == EFI_SUCCESS) AsciiStrCpyS (StatusStr, 16, "ok");
          else                                                    AsciiStrCpyS (StatusStr, 16, "unsigned");
        }
        if (PBuf != NULL) FreePool (PBuf);
      }
      AsciiSPrint (Line, sizeof (Line), "%-16a %-13a %-65a %-65a %a",
                   KnownParts[i], "chain", ExpectedHex, "(chain)", StatusStr);
    } else {
      AsciiSPrint (Line, sizeof (Line), "%-16a %-13a %-65a %-65a %a",
                   KnownParts[i], "unknown", "n/a", "n/a", "n/a");
    }
    FastbootInfo (Line);
  }

  /* Summary lines. */
  UINT8 VbmDigest[32];
  Sha256HashAll (VbmBuf, VbmSize, VbmDigest);
  CHAR8 Hex[129];
  HexDump (VbmDigest, 32, Hex, sizeof (Hex));
  AsciiSPrint (Line, sizeof (Line), "vbmeta digest    sha256        %a", Hex);
  FastbootInfo (Line);

  /* vbmeta flags (parsed from header at offset of VbmBuf). */
  GBL_AVB_VBMETA_HEADER VbmHdr;
  if (AvbParse_VbmetaHeader (VbmBuf, VbmSize, &VbmHdr) == EFI_SUCCESS) {
    AsciiSPrint (Line, sizeof (Line), "vbmeta flags     raw           0x%08x", VbmHdr.Flags);
    FastbootInfo (Line);
  }

  FreePool (VbmBuf);
  FastbootOkay ("");
}
```

- [ ] **Step 2: Register the command**

In the `cmd_list[]` (alongside the other `oem` commands):

```c
  { "oem vbmeta-status", CmdOemVbmetaStatus },
```

- [ ] **Step 3: Build + verify**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 4: Commit + bump**

```bash
cd /home/vivy/gbl-chainload/edk2 \
  && git add QcomModulePkg/Library/FastbootLib/FastbootCmds.c \
  && git commit -m "FastbootCmds: oem vbmeta-status multi-line report" \
  && git push origin main
cd /home/vivy/gbl-chainload \
  && git add edk2 \
  && git commit -m "edk2: bump submodule (oem vbmeta-status)"
```

---

## Task 8: Track 3 flashable ZIP template + static binary

**Files:**
- Create: `tools/avb-graft-recovery/graft-vbmeta.c`
- Create: `tools/avb-graft-recovery/Makefile`
- Create: `zip/avb-graft-installer/META-INF/com/google/android/update-binary`
- Create: `zip/avb-graft-installer/META-INF/com/google/android/updater-script`
- Create: `zip/avb-graft-installer/README.md`

- [ ] **Step 1: Author the small C graft binary**

Create `tools/avb-graft-recovery/graft-vbmeta.c` (port of the Python script's logic to plain C, statically-linkable):

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define AVB_FOOTER_MAGIC "AVBf"
#define AVB_FOOTER_SIZE  64
#define AVB_VBMETA_MAGIC "AVB0"

static uint64_t read_u64_be (const uint8_t *p) {
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32)
       | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8)  |  (uint64_t)p[7];
}

int main (int argc, char **argv) {
  if (argc != 4) {
    fprintf (stderr, "Usage: %s <stock.img> <custom.img> <out.img>\n", argv[0]);
    return 2;
  }
  /* Load both images. */
  FILE *fs = fopen (argv[1], "rb");
  FILE *fc = fopen (argv[2], "rb");
  if (!fs || !fc) { perror ("open"); return 1; }
  fseek (fs, 0, SEEK_END); long ssz = ftell (fs); fseek (fs, 0, SEEK_SET);
  fseek (fc, 0, SEEK_END); long csz = ftell (fc); fseek (fc, 0, SEEK_SET);
  if (ssz <= AVB_FOOTER_SIZE) { fprintf (stderr, "stock too small\n"); return 1; }

  uint8_t *stock = malloc (ssz);
  uint8_t *custom = malloc (ssz);  /* allocate stock-sized; pad custom with zeros if smaller */
  if (!stock || !custom) { fprintf (stderr, "OOM\n"); return 1; }
  memset (custom, 0, ssz);
  fread (stock, 1, ssz, fs);
  fread (custom, 1, csz < ssz ? csz : ssz, fc);
  fclose (fs); fclose (fc);

  /* Parse footer. */
  uint8_t *footer = stock + ssz - AVB_FOOTER_SIZE;
  if (memcmp (footer, AVB_FOOTER_MAGIC, 4) != 0) {
    fprintf (stderr, "AvbFooter magic missing in stock\n"); return 1;
  }
  uint64_t orig = read_u64_be (footer + 12);
  uint64_t vbm_off = read_u64_be (footer + 20);
  uint64_t vbm_sz  = read_u64_be (footer + 28);
  fprintf (stderr, "donor footer: orig=%llu vbm_off=%llu vbm_sz=%llu\n",
           (unsigned long long)orig, (unsigned long long)vbm_off, (unsigned long long)vbm_sz);

  if (vbm_off + vbm_sz > (uint64_t)ssz) { fprintf (stderr, "footer inconsistent\n"); return 1; }

  /* Splice: replace custom[vbm_off..ssz] with stock[vbm_off..ssz]. */
  memcpy (custom + vbm_off, stock + vbm_off, ssz - vbm_off);

  /* Write output. */
  FILE *fo = fopen (argv[3], "wb");
  if (!fo) { perror ("open out"); return 1; }
  fwrite (custom, 1, ssz, fo);
  fclose (fo);
  fprintf (stderr, "wrote %ld bytes to %s\n", ssz, argv[3]);
  free (stock); free (custom);
  return 0;
}
```

- [ ] **Step 2: Cross-compile Makefile**

Create `tools/avb-graft-recovery/Makefile`:

```makefile
# Cross-compile a small static aarch64 binary suitable for Android recovery's
# busybox env.  Uses aarch64-linux-gnu-gcc with static-glibc.
CC      ?= aarch64-linux-gnu-gcc
CFLAGS  ?= -O2 -static -Wall -Wextra -std=c11

graft-vbmeta: graft-vbmeta.c
	$(CC) $(CFLAGS) $^ -o $@
	aarch64-linux-gnu-strip $@ 2>/dev/null || true

clean:
	rm -f graft-vbmeta
```

Hard prerequisite: `aarch64-linux-gnu-gcc` (and `aarch64-linux-gnu-strip`) installed locally. On Debian/Ubuntu: `apt install gcc-aarch64-linux-gnu`. The README documents this prerequisite — implementer of this task adds the line.

- [ ] **Step 3: Build the binary**

```bash
cd /home/vivy/gbl-chainload/tools/avb-graft-recovery && make
file graft-vbmeta
```

Expected: `ELF 64-bit LSB executable, ARM aarch64, statically linked`. Size ~150K post-strip.

If `aarch64-linux-gnu-gcc` is not installed, this task is BLOCKED — install the prerequisite (`apt install gcc-aarch64-linux-gnu` on Debian/Ubuntu) before retrying. Do not invent alternative toolchains here.

- [ ] **Step 4: Author `update-binary`**

Create `zip/avb-graft-installer/META-INF/com/google/android/update-binary`:

```bash
#!/sbin/sh
# AVB graft installer — flashable ZIP for custom recovery.
# Reads on-disk active-slot stock recovery, grafts user's custom image,
# writes back to the same partition.

OUTFD="$2"
ZIP="$3"
BB=/sbin/busybox
[ -x "$BB" ] || BB=busybox

ui_print () {
  echo -e "ui_print $1\nui_print" >&"$OUTFD"
}

abort () {
  ui_print "ERROR: $1"
  exit 1
}

ui_print "AVB graft installer"
ui_print "==================="

WORK=/tmp/avb-graft
$BB rm -rf "$WORK"
$BB mkdir -p "$WORK"

# Unpack ZIP contents.
$BB unzip -o "$ZIP" -d "$WORK" >/dev/null
chmod +x "$WORK/tools/graft-vbmeta"

# Detect active slot.
SLOT=""
if [ -e /proc/cmdline ]; then
  SLOT=$($BB grep -o 'androidboot.slot_suffix=_[ab]' /proc/cmdline | $BB cut -d= -f2)
fi
[ -z "$SLOT" ] && SLOT="_a"
ui_print "active slot: $SLOT"

# Per-partition graft.
for PART in recovery dtbo; do
  CUSTOM="$WORK/${PART}_custom.img"
  if [ ! -f "$CUSTOM" ]; then
    ui_print "  skip $PART (no custom image in ZIP)"
    continue
  fi
  STOCK_DEV=""
  if [ -e "/dev/block/by-name/${PART}${SLOT}" ]; then
    STOCK_DEV="/dev/block/by-name/${PART}${SLOT}"
  elif [ -e "/dev/block/by-name/${PART}" ]; then
    STOCK_DEV="/dev/block/by-name/${PART}"
  else
    abort "no block device for $PART"
  fi

  ui_print "  $PART: stock=$STOCK_DEV custom=$CUSTOM"
  $BB dd if="$STOCK_DEV" of="$WORK/stock_${PART}.img" 2>/dev/null \
    || abort "dd stock $PART failed"

  "$WORK/tools/graft-vbmeta" "$WORK/stock_${PART}.img" "$CUSTOM" "$WORK/grafted_${PART}.img" \
    || abort "graft $PART failed"

  $BB dd if="$WORK/grafted_${PART}.img" of="$STOCK_DEV" 2>/dev/null \
    || abort "dd grafted $PART → block failed"
  ui_print "  $PART: grafted + flashed OK"
done

$BB rm -rf "$WORK"
ui_print "Done."
exit 0
```

`chmod +x zip/avb-graft-installer/META-INF/com/google/android/update-binary`.

Create `zip/avb-graft-installer/META-INF/com/google/android/updater-script` (just a marker file expected by the installer):

```
ui_print("avb-graft-installer");
```

- [ ] **Step 5: README for ZIP assembly**

Create `zip/avb-graft-installer/README.md`:

```markdown
# AVB graft installer (Track 3 flashable ZIP)

A flashable ZIP that grafts stock embedded vbmeta from your device's on-disk
recovery/dtbo partition into your custom recovery/dtbo image, then flashes
the grafted result back. Run from inside an existing custom recovery
(TWRP/OFOX/etc.).

## Assembly

This directory is a *template*. Add your custom images:

    cp /path/to/twrp.img zip/avb-graft-installer/recovery_custom.img
    cp /path/to/dtbo.img zip/avb-graft-installer/dtbo_custom.img   # optional

Then build the static graft binary and bundle:

    make -C tools/avb-graft-recovery
    cp tools/avb-graft-recovery/graft-vbmeta zip/avb-graft-installer/tools/

    cd zip/avb-graft-installer
    zip -r ../../dist/avb-graft-installer.zip .

## Flash

In your custom recovery, install `avb-graft-installer.zip`. Reboot to system.

## OTA recovery

After each OTA, your custom recovery + grafted state are overwritten by stock.
Reboot to a state that lets you flash again (e.g. TWRP via fastboot stage), then
re-run this installer with the same ZIP. The script reads the new on-disk stock
as the donor each time.
```

- [ ] **Step 6: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add tools/avb-graft-recovery zip/avb-graft-installer \
  && git commit -m "Track 3: flashable ZIP template + aarch64 static graft binary"
```

---

## Task 9: Final validation + push + tag

**Files:** none (verification + tag).

- [ ] **Step 1: Run full runall**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -15
```

Expected: ALL TESTS PASS. Includes:
- 11 AvbParseLib tests (3 footer + 3 header + 3 descriptor iter + 2 hash/chain)
- existing scan/encode/engine tests
- 052_graft_vbmeta_roundtrip
- 010_build_smoke (mode-0 + mode-1 builds)
- 045/046/047 lints

- [ ] **Step 2: Build all default artifacts**

```bash
cd /home/vivy/gbl-chainload \
  && ./scripts/build.sh --mode 0 \
  && ./scripts/build.sh --mode 1 \
  && ./scripts/build.sh --mode 1 --auto --debug --verbose
ls -lh dist/
```

- [ ] **Step 3: Push (gbl-chainload main)**

```bash
cd /home/vivy/gbl-chainload && git push origin main
```

- [ ] **Step 4: Tag the milestone**

```bash
cd /home/vivy/gbl-chainload \
  && git tag -a v2.0.0-plan3-avb-facade-graft -m "Plan 3 complete: AVB façade graft + libavb-status fastboot interface

End-state:
- AvbParseLib: minimal AVB structure parser (footer, header, descriptors).
- scripts/graft-vbmeta.py: host-side Track 1 with roundtrip test.
- fastboot oem graft-and-flash <recovery|dtbo>: on-device Track 2 with dry-run + commit.
- fastboot getvar vbmeta:*: per-partition status interface.
- fastboot oem vbmeta-status: multi-line diagnostic report.
- zip/avb-graft-installer/: Track 3 flashable ZIP template + aarch64 static binary.

Plan 2 deliverables preserved." && git push origin v2.0.0-plan3-avb-facade-graft
```

- [ ] **Step 5: Manual device validation (user-active)**

The user runs the end-to-end test:

```bash
# Pre-condition: device in custom recovery (e.g. TWRP) with mode-1 staging done.
# Stage Track 1 graft:
python3 scripts/graft-vbmeta.py \
  --partition recovery \
  --stock /path/to/infiniti-EU-16.0.5.703-stock-recovery.img \
  --custom /path/to/twrp.img \
  --out /tmp/grafted.img

# Flash grafted recovery (via fastboot or TWRP partition flash):
fastboot flash recovery_a /tmp/grafted.img
fastboot flash recovery_b /tmp/grafted.img

# Reboot to system:
fastboot reboot

# Verify on the booted system:
adb shell getprop ro.boot.verifiedbootstate    # → green
adb shell getprop ro.boot.vbmeta.device_state  # → locked
# Custom recovery is still flashed; verify by rebooting recovery and seeing TWRP.
```

End-state checklist:

- [ ] `dist/mode-1-auto-debug-verbose.efi` builds with all the new edk2 commands integrated.
- [ ] `fastboot oem graft-and-flash recovery` (dry-run) prints donor footer + header info.
- [ ] `fastboot oem graft-and-flash recovery commit` writes successfully.
- [ ] `fastboot getvar vbmeta:recovery:status` returns `ok` post-graft.
- [ ] `fastboot oem vbmeta-status` returns multi-line report.
- [ ] After grafted-recovery flash + reboot to system: device boots Android system; custom recovery still installed (boots when explicitly entering recovery mode).
- [ ] All host tests pass; runall green.

---

## Plan 3 done. Next.

After Plan 3 lands and device-validates the full custom-recovery + system-boot path:

- **Mode-2** (KM-payload spoof for Play Integrity / SafetyNet) — separate plan when interested.
- **ABL caching / build-time embed** — separate plan; addresses OTA-induced ABL changes.
- **Build flag gating + size optimization audit** (`--gc-sections`, dropping unused cherry-picked oem commands) — separate plan post Plan 3.
- **Mechanism documentation** for AOSP-side AVB tolerance behavior — parallel research session, separate doc.
