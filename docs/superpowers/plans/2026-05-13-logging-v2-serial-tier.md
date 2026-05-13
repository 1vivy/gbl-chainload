# Logging v2 Serial-Tier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire verbose-tier hook payloads through `gEfiSerialIoProtocol` so they land in `UefiLog<N>.txt` without touching the framebuffer console and without holding logfs open past `LoadImage`.

**Architecture:** Extend `GblDebugLib` with a `gEfiSerialIoProtocol`-backed tee so DEBUG emits land in both ConOut (screen, mask-gated) and UART → UefiLog. Add a `SERIAL_DBG` macro that calls the same SerialIo path directly, bypassing ConOut entirely — used for verbose-tier per-hook payload hex. The macro is compile-gated on `GBL_VERBOSE=1` so non-verbose builds emit zero verbose code.

**Tech Stack:** EDK2 UEFI (AArch64), `gEfiSerialIoProtocol` (provided by canoe BSP), our `edk2/QcomModulePkg` for the SerialIo pattern, our `GblChainloadPkg` for the chain-load app + libraries.

**Branch strategy:** Implementation lands on a new branch `feature/logging-v2-impl` cut from `main` AFTER PR #17 (`feature/cleanup-p1c-log-stream-split`) merges. The spec already lives on `feature/logging-v2-spec`; this plan commits to the same branch.

**Deviation from spec note:** Spec §2/§4.2/§7 propose runtime resize of the UART buffer via `SerialBufferReInit`, premised on access to QCOM's `SerialPortShLib`. Investigation showed that library is not present in our edk2 fork and the `SerialIo` protocol on canoe already routes writes to the UART buffer, so we de-scope the resize. Existing stock buffer size (32 KiB default) is accepted as the starting point; on-device verification will tell us if truncation is a real problem. If it is, a follow-on plan covers the resize.

---

## File Structure

**New files:**

| Path | Responsibility |
|------|----------------|
| `GblChainloadPkg/Include/Library/GblSerialDbg.h` | Public header declaring `SERIAL_DBG` macro + `GblSerialDbgPrint` function. Compile-gates on `GBL_VERBOSE`. |
| `GblChainloadPkg/Application/SerialIoProbe/SerialIoProbe.c` | Throwaway on-device probe to verify `SerialIo` reaches UefiLog. Deleted after Task 0. |
| `GblChainloadPkg/Application/SerialIoProbe/SerialIoProbe.inf` | INF for the probe app. Deleted after Task 0. |

**Modified files:**

| Path | Change |
|------|--------|
| `GblChainloadPkg/Library/GblDebugLib/GblDebugLib.c` | Add `GblWriteSerial` helper + `GblSerialDbgPrint` impl. Tee `DebugPrint` to call `GblWriteSerial` after the existing ConOut path. |
| `GblChainloadPkg/Library/GblDebugLib/GblDebugLib.inf` | Add `gEfiSerialIoProtocolGuid` to `[Protocols]`. |
| `GblChainloadPkg/GblChainloadPkg.dsc` | Revert `PcdDebugPrintErrorLevel` to unconditional `0x80000042` (drop the `GBL_VERBOSE`-conditional widen). Add `GBL_VERBOSE` BuildOption pass-through. |
| `GblChainloadPkg/GblChainloadPkg.dec` | (No change — `gEfiSerialIoProtocolGuid` is in MdePkg.dec already.) |
| `GblChainloadPkg/Include/Library/LogFsLib.h` | Delete the `GBL_DBG_LOGFS_ONLY` define. |
| `GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.c` | Re-tag traces from `GBL_DBG_LOGFS_ONLY` back to `DEBUG_INFO` (one-line summaries) or `SERIAL_DBG` (high-volume probe noise). |
| `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c` | Per-hook ID counter; summary at `DEBUG_INFO id=N`; payload via `SERIAL_DBG id=N`. |
| `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c` | Same pattern. |
| `GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c` | Same pattern. |
| `GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c` | Same pattern. |
| `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c` | Same pattern (if any verbose-tier traces present). |
| `scripts/build.sh` | Already has `--verbose` flag; verify it passes `GBL_VERBOSE` env var to docker. |
| `scripts/build-inside-docker.sh` | Already propagates `GBL_VERBOSE` to the build. Verify path. |
| `tests/052_log_stream_split.sh` | Drop checks 7/8/9; add new checks for `SERIAL_DBG`, the SerialIo tee, and per-hook ID counters. |
| `tests/010_build_smoke.sh` | Add symbol-presence check: `GblSerialDbgPrint` present in `mode-1-auto-debug-verbose.efi`, absent in `mode-1-auto-debug.efi`. |

---

## Task 0: BLOCKING GATE — verify SerialIo→UefiLog on canoe

**Why first:** The whole design rests on `gEfiSerialIoProtocol` writes landing in `UefiLog<N>.txt` on canoe. Verify this with a 30-line probe before building anything else. If it fails, stop and revisit design.

**Files:**
- Create: `GblChainloadPkg/Application/SerialIoProbe/SerialIoProbe.c`
- Create: `GblChainloadPkg/Application/SerialIoProbe/SerialIoProbe.inf`
- Modify: `GblChainloadPkg/GblChainloadPkg.dsc` (add component, remove after Task 0)

- [ ] **Step 1: Create `SerialIoProbe.c`**

```c
/** @file SerialIoProbe.c — verify SerialIo→UefiLog round-trip on canoe.

  Stages a unique magic string through gEfiSerialIoProtocol then exits.
  If canoe's UEFI environment installs SerialIo against a UART-log-buffer-
  backed driver, the string will appear in \UefiLog<N>.txt on the logfs
  partition at the next BDS flush.
**/

#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SerialIo.h>

#define PROBE_MAGIC  "GBL_SERIALIO_PROBE_MAGIC_2026_05_13_4F8A2B7C\n"

EFI_STATUS EFIAPI
SerialIoProbeMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS                Status;
  EFI_SERIAL_IO_PROTOCOL   *SerialIo = NULL;
  UINTN                     Len;

  Status = gBS->LocateProtocol (&gEfiSerialIoProtocolGuid, NULL, (VOID **)&SerialIo);
  if (EFI_ERROR (Status) || SerialIo == NULL) {
    Print (L"SerialIoProbe: LocateProtocol failed: %r\n", Status);
    return Status;
  }

  Len = AsciiStrLen (PROBE_MAGIC);
  Status = SerialIo->Write (SerialIo, &Len, PROBE_MAGIC);
  Print (L"SerialIoProbe: SerialIo->Write returned %r (wrote %u bytes)\n", Status, (UINT32)Len);
  return Status;
}
```

- [ ] **Step 2: Create `SerialIoProbe.inf`**

```
[Defines]
  INF_VERSION    = 0x00010005
  BASE_NAME      = SerialIoProbe
  FILE_GUID      = 7e8f3a1d-2c4b-4e5f-8a6b-9c7d8e9f0a1b
  MODULE_TYPE    = UEFI_APPLICATION
  VERSION_STRING = 1.0
  ENTRY_POINT    = SerialIoProbeMain

[Sources]
  SerialIoProbe.c

[Packages]
  MdePkg/MdePkg.dec

[LibraryClasses]
  UefiApplicationEntryPoint
  UefiBootServicesTableLib
  UefiLib
  BaseLib

[Protocols]
  gEfiSerialIoProtocolGuid
```

- [ ] **Step 3: Add to DSC `[Components.common]`**

Edit `GblChainloadPkg/GblChainloadPkg.dsc`, add inside `[Components.common]` next to `GblChainload.inf`:

```
  GblChainloadPkg/Application/SerialIoProbe/SerialIoProbe.inf
```

- [ ] **Step 4: Build the probe**

```bash
bash scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -5
```

Expected: build succeeds, `dist/mode-1-auto-debug.efi` exists. We won't actually stage the probe — we'll stage its dist artifact directly. But first locate it:

```bash
find /home/vivy/gbl-chainload/edk2/Build -name 'SerialIoProbe.efi' 2>/dev/null | head -1
```

Expected: a path like `/home/vivy/gbl-chainload/edk2/Build/.../SerialIoProbe.efi`. Copy it next to `dist/`:

```bash
cp $(find /home/vivy/gbl-chainload/edk2/Build -name 'SerialIoProbe.efi' | head -1) dist/SerialIoProbe.efi
```

- [ ] **Step 5: On-device stage + run**

Hand the following commands to the user (do NOT run via Bash — the project's CLAUDE.md rule bans autonomous fastboot for any non-HLOS image):

```
fastboot stage dist/SerialIoProbe.efi
fastboot oem boot-efi
# wait for the probe to print and the device to land in fastboot or reboot
# reboot to recovery (or whatever path leaves logfs accessible)
fastboot getvar product   # sanity that device is responsive
adb pull /dev/block/by-name/logfs ./logfs_dump.img    # or however the user pulls logs
```

The user already has a pull script (`logs/` dir layout suggests a wrapper). Use whatever the user normally runs.

- [ ] **Step 6: Verify magic in UefiLog**

```bash
grep -l GBL_SERIALIO_PROBE_MAGIC_2026_05_13_4F8A2B7C logs/*/logfs/UefiLog*.txt 2>&1 | head -3
```

Expected on PASS: at least one file matched.
Expected on FAIL: no matches.

- [ ] **Step 7a (PASS path): Clean up, commit gate-passed**

```bash
rm dist/SerialIoProbe.efi
git rm GblChainloadPkg/Application/SerialIoProbe/SerialIoProbe.c GblChainloadPkg/Application/SerialIoProbe/SerialIoProbe.inf
# Remove the DSC component line
# Verify build still passes
bash scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -3
```

Commit message:

```
verify(serial-io): SerialIo→UefiLog confirmed on canoe

Probe app emitted a unique magic string via gEfiSerialIoProtocol;
string was recovered from logfs/UefiLog<N>.txt after the next boot.
Logging v2 design (SerialIo tee + SERIAL_DBG macro) is unblocked.

Probe removed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

- [ ] **Step 7b (FAIL path): STOP — revisit design**

Do NOT delete the probe. Open an issue documenting:
- The exact `gBS->LocateProtocol` status returned
- The exact `SerialIo->Write` status + bytes-written
- Whether `UefiLog<N>.txt` contained any new content at all (not just the magic)

Then re-engage the user in the brainstorming flow. Likely pivot: RAM ring buffer drained at EBS-callback to a fresh logfs file. The rest of this plan does NOT apply if Task 0 fails.

---

## Task 1: SerialIo helper in GblDebugLib

**Files:**
- Modify: `GblChainloadPkg/Library/GblDebugLib/GblDebugLib.c`
- Modify: `GblChainloadPkg/Library/GblDebugLib/GblDebugLib.inf`

The goal is a single static helper `GblWriteSerial` that lazy-locates `SerialIo` and writes an ASCII buffer. Used both by the tee in `DebugPrint` (Task 2) and by `GblSerialDbgPrint` (Task 4).

- [ ] **Step 1: Add the helper to `GblDebugLib.c`**

Near the top of the file, after the existing module-state declarations (`mPostEBS`, `mExitBootServicesEvent`, `mDebugST`), add:

```c
#include <Protocol/SerialIo.h>

/* Lazy-located gEfiSerialIoProtocol. Used to tee DEBUG output and SERIAL_DBG
 * emits into the UART log buffer (canoe BDS dumps that buffer to
 * \UefiLog<N>.txt at pre-boot). Pattern matches
 * edk2/QcomModulePkg/Library/DebugLib/DebugLib.c lines 32-70. */
STATIC EFI_SERIAL_IO_PROTOCOL *mSerialIo = NULL;

STATIC VOID
GblWriteSerial (
  IN CONST CHAR8 *Buf,
  IN UINTN        Len
  )
{
  EFI_STATUS Status;
  UINTN      Bytes = Len;

  if (mPostEBS || Buf == NULL || Len == 0) {
    return;
  }

  if (mSerialIo == NULL) {
    if (mDebugST == NULL || mDebugST->BootServices == NULL) {
      return;
    }
    Status = mDebugST->BootServices->LocateProtocol (
                                       &gEfiSerialIoProtocolGuid,
                                       NULL,
                                       (VOID **)&mSerialIo);
    if (EFI_ERROR (Status) || mSerialIo == NULL) {
      mSerialIo = NULL;
      return;
    }
  }

  /* Cast away const — SerialIo->Write takes VOID*; we don't mutate. */
  mSerialIo->Write (mSerialIo, &Bytes, (VOID *)Buf);
}
```

- [ ] **Step 2: Add `gEfiSerialIoProtocolGuid` to INF**

Edit `GblChainloadPkg/Library/GblDebugLib/GblDebugLib.inf`, add a `[Protocols]` section (or append to existing one):

```
[Protocols]
  gEfiSerialIoProtocolGuid
```

- [ ] **Step 3: Build to confirm linkage**

```bash
bash scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -3
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Library/GblDebugLib/GblDebugLib.{c,inf}
git commit -m "$(cat <<'EOF'
GblDebugLib: add SerialIo helper for UART-buffer writes

mSerialIo lazy-located via LocateProtocol; GblWriteSerial appends to
the UART log buffer (→ UefiLog<N>.txt at next BDS flush). Mirrors the
pattern in edk2/QcomModulePkg/Library/DebugLib/DebugLib.c.

Used by Task 2 (DebugPrint tee) and Task 4 (GblSerialDbgPrint).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Tee DebugPrint to SerialIo

**Files:**
- Modify: `GblChainloadPkg/Library/GblDebugLib/GblDebugLib.c`

Existing `DebugPrint` formats with `AsciiVSPrint` then calls `mDebugST->ConOut->OutputString` (UCS-2). We add a parallel `GblWriteSerial` call with the ASCII buffer.

- [ ] **Step 1: Find the existing DebugPrint emit path**

Open `GblDebugLib.c`. The existing `DebugPrint` does (paraphrased):
```c
CHAR8  AsciiBuffer[MAX];
CHAR16 Wide[MAX];
...
AsciiVSPrint (AsciiBuffer, sizeof (AsciiBuffer), Format, Marker);
... AsciiStrToUnicodeStrS into Wide ...
gDbgCurrentLevel = ErrorLevel;
mDebugST->ConOut->OutputString (mDebugST->ConOut, Wide);
gDbgCurrentLevel = GBL_DBG_LEVEL_NONE;
```

We add a `GblWriteSerial(AsciiBuffer, AsciiStrLen(AsciiBuffer))` call immediately after the `OutputString` call but before the `gDbgCurrentLevel` reset.

- [ ] **Step 2: Apply the edit**

After the `mDebugST->ConOut->OutputString(...)` line, before `gDbgCurrentLevel = GBL_DBG_LEVEL_NONE;`, insert:

```c
  /* Tee to SerialIo so the line lands in the UART log buffer regardless
   * of whether ConOut→screen suppressed it (gGblScreenMask filter, framebuffer
   * teardown post-handoff, etc.). The UART buffer is dumped to
   * \UefiLog<N>.txt at the patched ABL's BDS pre-boot. */
  GblWriteSerial (AsciiBuffer, AsciiStrLen (AsciiBuffer));
```

- [ ] **Step 3: Build**

```bash
bash scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -3
```

Expected: build succeeds, dist artifact updated.

- [ ] **Step 4: On-device verify (hand to user)**

Test sequence — give the user this exact procedure:

```
fastboot stage dist/mode-1-auto-debug.efi
fastboot oem boot-efi
# escape to fastboot via timeout (don't hold VolUp)
# from fastboot: oem escape to continue normal boot
# wait for recovery / system to land
# pull logs as usual
```

Verify on the pulled logs:

```bash
LATEST=$(ls -td logs/*/ | head -1)
grep -c 'gbl-chainload entered' "$LATEST/logfs/UefiLog"*.txt
```

Expected: count ≥ 1 (our boundary marker now reaches UefiLog via the tee).

```bash
grep -c 'gbl-chainload entered' "$LATEST/logfs/gbl-chainload_Boot"*.txt
```

Expected: count ≥ 1 (sink mirror to logfs still works pre-handoff).

```bash
# Sanity: the boot didn't regress
grep -c 'Loader Build Info' "$LATEST/logfs/UefiLog"*.txt
```

Expected: count ≥ 1 (patched ABL banner present → chain still completes).

- [ ] **Step 5: Commit**

```bash
git add GblChainloadPkg/Library/GblDebugLib/GblDebugLib.c
git commit -m "$(cat <<'EOF'
GblDebugLib: tee DEBUG output to SerialIo

DebugPrint now mirrors every formatted DEBUG line through GblWriteSerial
in addition to ConOut. Lines land in the UART log buffer and ultimately
\UefiLog<N>.txt — required for verbose-tier id correlation (Task 5+)
where the summary line and the payload hex must share a destination.

On-device verified: "gbl-chainload entered" reaches both UefiLog and
gbl-chainload_BootN.txt; patched ABL chain completes.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: SERIAL_DBG public header

**Files:**
- Create: `GblChainloadPkg/Include/Library/GblSerialDbg.h`

Macro definition: stripped in non-verbose builds, calls `GblSerialDbgPrint` otherwise. The implementation comes in Task 4.

- [ ] **Step 1: Create the header**

```c
/** @file GblSerialDbg.h — verbose-tier emit macro for gbl-chainload.

  SERIAL_DBG(fmt, ...) emits an ASCII line via gEfiSerialIoProtocol
  (→ UART log buffer → \UefiLog<N>.txt). It does NOT touch ConOut, so
  the line never reaches the framebuffer console. It does NOT touch
  LogFsWrite, so it never depends on logfs being open.

  Compile-gated on GBL_VERBOSE. In non-verbose builds the macro body is
  a no-op and the call sites compile to nothing (no string literals
  emitted into .rodata either, because the macro arguments aren't
  evaluated).

  Intended for per-hook payload hex dumps tagged with `id=N` to
  correlate with debug-tier `DEBUG_INFO` summary lines from the same
  hook invocation.
**/

#ifndef GBL_SERIAL_DBG_H_
#define GBL_SERIAL_DBG_H_

#include <Uefi.h>

#ifndef GBL_VERBOSE
# define GBL_VERBOSE 0
#endif

#if (GBL_VERBOSE == 1)

VOID EFIAPI
GblSerialDbgPrint (
  IN CONST CHAR8 *Fmt,
  ...
  );

# define SERIAL_DBG(fmt, ...)  GblSerialDbgPrint (fmt, ##__VA_ARGS__)

#else

# define SERIAL_DBG(fmt, ...)  do { (void)0; } while (0)

#endif

#endif /* GBL_SERIAL_DBG_H_ */
```

- [ ] **Step 2: Build (header-only change, won't fail)**

```bash
bash scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -3
```

Expected: build succeeds (the header isn't `#include`d anywhere yet).

- [ ] **Step 3: Commit**

```bash
git add GblChainloadPkg/Include/Library/GblSerialDbg.h
git commit -m "$(cat <<'EOF'
add GblSerialDbg.h — SERIAL_DBG macro for verbose-tier emits

Compile-gated on GBL_VERBOSE. Empty body in non-verbose builds means
zero verbose code in mode-1.efi and mode-1-debug.efi. Implementation
of GblSerialDbgPrint follows in Task 4.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: GblSerialDbgPrint implementation

**Files:**
- Modify: `GblChainloadPkg/Library/GblDebugLib/GblDebugLib.c`

Implement the `GblSerialDbgPrint` function inside `GblDebugLib.c` (same compilation unit as `GblWriteSerial`). Wrap in `#if (GBL_VERBOSE == 1)` so the function body is absent in non-verbose builds. The library always exports the symbol declaration but the body is stripped — link-time, this works because callers in non-verbose builds never reference `GblSerialDbgPrint` (the macro expands to nothing).

- [ ] **Step 1: Add the implementation**

At the end of `GblDebugLib.c`, after the existing `DebugPrint`/`DebugVPrint`/etc. routines, add:

```c
#ifndef GBL_VERBOSE
# define GBL_VERBOSE 0
#endif

#if (GBL_VERBOSE == 1)

#include <Library/PrintLib.h>

VOID EFIAPI
GblSerialDbgPrint (
  IN CONST CHAR8 *Fmt,
  ...
  )
{
  CHAR8    Buffer[MAX_DEBUG_MESSAGE_LENGTH];
  VA_LIST  Marker;
  UINTN    Len;

  if (Fmt == NULL || mPostEBS) {
    return;
  }

  VA_START (Marker, Fmt);
  Len = AsciiVSPrint (Buffer, sizeof (Buffer), Fmt, Marker);
  VA_END (Marker);

  if (Len == 0) {
    return;
  }

  GblWriteSerial (Buffer, Len);
}

#endif /* GBL_VERBOSE == 1 */
```

- [ ] **Step 2: Build the verbose variant**

```bash
bash scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -3
```

Expected: build succeeds.

- [ ] **Step 3: Verify symbol presence/absence**

```bash
strings dist/mode-1-auto-debug-verbose.efi | grep -c GblSerialDbgPrint
```

Expected: ≥ 1.

```bash
bash scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -3
strings dist/mode-1-auto-debug.efi | grep -c GblSerialDbgPrint
```

Expected: 0 (function body stripped).

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Library/GblDebugLib/GblDebugLib.c
git commit -m "$(cat <<'EOF'
GblDebugLib: implement GblSerialDbgPrint for SERIAL_DBG macro

AsciiVSPrint into a stack buffer, then GblWriteSerial → SerialIo →
UART log buffer → UefiLog. No ConOut, no mask check, no logfs mirror.
Body gated on GBL_VERBOSE=1 so non-verbose builds emit zero code.

Verified: symbol present in mode-1-auto-debug-verbose.efi, absent in
mode-1-auto-debug.efi.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Drop GBL_DBG_LOGFS_ONLY infrastructure

**Files:**
- Modify: `GblChainloadPkg/Include/Library/LogFsLib.h`
- Modify: `GblChainloadPkg/GblChainloadPkg.dsc`
- Modify: `GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.c`

The custom `0x10000000` error-level bit was a workaround for a problem the new design doesn't have. Delete it and re-tag its remaining users (AblUnwrap) to standard `DEBUG_INFO`.

- [ ] **Step 1: Remove from `LogFsLib.h`**

Open `GblChainloadPkg/Include/Library/LogFsLib.h`, find the block:

```c
#define GBL_DBG_LOGFS_ONLY  0x10000000
```

(plus surrounding comment documenting the bit). Delete both. Save.

- [ ] **Step 2: Revert PCD widening in DSC**

In `GblChainloadPkg/GblChainloadPkg.dsc`, find:

```
!if $(GBL_VERBOSE) == 1
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x90000042
!else
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000042
!endif
```

Replace with:

```
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000042
```

- [ ] **Step 3: Re-tag AblUnwrap traces**

Open `GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.c`. Find every:

```c
DEBUG ((GBL_DBG_LOGFS_ONLY, "AblUnwrap: ...", ...));
```

For each one, evaluate the message:
- If it's a one-line summary (e.g., "matched abl_a", "found PE/TE %u bytes"): change to `DEBUG ((DEBUG_INFO, ...))`.
- If it's a high-volume scan probe (e.g., "section @ 0x%X type=0x%02X size=0x%X hdr=%u", "GUID_DEFINED off=%u inner=%u" — fires N times during section walk): wrap with `SERIAL_DBG(...)` instead. Remove the `DEBUG((...))` wrapper for those.

For the SERIAL_DBG conversions, add `#include <Library/GblSerialDbg.h>` near the top of the file.

Expected categorisation (review with the actual file before editing):
- `matched abl_a` → DEBUG_INFO
- `FV @ offset 0x%X size 0x%X` → DEBUG_INFO
- `section @ 0x%X type=0x%02X size=0x%X hdr=%u` → SERIAL_DBG
- `GUID_DEFINED off=%u inner=%u` → SERIAL_DBG
- `GUID-LZMA decompressed %u bytes` → DEBUG_INFO
- `FV_IMAGE section, scanning %u bytes` → SERIAL_DBG
- `found PE/TE %u bytes` → DEBUG_INFO
- `PE/TE size 0x%X` → DEBUG_INFO

- [ ] **Step 4: Also remove `LogFsLib.h` include where only `GBL_DBG_LOGFS_ONLY` was needed**

In `AblUnwrapLib.c` and in `GblChainloadPkg/Library/ProtocolHookLib/*.c`, find every `#include <Library/LogFsLib.h>` whose comment says "for GBL_DBG_LOGFS_ONLY". Delete those includes. Keep includes that are there for other reasons (e.g., LogFsWrite, LogFsIsReady).

- [ ] **Step 5: Build**

```bash
bash scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -5
```

Expected: build succeeds. Any compile error referencing `GBL_DBG_LOGFS_ONLY` means a site was missed — find it via `grep -rn GBL_DBG_LOGFS_ONLY GblChainloadPkg` and re-tag.

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/Include/Library/LogFsLib.h GblChainloadPkg/GblChainloadPkg.dsc GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.c
git commit -m "$(cat <<'EOF'
drop GBL_DBG_LOGFS_ONLY infrastructure

The custom 0x10000000 error-level bit was a workaround for a problem
the v2 design doesn't have. PcdDebugPrintErrorLevel goes back to
unconditional 0x80000042 (no GBL_VERBOSE-conditional widen). QCOM stock
DEBUG_VERBOSE (e.g. PartitionTableUpdate.c:174) is correctly gated.

AblUnwrap high-volume scan traces re-tagged to SERIAL_DBG; one-line
summaries re-tagged to DEBUG_INFO.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Per-hook ID counter + retag — QseecomHook

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c`

Pattern: at hook entry, increment a static `UINT32 gQseeCallId`. Use that ID in the summary `DEBUG_INFO` line and in every payload `SERIAL_DBG` line for that call.

- [ ] **Step 1: Add include + counter**

Near the top of `QseecomHook.c`, after the existing `#include` block, add:

```c
#include <Library/GblSerialDbg.h>
```

After the existing `STATIC` declarations near the top of file (e.g., `STATIC SpssProtocol *gHookedSpss = NULL;` analogue), add:

```c
STATIC UINT32 gQseeCallId = 0;
```

- [ ] **Step 2: Read the existing hook bodies and identify the SUMMARY vs PAYLOAD calls**

The hooked entry points in `QseecomHook.c` are `HookedStartApp` and `HookedSendCmd` (or similar). Each currently emits multiple `DEBUG((GBL_DBG_LOGFS_ONLY, ...))` lines mixing:
- Summary metadata (handle, command, sizes, status) — should become `DEBUG_INFO`
- Payload hex (the `qsee-buf` lines with `dir=`/`off=`/`hex=`) — should become `SERIAL_DBG`

For each hook entry function, the rewrite pattern is:

```c
STATIC EFI_STATUS EFIAPI
HookedSendCmd (
  IN ...
  )
{
  UINT32  Id = ++gQseeCallId;
  ...
  /* Summary — one line, human-readable. */
  DEBUG ((DEBUG_INFO,
          "qsee | id=%u | cmd=0x%x | h=%u | sl=%u | rl=%u | st=%r\n",
          Id, Cmd, Handle, SendLen, RespLen, Status));

  /* Payload hex — multiple lines, verbose-tier. */
  SERIAL_DBG ("qsee-buf | id=%u | dir=s | off=0 | hex=%a\n", Id, SHex);
  SERIAL_DBG ("qsee-buf | id=%u | dir=s | off=64 | hex=%a\n", Id, SHex64);
  SERIAL_DBG ("qsee-buf | id=%u | dir=s | off=128 | hex=%a\n", Id, SHex128);
  SERIAL_DBG ("qsee-buf | id=%u | dir=r | off=0 | hex=%a\n", Id, RHex);
  SERIAL_DBG ("qsee-buf | id=%u | dir=r | off=64 | hex=%a\n", Id, RHex64);
  SERIAL_DBG ("qsee-buf | id=%u | dir=r | off=128 | hex=%a\n", Id, RHex128);
  ...
}
```

Apply this pattern to every hook entry function in the file. Increment `gQseeCallId` ONCE per hook entry, use the same `Id` for all emits within that invocation. For nested calls (re-entry), the `gQseeCallId` counter still increments — that's fine, the prefix and `id=` value disambiguate.

- [ ] **Step 3: Build**

```bash
bash scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -3
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c
git commit -m "$(cat <<'EOF'
QseecomHook: per-call id counter, summary/payload tier split

Each hook invocation increments gQseeCallId. The summary line
(qsee | id=N | cmd=... | st=...) emits at DEBUG_INFO so it lands in
screen (under --debug) and UefiLog (via GblDebugLib tee). The payload
hex dumps (qsee-buf | id=N | ...) emit via SERIAL_DBG so they only
land in UefiLog under --verbose.

Host-side: grep on id=N pairs summary with payload.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Per-hook ID counter + retag — ScmHook

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c`

Same pattern as Task 6, with `gScmCallId`.

- [ ] **Step 1: Add include + counter**

```c
#include <Library/GblSerialDbg.h>
...
STATIC UINT32 gScmCallId = 0;
```

- [ ] **Step 2: Apply summary/payload tier split**

The Scm hook entry functions handle the 5 SCM slots (sip, send-data, etc.). Each one currently emits `DEBUG((GBL_DBG_LOGFS_ONLY, "scm-..."))` lines. Per call:

```c
UINT32 Id = ++gScmCallId;

/* Summary */
DEBUG ((DEBUG_INFO, "scm-send | id=%u | cmd=... | st=%r\n", Id, ...));

/* Payload (if hex bodies are emitted) */
SERIAL_DBG ("scm-send-buf | id=%u | s32=%a\n", Id, SHex);
SERIAL_DBG ("scm-send-buf | id=%u | r32=%a\n", Id, RHex);
```

Apply uniformly to every hook entry in the file.

- [ ] **Step 3: Build**

```bash
bash scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c
git commit -m "ScmHook: per-call id counter, summary/payload tier split

Same pattern as QseecomHook. gScmCallId increments per hook invocation;
summary DEBUG_INFO + payload SERIAL_DBG share the id=N for correlation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Per-hook ID counter + retag — VerifiedBootHook

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c`

Same pattern, `gVbCallId`. Verified-boot hook has 10 slots — the per-call DEBUG output covers RWDeviceState, RWRollback, READ_CONFIG fakelock swallow, SendRoT, milestone updates, secure-state queries, reset. Each entry function increments the counter.

- [ ] **Step 1: Add include + counter**

```c
#include <Library/GblSerialDbg.h>
...
STATIC UINT32 gVbCallId = 0;
```

- [ ] **Step 2: Apply tier split per entry**

Example for one slot:

```c
STATIC EFI_STATUS EFIAPI
HookedRWDeviceState (
  IN  OUT  ...
  )
{
  UINT32  Id = ++gVbCallId;
  ...
  DEBUG ((DEBUG_INFO,
          "vb-rwstate | id=%u | op=%a | bufLen=%u | st=%r\n",
          Id, OpStr, BufLen, Status));

  SERIAL_DBG ("vb-rwstate-buf | id=%u | first16=%a\n", Id, First16Hex);
}
```

For swallow-style hooks (fakelock):

```c
DEBUG ((DEBUG_INFO,
        "vb-fakelock | id=%u | READ_CONFIG | is_unlocked %u->0 | is_unlock_critical %u->0\n",
        Id, OrigUnlocked, OrigUnlockCrit));
```

(No payload tier — swallows don't have a payload to dump.)

Apply uniformly to every hook entry.

- [ ] **Step 3: Build**

```bash
bash scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c
git commit -m "VerifiedBootHook: per-call id counter, summary/payload tier split

Same pattern as QseecomHook. gVbCallId increments per hook invocation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Per-hook ID counter + retag — SpssHook

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c`

Same pattern, `gSpssCallId`. SPSS has one slot (`ShareKeyMintInfo`). Existing emits cover RoT, BootState (with PublicKey), Vbh.

- [ ] **Step 1: Add include + counter**

```c
#include <Library/GblSerialDbg.h>
...
STATIC UINT32 gSpssCallId = 0;
```

- [ ] **Step 2: Apply tier split**

```c
STATIC EFI_STATUS EFIAPI
HookedShareKeyMintInfo (
  IN KeymintSharedInfoStruct *Info
  )
{
  ...
  UINT32  Id = ++gSpssCallId;
  ...

  /* Summary lines — three sub-payloads, one DEBUG_INFO each. */
  DEBUG ((DEBUG_INFO,
          "spss-rot | id=%u | cmd=0x%x | offset=%u | size=%u | st=%r\n",
          Id, Info->RootOfTrust.CmdId, Info->RootOfTrust.RotOffset,
          Info->RootOfTrust.RotSize, Status));

  DEBUG ((DEBUG_INFO,
          "spss-bootstate | id=%u | cmd=0x%x | ver=%u | unlocked=%u | color=%u | sysVer=0x%x | sysSpl=0x%x\n",
          Id, Info->BootInfo.CmdId, Info->BootInfo.Version,
          Info->BootInfo.BootState.IsUnlocked,
          Info->BootInfo.BootState.Color,
          Info->BootInfo.BootState.SystemVersion,
          Info->BootInfo.BootState.SystemSecurityLevel));

  DEBUG ((DEBUG_INFO,
          "spss-vbh | id=%u | cmd=0x%x | st=%r\n",
          Id, Info->Vbh.CmdId, Status));

  /* Payload hex — verbose tier. */
  SERIAL_DBG ("spss-rot-hex | id=%u | digest=%a\n", Id, RotHex);
  SERIAL_DBG ("spss-bootstate-hex | id=%u | pubKey=%a\n", Id, PubKeyHex);
  SERIAL_DBG ("spss-vbh-hex | id=%u | digest=%a\n", Id, VbhHex);

  ...
}
```

- [ ] **Step 3: Build**

```bash
bash scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c
git commit -m "SpssHook: per-call id counter, summary/payload tier split

Same pattern. gSpssCallId increments per ShareKeyMintInfo invocation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: Mode1Overlay sweep (if any verbose-tier traces)

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c`

Mode1Overlay wires the mode-1-specific hooks on top of UniversalBaseline. If it has its own DEBUG emits (mode-1 specific behaviour like "installed mode-1 SCM drops" etc.), check whether they're summary or payload tier:

- [ ] **Step 1: Audit emits**

```bash
grep -n 'DEBUG ((.*GBL_DBG_LOGFS_ONLY\|DEBUG ((.*DEBUG_INFO' GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c
```

- [ ] **Step 2: Apply pattern**

For each `GBL_DBG_LOGFS_ONLY` emit, decide:
- One-line summary about overlay install / config / one-shot status → `DEBUG_INFO`
- Per-call high-volume payload → `SERIAL_DBG`

Most Mode1Overlay emits are likely install-time (one-shot), which means they stay `DEBUG_INFO` with no `id=` counter needed (no per-call concept here).

- [ ] **Step 3: Build, commit**

```bash
bash scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -3
git add GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c
git commit -m "Mode1Overlay: drop GBL_DBG_LOGFS_ONLY tagging

Mode-1 overlay emits are install-time one-shots, not per-call payloads.
Re-tag from GBL_DBG_LOGFS_ONLY to DEBUG_INFO — they land in UefiLog
under --debug via the GblDebugLib tee.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 11: Update tests/052

**Files:**
- Modify: `tests/052_log_stream_split.sh`

Remove the GBL_DBG_LOGFS_ONLY-specific checks (7, 8, 9). Add new checks for the SerialIo tee, SERIAL_DBG, and per-hook counters.

- [ ] **Step 1: Delete old checks**

Remove check 7 (LogFsSetScreenMask doesn't admit logfs-only tiers — no longer relevant), check 8 (GBL_DBG_LOGFS_ONLY defined — no longer defined), check 9 (AblUnwrap uses GBL_DBG_LOGFS_ONLY — no longer used).

Also remove check 9b (ProtocolHookLib per-call traces NOT at DEBUG_INFO — the new design REQUIRES them at DEBUG_INFO for the summary tier; payload tier is at SERIAL_DBG).

- [ ] **Step 2: Add new checks**

After the existing check 11 (LogFsLib EBS callback registered), append:

```bash
# ── Check 12: SERIAL_DBG macro defined in public header ────────────────────
DBG_HDR=GblChainloadPkg/Include/Library/GblSerialDbg.h
if [ ! -f "$DBG_HDR" ]; then
  echo "FAIL: $DBG_HDR not found — SERIAL_DBG macro missing" >&2
  fail=1
else
  if ! grep -q 'define SERIAL_DBG' "$DBG_HDR"; then
    echo "FAIL: SERIAL_DBG macro not defined in $DBG_HDR" >&2
    fail=1
  fi
  if ! grep -qE 'GBL_VERBOSE.*1.*\n.*GblSerialDbgPrint|if.*GBL_VERBOSE' "$DBG_HDR"; then
    echo "FAIL: SERIAL_DBG not gated on GBL_VERBOSE in $DBG_HDR" >&2
    fail=1
  fi
fi

# ── Check 13: GblDebugLib tees to SerialIo ─────────────────────────────────
DBG_C=$DBGLIB/GblDebugLib.c
if [ ! -f "$DBG_C" ]; then
  echo "FAIL: $DBG_C not found" >&2
  fail=1
else
  if ! grep -q 'gEfiSerialIoProtocolGuid' "$DBG_C"; then
    echo "FAIL: GblDebugLib.c does not locate gEfiSerialIoProtocol — DEBUG output won't reach UefiLog" >&2
    fail=1
  fi
  if ! grep -q 'GblWriteSerial' "$DBG_C"; then
    echo "FAIL: GblDebugLib.c missing GblWriteSerial helper" >&2
    fail=1
  fi
fi

# ── Check 14: Per-hook id counters declared in ProtocolHookLib ─────────────
for hook in QseecomHook ScmHook VerifiedBootHook SpssHook; do
  HOOK_C=GblChainloadPkg/Library/ProtocolHookLib/${hook}.c
  if [ ! -f "$HOOK_C" ]; then continue; fi
  # Each hook needs a static CallId counter.
  if ! grep -qE 'STATIC UINT32 g[A-Za-z]+CallId' "$HOOK_C"; then
    echo "FAIL: $HOOK_C missing STATIC UINT32 g*CallId counter" >&2
    fail=1
  fi
  if ! grep -q '#include <Library/GblSerialDbg.h>' "$HOOK_C"; then
    echo "FAIL: $HOOK_C missing #include <Library/GblSerialDbg.h>" >&2
    fail=1
  fi
done

# ── Check 15: Old GBL_DBG_LOGFS_ONLY infrastructure fully removed ──────────
if grep -rn 'GBL_DBG_LOGFS_ONLY' GblChainloadPkg 2>/dev/null | grep -v '^Binary'; then
  echo "FAIL: GBL_DBG_LOGFS_ONLY references still present (above) — should be deleted" >&2
  fail=1
fi
```

- [ ] **Step 3: Run the test**

```bash
bash tests/052_log_stream_split.sh
```

Expected: `OK: log stream split constants and routing in place.`

- [ ] **Step 4: Commit**

```bash
git add tests/052_log_stream_split.sh
git commit -m "tests/052: replace GBL_DBG_LOGFS_ONLY checks with v2 SerialIo design

Drops checks 7/8/9 (logfs-only tier was the wrong abstraction).
Adds checks for SERIAL_DBG macro + GBL_VERBOSE gate, SerialIo tee in
GblDebugLib, per-hook id counters in ProtocolHookLib, and a sweep
that no GBL_DBG_LOGFS_ONLY references remain anywhere.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 12: Update tests/010 — verbose-only symbol check

**Files:**
- Modify: `tests/010_build_smoke.sh`

- [ ] **Step 1: Add symbol presence/absence assertions**

After the existing build-success assertions, append:

```bash
# ── Verbose-tier compile gate verification ─────────────────────────────────
# mode-1-auto-debug-verbose MUST contain GblSerialDbgPrint (verbose tier
# compiles in). mode-1-auto-debug MUST NOT contain it (compile-gated out).
if [ -f dist/mode-1-auto-debug-verbose.efi ]; then
  if ! strings dist/mode-1-auto-debug-verbose.efi 2>/dev/null | grep -q GblSerialDbgPrint; then
    # Symbol name may not appear as a literal in the binary; check via nm/objdump if installed.
    # If strings doesn't find it, fall back to a string-marker check on a unique format literal.
    if ! grep -lq 'GblSerialDbgPrint' dist/mode-1-auto-debug-verbose.efi 2>/dev/null; then
      echo "WARN: GblSerialDbgPrint symbol not visible via strings in verbose build — manual nm/objdump verification needed" >&2
    fi
  fi
fi
if [ -f dist/mode-1-auto-debug.efi ]; then
  if strings dist/mode-1-auto-debug.efi 2>/dev/null | grep -q GblSerialDbgPrint; then
    echo "FAIL: GblSerialDbgPrint symbol present in non-verbose build — compile-gate failed" >&2
    fail=1
  fi
fi
```

Note: the symbol-presence check in the verbose build is best-effort because EDK2 may strip symbol names. The hard check is the *absence* in non-verbose builds.

- [ ] **Step 2: Run**

```bash
bash tests/010_build_smoke.sh
```

Expected: `ok 010_build_smoke` (assuming all variants build).

- [ ] **Step 3: Commit**

```bash
git add tests/010_build_smoke.sh
git commit -m "tests/010: assert SERIAL_DBG compile-gate works

mode-1-auto-debug.efi must NOT contain GblSerialDbgPrint (GBL_VERBOSE=0
strips the body). Verbose build check is best-effort because EDK2 may
strip symbol names from the final binary.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 13: On-device end-to-end verification

**This is not a code task — it's a procedure for the user to run on hardware.** The agent does not run fastboot autonomously (CLAUDE.md rule). Hand the user the commands below; collect and inspect the resulting logs.

- [ ] **Step 1: Stage and boot the verbose variant**

```
fastboot stage dist/mode-1-auto-debug-verbose.efi
fastboot oem boot-efi
# escape via timeout to fastboot
# from fastboot: oem escape to continue to recovery
# wait for recovery / system, pull logs
```

- [ ] **Step 2: Verify verbose payload presence in UefiLog**

```bash
LATEST=$(ls -td logs/*/ | head -1)
echo "Inspecting: $LATEST"

# Payloads should be in UefiLog only.
echo "qsee-buf lines in UefiLog:  $(grep -c 'qsee-buf | id=' "$LATEST/logfs/UefiLog"*.txt 2>/dev/null)"
echo "qsee-buf lines in gbl-chainload_Boot: $(grep -c 'qsee-buf | id=' "$LATEST/logfs/gbl-chainload_Boot"*.txt 2>/dev/null)"

# Summary lines should be in BOTH (debug-tier visible everywhere under --debug).
echo "qsee summary in UefiLog:    $(grep -c 'qsee | id=' "$LATEST/logfs/UefiLog"*.txt 2>/dev/null)"
echo "qsee summary in gbl-chainload_Boot: $(grep -c 'qsee | id=' "$LATEST/logfs/gbl-chainload_Boot"*.txt 2>/dev/null)"
```

Expected:
- `qsee-buf` count in UefiLog: > 0 (verbose payload landed via SerialIo tee).
- `qsee-buf` count in gbl-chainload_Boot: 0 (verbose tier bypasses ConOut, doesn't reach the sink mirror).
- `qsee summary` count in UefiLog: > 0 (debug-tier landed via SerialIo tee in GblDebugLib).
- `qsee summary` count in gbl-chainload_Boot: > 0 (debug-tier mirrored to logfs pre-handoff via the sink, or via the ConOut path under --debug).

- [ ] **Step 3: Verify id correlation works**

```bash
# Pick the first qsee summary id.
ID=$(grep -oP 'qsee \| id=\K[0-9]+' "$LATEST/logfs/UefiLog"*.txt 2>/dev/null | head -1)
echo "Probing id=$ID"
grep -E "qsee( | -buf)? \| id=$ID\b" "$LATEST/logfs/UefiLog"*.txt
```

Expected: one `qsee | id=N` summary line + multiple `qsee-buf | id=N` payload lines, all matched by the same N.

- [ ] **Step 4: Verify boot regression-free**

```bash
# Was the boot fully completed? Patched ABL banner present?
grep -c 'Loader Build Info' "$LATEST/logfs/UefiLog"*.txt
```

Expected: ≥ 1.

```bash
# Did recovery boot complete? (Approximate — depends on user's logs setup.)
ls "$LATEST/" 2>&1 | head
```

User confirms the device booted to recovery and is responsive (no hard power-off).

- [ ] **Step 5: Repeat with non-verbose variant**

```
fastboot stage dist/mode-1-auto-debug.efi
fastboot oem boot-efi
# continue as before
```

Then:

```bash
LATEST=$(ls -td logs/*/ | head -1)
echo "qsee-buf lines in UefiLog (non-verbose): $(grep -c 'qsee-buf | id=' "$LATEST/logfs/UefiLog"*.txt 2>/dev/null)"
```

Expected: 0 (`SERIAL_DBG` macro body stripped → no `qsee-buf` emit at all).

- [ ] **Step 6: Commit the verification record**

Create or update `docs/superpowers/specs/2026-05-13-logging-v2-serial-tier-design.md` with a final "Verification" section noting:
- Date verified
- Specific log directory used for verification
- The four assertions above all passed

```bash
git add docs/superpowers/specs/2026-05-13-logging-v2-serial-tier-design.md
git commit -m "spec: logging v2 verified on-device

Verbose-tier qsee-buf hex payloads landed in UefiLog (count > 0) via
SerialIo. Non-verbose build had 0 qsee-buf emits. id correlation
worked end-to-end. No boot regression observed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 14: Update memory note

**Files:**
- Modify: `/home/vivy/.claude/projects/-home-vivy-gbl-chainload/memory/logfs_open_across_handoff.md`

The existing memory note documents why `LogFsClose()` must run before `LoadImage`. It also notes that verbose-tier is "dead-in-logfs by design" — that's no longer true (verbose-tier now lands in UefiLog via SerialIo tee). Update the note.

- [ ] **Step 1: Rewrite the relevant section**

Replace the "Why verbose-tier is dead-in-logfs" paragraph with:

```
**Verbose tier destination (v2):** The hooks fire post-handoff and emit
through SERIAL_DBG (defined in GblChainloadPkg/Include/Library/GblSerialDbg.h)
which calls SerialIo->Write directly via gEfiSerialIoProtocol. That path
lands in the UART log buffer → \UefiLog<N>.txt at the next BDS flush.
ConOut and the closed logfs are NOT in the path. The screen stays
quiet; UefiLog accumulates the payload hex; gbl-chainload_Boot<N>.txt
captures only what the sink saw pre-handoff (debug-tier summaries).
```

- [ ] **Step 2: Update MEMORY.md pointer line**

Change:
```
- [BootFlow MUST close logfs before LoadImage](logfs_open_across_handoff.md) — keep-open observed to crash patched-ABL → recovery on infiniti; verbose-tier post-handoff is dead-in-logfs by design
```

To:
```
- [BootFlow MUST close logfs before LoadImage](logfs_open_across_handoff.md) — partition handle rule; verbose-tier lives in UefiLog via gEfiSerialIoProtocol tee (v2)
```

(Memory files live outside the repo — no git commit needed here.)

---

## Self-Review (run before handing off)

**Spec coverage (against `docs/superpowers/specs/2026-05-13-logging-v2-serial-tier-design.md`):**

| Spec section | Task implementing it |
|--------------|----------------------|
| §1 goals 1-2 (silent prod, --debug visible) | Task 2 (DebugLib tee covers both — prod's DEBUG_ERROR-only mask gates screen; --debug widens; UefiLog gets it via tee) |
| §1 goal 3 (--verbose to UefiLog only) | Tasks 3, 4 (SERIAL_DBG macro + impl) + Tasks 6-9 (hooks use it) |
| §1 goal 4 (id correlation) | Tasks 6-9 (per-hook id counters) + Task 13 step 3 (verify on-device) |
| §1 goal 5 (compile-time strip) | Task 3 (header gate) + Task 4 (body gate) + Task 12 (regression check) |
| §2 (UART buffer mechanism) | De-scoped (SerialIo provider already routes there — see Task 0 verification) |
| §3 tier table | Tasks 2 + 3 + 4 implement the three tiers' code paths |
| §4 components | Each Task creates/modifies exactly the file listed in §4 |
| §6.1 tests/052 | Task 11 |
| §6.1 tests/010 | Task 12 |
| §6.2 on-device | Task 13 |
| §7 open questions | §7.1 (1 MiB resize) de-scoped per plan deviation note; §7.2 (pre-resize copy) not relevant since no resize |
| §10 rollout | Implementation lands on `feature/logging-v2-impl` after #17 merges |

**Placeholder scan:** none — every step has the literal command or code block.

**Type consistency:**
- `GblWriteSerial(IN CONST CHAR8 *Buf, IN UINTN Len)` — used by Tasks 1, 2, 4. Signature consistent.
- `GblSerialDbgPrint(IN CONST CHAR8 *Fmt, ...)` — declared in header Task 3, defined in Task 4. Consistent.
- `SERIAL_DBG(fmt, ...)` macro — used in Tasks 6-10. Always with `id=N` prefix in payload calls.
- `g<Category>CallId` static counters — named consistently `gQseeCallId`, `gScmCallId`, `gVbCallId`, `gSpssCallId`. Each declared once per file.
- `mSerialIo` — file-local, declared in Task 1, referenced in Task 1 and Task 4 only (both in `GblDebugLib.c`).
- `mPostEBS` — already exists in `GblDebugLib.c` (PR #17 baseline). Tasks 1 and 4 reference it for safety.

**Scope check:** single subsystem (logging). One plan is correct.

**Gap check:** the `mPostEBS` flag is referenced in Tasks 1 and 4 — that flag is set by the existing `ExitBootServicesCallback` in `GblDebugLib.c` (added by PR #17). Confirmed it's available post-#17-merge.

**Backwards-compat for any consumer of `GBL_DBG_LOGFS_ONLY` outside `GblChainloadPkg`?** Run `grep -rn GBL_DBG_LOGFS_ONLY` across the whole repo (not just GblChainloadPkg) at the start of Task 5. If anything else uses it (unlikely), add a task to re-tag those too.

---

## Execution choice

Plan complete and saved to `docs/superpowers/plans/2026-05-13-logging-v2-serial-tier.md`. Two execution options:

**1. Subagent-Driven (recommended)** — fresh subagent per task, two-stage review between tasks. Best for this plan because Task 0 is a hard gate; if it fails, downstream tasks become moot and a fresh subagent isn't carrying assumptions.

**2. Inline Execution** — execute tasks in this session via executing-plans, batch with checkpoints. Faster for the trivial commit-and-move tasks; less protective on the Task 0 gate.

Which approach?
