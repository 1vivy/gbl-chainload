/** @file HookCommon.h
  Internal contract between InstallAll.c and the per-protocol hook
  source files. Each hook source exports one EFI_STATUS Install_<name>()
  that locates the protocol, swaps the chosen vtable slot with its
  own wrapper, and stashes the original pointer in a file-scope global
  for pass-through.

  ## Reentry guard (Phase A)

  Verbose hooks can fan out a log emit per call. Depending on the active
  build flags and firmware logging path, those emits can route through
  firmware services that eventually perform block I/O or SCM/QSEE traffic.
  If one of those internal calls lands back in our wrappers, the wrapper
  would recurse, log again, and either explode the stack or starve the log
  budget. Each hook source declares its own per-record reentry guard and
  wraps its body in `HookEnter`/`HookLeave`.
**/
#ifndef GBL_CHAINLOAD_HOOK_COMMON_H
#define GBL_CHAINLOAD_HOOK_COMMON_H

#include <Uefi.h>

EFI_STATUS InstallQseecomHook (VOID);
EFI_STATUS InstallScmHook (VOID);
EFI_STATUS InstallVerifiedBootHook (VOID);
EFI_STATUS InstallSpssHook (VOID);
EFI_STATUS InstallBlockIoHook (VOID);
EFI_STATUS InstallEbsHook (VOID);

/* ------------------------------------------------------------------
 * Reentry guard
 * ------------------------------------------------------------------ */

typedef struct {
  UINT32 Depth;             /* current call depth (>0 means we're inside a hooked call) */
  UINT32 MaxObservedDepth;  /* peak depth across the boot — logged once at exit */
  UINT64 TotalCalls;        /* count of HookEnter invocations */
} HOOK_REENTRY_GUARD;

/** Declare a static, file-scope reentry guard for a hook source.
    Use once near the top of each hook .c file:
        HOOK_REENTRY_DEFINE (gQseecomGuard);
**/
#define HOOK_REENTRY_DEFINE(name)                                   \
  STATIC HOOK_REENTRY_GUARD name = { 0, 0, 0 }

/** Increment the guard's depth. Returns TRUE on first entry (caller
    holds the slot — proceed with logging + pass-through). Returns FALSE
    on reentry (caller must skip logging and pass through silently to
    avoid recursive log explosion).

    Implementation note: UEFI Boot Services run on the boot CPU only —
    no SMP, no preemption — so plain reads/writes are sufficient.
    Compiler-builtin __atomic_* would emit calls to outline-atomic
    helpers (__aarch64_ldadd*) that EDK2 doesn't link against, and the
    BaseLib Interlocked* family only covers UINT32 and returns the new
    value. Plain non-atomic ops match the actual execution model.
**/
static inline BOOLEAN
HookEnter (
  IN OUT HOOK_REENTRY_GUARD  *Guard
  )
{
  UINT32 New;

  if (Guard == NULL) {
    return FALSE;
  }

  Guard->TotalCalls++;
  New = ++Guard->Depth;

  if (New > Guard->MaxObservedDepth) {
    Guard->MaxObservedDepth = New;
  }

  return (New == 1);
}

/** Decrement the guard's depth. Pair every HookEnter (TRUE or FALSE
    return) with exactly one HookLeave to balance the unconditional
    Depth increment. **/
static inline VOID
HookLeave (
  IN OUT HOOK_REENTRY_GUARD  *Guard
  )
{
  if (Guard == NULL) {
    return;
  }
  if (Guard->Depth > 0) {
    Guard->Depth--;
  }
}

#endif /* GBL_CHAINLOAD_HOOK_COMMON_H */
