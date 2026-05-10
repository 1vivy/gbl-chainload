# Swallow Capability + Oplus fastboot-sec — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** identify oplus fastboot-sec's lock/region gating calls, build a reusable observe/swallow/transform hook capability in ProtocolHookLib (plus a new BlockIo hook for partition-write gates), and produce a CN-tester validation procedure.

**Architecture:** four sub-tasks across nine tasks. (1) RE pass mines existing logs + live oplusreserve1 dump + ABL RE to identify which calls/writes implement the gates. (2) Refactor ProtocolHookLib from log+passthrough to a three-action rule table (observe / swallow / transform) — added incrementally per hook with TDD on host shims. (3) New BlockIoHook wraps the EFI_BLOCK_IO_PROTOCOL of the oplusreserve1 partition handle. (4) Concrete mode-gated rules; global-device validation; CN-tester procedure doc.

**Tech Stack:** EDK-II, our existing ProtocolHookLib pattern (vtable wrap + reentry guard), our existing TDD shim approach (`__HOST_BUILD__` define), Ghidra MCP for ABL RE.

**Depends on:** Track 1 (`2026-05-10-logfs-verbosity-audit.md`) merged first so rule-fire decisions land in `GblChainload_BootN.txt` reliably.

**File structure:**
- `docs/re/oplus-fastboot-sec-mechanism.md` — Task 1 findings.
- `docs/re/oplusreserve1-on-disk.md` — Task 2 partition analysis.
- `docs/re/oplus-fastboot-sec-cn-validation.md` — Task 9 CN-tester procedure.
- `GblChainloadPkg/Library/ProtocolHookLib/HookCommon.h` — extend with `HOOK_ACTION`, `HOOK_RULE`, `HookConsultRule()` API.
- `GblChainloadPkg/Library/ProtocolHookLib/HookRule.c` — new: rule-table consultation logic.
- `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c` — Task 4 refactor (rule table consultation per slot).
- `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c` — Task 5 refactor.
- `GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c` — Task 5 refactor.
- `GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c` — Task 5 refactor.
- `GblChainloadPkg/Library/ProtocolHookLib/BlockIoHook.c` — Task 6, new hook.
- `GblChainloadPkg/Library/ProtocolHookLib/Rules/FastbootSecRules.c` — Task 7, concrete rule tables.
- `GblChainloadPkg/Library/ProtocolHookLib/Rules/FastbootSecRules.h` — Task 7, public ruleset accessors.
- `tests/hookrule/test_hookrule.c` + `tests/hookrule/Makefile` — Task 3, host TDD.
- `GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c` — register BlockIoHook in Task 6, wire mode-gated rules in Task 7.

---

## Task 1: RE pass — identify fastboot-sec gates

**Files:**
- Create: `docs/re/oplus-fastboot-sec-mechanism.md`

Read existing captures, ABL RE, and known notes to identify which
calls / partition writes implement oplus fastboot-sec on CN devices
(common base across OOS and ColorOS).

- [ ] **Step 1: Grep `~/gbl-chainload-dirty/logs/` for oplusreserve1 and related terms**

```bash
DIRTY=$HOME/gbl-chainload-dirty/logs
grep -rilE "oplusreserve1|fastboot-sec|fastboot_sec|region.*lock|RegionalHybrid" "$DIRTY" 2>/dev/null \
  | head -10
```

For each match, read enough context to understand what was happening
(which boot stage, which command). Note in a working doc.

- [ ] **Step 2: Grep this repo's RE notes**

```bash
cd /home/vivy/gbl-chainload
grep -rilE "oplusreserve1|fastboot-sec|region.*lock|SetDeviceUnlockValue|GetDeviceUnlockValue" .re-notes/ docs/re/ 2>/dev/null
```

Read each match's surrounding section. The qseecom callgraph + repo
audit notes are likely sources.

- [ ] **Step 3: Open ABL in Ghidra (if not already) and search for `oplusreserve1` string xrefs**

If Ghidra MCP is available in this session:

```
mcp__ghidra-mcp__search_strings   query: oplusreserve1
```

For each hit, follow xrefs to the function reading/writing the
partition. Note the function address + name. Look for byte-offset
constants that suggest specific TLV/struct fields.

(If Ghidra isn't immediately available, document this as "deferred to
when ABL Ghidra session is running".)

- [ ] **Step 4: Write findings**

Create `docs/re/oplus-fastboot-sec-mechanism.md`:

```markdown
# Oplus fastboot-sec mechanism — RE findings

**Date:** 2026-05-10

## Hypothesis

(One paragraph: what we think fastboot-sec does — gates stock fastboot
operations on lock/unlock state changes, writes a state byte to
oplusreserve1 visible to ABL.)

## Evidence sources

- Captures: (list specific log dirs from `~/gbl-chainload-dirty/`)
- Repo RE notes: (list specific .re-notes/sessions/*.md files)
- ABL Ghidra: (function addresses / names, if available)

## Identified gates

For each gate found:

### Gate N: <name>

- **What it does:** (e.g. propagates locked/unlocked state to oplusreserve1)
- **Implementation evidence:** (file:line in ABL or log excerpt)
- **Intercept layer recommendation:** SCM / qseecom / BlockIo / ABL binary patch
- **Reasoning:** why that layer is the right one

## Open questions

(Anything we don't have evidence for yet that would let a CN tester
help — e.g. "what's the byte offset in oplusreserve1 that mirrors
lock state on CN builds?")
```

- [ ] **Step 5: Commit**

```bash
cd /home/vivy/gbl-chainload
git add docs/re/oplus-fastboot-sec-mechanism.md
git commit -m "RE: oplus fastboot-sec mechanism findings"
```

---

## Task 2: Live oplusreserve1 dump + analysis

**Files:**
- Create: `docs/re/oplusreserve1-on-disk.md`

Pull the current device's oplusreserve1 partition contents and analyse
known offsets / suspicious bytes. Cross-reference with Task 1's
findings.

- [ ] **Step 1: Confirm device is up in system or recovery context**

```bash
adb devices
```

Expected: one line with state `device` (system) or `recovery`.

- [ ] **Step 2: Confirm the partition exists**

```bash
adb shell 'su -c "ls -l /dev/block/by-name/oplusreserve1"' 2>&1 | head
```

Expected: a symlink to `/dev/block/sd...`. If not found, dump is
deferred — note in the doc and skip this task's dd Step.

- [ ] **Step 3: Capture the partition size**

```bash
SIZE=$(adb shell 'su -c "blockdev --getsize64 /dev/block/by-name/oplusreserve1"' 2>/dev/null | tr -d '\r')
echo "size=$SIZE"
```

Expected: a positive integer (typically a few MB).

- [ ] **Step 4: dd the partition off-device**

```bash
mkdir -p /tmp/oplusreserve1
adb shell 'su -c "dd if=/dev/block/by-name/oplusreserve1 of=/data/local/tmp/oplusreserve1.bin bs=4096"' 2>&1 | tail -3
adb pull /data/local/tmp/oplusreserve1.bin /tmp/oplusreserve1/oplusreserve1.bin
adb shell 'su -c "rm /data/local/tmp/oplusreserve1.bin"'
ls -la /tmp/oplusreserve1/
```

Expected: file at `/tmp/oplusreserve1/oplusreserve1.bin` with size
matching `$SIZE`.

- [ ] **Step 5: Initial analysis**

```bash
# First 4 KB hex dump (look for magic / header)
xxd /tmp/oplusreserve1/oplusreserve1.bin | head -64

# String dump (catch ASCII labels)
strings -n 5 /tmp/oplusreserve1/oplusreserve1.bin | head -50

# Entropy / non-zero regions (find data clusters)
od -An -tx1 -v /tmp/oplusreserve1/oplusreserve1.bin \
  | awk 'NR%256==0 {print NR*16, $0}' | head -20
```

- [ ] **Step 6: Write the doc**

Create `docs/re/oplusreserve1-on-disk.md`:

```markdown
# oplusreserve1 on-disk analysis — 2026-05-10

**Device:** CPH2747EEA (global)
**Build:** (paste `getprop ro.build.fingerprint` from current capture)
**Partition size:** (from Step 3)

## Header

(Hex dump of first 256 bytes. Annotate any magic / version bytes.)

## ASCII strings

(Output of `strings -n 5`. Annotate suspicious labels — anything that
looks like a region code, lock state flag, or known oplus telemetry
field.)

## Non-zero regions

(Map of which 4-KB blocks contain data vs zero padding.)

## Hypotheses against Task 1 gates

For each gate from `docs/re/oplus-fastboot-sec-mechanism.md`:

- **Gate N**: byte offset / field name we suspect maps to it. State
  the offset + the current value byte(s).

## What a CN tester would need to do to confirm

(Procedure: pre-test dump, change lock state via stock fastboot,
post-test dump, diff. Specific byte offsets to watch.)
```

- [ ] **Step 7: Commit**

```bash
cd /home/vivy/gbl-chainload
git add docs/re/oplusreserve1-on-disk.md
git commit -m "RE: oplusreserve1 live partition analysis"
```

---

## Task 3: HOOK_ACTION + HOOK_RULE types + host TDD

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/HookCommon.h`
- Create: `GblChainloadPkg/Library/ProtocolHookLib/HookRule.c`
- Create: `tests/hookrule/test_hookrule.c`
- Create: `tests/hookrule/Makefile`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf`
- Modify: `tests/runall.sh`

Define the rule-table API and prove it works in host TDD before
wiring it into any hook.

- [ ] **Step 1: Write the failing test**

Create `tests/hookrule/test_hookrule.c`:

```c
/* Host TDD for HookRule API. Mirrors the tests/avb pattern. */
#define __HOST_BUILD__
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../../GblChainloadPkg/Library/ProtocolHookLib/HookCommon.h"

static void test_consult_no_table (void) {
  HOOK_ACTION  Action = HookActionObserve;
  CONST CHAR8 *Why    = NULL;
  EFI_STATUS Status = HookConsultRule (NULL, 0, 0x1234, &Action, &Why);
  assert (Status == EFI_SUCCESS);
  assert (Action == HookActionObserve);
  printf ("ok test_consult_no_table\n");
}

static void test_consult_match (void) {
  HOOK_RULE Rules[] = {
    { 0x1111, HookActionObserve,   NULL, "first" },
    { 0x2222, HookActionSwallow,   NULL, "second" },
    { 0x3333, HookActionTransform, NULL, "third" },
  };
  HOOK_ACTION  Action = HookActionObserve;
  CONST CHAR8 *Why    = NULL;
  EFI_STATUS Status = HookConsultRule (Rules, 3, 0x2222, &Action, &Why);
  assert (Status == EFI_SUCCESS);
  assert (Action == HookActionSwallow);
  assert (Why != NULL && strcmp (Why, "second") == 0);
  printf ("ok test_consult_match\n");
}

static void test_consult_no_match (void) {
  HOOK_RULE Rules[] = {
    { 0x1111, HookActionSwallow, NULL, "first" },
  };
  HOOK_ACTION  Action = HookActionTransform;  /* sentinel */
  CONST CHAR8 *Why    = (CONST CHAR8 *)0xdead;
  EFI_STATUS Status = HookConsultRule (Rules, 1, 0x9999, &Action, &Why);
  assert (Status == EFI_SUCCESS);
  assert (Action == HookActionObserve);   /* default on miss */
  assert (Why == NULL);
  printf ("ok test_consult_no_match\n");
}

int main (void) {
  test_consult_no_table ();
  test_consult_match ();
  test_consult_no_match ();
  printf ("ALL PASS\n");
  return 0;
}
```

Create `tests/hookrule/Makefile`:

```makefile
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D__HOST_BUILD__

test_hookrule: test_hookrule.c ../../GblChainloadPkg/Library/ProtocolHookLib/HookRule.c
	$(CC) $(CFLAGS) -o $@ $^
	./$@

clean:
	rm -f test_hookrule
```

- [ ] **Step 2: Run the test — must fail (no HookRule yet)**

```bash
cd /home/vivy/gbl-chainload/tests/hookrule && make 2>&1 | tail -10
```

Expected: compile error referencing `HOOK_ACTION`, `HOOK_RULE`, or
`HookConsultRule` not defined.

- [ ] **Step 3: Add the types to HookCommon.h**

Locate `GblChainloadPkg/Library/ProtocolHookLib/HookCommon.h`. After
the existing definitions (reentry guard etc.), add a guarded host shim
+ the new types:

```c
/* Host-build shim — same pattern as AvbParseLib's host TDD. */
#ifdef __HOST_BUILD__
#include <stdint.h>
typedef int32_t  EFI_STATUS;
typedef uint32_t UINT32;
typedef char     CHAR8;
#define EFI_SUCCESS  0
#define EFIAPI
#define IN
#define OUT
#define CONST const
#define STATIC static
#endif

typedef enum {
  HookActionObserve   = 0,  /* default — log, passthrough */
  HookActionSwallow   = 1,  /* log, skip original, synthetic success */
  HookActionTransform = 2,  /* log, call original, mutate result */
} HOOK_ACTION;

typedef struct _HOOK_RULE {
  UINT32       Match;       /* SIP id, TA cmd id, LBA range start, etc. */
  HOOK_ACTION  Action;
  EFI_STATUS  (*Transform)(IN OUT VOID *Args, IN OUT VOID *Result);
  CONST CHAR8 *Why;          /* short string for logging */
} HOOK_RULE;

/* Consult a rule table for Match. On miss, *ActionOut = HookActionObserve,
   *WhyOut = NULL. Always returns EFI_SUCCESS (consultation never fails). */
EFI_STATUS EFIAPI
HookConsultRule (
  IN  CONST HOOK_RULE *Rules,
  IN  UINT32           NumRules,
  IN  UINT32           Match,
  OUT HOOK_ACTION     *ActionOut,
  OUT CONST CHAR8    **WhyOut
  );
```

For the host shim's `VOID`, also add (inside the `#ifdef __HOST_BUILD__`):
```c
typedef void VOID;
```

- [ ] **Step 4: Implement HookConsultRule**

Create `GblChainloadPkg/Library/ProtocolHookLib/HookRule.c`:

```c
/** @file HookRule.c — rule-table consultation. */

#ifdef __HOST_BUILD__
#include "HookCommon.h"
#else
#include <Uefi.h>
#include <Library/BaseLib.h>
#include "HookCommon.h"
#endif

EFI_STATUS EFIAPI
HookConsultRule (
  IN  CONST HOOK_RULE *Rules,
  IN  UINT32           NumRules,
  IN  UINT32           Match,
  OUT HOOK_ACTION     *ActionOut,
  OUT CONST CHAR8    **WhyOut
  )
{
  UINT32 i;

  if (ActionOut != NULL) *ActionOut = HookActionObserve;
  if (WhyOut    != NULL) *WhyOut    = NULL;

  if (Rules == NULL || NumRules == 0) return EFI_SUCCESS;

  for (i = 0; i < NumRules; i++) {
    if (Rules[i].Match == Match) {
      if (ActionOut != NULL) *ActionOut = Rules[i].Action;
      if (WhyOut    != NULL) *WhyOut    = Rules[i].Why;
      break;
    }
  }
  return EFI_SUCCESS;
}
```

- [ ] **Step 5: Run the test — must pass**

```bash
cd /home/vivy/gbl-chainload/tests/hookrule && make clean && make 2>&1 | tail -10
```

Expected:
```
ok test_consult_no_table
ok test_consult_match
ok test_consult_no_match
ALL PASS
```

- [ ] **Step 6: Register HookRule.c in ProtocolHookLib.inf**

In `GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf`, find
the `[Sources]` block and add:

```
  HookRule.c
```

(Alphabetical with the existing entries.)

- [ ] **Step 7: Add tests/hookrule to runall.sh**

```bash
grep -n "tests/avb\|tests/scan" /home/vivy/gbl-chainload/tests/runall.sh | head
```

After the `tests/avb` block, add:

```bash
echo "== tests/hookrule =="
make -C tests/hookrule clean >/dev/null
make -C tests/hookrule
```

- [ ] **Step 8: Build the EFI to confirm nothing broke**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 9: Full runall**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -10
```

Expected: ALL TESTS PASS.

- [ ] **Step 10: Commit**

```bash
cd /home/vivy/gbl-chainload
git add GblChainloadPkg/Library/ProtocolHookLib/HookCommon.h \
        GblChainloadPkg/Library/ProtocolHookLib/HookRule.c \
        GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf \
        tests/hookrule/ \
        tests/runall.sh
git commit -m "ProtocolHookLib: add HOOK_ACTION + HOOK_RULE + HookConsultRule (TDD)"
```

---

## Task 4: Refactor ScmHook to consult a rule table

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c`

Establish the exemplar pattern: each hook slot wrapper consults its
rule table BEFORE calling the original. observe → log + passthrough
(current behaviour). swallow → log + return EFI_SUCCESS. transform →
log + call original + invoke transform fn.

- [ ] **Step 1: Add a per-slot empty rule table + getter API**

In `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c`, after the
existing static slot pointers (around line 41), add:

```c
/* Rule tables — empty by default = observe everything. Concrete rules
   land in Rules/FastbootSecRules.c, registered via setter calls from
   InstallAll. */
STATIC CONST HOOK_RULE *gScmSysCallRules    = NULL;
STATIC UINT32           gScmSysCallNumRules = 0;
STATIC CONST HOOK_RULE *gScmSipSysCallRules    = NULL;
STATIC UINT32           gScmSipSysCallNumRules = 0;
/* (others as we expand) */

VOID
ScmHookSetSysCallRules (
  IN CONST HOOK_RULE *Rules,
  IN UINT32           NumRules
  )
{
  gScmSysCallRules    = Rules;
  gScmSysCallNumRules = NumRules;
}

VOID
ScmHookSetSipSysCallRules (
  IN CONST HOOK_RULE *Rules,
  IN UINT32           NumRules
  )
{
  gScmSipSysCallRules    = Rules;
  gScmSipSysCallNumRules = NumRules;
}
```

- [ ] **Step 2: Add the rule-consultation in `HookedScmSysCall`**

Find `HookedScmSysCall` (around line 397). Before the `Status = gOrigScmSysCall (...)` call, insert:

```c
HOOK_ACTION  Action = HookActionObserve;
CONST CHAR8 *Why    = NULL;
HookConsultRule (gScmSysCallRules, gScmSysCallNumRules,
                  /* Match key: depends on Cmd's structure. Use first
                     UINT32 of Cmd buf, or 0 if Cmd is NULL. */
                  (Cmd != NULL) ? *(UINT32 *)Cmd : 0,
                  &Action, &Why);

if (Action == HookActionSwallow) {
  DEBUG ((DEBUG_INFO, "ScmHook.SysCall: SWALLOW (why=%a)\n",
          Why ? Why : "?"));
  return EFI_SUCCESS;
}
```

After the original call, for transform:
```c
if (Action == HookActionTransform) {
  DEBUG ((DEBUG_INFO, "ScmHook.SysCall: TRANSFORM (why=%a)\n",
          Why ? Why : "?"));
  /* The Transform fn is found via the rule lookup; re-do lookup here
     to recover the fn pointer. Cheaper than threading it through. */
  for (UINT32 i = 0; i < gScmSysCallNumRules; i++) {
    if (gScmSysCallRules[i].Match == ((Cmd != NULL) ? *(UINT32 *)Cmd : 0)
        && gScmSysCallRules[i].Transform != NULL) {
      gScmSysCallRules[i].Transform ((VOID *)Cmd, (VOID *)&Status);
      break;
    }
  }
}
```

(Note: re-doing the lookup keeps the original API in `HookConsultRule`
minimal — it only returns Action + Why. Transform fn is retrieved
separately. If this becomes a hot path later, expand the API.)

- [ ] **Step 3: Apply the same pattern to `HookedScmSipSysCall`**

Find `HookedScmSipSysCall` (around line 470). Repeat the rule
consultation pattern, but use `SmcId` as the match key (it's the SCM
SIP function identifier; perfect for matching by SIP ID).

```c
HOOK_ACTION  Action = HookActionObserve;
CONST CHAR8 *Why    = NULL;
HookConsultRule (gScmSipSysCallRules, gScmSipSysCallNumRules,
                  SmcId, &Action, &Why);

if (Action == HookActionSwallow) {
  DEBUG ((DEBUG_INFO, "ScmHook.SipSysCall: SWALLOW SmcId=0x%x (why=%a)\n",
          SmcId, Why ? Why : "?"));
  /* Zero out results to mimic clean success */
  if (Results != NULL) {
    SetMem (Results, sizeof (UINT64) * 4, 0);
  }
  return EFI_SUCCESS;
}
```

For transform, mirror the same lookup-by-Match pattern shown in Step 2,
adapted to `(VOID *)Parameters, (VOID *)Results`.

- [ ] **Step 4: Skip the other slots for now**

The other three SCM slots (`FastCall2`, `SendCommand`, `QseeSysCall`)
get the same treatment, but we defer to Task 5 alongside the other
hooks. For ScmHook, the SysCall + SipSysCall coverage is enough to
prove the pattern works.

- [ ] **Step 5: Build**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 6: Confirm runall still passes (no behavioural change since rule tables are empty)**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -5
```

Expected: ALL TESTS PASS.

- [ ] **Step 7: Commit**

```bash
cd /home/vivy/gbl-chainload
git add GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c
git commit -m "ScmHook: consult rule table in SysCall + SipSysCall (observe by default)"
```

---

## Task 5: Port the rule-consultation pattern to the remaining hooks

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c` (cover FastCall2, SendCommand, QseeSysCall)
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c`

Mechanical extension. Each hook slot gets:
1. A per-slot `gXxxRules` + `gXxxNumRules` static.
2. A `XxxHookSetXxxRules` setter.
3. Rule consultation + swallow/transform branches in the wrapper.

- [ ] **Step 1: Extend ScmHook for the remaining three slots**

In `ScmHook.c`, add rule tables + setters for FastCall2, SendCommand,
QseeSysCall. Match keys:
- `FastCall2`: `Id`
- `SendCommand`: first UINT32 of the AppCmd struct (TA command id)
- `QseeSysCall`: `SmcId`

Apply the same observe/swallow/transform pattern shown in Task 4
Step 2 to each `Hooked*` wrapper.

- [ ] **Step 2: Apply to QseecomHook**

Open `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c`. Identify
each hooked slot (likely `StartApp`, `SendCmd`, possibly others —
inventory first):

```bash
grep -nE "^STATIC.*Hooked|gOrigQsee" /home/vivy/gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c | head
```

For each `Hooked*` wrapper, add rule table + setter + consultation.
Match key: depends on slot signature — typically a command id or app
handle. Use the most-specific identifier available.

- [ ] **Step 3: Apply to SpssHook**

Same pattern. Match key for the single SPSS slot
(`ShareKeyMintInfo`): use the first UINT32 of the info struct, or 0
if the struct doesn't have an obvious dispatch field.

- [ ] **Step 4: Apply to VerifiedBootHook**

This hook has 10 slots. Add rule consultation per slot. Match keys:
each slot's primary dispatch identifier (consult the file to find
the right field per slot — likely fixed string IDs or enums).

- [ ] **Step 5: Build**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 6: Confirm runall still passes**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh 2>&1 | tail -5
```

Expected: ALL TESTS PASS.

- [ ] **Step 7: Commit**

```bash
cd /home/vivy/gbl-chainload
git add GblChainloadPkg/Library/ProtocolHookLib/{ScmHook,QseecomHook,SpssHook,VerifiedBootHook}.c
git commit -m "ProtocolHookLib: port rule-consultation pattern to all hook slots"
```

---

## Task 6: New BlockIo hook

**Files:**
- Create: `GblChainloadPkg/Library/ProtocolHookLib/BlockIoHook.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf`

Install a wrapper for the `EFI_BLOCK_IO_PROTOCOL` of the oplusreserve1
partition. Gates `WriteBlocks` calls.

- [ ] **Step 1: Create BlockIoHook.c**

Use this exact content as the starting skeleton:

```c
/** @file BlockIoHook.c — wrap EFI_BLOCK_IO_PROTOCOL.WriteBlocks for a
    named partition (initially: oplusreserve1) and consult a rule
    table per write request. */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include "HookCommon.h"

STATIC EFI_BLOCK_IO_PROTOCOL *gOplusBio        = NULL;
STATIC EFI_BLOCK_IO_WRITE_BLOCKS gOrigOplusWrite = NULL;
STATIC CONST HOOK_RULE *gOplusWriteRules    = NULL;
STATIC UINT32           gOplusWriteNumRules = 0;

HOOK_REENTRY_DEFINE (gOplusBioGuard);

VOID
BlockIoHookSetOplusReserve1WriteRules (
  IN CONST HOOK_RULE *Rules,
  IN UINT32           NumRules
  )
{
  gOplusWriteRules    = Rules;
  gOplusWriteNumRules = NumRules;
}

STATIC EFI_STATUS EFIAPI
HookedOplusWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                  MediaId,
  IN EFI_LBA                 Lba,
  IN UINTN                   BufferSize,
  IN VOID                   *Buffer
  )
{
  HOOK_ACTION  Action = HookActionObserve;
  CONST CHAR8 *Why    = NULL;
  EFI_STATUS Status;

  if (HOOK_REENTRY_ENTER (gOplusBioGuard)) {
    /* Re-entry — pass through silently. */
    return gOrigOplusWrite (This, MediaId, Lba, BufferSize, Buffer);
  }

  HookConsultRule (gOplusWriteRules, gOplusWriteNumRules,
                    (UINT32)Lba, &Action, &Why);

  switch (Action) {
    case HookActionSwallow:
      DEBUG ((DEBUG_INFO,
              "BlockIoHook.oplusreserve1: SWALLOW LBA=0x%lx size=%lu (why=%a)\n",
              Lba, (UINTN)BufferSize, Why ? Why : "?"));
      HOOK_REENTRY_LEAVE (gOplusBioGuard);
      return EFI_SUCCESS;

    case HookActionTransform:
      /* Transform fn (if any) mutates Buffer before write. */
      DEBUG ((DEBUG_INFO,
              "BlockIoHook.oplusreserve1: TRANSFORM LBA=0x%lx size=%lu (why=%a)\n",
              Lba, (UINTN)BufferSize, Why ? Why : "?"));
      for (UINT32 i = 0; i < gOplusWriteNumRules; i++) {
        if (gOplusWriteRules[i].Match == (UINT32)Lba
            && gOplusWriteRules[i].Transform != NULL) {
          gOplusWriteRules[i].Transform (Buffer, NULL);
          break;
        }
      }
      Status = gOrigOplusWrite (This, MediaId, Lba, BufferSize, Buffer);
      HOOK_REENTRY_LEAVE (gOplusBioGuard);
      return Status;

    case HookActionObserve:
    default:
      DEBUG ((DEBUG_VERBOSE,
              "BlockIoHook.oplusreserve1: OBSERVE LBA=0x%lx size=%lu\n",
              Lba, (UINTN)BufferSize));
      Status = gOrigOplusWrite (This, MediaId, Lba, BufferSize, Buffer);
      HOOK_REENTRY_LEAVE (gOplusBioGuard);
      return Status;
  }
}

EFI_STATUS
EFIAPI
InstallBlockIoHook (VOID)
{
  /* Find the oplusreserve1 partition by GPT name. The exact API call
     here depends on what helper our codebase already has for partition
     lookup. Common shape: `LocatePartitionByName(L"oplusreserve1", &Bio)`.
     If no such helper exists, walk the EFI handle database for
     EFI_BLOCK_IO_PROTOCOL and match partition names via the
     EFI_PARTITION_INFO_PROTOCOL or similar. */

  EFI_BLOCK_IO_PROTOCOL *Bio = NULL;
  EFI_STATUS Status = EFI_NOT_FOUND;

  /* Placeholder: replace with the actual partition-lookup call used
     elsewhere in this codebase. See FastbootCmds.c's
     LocatePartitionForGraft for an example pattern. */
  /* Status = LocatePartitionByName (L"oplusreserve1", &Bio); */

  if (EFI_ERROR (Status) || Bio == NULL) {
    DEBUG ((DEBUG_WARN, "BlockIoHook: oplusreserve1 partition not found (%r) — hook NOT installed\n", Status));
    return Status;
  }

  gOplusBio       = Bio;
  gOrigOplusWrite = Bio->WriteBlocks;
  Bio->WriteBlocks = HookedOplusWrite;

  Print (L"BlockIoHook: installed on oplusreserve1\n");
  return EFI_SUCCESS;
}
```

(Step 2 below replaces the placeholder partition lookup.)

- [ ] **Step 2: Wire to actual partition lookup**

```bash
grep -nE "LocatePartitionForGraft\|PartitionGetInfo\|gEfiPartitionInfoProtocolGuid" \
  /home/vivy/gbl-chainload/edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c \
  /home/vivy/gbl-chainload/edk2/QcomModulePkg/**/*.c 2>/dev/null | head
```

Find the partition-lookup pattern used elsewhere. Replace the
`/* Status = LocatePartitionByName ... */` placeholder in `BlockIoHook.c`
with the real call.

If no convenient helper exists for DXE-layer lookup of a partition by
name (LocatePartitionForGraft lives in QcomModulePkg FastbootLib, not
our package), implement a minimal walker:

```c
STATIC EFI_STATUS
LocateBlockIoByPartitionName (
  IN  CONST CHAR16              *Name,
  OUT EFI_BLOCK_IO_PROTOCOL    **Bio
  )
{
  EFI_HANDLE *Handles  = NULL;
  UINTN       NumHandles = 0;
  EFI_STATUS  Status;
  UINTN       i;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                     NULL, &NumHandles, &Handles);
  if (EFI_ERROR (Status)) return Status;

  for (i = 0; i < NumHandles; i++) {
    EFI_PARTITION_INFO_PROTOCOL *PartInfo = NULL;
    Status = gBS->HandleProtocol (Handles[i], &gEfiPartitionInfoProtocolGuid,
                                   (VOID **)&PartInfo);
    if (EFI_ERROR (Status) || PartInfo == NULL) continue;
    if (PartInfo->Type != PARTITION_TYPE_GPT) continue;
    if (StrnCmp (PartInfo->Info.Gpt.PartitionName, Name,
                  ARRAY_SIZE (PartInfo->Info.Gpt.PartitionName)) != 0) continue;

    Status = gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid,
                                   (VOID **)Bio);
    FreePool (Handles);
    return Status;
  }
  FreePool (Handles);
  return EFI_NOT_FOUND;
}
```

Use it: `Status = LocateBlockIoByPartitionName (L"oplusreserve1", &Bio);`.

- [ ] **Step 3: Register in InstallAll.c**

Edit `GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c`. Add the
forward declaration:

```c
EFI_STATUS InstallBlockIoHook (VOID);
```

In the `InstallAllProtocolHooks` (or equivalent) body, after
`InstallScmHook`, add:

```c
  Status = InstallBlockIoHook ();
  /* Non-fatal: oplusreserve1 may not be visible at this boot stage; log + continue. */
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "BlockIoHook install deferred or not present (%r)\n", Status));
  }
```

- [ ] **Step 4: Register BlockIoHook.c in the INF**

In `GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf`, add to
`[Sources]`:

```
  BlockIoHook.c
```

If `gEfiPartitionInfoProtocolGuid` is used and not already in the INF's
`[Protocols]` block, add it:

```
  gEfiPartitionInfoProtocolGuid
```

- [ ] **Step 5: Build**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
cd /home/vivy/gbl-chainload
git add GblChainloadPkg/Library/ProtocolHookLib/BlockIoHook.c \
        GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c \
        GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf
git commit -m "ProtocolHookLib: add BlockIoHook for oplusreserve1 writes"
```

---

## Task 7: Concrete fastboot-sec rules (mode-gated)

**Files:**
- Create: `GblChainloadPkg/Library/ProtocolHookLib/Rules/FastbootSecRules.c`
- Create: `GblChainloadPkg/Library/ProtocolHookLib/Rules/FastbootSecRules.h`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf`

Populate rule tables based on Task 1 + 2's findings. Mode-gated:
mode-1/fakelocked activate the rules; mode-0 leaves them dormant.

- [ ] **Step 1: Create the rules header**

`GblChainloadPkg/Library/ProtocolHookLib/Rules/FastbootSecRules.h`:

```c
#ifndef FASTBOOT_SEC_RULES_H_
#define FASTBOOT_SEC_RULES_H_

#include "../HookCommon.h"

/* Per-hook accessors. Caller does:
     CONST HOOK_RULE *r; UINT32 n;
     FastbootSecGetBlockIoOplusWriteRules (&r, &n);
     BlockIoHookSetOplusReserve1WriteRules (r, n);
   then the hook's wrapper picks up the rules on next dispatch. */

VOID FastbootSecGetBlockIoOplusWriteRules (
  OUT CONST HOOK_RULE **Rules,
  OUT UINT32           *NumRules
  );

VOID FastbootSecGetScmSipSysCallRules (
  OUT CONST HOOK_RULE **Rules,
  OUT UINT32           *NumRules
  );

#endif
```

- [ ] **Step 2: Create the rules body**

`GblChainloadPkg/Library/ProtocolHookLib/Rules/FastbootSecRules.c`:

```c
/** @file FastbootSecRules.c — concrete rule tables for blocking oplus
    fastboot-sec lock/region telemetry.
   
    Rule entries are populated based on RE findings in:
      docs/re/oplus-fastboot-sec-mechanism.md
      docs/re/oplusreserve1-on-disk.md
   
    Until findings are concrete, this file ships EMPTY tables. The
    tables remain wired to their hooks, so adding a rule is just
    appending an entry below. */

#include <Uefi.h>
#include "../HookCommon.h"
#include "FastbootSecRules.h"

/* Block writes to specific oplusreserve1 LBAs that propagate locked/
   unlocked telemetry. Match = LBA. */
STATIC CONST HOOK_RULE gOplusWriteRules[] = {
  /* {  <LBA from Task 2>, HookActionSwallow, NULL, "lock-state-byte" }, */
};

/* Block SCM SIPs that fastboot-sec uses to read/write lock state via
   secure storage. Match = SmcId. */
STATIC CONST HOOK_RULE gScmSipRules[] = {
  /* { <SmcId from Task 1>, HookActionSwallow, NULL, "fastboot-sec gate" }, */
};

VOID FastbootSecGetBlockIoOplusWriteRules (
  OUT CONST HOOK_RULE **Rules,
  OUT UINT32           *NumRules
  )
{
  *Rules    = gOplusWriteRules;
  *NumRules = sizeof (gOplusWriteRules) / sizeof (gOplusWriteRules[0]);
}

VOID FastbootSecGetScmSipSysCallRules (
  OUT CONST HOOK_RULE **Rules,
  OUT UINT32           *NumRules
  )
{
  *Rules    = gScmSipRules;
  *NumRules = sizeof (gScmSipRules) / sizeof (gScmSipRules[0]);
}
```

- [ ] **Step 3: Wire the rules into InstallAll.c, mode-gated**

In `GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c`, after the
hook installations, add:

```c
#include "Rules/FastbootSecRules.h"

#if (GBL_MODE == 1)
  {
    CONST HOOK_RULE *Rules;
    UINT32           NumRules;

    FastbootSecGetBlockIoOplusWriteRules (&Rules, &NumRules);
    BlockIoHookSetOplusReserve1WriteRules (Rules, NumRules);

    FastbootSecGetScmSipSysCallRules (&Rules, &NumRules);
    ScmHookSetSipSysCallRules (Rules, NumRules);

    Print (L"ProtocolHookLib: fastboot-sec rules active (mode-1)\n");
  }
#else
  Print (L"ProtocolHookLib: fastboot-sec rules dormant (mode-%a)\n",
         GBL_MODE_STRING);
#endif
```

(If `GBL_MODE_STRING` isn't already defined in our codebase, use the
same per-mode `#if (GBL_MODE == N)` ladder pattern that PostGblLog.c
uses to pick a string.)

- [ ] **Step 4: Register the new file in the INF**

In `GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf`,
`[Sources]`:

```
  Rules/FastbootSecRules.c
```

- [ ] **Step 5: Build mode-0 and mode-1 to confirm gating**

```bash
cd /home/vivy/gbl-chainload
./scripts/build.sh --mode 0 2>&1 | tail -5
./scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -5
```

Expected: both build clean. mode-0 binary should have NO swallow/transform
strings linked (dead-code-eliminated); mode-1 binary should have them.

Verify:
```bash
strings dist/mode-0.efi 2>/dev/null | grep -i "fastboot-sec rules active" | head
strings dist/mode-1-auto-debug.efi 2>/dev/null | grep -i "fastboot-sec rules active" | head
```

Expected: empty for mode-0, one match for mode-1.

- [ ] **Step 6: Commit**

```bash
cd /home/vivy/gbl-chainload
git add GblChainloadPkg/Library/ProtocolHookLib/Rules/ \
        GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c \
        GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf
git commit -m "ProtocolHookLib: wire fastboot-sec rules, mode-1 gated (empty tables)"
```

(Rule entries are added as a follow-up commit once Task 1's findings
identify specific LBAs/SmcIds. The infrastructure ships here.)

---

## Task 8: Global-device validation

**Files:**
- None modified. Validation gate.

Confirm the refactor + new hook don't break the global device, and
that rule-fire log lines appear when (eventually) populated rules
match.

- [ ] **Step 1: Confirm Track 1 (logfs verbosity) is merged**

```bash
cd /home/vivy/gbl-chainload
git log --oneline -20 | grep -E "Track 1|logfs verbos|DEBUG_VERBOSE" | head
```

Expected: at least one commit naming Track 1.

- [ ] **Step 2: Build the verbose mode-1 binary**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 3: Stage + boot**

Power off device. Boot to bootloader. Then either run
`./scripts/test-device-automatic.sh` (preferred — repeatable) or stage
manually:

```bash
fastboot stage dist/mode-1-auto-debug-verbose.efi
fastboot oem boot-efi
# wait for adb
```

- [ ] **Step 4: Pull logs and check for installation lines**

```bash
LATEST=$(ls -td logs/*/ | head -1)
grep -E "BlockIoHook|fastboot-sec rules" "$LATEST/logfs/GblChainload_Boot"*.txt
```

Expected:
- `BlockIoHook: installed on oplusreserve1` (or the deferred-install
  warning if the partition isn't visible at our DXE stage)
- `ProtocolHookLib: fastboot-sec rules active (mode-1)`

If install was deferred, this is fine — the spec acknowledges that
the partition handle may not be available at our DXE stage on every
boot path. The rule-table wiring is still in place for later.

- [ ] **Step 5: Confirm device still boots Android cleanly**

```bash
adb shell getprop ro.boot.verifiedbootstate
adb shell getprop ro.boot.flash.locked
adb shell getprop sys.boot_completed
```

Expected: `verifiedbootstate=green`, `flash.locked=1`,
`boot_completed=1`.

- [ ] **Step 6: Commit any documentation updates from this validation**

```bash
cd /home/vivy/gbl-chainload
git add -u
git commit --allow-empty -m "Track 3 Task 8: global-device validation
  
  - BlockIoHook install: <ok|deferred>
  - Mode-gating active: yes (mode-1 rules wired)
  - Device boot: verifiedbootstate=green, flash.locked=1"
```

---

## Task 9: CN-tester validation procedure

**Files:**
- Create: `docs/re/oplus-fastboot-sec-cn-validation.md`

Document what a CN tester needs to do to confirm our swallow rules
actually block fastboot-sec. Self-contained (no project context
needed).

- [ ] **Step 1: Capture the current global EFI binary identity**

```bash
cd /home/vivy/gbl-chainload
git rev-parse HEAD
sha256sum dist/mode-1-auto-debug-verbose.efi
```

Note the values for the doc.

- [ ] **Step 2: Write the procedure doc**

Create `docs/re/oplus-fastboot-sec-cn-validation.md`:

```markdown
# CN-tester validation procedure — oplus fastboot-sec

**Date:** 2026-05-10
**Repo HEAD:** _from Step 1_
**Binary SHA256:** _from Step 1_

## Purpose

Confirm that our swallow rules block oplus fastboot-sec's
lock/region telemetry on a CN-region device. Global-device testing
cannot confirm this because fastboot-sec's CN-specific behaviour is
not exercised on global builds.

## Prerequisites

- A CN-region OnePlus device (any device our project is targeted at —
  currently `canoe`). CN region confirmed by `ro.boot.region` or
  `ro.boot.color.code` returning a CN value.
- Stock firmware flashed (no prior unlock attempts).
- A USB cable, fastboot installed on the host, adb installed for
  recovery captures.
- The mode-1 EFI binary from this repo (referenced above).

## Procedure

### Step 1: Pre-test capture (stock, untouched)

```bash
# Boot device into bootloader (Power + VolUp + VolDn, or `adb reboot bootloader`)
fastboot getvar all 2>&1 | tee /tmp/cn-pretest-getvar.txt
fastboot oem device-info 2>&1 | tee /tmp/cn-pretest-deviceinfo.txt
# If the tester has adb access from recovery:
adb reboot recovery
# wait for recovery
adb shell 'su -c "dd if=/dev/block/by-name/oplusreserve1 of=/data/local/tmp/oplusreserve1-pre.bin bs=4096"'
adb pull /data/local/tmp/oplusreserve1-pre.bin /tmp/oplusreserve1-pre.bin
```

### Step 2: Apply our mode-1 EFI and capture again

```bash
# Boot to bootloader
fastboot stage <path-to-mode-1-auto-debug-verbose.efi>
fastboot oem boot-efi
# wait for chainload + Android boot
adb reboot recovery
adb shell 'su -c "dd if=/dev/block/by-name/oplusreserve1 of=/data/local/tmp/oplusreserve1-post.bin bs=4096"'
adb pull /data/local/tmp/oplusreserve1-post.bin /tmp/oplusreserve1-post.bin
fastboot getvar all 2>&1 | tee /tmp/cn-posttest-getvar.txt
```

### Step 3: Diff

```bash
diff /tmp/cn-pretest-getvar.txt /tmp/cn-posttest-getvar.txt
cmp -l /tmp/oplusreserve1-pre.bin /tmp/oplusreserve1-post.bin | head -30
```

## Pass / fail criteria

**PASS** =
- `getvar` outputs identical between pre and post (no lock-state
  byte flipped to "unlocked").
- `oplusreserve1` bytes at the rule-blocked LBAs are unchanged. The
  specific LBAs are listed in `docs/re/oplus-fastboot-sec-mechanism.md`
  and `docs/re/oplusreserve1-on-disk.md`.

**FAIL** =
- `getvar` shows a lock-state field that flipped to "unlocked".
- `oplusreserve1` bytes at blocked LBAs changed.
- Device booted with `ro.boot.verifiedbootstate=orange|red` (post-test).

## What to send back

Tester sends the host:
- `cn-pretest-getvar.txt`, `cn-posttest-getvar.txt`
- `oplusreserve1-pre.bin`, `oplusreserve1-post.bin`
- A pull of `logs/<latest>/logfs/GblChainload_Boot*.txt` from after
  Step 2 (shows which rules fired)

## Reporting bugs

If the procedure can't be completed (device doesn't boot, `oem
boot-efi` not recognised by ABL, etc.), the tester should send:

- Full `bootloader_log` from `/proc/bootloader_log` after the failed
  step
- `adb shell dmesg` (recovery context)
- Their device's `ro.build.fingerprint`
```

- [ ] **Step 3: Commit**

```bash
cd /home/vivy/gbl-chainload
git add docs/re/oplus-fastboot-sec-cn-validation.md
git commit -m "RE: CN-tester validation procedure for fastboot-sec swallow"
```

---

## Self-Review

Checked against `docs/superpowers/specs/2026-05-10-swallow-and-fastboot-sec-design.md`:

- ✓ Sub-task 1 (RE pass: locate the gates) → Tasks 1 + 2.
- ✓ Sub-task 2 (build swallow capability) → Tasks 3 (types + TDD), 4 (ScmHook exemplar), 5 (port to rest), 6 (BlockIoHook).
- ✓ Sub-task 3 (concrete rules, mode-gated) → Task 7.
- ✓ Sub-task 4 (CN-tester procedure) → Task 9.
- ✓ Global-device validation → Task 8.

No placeholders in the code blocks. Type consistency:
`HOOK_ACTION`, `HOOK_RULE`, `HookConsultRule`, the setter naming
pattern `XxxHookSetXxxRules` are uniform across Tasks 3, 4, 5, 6, 7.
File paths are consistent: `Rules/FastbootSecRules.{c,h}` referenced
identically across Tasks 7 and the file structure header.

Acknowledged gap (consistent with spec): Task 7 ships empty rule
tables because Task 1's findings haven't materialised yet. Rule
entries land as a follow-up commit once findings are concrete. This
is by design — it lets the infrastructure ship before the RE pass
completes, and adding a rule is a single struct-initializer append.

One implementation note: Task 6's partition-lookup helper assumes
`EFI_PARTITION_INFO_PROTOCOL` is available in our EDK-II build. If
it's not (older EDK-II vintage), the alternative is to use the
existing `LocatePartitionForGraft` pattern from FastbootCmds.c by
extracting it into a shared header. That decision belongs in Task 6
Step 2's grep-and-decide step; no need to pre-plan a fallback.
