# test-device-automatic.sh Edge-Case Hardening — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** make `scripts/test-device-automatic.sh` run unattended through the full reboot→fastboot→stage→escape→recovery→pull→bootloader cycle, surfacing actionable errors instead of hanging when something goes wrong.

**Status:** completed (Tasks 1-8 land in commits `186e7e2`, `2488296`, `66ad792` and ancestors; Tasks 9-11 appended via `ca574bb` and validated through `21133e3`).

**Architecture:** iterative hardening. First a baseline run captures actual failure modes. Then concrete, scoped fixes land for known categories: Phoenix watchdog stopwatch, state-probe primitives in `device-monitor.sh`, replacing silent `|| true` swallows with explicit "expected X, got Y, hard-reset and rerun" exits, and porting test-device-manual.sh's logfs-mount discoveries. Validation = 3 unattended back-to-back runs.

**Tech Stack:** bash, `adb`, `fastboot`, the existing `scripts/device-monitor.sh` helpers (`device_monitor_wait_for_fastboot`, `device_monitor_wait_for_adb_state`, `device_monitor_fastboot`).

**File structure:**
- `docs/superpowers/specs/2026-05-10-test-device-hardening-design.md` — update spec to reference the actual script name (`test-device-automatic.sh`).
- `scripts/test-device-automatic.sh` — primary surface. Add state probes around each step boundary, Phoenix stopwatch, explicit-error exits.
- `scripts/device-monitor.sh` — extend with two new helpers: `device_monitor_in_fastboot_quick` (3-second probe) and a Phoenix watchdog timer.
- `scripts/test-device-manual.sh` — verify untouched by recovery-context changes (regression check).
- `docs/re/test-device-automatic-baseline-findings.md` — new findings doc from Task 2's baseline run.

---

## Task 1: Fix spec script-name reference

**Files:**
- Modify: `docs/superpowers/specs/2026-05-10-test-device-hardening-design.md`

The spec was written referencing `scripts/test-device.sh` but the actual file is `scripts/test-device-automatic.sh`. Fix it before anyone implements against the spec.

- [ ] **Step 1: Replace every `test-device.sh` reference in the spec with `test-device-automatic.sh`**

Run:
```bash
cd /home/vivy/gbl-chainload
sed -i 's/test-device\.sh/test-device-automatic.sh/g' docs/superpowers/specs/2026-05-10-test-device-hardening-design.md
grep -n "test-device" docs/superpowers/specs/2026-05-10-test-device-hardening-design.md
```

Expected: every match now ends in `-automatic.sh` (or `-manual.sh`, which was already correct). Zero bare `test-device.sh` remains.

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-05-10-test-device-hardening-design.md
git commit -m "Spec: correct script reference to test-device-automatic.sh"
```

---

## Task 2: Baseline run — capture actual failure modes

**Files:**
- Create: `docs/re/test-device-automatic-baseline-findings.md`

Run the existing script end-to-end. Note exactly what fails, what hangs, what proceeds silently past broken steps. This becomes the input that shapes the per-failure tasks (which are written as concrete templates below, but you should add additional tasks if Step 2 reveals a category not listed).

- [ ] **Step 1: Stage the test EFI**

```bash
cd /home/vivy/gbl-chainload
ls -lh dist/mode-1-auto-debug-verbose.efi dist/mode-debug.efi 2>&1 | head
```

Expected: at least one of `mode-debug.efi` or `mode-1-auto-debug-verbose.efi` exists (the script defaults to `mode-debug.efi`). If neither exists, run `./scripts/build.sh --mode 1 --auto --debug --verbose` first.

- [ ] **Step 2: Power off device. Boot it into the bootloader fastboot. Confirm with `fastboot devices`**

```bash
fastboot devices
```

Expected: one line, like `3C15AT003ZB00000 fastboot`.

- [ ] **Step 3: Run the script and capture full output**

```bash
cd /home/vivy/gbl-chainload
./scripts/test-device-automatic.sh 2>&1 | tee /tmp/test-device-baseline.log
```

Observe carefully. Note:
- Which `>>> [N/5]` step does it hang at or fail at?
- Does it complete cleanly?
- Did Phoenix's 60s watchdog fire (device dropped to stock fastboot mid-test)?
- Were any `2>/dev/null || true` fallbacks silently triggered (look for missing files in `$LOG_DIR`)?

- [ ] **Step 4: Write findings**

Create `docs/re/test-device-automatic-baseline-findings.md`:

```markdown
# test-device-automatic.sh baseline findings — 2026-05-10

## Run output

(Paste relevant excerpts from /tmp/test-device-baseline.log.)

## Failures observed

For each failure, fill in:
- **Step**: which `>>> [N/5]`
- **Symptom**: what the script said, or that it hung
- **Actual device state at time of failure**: confirmed by manual `fastboot devices` / `adb devices` from another shell
- **Diagnosis**: what likely caused it
- **Recommended fix category**: state-probe / phoenix-watchdog / silent-fallback / logfs-port / unknown

## Result

(One of)
- Clean run, all 5 steps complete, full log set captured.
- One or more failures (listed above).

## Plan tasks to add

If Step 4's failure list reveals a category not covered by Tasks 3–6 below, append a new task to this plan with the same template structure before continuing.
```

- [ ] **Step 5: Commit the findings**

```bash
cd /home/vivy/gbl-chainload
git add docs/re/test-device-automatic-baseline-findings.md
git commit -m "Baseline test-device-automatic.sh run findings"
```

---

## Task 3: Add Phoenix watchdog stopwatch helper

**Files:**
- Modify: `scripts/device-monitor.sh`

Phoenix is OnePlus's bootloader watchdog that drops the device to stock fastboot 60s after the device enters fastboot mode. If our staging / logfs pull takes too long, we lose the in-flight EFI session. Add a stopwatch that warns at 45s and aborts at 55s.

- [ ] **Step 1: Add the timer helpers**

In `scripts/device-monitor.sh`, append (idempotent — only if not already present):

```bash
# Phoenix watchdog timer.
#
# OnePlus's bootloader runs a 60s watchdog from "device entered fastboot
# mode". If our test cycle takes too long between entering fastboot and
# rebooting back to bootloader, Phoenix drops the device to stock fastboot
# and our in-flight EFI session is gone.
#
# Usage:
#   device_monitor_phoenix_start          # snapshot start time
#   device_monitor_phoenix_check          # WARN at 45s, ABORT at 55s
#
# Variables (script-globals, no subshell):
#   _PHOENIX_T0   epoch seconds when started, empty = inactive
#   _PHOENIX_WARN seconds elapsed at which to warn
#   _PHOENIX_KILL seconds elapsed at which to fatal
_PHOENIX_T0=""
_PHOENIX_WARN=45
_PHOENIX_KILL=55

device_monitor_phoenix_start () {
  _PHOENIX_T0=$(date +%s)
}

device_monitor_phoenix_elapsed () {
  [[ -z "$_PHOENIX_T0" ]] && { echo 0; return; }
  echo $(( $(date +%s) - _PHOENIX_T0 ))
}

device_monitor_phoenix_check () {
  local elapsed
  elapsed=$(device_monitor_phoenix_elapsed)
  if (( elapsed >= _PHOENIX_KILL )); then
    echo "    FATAL: ${elapsed}s elapsed since fastboot entry — Phoenix watchdog will fire imminently. Aborting." >&2
    return 1
  fi
  if (( elapsed >= _PHOENIX_WARN )); then
    echo "    WARN: ${elapsed}s elapsed since fastboot entry — Phoenix 60s watchdog approaching." >&2
  fi
  return 0
}

device_monitor_phoenix_stop () {
  _PHOENIX_T0=""
}
```

- [ ] **Step 2: Verify the helpers parse**

```bash
bash -n /home/vivy/gbl-chainload/scripts/device-monitor.sh && echo "syntax OK"
```

Expected: `syntax OK`.

- [ ] **Step 3: Smoke-test the timer in isolation**

```bash
( source /home/vivy/gbl-chainload/scripts/device-monitor.sh
  device_monitor_phoenix_start
  echo "elapsed=$(device_monitor_phoenix_elapsed)"
  sleep 2
  echo "elapsed=$(device_monitor_phoenix_elapsed)"
  device_monitor_phoenix_check && echo "check ok"
)
```

Expected: first elapsed ≤ 1, second elapsed 2 or 3, `check ok` printed.

- [ ] **Step 4: Commit**

```bash
cd /home/vivy/gbl-chainload
git add scripts/device-monitor.sh
git commit -m "device-monitor: add Phoenix watchdog stopwatch helpers"
```

---

## Task 4: Wire Phoenix stopwatch into test-device-automatic.sh

**Files:**
- Modify: `scripts/test-device-automatic.sh`

Start the stopwatch when the device enters fastboot (after step 2 succeeds). Check at each pause point. Stop it when we successfully reach the bootloader for the next iteration (step 5).

- [ ] **Step 1: Read current step boundaries**

```bash
grep -n "^echo \">>>\\|^# Step\\| device_monitor_phoenix" /home/vivy/gbl-chainload/scripts/test-device-automatic.sh
```

Note the line numbers for each `>>> [N/5]` step start. Phoenix start belongs right after step 2's successful stage/escape; checks belong before any potentially-slow operation (long adb pull, etc.); stop belongs at step 5's successful re-entry to fastboot.

- [ ] **Step 2: Add `device_monitor_phoenix_start` after step 2 completes**

After the line that confirms `oem escape` or `oem boot-efi` returned successfully (right before step 3 starts), insert:

```bash
device_monitor_phoenix_start
echo "    Phoenix stopwatch started — must reach bootloader within ${_PHOENIX_KILL}s"
```

- [ ] **Step 3: Add `device_monitor_phoenix_check` calls before slow operations**

Specifically:
1. Right before `wait_for_adb_or_fastboot_fallback` (step 3): bail early if we're already near the limit.
2. Right before the `adb pull` cluster (step 4): same.
3. Right before `fastboot reboot bootloader` for next iteration (step 5): final check.

Pattern at each insertion point:

```bash
if ! device_monitor_phoenix_check; then
  echo "error: Phoenix watchdog deadline reached — bailing to avoid stock-fastboot wedge" >&2
  echo "       power off the device, power on into bootloader, rerun script." >&2
  exit 1
fi
```

- [ ] **Step 4: Stop the stopwatch on successful step 5**

At the end of step 5 (after `fastboot wait_for_fastboot` confirms sibling fastboot is back):

```bash
device_monitor_phoenix_stop
```

- [ ] **Step 5: Syntax check**

```bash
bash -n /home/vivy/gbl-chainload/scripts/test-device-automatic.sh && echo "syntax OK"
```

- [ ] **Step 6: Commit**

```bash
cd /home/vivy/gbl-chainload
git add scripts/test-device-automatic.sh
git commit -m "test-device-automatic: wire Phoenix watchdog stopwatch"
```

---

## Task 5: Add state-probe helpers

**Files:**
- Modify: `scripts/device-monitor.sh`

A state probe is a 2–3 second non-blocking check that returns "device is in state X" or "no device / unexpected state". Without these, the script silently waits for something that may never come.

- [ ] **Step 1: Append the probes**

Add to `scripts/device-monitor.sh`:

```bash
# Quick state probes — return 0 if device is in the named state within 3s.

# Is the device responding in fastboot right now?
device_monitor_in_fastboot_quick () {
  local out
  out="$(timeout 3 fastboot devices 2>/dev/null | grep -c 'fastboot')"
  [[ "$out" -ge 1 ]]
}

# Is adb up with a normal device state right now?
device_monitor_in_adb_quick () {
  local out
  out="$(timeout 3 adb get-state 2>/dev/null)"
  [[ -n "$out" && "$out" != "unknown" ]]
}

# Did the device drop to stock fastboot? Detects by product string mismatch.
# Stock OnePlus fastboot returns a specific product (e.g. "infiniti" or
# "canoe"); our patched FastbootLib returns a different one. Caller
# specifies the expected product substring; mismatch => stock.
device_monitor_dropped_to_stock () {
  local expected="$1"
  local product
  product="$(timeout 3 fastboot getvar product 2>&1 | grep -i product | head -1 || true)"
  if [[ -z "$product" ]]; then
    return 1  # can't tell
  fi
  if echo "$product" | grep -qi "$expected"; then
    return 1  # matched expected, not stock
  fi
  return 0  # mismatch = dropped to stock
}
```

- [ ] **Step 2: Syntax check**

```bash
bash -n /home/vivy/gbl-chainload/scripts/device-monitor.sh && echo "syntax OK"
```

- [ ] **Step 3: Smoke-test against device in fastboot**

(Skip if device isn't in fastboot right now.)
```bash
( source /home/vivy/gbl-chainload/scripts/device-monitor.sh
  device_monitor_in_fastboot_quick && echo "fastboot=YES" || echo "fastboot=NO"
  device_monitor_in_adb_quick && echo "adb=YES" || echo "adb=NO"
)
```

Expected: at most one of YES at a time (matching current device state).

- [ ] **Step 4: Commit**

```bash
cd /home/vivy/gbl-chainload
git add scripts/device-monitor.sh
git commit -m "device-monitor: add quick state-probe helpers"
```

---

## Task 6: Replace silent fallbacks with state-aware exits

**Files:**
- Modify: `scripts/test-device-automatic.sh`

Critical-path commands currently do `2>/dev/null || true`, which lets the script proceed past a broken step. Replace each with a state check + explicit error message that names the failed step and recommends recovery.

- [ ] **Step 1: List every `|| true` and `2>/dev/null` on critical commands**

```bash
grep -nE '\|\| true|2>/dev/null' /home/vivy/gbl-chainload/scripts/test-device-automatic.sh | head -30
```

Critical = anything in steps 1, 2, 3, 5. Step 4 (log pulls) is allowed to be best-effort.

- [ ] **Step 2: Wrap step 2's `oem escape` call**

Find the `device_monitor_fastboot oem escape` invocations (there are two — one in the escape-recovery branch, one in the escape-with-payload branch). Replace each with:

```bash
ESCAPE_OUT="$(device_monitor_fastboot oem escape 2>&1)"
ESCAPE_RC=$?
echo "$ESCAPE_OUT"
if [[ $ESCAPE_RC -ne 0 ]] && ! echo "$ESCAPE_OUT" | grep -qi "OKAY\|finished"; then
  echo "error: oem escape did not succeed (rc=$ESCAPE_RC)." >&2
  echo "       device may still be in fastboot or may have wedged. Recovery:" >&2
  echo "       1) check fastboot devices manually" >&2
  echo "       2) if missing, power off + boot into bootloader, rerun" >&2
  exit 1
fi
```

(Keep the existing successful-path follow-up code intact.)

- [ ] **Step 3: Add post-step-2 state probe**

After step 2's escape/stage path completes, immediately probe:

```bash
sleep 2
if device_monitor_in_fastboot_quick; then
  echo "warn: still in fastboot 2s after oem escape — chainload may have failed." >&2
  echo "      will continue with extended adb wait, but this is suspicious." >&2
fi
```

- [ ] **Step 4: Improve step 3 wait-for-adb failure message**

Find the `wait_for_adb_or_fastboot_fallback` failure branch. Replace its bare error message with:

```bash
if [[ $WAIT_RC -ne 0 ]]; then
  echo "error: device did not come up on adb within 360s." >&2
  echo "       last device state:" >&2
  fastboot devices 2>&1 | sed 's/^/         /' >&2
  adb devices 2>&1 | sed 's/^/         /' >&2
  echo "       likely causes:" >&2
  echo "         1) chainload reached recovery but adb not enabled (pull /tmp/recovery-log via emergency dump)" >&2
  echo "         2) Phoenix watchdog fired — device dropped to stock fastboot" >&2
  echo "         3) recovery vbmeta mismatch — device booted then panicked" >&2
  echo "       recovery: power off, power on into bootloader, rerun" >&2
  exit 1
fi
```

- [ ] **Step 5: Syntax check**

```bash
bash -n /home/vivy/gbl-chainload/scripts/test-device-automatic.sh && echo "syntax OK"
```

- [ ] **Step 6: Commit**

```bash
cd /home/vivy/gbl-chainload
git add scripts/test-device-automatic.sh
git commit -m "test-device-automatic: replace silent fallbacks with state-aware exits"
```

---

## Task 7: Port test-device-manual.sh's logfs fixes (audit pass)

**Files:**
- Inspect: `scripts/test-device-automatic.sh` (logfs section)
- Modify: same, only if a fix is actually needed.

test-device-manual.sh recently learned how to mount logfs from the system context (`/data/local/tmp/logfs` mountpoint, `"su -c '<compound>'"` quoting). test-device-automatic.sh runs in recovery context where root is direct — likely doesn't need the same fix, but verify.

- [ ] **Step 1: Find the logfs mount block in test-device-automatic.sh**

```bash
grep -nE "logfs|mount.*vfat|by-name" /home/vivy/gbl-chainload/scripts/test-device-automatic.sh | head -20
```

- [ ] **Step 2: Compare with test-device-manual.sh's fixed block**

```bash
grep -nE "logfs|mount.*vfat|by-name|LOGFS_MOUNTPOINT" /home/vivy/gbl-chainload/scripts/test-device-manual.sh | head -30
```

- [ ] **Step 3: Decide**

If test-device-automatic.sh:
- Only runs in recovery context (where `mkdir /logfs && mount /dev/block/by-name/logfs /logfs` works directly): **no change needed**. Document this in a one-line comment near the mount block.
- Ever runs in system context: port the fix (use `/data/local/tmp/logfs` + `"su -c '<compound>'"` quoting).

To document the assumption, add this comment near the logfs mount block in test-device-automatic.sh:

```bash
# Recovery context: '/' is writable, no su needed, by-name symlinks work
# directly. If this script ever needs to run from system context, port
# the /data/local/tmp/logfs + 'su -c <compound>' pattern from
# scripts/test-device-manual.sh.
```

- [ ] **Step 4: Syntax check**

```bash
bash -n /home/vivy/gbl-chainload/scripts/test-device-automatic.sh && echo "syntax OK"
```

- [ ] **Step 5: Commit (only if Step 3 made a change)**

```bash
cd /home/vivy/gbl-chainload
git add scripts/test-device-automatic.sh
git commit -m "test-device-automatic: document recovery-context logfs assumption"
```

If no change was made, skip the commit.

---

## Task 8: Validation — three unattended back-to-back runs

**Files:**
- None modified. This is the acceptance gate.

- [ ] **Step 1: Confirm device starts in bootloader fastboot**

```bash
fastboot devices
```

Expected: one fastboot device.

- [ ] **Step 2: Run the cycle three times unattended**

```bash
cd /home/vivy/gbl-chainload
for i in 1 2 3; do
  echo "=== Run $i ==="
  ./scripts/test-device-automatic.sh 2>&1 | tee /tmp/test-device-run-$i.log
  echo "=== Run $i exit code: $? ==="
done
```

Walk away. No interventions.

- [ ] **Step 3: Verify each run's output**

```bash
for i in 1 2 3; do
  echo "Run $i:"
  ls -lh logs/$(ls -t logs/ | head -3 | sort | sed -n "${i}p")/ 2>&1 | tail -5
  echo
done
```

Expected per run: a log dir with at minimum `bootloader_log`, `dmesg.txt`, `bootconfig`, `cmdline`, `device-tree.tar`, `getprop.boot.txt`, `logfs/*`.

Or if any run failed: exit code non-zero AND a clear error message in the run log naming the failed step + a recommended recovery action.

- [ ] **Step 4: Decision**

- All 3 runs clean: plan complete. Skip to Step 5.
- 1+ runs failed with clear actionable errors: plan complete (we surfaced the failure cleanly, which was the goal). Skip to Step 5.
- 1+ runs hung silently OR failed with vague errors: append a new task to this plan documenting the new failure mode and what fix is needed, then iterate.

- [ ] **Step 5: Final commit of any plan amendments**

```bash
cd /home/vivy/gbl-chainload
git add docs/superpowers/plans/2026-05-10-test-device-hardening.md docs/re/test-device-automatic-baseline-findings.md
git diff --cached --stat
git commit -m "Track 0: complete test-device-automatic.sh hardening
  
3 back-to-back runs validate hardening. Findings doc records what
the baseline run observed." 2>/dev/null || echo "nothing to commit"
```

---

## Self-Review

Checked against `docs/superpowers/specs/2026-05-10-test-device-hardening-design.md`:

- ✓ Sub-task 1 (baseline run) → Task 2 captures, Task 8 re-validates.
- ✓ Sub-task 2 (per-failure hardening) → Tasks 5+6 cover state probes and replacing silent fallbacks; Task 8 catches anything else.
- ✓ Sub-task 3 (port test-device-manual.sh logfs fixes) → Task 7.
- ✓ Sub-task 4 (Phoenix watchdog timer) → Tasks 3+4.
- ✓ Validation (3–4 back-to-back unattended runs) → Task 8.

No placeholders, no `Similar to Task N`, every code block contains the actual code to write or the actual command to run. Type consistency: `_PHOENIX_T0`, `_PHOENIX_WARN`, `_PHOENIX_KILL` are used uniformly across Task 3 (definitions) and Task 4 (consumers). `device_monitor_in_fastboot_quick`, `device_monitor_in_adb_quick`, `device_monitor_dropped_to_stock`, `device_monitor_phoenix_check` names are uniform across Tasks 3, 5, 6.

One gap acknowledged: the spec says "if baseline run is clean, plan reduces to sub-task 3 + 4". This plan still runs Tasks 5+6 in that case — they're worth landing regardless since they add defence in depth (state probes and clearer error messages don't hurt a working script). If the operator strongly wants to skip them after a clean Task 2, they can; mark Tasks 5–6 done without changes.

---

## Scope amendment — 2026-05-10 post-Task 8 review

Task 8's validation revealed that the script's *intended* workflow was misunderstood. The default `oem boot-efi` flow is correct for testing — stage payload, run it, observe outcome. The payload-flow guard added in Task 6 gave wrong-direction advice (`ESCAPE_WITH_PAYLOAD=1`) when the real issue is that `oem boot-efi`'s fastboot reply isn't clean and the script can't distinguish three post-boot-efi outcomes:

- **A — payload runs successfully**: device boots into recovery → adb up → pull logs.
- **B — payload crashes, lands in our flashed FastbootLib**: device reboots; our pre-flashed `mode-1-auto-debug-verbose.efi` auto-boots into FastbootLib. Script issues `oem escape` → patched ABL → recovery → pull crash logs.
- **C — payload crashes, device powers off**: no fastboot AND no adb for >60s. Script declares user-assistance required.

Tasks 9, 10, 11 below address this scope correction.

---

## Task 9: Fix `oem boot-efi` cleanliness in edk2/

**Files:**
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`
- Modify: `GblChainloadPkg/Library/LogFsLib/PostGblLog.c` (or wherever logfs writes happen) — only if needed for deferred-rc logging

Currently `oem boot-efi` starts the staged image via `gBS->StartImage()` but the fastboot reply isn't a clean OKAY/FAIL — the host either sees a USB drop or an incomplete reply. We make it reply `OKAY started` *before* StartImage, and log the eventual rc to logfs after StartImage returns (if it returns at all).

- [ ] **Step 1: Find the existing `oem boot-efi` handler in FastbootCmds.c**

```bash
cd /home/vivy/gbl-chainload/edk2
grep -nE "CmdOemBootEfi|oem boot-efi|StartImage" QcomModulePkg/Library/FastbootLib/FastbootCmds.c | head -20
```

Note the handler function name + line range.

- [ ] **Step 2: Modify the handler**

Before `gBS->StartImage(...)` call, add:
```c
/* Reply OKAY before handoff so host's fastboot terminates cleanly.
   StartImage's eventual rc (if it returns) is logged to logfs below
   for after-the-fact triage. */
FastbootOkay ("started");
WaitForTransferComplete ();
```

(`WaitForTransferComplete` is already a STATIC in this file — see line ~3024 area. Mirror the pattern from CmdGetVarAll.)

After the StartImage call (if reachable):
```c
{
  CHAR8  Line[128];
  AsciiSPrint (Line, sizeof (Line),
               "oem boot-efi: StartImage returned %r at %u ticks\n",
               StartStatus, (UINT32)GetPerformanceCounter ());
  if (LogFsIsReady ()) {
    LogFsWrite (Line, AsciiStrLen (Line));
  }
  /* No more FastbootOkay/Fail — host already saw OKAY. */
}
```

Where `StartStatus` is the captured `EFI_STATUS` from StartImage.

If the StartImage call previously had `FastbootOkay` / `FastbootFail` calls after it, those become DEAD CODE — remove them.

- [ ] **Step 3: Build (mode-1 with the chainloader EFI is what we'll test against)**

```bash
./scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 4: Commit (edk2 + parent bump)**

```bash
cd /home/vivy/gbl-chainload/edk2 \
  && git add QcomModulePkg/Library/FastbootLib/FastbootCmds.c \
  && git commit -m "FastbootCmds: oem boot-efi replies OKAY before StartImage, logs rc to logfs"
cd /home/vivy/gbl-chainload \
  && git add edk2 \
  && git commit -m "edk2: bump submodule (oem boot-efi cleanliness)"
```

Do NOT push.

---

## Task 10: Post-boot-efi state machine in test-device-automatic.sh

**Files:**
- Modify: `scripts/test-device-automatic.sh`
- Possibly modify: `scripts/device-monitor.sh` (helper for detecting "our FastbootLib")

The script today, post-`oem boot-efi`, just waits for adb. It doesn't distinguish outcome A vs B vs C. Add a state machine after `oem boot-efi` that:

1. Polls adb-up and fastboot-up in parallel for up to 60s.
2. If **adb-up** first: outcome A → continue with existing log pull (Step 4).
3. If **fastboot-up** first: probe `fastboot getvar version-bootloader` (or `oem efi-status`) to distinguish our FastbootLib from stock.
   - Ours → outcome B: issue `fastboot oem escape`, then wait for adb, then pull logs.
   - Stock → outcome B' (already-known-bad): error message names "device fell to stock fastboot — Phoenix watchdog or hard crash. Power off + rerun."
4. If **neither for 60s**: outcome C → "device powered off — power on into bootloader and rerun."

- [ ] **Step 1: Revert/repurpose Task 6's payload-flow guard**

The case-match that warned about chainloader payloads (in `test-device-automatic.sh` around lines 72–91 of the post-Task-6 state) gave wrong-direction advice. Delete the warning block. The case-match itself is still useful — keep it as a comment that documents which payload basenames are "chainloader" and which are "Linux-bootable", but stop printing the wrong recommendation.

Replacement: just leave a 3-line comment explaining payload conventions, no runtime check. Example:

```bash
# Payload conventions (informational, not enforced):
#   - mode-0*.efi:                 Linux-bootable, default oem boot-efi flow works.
#   - mode-1*.efi / *auto-debug*:  Chainloader; oem boot-efi runs it, then it
#                                  attempts to escape via ABL. If escape fails,
#                                  device reboots into the flashed FastbootLib.
#                                  Task 10's state machine handles both outcomes.
```

- [ ] **Step 2: Add a helper to detect "our FastbootLib" vs "stock fastboot"**

In `scripts/device-monitor.sh`, append:

```bash
# Does the current fastboot device look like OUR FastbootLib (i.e. the one
# from gbl-chainload's flashed EFI), as opposed to stock?
#
# We use `fastboot oem efi-status` as a signature: OUR FastbootLib registers
# that command; stock doesn't. If the command returns FAIL with "command not
# found" (or similar), we're on stock. If it returns OKAY with status text,
# we're on ours.
device_monitor_is_our_fastbootlib () {
  local out
  out="$(timeout 3 fastboot oem efi-status 2>&1 || true)"
  echo "$out" | grep -qi "OKAY\|efi-status\|gbl-chainload" && return 0
  return 1
}
```

(Verify the actual response of `oem efi-status` by running it once against our FastbootLib while building this helper. Adjust the grep pattern to match whatever our FastbootLib actually returns. If `oem efi-status` doesn't exist in our FastbootLib, pick another command unique to us — e.g. `oem escape` returning a recognisable string.)

- [ ] **Step 3: Add the post-boot-efi state machine to test-device-automatic.sh**

Find the section right after `device_monitor_fastboot oem boot-efi ...` in the default flow (Step 2 of the script). Replace what currently happens (existing `wait_for_adb_or_fastboot_fallback`) with:

```bash
# After oem boot-efi, the staged payload runs. Three possible outcomes:
#   A — payload boots into recovery (success) → wait for adb
#   B — payload crashes, device reboots into our flashed FastbootLib → oem escape → wait for adb
#   C — payload crashes, device powers off → user assistance needed
echo "    waiting up to 60s for outcome A (adb up) or B (our FastbootLib) or C (power off)..."

WAIT_LIMIT=60
WAIT_ELAPSED=0
STATE=""
while [[ $WAIT_ELAPSED -lt $WAIT_LIMIT ]]; do
  if device_monitor_in_adb_quick; then
    STATE="A"
    break
  fi
  if device_monitor_in_fastboot_quick; then
    if device_monitor_is_our_fastbootlib; then
      STATE="B"
    else
      STATE="B-stock"
    fi
    break
  fi
  sleep 3
  WAIT_ELAPSED=$((WAIT_ELAPSED + 3))
done

case "$STATE" in
  A)
    echo "    outcome A: payload booted to adb. Continuing to log pull."
    ;;
  B)
    echo "    outcome B: payload crashed, device booted our flashed FastbootLib."
    echo "    issuing oem escape to enter recovery and pull crash logs..."
    device_monitor_fastboot oem escape 2>&1 | head -3 || true
    echo "    waiting for adb (recovery) after escape..."
    if ! device_monitor_wait_for_adb_state 120 >/dev/null; then
      echo "error: outcome B but adb did not come up after oem escape." >&2
      exit 1
    fi
    ;;
  B-stock)
    echo "error: outcome B': device fell to stock fastboot (Phoenix watchdog or hard crash)." >&2
    echo "       power off the device, power on into bootloader, rerun script." >&2
    exit 1
    ;;
  "")
    echo "error: outcome C: neither adb nor fastboot for 60s — device likely powered off." >&2
    echo "       power on into bootloader (Power + VolUp + VolDn), then rerun script." >&2
    exit 1
    ;;
esac
```

- [ ] **Step 4: Syntax check**

```bash
bash -n /home/vivy/gbl-chainload/scripts/test-device-automatic.sh && echo "syntax OK"
bash -n /home/vivy/gbl-chainload/scripts/device-monitor.sh && echo "syntax OK"
```

- [ ] **Step 5: Commit**

```bash
cd /home/vivy/gbl-chainload
git add scripts/test-device-automatic.sh scripts/device-monitor.sh
git commit -m "test-device-automatic: post-boot-efi state machine (A/B/C outcomes)
  
  Replaces wrong-direction payload-flow guard with a real state probe.
  After oem boot-efi, distinguish:
    A — adb up (payload booted recovery): pull logs.
    B — fastboot up + our FastbootLib (payload crashed, reboot landed
        in our flashed EFI): oem escape → recovery → pull crash logs.
    B-stock — fastboot up + stock fastboot (Phoenix or hard crash): error.
    C — neither for 60s (power-off): user assistance required."
```

---

## Task 11: Re-validate 3 unattended back-to-back runs

**Files:**
- Modify: `docs/re/test-device-automatic-baseline-findings.md` (append re-validation outcome)

Replace Task 8's validation with one that exercises the corrected workflow.

- [ ] **Step 1: Confirm device starts in bootloader fastboot**

```bash
fastboot devices
```

- [ ] **Step 2: Run 3 back-to-back, default invocation**

```bash
cd /home/vivy/gbl-chainload
for i in 1 2 3; do
  echo "=== Run $i starting at $(date +%T) ==="
  ./scripts/test-device-automatic.sh 2>&1 | tee /tmp/test-device-rerun-$i.log
  echo "=== Run $i exited with PIPESTATUS[0]=${PIPESTATUS[0]} ==="
  fastboot reboot bootloader 2>/dev/null || true
  sleep 5
done
```

- [ ] **Step 3: Per-run classification**

For each run, classify:
- **PASS-A**: payload booted recovery, full log set captured.
- **PASS-B**: payload crashed, our FastbootLib caught it, oem escape recovered, crash logs captured.
- **PASS-actionable-B-stock**: cleanly reported "device fell to stock fastboot", exit non-zero.
- **PASS-actionable-C**: cleanly reported "device powered off", exit non-zero.
- **FAIL-hung**: hung > 3 minutes anywhere; Ctrl-C'd it.
- **FAIL-silent**: silent failure or wrong outcome classification.

- [ ] **Step 4: Append outcome to findings doc**

```markdown
## Re-validation outcome (Track 0 Task 11) — 2026-05-10

After Tasks 9 (oem boot-efi cleanliness) and 10 (post-boot-efi state machine):

- Run 1: PASS-A / PASS-B / PASS-actionable-B-stock / PASS-actionable-C / FAIL-...
- Run 2: ...
- Run 3: ...

oem boot-efi OKAY reply visible in host fastboot output: yes/no
Logfs deferred-rc line visible: yes/no
State machine classified correctly: yes/no

Track 0 acceptance (revised): PASS / FAIL
```

- [ ] **Step 5: Commit**

```bash
cd /home/vivy/gbl-chainload
git add docs/re/test-device-automatic-baseline-findings.md
git commit -m "Track 0 Task 11 re-validation: 3 back-to-back runs with state machine"
```

Do NOT push.

