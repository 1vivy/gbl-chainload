# test-device-automatic.sh baseline findings — 2026-05-10

## Run output

```
======================================================================
  test-device-automatic.sh
  payload  : dist/mode-1-auto-debug-verbose.efi (569344 bytes)
  label    : mode-1-auto-debug-verbose
  version  : 2.0-plan1
  log dir  : /home/vivy/gbl-chainload/logs/20260510-160014_auto_mode-1-auto-debug-verbose_v2.0-plan1
  monitor  : ...20260510-160014_device-monitor.log
  escape   : 0 (with-payload=0)
  return   : 1
======================================================================

>>> [1/5] confirming fastboot device
8549c105	 fastboot

>>> [2/5] fastboot stage + oem boot-efi  (dist/mode-1-auto-debug-verbose.efi)
Sending 'dist/mode-1-auto-debug-verbose.efi' (556 KB) OKAY [  0.026s]
Finished. Total time: 0.113s
(bootloader) loading staged 569344 bytes
fastboot: error: Command failed
    (USB drop on StartImage — expected handoff)

>>> [3/5] waiting for adb (Linux or recovery)...
    fastboot fallback captured: .../fastboot-fallback.txt
error: expected adb/recovery, but device is back in fastboot.
       This usually means the payload failed to boot Linux/recovery and
       reset/powered back into sibling FastbootLib.
```

device-monitor.log (full):
```
2026-05-10 16:00:14 adb=unknown fastboot=8549c105	 fastboot
2026-05-10 16:00:19 adb=unknown fastboot=unknown
2026-05-10 16:00:24 adb=unknown fastboot=8549c105	 fastboot
```

fastboot-fallback.txt:
```
reason: adb-timeout-hit-fastboot-after-chainload
time: 2026-05-10 16:00:24

[fastboot oem efi-status]
FAILED (remote: 'unknown command')
```

`oem efi-status` returning `unknown command` confirms the device fell into **stock fastboot** (product: `canoe`), not sibling's FastbootLib.

## Run outcome

**Failed at step 3**: device returned to stock fastboot instead of adb ~10s after StartImage handoff. Script detected the fallback correctly and exited with rc=2.

## Failures observed

### Failure 1: Payload mode/flow mismatch

- **Step**: `[3/5]` (detected immediately after `[2/5]` StartImage handoff)
- **Symptom**: `wait_for_adb_or_fastboot_fallback` found fastboot after ~10s; `fastboot oem efi-status` returned `unknown command`, confirming stock fastboot (not sibling FastbootLib)
- **Timeline**: T+0 sibling fastboot up → T+5 USB drop (StartImage) → T+10 stock fastboot (`canoe`) back. Only 10s between StartImage and stock fastboot reappearing — far too fast for a Linux boot.
- **Actual device state at time of failure**: stock fastboot, product=`canoe`, `oem efi-status` unknown. Confirmed by `fastboot getvar product` and `fastboot oem efi-status` immediately after script exit.
- **Root cause**: `mode-1-auto-debug-verbose.efi` is a chainloader (mode-1 = fakelocked/auto). Its escape path tried to chainload into ABL and fell back to its own FastbootLib (embedded string: `"escape returned %r (falling back to fastboot)"`). Because it was loaded via `oem boot-efi` (RAM staging, not flashed), once it entered its fallback FastbootLib there was no sibling escape table to return to — eventual reset dropped to stock. The correct test flow for this payload is `--escape-with-payload` (stage → `oem boot-efi` → wait for mode-1 FastbootLib → `oem escape` → ABL → Linux), not the default direct `oem boot-efi` flow which expects the payload to boot Linux unaided.
- **Recommended fix category**: **payload-flow-mismatch** (the script was given the wrong payload for the default flow, OR needs to be invoked with `--escape-with-payload`)

### Failure 2 (pre-run): Default payload missing

- **Step**: pre-flight (line 77-78)
- **Symptom**: `error: payload not found: dist/mode-debug.efi` when run without args
- **Root cause**: `mode-debug.efi` was never built for the current branch (`mode-fakelocked`). Only `mode-1-auto-debug-verbose.efi` exists in `dist/`.
- **Recommended fix category**: **silent-fallback** — the script's default `PAYLOAD` should either be auto-discovered from `dist/` (pick the most recent `.efi`), or the error message should suggest the correct invocation for the available payload.

## Captured log dir

`/home/vivy/gbl-chainload/logs/20260510-160014_auto_mode-1-auto-debug-verbose_v2.0-plan1/`

```
-rw-r--r-- 169  20260510-160014_device-monitor.log
-rw-r--r-- 209  fastboot-fallback.txt
-rw-r--r-- 118  fastboot-oem-boot-efi.txt
```

Missing (expected but not created because step 3 failed before step 4):
- `bootloader_log`, `dmesg.txt`, `bootconfig`, `cmdline`, `getprop.boot.txt`, `logfs/`, `recovery.props`

## Plan tasks to add

The baseline revealed one category not explicitly called out in Tasks 3–6:

**New Task: payload-flow-mismatch guard** — Before running `oem boot-efi`, probe whether the staged payload is a chainloader (mode-1/fakelocked) vs a direct-boot payload (mode-debug). If it's a chainloader and `--escape-with-payload` is not set, warn and either auto-set the flag or exit with a clear message. Alternatively, document in the script header which payload types work with which invocation flags, and add a pre-flight check that validates the combination.

The missing `mode-debug.efi` is a build gap, not a script gap — but the script could be more helpful: instead of `error: payload not found`, suggest `./scripts/build.sh --mode debug` or list available `dist/*.efi` files.

## Decision for the plan

**Failed**: step 3 failure with a clear, correctly-diagnosed error message (rc=2, explicit "device is back in fastboot" message). The detection machinery works. Tasks 3–6 still apply:

- **Task 3** (Phoenix stopwatch): still needed — the scenario where Phoenix *does* fire (longer-running payloads) isn't guarded.
- **Task 4** (wire stopwatch): same.
- **Task 5** (state probes): the `device_monitor_in_fastboot_quick` + product-string check would let us distinguish sibling-fastboot from stock-fastboot, which is the key diagnostic needed here.
- **Task 6** (replace fallbacks): step 2's `reboot recovery 2>/dev/null || true` is a silent swallow — if reboot fails, the script proceeds silently. Worth hardening.
- **New task** (payload-flow-mismatch guard): add to plan before Task 7.

---

## Validation outcome (Track 0 Task 8) — 2026-05-10

Three back-to-back runs with `dist/mode-1-auto-debug-verbose.efi` (only EFI in `dist/`; `mode-debug.efi` absent from this branch).

- Run 1: **PASS-actionable-failure**
  - Exit code: 2, elapsed: ~16s
  - Dominant cause: payload-flow mismatch — chainloader EFI loaded via `oem boot-efi`, device returned to stock fastboot within ~10s of StartImage handoff. Script detected fastboot fallback at step 3, emitted "expected adb/recovery, but device is back in fastboot" with explicit recovery guidance.

- Run 2: **PASS-actionable-failure**
  - Exit code: 2, elapsed: ~8s
  - Same cause. Additionally: post-step-2 state probe fired ("warn: still in fastboot 2s after oem escape — chainload may have failed"), providing early warning before the step 3 fallback detection.

- Run 3: **PASS-actionable-failure**
  - Exit code: 2, elapsed: ~8s
  - Same cause. Consistent with runs 1 and 2.

Payload-flow guard fired: **yes** — in all 3 runs, the pre-flight warning
`"payload mode-1-auto-debug-verbose.efi looks like a chainloader EFI"` with
`"consider rerunning with GBL_TEST_ESCAPE_WITH_PAYLOAD=1"` appeared before
any device interaction.

Phoenix stopwatch wired: **yes** — `"Phoenix stopwatch started — must reach bootloader within 55s"` appeared in all 3 runs.

Phoenix watchdog ever crossed warn/kill threshold: **no** — all runs completed (or failed) in under 20s, well below the 45s warn threshold.

Note on exit code capture: `./script | tee` in the outer for-loop masked the script's rc (tee exits 0). A subshell with `set -o pipefail` + `PIPESTATUS[0]` confirmed rc=2. The script itself exits correctly; the test harness loop needs `PIPESTATUS` awareness for reliable rc capture.

(Optional) Bonus run with `GBL_TEST_ESCAPE_WITH_PAYLOAD=1`: not performed — the three required runs all classified as PASS-actionable-failure, which satisfies the acceptance criteria.

Track 0 acceptance: **PASS** — each of 3 back-to-back runs exited non-zero with an actionable error message naming the failed step (step 3) and recommending a recovery action (`GBL_TEST_ESCAPE_WITH_PAYLOAD=1` rerun or payload swap). No run hung.
