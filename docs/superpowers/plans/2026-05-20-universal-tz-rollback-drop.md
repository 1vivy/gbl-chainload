# Universal TZ rollback-bump drop — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Promote the existing `DROP-CANDIDATE(NOT-DROPPED)` scaffold for the two TZ anti-rollback SCM SIPs into a live universal drop, mirroring the established `TZ_BLOW_SW_FUSE_ID` short-circuit pattern.

**Architecture:** A 3-case switch in `UniversalBaseline.c::UniversalPolicy_ShouldDropScmSip` covers all three SmcIds; the `ScmHook` dispatcher already short-circuits via the existing call site, so no dispatcher change is needed. The mode-2 design doc deferred-audit row closes as part of the same change.

**Tech Stack:** EDK2 / UEFI C (Tianocore conventions), `GBL_INFO` logging via `GblLog.h`, on-device verification via fastboot `stage` + `oem boot-efi` (per CLAUDE.md safety boundary).

**Spec:** `docs/superpowers/specs/2026-05-20-universal-tz-rollback-drop-design.md`

---

## File Structure

- `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.h` — add two new SmcId `#define`s, widen the doc-comment on `UniversalPolicy_ShouldDropScmSip` to "soft-fuse blow and TZ rollback bumps".
- `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c` — convert the single-case check to a switch covering all three SmcIds; emit `GBL_INFO ... DROPPED (universal)` per case.
- `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c` — flip the `DROP-CANDIDATE(NOT-DROPPED)` log strings in the rollback decoder cases to `DROPPED (universal)` for self-consistency. (Decoder runs *after* the universal short-circuit, so these cases will not fire on the drop path — purely cosmetic and a hedge against future direct-call paths.)
- `docs/superpowers/specs/2026-05-17-mode-2-design.md` — replace the §7 deferred-audit paragraph and the appendix table row with a one-line cross-reference to the new spec.

No new files. No build-system changes. No EDK `.inf` updates (`UniversalBaseline.{c,h}` are already listed in `ProtocolHookLib.inf`).

---

### Task 1: Extend `UniversalPolicy_ShouldDropScmSip` to drop both TZ rollback SmcIds + close mode-2 deferred row

**Goal:** All three drops live in one switch; `DROPPED (universal)` log lines fire on real hardware; mode-2 design doc no longer says "deferred audit" for this row.

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.h`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c` (log strings only — no behavior change)
- Modify: `docs/superpowers/specs/2026-05-17-mode-2-design.md`

**Acceptance Criteria:**
- [ ] `UniversalPolicy_ShouldDropScmSip` returns `TRUE` with `*FakeStatus = EFI_SUCCESS` for `0x02000801`, `0x0200011E`, and `0x32000110`; returns `FALSE` for any other SmcId.
- [ ] Each drop emits exactly one `GBL_INFO` line of the form `scm-sip | smcid=0x%08x(<NAME>) | DROPPED (universal)\n` with the SmcId formatted as `0x%08x`.
- [ ] `ScmHook.c` no longer contains the substring `DROP-CANDIDATE(NOT-DROPPED)`.
- [ ] `docs/superpowers/specs/2026-05-17-mode-2-design.md` §7 anti-rollback bullet replaced with a one-line cross-reference; appendix table row reads `drop universally — UniversalBaseline.c`.
- [ ] EDK2 build for the existing infiniti target succeeds with no new warnings (`./build.sh` exits 0 and produces `dist/gbl-chainload-mode1.efi` and `dist/gbl-chainload-mode2.efi`).

**Verify:**
1. `grep -n "DROP-CANDIDATE" GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c` → no output.
2. `grep -n "0x0200011E\|0x32000110" GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c` → both constants present.
3. `grep -n "deferred audit (§7)" docs/superpowers/specs/2026-05-17-mode-2-design.md` → no output.
4. `./build.sh --mode 1 && ./build.sh --mode 2` → exit 0, both EFIs in `dist/`.

**Steps:**

- [ ] **Step 1: Update `UniversalBaseline.h`**

Replace the header-comment + declaration with:

```c
/** @file UniversalBaseline.h — universal-mode hook policy declarations.
    These run on every GBL_MODE; the per-mode overlay layers on top.
**/
#ifndef UNIVERSAL_BASELINE_H_
#define UNIVERSAL_BASELINE_H_

#include <Uefi.h>
#include "HookCommon.h"

/* SCM policy: drop soft-fuse-blow and TZ anti-rollback bumps. */

/** If SmcId is one of the universally-dropped SCM SIPs
    (TZ_BLOW_SW_FUSE_ID, TZ_UPDATE_ROLLBACK_VERSION_ID, or
    TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID),
    returns TRUE and writes EFI_SUCCESS into FakeStatus; caller
    short-circuits without forwarding the SMC.  Returns FALSE for any
    other SmcId — caller proceeds normally. **/
BOOLEAN
UniversalPolicy_ShouldDropScmSip (
  IN  UINT32       SmcId,
  OUT EFI_STATUS  *FakeStatus
  );

#endif
```

- [ ] **Step 2: Update `UniversalBaseline.c` — switch covering all three SmcIds**

Replace the file body with:

```c
/** @file UniversalBaseline.c — universal-mode hook policy implementation.

    These policies are unconditional. The universal contract drops:
      - TZ_BLOW_SW_FUSE_ID (soft-fuse advance — keeps the device's
        tamper fuse un-blown across boots).
      - TZ_UPDATE_ROLLBACK_VERSION_ID and
        TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID
        (TZ-side anti-rollback floor bumps — once advanced, the
        RPMB-stored floor would refuse subsequent boots of older
        os_version/patchlevel and on Qcom KM stacks that funnels the
        user toward a /data factory reset; see
        docs/superpowers/specs/2026-05-20-universal-tz-rollback-drop-design.md).

    KM 0x207 SET_VERSION is intentionally NOT dropped — KM-side
    os_version/patchlevel mismatches return KM_ERROR_KEY_REQUIRES_UPGRADE
    and are handled silently by Keystore via upgradeKey().

    Mode-specific overlays (Mode1Overlay.c, etc.) layer on top; they are
    called from slot wrappers before or after these universal checks as
    appropriate.
**/

#include "UniversalBaseline.h"
#include <Library/DebugLib.h>
#include <Library/GblLog.h>

/* Match the SCM SIPs decoded in ScmHook.c. */
#define SCM_SIP_TZ_BLOW_SW_FUSE_ID         0x02000801U
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER     0x0200011EU
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB  0x32000110U

BOOLEAN
UniversalPolicy_ShouldDropScmSip (
  IN  UINT32       SmcId,
  OUT EFI_STATUS  *FakeStatus
  )
{
  switch (SmcId) {
    case SCM_SIP_TZ_BLOW_SW_FUSE_ID:
      *FakeStatus = EFI_SUCCESS;
      GBL_INFO ("scm-sip | smcid=0x%08x(TZ_BLOW_SW_FUSE_ID) | DROPPED (universal)\n",
                SmcId);
      return TRUE;

    case SCM_SIP_TZ_UPDATE_ROLLBACK_VER:
      *FakeStatus = EFI_SUCCESS;
      GBL_INFO ("scm-sip | smcid=0x%08x(TZ_UPDATE_ROLLBACK_VERSION_ID)"
                " | DROPPED (universal)\n",
                SmcId);
      return TRUE;

    case SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB:
      *FakeStatus = EFI_SUCCESS;
      GBL_INFO ("scm-sip | smcid=0x%08x"
                "(TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID)"
                " | DROPPED (universal)\n",
                SmcId);
      return TRUE;
  }
  return FALSE;
}
```

- [ ] **Step 3: Flip `ScmHook.c` decoder log strings**

In `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c`, in the `DecodeSipSmcId` function, locate the two rollback cases and update only the log strings (no logic change).

For `case SCM_SIP_TZ_UPDATE_ROLLBACK_VER:` (currently around line 361), change:

```c
      GBL_INFO ("scm-sip | smcid=0x%08x(TZ_UPDATE_ROLLBACK_VERSION_ID)"
                " | p0=0x%llx | DROP-CANDIDATE(NOT-DROPPED) | st=%r\n",
                SmcId, P0, Status);
```

to:

```c
      GBL_INFO ("scm-sip | smcid=0x%08x(TZ_UPDATE_ROLLBACK_VERSION_ID)"
                " | p0=0x%llx | DROPPED (universal) | st=%r\n",
                SmcId, P0, Status);
```

For `case SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB:` (currently around line 370), change:

```c
      GBL_INFO ("scm-sip | smcid=0x%08x"
                "(TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID)"
                " | p0=0x%llx | DROP-CANDIDATE(NOT-DROPPED) | st=%r\n",
                SmcId, P0, Status);
```

to:

```c
      GBL_INFO ("scm-sip | smcid=0x%08x"
                "(TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID)"
                " | p0=0x%llx | DROPPED (universal) | st=%r\n",
                SmcId, P0, Status);
```

Also update the trailing-comment annotations: in the same two cases, change `* DROP CANDIDATE for re-flashability — see gbl_root` to `* DROPPED universally — see UniversalBaseline.c (and` (keep the rest of the comment intact). For the `#define` block near lines 279-280, change:

```c
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER     0x0200011Eu  /* DROP CANDIDATE */
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB  0x32000110u  /* DROP CANDIDATE */
```

to:

```c
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER     0x0200011Eu  /* dropped universally — see UniversalBaseline.c */
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB  0x32000110u  /* dropped universally — see UniversalBaseline.c */
```

And in the leading-comment ladder (lines ~268-272), change the two `— DROP CANDIDATE for re-flashability (gbl_root KM_BLOCK_TZ_ROLLBACK)` / `— DROP CANDIDATE same reason` lines to `— dropped universally (see UniversalBaseline.c)`.

- [ ] **Step 4: Close mode-2 design doc deferred-audit row**

In `docs/superpowers/specs/2026-05-17-mode-2-design.md`, replace lines 188-191 (the `**Anti-rollback / version-set SCM SmcIds — deferred audit.**` bullet) with:

```
- **Anti-rollback / version-set SCM SmcIds — closed.** Dropped universally
  by `UniversalBaseline.c`; see
  `docs/superpowers/specs/2026-05-20-universal-tz-rollback-drop-design.md`.
```

And replace line 230 (the appendix table row):

```
| | anti-rollback / version-set SmcIds | deferred audit (§7) |
```

with:

```
| | anti-rollback / version-set SmcIds | drop universally — UniversalBaseline.c |
```

- [ ] **Step 5: Static verification**

Run:

```bash
grep -n "DROP-CANDIDATE" GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c
grep -n "deferred audit (§7)" docs/superpowers/specs/2026-05-17-mode-2-design.md
grep -n "0x0200011E\|0x32000110" GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c
```

Expected: first two produce no output; third prints two lines (one per constant).

- [ ] **Step 6: Build mode-1 and mode-2 EFIs**

Run:

```bash
./build.sh --mode 1
./build.sh --mode 2
ls -la dist/*.efi
```

Expected: both invocations exit 0; `dist/gbl-chainload-mode1.efi` and `dist/gbl-chainload-mode2.efi` present with reasonable mtimes. No new compiler warnings (compare stderr against a baseline build of the parent commit if any new `warning:` lines appear).

- [ ] **Step 7: Commit on a fresh feature branch off `main`**

Per CLAUDE.md, landing on `main` is via PR. Create a branch:

```bash
git checkout main
git pull --ff-only
git checkout -b universal-tz-rollback-drop
git add GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.h \
        GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c \
        GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c \
        docs/superpowers/specs/2026-05-17-mode-2-design.md \
        docs/superpowers/plans/2026-05-20-universal-tz-rollback-drop.md \
        docs/superpowers/plans/2026-05-20-universal-tz-rollback-drop.md.tasks.json
git commit -m "feat(hook): drop TZ anti-rollback SmcIds universally

Extend UniversalBaseline.c to short-circuit
TZ_UPDATE_ROLLBACK_VERSION_ID (0x0200011E) and
TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID
(0x32000110) in addition to TZ_BLOW_SW_FUSE_ID. Pins the RPMB-stored
rollback floor and avoids the userdata-factory-reset funnel after older
OS images. KM 0x207 SET_VERSION untouched (Keystore upgradeKey() flow
intact).

Closes the deferred-audit row in the mode-2 design doc."
```

> Note: the design-doc commit (`spec: universal TZ rollback-bump drop`) already exists on the current branch. Cherry-pick it onto the new branch (or rebase) before pushing — the spec and the implementation should ship together.

---

### Task 2: On-device verification on infiniti

**USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Goal:** Confirm on real hardware (infiniti, the only available test device per the KM-hidden-set memory) that (a) the `DROPPED (universal)` log line fires for the actual rollback SmcId infiniti uses, (b) Android boots normally under both mode-1 and mode-2, (c) keystore-backed functionality survives a power cycle, and (d) `/data` survives.

**Files:** none (verification only; evidence captured into a note under `.re-notes/sessions/`).

**Acceptance Criteria:**
- [ ] After staging the new `dist/gbl-chainload-mode1.efi` and running `fastboot oem boot-efi`, the on-screen log shows a line matching `scm-sip | smcid=0x32000110(TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID) | DROPPED (universal)` at least once during boot. (If only `0x0200011E` fires instead, that is equally acceptable and indicates the legacy path — record which one was observed.)
- [ ] Android boots fully to lockscreen under mode-1 (no boot loop, no recovery fallback).
- [ ] Repeat under mode-2 (`dist/gbl-chainload-mode2.efi`): Android boots fully, mode-2 spoof line and the `DROPPED (universal)` line both appear.
- [ ] After full boot, exercise a keystore-backed flow (fingerprint unlock, or open a Strongbox-using app — Google Pay / banking app / `keystore-cli` test). Result: succeeds, no `KM_ERROR_*` in `logcat -b system | grep -i keymaster`.
- [ ] Power-cycle the device once more (without re-staging — boot from the same EFI via stage+oem-boot-efi flow), confirm Android boots and keystore still works. `/data` intact (settings, accounts, photos still present).
- [ ] Capture all evidence (on-screen log photos or `/proc/bootloader_log` dump, `logcat` excerpt, brief observations) into `.re-notes/sessions/2026-05-20-universal-tz-rollback-drop.md`.

**Verify:** `cat .re-notes/sessions/2026-05-20-universal-tz-rollback-drop.md` shows the captured evidence and a concluding "PASS" / "FAIL" line.

**Steps:**

- [ ] **Step 1: Stage and boot mode-1 EFI**

From host:

```bash
fastboot stage dist/gbl-chainload-mode1.efi
fastboot oem boot-efi
```

Photograph the on-screen log (or after boot, dump `/proc/bootloader_log`). Grep for `DROPPED (universal)` and note which SmcId fired.

- [ ] **Step 2: Confirm Android boots and exercise keystore (mode-1)**

After boot reaches lockscreen, unlock with credentials (fingerprint if normally enrolled). Open an app that uses Strongbox-backed keys (e.g., a banking app, or run `keystore-cli list` over `adb shell` if available). Run:

```bash
adb shell logcat -d -b system | grep -iE "keymaster|keymint|KM_ERROR" | tail -50
```

Expected: no `KM_ERROR_*` lines tied to this boot's PID range; routine keystore activity OK.

- [ ] **Step 3: Repeat with mode-2 EFI**

```bash
adb reboot bootloader
fastboot stage dist/gbl-chainload-mode2.efi
fastboot oem boot-efi
```

Repeat the keystore check. Expected: mode-2 spoof-overlay lines AND `DROPPED (universal)` both present in the on-screen log; Android boots; keystore OK.

- [ ] **Step 4: Power-cycle without re-staging**

Without re-staging — power-cycle the device and let it boot normally (it will boot stock ABL → stock OS, since gbl-chainload is RAM-staged). Confirm Android still boots, settings/accounts intact. This confirms the universal drop didn't leave the RPMB in a state stock ABL refuses.

Then re-stage and boot the gbl-chainload EFI a second time:

```bash
fastboot stage dist/gbl-chainload-mode1.efi
fastboot oem boot-efi
```

Confirm `DROPPED (universal)` fires again (the SmcIds are per-boot, not per-power-cycle).

- [ ] **Step 5: Capture evidence and write up**

Create `.re-notes/sessions/2026-05-20-universal-tz-rollback-drop.md` with:
- Date, device (infiniti), build SHA.
- Which SmcId(s) actually fired (`0x0200011E` vs `0x32000110`).
- Mode-1 result + keystore log excerpt.
- Mode-2 result + keystore log excerpt.
- Power-cycle / stock-boot result.
- Re-stage result.
- Final line: `PASS` or `FAIL`.

- [ ] **Step 6: If PASS, open PR**

```bash
git push -u origin universal-tz-rollback-drop
gh pr create --title "Universal TZ rollback-bump drop" --body "$(cat <<'EOF'
## Summary
- Drops `TZ_UPDATE_ROLLBACK_VERSION_ID` (0x0200011E) and
  `TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID`
  (0x32000110) at the universal-baseline layer, alongside the existing
  `TZ_BLOW_SW_FUSE_ID` drop.
- Pins the RPMB-stored anti-rollback floor and avoids the
  userdata-factory-reset funnel after older OS images.
- KM 0x207 SET_VERSION untouched (Keystore `upgradeKey()` flow intact).
- Spec: `docs/superpowers/specs/2026-05-20-universal-tz-rollback-drop-design.md`.

## Test plan
- [x] Mode-1 EFI boots infiniti to Android lockscreen; `DROPPED (universal)` line observed for the rollback SmcId.
- [x] Mode-2 EFI boots infiniti to Android lockscreen; both spoof and rollback-drop lines present.
- [x] Keystore-backed app exercised post-boot under both modes; no `KM_ERROR_*`.
- [x] Power cycle then re-stage: stable; `/data` intact.
- [x] Evidence: `.re-notes/sessions/2026-05-20-universal-tz-rollback-drop.md`.
EOF
)"
```

If any criterion FAILs, do not open the PR — capture the failure in the re-notes file, attach screenshots/logs, and re-enter brainstorming for the remediation.

---

## Self-review

**Spec coverage.** Spec §1 (problem) → motivation in plan header. §2 (out of scope: KM 0x207) → reflected in the file-header comment in `UniversalBaseline.c` (Step 2) and the commit message rationale. §3.1 (switch + drop semantics) → Step 2. §3.2 (ScmHook decoder log flip) → Step 3. §3.3 (mode-2 doc closure) → Step 4. §4 (risk) → covered indirectly via the verification gate (Task 2 catches every named risk: keystore, stock fallback, log line). §5 (verification) → Task 2 verbatim. §6 (decomposition, single PR) → Task 1 / Step 7 + Task 2 / Step 6.

**Placeholder scan.** No TBDs, no "add error handling", no "similar to Task N". Every code block is the full file or the full hunk.

**Type consistency.** `UniversalPolicy_ShouldDropScmSip` signature unchanged between header and implementation. SmcId constant names match between `UniversalBaseline.c` and the existing `ScmHook.c` `#define`s (verified against the spec table and `ScmHook.c:279-280`).

**Gate scope.** Task 2 is the only gate; HOW is concrete (named SmcIds, named commands, named log strings, named files). No `requiresUserSpecification` needed.
