# AVB Parser Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `GblChainloadPkg/Library/AvbParseLib` the single AVB parser used on-device and across host tools, by retiring the parallel libavb-backed implementation in `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` and migrating `tools/mode2-profile/mode2-profile.c` off its standalone parser.

**Architecture:** AvbParseLib stays a thin manual-BE parser (no libavb dep, no crypto). Its `AvbParse_HashDescriptor` signature is widened additively with three OPTIONAL trailing outs (`SaltOut`, `SaltLenOut`, `ImageSizeOut`) so it can replace the FastbootCmds.c helper that today returns those fields. FastbootLib declares an `AvbParseLib` LibraryClass dependency, drops its 4 static `AvbParse_*` impls (~100 LOC), and existing call sites at `FastbootCmds.c:4862,4877,4883,4899` keep their parameter lists. mode2-profile.c reuses AvbParseLib via the existing `__HOST_BUILD__` shim that vbmeta-graft already uses.

**Tech Stack:** EDK2 / Tianocore (UEFI), C99 host tools, GNU Make. No new dependencies. Test harness: `tests/avb/test_avbparse.c` (existing C unit tests), on-device fastboot menu smoke check.

**Out of scope (per decisions in audit conversation):**
- No new "graft OK?" verdict API — `mVbmetaWarning` + `mVbmetaPartVars[]` stay as the implicit verdict.
- `vbmeta-graft list-hash` / `probe_graft` diagnostic subcommands stay where they are.
- AvbParseLib structural strictness unchanged — A's checks are accepted as-is (the menu becomes slightly more permissive about malformed vbmeta partitions; that's acceptable because ABL crypto-verifies pre-handoff and AOSP init verifies on boot).
- No upstream review for the edk2/ side — commits land directly on the fork's `main` branch (per user policy for the private fork).

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `GblChainloadPkg/Include/Library/AvbParseLib.h` | Modify | Widen `AvbParse_HashDescriptor` declaration with 3 OPTIONAL trailing outs. |
| `GblChainloadPkg/Library/AvbParseLib/AvbParse.c` | Modify | Implement new optional outs (salt pointer, salt_len, image_size). Skip writes when NULL. |
| `tests/avb/test_avbparse.c` | Modify | Update existing 2 callers to pass NULL for new outs; add 1 new test asserting the optional outs return correct values when provided. |
| `tools/vbmeta-graft/vbmeta-graft.c` | Modify | Update `list_cb` (line 185) and `lh_cb` (line 495) to pass NULL,NULL,NULL for new outs. Optionally drop lh_cb's manual salt/image_size decode (offset 16, 132+name_len) in favor of the new outs. |
| `edk2/QcomModulePkg/Library/FastbootLib/FastbootLib.inf` | Modify | Add `AvbParseLib` to `[LibraryClasses]`. (`GblChainloadPkg/GblChainloadPkg.dec` is already in `[Packages]` at line 141.) |
| `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` | Modify | Add `#include <Library/AvbParseLib.h>`. Delete the 4 STATIC forward-decls at lines 133–136 and the 4 STATIC implementations at lines 4630–4731. Verify call sites at lines 4862, 4877, 4883, 4899 still compile against the (now public, identical-signature) AvbParseLib symbols. |
| `tools/mode2-profile/Makefile` | Modify | Add include paths to `AvbParseLib/Internal` and `Include/Library`. Add `-D__HOST_BUILD__`. Add `AvbParse.c` to sources. Update Android, Windows, macOS cross-compile targets identically. |
| `tools/mode2-profile/mode2-profile.c` | Modify | Delete `rbe64()`, all `AVB_*_OFF` macros, the manual header field reads, the manual property-descriptor walk. Replace with `AvbParse_VbmetaHeader` + `AvbParse_NextDescriptor`. TOML output must be byte-identical to the pre-migration baseline. |
| `tools/mode2-profile/tests/regression-fixture.sh` | Create | Capture pre-migration TOML output on a known fixture (`tests/images/vbmeta.img` or `images/vbmeta.img`). Used to gate the migration. |

---

## Task 1: Widen `AvbParse_HashDescriptor` with optional salt and image_size outs

**Goal:** Extend `AvbParse_HashDescriptor` so it can return `salt`, `salt_len`, and `image_size` when the caller passes non-NULL pointers, matching the signature of the static helper in FastbootCmds.c that Task 2 will delete.

**Files:**
- Modify: `GblChainloadPkg/Include/Library/AvbParseLib.h:55`
- Modify: `GblChainloadPkg/Library/AvbParseLib/AvbParse.c:91-115`
- Modify: `tests/avb/test_avbparse.c:268, 331` (NULL the new outs in existing tests)
- Modify: `tests/avb/test_avbparse.c` (add 1 new test exercising the new outs)
- Modify: `tools/vbmeta-graft/vbmeta-graft.c:185, 495` (pass NULL,NULL,NULL)

**Acceptance Criteria:**
- [ ] `AvbParse_HashDescriptor` accepts and writes through three trailing OPTIONAL outs: `OUT CONST UINT8 **SaltOut`, `OUT UINT32 *SaltLenOut`, `OUT UINT64 *ImageSizeOut`.
- [ ] When any of those three are NULL, the function does not crash and the other outputs are unchanged.
- [ ] When all three are non-NULL on a known descriptor, the returned salt pointer points to `Descriptor + 132 + partition_name_len`, the salt_len matches what was at offset 60 (BE u32), and image_size matches what was at offset 16 (BE u64).
- [ ] Existing 2 callers in `vbmeta-graft.c` (lines 185, 495) compile and pass NULL,NULL,NULL.
- [ ] `tests/avb/test_avbparse` binary builds and ALL PASS line is printed.

**Verify:** `cd /home/vivy/gbl-chainload && make -C tests/avb && ./tests/avb/test_avbparse && make -C tools/vbmeta-graft && ./tools/vbmeta-graft/vbmeta-graft --help 2>&1 | head -5` → ends with "ALL PASS" then vbmeta-graft usage line.

**Steps:**

- [ ] **Step 1: Read the existing test harness to find `build_hash_descriptor` and the fixture conventions**

```bash
head -100 /home/vivy/gbl-chainload/tests/avb/test_avbparse.c
```

Confirm `build_hash_descriptor(desc, name, digest, digest_len)` exists and what offsets it uses. Note: it currently produces a descriptor with `name_len`, `salt_len`, `digest_len` at offsets 56/60/64 and `image_size` at offset 16 (the libavb wire format).

- [ ] **Step 2: Write the failing test for the new optional outs**

Append a new test function to `tests/avb/test_avbparse.c` right above `int main (void)`:

```c
static void test_parse_hash_descriptor_with_optional_outs (void) {
  UINT8 desc[256];
  memset (desc, 0, sizeof (desc));
  UINT8 digest[32]; memset (digest, 0xAB, 32);
  /* build_hash_descriptor doesn't set salt_len or image_size; we set them
     explicitly so the test fixture exercises both fields. */
  UINT8 salt_bytes[8] = { 0x53, 0x41, 0x4c, 0x54, 0x21, 0x21, 0x21, 0x21 };
  UINT64 dlen = build_hash_descriptor (desc, "init_boot", digest, 32);
  /* Patch in salt + image_size on top of build_hash_descriptor's output.
     Re-pack the body so name | salt | digest order is preserved.
     name @ 132..132+9, then we want salt at 132+9..132+9+8, then digest at
     132+9+8..132+9+8+32. */
  put_u32_be (desc + 60, 8);                   /* salt_len = 8 */
  put_u64_be (desc + 16, 0xC0FFEE0000000123ULL); /* image_size */
  /* Shift the digest right by salt_len bytes; place salt in the gap. */
  memmove (desc + 132 + 9 + 8, desc + 132 + 9, 32); /* digest moves right */
  memcpy (desc + 132 + 9, salt_bytes, 8);
  /* num_bytes_following grew by salt_len = 8; update it (header u64 BE @ +8). */
  put_u64_be (desc + 8, AvbReadU64Be (desc + 8) + 8);
  dlen += 8;

  CONST UINT8 *name; UINT32 name_len;
  CONST UINT8 *out_digest; UINT32 out_dlen;
  CONST UINT8 *out_salt; UINT32 out_salt_len;
  UINT64 out_image_size;
  EFI_STATUS s = AvbParse_HashDescriptor (desc, dlen,
                                          &name, &name_len,
                                          &out_digest, &out_dlen,
                                          &out_salt, &out_salt_len,
                                          &out_image_size);
  assert (s == EFI_SUCCESS);
  assert (name_len == 9);
  assert (out_dlen == 32);
  assert (out_salt_len == 8);
  assert (memcmp (out_salt, salt_bytes, 8) == 0);
  assert (out_image_size == 0xC0FFEE0000000123ULL);
  assert (memcmp (out_digest, digest, 32) == 0);
  printf ("ok test_parse_hash_descriptor_with_optional_outs\n");

  /* NULL-passthrough: same call with optional outs NULL must succeed and
     leave the required outs correct. */
  CONST UINT8 *name2; UINT32 name_len2;
  CONST UINT8 *digest2; UINT32 dlen2;
  s = AvbParse_HashDescriptor (desc, dlen,
                               &name2, &name_len2, &digest2, &dlen2,
                               NULL, NULL, NULL);
  assert (s == EFI_SUCCESS);
  assert (name_len2 == 9);
  assert (dlen2 == 32);
  assert (memcmp (digest2, digest, 32) == 0);
  printf ("ok test_parse_hash_descriptor_with_optional_outs (NULL passthrough)\n");
}
```

Register it in `main()`:

```c
  test_parse_hash_descriptor_with_optional_outs ();
```

- [ ] **Step 3: Run the test to verify it fails to compile**

Run: `cd /home/vivy/gbl-chainload && make -C tests/avb`

Expected: compile error like `too many arguments to function 'AvbParse_HashDescriptor'`.

- [ ] **Step 4: Widen the header declaration**

Replace `GblChainloadPkg/Include/Library/AvbParseLib.h:55`:

```c
EFI_STATUS EFIAPI AvbParse_HashDescriptor (
  IN CONST UINT8   *Descriptor, IN UINT64 DescriptorLen,
  OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut,
  OUT CONST UINT8 **DigestOut, OUT UINT32 *DigestLenOut,
  OUT CONST UINT8 **SaltOut OPTIONAL, OUT UINT32 *SaltLenOut OPTIONAL,
  OUT UINT64       *ImageSizeOut OPTIONAL);
```

Note: `OPTIONAL` is the EDK2 idiom for "may be NULL". For `__HOST_BUILD__` the macro expands to nothing (defined in `AvbBigEndian.h` shim). If not, add `#define OPTIONAL` to the host shim.

- [ ] **Step 5: Widen the implementation**

Replace `GblChainloadPkg/Library/AvbParseLib/AvbParse.c:91-115`:

```c
EFI_STATUS EFIAPI
AvbParse_HashDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen,
                         OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut,
                         OUT CONST UINT8 **DigestOut, OUT UINT32 *DigestLenOut,
                         OUT CONST UINT8 **SaltOut OPTIONAL,
                         OUT UINT32 *SaltLenOut OPTIONAL,
                         OUT UINT64 *ImageSizeOut OPTIONAL)
{
  UINT32 NameLen, SaltLen, DigestLen;
  UINT64 ImageSize;
  UINT64 BodyStart;
  if (Descriptor == NULL || PartitionNameOut == NULL || PartitionNameLenOut == NULL
      || DigestOut == NULL || DigestLenOut == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (DescriptorLen < 132)                return EFI_INVALID_PARAMETER;
  ImageSize = AvbReadU64Be (Descriptor + 16);
  NameLen   = AvbReadU32Be (Descriptor + 56);
  SaltLen   = AvbReadU32Be (Descriptor + 60);
  DigestLen = AvbReadU32Be (Descriptor + 64);
  BodyStart = 132;
  if ((UINT64)NameLen + (UINT64)SaltLen + (UINT64)DigestLen + BodyStart > DescriptorLen) {
    return EFI_INVALID_PARAMETER;
  }
  *PartitionNameOut    = Descriptor + BodyStart;
  *PartitionNameLenOut = NameLen;
  *DigestOut           = Descriptor + BodyStart + NameLen + SaltLen;
  *DigestLenOut        = DigestLen;
  if (SaltOut != NULL)      *SaltOut      = Descriptor + BodyStart + NameLen;
  if (SaltLenOut != NULL)   *SaltLenOut   = SaltLen;
  if (ImageSizeOut != NULL) *ImageSizeOut = ImageSize;
  return EFI_SUCCESS;
}
```

- [ ] **Step 6: Patch existing callers in vbmeta-graft.c to pass NULL for new outs**

Replace `tools/vbmeta-graft/vbmeta-graft.c:185`:

```c
    AvbParse_HashDescriptor(desc, desc_len, &name, &name_len, &digest, &digest_len,
                            NULL, NULL, NULL);
```

Replace `tools/vbmeta-graft/vbmeta-graft.c:495-497`:

```c
    if (AvbParse_HashDescriptor(desc, desc_len, &name, &name_len,
                                &digest, &digest_len,
                                NULL, NULL, NULL) != EFI_SUCCESS)
      return;
```

- [ ] **Step 7: Patch existing test callers to pass NULL**

Replace `tests/avb/test_avbparse.c:268`:

```c
  EFI_STATUS s = AvbParse_HashDescriptor (desc, dlen, &name, &name_len, &out_digest, &out_dlen,
                                          NULL, NULL, NULL);
```

Replace `tests/avb/test_avbparse.c:331`:

```c
  EFI_STATUS s = AvbParse_HashDescriptor (desc, 100, &name, &name_len, &out_digest, &out_dlen,
                                          NULL, NULL, NULL);
```

- [ ] **Step 8: Run the unit tests to verify everything passes**

Run: `cd /home/vivy/gbl-chainload && make -C tests/avb && ./tests/avb/test_avbparse`

Expected:
```
ok test_footer_parse_ok
ok test_footer_no_magic
... (all existing tests)
ok test_parse_hash_descriptor_with_optional_outs
ok test_parse_hash_descriptor_with_optional_outs (NULL passthrough)
ALL PASS
```

- [ ] **Step 9: Rebuild vbmeta-graft to confirm it still compiles + runs**

Run: `cd /home/vivy/gbl-chainload && make -C tools/vbmeta-graft clean && make -C tools/vbmeta-graft`

Expected: builds without warnings; `./tools/vbmeta-graft/vbmeta-graft` runs and prints usage when given no args (exit 2 is fine).

- [ ] **Step 10: Commit**

```bash
cd /home/vivy/gbl-chainload
git checkout -b avb-parser-consolidation
git add GblChainloadPkg/Include/Library/AvbParseLib.h \
        GblChainloadPkg/Library/AvbParseLib/AvbParse.c \
        tests/avb/test_avbparse.c \
        tools/vbmeta-graft/vbmeta-graft.c
git commit -m "feat(avb): AvbParse_HashDescriptor — optional salt + image_size outs

Adds three OPTIONAL trailing outs (SaltOut, SaltLenOut, ImageSizeOut) so
the library can replace the libavb-backed static helper in FastbootCmds.c
that already returns those fields. Existing callers pass NULL and are
unchanged behaviorally.

Test added: test_parse_hash_descriptor_with_optional_outs (exercises both
the populated path and the NULL-passthrough path)."
```

---

## Task 2: Wire FastbootLib → AvbParseLib (delete parallel parser)

**Goal:** Make `AvbParseLib` the live on-device AVB parser by deleting the 4 file-local `STATIC AvbParse_*` helpers in `FastbootCmds.c` and routing the existing call sites through `AvbParseLib`. Net effect: ~100 LOC removed from the edk2 fork, 4 libavb function calls (`avb_vbmeta_image_verify`, `avb_descriptor_validate_and_byteswap`, `avb_hash_descriptor_validate_and_byteswap`, `avb_chain_partition_descriptor_validate_and_byteswap`) and `avb_vbmeta_image_header_to_host_byte_order` no longer reached from the fastboot vbmeta probe.

**Files:**
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootLib.inf:143-152`
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c:133-136` (delete 4 forward decls)
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c:4630-4750` (delete 4 STATIC impls)
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` near top includes (add AvbParseLib.h)

**Acceptance Criteria:**
- [ ] `edk2/QcomModulePkg/Library/FastbootLib/FastbootLib.inf` `[LibraryClasses]` contains `AvbParseLib` (and the existing `AvbLib` entry stays — libavb is still needed elsewhere in QcomModulePkg).
- [ ] `FastbootCmds.c` contains zero `STATIC EFI_STATUS AvbParse_*` definitions and zero forward-decls.
- [ ] `FastbootCmds.c` has `#include <Library/AvbParseLib.h>` near the top.
- [ ] `grep -n "STATIC EFI_STATUS AvbParse_" edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` returns empty.
- [ ] `grep -n "avb_vbmeta_image_verify\\|avb_descriptor_validate_and_byteswap\\|avb_hash_descriptor_validate_and_byteswap\\|avb_chain_partition_descriptor_validate_and_byteswap" edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` returns empty (those calls were only inside the deleted helpers).
- [ ] EDK2 build completes for both canoe and infiniti targets (the existing build scripts under `scripts/`).
- [ ] Resulting EFI loads on a real device (infiniti) and the fastboot menu still shows the same "AVB WARNING - none" / "AVB WARNING - unsigned:recovery" line as before, with no new error states.

**Verify:** Build + on-device smoke. Build: `cd /home/vivy/gbl-chainload && bash scripts/build.sh` (or whatever the existing build entry point is — see `scripts/` and `docker/`). On-device: stage the new EFI via `fastboot stage dist/<artifact>.efi && fastboot oem boot-efi`, then re-enter fastboot and confirm the AVB WARNING line is unchanged from a known-good baseline.

**Steps:**

- [ ] **Step 1: Read the surrounding context at lines 4630–4750 to confirm exactly what's being deleted**

```bash
awk 'NR>=4620 && NR<=4760' /home/vivy/gbl-chainload/edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c
```

Expected: 4 contiguous `STATIC EFI_STATUS AvbParse_*` function definitions (VbmetaHeader, NextDescriptor, HashDescriptor, ChainPartitionDescriptor). Confirm no other code is interspersed; if there is, the delete-range needs adjustment.

- [ ] **Step 2: Add AvbParseLib to FastbootLib.inf `[LibraryClasses]`**

Edit `edk2/QcomModulePkg/Library/FastbootLib/FastbootLib.inf:143-152` — add `AvbParseLib` after `AvbLib`:

```
[LibraryClasses]
  UefiApplicationEntryPoint
  UefiLib
  PcdLib
  BootLib
  StackCanary
  DebugLib
  UbsanLib
  CompilerIntrinsicsLib
  AvbLib
  AvbParseLib
```

Note: `GblChainloadPkg/GblChainloadPkg.dec` is already in `[Packages]` at line 141, so no Packages change needed.

- [ ] **Step 3: Add the AvbParseLib include to FastbootCmds.c**

Find the existing block of `#include <Library/...>` lines near the top of `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` (around the top 100 lines). Add:

```c
#include <Library/AvbParseLib.h>
```

(Alphabetize within the existing block if the file already alphabetizes; otherwise just append.)

- [ ] **Step 4: Delete the 4 STATIC forward-decls at lines 133–136**

Delete these 4 lines from `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c:133-136`:

```c
STATIC EFI_STATUS AvbParse_VbmetaHeader (IN CONST UINT8 *Vbmeta, IN UINT64 VbmetaSize, OUT GBL_AVB_VBMETA_HEADER *HeaderOut);
STATIC EFI_STATUS AvbParse_NextDescriptor (IN CONST UINT8 *AuxBlock, IN UINT64 AuxSize, IN OUT UINT64 *Cursor, OUT GBL_AVB_DESCRIPTOR_TAG *TagOut, OUT CONST UINT8 **DescriptorOut, OUT UINT64 *DescriptorLenOut);
STATIC EFI_STATUS AvbParse_HashDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen, OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut, OUT CONST UINT8 **DigestOut, OUT UINT32 *DigestLenOut, OUT CONST UINT8 **SaltOut, OUT UINT32 *SaltLenOut, OUT UINT64 *ImageSizeOut);
STATIC EFI_STATUS AvbParse_ChainPartitionDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen, OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut, OUT CONST UINT8 **PublicKeyOut, OUT UINT32 *PublicKeyLenOut);
```

- [ ] **Step 5: Delete the 4 STATIC implementations at lines ~4630–4750**

Delete the four contiguous function definitions:

- `STATIC EFI_STATUS AvbParse_VbmetaHeader (...)`
- `STATIC EFI_STATUS AvbParse_NextDescriptor (...)`
- `STATIC EFI_STATUS AvbParse_HashDescriptor (...)`
- `STATIC EFI_STATUS AvbParse_ChainPartitionDescriptor (...)`

…and any local `#include "Internal/AvbBigEndian.h"` or libavb headers (`avb_vbmeta_image.h`, `avb_descriptor.h`, `avb_hash_descriptor.h`, `avb_chain_partition_descriptor.h`) that were only used by these four helpers.

- [ ] **Step 6: Verify nothing references the removed includes/symbols**

```bash
grep -n "STATIC EFI_STATUS AvbParse_\|avb_vbmeta_image_verify\|avb_descriptor_validate_and_byteswap\|avb_hash_descriptor_validate_and_byteswap\|avb_chain_partition_descriptor_validate_and_byteswap\|avb_vbmeta_image_header_to_host_byte_order" \
     /home/vivy/gbl-chainload/edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c
```

Expected: empty output.

- [ ] **Step 7: Verify the call sites still match AvbParseLib's signatures**

```bash
awk 'NR>=4860 && NR<=4905' /home/vivy/gbl-chainload/edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c
```

Expected: the calls at the four sites (now ~lines 4760-ish after the delete) must match:
- `AvbParse_VbmetaHeader (VbmBuf, VbmSize, &Hdr)` — matches AvbParseLib signature.
- `AvbParse_NextDescriptor (AuxBlock, AuxSize, &Cursor, &Tag, &Desc, &DescLen)` — matches.
- `AvbParse_HashDescriptor (Desc, DescLen, &DName, &DNameLen, DigestOut, DigestLenOut, SaltOut, SaltLenOut, HashImageSizeOut)` — matches widened signature from Task 1.
- `AvbParse_ChainPartitionDescriptor (Desc, DescLen, &DName, &DNameLen, PubKeyOut, PubKeyLenOut)` — matches.

If a call site doesn't compile (signature drift), patch the call site, not the library — AvbParseLib is the authoritative signature post-Task-1.

- [ ] **Step 8: Build the EFI artifact**

Identify the build entry point. Try in this order:

```bash
ls /home/vivy/gbl-chainload/scripts/ /home/vivy/gbl-chainload/docker/
cat /home/vivy/gbl-chainload/scripts/build.sh 2>/dev/null | head -20
```

Run the build. Expected: no compile errors. The `AvbLib` library is still linked (it's used elsewhere in QcomModulePkg's boot path), so the libavb TUs stay in the final binary — only our **calls** to them go away.

- [ ] **Step 9: Commit the edk2 submodule changes**

Per user policy: commit directly to the fork's `main`.

```bash
cd /home/vivy/gbl-chainload/edk2
git checkout main  # confirm we're on main (per user policy)
git add QcomModulePkg/Library/FastbootLib/FastbootLib.inf \
        QcomModulePkg/Library/FastbootLib/FastbootCmds.c
git commit -m "FastbootLib: route vbmeta probe through AvbParseLib

Deletes the 4 file-local STATIC AvbParse_* helpers (~100 LOC) and the
libavb-backed wrappers (avb_vbmeta_image_verify,
avb_descriptor_validate_and_byteswap, *_hash_descriptor_*,
*_chain_partition_descriptor_*, *_vbmeta_image_header_to_host_byte_order)
in favor of GblChainloadPkg's AvbParseLib. Existing call sites in
GblVbmetaLookupDescriptor compile unchanged after AvbParseLib's
AvbParse_HashDescriptor was widened with optional salt/image_size outs.

AvbLib stays linked (boot path elsewhere needs libavb); only our calls
to it from the fastboot vbmeta probe are removed."
```

- [ ] **Step 10: Bump the submodule pointer in the parent repo**

```bash
cd /home/vivy/gbl-chainload
git add edk2
git commit -m "edk2: bump submodule for AvbParseLib wiring

See edk2 commit: FastbootLib: route vbmeta probe through AvbParseLib"
```

- [ ] **Step 11: On-device smoke test**

(User-driven — surface the commands; do not run autonomously.)

Recommend the user run, from a real shell:

```bash
# Build the artifact (find the existing build entry — likely already done in Step 8)
# Stage and one-shot boot it:
fastboot stage dist/<artifact>.efi
fastboot oem boot-efi
# Wait for device to come back up to fastboot
# Visually compare the AVB WARNING line on the menu screen against a known
# baseline screenshot. Expected: identical string.
```

Capture the rendered string (via `fastboot getvar all | grep vbmeta` or the `oem vbmeta-status` command per `edk2` commit `49c5621b18`) and diff against a known-good baseline before declaring success.

---

## Task 3: Migrate `mode2-profile.c` (host) to AvbParseLib

**Goal:** Delete ~80 LOC of duplicated AVB parsing in `tools/mode2-profile/mode2-profile.c` (the `rbe64()` helper, the `AVB_*_OFF` literal offsets, the manual header decode, the manual property-descriptor walk) in favor of `AvbParse_VbmetaHeader` + `AvbParse_NextDescriptor` from AvbParseLib. TOML output must remain byte-identical to the pre-migration baseline.

**Files:**
- Modify: `tools/mode2-profile/Makefile`
- Modify: `tools/mode2-profile/mode2-profile.c`
- Create: `tools/mode2-profile/tests/regression-fixture.sh` (regression gate)

**Acceptance Criteria:**
- [ ] `mode2-profile.c` no longer defines `rbe64`, `AVB_HDR_SIZE`, `AVB_AUTH_SIZE_OFF`, `AVB_AUX_SIZE_OFF`, `AVB_PK_OFF_OFF`, `AVB_PK_SIZE_OFF`, `AVB_DESC_OFF_OFF`, `AVB_DESC_SIZE_OFF`.
- [ ] `mode2-profile.c` reads vbmeta via `AvbParse_VbmetaHeader` and walks descriptors via `AvbParse_NextDescriptor`.
- [ ] The Makefile builds `mode2-profile` for native, Android, Windows, and macOS targets (the existing 4 entry points: `all`, `android`, `windows`, `macos-x64`, `macos-arm64`).
- [ ] Running `./mode2-profile derive <fixture-vbmeta> -o /tmp/new.toml` produces a TOML file byte-identical to the pre-migration baseline (`diff /tmp/baseline.toml /tmp/new.toml` is empty).

**Verify:**
```bash
cd /home/vivy/gbl-chainload/tools/mode2-profile
bash tests/regression-fixture.sh
```

Exits 0 with "PASS: mode2-profile TOML output byte-identical to baseline".

**Steps:**

- [ ] **Step 1: Capture the pre-migration baseline before touching anything**

First identify a usable fixture. Candidates:

```bash
ls /home/vivy/gbl-chainload/tests/images/ /home/vivy/gbl-chainload/images/ 2>/dev/null | grep -i vbmeta
```

If none has a real signed vbmeta with the `com.android.build.boot.os_version` and `com.android.build.boot.security_patch` properties, pull one from the device:

```bash
# (User-driven if needed — surface the command, do not run autonomously.)
fastboot get_staged /tmp/vbmeta.img
# or
adb shell su -c 'dd if=/dev/block/by-name/vbmeta_a of=/sdcard/vbmeta.img bs=4096'
adb pull /sdcard/vbmeta.img /tmp/vbmeta-fixture.img
```

Then capture baseline:

```bash
cd /home/vivy/gbl-chainload/tools/mode2-profile
make clean && make
./mode2-profile derive /tmp/vbmeta-fixture.img -o /tmp/mode2-profile-baseline.toml
cp /tmp/mode2-profile-baseline.toml tests/baseline.toml.golden
cp /tmp/vbmeta-fixture.img tests/vbmeta-fixture.img
```

- [ ] **Step 2: Create the regression-fixture script**

Write `tools/mode2-profile/tests/regression-fixture.sh`:

```bash
#!/usr/bin/env bash
# Regression test: mode2-profile derive output must be byte-identical to
# the captured golden TOML for the vbmeta-fixture.img.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ ! -f tests/baseline.toml.golden ]]; then
  echo "SKIP: no baseline (run capture step first)"; exit 0
fi
if [[ ! -f tests/vbmeta-fixture.img ]]; then
  echo "SKIP: no vbmeta fixture"; exit 0
fi

make >/dev/null
./mode2-profile derive tests/vbmeta-fixture.img -o /tmp/mode2-profile-new.toml >/dev/null

if diff -q tests/baseline.toml.golden /tmp/mode2-profile-new.toml >/dev/null; then
  echo "PASS: mode2-profile TOML output byte-identical to baseline"
else
  echo "FAIL: mode2-profile TOML diverged from baseline"
  diff tests/baseline.toml.golden /tmp/mode2-profile-new.toml || true
  exit 1
fi
```

```bash
chmod +x /home/vivy/gbl-chainload/tools/mode2-profile/tests/regression-fixture.sh
```

- [ ] **Step 3: Update Makefile — add include paths, -D__HOST_BUILD__, AvbParse.c source**

Replace the `CFLAGS`, `SRCS`, `HDRS`, `NDK_CFLAGS`, and `XCFLAGS` lines in `tools/mode2-profile/Makefile`:

```makefile
CC      ?= gcc
PROJ    := $(realpath ../..)
GPL     := $(PROJ)/GblChainloadPkg/Library/GblPayloadLib
AVB     := $(PROJ)/GblChainloadPkg/Library/AvbParseLib
INCS    := -I$(GPL) -I$(AVB)/Internal -I$(PROJ)/GblChainloadPkg/Include/Library \
           -D__HOST_BUILD__
CFLAGS  ?= -Wall -Wextra -Werror -O2 -std=c11 -DGBL_HOST_BUILD=1 $(INCS)
SRCS     = mode2-profile.c vendor/tomlc99/toml.c $(GPL)/Sha256.c $(AVB)/AvbParse.c
HDRS     = vendor/tomlc99/toml.h ../shared/gbl_mode2_profile.h \
           $(GPL)/Internal/Sha256.h \
           $(PROJ)/GblChainloadPkg/Include/Library/AvbParseLib.h \
           $(AVB)/Internal/AvbBigEndian.h
```

Then update the per-target object build to include `avbparse.o`:

```makefile
mode2-profile: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -c -o mode2-profile.o mode2-profile.c
	$(CC) -w -O2 -std=c11 -DGBL_HOST_BUILD=1 -c -o toml.o vendor/tomlc99/toml.c
	$(CC) $(CFLAGS) -c -o sha256.o $(GPL)/Sha256.c
	$(CC) $(CFLAGS) -c -o avbparse.o $(AVB)/AvbParse.c
	$(CC) $(CFLAGS) -o $@ mode2-profile.o toml.o sha256.o avbparse.o
```

Apply identical pattern to Android, Windows, macOS-x64, macOS-arm64 targets (add `avbparse-android.o`, `avbparse-win.o`, etc.). Add `NDK_CFLAGS = -static -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 $(INCS)` and `XCFLAGS = -O2 -Wall -Wextra -Werror -std=c11 -DGBL_HOST_BUILD=1 $(INCS)`. Update `clean:` to remove the new `.o` artifacts.

- [ ] **Step 4: Replace mode2-profile.c manual header decode with `AvbParse_VbmetaHeader`**

Delete `tools/mode2-profile/mode2-profile.c:98-120` (the comment block, `AVB_*_OFF` macros, and `rbe64`). Replace `mode2-profile.c:11` (the include block) with the addition of:

```c
#include "AvbParseLib.h"
```

Then replace the magic check + manual field decode (lines 166–208) with:

```c
    /* --- parse vbmeta header via AvbParseLib --- */
    GBL_AVB_VBMETA_HEADER hdr;
    EFI_STATUS s = AvbParse_VbmetaHeader (img, (UINT64)fsz, &hdr);
    if (s == EFI_NOT_FOUND) {
        fprintf(stderr, "error: %s: not a vbmeta image (bad magic)\n",
                vbmeta_path);
        free(img); return 1;
    }
    if (EFI_ERROR (s)) {
        fprintf(stderr, "error: %s: malformed vbmeta header\n", vbmeta_path);
        free(img); return 1;
    }

    uint64_t auth_size  = hdr.AuthenticationDataBlockSize;
    uint64_t aux_size   = hdr.AuxiliaryDataBlockSize;
    uint64_t pk_off     = hdr.PublicKeyOffset;
    uint64_t pk_size    = hdr.PublicKeySize;
    uint64_t desc_off   = hdr.DescriptorsOffset;
    uint64_t desc_size  = hdr.DescriptorsSize;

    if (pk_size == 0) {
        fprintf(stderr, "error: %s: vbmeta has no public key (unsigned?)\n",
                vbmeta_path);
        free(img); return 1;
    }
    /* AvbParse_VbmetaHeader already enforced header + auth + aux <= file size,
       so aux_off computation below cannot overflow. */
    uint64_t aux_off = 256u + auth_size;
    if (pk_off > aux_size || pk_size > aux_size - pk_off) {
        fprintf(stderr,"error: %s: public key extends past aux block\n",
                vbmeta_path);
        free(img); return 1;
    }
    if (desc_off > aux_size || desc_size > aux_size - desc_off) {
        fprintf(stderr,"error: %s: descriptor region extends past aux block\n",
                vbmeta_path);
        free(img); return 1;
    }
```

Note: keep the `pk_size == 0`, `pk_off > aux_size`, `desc_off > aux_size` checks — `AvbParseLib` validates header-level bounds but doesn't validate offsets *within* the aux block.

- [ ] **Step 5: Replace the manual property descriptor walk with `AvbParse_NextDescriptor`**

Replace `tools/mode2-profile/mode2-profile.c:242-293` (the manual `doff` walk):

```c
    /* --- walk property descriptors via AvbParseLib --- */
    const uint8_t *aux_block = img + aux_off;
    uint64_t os_ver_encoded = 0, spl_encoded = 0;
    char os_ver_str[128] = {0}, spl_str[128] = {0};
    int found_os = 0, found_spl = 0;

    UINT64 cursor = desc_off;
    UINT64 walk_end = desc_off + desc_size;
    while (cursor < walk_end) {
        GBL_AVB_DESCRIPTOR_TAG tag;
        const UINT8 *desc;
        UINT64 desc_len;
        EFI_STATUS ds = AvbParse_NextDescriptor (aux_block, walk_end,
                                                 &cursor, &tag, &desc, &desc_len);
        if (ds == EFI_END_OF_MEDIA) break;
        if (EFI_ERROR (ds)) break; /* malformed — stop walking, do not abort */
        if (tag != GblAvbDescPropertyTag) continue;

        /* Property descriptor body layout (libavb): after the 16-byte header,
           key_size(u64 BE) at +16, val_size(u64 BE) at +24, then key\0val\0. */
        if (desc_len < 32) continue;
        uint64_t klen = AvbReadU64Be (desc + 16);
        uint64_t vlen = AvbReadU64Be (desc + 24);
        /* Same overflow-safe checks as before. */
        if (klen > desc_len - 32 || vlen > desc_len - 32 - klen) continue;
        if (klen + 1 + vlen + 1 > desc_len - 32) continue;

        const char *key = (const char *)(desc + 32);
        const char *val = (const char *)(desc + 32 + klen + 1);

        if (klen == strlen("com.android.build.boot.os_version") &&
            memcmp(key, "com.android.build.boot.os_version", klen) == 0) {
            size_t vsz = vlen < sizeof(os_ver_str)-1 ? (size_t)vlen
                                                       : sizeof(os_ver_str)-1;
            memcpy(os_ver_str, val, vsz);
            os_ver_str[vsz] = '\0';
            found_os = 1;
        }
        if (klen == strlen("com.android.build.boot.security_patch") &&
            memcmp(key, "com.android.build.boot.security_patch", klen) == 0) {
            size_t vsz = vlen < sizeof(spl_str)-1 ? (size_t)vlen
                                                    : sizeof(spl_str)-1;
            memcpy(spl_str, val, vsz);
            spl_str[vsz] = '\0';
            found_spl = 1;
        }
    }
```

Note: `AvbParse_NextDescriptor`'s `AuxSize` argument bounds the walk; passing `walk_end` (absolute, not relative to aux_block) along with `cursor` starting at `desc_off` keeps the existing semantics.

- [ ] **Step 6: Build natively and run the regression**

```bash
cd /home/vivy/gbl-chainload/tools/mode2-profile
make clean && make
bash tests/regression-fixture.sh
```

Expected: `PASS: mode2-profile TOML output byte-identical to baseline`.

If FAIL, diff inspection. Most likely culprits:
- Property-descriptor walk hit a malformed descriptor that the old code skipped but new code stops at — adjust the `EFI_ERROR (ds) → break` to `continue` if appropriate.
- TOML write format drift (none expected — we didn't touch the `fprintf`).

- [ ] **Step 7: Verify cross-compile targets build**

```bash
cd /home/vivy/gbl-chainload/tools/mode2-profile
make clean
ANDROID_NDK=$ANDROID_NDK make android  # if NDK is set up
make windows                            # zig cc
make macos-x64                          # zig cc
make macos-arm64                        # zig cc
```

Expected: each target builds without warnings or errors.

- [ ] **Step 8: Commit**

```bash
cd /home/vivy/gbl-chainload
git add tools/mode2-profile/Makefile \
        tools/mode2-profile/mode2-profile.c \
        tools/mode2-profile/tests/regression-fixture.sh
# Only commit baseline.toml.golden + vbmeta-fixture.img if user wants the fixture in-tree;
# otherwise leave them as local-only artifacts (.gitignore them).
git commit -m "tools(mode2-profile): use AvbParseLib instead of rolled-own parser

Drops rbe64() and the AVB_*_OFF literal offsets (~30 LOC) in favor of
AvbParse_VbmetaHeader + AvbParse_NextDescriptor from GblChainloadPkg's
AvbParseLib. Aligns mode2-profile.c with vbmeta-graft.c (already uses
AvbParseLib via __HOST_BUILD__).

Regression-gated by tests/regression-fixture.sh: TOML output is verified
byte-identical to a captured baseline before/after the change."
```

---

## Task 4: Cross-tree verification + audit closure

**Goal:** Confirm no stragglers — no other code references the removed B implementations, no other host tool reinvents AVB parsing.

**Files:** None modified. Read-only verification.

**Acceptance Criteria:**
- [ ] `grep -rn "rbe64\|put_u64_be\|put_u32_be" tools/` shows hits only in `tools/vbmeta-graft/vbmeta-graft.c` (writer helpers still local; expected) and `tests/avb/test_avbparse.c` (test helpers; expected). No occurrences in `tools/mode2-profile/`.
- [ ] `grep -rn "AVB_HDR_SIZE\|AVB_AUTH_SIZE_OFF\|AVB_AUX_SIZE_OFF\|AVB_PK_OFF_OFF\|AVB_PK_SIZE_OFF\|AVB_DESC_OFF_OFF\|AVB_DESC_SIZE_OFF" tools/` returns empty.
- [ ] `grep -rn "STATIC EFI_STATUS AvbParse_" edk2/QcomModulePkg/` returns empty.
- [ ] `grep -rn "avb_vbmeta_image_verify" edk2/QcomModulePkg/` — only hits in `Library/avb/libavb/` itself (the definition + internal calls); no consumer outside libavb's tree.
- [ ] `dist/<artifact>.efi` (built in Task 2) loads on the device and `oem vbmeta-status` returns the same per-partition status as the pre-change build for a stock-graft vbmeta.
- [ ] PR description summarizes the audit findings, the consolidation, and links the on-device smoke-test evidence.

**Verify:** All grep commands above + on-device evidence in PR description.

**Steps:**

- [ ] **Step 1: Run the grep audit suite**

```bash
cd /home/vivy/gbl-chainload
echo "=== tools/ local BE helpers (vbmeta-graft writers + test helpers are expected) ==="
grep -rn "rbe64\|put_u64_be\|put_u32_be" tools/ tests/avb/

echo "=== tools/ literal AVB offsets (should be empty) ==="
grep -rn "AVB_HDR_SIZE\|AVB_AUTH_SIZE_OFF\|AVB_AUX_SIZE_OFF\|AVB_PK_OFF_OFF\|AVB_PK_SIZE_OFF\|AVB_DESC_OFF_OFF\|AVB_DESC_SIZE_OFF" tools/

echo "=== edk2 static AvbParse_ (should be empty) ==="
grep -rn "STATIC EFI_STATUS AvbParse_" edk2/QcomModulePkg/

echo "=== libavb consumers (should only be definitions in libavb tree) ==="
grep -rn "avb_vbmeta_image_verify\|avb_descriptor_validate_and_byteswap" edk2/QcomModulePkg/ | grep -v "Library/avb/libavb/"
```

If any of these returns unexpected output, surface it and patch as a follow-up commit.

- [ ] **Step 2: Run all host tests**

```bash
cd /home/vivy/gbl-chainload
make -C tests/avb && ./tests/avb/test_avbparse
make -C tools/vbmeta-graft && ./tools/vbmeta-graft/vbmeta-graft || true  # usage exit 2 OK
bash tools/mode2-profile/tests/regression-fixture.sh
bash tests/runall.sh 2>&1 | tail -20  # full host test suite
```

Expected: each emits its own "PASS" or "ALL PASS" line; no FAIL.

- [ ] **Step 3: Open the PR**

```bash
cd /home/vivy/gbl-chainload
git push -u origin avb-parser-consolidation
gh pr create --base main --title "AVB parser consolidation: single source on AvbParseLib" \
  --body "$(cat <<'EOF'
Consolidates three independent AVB parsing implementations onto a single
source: GblChainloadPkg/Library/AvbParseLib.

**Before:**
- AvbParseLib (manual BE, no libavb dep) — built but dead on-device; host
  tools (vbmeta-graft) only.
- FastbootCmds.c file-local STATIC AvbParse_* helpers (libavb-backed) —
  same names, different signatures, different impls. Used the on-device
  vbmeta walk for the fastboot menu warning.
- mode2-profile.c — third standalone parser with its own rbe64() + literal
  field offsets.

**After:**
- AvbParseLib used everywhere. ~100 LOC removed from FastbootCmds.c.
  ~30 LOC removed from mode2-profile.c. Fastboot menu warning now driven
  by AvbParseLib directly (was: libavb).
- AvbParse_HashDescriptor signature widened additively with three OPTIONAL
  trailing outs (salt pointer, salt_len, image_size) to match what
  FastbootCmds.c was returning. NULL-passthrough verified.

**Verification:**
- tests/avb/test_avbparse — all pass, +2 new tests for optional outs.
- tools/mode2-profile/tests/regression-fixture.sh — TOML output
  byte-identical to pre-change baseline.
- On-device (infiniti): fastboot menu AVB WARNING line renders identically
  to pre-change build. `oem vbmeta-status` returns the same per-partition
  status table.

**Out of scope:**
- No new "graft OK?" verdict API.
- vbmeta-graft list-hash / probe_graft kept as-is.
- AvbParseLib's structural strictness unchanged (no libavb-equivalent
  internal-hash recompute or signature verify added).

**Submodule:** edk2 bumped — see edk2 commit "FastbootLib: route vbmeta
probe through AvbParseLib" on the fork's main branch.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review Checklist

1. **Spec coverage:**
   - ✅ Parser unification (B → A): Task 2.
   - ✅ mode2-profile migration: Task 3.
   - ✅ HashDescriptor signature widen (needed for Task 2): Task 1.
   - ✅ Fastboot menu still works: Task 2 acceptance + on-device smoke.
   - ✅ Verification: Task 4.
   - ✅ Out-of-scope items (verdict API, list-hash split, strictness match) explicitly noted up top.

2. **Placeholder scan:** No "TBD", "implement later", "similar to Task N", or "add error handling". Every code-changing step has the exact code block.

3. **Type consistency:**
   - `AvbParse_HashDescriptor` signature matches in Task 1 declaration, Task 1 implementation, Task 1 test, Task 1 caller updates, and Task 2 acceptance (verified against `FastbootCmds.c:4883` call site).
   - `GBL_AVB_VBMETA_HEADER` field names (`AuthenticationDataBlockSize`, `AuxiliaryDataBlockSize`, `PublicKeyOffset`, `PublicKeySize`, `DescriptorsOffset`, `DescriptorsSize`) match between Task 1's header (already exists) and Task 3's new use.
   - `EFI_END_OF_MEDIA`, `EFI_INVALID_PARAMETER`, `EFI_NOT_FOUND` are EDK2 status codes; via `__HOST_BUILD__` shim in `AvbBigEndian.h` they are typedef'd to int constants for host build (verified by existing `vbmeta-graft.c` use).
