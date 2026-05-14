# QCOM UART Log Buffer — On-Device Probe Plan

**Goal:** Answer questions Q1, Q4, Q5, Q6 from `qcom-uart-log-buffer-research.md` §"Open questions" with a minimum number of boot cycles. Each question is load-bearing for the option-(a) implementation; we want answers before writing the production design.

**Companion docs:** `qcom-uart-log-buffer-investigation.md` (brief), `qcom-uart-log-buffer-research.md` (research output).

**Constraints:** `fastboot stage dist/<artifact>.efi` + `fastboot oem boot-efi` only (CLAUDE.md). All probes are throwaway, get removed after their question is answered.

## What we're answering

| ID | Question | Probe |
|----|----------|-------|
| Q1 | `gEfiInfoBlkHobGuid` literal + `UefiInfoBlkType` prefix layout reachable from stage-2 | Probe A (cheap, read-only) |
| Q5 | When does `WriteLogBufToPartition` actually fire on canoe? | Probe A (writes a magic into the buffer; we observe which `UefiLog<N>.txt` rotation slot it lands in) |
| Q4 | Does stock ABL / patched ABL's `SerialPortShLib` constructor cache the buffer pointer at image-load? | Probe B (writes via `SerialPortWrite` before AND after a `SerialBufferReInit` call from patched ABL) |
| Q6 | Is `SerialBufferReInit` callable from a patch injected into patched ABL? | Probe B (the call attempt itself) |

Q1 and Q5 are answered by one app. Q4 and Q6 are entangled — the only way to definitively test caching is to issue a reinit, which requires the symbol — and only viable via a patched-ABL injection. So 2 probes total.

The two probes are independent: Probe A can ship and run before Probe B is built.

## Probe A — HOB + struct + direct buffer write

**Files** (all throwaway, deleted after answers captured):

- Create: `GblChainloadPkg/Application/UartProbeA/UartProbeA.c`
- Create: `GblChainloadPkg/Application/UartProbeA/UartProbeA.inf`
- Modify: `GblChainloadPkg/GblChainloadPkg.dsc` (add component)

### Behavior

1. Walk `EFI_SYSTEM_TABLE->ConfigurationTable` looking for `gEfiHobListGuid` — that gives us the HOB list base.
2. Walk the HOB list. For every `EFI_HOB_TYPE_GUID_EXTENSION` HOB, dereference the first 4 bytes of its body and check whether they match the ASCII signature `'IBlk'` (`0x6B6C4249` little-endian, big-endian is `0x49426C6B`) — the `UefiInfoBlkType.Signature` value per the BSP convention.

   - If the convention's actual `Signature` differs on canoe, the test fails and we widen to scan all 4 endian/case combinations. Print every candidate so the user can identify the right one from the screen.
3. On match: cast the HOB body to a `UefiInfoBlkPrefix` (our own minimal struct — 4× `UINT32` + `UINT64 UartLogBufferPtr` + `UINT32 UartLogBufferLen`):

   ```c
   typedef struct {
     UINT32 Signature;
     UINT32 StructVersion;
     UINT32 _Reserved1;     // PMIC info / other early fields - opaque
     UINT32 _Reserved2;
     UINT64 UartLogBufferPtr;
     UINT32 UartLogBufferLen;
   } UefiInfoBlkPrefix;
   ```

   Print: HOB GUID (32 hex chars), `Signature` (`%08x`), `StructVersion` (`%08x`), `UartLogBufferPtr` (`%016lx`), `UartLogBufferLen` (`%08x`), and a hex dump of the first 64 bytes the pointer points to (verify it looks like ASCII log text).

   **Note on offsets:** the struct prefix above is a guess. If `Signature/StructVersion` print as nonsense, the layout differs — print 256 bytes of the HOB body raw hex so the user can find the right offsets manually.
4. **Read-mode answer to Q1:** at this point we have enough info to write the production code. If desired, stop here. If proceeding to Q5, continue:
5. **Write-mode for Q5:** scan the buffer from `UartLogBufferPtr` for the first `0x00` byte (treating the buffer as ASCII text with NUL padding). At that offset, write the magic string `PROBE_A_DIRECT_<timestamp>\n` where `<timestamp>` is the current time-counter from a 64-bit register (use `AsmReadTsc()` or any rough monotonic).
6. Print magic position and length on-screen too.
7. Return `EFI_SUCCESS` so device falls back to fastboot normally.

### Source skeleton

```c
/** @file UartProbeA.c — read UefiInfoBlk HOB on canoe + plant a magic.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/HobLib.h>
#include <Library/DebugLib.h>

#pragma pack(1)
typedef struct {
  UINT32 Signature;
  UINT32 StructVersion;
  UINT32 _Reserved1;
  UINT32 _Reserved2;
  UINT64 UartLogBufferPtr;
  UINT32 UartLogBufferLen;
} UefiInfoBlkPrefix;
#pragma pack()

#define MAGIC_FMT  "PROBE_A_DIRECT_%016lx\n"

EFI_STATUS EFIAPI
UartProbeAEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_PEI_HOB_POINTERS  Hob;
  UINT64                Tsc;
  CHAR8                 MagicBuf[64];
  UINTN                 MagicLen;

  Print (L"UartProbeA: start\n");

  /* Walk HOB list — GetHobList() returns the chain root via gEfiHobListGuid
     ConfigurationTable lookup. */
  Hob.Raw = GetHobList ();
  if (Hob.Raw == NULL) {
    Print (L"UartProbeA: GetHobList returned NULL — no HOBs visible\n");
    return EFI_NOT_FOUND;
  }

  while (!END_OF_HOB_LIST (Hob)) {
    if (Hob.Header->HobType == EFI_HOB_TYPE_GUID_EXTENSION) {
      EFI_GUID  *Guid = &Hob.Guid->Name;
      UINTN      Size = Hob.Header->HobLength - sizeof (EFI_HOB_GUID_TYPE);
      UINT8     *Body = (UINT8 *)Hob.Guid + sizeof (EFI_HOB_GUID_TYPE);

      if (Size >= sizeof (UefiInfoBlkPrefix)) {
        UefiInfoBlkPrefix *Blk = (UefiInfoBlkPrefix *)Body;
        /* Try the BSP-convention 'IBlk' signature. */
        if (Blk->Signature == 0x6B6C4249) {  // 'IBlk' little-endian
          Print (L"\n=== UefiInfoBlk HOB FOUND ===\n");
          Print (L"HOB GUID:           %g\n", Guid);
          Print (L"Signature:          0x%08x\n", Blk->Signature);
          Print (L"StructVersion:      0x%08x\n", Blk->StructVersion);
          Print (L"UartLogBufferPtr:   0x%016lx\n", Blk->UartLogBufferPtr);
          Print (L"UartLogBufferLen:   0x%08x  (= %u bytes)\n",
                 Blk->UartLogBufferLen, Blk->UartLogBufferLen);

          if (Blk->UartLogBufferPtr != 0 && Blk->UartLogBufferLen >= 64) {
            UINT8 *Buf = (UINT8 *)(UINTN)Blk->UartLogBufferPtr;
            Print (L"\nFirst 64 bytes of buffer:\n");
            for (UINTN i = 0; i < 64; i++) {
              Print (L"%02x ", Buf[i]);
              if ((i & 15) == 15) Print (L"\n");
            }

            /* Plant magic at first NUL. */
            Tsc = AsmReadTsc ();
            MagicLen = AsciiSPrint (MagicBuf, sizeof (MagicBuf), MAGIC_FMT, Tsc);

            UINTN End;
            for (End = 0; End < Blk->UartLogBufferLen; End++) {
              if (Buf[End] == 0) break;
            }
            Print (L"\nFirst NUL at offset 0x%x (%u bytes of content)\n",
                   (UINT32)End, (UINT32)End);

            if (End + MagicLen < Blk->UartLogBufferLen) {
              CopyMem (Buf + End, MagicBuf, MagicLen);
              Print (L"Wrote magic at offset 0x%x: %a", (UINT32)End, MagicBuf);
            } else {
              Print (L"BUFFER FULL — cannot append (wrap-around territory)\n");
            }
          }
          /* Done — there's only one block. */
          goto done;
        }
      }
    }
    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  Print (L"UartProbeA: no HOB with signature 0x6B6C4249 (=IBlk) found\n");
  Print (L"All GUID HOBs (for manual identification):\n");

  /* Second pass — enumerate every GUID HOB so user can identify the right one. */
  Hob.Raw = GetHobList ();
  while (!END_OF_HOB_LIST (Hob)) {
    if (Hob.Header->HobType == EFI_HOB_TYPE_GUID_EXTENSION) {
      EFI_GUID  *Guid = &Hob.Guid->Name;
      UINTN      Size = Hob.Header->HobLength - sizeof (EFI_HOB_GUID_TYPE);
      UINT8     *Body = (UINT8 *)Hob.Guid + sizeof (EFI_HOB_GUID_TYPE);

      Print (L"  GUID=%g  size=0x%x  first16=", Guid, (UINT32)Size);
      for (UINTN i = 0; i < (Size < 16 ? Size : 16); i++) {
        Print (L"%02x", Body[i]);
      }
      Print (L"\n");
    }
    Hob.Raw = GET_NEXT_HOB (Hob);
  }

done:
  Print (L"\nUartProbeA: done — reboot to recovery, pull logs, inspect.\n");
  return EFI_SUCCESS;
}
```

INF:
```
[Defines]
  INF_VERSION    = 0x00010005
  BASE_NAME      = UartProbeA
  FILE_GUID      = a3b4c5d6-1111-2222-3333-444455556666
  MODULE_TYPE    = UEFI_APPLICATION
  VERSION_STRING = 1.0
  ENTRY_POINT    = UartProbeAEntry

[Sources]
  UartProbeA.c

[Packages]
  MdePkg/MdePkg.dec

[LibraryClasses]
  UefiApplicationEntryPoint
  UefiBootServicesTableLib
  UefiLib
  BaseLib
  HobLib
  PrintLib
  DebugLib
  BaseMemoryLib
```

### Test procedure

```
fastboot stage dist/UartProbeA.efi
fastboot oem boot-efi
# read the on-screen output; record:
#   - Whether "UefiInfoBlk HOB FOUND" appeared, or "no HOB with signature"
#   - HOB GUID printed
#   - Signature, StructVersion, UartLogBufferPtr, UartLogBufferLen
#   - First 64 bytes of buffer
#   - Magic insertion offset
# reboot to recovery, pull logs as usual
```

### Inspecting results

```bash
LATEST=$(ls -td logs/*/ | head -1)

# Did our magic land in any UefiLog slot?
grep -ln PROBE_A_DIRECT "$LATEST/logfs/UefiLog"*.txt
grep -ln PROBE_A_DIRECT "$LATEST/logfs/UefiLogSaved"*.txt

# If yes, which slot and approximate offset:
for f in "$LATEST/logfs/UefiLog"*.txt "$LATEST/logfs/UefiLogSaved"*.txt; do
  [ -f "$f" ] || continue
  pos=$(grep -bo PROBE_A_DIRECT "$f" 2>/dev/null | head -1 | cut -d: -f1)
  [ -n "$pos" ] && echo "$f: at byte $pos"
done
```

### What Q1 and Q5 conclusions look like

**Q1 answers (from screen output):**
- HOB GUID literal → record verbatim for use in production code as `gEfiInfoBlkHobGuid`.
- Signature value confirms BSP convention or reveals canoe-specific value.
- StructVersion value bounds our minimum-supported version.
- UartLogBufferLen confirms 32 KiB default (or larger if canoe was pre-tuned).

**Q5 answers (from the grep):**
- Magic in `UefiLog<current-cycle>.txt`: BDS flushed during the boot that ran our probe (good — means we'd capture content from the same run).
- Magic in `UefiLogSaved<N>.txt`: BDS flushed at start of next boot (the rotation moved current → saved). Content captured but one cycle delayed.
- Magic in neither: either the SerialPortLib's cursor was past our write before flush (write was overwritten by stock writes happening between our probe exit and BDS flush), or the flush already happened before the buffer state stabilized. Repeat with the magic plant happening from a hook fired closer to BDS.

## Probe B — patched ABL injection (`SerialBufferReInit` callability + caching test)

**This probe requires building a patched ABL with two extra changes.** It is more involved than Probe A. Do Probe A first; if Q1 is uncertain or the buffer pointer can't be reached, Probe B won't even compile correctly.

**Files:**
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/Patches.c` (add `patch_probe_b`)
- Create: `GblChainloadPkg/Application/UartProbeB/UartProbeB.c` — stage-2 driver that patches UefiInfoBlk and chain-loads the patched ABL
- Create: `GblChainloadPkg/Application/UartProbeB/UartProbeB.inf`

### Architectural decision: where to find `SerialBufferReInit`?

We need the address of `SerialBufferReInit` in the loaded patched-ABL PE. Options:

1. **Have the user supply the offset** from their local `SerialPortShLib.lib` symbol table (e.g., `objdump -t SerialPortShLib.lib | grep SerialBufferReInit`). Hard-code as a `#define SERIAL_BUFFER_REINIT_OFFSET_FROM_BASE 0x...` in the patch.
2. **Pattern-scan** the loaded ABL PE for a byte signature unique to `SerialBufferReInit` (e.g., a known prologue + a literal reference to a string constant). Same approach as our existing patch1/patch6/patch10 already use via `DynamicPatchLib`.

Option 1 is faster to write but BSP-version-specific. Option 2 matches the project's existing patch infrastructure. **Recommendation: Option 1 for the probe, Option 2 for production.**

The user needs to provide: the byte offset of `SerialBufferReInit` from the start of the patched ABL PE (or from a known anchor symbol). This is the **prerequisite for Probe B to be implementable**.

### Probe B behavior

**Stage-2 (UartProbeB.efi):**

1. Walk HOB, find `UefiInfoBlk` (use literal GUID from Probe A's answer).
2. Save old `UartLogBufferPtr` (`OldPtr`), `UartLogBufferLen` (`OldLen`).
3. Allocate 1 MiB: `gBS->AllocatePages(EfiBootServicesData, 256, &NewPtr)`.
4. `CopyMem(NewPtr, OldPtr, OldLen)`; `ZeroMem(NewPtr+OldLen, 0x100000 - OldLen)`.
5. Patch `UefiInfoBlk->UartLogBufferPtr = NewPtr`; `UefiInfoBlk->UartLogBufferLen = 0x100000`.
6. Plant magic `PROBE_B_GBL_DIRECT_<tsc>` at the (new) buffer at offset `OldLen` (just past the copied content).
7. Load and start patched ABL (with our DynamicPatch + a new `patch_probe_b`).

**`patch_probe_b` in patched ABL:**

1. At ABL entry-point (or first DEBUG-print site), call the function pointer at `imagebase + SERIAL_BUFFER_REINIT_OFFSET` with `(NewPtr, 0x100000)`. We don't have `NewPtr` directly — re-read it from `UefiInfoBlk` HOB (which our stage-2 patched). So the ABL patch *also* walks HOB.
2. **Before the reinit call**, emit `SerialPortWrite("PROBE_B_ABL_PRE_REINIT\n", ...)`. (We have `SerialPortWrite` via ABL's static-linked SerialPortShLib.)
3. **After the reinit call**, emit `SerialPortWrite("PROBE_B_ABL_POST_REINIT\n", ...)`.
4. Continue normal ABL flow.

### Test procedure

```
fastboot stage dist/UartProbeB.efi
fastboot oem boot-efi
# ABL runs, recovery or fastboot lands, pull logs as usual
```

### Inspecting results

```bash
LATEST=$(ls -td logs/*/ | head -1)

# Where do the three magics land? (Across all UefiLog rotations)
for f in "$LATEST/logfs/UefiLog"*.txt "$LATEST/logfs/UefiLogSaved"*.txt; do
  [ -f "$f" ] || continue
  for magic in PROBE_B_GBL_DIRECT PROBE_B_ABL_PRE_REINIT PROBE_B_ABL_POST_REINIT; do
    if grep -q $magic "$f"; then
      pos=$(grep -bo $magic "$f" | head -1 | cut -d: -f1)
      echo "$f: $magic at byte $pos"
    fi
  done
done
```

### Q4 and Q6 conclusions

**Q4 (caching) — interpret the magic positions:**

The buffer we flushed has up to 1 MiB of content. The UefiLog<N>.txt file QCOM produces is whatever was in `UartLogBufferPtr[0..UartLogBufferLen]` at flush time. After our patch, that range is the 1 MiB new buffer.

| Where each magic lands | Diagnosis |
|------------------------|-----------|
| All 3 magics in same `UefiLog<N>.txt` (the patched-1MiB output) | Best case: reinit worked, ABL writes after reinit went to 1 MiB, our PRE_REINIT also went to 1 MiB because the reinit happened before SerialPortShLib's first emit ran. (Or PRE_REINIT was emitted via a serialwrite that did NOT cache — implies no caching, Q4 = NO.) |
| `PROBE_B_GBL_DIRECT` and `POST_REINIT` in 1MiB output; `PRE_REINIT` only in old 32 KiB ring (which is now orphaned but its content was already copied forward, so PRE_REINIT also appears in copied-prefix of 1MiB) | Caching confirmed (Q4 = YES). Reinit retargeted post-reinit emits successfully (Q6 = YES). This is the expected option-(a) signature. |
| `POST_REINIT` absent everywhere | Reinit didn't redirect; Q6 = NO, design needs different approach. |
| All 3 magics absent | Patch didn't apply or buffer wrap; debug separately. |

**Q6 (callability):**

- If `POST_REINIT` appears anywhere (including in the new 1 MiB ring): `SerialBufferReInit` is callable from the injected patch and produced an observable effect → Q6 = YES.
- If only `PRE_REINIT` appears and `POST_REINIT` is absent: reinit either crashed mid-call (we'd see screen output) or silently failed → Q6 = AMBIGUOUS, need more diagnostic.

## Sequencing recommendation

1. **Build and run Probe A first.** It's the cheapest (no ABL patches, no symbol resolution) and answers Q1 (which we need before Probe B can even compile correctly) + Q5 (independent of the rest).
2. After Probe A passes, **decide whether to proceed to Probe B.** Probe B is only needed if we want to validate the production design before committing to it. If you'd rather skip and just build the production design assuming the conventional answers (Q4 = YES, Q6 = YES — both are BSP-convention defaults), Probe B can be skipped — we'd just verify in production.
3. **Probe B requires the user to supply the symbol offset** for `SerialBufferReInit` from their local SerialPortShLib.lib. Without that, Probe B can't be implemented.

## Clean-up after probes

Both probes are throwaway. After each one's questions are answered:

- Delete `GblChainloadPkg/Application/UartProbeA/` (or `UartProbeB/`).
- Remove the DSC `[Components.common]` entry.
- Remove `patch_probe_b` from `Patches.c` (if used).
- Commit: `verify(uart-buffer): probe A/B results — Q1=…, Q4=…, Q5=…, Q6=…`. Include the literal HOB GUID and any byte-level facts captured.

## Risks

1. **Probe A: magic gets overwritten before BDS flush.** If stock-ABL or BDS writes to the same buffer between our magic plant and the BDS flush, our content can be overwritten. Mitigation: place magic far enough into the buffer that no normal write reaches it, OR rely on Probe B (which writes via SerialPortWrite, properly tracked by SioPortLib).

2. **Probe A: HOB enumeration trips on an unknown HOB type.** If our walk encounters something weird (length=0, type=unknown), `GET_NEXT_HOB` may not converge. Add an iteration guard (max 256 HOBs) to prevent infinite loops.

3. **Probe B: `SerialBufferReInit` at a different offset than supplied.** Symbol position can shift across BSP minor versions. If Probe B's PRE_REINIT lands but POST_REINIT doesn't and ABL never crashes visibly, the call was to a wrong address. Mitigation: pattern-scan rather than hard-code offset (see option 2 above).

4. **Probe B: ABL hangs / crashes after PRE_REINIT.** We're injecting a call into ABL's early path. If the call sequence violates ABI (wrong calling convention, missing register save) ABL fault-faults. Recovery: power-cycle, no permanent state changes (we used `EfiBootServicesData`, freed at next ExitBootServices). Risk score: low for AArch64 (AAPCS64 has small caller-save set) but real.

## Out of scope

- Touching `WriteLogBufToPartition` itself.
- Replacing canoe's BDS with our own.
- Anything that requires `fastboot flash` of non-HLOS partitions.
- Building a permanent fastboot OEM command for buffer inspection (separate work).
