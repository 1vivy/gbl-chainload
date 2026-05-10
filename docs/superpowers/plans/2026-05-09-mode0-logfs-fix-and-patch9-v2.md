# Mode-0 + logfs always-works + patch9 v2 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land mode-0 (minimal foundation), fix logfs to mount across every mode, redesign patch9 to survive multi-binary anchor verification, and validate end-to-end on device through to a custom-recovery boot under mode-1 fakelock.

**Architecture:** Mode-0 is GBL_MODE=0 — patch1 only, no protocol hooks, no patch9. Logfs always mounts (universal across modes), with a "proper transition" (flush+close before LoadImage) so chained EFIs aren't blocked. Patch9 v2 has two sites (Approach A: VerifyFlags-derivation rewrite + post-libavb gate rewrite; Approach B alternative: libavb-internal return remap). Selection is data-driven during disassembly. Anchors validated against 5 PE fixtures (old infiniti, EU 16.0.5.703, IN 16.0.7.201, fairlady CN 16.0.7.200, myron) with at least 3-of-5 PATCH_OK requirement.

**Tech Stack:** EDK-II (UEFI Application + Library, AArch64), C99, host-side disassembly via `aarch64-linux-gnu-objdump` and/or Ghidra MCP, Docker for build, `gh` for git/release.

**Spec reference:** `docs/superpowers/specs/2026-05-09-mode0-logfs-fix-and-patch9-v2-design.md`

**Predecessor:** Plan 1 (`v2.0.0-plan1-foundation` tag, May 9). Repo at `/home/vivy/gbl-chainload`, edk2 submodule at `1vivy/edk2-gbl-chainload@c937d91c64`.

---

## File Structure

```
gbl-chainload/
├── GblChainloadPkg/
│   ├── Application/GblChainload/
│   │   ├── BootFlow.c                          # MODIFY — gate ProtocolHook_InstallAll behind GBL_MODE>=1; add LogFsFlush+Close before LoadImage (proper transition)
│   │   └── Entry.c                             # MODIFY — accept GBL_MODE=0 explicitly; keep LogFsInit unconditional
│   ├── GblChainloadPkg.dsc                      # MODIFY — feature pcd allows GBL_MODE=0
│   └── Library/
│       ├── DynamicPatchLib/
│       │   ├── PatchTable.c                    # MODIFY — gate mode_1 patches behind GBL_MODE>=1 (was ==1)
│       │   ├── mode_1/
│       │   │   ├── Signatures.h                # MODIFY — replace anchor constants for patch9 v2 (Site V, Site G)
│       │   │   └── mode_1.c                    # MODIFY — rewrite ApplyAvbLockedRecoverableContinue for two-site Approach A (or libavb internal for Approach B)
│       │   └── oem/
│       │       └── oneplus_canoe.c             # MODIFY — comment out kOemOneplusPatches[] entry for patch7 (archive)
│       └── LogFsLib/
│           └── Mount.c                         # MODIFY — add diagnostic verbosity around ConnectController; apply root-cause fix
├── images/
│   ├── infiniti/LinuxLoader_infiniti.efi       # symlink, already PE
│   ├── infiniti-EU-16.0.5.703/abl.bin          # raw FV → unwrap to LinuxLoader.efi
│   ├── infiniti-IN-16.0.7.201.img              # raw FV → unwrap
│   ├── fairlady-CN-16.0.7.200.img              # raw FV → unwrap
│   └── pe/                                     # NEW — populated by extract-pe-from-fv tool with unwrapped LinuxLoader.efi per fixture
│       ├── infiniti-EU-16.0.5.703.efi
│       ├── infiniti-IN-16.0.7.201.efi
│       ├── fairlady-CN-16.0.7.200.efi
│       └── myron.efi                           # copied from /home/vivy/gbl_root_canoe/tests/extracted/LinuxLoader.efi
├── scripts/
│   ├── build.sh                                 # MODIFY — argv accepts --mode 0
│   └── extract-pe-from-fv.sh                   # NEW — wrapper around tools/fv-unwrap
├── tests/
│   ├── 045_mode_taxonomy_lint.sh                # MODIFY — assert mode_1 patches gated behind GBL_MODE>=1
│   ├── 046_mode1_protocol_hook_lint.sh          # MODIFY — assert mode-0 path skips ProtocolHook_InstallAll
│   ├── 010_build_smoke.sh                       # MODIFY — build mode-0 + mode-1 default artifacts
│   └── patches/
│       └── test_patch9.c                        # MODIFY — new expected post-bytes for each PE fixture
└── tools/
    ├── abl-patcher/                             # MODIFY — accepts --mode 0 (just for diagnostic; --check-anchors-only excludes patch9 in mode-0)
    └── fv-unwrap/                               # NEW
        ├── fv-unwrap.c                          # NEW — host C tool that strips FV header to expose LinuxLoader.efi PE
        └── Makefile                             # NEW
```

---

## Task 1: Archive patch7 from active mode-1 table

**Files:**
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c`

The patch7 PATCH_DESC source stays in the file (not deleted). The aggregator just doesn't include it.

- [ ] **Step 1: Read the current `kOemOneplusPatches[]` declaration**

```bash
grep -nA 10 'kOemOneplusPatches\[\]' /home/vivy/gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c
```

Expected: shows `CONST PATCH_DESC kOemOneplusPatches[] = { ... patch7-orange-screen ... };` block.

- [ ] **Step 2: Wrap the patch7 entry in `#ifdef GBL_PATCH7_ENABLED`**

Edit `oem/oneplus_canoe.c`. Replace:

```c
CONST PATCH_DESC kOemOneplusPatches[] = {
  {
    .Name      = "patch7-orange-screen",
    .Scope     = SCOPE_OEM_ONEPLUS,
    .Mandatory = FALSE,
    .Apply     = ApplyOrangeScreen,
  },
};
CONST UINTN kOemOneplusPatchesCount =
  sizeof (kOemOneplusPatches) / sizeof (kOemOneplusPatches[0]);
```

with:

```c
CONST PATCH_DESC kOemOneplusPatches[] = {
#ifdef GBL_PATCH7_ENABLED
  {
    .Name      = "patch7-orange-screen",
    .Scope     = SCOPE_OEM_ONEPLUS,
    .Mandatory = FALSE,
    .Apply     = ApplyOrangeScreen,
  },
#else
  /* patch7 archived from active mode-1 table.
     Mode-1 fakelocks the protocol view so the orange-screen warning
     never fires; patch7 is dead at runtime under fakelock.
     Re-enable as a one-line build flag (-DGBL_PATCH7_ENABLED) if patch7
     is wanted for diagnostic purposes (e.g., probing ABL's lock-state
     output behavior when libavb patching needs to be debugged).  */
  { .Name = NULL, .Scope = 0, .Mandatory = FALSE, .Apply = NULL }, /* sentinel */
#endif
};

#ifdef GBL_PATCH7_ENABLED
CONST UINTN kOemOneplusPatchesCount =
  sizeof (kOemOneplusPatches) / sizeof (kOemOneplusPatches[0]);
#else
CONST UINTN kOemOneplusPatchesCount = 0;
#endif
```

The sentinel entry (when patch7 is archived) keeps the array non-empty for ISO C — but `kOemOneplusPatchesCount=0` ensures the aggregator doesn't iterate it.

- [ ] **Step 3: Run host tests — should still pass with patch7 absent**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -10
```

Expected: `ALL TESTS PASS`. The `test_patch7` host test in `tests/patches/test_patch7.c` directly drives `ApplyOrangeScreen` (not via the aggregator), so it should still pass — patch7 source is intact. If `test_patch7` fails because of the sentinel, comment out the test temporarily (we'll handle it in Step 5).

- [ ] **Step 4: If `test_patch7` failed, gate it behind the same flag**

If Step 3 surfaced a `test_patch7` failure due to the changed array shape, edit `tests/patches/test_patch7.c` to bypass the aggregator and call `ApplyOrangeScreen` directly (an `extern PATCH_OUTCOME ApplyOrangeScreen(UINT8*, UINT32);` declaration at the top of the test file, then call it). This keeps `test_patch7` valid as a regression test for `ApplyOrangeScreen` itself, independent of the active table membership.

- [ ] **Step 5: Re-run host tests — pass**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -5
```

Expected: `ALL TESTS PASS`.

- [ ] **Step 6: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c tests/patches/test_patch7.c \
  && git commit -m "DynamicPatchLib: archive patch7 from active mode-1 table (-DGBL_PATCH7_ENABLED to revive)"
```

---

## Task 2: Build the FV-unwrap host tool

**Files:**
- Create: `tools/fv-unwrap/fv-unwrap.c`
- Create: `tools/fv-unwrap/Makefile`
- Create: `scripts/extract-pe-from-fv.sh`

We need a host program that takes a raw `abl_a` FV partition and writes out the embedded `LinuxLoader.efi` PE. The runtime AblUnwrapLib does this on-device; we want the same logic host-side for fixture preparation.

- [ ] **Step 1: Read the runtime AblUnwrapLib source**

```bash
ls /home/vivy/gbl-chainload/GblChainloadPkg/Library/AblUnwrapLib/
```

Expected: at least `AblUnwrapLib.c` and a header. Read the implementation to understand the FV format (Qualcomm's FV with one or more PE inside).

```bash
sed -n '1,80p' /home/vivy/gbl-chainload/GblChainloadPkg/Library/AblUnwrapLib/AblUnwrap.c 2>/dev/null \
  || ls /home/vivy/gbl-chainload/GblChainloadPkg/Library/AblUnwrapLib/
```

The library ports from gbl_root_canoe. The session note 2026-05-08 line 154 mentions `extractfv` (gbl_root_canoe's tool) which links `-llzma`. Some Qualcomm FVs use LZMA section compression; `extractfv` handles this.

- [ ] **Step 2: Write a minimal host tool that handles the simplest case**

Create `tools/fv-unwrap/fv-unwrap.c`:

```c
/** @file fv-unwrap.c — extract a PE32+ image from a Qualcomm-style FV
    partition image.  Host-side, plain C, no EDK-II. **/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* MZ header search: the embedded PE starts at the first 'M' 'Z' pair followed
   by a valid e_lfanew pointing at 'P' 'E' '\0' '\0' inside the FV.
   This handles uncompressed embedded PE only.  For LZMA-compressed sections
   we'd need to link liblzma; out of scope for this minimal tool. */

static size_t find_mz_pe (const unsigned char *buf, size_t size, size_t *peSize) {
  for (size_t i = 0; i + 0x80 + 4 < size; ++i) {
    if (buf[i] == 'M' && buf[i + 1] == 'Z') {
      uint32_t e_lfanew = (uint32_t)buf[i + 0x3C]
                       | ((uint32_t)buf[i + 0x3D] << 8)
                       | ((uint32_t)buf[i + 0x3E] << 16)
                       | ((uint32_t)buf[i + 0x3F] << 24);
      size_t peoff = i + e_lfanew;
      if (peoff + 4 < size
          && buf[peoff] == 'P' && buf[peoff + 1] == 'E'
          && buf[peoff + 2] == 0 && buf[peoff + 3] == 0) {
        /* Found a valid PE.  Compute size from the SizeOfImage field
           of the optional header.  PE header at peoff, COFF header
           at peoff+4 (20 bytes), opt header starts at peoff+24. */
        size_t opthdr = peoff + 4 + 20;
        if (opthdr + 60 < size) {
          /* SizeOfImage at opt-header + 56 (PE32) or +56 (PE32+).
             For our PE32+ case (AArch64 UEFI applications), it's at +56. */
          uint32_t sz = (uint32_t)buf[opthdr + 56]
                     | ((uint32_t)buf[opthdr + 57] << 8)
                     | ((uint32_t)buf[opthdr + 58] << 16)
                     | ((uint32_t)buf[opthdr + 59] << 24);
          /* SizeOfImage is the in-memory size; the on-disk size may be
             smaller (alignment).  As a coarse estimate use SizeOfImage,
             clamped to the remaining buffer. */
          if (sz == 0 || i + sz > size) sz = (uint32_t)(size - i);
          *peSize = sz;
          return i;
        }
      }
    }
  }
  return (size_t)-1;
}

int main (int argc, char **argv) {
  if (argc != 3) {
    fprintf (stderr, "Usage: %s <fv-partition.bin> <pe-output.efi>\n", argv[0]);
    return 2;
  }
  FILE *f = fopen (argv[1], "rb");
  if (!f) { perror (argv[1]); return 1; }
  fseek (f, 0, SEEK_END);
  long sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  if (sz <= 0) { fprintf (stderr, "%s: empty\n", argv[1]); return 1; }
  unsigned char *buf = (unsigned char *)malloc ((size_t)sz);
  if (fread (buf, 1, (size_t)sz, f) != (size_t)sz) {
    fprintf (stderr, "%s: read failed\n", argv[1]); return 1;
  }
  fclose (f);

  size_t peSize = 0;
  size_t peOff = find_mz_pe (buf, (size_t)sz, &peSize);
  if (peOff == (size_t)-1) {
    fprintf (stderr, "%s: no MZ/PE found in FV\n", argv[1]);
    free (buf); return 1;
  }

  fprintf (stderr, "%s: PE at offset 0x%zx, size 0x%zx (%zu bytes)\n",
           argv[1], peOff, peSize, peSize);

  FILE *o = fopen (argv[2], "wb");
  if (!o) { perror (argv[2]); free (buf); return 1; }
  fwrite (buf + peOff, 1, peSize, o);
  fclose (o);
  fprintf (stderr, "wrote %zu bytes to %s\n", peSize, argv[2]);
  free (buf);
  return 0;
}
```

- [ ] **Step 3: Write `tools/fv-unwrap/Makefile`**

```makefile
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11

fv-unwrap: fv-unwrap.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f fv-unwrap *.o
```

- [ ] **Step 4: Build the tool**

```bash
cd /home/vivy/gbl-chainload/tools/fv-unwrap && make
```

Expected: `fv-unwrap` binary, no warnings.

- [ ] **Step 5: Write the wrapper script**

Create `scripts/extract-pe-from-fv.sh`:

```bash
#!/usr/bin/env bash
# extract-pe-from-fv.sh — unwrap LinuxLoader.efi from each raw FV in images/
# into images/pe/<fixture>.efi.  Skip fixtures that already have a PE form.
set -euo pipefail

cd "$(dirname "$0")/.."
make -C tools/fv-unwrap

mkdir -p images/pe

# Per-fixture extraction.  Add new entries when more fixtures land.
declare -A FV_TO_PE=(
  ["images/infiniti-EU-16.0.5.703/abl.bin"]="images/pe/infiniti-EU-16.0.5.703.efi"
  ["images/infiniti-IN-16.0.7.201.img"]="images/pe/infiniti-IN-16.0.7.201.efi"
  ["images/fairlady-CN-16.0.7.200.img"]="images/pe/fairlady-CN-16.0.7.200.efi"
)

for src in "${!FV_TO_PE[@]}"; do
  dest="${FV_TO_PE[$src]}"
  if [[ -f "$src" ]]; then
    echo "==> $src → $dest"
    tools/fv-unwrap/fv-unwrap "$src" "$dest" || {
      echo "WARN: failed to extract $src (FV may need LZMA decompression — out of scope)"; continue;
    }
    sha256sum "$dest"
  else
    echo "SKIP: $src (file missing)"
  fi
done

# Copy the gbl_root_canoe-extracted myron PE (already a PE).
if [[ -f /home/vivy/gbl_root_canoe/tests/extracted/LinuxLoader.efi ]]; then
  cp /home/vivy/gbl_root_canoe/tests/extracted/LinuxLoader.efi images/pe/myron.efi
  sha256sum images/pe/myron.efi
fi

echo "==> Done. PE fixtures available:"
ls -la images/pe/
```

`chmod +x scripts/extract-pe-from-fv.sh`.

- [ ] **Step 6: Run the extraction**

```bash
cd /home/vivy/gbl-chainload && bash scripts/extract-pe-from-fv.sh
```

Expected: `images/pe/` contains 3-4 `.efi` files (3 unwrapped from FV + 1 copied from myron). Each is a PE32+ image readable by `objdump`.

- [ ] **Step 7: Verify each PE is a valid AArch64 PE**

```bash
for f in /home/vivy/gbl-chainload/images/pe/*.efi /home/vivy/gbl-chainload/images/infiniti/LinuxLoader_infiniti.efi; do
  echo "=== $f ==="
  file "$f"
done
```

Expected: each file is identified as `MS-DOS executable, MZ for MS-DOS` or `PE32+ executable (EFI application) Aarch64`. If a fixture comes back as "data" (no PE detected), the FV may be LZMA-compressed — surface that as DONE_WITH_CONCERNS for that specific fixture.

- [ ] **Step 8: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add tools/fv-unwrap scripts/extract-pe-from-fv.sh \
  && git commit -m "tools/fv-unwrap: host program to extract LinuxLoader PE from raw FV partition"
```

The `images/pe/` outputs are gitignored (per the `images/` rule), so they're not tracked.

---

## Task 3: Disassemble libavb call region across all PE fixtures

**Files:**
- Create: `docs/re/patch9-v2-disassembly.md` (an investigation doc — output of this task is data, not code)

This task is a one-off RE step. We disassemble the `LoadImageAndAuthVB2` region in each available PE fixture and capture the instruction sequences around (a) the `bl avb_slot_verify` call, (b) the post-libavb gate. The captured data drives Tasks 4-6.

- [ ] **Step 1: Locate the libavb call site in the gbl_root_canoe infiniti PE (known reference)**

The known site from session note 2026-05-08 line 322-352:
- VerifyFlagsInit at 0x25388
- RecoveryGate at 0x25A64
- CommonGate at 0x25C44

Disassemble around each:

```bash
F=/home/vivy/gbl-chainload/images/infiniti/LinuxLoader_infiniti.efi
for off in 0x25368 0x25A44 0x25C24; do
  echo "=== infiniti $off (32 bytes) ==="
  aarch64-linux-gnu-objdump -D -b binary -m aarch64 \
    --adjust-vma=0 --start-address=$off --stop-address=$((off+32)) \
    "$F" 2>&1 | grep -E '^[ ]+[0-9a-f]+:'
done
```

If `aarch64-linux-gnu-objdump` isn't available, use `objdump -D -b binary -m aarch64` (modern binutils handles AArch64 natively).

Save the output. This is the BASELINE — what we anchor against.

- [ ] **Step 2: For each unwrapped PE in `images/pe/`, locate the equivalent sites**

For each file in `images/pe/`:

```bash
for F in /home/vivy/gbl-chainload/images/pe/*.efi; do
  echo "=== $F ==="
  # Find the libavb call site by searching for the unique surrounding pattern.
  # As a coarse first attempt, look for the same instruction sequence we know
  # from infiniti.  If the offsets shifted, this gives us the new offset.
  python3 -c "
import sys
with open('$F','rb') as f:
    data = f.read()
# Search for the original cset+context bytes (32 bytes around 0x25388)
import struct
ref = open('/home/vivy/gbl-chainload/images/infiniti/LinuxLoader_infiniti.efi','rb').read()
ref_anchor = ref[0x25368:0x25388]  # 32 bytes ending at the cset
for i in range(0, len(data) - 32):
    if data[i:i+32] == ref_anchor:
        print(f'EXACT match at 0x{i:x}')
        break
else:
    print('No exact match — pattern shifted or differs')
"
done
```

If exact match: we have the offset on this fixture; proceed with disassembly at that offset.
If no exact match: we need a different anchor strategy — see Step 3.

- [ ] **Step 3: Locate libavb call sites by string-anchoring (when exact byte match fails)**

The libavb function `avb_slot_verify` has a recognizable string in its codebase: error strings like "Error verifying vbmeta image:", "OK_NOT_SIGNED", "Hashtree error mode". Find these strings in the binary, then walk back to find the BLs that load them as arguments to error-printing functions.

For each PE without an exact match:

```bash
for F in /home/vivy/gbl-chainload/images/pe/*.efi; do
  echo "=== $F: AVB strings ==="
  # 4-byte magic and known libavb strings
  strings -t x "$F" | grep -E 'AVB0|allow_verification_error|OK_NOT_SIGNED|avb_slot_verify' | head -20
done
```

The string `OK_NOT_SIGNED` (or its preceding fragment in the strings table) is a libavb-specific marker. Its file offset gives us a known anchor; cross-references (xrefs) from `bl <error_print>(<this_string>)` instructions inside `avb_slot_verify` reveal the function's address range.

Document the offset of `OK_NOT_SIGNED` and the function range in `docs/re/patch9-v2-disassembly.md`.

- [ ] **Step 4: Disassemble `LoadImageAndAuthVB2` (or equivalent) on each fixture**

For each fixture, use the located function offset to disassemble the region:

```bash
# Replace <START> and <END> with the function range identified in Step 3
aarch64-linux-gnu-objdump -D -b binary -m aarch64 \
  --adjust-vma=0 --start-address=<START> --stop-address=<END> \
  /home/vivy/gbl-chainload/images/pe/<FIXTURE>.efi > /tmp/dis-<fixture>.txt
```

Save each fixture's disassembly to `/tmp/dis-<fixture>.txt`.

- [ ] **Step 5: Locate Site V (VerifyFlags derivation) in each disassembly**

In each `/tmp/dis-<fixture>.txt`, find the instruction sequence that derives `VerifyFlags` from `AllowVerificationError`. Per the libavb audit (spec §4): this is the source-equivalent of `VerifiedBoot.c:1379-1381`. Compiler typically emits something like:

```
cmp Wbool, #0       ; check AllowVerificationError
mov Wflags, #0      ; default flags = 0
b.eq +4
mov Wflags, #1      ; if not zero, flags |= ALLOW_VERIFICATION_ERROR
str Wflags, [SP, #N]   ; store to stack for later avb_slot_verify call arg
```

Or more compactly:

```
csel Wflags, Wone, Wzr, NE
```

Or:

```
and Wflags, Wbool, #1
```

Identify the specific instructions on each fixture. Record file offsets per fixture.

- [ ] **Step 6: Locate Site G (post-libavb gate) in each disassembly**

After `bl avb_slot_verify` (or `bl avb_slot_verify_full`) returns, ABL evaluates:

```c
if (AllowVerificationError && ResultShouldContinue(Result)) { /* continue */ }
else if (Result != OK) { /* fatal */ }
else { /* OK + rollback path */ }
```

In assembly, this typically becomes:

```
bl  avb_slot_verify        ; call into libavb
mov Wresult, W0            ; capture result
cbz Wbool, <else_branch>   ; if AllowVerificationError == 0, jump to else
bl  ResultShouldContinue   ; check result
cbz W0, <fatal_else>
b   <continue_path>
<else_branch>:
cbnz Wresult, <fatal>
<rollback_path>:
...
```

Record the cbz on AllowVerificationError (Site G candidate) per fixture.

- [ ] **Step 7: Compile the findings into `docs/re/patch9-v2-disassembly.md`**

Create `docs/re/patch9-v2-disassembly.md` with a per-fixture table:

```markdown
# Patch9 v2 disassembly findings

## Per-fixture sites

| Fixture | Has GBL? | Site V offset | Site V instructions (hex) | Site G offset | Site G instructions (hex) |
|---|---|---|---|---|---|
| infiniti (gbl_root_canoe) | yes | 0x........ | ... | 0x........ | ... |
| infiniti-EU-16.0.5.703 | yes | 0x........ | ... | 0x........ | ... |
| infiniti-IN-16.0.7.201 | unknown | 0x........ | ... | 0x........ | ... |
| fairlady-CN-16.0.7.200 | unknown | 0x........ | ... | 0x........ | ... |
| myron | yes | 0x........ | ... | 0x........ | ... |

## Cross-fixture comparison

[Describe which bytes are stable across fixtures and which shift.]

## Anchor candidates

- Site V anchor: <byte sequence + length + mask if needed>
- Site G anchor: <byte sequence + length + mask if needed>

## Approach decision

[A or B, with rationale based on cross-fixture stability.]
```

Fill in the table during disassembly. The goal is a clear data picture before writing patch9 v2's actual code.

- [ ] **Step 8: Commit the disassembly doc**

```bash
cd /home/vivy/gbl-chainload \
  && git add docs/re/patch9-v2-disassembly.md \
  && git commit -m "re: patch9 v2 disassembly findings across 5 PE fixtures"
```

---

## Task 4: Decide Approach A vs B

**Files:**
- Modify: `docs/re/patch9-v2-disassembly.md` (add the decision)

Based on Task 3's findings, choose between Approach A (Site V + Site G ABL-side rewrites) and Approach B (libavb-internal return remap).

- [ ] **Step 1: Score Approach A's anchor stability**

For Site V and Site G anchors:
- How many of 5 fixtures had unique-and-correct matches with a 24-byte anchor?
- If <3, score down; consider shorter anchors with masked wildcards. If still <3 after wildcards, Approach A is fragile.

- [ ] **Step 2: Score Approach B's anchor stability**

If we go Approach B, we need to find `avb_slot_verify_full`'s return-value setup site. In each disassembly:
- Locate the function entry by signature (the libavb function has specific prologue patterns and returns AvbSlotVerifyResult enum values).
- Find the `mov W0, #N` instructions that set return values (`OK=0, ERROR_OOM=1, ..., OK_NOT_SIGNED=4` per libavb's enum).
- Find the path that handles the recoverable-error case (where OK_NOT_SIGNED is set).
- Score: how many of 5 fixtures have this site with a stable anchor?

- [ ] **Step 3: Pick the architecture with the higher cross-fixture stability**

Document the decision in `docs/re/patch9-v2-disassembly.md`. Whichever architecture wins is what Tasks 5-7 implement. The other becomes "alternative not selected" — note it briefly so future iterations know which fork was tried.

- [ ] **Step 4: Commit the decision**

```bash
cd /home/vivy/gbl-chainload \
  && git add docs/re/patch9-v2-disassembly.md \
  && git commit -m "re: patch9 v2 architecture decision (A or B) based on disassembly findings"
```

---

## Task 5: Implement patch9 v2 code (chosen architecture)

**Files:**
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/mode_1/Signatures.h`
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/mode_1/mode_1.c`

This task implements the architecture chosen in Task 4. The structure is identical for A and B; only the anchor constants differ.

- [ ] **Step 1: Replace `Signatures.h` with the new anchor constants**

If Approach A: Two anchors (kPatch9SiteVAnchor, kPatch9SiteGAnchor).
If Approach B: One anchor (kPatch9LibavbReturnAnchor).

Edit `GblChainloadPkg/Library/DynamicPatchLib/mode_1/Signatures.h`:

For Approach A, replace the existing three-site constants with:

```c
#ifndef DPL_MODE_1_SIGNATURES_H_
#define DPL_MODE_1_SIGNATURES_H_

#include "../../../Include/Library/ScanLib.h"

/* patch9 v2 — Approach A: VerifyFlags-derivation rewrite + post-libavb gate rewrite.
   See docs/re/patch9-v2-disassembly.md for the data driving these constants. */

/* Site V: VerifyFlags derivation.  Anchor matches the instruction sequence
   immediately preceding the rewrite point.  The rewrite forces VerifyFlags
   to always include AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR. */
STATIC CONST UINT8 kPatch9SiteVAnchor[] = {
  /* <FROM TASK 3 DISASSEMBLY> — 24-32 byte sequence ending just before
     the VerifyFlags-derivation instruction. */
};
#define kPatch9SiteVAnchorLen     sizeof (kPatch9SiteVAnchor)
#define kPatch9SiteVRewriteDelta  /* offset from anchor match to the rewrite-target instruction */
#define kPatch9SiteVReplacement   /* the new instruction word that always sets the flag */

/* Optional mask for variable bytes (e.g., stack offsets). NULL = exact match. */
STATIC CONST UINT8 *kPatch9SiteVMask = NULL;

/* Site G: post-libavb gate.  Anchor at the cbz on AllowVerificationError.
   Rewrite forces the gate to always proceed (turns the cbz into a no-op,
   so control flow takes the recoverable-continue branch). */
STATIC CONST UINT8 kPatch9SiteGAnchor[] = {
  /* <FROM TASK 3 DISASSEMBLY> */
};
#define kPatch9SiteGAnchorLen     sizeof (kPatch9SiteGAnchor)
#define kPatch9SiteGRewriteDelta  /* offset from anchor match to the cbz */
#define kPatch9SiteGReplacement   /* the new instruction word — typically 0xD503201F (nop) */

STATIC CONST UINT8 *kPatch9SiteGMask = NULL;

#endif
```

For Approach B:

```c
/* patch9 v2 — Approach B: libavb-internal return remap.
   Single rewrite at avb_slot_verify_full's recoverable-result return path. */

STATIC CONST UINT8 kPatch9LibavbReturnAnchor[] = {
  /* <FROM TASK 3 DISASSEMBLY> */
};
#define kPatch9LibavbReturnAnchorLen     sizeof (kPatch9LibavbReturnAnchor)
#define kPatch9LibavbReturnRewriteDelta  /* offset from anchor match to the mov W0 instruction */
#define kPatch9LibavbReturnReplacement   /* mov W0, #0 (AVB_SLOT_VERIFY_RESULT_OK) = 0x52800000 */

STATIC CONST UINT8 *kPatch9LibavbReturnMask = NULL;

#endif
```

Fill in the actual byte sequences from Task 3's findings.

- [ ] **Step 2: Rewrite `mode_1.c::ApplyAvbLockedRecoverableContinue`**

For Approach A:

```c
/** @file mode_1.c — mode-1-scope patches (only included when GBL_MODE>=1). **/

#include "../../../Include/Library/PatchDesc.h"
#include "../../../Include/Library/ScanLib.h"
#include "../Internal/Encode.h"
#include "Signatures.h"

STATIC PATCH_OUTCOME
ApplyAvbLockedRecoverableContinue (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      VOff, GOff;
  SCAN_RESULT R;

  /* Site V — force VerifyFlags's ALLOW_VERIFICATION_ERROR bit. */
  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9SiteVAnchor, kPatch9SiteVMask,
                             kPatch9SiteVAnchorLen, &VOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  /* Site G — bypass the post-libavb gate's AllowVerificationError test. */
  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9SiteGAnchor, kPatch9SiteGMask,
                             kPatch9SiteGAnchorLen, &GOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  WriteInstrU32 (Buf, VOff + kPatch9SiteVRewriteDelta, kPatch9SiteVReplacement);
  WriteInstrU32 (Buf, GOff + kPatch9SiteGRewriteDelta, kPatch9SiteGReplacement);
  return PATCH_OK;
}

CONST PATCH_DESC kMode1Patches[] = {
  {
    .Name      = "patch9-avb-locked-recoverable-continue",
    .Scope     = SCOPE_MODE_1,
    .Mandatory = TRUE,
    .Apply     = ApplyAvbLockedRecoverableContinue,
  },
};
CONST UINTN kMode1PatchesCount =
  sizeof (kMode1Patches) / sizeof (kMode1Patches[0]);
```

For Approach B (single site):

```c
STATIC PATCH_OUTCOME
ApplyAvbLockedRecoverableContinue (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      Off;
  SCAN_RESULT R;

  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9LibavbReturnAnchor, kPatch9LibavbReturnMask,
                             kPatch9LibavbReturnAnchorLen, &Off);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  /* Remap the recoverable-result mov W0,#OK_NOT_SIGNED → mov W0,#OK (0). */
  WriteInstrU32 (Buf, Off + kPatch9LibavbReturnRewriteDelta,
                 kPatch9LibavbReturnReplacement);
  return PATCH_OK;
}

CONST PATCH_DESC kMode1Patches[] = {
  {
    .Name      = "patch9-avb-locked-recoverable-continue",
    .Scope     = SCOPE_MODE_1,
    .Mandatory = TRUE,
    .Apply     = ApplyAvbLockedRecoverableContinue,
  },
};
CONST UINTN kMode1PatchesCount =
  sizeof (kMode1Patches) / sizeof (kMode1Patches[0]);
```

- [ ] **Step 3: Don't run device tests yet — just verify the code compiles host-side**

```bash
cd /home/vivy/gbl-chainload && make -C tests/scan clean && make -C tests/scan 2>&1 | tail -10
```

Expected: scan tests still pass (mode_1.c isn't in the host-test path; it links via abl-patcher).

```bash
cd /home/vivy/gbl-chainload && make -C tools/abl-patcher clean && make -C tools/abl-patcher 2>&1 | tail -5
```

Expected: clean build, no warnings. abl-patcher rebuilds with the new mode_1.c.

If compile errors surface — fix them; surface DONE_WITH_CONCERNS if the new constants in Signatures.h are syntactically off.

- [ ] **Step 4: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Library/DynamicPatchLib/mode_1/{Signatures.h,mode_1.c} \
  && git commit -m "DynamicPatchLib: patch9 v2 (chosen architecture from disassembly findings)"
```

---

## Task 6: Update `tests/patches/test_patch9.c` for multi-fixture validation

**Files:**
- Modify: `tests/patches/test_patch9.c`

The test must now drive patch9 against every available PE fixture and assert the per-fixture expected post-bytes (recorded from Task 3 disassembly + Task 5's rewrite logic).

- [ ] **Step 1: Replace the test main with a multi-fixture loop**

Replace `tests/patches/test_patch9.c` content with:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../../GblChainloadPkg/Include/Library/PatchDesc.h"

extern CONST PATCH_DESC kMode1Patches[];
extern CONST UINTN      kMode1PatchesCount;

static UINT8 *load_file (const char *path, UINT32 *size_out) {
  FILE *f = fopen (path, "rb");
  if (!f) return NULL;
  fseek (f, 0, SEEK_END);
  long sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  UINT8 *buf = (UINT8 *)malloc ((size_t)sz);
  fread (buf, 1, (size_t)sz, f);
  fclose (f);
  *size_out = (UINT32)sz;
  return buf;
}

typedef struct {
  CONST char *path;
  int         expect_required;   /* 1 = MUST PATCH_OK; 0 = OK or MISS acceptable */
  /* Per-fixture expected post-bytes (filled in from disassembly+rewrite logic).
     Format: { offset, expected_word_le }, terminated by { 0, 0 }. */
  struct { UINT32 off; UINT32 word_le; } expected[6];
} fixture_t;

/* Populate from docs/re/patch9-v2-disassembly.md.  Each fixture's
   `expected` array lists the byte offsets and expected little-endian
   instruction words after patch9 v2 applies. */
static fixture_t fixtures[] = {
  {
    .path = "/home/vivy/gbl-chainload/images/infiniti/LinuxLoader_infiniti.efi",
    .expect_required = 1,
    .expected = {
      /* {Site V offset, expected word}, {Site G offset, expected word}, {0,0} */
      { 0, 0 }
    },
  },
  {
    .path = "/home/vivy/gbl-chainload/images/pe/infiniti-EU-16.0.5.703.efi",
    .expect_required = 1,
    .expected = { { 0, 0 } },
  },
  {
    .path = "/home/vivy/gbl-chainload/images/pe/infiniti-IN-16.0.7.201.efi",
    .expect_required = 0,  /* depends on whether GBL was removed; relax to optional */
    .expected = { { 0, 0 } },
  },
  {
    .path = "/home/vivy/gbl-chainload/images/pe/fairlady-CN-16.0.7.200.efi",
    .expect_required = 0,
    .expected = { { 0, 0 } },
  },
  {
    .path = "/home/vivy/gbl-chainload/images/pe/myron.efi",
    .expect_required = 1,
    .expected = { { 0, 0 } },
  },
};

int main (void) {
  int passed = 0, failed = 0, skipped = 0;
  PATCH_APPLY apply = NULL;
  for (UINTN i = 0; i < kMode1PatchesCount; ++i) {
    if (strcmp (kMode1Patches[i].Name, "patch9-avb-locked-recoverable-continue") == 0) {
      apply = kMode1Patches[i].Apply;
      break;
    }
  }
  assert (apply != NULL);

  for (size_t i = 0; i < sizeof (fixtures) / sizeof (fixtures[0]); ++i) {
    fixture_t *fx = &fixtures[i];
    UINT32 size = 0;
    UINT8 *buf = load_file (fx->path, &size);
    if (!buf) {
      printf ("skip %s (file missing)\n", fx->path);
      ++skipped; continue;
    }

    PATCH_OUTCOME o = apply (buf, size);
    if (o == PATCH_OK) {
      /* Verify expected post-bytes. */
      int byte_check_ok = 1;
      for (size_t j = 0; j < sizeof (fx->expected) / sizeof (fx->expected[0]); ++j) {
        if (fx->expected[j].off == 0 && fx->expected[j].word_le == 0) break;
        UINT32 got = (UINT32)buf[fx->expected[j].off]
                  | ((UINT32)buf[fx->expected[j].off + 1] << 8)
                  | ((UINT32)buf[fx->expected[j].off + 2] << 16)
                  | ((UINT32)buf[fx->expected[j].off + 3] << 24);
        if (got != fx->expected[j].word_le) {
          printf ("FAIL %s @0x%x: got 0x%08x expected 0x%08x\n",
                  fx->path, fx->expected[j].off, got, fx->expected[j].word_le);
          byte_check_ok = 0;
        }
      }
      if (byte_check_ok) {
        printf ("ok %s — PATCH_OK + bytes match\n", fx->path);
        ++passed;
      } else {
        ++failed;
      }
    } else {
      if (fx->expect_required) {
        printf ("FAIL %s — expected PATCH_OK, got %d\n", fx->path, o);
        ++failed;
      } else {
        printf ("ok %s — PATCH_MISS acceptable (optional fixture, may have GBL removed)\n", fx->path);
        ++passed;
      }
    }
    free (buf);
  }

  printf ("---\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);

  /* Spec stop-line: 3+ PE fixtures must hit PATCH_OK. */
  if (passed < 3) {
    printf ("FAIL: <3 fixtures hit PATCH_OK; spec stop-line violated\n");
    return 1;
  }

  return failed == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Fill in the per-fixture `expected` arrays**

Open `docs/re/patch9-v2-disassembly.md`. For each fixture row, compute the post-patch instruction word at Site V's rewrite offset and Site G's rewrite offset using:
- `kPatch9SiteVReplacement` (the new instruction word for Site V)
- `kPatch9SiteGReplacement` (the new instruction word for Site G)

Per fixture, the offsets are: `(SiteVAnchorMatchOffset + kPatch9SiteVRewriteDelta)` and `(SiteGAnchorMatchOffset + kPatch9SiteGRewriteDelta)`.

Update each `.expected` array in test_patch9.c with the actual numeric offset and expected post-byte word for that fixture. Both Approach A and B follow the same shape (B has just one entry per fixture).

- [ ] **Step 3: Update the Makefile to include the patch9 test against new mode_1.c**

The existing rule should already pick up `mode_1.c`. Verify:

```bash
grep -A 3 'test_patch9:' /home/vivy/gbl-chainload/tests/patches/Makefile
```

Expected: rule lists `mode_1/mode_1.c`. If it already does, no change needed.

- [ ] **Step 4: Run the test**

```bash
cd /home/vivy/gbl-chainload/tests/patches && make clean && make 2>&1 | tail -30
```

Expected: `test_patch9` shows `≥3 passed, 0 failed`, exits 0.

If a fixture fails the byte-check: re-derive its anchor (Task 3 may have a stale offset), re-update Signatures.h (Task 5), re-run.

- [ ] **Step 5: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add tests/patches/test_patch9.c \
  && git commit -m "tests/patches: multi-fixture patch9 v2 validation (3-of-5 PATCH_OK requirement)"
```

---

## Task 7: Add GBL_MODE=0 plumbing to PatchTable + BootFlow

**Files:**
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/PatchTable.c`
- Modify: `GblChainloadPkg/Application/GblChainload/BootFlow.c`

- [ ] **Step 1: Update PatchTable.c — gate mode_1 patches behind `GBL_MODE>=1`**

Edit `GblChainloadPkg/Library/DynamicPatchLib/PatchTable.c`. Find the existing `#if (GBL_MODE == 1)` and replace with `#if (GBL_MODE >= 1)`:

```c
extern CONST PATCH_DESC kUniversalPatches[];
extern CONST UINTN      kUniversalPatchesCount;
extern CONST PATCH_DESC kOemOneplusPatches[];
extern CONST UINTN      kOemOneplusPatchesCount;

#if (GBL_MODE >= 1)
extern CONST PATCH_DESC kMode1Patches[];
extern CONST UINTN      kMode1PatchesCount;
#endif

/* ... InitAggregate body ... */

STATIC VOID
InitAggregate (VOID) {
  UINTN n = 0;
  UINTN i;

  for (i = 0; i < kUniversalPatchesCount; ++i) {
    if (n < MAX_PATCHES) gAggregated[n++] = kUniversalPatches[i];
  }
  for (i = 0; i < kOemOneplusPatchesCount; ++i) {
    if (n < MAX_PATCHES) gAggregated[n++] = kOemOneplusPatches[i];
  }
#if (GBL_MODE >= 1)
  for (i = 0; i < kMode1PatchesCount; ++i) {
    if (n < MAX_PATCHES) gAggregated[n++] = kMode1Patches[i];
  }
#endif
  /* ... rest unchanged ... */
}
```

When `GBL_MODE=0`, only universal patches (patch1) and the now-empty `kOemOneplusPatches` (since patch7 is archived) are aggregated.

- [ ] **Step 2: Update BootFlow.c — gate `ProtocolHook_InstallAll` behind `GBL_MODE>=1`**

Edit `GblChainloadPkg/Application/GblChainload/BootFlow.c`. Find the `ProtocolHook_InstallAll` call:

```c
/* 3. Install protocol hooks (universal baseline + mode-N overlay).
      Mode-0 skips this entirely — no fakelock, no SCM/OplusSec drops. */
#if (GBL_MODE >= 1)
  Status = ProtocolHook_InstallAll (&HookRes);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootFlow: hook install failed (%r) - aborting\n",
            Status));
    SCR_PRINT (L"BootFlow: hook install failed (%r) - aborting\n", Status);
    FreePool (Pe);
    return Status;
  }
#else
  DEBUG ((DEBUG_INFO, "BootFlow: mode-0 — skipping ProtocolHook_InstallAll\n"));
  SCR_PRINT (L"BootFlow: mode-0 — skipping ProtocolHook_InstallAll\n");
#endif
```

- [ ] **Step 3: Update Entry.c — accept GBL_MODE=0 explicitly**

Edit `GblChainloadPkg/Application/GblChainload/Entry.c`. Find the `#error` guard:

```c
#ifndef GBL_MODE
# error "GBL_MODE (0, 1, 2, or 3) must be defined at build time"
#endif
```

Replace with the same line listing 0 as valid.

In `InstallAll.c` (ProtocolHookLib), the `#error "GBL_MODE must be 1, 2, or 3"` guard needs updating too — but ProtocolHook_InstallAll is now never called in mode-0 (per Step 2), so InstallAll.c's body is dead code in mode-0. Add a guard:

```c
/* In InstallAll.c: */
#if (GBL_MODE == 0)
EFI_STATUS
EFIAPI
ProtocolHook_InstallAll (
  OUT HOOK_INSTALL_RESULT  *Result
  )
{
  /* Mode-0 — no protocol hooks installed.  Caller (BootFlow.c) doesn't call
     this in mode-0, but provide a stub to satisfy the linker. */
  if (Result != NULL) ZeroMem (Result, sizeof (*Result));
  Result->UniversalRequiredOk = TRUE;
  Result->ModeOverlayOk       = TRUE;
  return EFI_SUCCESS;
}
#elif (GBL_MODE == 1 || GBL_MODE == 2 || GBL_MODE == 3)
/* ... existing body ... */
#else
# error "GBL_MODE must be 0, 1, 2, or 3"
#endif
```

- [ ] **Step 4: Update tests/045_mode_taxonomy_lint.sh**

Edit `tests/045_mode_taxonomy_lint.sh`. Replace the assertion that PatchTable.c gates `GBL_MODE == 1` with `GBL_MODE >= 1`:

```bash
grep -q '#if (GBL_MODE >= 1)' "$PKG/PatchTable.c" \
  || { echo "FAIL: PatchTable.c must gate mode_1 patches behind GBL_MODE>=1"; exit 1; }
```

- [ ] **Step 5: Update tests/046_mode1_protocol_hook_lint.sh**

Add an assertion that BootFlow.c gates `ProtocolHook_InstallAll` behind `GBL_MODE >= 1`:

```bash
grep -q '#if (GBL_MODE >= 1)' \
  GblChainloadPkg/Application/GblChainload/BootFlow.c \
  || { echo "FAIL: BootFlow.c must gate ProtocolHook_InstallAll behind GBL_MODE>=1"; exit 1; }
```

- [ ] **Step 6: Run lints — pass**

```bash
cd /home/vivy/gbl-chainload \
  && bash tests/045_mode_taxonomy_lint.sh \
  && bash tests/046_mode1_protocol_hook_lint.sh
```

Expected: both `ok 045_mode_taxonomy_lint` and `ok 046_mode1_protocol_hook_lint`.

- [ ] **Step 7: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Library/DynamicPatchLib/PatchTable.c \
            GblChainloadPkg/Application/GblChainload/BootFlow.c \
            GblChainloadPkg/Application/GblChainload/Entry.c \
            GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c \
            tests/045_mode_taxonomy_lint.sh \
            tests/046_mode1_protocol_hook_lint.sh \
  && git commit -m "build: GBL_MODE=0 support — gate mode_1 patches + ProtocolHook_InstallAll behind >=1"
```

---

## Task 8: Update build.sh + 010 build smoke for mode-0

**Files:**
- Modify: `scripts/build.sh`
- Modify: `tests/010_build_smoke.sh`

- [ ] **Step 1: Update `scripts/build.sh` argv handling**

Edit `scripts/build.sh`. The mode validator currently warns for modes other than 1. Update:

```bash
case "$MODE" in
  0|1) ;;
  2|3) echo "WARN: mode $MODE not yet supported in plan 1; building anyway" >&2 ;;
  *) echo "--mode must be 0, 1, 2, or 3" >&2; exit 2;;
esac
```

- [ ] **Step 2: Run a clean mode-0 build**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 0 2>&1 | tail -10
```

Expected: `==> Built dist/mode-0.efi (XXXX bytes)`. The binary should be smaller than mode-1 (no protocol hooks linked).

- [ ] **Step 3: Run mode-0 with all flags**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 0 --auto --debug --verbose 2>&1 | tail -5
```

Expected: `dist/mode-0-auto-debug-verbose.efi` exists.

- [ ] **Step 4: Update `tests/010_build_smoke.sh` to build mode-0 too**

Edit `tests/010_build_smoke.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== building dist/mode-0.efi =="
./scripts/build.sh --mode 0
test -f dist/mode-0.efi || { echo "FAIL: dist/mode-0.efi missing"; exit 1; }

echo "== building dist/mode-1.efi =="
./scripts/build.sh --mode 1
test -f dist/mode-1.efi || { echo "FAIL: dist/mode-1.efi missing"; exit 1; }

echo "== building dist/mode-1-auto-debug-verbose.efi =="
./scripts/build.sh --mode 1 --auto --debug --verbose
test -f dist/mode-1-auto-debug-verbose.efi \
  || { echo "FAIL: dist/mode-1-auto-debug-verbose.efi missing"; exit 1; }

echo "ok 010_build_smoke"
```

- [ ] **Step 5: Run runall — all green**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -15
```

Expected: `ALL TESTS PASS`. Both mode-0 and mode-1 artifacts produced; lints + patch9 multi-fixture test all pass.

- [ ] **Step 6: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add scripts/build.sh tests/010_build_smoke.sh \
  && git commit -m "build: scripts/build.sh accepts --mode 0; 010_build_smoke covers mode-0"
```

---

## Task 9: Add LogFs diagnostic verbosity

**Files:**
- Modify: `GblChainloadPkg/Library/LogFsLib/Mount.c`

The `connectcontroller (Not Found)` error tells us LogFsInit's call to `gBS->ConnectController()` returns EFI_NOT_FOUND, but we don't know which step (handle lookup, protocol open, controller bind) actually fails. Add per-step diagnostic prints so the next on-device boot reveals the exact failure point.

- [ ] **Step 1: Read `Mount.c` to find the LogFsInit / MountLogFsRoot logic**

```bash
sed -n '1,160p' /home/vivy/gbl-chainload/GblChainloadPkg/Library/LogFsLib/Mount.c
```

Identify:
- The function that calls `GetBlkIOHandles` (or equivalent) to find the partition handle.
- The function that calls `ConnectController`.
- The point where EFI_NOT_FOUND is first surfaced.

- [ ] **Step 2: Add diagnostic prints around each step**

Edit `Mount.c`. For every key step (handle search, BlockIO protocol open, ConnectController, FAT FS protocol open, root directory open), wrap with `Print(L"LogFs: <step> ...\n")` so the message appears on screen even when DEBUG=0. Example:

```c
EFI_STATUS
LogFsInit (VOID)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  UINTN HandleCount = 0;

  Print (L"LogFs: locating partition handles by label='logfs'\n");
  Status = GetBlkIOHandles (BLK_IO_SEL_MATCH_PARTITION_LABEL, ...);
  if (EFI_ERROR (Status) || HandleCount == 0) {
    Print (L"LogFs: GetBlkIOHandles failed (%r, count=%u)\n", Status, HandleCount);
    return EFI_NOT_FOUND;
  }
  Print (L"LogFs: found %u logfs handles\n", HandleCount);

  for (UINTN i = 0; i < HandleCount; ++i) {
    Print (L"LogFs: handle %u — calling ConnectController\n", i);
    Status = gBS->ConnectController (Handles[i], NULL, NULL, TRUE);
    Print (L"LogFs: handle %u — ConnectController returned %r\n", i, Status);
    if (EFI_ERROR (Status)) continue;

    /* ... open FS protocol, etc. — print at each step ... */
    Print (L"LogFs: handle %u — opening EFI_SIMPLE_FILE_SYSTEM_PROTOCOL\n", i);
    /* ... */
  }

  /* ... */
}
```

The exact pattern depends on Mount.c's actual structure. The principle: every EFI call that can fail logs both before and after, with `%r` for the EFI_STATUS.

- [ ] **Step 3: Verify the verbose version still compiles**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 0 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 4: Build the diagnostic mode-0 build**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 0 --debug --verbose
ls -lh dist/mode-0-debug-verbose.efi
```

Expected: artifact exists. This is the diagnostic build the user will stage.

- [ ] **Step 5: Run runall — should still pass**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -5
```

Expected: `ALL TESTS PASS`.

- [ ] **Step 6: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Library/LogFsLib/Mount.c \
  && git commit -m "LogFsLib: add per-step diagnostic verbosity for on-device root-cause investigation"
```

---

## Task 10: Push + first device-test handoff

**No files modified.** This is a synchronization step.

- [ ] **Step 1: Push all accumulated commits**

```bash
cd /home/vivy/gbl-chainload && git push origin main
```

- [ ] **Step 2: Stage commands for the user**

Provide the user with the exact device-test commands. The user will execute these:

```text
# Test #1 — mode-0 baseline (proves chain-load works without protocol hooks)
fastboot reboot recovery
fastboot stage dist/mode-0-auto.efi
fastboot oem boot-efi
# (waits for FastbootLib menu — confirms our payload booted)
fastboot oem escape
# (chainload triggers — boots stock ABL → custom recovery)

# After custom recovery boots, pull evidence:
adb pull /proc/bootloader_log /tmp/mode-0-bootloader_log
adb pull /proc/cmdline /tmp/mode-0-cmdline
# (or wherever the device's bootloader log lives)

# Test #2 — mode-0 with logfs diagnostics
fastboot reboot recovery
fastboot stage dist/mode-0-debug-verbose.efi
fastboot oem boot-efi
# Wait for FastbootLib (DEBUG=1 should show LogFs: ... lines on screen)
# Capture the logfs init lines verbatim from screen photo OR from bootloader_log
```

- [ ] **Step 3: Wait for device evidence**

The user runs the above and reports back with:
- Result of `dist/mode-0-auto.efi`: does it chainload through to ABL? (YES = foundation works; NO = patch1 missed or LoadImage failed → diagnose)
- Logfs diagnostic output: which step prints failure? (`GetBlkIOHandles failed`, `ConnectController returned %r`, etc.)

The next set of tasks (11-13) branch based on this evidence.

---

## Task 11: Logfs root-cause diagnosis + fix (evidence-driven)

**Files:**
- Modify: `GblChainloadPkg/Library/LogFsLib/Mount.c` (specific change determined by evidence)

This task's specific code change depends on the diagnostic output from Task 10's device test. The decision tree:

- [ ] **Step 1: Inspect Task 10's diagnostic evidence**

The user reports the LogFs print output from the device. Identify which step failed.

- [ ] **Step 2a — If `GetBlkIOHandles` returned 0 handles**

The partition isn't being found by label. Possible causes:
- The partition label has changed in the user's GPT (unlikely — same device).
- `GetBlkIOHandles` requires partition records to be installed; if `EnumeratePartitions` didn't run or didn't install them, the handle search fails.

Fix: call `EnumeratePartitions` directly inside `LogFsInit` if the initial search returns 0, OR add a re-scan loop with backoff. Prefer the direct call if `EnumeratePartitions` is idempotent (verify by reading its source).

```c
if (HandleCount == 0) {
  Print (L"LogFs: 0 handles — invoking EnumeratePartitions and retrying\n");
  EnumeratePartitions ();
  Status = GetBlkIOHandles (...);
}
```

- [ ] **Step 2b — If `ConnectController` returned EFI_NOT_FOUND**

A handle was found but no driver could bind to it. Probable cause: FAT driver isn't loaded into the platform yet (DXE phase). The fix: explicitly load the FAT driver (or any required filesystem driver) from our payload's FV before `ConnectController`.

```c
Print (L"LogFs: forcing-load FAT driver from current FV before ConnectController\n");
LoadFilesystemDriversFromCurrentFv ();   /* new helper, calls LoadFVLib equivalents */
Status = gBS->ConnectController (Handle, NULL, NULL, TRUE);
```

`LoadFilesystemDriversFromCurrentFv` is implemented as a thin wrapper over the existing `LoadDriversFromCurrentFv` but filtered to filesystem drivers only (or just calls `LoadDriversFromCurrentFv` again — idempotent).

- [ ] **Step 2c — If `ConnectController` succeeded but FS protocol open failed**

The driver bound but no FS protocol surfaced. Likely cause: the FAT driver bound but the partition isn't actually FAT-formatted (e.g., it's been overwritten with raw blocks).

Fix: surface as a hard error (the partition isn't logfs-usable on this device) and recommend the user re-flash the EFISP partition with a known-good FAT image.

This is the "deep architectural blocker" branch in the spec. If this is what we hit, surface BLOCKED + a re-spec recommendation.

- [ ] **Step 3: Apply the matched fix**

Pick 2a, 2b, or 2c based on diagnostic evidence. Apply the corresponding change. The fix must address the root cause, not paper over it.

- [ ] **Step 4: Verify host build**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 0
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Library/LogFsLib/Mount.c \
  && git commit -m "LogFsLib: <specific fix from diagnosis> — root-cause for v2 mount failure"
```

The commit message describes WHICH fix branch (2a, 2b, 2c) was taken.

---

## Task 12: Add LogFs proper transition (flush+close before LoadImage)

**Files:**
- Modify: `GblChainloadPkg/Application/GblChainload/BootFlow.c`

Per the spec §"proper transition": clean unmount before LoadImage so chained EFIs aren't blocked by stale partition handles.

- [ ] **Step 1: Edit BootFlow.c**

Locate the section just before `gBS->LoadImage` in `BootFlowChainLoad`. Add:

```c
/* Proper transition: release the logfs partition handle so the next EFI in
   the chain (the patched ABL or further-chained payloads) can mount it
   if they want.  Without this, the partition stays bound to our driver
   instance and ConnectController returns EFI_NOT_FOUND for the next caller. */
Print (L"BootFlow: LogFs flush+close before LoadImage\n");
LogFsFlush ();
LogFsClose ();
```

Place this before the `gBS->LoadImage` call.

- [ ] **Step 2: Verify host build**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 0
```

Expected: clean build.

- [ ] **Step 3: Run runall — pass**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -5
```

Expected: `ALL TESTS PASS`.

- [ ] **Step 4: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Application/GblChainload/BootFlow.c \
  && git commit -m "BootFlow: LogFs flush+close before LoadImage (proper transition for chained EFIs)"
```

---

## Task 13: Push + second device-test handoff (mode-0 with logfs verified)

**No files modified.**

- [ ] **Step 1: Push**

```bash
cd /home/vivy/gbl-chainload && git push origin main
```

- [ ] **Step 2: User stages & verifies**

User runs:

```text
fastboot reboot recovery
fastboot stage dist/mode-0-auto-debug-verbose.efi
fastboot oem boot-efi
# Watch for: "LogFs: <step>" lines should now succeed all the way through
# Confirm: "BootFlow: LogFs flush+close before LoadImage" prints
fastboot oem escape
# Chainload completes; device boots stock ABL → custom recovery

adb pull /proc/bootloader_log /tmp/mode-0-with-logfs-bootloader_log
```

The user reports back:
- Whether logfs successfully mounted (specifically, no `connectcontroller (Not Found)` and the diagnostic prints reach the "FS protocol opened, root directory ready" milestone).
- Whether the device successfully boots into custom recovery via mode-0.

If both pass: foundation is solid; proceed to mode-1 testing.
If logfs still fails: revisit Task 11's diagnosis with the new evidence.

---

## Task 14: Build mode-1 with patch9 v2 + push

**No files modified.** Build + verify artifacts.

- [ ] **Step 1: Run a clean mode-1 build**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 --auto --debug --verbose
ls -lh dist/mode-1-auto-debug-verbose.efi dist/mode-1.efi
```

Expected: both `.efi` artifacts present.

- [ ] **Step 2: Run full runall — pass**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -10
```

Expected: `ALL TESTS PASS`.

- [ ] **Step 3: Push (no new commits expected, but ensure remote is current)**

```bash
cd /home/vivy/gbl-chainload && git push origin main
```

---

## Task 15: Third device-test — mode-1 with patch9 v2 (final validation)

**No files modified.** User-active validation.

- [ ] **Step 1: User stages mode-1**

```text
fastboot reboot recovery
fastboot stage dist/mode-1-auto-debug-verbose.efi
fastboot oem boot-efi
# Wait for FastbootLib menu — confirms staged payload is running
fastboot oem escape
```

- [ ] **Step 2: Capture device evidence**

Expected lines in `bootloader_log` (per spec §6 success criteria):

```
gbl-chainload | mode=1 auto=1 debug=1 verbose=1
LogFs: <success path through all diagnostic steps>
BootFlow: start (mode=1)
DynamicPatch: patch1 [universal, mandatory] -> OK
DynamicPatch: patch9-avb-locked-recoverable-continue [mode-1, mandatory] -> OK
ProtocolHookLib: installed (mode=1, vb=1/1 scm=1/1 qsee=1/1 spss=...)
vb-fakelock | READ_CONFIG | is_unlocked 1->0 | is_unlock_critical 1->0
BootFlow: LogFs flush+close before LoadImage
BootFlow: handing off to patched ABL
... (handoff to ABL — its own logging continues)
qsee-km | cmd=0x208(SET_BOOT_STATE) | isUnlocked=0 | color=0 | pubKey=<real OEM hash>
[AddOplusCmdLineFromVBCmdLineLen]: Adding oplusboot.verifiedbootstate=green
... (libavb completes; non-OK Result with populated SlotData)
... (ABL's recoverable-continue path — patch9 v2's gate rewrite kicks in)
... (LoadImage(recovery) on the custom recovery image)
... (custom recovery kernel boots)
```

- [ ] **Step 3: Pull adb props + bootloader_log**

```text
adb pull /proc/bootloader_log /tmp/mode-1-bootloader_log
adb shell getprop ro.boot.verifiedbootstate
adb shell getprop ro.boot.vbmeta.device_state
adb shell getprop ro.boot.flash.locked
```

Expected:
- `ro.boot.verifiedbootstate` = `green`
- `ro.boot.vbmeta.device_state` = `locked`
- `ro.boot.flash.locked` = `1`

- [ ] **Step 4: User reports back**

Either:
- (a) All checks pass — patch9 v2 succeeded; KM 0x208 shows isUnlocked=0; custom recovery boots; cmdline shows green/locked.
- (b) Patch9 still misses — re-check anchors against the user's actual device build (which may not be in our fixture set yet); add the device's PE to fixtures, re-derive anchors, re-test.
- (c) Patch9 hits but ABL still fatals — Approach A's gate rewrite may not have been the right site; consider Approach B (libavb return remap).
- (d) ABL handoff but custom recovery doesn't boot — different failure mode (e.g., kernel command line wrong, dtbo issue); investigate separately.

Each failure mode triggers a follow-up task; (a) means we move to Task 16.

---

## Task 16: Final tag + closure

**No files modified.**

- [ ] **Step 1: Tag the milestone**

```bash
cd /home/vivy/gbl-chainload \
  && git tag -a v2.0.0-plan2-mode0-logfs-patch9v2 -m "Plan 2 complete: mode-0 + logfs always-works + patch9 v2

End-state:
- dist/mode-0.efi boots through chainload to stock ABL with logfs mounted.
- dist/mode-1.efi (with patch9 v2) boots into custom recovery on the user's device.
- KM 0x208 SET_BOOT_STATE emits isUnlocked=0, color=0, real OEM pubkey.
- oplusboot.verifiedbootstate=green; ro.boot.flash.locked=1.
- Multi-binary anchor verification: ≥3 of {old infiniti, EU 16.0.5.703, IN 16.0.7.201, fairlady CN 16.0.7.200, myron} hit PATCH_OK on patch9 v2.
- Patch7 archived (one-line revert with -DGBL_PATCH7_ENABLED).
- LogFs proper transition (flush+close before LoadImage) for chained-EFI handoff.

Plan 1 deliverables (preserved):
- Patch engine v2 (scan-based, dual host/runtime).
- Universal baseline + Mode1Overlay protocol hooks.
- Per-patch outcome logging.
- 045/046/047 lints, 042 patch harness, 010 build smoke.
"
```

- [ ] **Step 2: Push the tag**

```bash
cd /home/vivy/gbl-chainload && git push origin v2.0.0-plan2-mode0-logfs-patch9v2
```

- [ ] **Step 3: Final state checklist (verifier)**

- [ ] `dist/mode-0.efi` builds; chainloads through to ABL on device with patch1 OK and logfs mounted.
- [ ] `dist/mode-0-auto-debug-verbose.efi` boots silently to ABL with full diagnostic output.
- [ ] LogFs proper transition releases the partition handle before LoadImage.
- [ ] `dist/mode-1-auto-debug-verbose.efi` boots through to custom recovery with KM 0x208 isUnlocked=0, color=0, real OEM pubkey.
- [ ] patch9 v2 anchors match unique-and-correctly on at least 3 of 5 PE fixtures.
- [ ] No `AllowVerificationError` color-site rewrite (line 1728 untouched).
- [ ] All host tests pass; runall green.
- [ ] Tag `v2.0.0-plan2-mode0-logfs-patch9v2` pushed.

---

## Plan 2 done. Next steps.

After Plan 2 lands and device-validates:

- **Plan 3 (Mode-2):** typed-struct profile system, Qseecom/SPSS overlay, vtable fingerprint extractor, `extract-mode2-profile.py`, mode-2 build flag wiring, device validation against custom recovery boot with KM HAL up.
- **Plan 4 (Mode-3 + Embed + RE docs):** empty mode-3 overlay (universal-only), `--embed <abl.bin>` flag, runtime cache-key check, `tests/050_embed_determinism.sh`, `docs/re/oplusreserve1-write-paths.md`, `docs/re/gbl-load-mechanism.md`, `docs/re/scm-fuse-classification.md`.

Each follow-up plan is self-contained and produces working, testable software.
