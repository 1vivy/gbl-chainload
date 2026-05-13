# QCOM UART Log Buffer — Investigation Brief

**Goal:** Determine whether gbl-chainload (a stage-2 EFI UEFI_APPLICATION loaded by stock QCOM GBL/XBL on canoe) can **resize and write into the same UART log buffer** that the patched ABL writes into — the buffer whose contents are eventually flushed to `\UefiLog<N>.txt` on the `logfs` partition by `PlatformBdsLib::WriteLogBufToPartition`.

**Why:** From our app, `DEBUG()` calls and `gEfiSerialIoProtocol` writes do not reach `\UefiLog<N>.txt` (verified on-device with a probe: `LocateHandleBuffer(&gEfiSerialIoProtocolGuid)` returns `EFI_NOT_FOUND`). The patched ABL's content lands in UefiLog through a different path — statically-linked QCOM `SerialPortShLib` calling into `SioPortLib` via QCOM's `ShLib` runtime loader, writing into a RAM ring `UefiInfoBlk->UartLogBufferPtr`. We want our DEBUG output to share that destination, and we want the destination to be 1 MiB instead of the default 32 KiB so verbose-tier hex dumps don't truncate.

**Context for the agent:** This investigation is part of `gbl-chainload`, a chain-loader EFI app for OnePlus 12R (canoe / SM8550). Project lives at `/home/vivy/gbl-chainload`. The patched ABL is the device's stock ABL with a small number of patches; we install runtime hooks (VerifiedBoot / SCM / QSEECOM / SPSS) inside it to spoof lock state. The previous brainstorming attempt that triggered this investigation is in `docs/superpowers/specs/2026-05-13-logging-v2-serial-tier-design.md` and the plan that didn't survive Task 0 is in `docs/superpowers/plans/2026-05-13-logging-v2-serial-tier.md` — don't follow them; they're superseded by this investigation.

**Constraints:**
- Source we have direct access to: BOOT.MXF.2.5.1 at `~/BOOT.MXF.2.5.1/buildpath0/boot_images/` (QCOM reference BSP for an adjacent SoC family). License header is "Confidential and Proprietary - Qualcomm Technologies, Inc.", so we read for understanding but cannot vendor it verbatim into our open repo.
- edk2 fork pinned in our repo: `/home/vivy/gbl-chainload/edk2/`. Already has `QcomModulePkg` for the ABL bits we modify; does NOT have `QcomPkg` or `SerialPortShLib`.
- On-device test loop: `fastboot stage dist/<artifact>.efi` + `fastboot oem boot-efi`. CLAUDE.md project rules forbid autonomous non-HLOS flash.

---

## Background — what we already know

### The mechanism

`~/BOOT.MXF.2.5.1/buildpath0/boot_images/boot/QcomPkg/XBLCore/SerialBuffer.c` allocates the UART log buffer:

- `UefiInfoBlk->UartLogBufferPtr` (typed `UINTN*`)
- `UefiInfoBlk->UartLogBufferLen` (default 32 KiB; max 1 MiB per `uefiplat.cfg`'s `UARTLogBufferSize`).

Allocation is one of:
- `StaticAllocLogBuffer` — uses a dedicated `UEFI_Log` mem region from `ipcat`.
- `DynamicAllocLogBuffer` — `AllocatePagesRuntimeServiceData(EFI_SIZE_TO_PAGES(UARTLogBufferSize))`.

Both call `SerialBufferReInit (NewBuf, NewSize)` from `<Library/SerialPortShLibInstall.h>` to update the live SerialPortLib's pointer-into-buffer. `SerialBufferReInit` is declared in `QcomPkg/Include/Library/SerialPortShLibInstall.h` and implemented in `QcomPkg/Library/SerialPortLib/SerialPortShLib.c`.

`SerialPortShLib.c::SerialPortWrite` calls `SioPortLibPtr->Write (Buffer, Bytes)` where `SioPortLibPtr` is loaded once via `GetShLibLoader()->LoadLib(SIO_PORT_LIB_NAME, ...)` — runtime shared-library loading through QCOM's `ShLib` facility.

`UefiInfoBlk` is created by XBLCore and published via HOB with `gEfiInfoBlkHobGuid` (see `QcomPkg/XBLCore/UefiInfoBlk.c::AddInfoBlkHob`).

The buffer is flushed at `PlatformBdsLib.c::WriteLogBufToPartition` (line ~2858 of the BSP file) at a BDS pre-boot milestone — it calls `WriteFile(LogBufFile, ..., L"logfs", ..., (UINT8*)UefiInfoBlk->UartLogBufferPtr, UefiInfoBlk->UartLogBufferLen)`. The file name rotates: `UefiLog<N>.txt` where `N = BootCycleCount % 5`.

### What we tested (Task 0 probe)

We staged a small UEFI_APPLICATION on canoe that does:

```c
gBS->LocateHandleBuffer(ByProtocol, &gEfiSerialIoProtocolGuid, NULL, &NumHandles, &Handles);
```

Result on canoe: `EFI_NOT_FOUND`. Zero `gEfiSerialIoProtocol` handles installed in our context. So the standard EDK2 "DEBUG → SerialPortWrite → SerialIoProtocol → UART" path that other ARM platforms (linaro Hikey, SynQuacer, etc.) rely on **does not exist** here. The probe also called `DEBUG((DEBUG_ERROR, ...))` as a fallback; its output did not reach `UefiLog<N>.txt` either (verifying our `DebugLib`'s `OutputString` path doesn't reach the UART buffer).

### What the patched ABL does differently

The patched ABL is a stock ABL PE with our patches applied. It is statically linked against QCOM's `SerialPortShLib.lib` (and the rest of `QcomPkg`). When ABL emits `DEBUG()`, the call resolves to ABL's own `DebugLib` instance, which calls ABL's own `SerialPortWrite` — i.e., `SerialPortShLib::SerialPortWrite` → `SioPortLibPtr->Write` → ring buffer at `UefiInfoBlk->UartLogBufferPtr`.

Both stock GBL and patched ABL share the same `UefiInfoBlk` struct (because the HOB carries the pointer across image loads), so they write to the same buffer. Each has its own copy of QCOM's libs statically linked into its own image.

Our `gbl-chainload.efi` does NOT have `SerialPortShLib` or `ShLib` statically linked. Our `DEBUG()` resolves to `UefiDebugLibConOut` (or to `GblDebugLib` post-PR-#17, which still routes ConOut-only) — neither writes to the UART buffer.

---

## Investigation questions

Group A and B are the load-bearing questions. Group C and D are useful context if A/B turn up something.

### Group A — can we resize the buffer from our app?

**A1.** Does the `UefiInfoBlk` HOB (`gEfiInfoBlkHobGuid`) survive the load of a stage-2 UEFI_APPLICATION on canoe?
- Check: build a probe that does `GetNextGuidHob(&gEfiInfoBlkHobGuid)`, dereferences the result, prints `Signature`, `StructVersion`, `UartLogBufferPtr`, `UartLogBufferLen`. Stage + boot-efi. Examine `Print()` output on-screen (logfs may not be reachable if our app exits abnormally — design the probe to print and `WaitForEvent` so the user can read the screen, OR write the values into a file in logfs before exit).
- Pin down the literal `gEfiInfoBlkHobGuid` value from `QcomPkg`'s DEC file. (Note: it's defined somewhere in `QcomPkg.dec` or a similar package DEC. Find the GUID literal so we can declare it externally.)
- Expected if HOB survives: signature equals `'I' | 'B'<<8 | 'l'<<16 | 'k'<<24` (i.e., the four ASCII bytes `IBlk`), version is `UEFI_INFO_BLOCK_VERSION` (`0x00010010` in BOOT.MXF.2.5.1; canoe's version may differ — record exact value), `UartLogBufferPtr` is a non-NULL address in DDR, `UartLogBufferLen` is some power of two (likely 0x8000 = 32 KiB for canoe stock).

**A2.** If `UefiInfoBlk` is reachable, can we allocate a new 1 MiB buffer, copy the existing content forward, and update `UefiInfoBlk->UartLogBufferPtr/Len`?
- The mechanical operation is simple: `AllocatePagesRuntimeServiceData(256)`, `CopyMem(NewBuf, OldPtr, OldLen)`, `ZeroMem(NewBuf+OldLen, NewSize-OldLen)`, write the two fields.
- The critical question: **does anything in the chain cache the old `UartLogBufferPtr`?** If `SerialPortShLib`'s `SioPortLibPtr` caches the address at init time (which seems likely from `SerialBufferReInit`'s existence), our pointer swap is invisible to continuing writes.
- Investigate: read `SerialPortShLib.c::SerialPortInitialize` (line ~131 of BOOT.MXF.2.5.1) and the SioPortLib it loads. Where is the buffer address stored? Is it re-read from `UefiInfoBlk` per write, or cached?
- Investigate: what does `SerialBufferReInit` actually do? Source is `~/BOOT.MXF.2.5.1/buildpath0/boot_images/boot/QcomPkg/Library/SerialPortLib/SerialPortShLib.c`. Read it.

**A3.** If the SerialPortLib caches the buffer pointer, is there a way to force a re-init from outside the patched ABL?
- Option α: vendor-in `SerialPortShLib.c` + `SerialPortShLibInstall.h` (and any small ShLib client glue) into our edk2 fork's `QcomModulePkg`. Same license concern as before — read the BOOT.MXF.2.5.1 source for "redistributable" terms (some QCOM files are BSD-licensed despite the "Confidential and Proprietary" header which is sometimes a stale boilerplate). Check the actual SPDX or LICENSE markers.
- Option β: locate `SioPortLibPtr` (or the analogous field in the loaded SioPortLib instance) at runtime — it lives in the SerialPortLib statically linked into stock GBL, whose image base we can find via `EFI_LOADED_IMAGE_PROTOCOL` (call `HandleProtocol` against the parent that loaded us). Pattern-scan the parent image for the `SioPortLibPtr` variable. Brittle but possible.
- Option γ: don't try to redirect writes; just leak our own writes into the buffer at our own offset. Race risk discussed below.

**A4.** Is there a different mechanism we missed that already does the same thing on canoe — e.g., a `gQcomUefiLogProtocolGuid` or `gQcomSerialLogProtocolGuid` that wraps the buffer with a public-API write entry point?
- Grep `~/BOOT.MXF.2.5.1` for all installed protocols (`InstallProtocolInterface`, `InstallMultipleProtocolInterfaces`) that contain "UefiLog" / "Serial" / "Uart" / "DebugLog" in any GUID name or callback name. Look in `QcomPkg/Drivers/` and `QcomPkg/Library/PlatformBdsLib/`.
- The probe in Group A1 can also enumerate ALL protocols installed in our context (use `gBS->LocateHandleBuffer(AllHandles, NULL, NULL, ...)` then `gBS->ProtocolsPerHandle(...)`) and print GUIDs. If any QCOM-private "log" protocol exists, we'd see it.

### Group B — can we share-write into the existing buffer with our own bookkeeping?

This is the "no resize, no API redirect — just write into the buffer at our own offset" path.

**B1.** What is the actual write pattern of QCOM's `SioPortLib->Write`? Is it a linear append (with an internal cursor) that wraps at `UartLogBufferLen`? Or a free-form fill?
- Read the SioPortLib implementation. It's in `~/BOOT.MXF.2.5.1/...` — but which file backs `SIO_PORT_LIB_NAME` via `ShLib`'s `LoadLib`? Look at `QcomPkg/XBLCore/ShLib*.c` or `QcomPkg/Library/ShLib*` for the loader, then trace what gets installed under that name.

**B2.** If it's a linear append with internal cursor, is the cursor stored in `UefiInfoBlk` or in the SioPortLib's private state?
- If in `UefiInfoBlk`: we can read/write it. Append cleanly possible.
- If in private state: we can't observe it. Our writes would race with QCOM's at unknown offsets.

**B3.** Is the buffer zero-padded at init? If yes, we can scan from start for the first non-zero byte sequence and identify "end of valid content". (Even then, races with concurrent writers, but maybe acceptable if our hooks fire after all other writers have quiesced.)

**B4.** When does the patched ABL stop writing to the buffer? E.g., does it call `WriteLogBufToPartition` itself and then stop? Or does it keep writing until EBS?
- Read `PlatformBdsLib.c::WriteLogBufToPartition`'s call sites. When is it called?

### Group C — linaro / non-QCOM reference patterns

The user noted "linaro and other projects have a further down bootchain loading for other devices (i.e laptops kits etc)" — specifically about how the buffer-increase mechanism is handled.

**C1.** How does linaro/ARM-Trusted-Firmware / OP-TEE / Linaro EDK2 Platforms handle the "log buffer that gets dumped to file at BDS pre-boot" pattern? Is there an upstream-EDK2 idiom for this?
- Look at: `edk2-platforms` repo (Linaro maintained), `MdeModulePkg`, `ArmPkg`, `EmbeddedPkg`.
- Specifically search for any `RamDebugLib` / `RamLogLib` / `MemoryLogDebugLib` pattern that ships a ring buffer with a public `Resize` or `Append` API.

**C2.** Is there a way to register a "log redirector" via a UEFI protocol that other images then route through? (Like a kind of capture proxy.)
- Search EDK2 master for "Status Code" libraries — `ReportStatusCodeLib` etc. Some platforms route DEBUG through this. Determine if canoe does (probably not, but worth checking).

**C3.** Look at gbl_root_canoe (the project's existing reference) for any UART-buffer-related code we may have missed when porting. The README and `tests/` may hint at it. The user already pointed at this codebase in the project's CLAUDE.md.

### Group D — context boundaries

The user raised: "afaik early bootchain lives in its own uefi context."

**D1.** Does stock canoe ABL run in the same UEFI Boot Services context as the loading GBL, or in a fresh one?
- Per UEFI spec, `LoadImage` + `StartImage` uses the same `gST`. But QCOM might do an `ExitBootServices` between GBL and ABL — rare but worth verifying.
- Check `~/BOOT.MXF.2.5.1/buildpath0/boot_images/boot/QcomPkg/Drivers/Deprecated/BdsDxe/BdsEntry.c` for `EFI_BOOT_SERVICES *gBS` references near `LoadImage` of `ABL.efi` (or whatever the ABL image is named in canoe).

**D2.** Does the loaded patched ABL get a fresh `UefiInfoBlk` HOB list, or inherit ours? If inherits, A1's HOB lookup is the right path.

**D3.** Are there multiple `UefiInfoBlk` instances in the chain — one per image? Search BOOT.MXF.2.5.1 for `InitInfoBlock` call sites. If GBL re-initializes the block before loading ABL, our stage-2 app might see a different block than ABL eventually sees.

---

## Suggested probe sequence

Order these so each probe's result narrows the next:

### Probe 1 — HOB visibility + buffer pointer/size

Goal: confirm `UefiInfoBlk` is reachable; record canoe's actual buffer base and size.

```c
EFI_HOB_GUID_TYPE *Hob = GetNextGuidHob (&gEfiInfoBlkHobGuid, GetHobList());
if (Hob) {
  UefiInfoBlkType *Blk = *(UefiInfoBlkType **)GET_GUID_HOB_DATA(Hob);
  /* Print: Blk->Signature, Blk->StructVersion, Blk->UartLogBufferPtr, Blk->UartLogBufferLen */
}
```

Find the literal GUID for `gEfiInfoBlkHobGuid` first (it's in `QcomPkg.dec`).

### Probe 2 — write to buffer, observe in UefiLog

If Probe 1 reveals a valid `UartLogBufferPtr`, write a known magic string to it directly (at a guessed offset — e.g., `UartLogBufferPtr + 0x100` — somewhere safe-looking). Then reboot to recovery, pull `\UefiLog<N>.txt`, search for the magic. If found, we know direct writes work. If not, we know the buffer pointer in `UefiInfoBlk` is stale or the write was overwritten.

### Probe 3 — allocate new buffer, swap pointer, observe

Allocate 1 MiB, copy, swap. Trigger a known DEBUG_INFO from stock GBL (e.g., by waiting for the next BDS phase) and see whether the new buffer received it (proves the SerialPortLib re-reads `UefiInfoBlk`) or whether the old buffer still grows (proves it caches).

### Probe 4 — enumerate all protocols + GUIDs

```c
gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &Count, &Handles);
for each handle:
  gBS->ProtocolsPerHandle(handle, &Guids, &GuidCount);
  for each guid: print
```

Look for any QCOM-prefix GUID with "log" or "uart" semantic. Match against `QcomPkg.dec`'s `[Protocols]` section.

### Probe 5 — direct pattern scan of parent image for SioPortLibPtr

If the parent (stock GBL) has `SerialPortShLib` statically linked, the `SioPortLibPtr` variable lives in its image. Find via `EFI_LOADED_IMAGE_PROTOCOL` on our parent, then scan for a pointer pattern that matches `UartLogBufferPtr` (since SioPortLibPtr's `Write` function probably references it). This is a long shot but proves option A3-β feasibility.

---

## Deliverables expected from the agent

1. **Probe results** — text from on-device runs of probes 1–5, including:
   - Whether `UefiInfoBlk` is reachable (with values).
   - Whether direct writes land in `UefiLog<N>.txt`.
   - Whether buffer swap takes effect (or cache invalidates).
   - List of all protocol GUIDs installed in our context.
   - Whether a `SioPortLibPtr` can be located in the parent image.

2. **Source citations** — for any claim about QCOM behavior, cite the specific file + line in `~/BOOT.MXF.2.5.1/`. For claims about EDK2 or linaro, cite the specific upstream repo + commit + path.

3. **Recommendation** — given the probe results, which of these is implementable from a stage-2 EFI app on canoe:
   - (a) Resize buffer + redirect QCOM writes (most ideal — patched ABL writes also land in our 1 MiB).
   - (b) Resize buffer + we write into new buffer only; QCOM keeps using old 32 KiB.
   - (c) No resize; we share-write into existing buffer at a tracked offset.
   - (d) Different mechanism we haven't considered.

4. **Licensing review** — for any QCOM file we'd need to vendor (e.g., `SerialPortShLib.c`), determine whether the SPDX line + license header allow inclusion in our open-source repo. If not, identify which APIs can be reimplemented without copying the source.

5. **Open questions** — anything the probes turned up that needs follow-on investigation.

---

## Out of scope

- Anything that requires re-flashing the device (we use `fastboot stage` + `oem boot-efi` only).
- Building a fastboot OEM command to manipulate the buffer at runtime (separate work).
- Implementing the actual logging redesign (this is investigation; design comes after).
- Replacing `WriteLogBufToPartition` with our own logfs-write path (different design direction).

---

## Why this matters (one paragraph for the agent)

The current logging design in `feature/cleanup-p1c-log-stream-split` (PR #17) gets debug-tier summary lines on screen under `--debug` and into `gbl-chainload_Boot<N>.txt` via a logfs sink mirror — both before the patched ABL handoff. Post-handoff, ABL writes its DEBUG output into the UART log buffer which is flushed to `UefiLog<N>.txt`. We want our gbl-chainload code to also write into that UART buffer (so our pre-handoff content is in UefiLog too, alongside ABL's content) and to expand the buffer to 1 MiB so verbose hex dumps from our hooks don't truncate. This investigation determines whether those two goals are mechanically possible from a stage-2 EFI app on canoe without vendoring proprietary QCOM source.
