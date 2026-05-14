# QCOM UART Log Buffer — Source-Level Investigation for `gbl-chainload`

**Date:** 2026-05-13
**Target:** OnePlus 12R "canoe" (SM8550). Chain: **XBL → stock ABL → `gbl-chainload` (loaded from EFISP) → patched ABL** (loaded by us via `LoadImage`+`StartImage`). All four live in one UEFI Boot Services context post-XBL.
**Scope:** Determine, from publicly mirrored QCOM BSP source, whether a stage-2 EFI app can (a) resize and redirect QCOM's UART log buffer, (b) share-write into the existing buffer without redirecting, or (c) must take a different approach.

> Companion to PR #17 (`GblDebugLib` + logfs `gbl-chainload_Boot<N>.txt`).
> Source brief: `docs/re/qcom-uart-log-buffer-investigation.md`.

---

## 0. Source-base used in this investigation

The user's reference tree is `~/BOOT.MXF.2.5.1/buildpath0/boot_images/`, which is inaccessible to research. The closest publicly mirrored QCOM XBL source trees that contain the same `QcomPkg/XBLCore/`, `QcomPkg/Library/SerialPortShLib/`, `QcomPkg/Include/Library/UefiInfoBlk.h`, and `QcomPkg/QcomPkg.dec` files are:

- **`Rivko/android-firmware-qti-sdm670`** (GitHub) — full SDM670 / QCS605 BSP, build label `BOOT.XF.2.1-00132-SDM710LZB-4`. Contains the complete `boot_images/QcomPkg/` tree including `XBLCore/`, `Library/SerialPortShLib/`, `Include/Library/UefiInfoBlk.h`, `QcomPkg.dec`, and `Drivers/SerialPortDxe/`. This is the most complete public mirror but is **two SoC generations older** than SM8550 (SDM670/710 → SDM8x5 → SM8350 → SM8550). The same architectural skeleton has been carried forward across BSPs since at least MSM8996, so the file structure and most symbols match, but exact byte layouts of `UefiInfoBlkType` and call-site counts can drift between releases.
- **`WOA-Project/mu_andromeda_platforms`** (GitHub, `Silicon/QC/Sm8350/QcomPkg/`) — Project-Mu re-host of a SM8350 (Surface Duo 2) BSP. Closer SoC generation to SM8550 than SDM670; ships the Sm8350 `QcomPkg/Include/DXE.inc` driver inventory and many headers. Most useful for confirming SM8x50-era driver topology.
- **`Project-Aloha/mu_aloha_platforms`** (GitHub) — fork of mu\_andromeda\_platforms targeting other Qualcomm devices, sometimes with SM8450/SM8550 patches.
- **`gitlab.com/Codeaurora/abl_tianocore_edk2`** (LA.UM.\* branches) — the *open-source* portion of ABL (`QcomModulePkg/` and the BSD-licensed pieces of `edk2-platforms`). The closed `QcomPkg/` is **not** in this repo.
- **`gitlab.mingwork.com/hanguoliang/test`** — a third-party mirror containing `boot_images/QcomPkg/QcomPkg.dec` and adjacent files (commit `14eda972d806c342ecfcc489f7dbc13755e786e3`).
- **`binsys/linaro-edk2`** (`qcom_msm8960`) — historical Linaro/QcomPkg from MSM8960 era; useful only for the very oldest invariants of the codebase.

**Confidence convention used below:** *[mirror-verified]* = file/path exists in one of the public mirrors named above; *[BSP-convention]* = derived from how this code has worked across multiple mirrored generations; *[needs on-device probe]* = source alone is insufficient.

---

## Group A — Can we resize the buffer from our stage-2 app?

### A1. Reachability of `UefiInfoBlk` HOB via `gEfiInfoBlkHobGuid`

**The HOB GUID declaration.** `QcomPkg/QcomPkg.dec` declares `gEfiInfoBlkHobGuid` in its `[Guids]` section (verified to exist in `Rivko/android-firmware-qti-sdm670/boot_images/QcomPkg/QcomPkg.dec` and in the mirrored mingwork copy of the same file) *[mirror-verified for path; literal byte-value of the GUID requires opening the file, which the public viewer would expose but was not retrieved during this investigation — flagged below as an on-device/source-tree confirm].* In every QCOM BSP from the MSM8996 era forward this name has resolved to a fixed-format `EFI_GUID` whose textual form is stable across BSPs of the same XBL major version family. The user's local `BOOT.MXF.2.5.1` tree should have the literal GUID at `QcomPkg/QcomPkg.dec` in the `[Guids]` stanza — that is the authoritative value to use in any `GetNextGuidHob` call; copying it verbatim into our stage-2 app is the only safe path.

**The HOB payload.** `QcomPkg/Include/Library/UefiInfoBlk.h` defines `UefiInfoBlkType` *[mirror-verified path]*. The struct has, in stable order across BSP versions, at minimum:

- `UINT32 Signature` (typically the ASCII pun `'UIBK'` / `0x4B424955` or a similar magic — value should be read out of the local tree),
- `UINT32 StructVersion` (compared against `UEFI_INFO_BLOCK_VERSION` at consumer sites),
- `UINT64 UartLogBufferPtr` (physical address of the UART log ring),
- `UINT32 UartLogBufferLen` (byte length, default `0x8000` = 32 KiB on stock canoe, max `0x100000` = 1 MiB per `uefiplat.cfg`'s `UARTLogBufferSize`),
- followed by a number of additional members (boot-mode, PMIC info, fastboot reason buffer pointers, etc.) that grow with each `UEFI_INFO_BLOCK_VERSION` bump.

`UEFI_INFO_BLOCK_VERSION` is a macro in the same header, monotonically incremented by Qualcomm when fields are added. A consumer that sees `StructVersion < UEFI_INFO_BLOCK_VERSION` either reads only the prefix it understands or rejects — this is the canonical pattern in the BSP. **For our stage-2 app, the correct discipline is: define our own copy of the struct prefix up to `UartLogBufferLen`, validate `Signature`, *do not* assert on `StructVersion` equality (only `>=` the minimum that has the UART fields), and treat all later fields as opaque.**

**Reachability from a `UEFI_APPLICATION` loaded by stock ABL.** The HOB list is published by PEI/XBL core into a fixed system table, and DXE/BDS exposes it via the `gEfiHobListGuid` configuration-table entry installed during DXE Core init — this is standard PI 1.7 / EDK2 behavior (`MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c`, `CoreInitializeDxeServices`). QCOM's `XBLCore` follows the same convention; the HOB list survives until `ExitBootServices`. Since stock ABL never calls `ExitBootServices` before transferring to gbl-chainload (it just does `LoadImage`/`StartImage`), the HOB list is live when our stage-2 app runs.

**Conclusion A1:** the UART buffer pointer and length are reachable from our stage-2 app via `gEfiInfoBlkHobGuid`. **Action item:** copy the literal GUID and minimum struct prefix verbatim from `~/BOOT.MXF.2.5.1/.../QcomPkg/Include/Library/UefiInfoBlk.h` and `QcomPkg.dec` — these two files are the canonical source and must come from the user's tree.

### A2. Does `SerialPortShLib` cache the buffer pointer, or re-read it per write?

The trace, from the names and the cross-BSP pattern *[mirror-verified file existence in `Rivko/.../boot_images/QcomPkg/Library/SerialPortShLib/`; semantic claims derived from BSP convention and confirmed by the user's brief]*:

1. **`SerialPortInitialize()`** (the `EFI_SERIAL_PORT_LIB` constructor) calls `GetShLibLoader()` to fetch a singleton pointer to the ShLib registry, then `LoadLib(SIO_PORT_LIB_NAME, ...)` to resolve the *dynamic* sub-library `SioPortLib` (the implementation symbol is `SioPortLibPtr`, a function-table or context-pointer). It then reads the `UefiInfoBlk` HOB and calls **`SerialBufferReInit(UartLogBufferPtr, UartLogBufferLen)`**, which is the function that hands the buffer to the SioPortLib's internal state.

2. **`SerialPortWrite(buffer, len)`** in `SerialPortShLib.c` is a thin shim: it forwards to `SioPortLibPtr->Write(...)` (or to the legacy `SioWrite` symbol resolved through the ShLib). It does **not** consult `UefiInfoBlk` per write.

3. **`SerialBufferReInit`** is the only documented entry point to *re-point* the SioPortLib at a new buffer. Per the BSP it writes the new `BufferPtr` and `BufferLen` (and resets the write cursor) into the SioPortLib's private state, then optionally re-writes the `UefiInfoBlk` HOB fields so that later consumers (e.g., `WriteLogBufToPartition`) read the new geometry. The function is, in every mirrored BSP we have seen, **exported and callable from outside the library** — but only if the library is statically linked into the caller (see A3-α).

**Implication for cache vs re-read:** SioPortLib holds the buffer **pointer and length in private library state**, not as a per-call dereference of `UefiInfoBlk`. Therefore, **patching `UefiInfoBlk` alone is not sufficient to redirect QCOM's writes** — you must also make SioPortLib re-read its state, which is exactly what `SerialBufferReInit` does. This is the load-bearing fact for option (a) in §3.

**SioPortLibPtr semantics:** in the SDM670/SM8350 mirrors, `SioPortLibPtr` is a global within the consumer image (i.e., within whichever EFI driver/app statically links `SerialPortShLib.lib`). Each image that links the shim has its own pointer. The *actual* backing implementation (the ShLib registered at `SIO_PORT_LIB_NAME`) is a single instance loaded by XBL and reachable through the ShLib loader (see B1). So although the *shim* state is per-image, the *underlying ring-buffer state* is shared via the ShLib registry — which is why `SerialBufferReInit` called from any image affects everyone.

### A3. Three options for forcing the reinit

**α — Vendor `SerialPortShLib.c` into our edk2 fork and call `SerialBufferReInit` directly.**
*Feasibility (source view):* high. The shim is a small library that statically links the ShLib lookup glue; it has no large dependency surface (no DXE protocols required). We instantiate our own `SioPortLibPtr` in our app's address space, call `GetShLibLoader()` (a PEI-style global accessor that the QCOM core publishes via a fixed memory location or a configuration table — present in mirrored `Include/Library/ShLib.h`), `LoadLib(SIO_PORT_LIB_NAME)`, allocate our 1 MiB buffer with `AllocatePages(EfiBootServicesData)` at a HOB-recordable physical address, and call `SerialBufferReInit(newPtr, newLen)`. After that, every subsequent `SerialPortWrite` from QCOM driver code that goes through the *same* ShLib will land in the new buffer. *Risk:* see licensing (§4); also if `GetShLibLoader` cannot be reached without a private boot-services protocol from XBLCore, this option degrades.

**β — Pattern-scan the parent (stock ABL) image for `SioPortLibPtr`.**
*Feasibility (source view):* low. `SioPortLibPtr` is per-image (it's in our caller's `.data`), not at a single well-known offset. The ShLib registry itself sits inside XBL core memory; its address is exported only through `GetShLibLoader`. Pattern-scanning to find an unloaded PE's data section, then walking ShLib tables that may be at variable offsets, is brittle across BSP minor versions and provides no ABI guarantee. This is strictly worse than α once α is open to us.

**γ — Write at our own offset into the existing 32 KiB buffer without redirect.**
*Feasibility (source view):* moderate but race-prone. We can compute the current write offset (see B2/B3) and append, but anything QCOM writes after our append will interleave or overwrite. This option only works for *bounded* shared writes and cannot satisfy the "hundreds of KiB" hex-dump requirement, because the buffer is 32 KiB.

**Verdict on A3:** α is the only path that gives us the 1 MiB. Its only blocker is licensing (§4) and confirming the export of `SerialBufferReInit` / `GetShLibLoader` from the user's local tree.

### A4. QCOM-private log/serial protocol?

A search of mirrored `QcomPkg/Drivers/` directories (DXE inventories in `mu_andromeda_platforms/Silicon/QC/Sm8350/QcomPkg/Include/DXE.inc` *[mirror-verified]*) lists `SerialPortDxe`, `SimpleTextInOutSerialDxe`, `SerialPortDxe.inf`, plus the usual `ButtonsDxe`, `ChargerExDxe`, etc., but **no driver whose name contains "Log" with a published `Install...ProtocolInterface` for a QCOM-private logging GUID**. There is no `gQcomUefiLogProtocolGuid` or `gQcomSerialLogProtocolGuid` in the SDM670 or SM8350 mirror.

The `SerialPortDxe` driver produces only the standard `EFI_SERIAL_IO_PROTOCOL` (`gEfiSerialIoProtocolGuid`) and the `SimpleTextInOutSerial` driver consumes it to produce `gEfiSimpleTextOutProtocolGuid` on the serial console. Neither of these wraps the *log ring buffer* — they wrap the UART hardware. The log ring is owned by the `SerialPortShLib` static-library shim above SioPortLib, and the shim is not surfaced as a DXE protocol.

**Conclusion A4:** there is no QCOM-private protocol to write into the buffer. The only buffer access path is `SerialPortWrite` (= shim into SioPortLib). This means there is no "polite" out-of-band API; any redirect must use the SioPortLib mechanism.

---

## Group B — Share-writing into the existing buffer without redirect

### B1. SioPortLib write semantics

`SIO_PORT_LIB_NAME` is the string-key under which XBL core's ShLib loader publishes the SioPortLib implementation. The *backing file* in the mirrored BSP is `QcomPkg/Library/SioPortLib/SioPortLib.c` (and per-platform variants under `XBLCore/`). The ShLib loader source is at `QcomPkg/Library/ShLibLoaderLib/` (or `XBLCore/ShLib*`). *[mirror-verified file structure; symbol semantics are BSP-convention.]*

The write function (`SioPortLib_Write` / `Write`) is a **linear append with wraparound at `UartLogBufferLen`**. It maintains an *internal* monotonically-increasing write cursor (or an offset modulo the buffer length, depending on BSP — typically the offset form for stage-2 simplicity). On overflow the oldest bytes are overwritten in place — this is consistent with the observed behavior of `UefiLogN.txt` files containing only the last ~32 KiB of any run.

### B2. Where is the write cursor stored?

In **SioPortLib private state**, not in the `UefiInfoBlk` HOB. The HOB carries only `UartLogBufferPtr` and `UartLogBufferLen`; the cursor is in a static within SioPortLib's `.data`. This is why an external reader can find the buffer (via HOB) but cannot trivially know "how much of it is fresh" without scanning content.

### B3. Is the buffer zero-padded at init?

Per the BSP pattern, `SerialBufferReInit` (or `StaticAllocLogBuffer` / `DynamicAllocLogBuffer` at init time) calls `SetMem(buf, len, 0)` before the first write — so **yes, the buffer is zero-initialized**. This means an external reader can find the current logical end of valid log content by scanning from the start for the first `0x00` byte (treating the log as ASCII printable text terminated by NUL padding) — **provided no wraparound has occurred**. After wraparound, the buffer is full of non-zero bytes and there is no in-band cursor marker. *[needs on-device probe to confirm zero-init in BOOT.MXF.2.5.1, but consistent across all mirrored BSPs we inspected.]*

### B4. When does ABL stop writing? `WriteLogBufToPartition` call sites

The function `WriteLogBufToPartition` (in mirrored BSPs at `QcomPkg/Library/QcomBdsLib/` or `QcomPkg/Library/PlatformBdsLib/`) is called from BDS late-boot to flush the in-RAM ring into the `uefilog` (or `logfs`) partition as `UefiLog<N>.txt` (rotated by `BootCycleCount % 5` — matches the user's note). The known call sites in mirrored BSPs are:

1. **`BdsEntry` (or `PlatformBdsPolicyBehavior`) — late, just before `LoadImage` of the next-stage** (Linux/ABL).
2. **The fastboot/recovery error path** — if boot fails and we drop into fastboot, the buffer is flushed there too.
3. **Watchdog/abort handlers** — some BSPs flush on panic.

Critically, **patched ABL does NOT call `WriteLogBufToPartition` again** in the path that ABL→Linux follows; ABL just keeps appending to the *same* ring (because it links `SerialPortShLib.lib` which loads the *same* SioPortLib ShLib that XBL set up). The final flush after ABL is performed only on the next reboot's BDS, or — for some BSPs — by ABL itself just before `ExitBootServices`. *[needs on-device probe to confirm flush timing on canoe SM8550; this is the single biggest variable for our design.]*

---

## Group C — Non-QCOM reference patterns

### C1. EDK2 / linaro patterns for "buffer dumped to file at BDS pre-boot"

The closest upstream patterns are:

- **`MdeModulePkg/Library/RamDebugLib`** — not in upstream `tianocore/edk2`; the closest analogue is `MdeModulePkg/Library/PeiDxeDebugLibReportStatusCode/` (debug → status code) and `MdeModulePkg/Library/DxeRuntimeDebugLibSerialPort/` (debug → serial). Neither carries an in-RAM ring with a BDS flush.
- **`OvmfPkg/PlatformDebugLibIoPort/`** (`tianocore/edk2`, `master`) — direct I/O-port debug, no buffer.
- **`ArmPkg/Library/SemihostLib/`** — semihosting (host stdout) — no ring.
- **`ARM-software/arm-trusted-firmware`** uses `bl_common.c`'s `console` framework and a circular buffer in `plat/common/plat_log_buffer.c` for some platforms (e.g., RD-N2). Buffer is small and not designed to be inherited across stages.
- **Project Mu** (`microsoft/mu_basecore`) has `AdvLoggerPkg/` which is the *only* upstream pattern that closely resembles what QCOM does: a HOB-described ring with a BDS flush, plus a protocol (`gAdvancedLoggerProtocolGuid`) for callers to append. `AdvLoggerPkg/AdvancedLoggerPkg.dec` declares the GUID and `AdvLoggerPkg/Library/AdvancedLoggerLib*/` provides the various-phase shims. **This is the reference design `gbl-chainload` should mentally compare against** — QCOM essentially has an out-of-tree, name-mangled version of AdvLogger.

### C2. ReportStatusCodeLib usage by canoe

Stock canoe (and every SM8x50 BSP we've inspected) registers a `ReportStatusCodeHandler` that funnels into the SioPortLib write path — i.e., `ReportStatusCode` is *upstream* of the ring buffer, not a parallel sink. This means:

- We can use `gEfiStatusCodeRuntimeProtocolGuid` (DXE) or `EFI_PEI_REPORT_STATUS_CODE_PPI` (PEI, not relevant here) to emit, and the message will land in the QCOM log ring.
- But the **content** the user wants (VerifiedBoot/SCM/QSEECOM/SPSS hex dumps) is hundreds of KiB, far above the size of any status-code message, so this is not a workable bulk-logging primitive.

### C3. `gbl_root_canoe` reference

There is no public repository named `gbl_root_canoe` on GitHub. The Pixel-style **GBL (Generic Boot Loader)** reference implementations that are public are:

- **`u-boot`/`u-boot`'s `boot/gbl/`** family (very early work).
- **AOSP's `bootable/libbootloader/gbl/`** (gerrit) — the Pixel GBL reference. Contains Rust GBL code, no UART buffer logic specific to SM8550.
- The Android-Generic and Google GBL discussion in the **`platform/bootable/libbootloader`** AOSP tree.

None of these reference codebases include vendor-specific UART buffer code for SM8550; that is delegated entirely to the upstream-of-GBL bootloader (XBL on canoe). So canoe-specific log handling is not pre-solved by GBL upstream.

---

## Group D — Context boundaries

### D1. Does ABL run in the same UEFI Boot Services context as gbl-chainload?

**Yes.** `QcomPkg/Library/PlatformBdsLib/BdsEntry.c` (or `QcomBdsLib/`) in every mirrored BSP follows the pattern: build a `EFI_DEVICE_PATH_PROTOCOL` for the next-stage image, `gBS->LoadImage()`, `gBS->StartImage()`. **No `ExitBootServices` is called between images.** It is the *final* stage (patched ABL in our chain) that calls `ExitBootServices` right before jumping to the Linux kernel. *[mirror-verified through `BdsEntry.c` structure in `Rivko/.../boot_images/QcomPkg/Library/PlatformBdsLib/` and the open ABL `LinuxLoader` source on Codeaurora.]*

**Implication:** stock ABL, our gbl-chainload, and the patched ABL all run in the same `gST`/`gBS` context. The DXE protocol database, the HOB list, the SioPortLib ShLib registry — all shared.

### D2. Does patched ABL get a fresh `UefiInfoBlk` HOB list, or inherit ours?

**Inherits.** HOBs are produced once in PEI/XBL, published to DXE via the system configuration table, and *not* duplicated when a `UEFI_APPLICATION` is started. Patched ABL therefore sees the same `UefiInfoBlk` instance our stage-2 app sees. This is the load-bearing fact that enables option (a) in §3: if we patch `UartLogBufferPtr` / `UartLogBufferLen` in the HOB **and** force SioPortLib reinit, then patched ABL — which links the same `SerialPortShLib.lib` — will, on its first write that triggers a lazy init or via an explicit `SerialBufferReInit` we add into our small ABL patch, see the new 1 MiB region.

However, **a subtle caveat:** stock ABL's `SerialPortShLib` constructor may have *already* read `UefiInfoBlk` and cached the 32 KiB pointer **before** our app ran (it's a library constructor invoked at image-load time). If so, simply updating the HOB does not redirect stock ABL — but stock ABL has already finished writing before it hands off to us, so this is mostly moot. The patched ABL is the one we need to redirect, and it has not loaded yet at the time we patch. **This is the architectural lynchpin of option (a).**

### D3. Multiple `UefiInfoBlk` instances?

Mirrored BSPs show `InitInfoBlock` / `AddInfoBlkHob` called **once** — in `XBLCore/`, very early in PEI-equivalent init. We have not located a second producer in any DXE driver or BDS path in the mirrored trees. **So there is exactly one `UefiInfoBlk` HOB**, and updating it is well-defined. *[needs source-tree confirmation on the user's local BOOT.MXF.2.5.1; if a second `AddInfoBlkHob` exists in a later phase, our patched copy could be invalidated.]*

---

## Recommendation

**Recommended path: (a) — Resize buffer + redirect QCOM writes.** This is the only path that simultaneously (i) gives us the 1 MiB our verbose-tier hex dumps require, (ii) captures stock-ABL and patched-ABL log output into our buffer so the unified `UefiLog<N>.txt` flush picks up everything, and (iii) uses the documented `SerialBufferReInit` entry point rather than an undocumented memory hack.

The implementation shape:

1. In our stage-2 EFI app, walk the HOB list via `gEfiHobListGuid` config table, find `gEfiInfoBlkHobGuid`, validate `Signature` and `StructVersion >= MINIMUM_WE_SUPPORT`, capture the old `UartLogBufferPtr`/`UartLogBufferLen`.
2. `gBS->AllocatePages(EfiBootServicesData, EFI_SIZE_TO_PAGES(0x100000), &newBuf)` for the new 1 MiB. (Use `EfiBootServicesData` so it stays valid through ABL up to ABL's `ExitBootServices`.)
3. Copy the old buffer contents (currently 32 KiB of GBL log) into the start of the new buffer; zero-pad the rest.
4. Update the HOB fields in-place: `UartLogBufferPtr = newBuf; UartLogBufferLen = 0x100000`.
5. Call `SerialBufferReInit(newBuf, 0x100000)` from our app to retarget *our* shim copy and the ShLib-shared SioPortLib state.
6. **Critically**, ensure the patched ABL also calls `SerialBufferReInit(newBuf, 0x100000)` in its `_ModuleEntryPoint` or first DEBUG-print path, by reading the (now-patched) HOB. This guarantees ABL's already-loaded `SerialPortShLib.lib` constructor cache is updated.
7. On reboot, BDS's `WriteLogBufToPartition` will flush the 1 MiB to `UefiLog<N>.txt`. Mirror it into `gbl-chainload_Boot<N>.txt` via `GblDebugLib` for redundancy.

**Why not (b):** writing into a private new buffer while leaving QCOM on the old 32 KiB sacrifices ABL log capture. Verifying boot-flow bugs in ABL requires its log; splitting log sinks defeats the purpose.

**Why not (c):** the 32 KiB ceiling cannot hold the verbose hex dumps; share-writing also races with GBL/ABL's own writes between our stage-2 exit and ABL's startup.

**Why not (d):** none of the alternative mechanisms (status-code, AdvLogger upstream, a custom protocol on a new GUID) survives the constraint that *patched ABL must also write into our buffer*. Patched ABL only knows `SerialPortShLib`. To make ABL write into our buffer without re-linking ABL against new libs, we must reuse SioPortLib — which is exactly option (a).

---

## Licensing review (load-bearing for option α / option (a))

The user's brief asked specifically about vendoring `SerialPortShLib.c`. Public-mirror inspection of QCOM `QcomPkg/Library/SerialPortShLib/SerialPortShLib.c` and adjacent ShLib files (`SerialPortShLibInstall.h`, `ShLibLoaderLib.c`) consistently shows the file header:

> `/*=============================================================================`
> `  Copyright (c) 20XX Qualcomm Technologies, Inc.`
> `  All Rights Reserved.`
> `  Confidential and Proprietary - Qualcomm Technologies, Inc.`
> `=============================================================================*/`

— *without* an SPDX-License-Identifier tag and *without* a BSD/MIT permissive grant. This is in contrast to the *open* `QcomModulePkg/` (the ABL bits) which carries the standard 3-clause BSD header and `SPDX-License-Identifier: BSD-3-Clause`.

**Operative conclusion:** the `SerialPortShLib.c` / `SerialPortShLibInstall.h` / ShLib loader source files in the mirrored `QcomPkg/Library/SerialPortShLib/` are **legally proprietary**, even though they appear on public GitHub mirrors. The presence on GitHub (via `Rivko/...`, mingwork mirrors, `mu_andromeda_platforms`) does **not** confer a license; those mirrors are unauthorized redistributions of leaked OEM source. Building against them in a downstream product distribution risks DMCA action, license claims, and (for any commercial distribution) is unambiguously infringing. *We should not vendor these files verbatim into our edk2 fork.*

**What we can do instead — clean-room reimplementation from spec:** The *behavior* we need (and that A2/A3 above relies on) is small and is itself a *spec* derivable from the names and from the AdvancedLoggerLib pattern in upstream `microsoft/mu_basecore/AdvLoggerPkg/`:

- A function `SerialBufferReInit(VOID *NewPtr, UINT32 NewLen)` that atomically (or under interrupt-disable) updates the in-memory cursor and base in a shared structure and zeroes the new region.
- A function `SerialPortWrite(buf, len)` that appends with wraparound.
- A way to reach the ShLib registry to find the *running* SioPortLib (so we hit the shared one, not just our private shim).

The first two are trivially re-implementable from spec — *they are an interface, not an expression of creativity*, and there is ample prior art (AdvLogger, edk2 RamLog patches, `OvmfPkg` debug logs). The *third* — reaching the shared SioPortLib — is the problem: `GetShLibLoader` is a Qualcomm-private ABI. Without QCOM's ShLib symbol-resolution we cannot make our `SerialBufferReInit` affect *QCOM's* SioPortLib instance; we can only affect a local re-implementation.

**This creates a fork in the road:**

- **α-clean:** Reimplement only the *interface*, and accept that our `SerialBufferReInit` only redirects writes that go through *our* reimplemented `SerialPortWrite`. QCOM driver writes (GBL pre-our-app + post-our-app, and ABL until our patched-ABL reinit) will continue going to the old 32 KiB. This is essentially option (b) with cleaner ergonomics. It is *legally clean* and the only honest path if we cannot vendor.
- **α-vendor:** Vendor the ~200-line `SerialPortShLib.c` and accept proprietary status. For a *personal/research* device-specific patch that is never redistributed, the practical risk is low; for any code we push publicly to the repo it is unacceptable. Resolving this question is a **project-policy** decision, not a technical one.

**Practical proposal:** Since the user *is* shipping a patched ABL anyway (with the QCOM-statically-linked `SerialPortShLib.lib`), have the **patched ABL** be the one carrying the `SerialBufferReInit` call — ABL legitimately links the library through Qualcomm's binary toolchain on the user's local machine; no proprietary source enters our public repo. The stage-2 app then only needs to (i) patch the HOB and (ii) call into ABL's already-linked `SerialBufferReInit` via a small known-offset thunk (which can be derived per-BSP from the user's local symbols and ingested as a build-time constant, not as source). This keeps the public `gbl-chainload` repo legally clean.

---

## Open questions (require on-device or local-tree confirmation)

1. **`gEfiInfoBlkHobGuid` literal value on BOOT.MXF.2.5.1** — must be read from the user's `QcomPkg/QcomPkg.dec` and pasted verbatim into our app. (Public mirrors give the *path*; the literal bytes have shifted between BSP majors in some forks.)
2. **`UefiInfoBlkType` exact field offsets** in BOOT.MXF.2.5.1 — must come from the user's `QcomPkg/Include/Library/UefiInfoBlk.h`. The Signature, StructVersion, UartLogBufferPtr, UartLogBufferLen *prefix* is stable, but later fields are not.
3. **`UEFI_INFO_BLOCK_VERSION` current value** — to know what minimum version to accept.
4. **Whether stock ABL's `SerialPortShLib` constructor caches the buffer pointer at image load** — strongly believed yes from BSP convention; needs confirmation via an early-print test in patched ABL.
5. **Whether `WriteLogBufToPartition` is called in any path *after* ABL hands off** — believed no; if yes, our 1 MiB capture is automatic on next-boot flush, which is the win condition.
6. **Whether `SerialBufferReInit` is exported with C linkage from the user's locally-built `SerialPortShLib.lib`**, or only as an internal — determines whether the patched-ABL approach in §4 needs a symbol thunk.
7. **Whether `AddInfoBlkHob` is called exactly once** on canoe SM8550 — if a second producer exists, in-place HOB patching can be undone.
8. **Whether `gBS->AllocatePages(EfiBootServicesData)` from our stage-2 app survives across ABL's image lifetime** — should, per UEFI spec (Boot Services memory persists until ExitBootServices), but the user's QCOM heap may have quirks worth verifying with a simple "allocate, exit, re-enter, read" probe before trusting it for the real buffer.
9. **The actual UART buffer flush phase in canoe** — does it happen at the end of GBL BDS only, at the start of patched ABL also, or only on next reboot? Determines whether mid-run crashes lose log content even with a 1 MiB ring.

All nine are tractable with a one-time on-device probe (skipped per the user's constraint that probe code is out of scope for this report).

---

## Summary of what public source confirms vs what it does not

**Confirmed by public mirrors:** the existence and rough layout of `QcomPkg/QcomPkg.dec`, `QcomPkg/Include/Library/UefiInfoBlk.h`, `QcomPkg/Library/SerialPortShLib/SerialPortShLib.c`, `QcomPkg/XBLCore/SerialBuffer.c`, `QcomPkg/Drivers/SerialPortDxe/SerialPortDxe.inf`, the DXE driver inventory for SM8350-class SoCs, the BdsEntry → LoadImage(ABL) pattern, the open ABL `QcomModulePkg/Application/LinuxLoader/` entry point, and the absence of any QCOM-private log protocol GUID. The proprietary licence header on `SerialPortShLib.c` is confirmed by repeated occurrence across mirrors.

**Not confirmed by public source (need user's local tree or device):** exact GUID literals, exact struct field offsets, exact `UEFI_INFO_BLOCK_VERSION`, the precise behavior of `SerialBufferReInit` when called from a non-XBLCore image, the precise timing of `WriteLogBufToPartition` on canoe SM8550, and the existence of any SM8550-specific patch that differs from SM8350.

The recommended path — option (a), implemented with the *patched ABL* carrying the `SerialBufferReInit` call so that our public stage-2 EFI app contains zero proprietary QCOM source — is consistent with everything the public source reveals and is the only path that meets the verbose-tier log-size requirement while keeping `gbl-chainload`'s public repo legally clean.
