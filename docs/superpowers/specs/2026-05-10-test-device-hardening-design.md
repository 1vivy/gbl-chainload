# Test-device.sh Edge-Case Hardening — Track 0 Design

**Status:** approved — ready for implementation plan.

## Goal

The full automated cycle in `scripts/test-device-automatic.sh` runs unattended without
silently moving past a broken step. When it fails, the script tells the
operator exactly which state the device is in and what to do (hard-reset,
rerun, etc.) instead of hanging or pulling empty/partial logs.

## Cycle this script drives

1. Reboot device (cold or from-system) into bootloader
2. Reboot to recovery (so device enters our auto-fastboot path on the way back)
3. Auto-boot lands in our FastbootLib (via patched ABL, mode-1 staged)
4. Host stages `dist/<artifact>.efi` and issues `oem escape`
5. Patched ABL chainloads the staged EFI; chainload boots into recovery
6. ADB up in recovery → pull captures (bootloader_log, dmesg, logfs partition contents)
7. Reboot bootloader **before Phoenix's 60s watchdog fires** (otherwise device drops to stock fastboot and the in-flight EFI session is lost)

## Approach

Iterative, not exhaustive enumeration. Existing flow works "for the most
part"; we want to harden specifically against the cases that actually
bite.

### Sub-task 1: baseline run

Execute current `scripts/test-device-automatic.sh` end-to-end against
`dist/mode-1-auto-debug-verbose.efi` already flashed. Capture exactly
which steps fail or behave unexpectedly (no failure → easy plan: skip
to sub-task 3 cleanups). Failure modes recorded into the plan as
concrete fix tasks.

### Sub-task 2: per-failure hardening

For each failure surfaced in sub-task 1:

- **Device-state probe before / after the step.** Examples:
  - Before stage: confirm `fastboot getvar product` returns within 2s
  - After escape: confirm `fastboot getvar product` *fails* within 5s
    (device should be transitioning, not still in stock fastboot)
  - Before pull: confirm `adb shell true` returns 0
- **Replace silent `2>/dev/null || true`** on critical-path commands
  with explicit "expected X, got Y, hard-reset and rerun" exit and
  non-zero status.
- **Phoenix-watchdog wedge detector.** If `fastboot getvar product` hangs
  for >5s after the escape window or returns OnePlus stock product
  string when we expected our chainloaded recovery, emit:
  ```
  device dropped to stock fastboot — Phoenix watchdog likely fired
  → power off, power on into bootloader, rerun script
  ```

### Sub-task 3: port test-device-manual.sh logfs fixes

The `test-device-manual.sh` recently learned how to mount logfs from
the system context (`/data/local/tmp/logfs` mountpoint, `"su -c
'<compound command>'"` quoting). The recovery-context flow in
`test-device-automatic.sh` is simpler (mounts at `/logfs`, no su needed) but
should be verified intact.

Specifically:
- Confirm test-device-automatic.sh logfs mount works in recovery (single command
  shells, simpler quoting). Don't break.
- If test-device-automatic.sh ever runs in system context (e.g. user opted to
  skip the recovery handoff), it needs the same fixes.

### Sub-task 4: Phoenix watchdog timer

Add a script-side stopwatch from "device entered our FastbootLib" (i.e.
right after stage succeeds) to "ready for bootloader reboot".

- Warn at 45s elapsed: "approaching Phoenix 60s watchdog"
- Hard-abort with clear guidance at 55s: "Phoenix watchdog will fire
  in ~5s — bailing now to avoid stock-fastboot wedge. rerun."

Implementation: bash `SECONDS` builtin or `date +%s` deltas. No
threading needed.

## Validation

Run the hardened script 3–4 times back-to-back, no human assistance.
Each run must:

- Pull a complete log set (bootloader_log + dmesg + logfs/* + cmdline +
  bootconfig) **or** exit non-zero with an actionable message that
  names the failed step and recommended recovery action.

## Out of scope

- Reformatting or refactoring `test-device-automatic.sh` (only fix what's broken)
- New `test-device-*` variants
- Recovery-side test orchestration changes (separate concern)
- Replacing the script with Python or another language

## Files in scope

- `scripts/test-device-automatic.sh` (primary)
- `scripts/test-device-manual.sh` (verify still working after sub-task 3)
- `scripts/device-monitor.sh` (utility helpers; only modify if a
  helper needs to grow a new state-probe primitive)

## Risks and known gotchas

- The iterative approach means the plan structure is shaped by what
  sub-task 1 reveals. If the baseline run is clean, the plan reduces
  to just sub-task 3 + 4. If baseline reveals 5 failure modes, the
  plan grows accordingly. The plan author should size the plan
  *after* the baseline run completes.
- Phoenix watchdog is wall-clock 60s from entering fastboot. Long pulls
  (dmesg can be 2 MB+) eat into that budget. Sub-task 4's stopwatch
  needs to start from the right anchor — entering fastboot, not
  entering recovery.
