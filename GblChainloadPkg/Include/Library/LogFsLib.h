/** @file LogFsLib.h
  Mount the device's `logfs` partition (located by GPT label) and provide
  a clean post-GBL log file. Pre-GBL UefiLog1.txt is rotated to
  UefiLogSaved{0..4}.txt across boots so BDS log history is preserved.
  Our own post-GBL output goes to gbl-chainload_Boot{0..4}.txt — a separate
  file that doesn't get corrupted by the existing UefiLog volume / format.

  See REWRITE_PLAN.md §3.5.

  Step 2 of bring-up: just enough to mount + write a startup banner to
  the post-GBL log. The DEBUG/Print interception sink lives in step 3.
**/
#ifndef GBL_CHAINLOAD_LOGFSLIB_H
#define GBL_CHAINLOAD_LOGFSLIB_H

#include <Uefi.h>

/** Private error-level bit for "logfs-only" output. EDK2's standard
    levels occupy 0x80000000 (ERROR), 0x00800000 (MANAGEABILITY) and
    most bits in between; 0x10000000 is unused by MdePkg. QCOM stock
    code in our edk2 fork doesn't use it either — PartitionTableUpdate
    et al. use DEBUG_VERBOSE (0x00400000) which we keep filtered out
    of PcdDebugPrintErrorLevel.

    Modules with high-volume traces (AblUnwrap's per-section LZMA
    decode, future patch-engine per-byte spew, etc.) emit at this
    level. When GBL_VERBOSE=1 the DSC widens
    PcdDebugPrintErrorLevel to include this bit, so the line passes
    the EDK2 gate, reaches the sink hook, and the runtime
    gGblScreenMask (which never includes this bit) drops the ConOut
    leg — leaving the line in gbl-chainload_BootN.txt only. Under
    GBL_VERBOSE=0 the bit is masked out at the gate and the call is
    a no-op at runtime. **/
#define GBL_DBG_LOGFS_ONLY  0x10000000

/** Mount logfs, rotate UefiLog1.txt, open gbl-chainload_BootN.txt, and
    write an identifying banner to it. Caller may call LogFsWrite()
    afterwards to append more output.

    @retval EFI_SUCCESS    logfs mounted, post-GBL log file open + banner written
    @retval EFI_NOT_FOUND  logfs partition absent — caller should warn on console
    @retval other          partition lookup / mount / file-open errors propagated
**/
EFI_STATUS
EFIAPI
LogFsInit (VOID);

/** Append `Len` bytes from `Buf` to the post-GBL log file. No-op if
    LogFsInit failed or wasn't called.

    Durability: dirty bytes accumulate across calls and the implementation
    auto-flushes once the accumulated total reaches an internal threshold
    (currently 4 KiB). Callers about to hand control off to another image
    or fastboot loop must call `LogFsFlush()` explicitly to guarantee
    bytes hit the underlying SimpleFS — the canoe BDS SimpleFS only
    commits at ExitBootServices, which the fastboot fallback path never
    reaches. See `memory/active_investigation_log_flush.md` for context. */
EFI_STATUS
EFIAPI
LogFsWrite (
  IN CONST CHAR8 *Buf,
  IN UINTN Len
  );

/** Write `Len` bytes from `Data` to a fresh file at the logfs root
    named `FileName` (e.g. L"\\gbl-chainload_Boot0_fdt.bin"). Creates
    the file if it doesn't exist; overwrites if it does. Used by EBS
    hooks to dump captured FDT / bootconfig blobs for offline analysis
    rather than spamming chunked DEBUG lines. **/
EFI_STATUS
EFIAPI
LogFsWriteBlob (
  IN CONST CHAR16 *FileName,
  IN CONST VOID   *Data,
  IN UINTN         Len
  );

/** Flush any buffered writes. No-op if logfs not mounted. */
EFI_STATUS
EFIAPI
LogFsFlush (VOID);

/** Close the post-GBL log + the logfs root volume. Call before any
    chain-load handoff so writes are durable. */
EFI_STATUS
EFIAPI
LogFsClose (VOID);

/** TRUE if LogFsInit succeeded — handy for callers that want to gate
    optional rotation/banner work. */
BOOLEAN
EFIAPI
LogFsIsReady (VOID);

/** Install a wrapper around gST->ConOut->OutputString that mirrors all
    Print()/DEBUG() output to the post-GBL log file in addition to the
    console. No-op if LogFs isn't mounted. */
EFI_STATUS
EFIAPI
LogFsInstallDebugSink (VOID);

/** Restore the original gST->ConOut->OutputString. Call before chain-load
    handoff so the next-stage image gets a clean console table. */
EFI_STATUS
EFIAPI
LogFsRemoveDebugSink (VOID);

/** Set the bitmask of DEBUG() error-levels that the screen sink is
    allowed to mirror onto ConOut. Levels not in the mask are dropped
    from the screen but still written to gbl-chainload_BootN.txt.
    Bare Print() (no level recorded) ignores this mask and always
    reaches the screen — callers use Print() for failures and user-
    interrupt acks that must always be visible.

    Default at install time is DEBUG_ERROR. Production callers leave
    it alone; debug builds widen it via this entry point. **/
VOID
EFIAPI
LogFsSetScreenMask (
  IN UINTN  Mask
  );

#endif /* GBL_CHAINLOAD_LOGFSLIB_H */
