# 2026-05-14 warm-reset Fastboot splash hang audit

## Context

- Incident log set: `logs/20260514-143945_manual_manual_v5c1aba8/`.
- Prior comparison log set: `logs/20260514-123512_manual_manual_v581ea11/`.
- User reported the failure path was all soft resets, ending in a `Fastboot Mode` splash hang where no useful final log flush is expected.
- Current PR #18 head during the incident: `5c1aba8` (`Print token-zero preservation event`).

## Runtime evidence

- Mode-0 chainload and hook install completed in the incident logs.
  - `bootloader_log:952`: `BootFlow: start (mode=0)`.
  - `bootloader_log:966`: `ProtocolHookLib: installed (mode=0, vb=1/1 scm=1/1 qsee=1/1 spss=1/1 blockio=1/1)`.
- Universal SCM soft-fuse drops occurred.
  - `bootloader_log:84`, `bootloader_log:1054-1055`: `scm-sip | smcid=0x02000801(TZ_BLOW_SW_FUSE_ID) | DROPPED (universal)`.
- Mode-0 BlockIo hook swallowed `oplusreserve1` writes beyond the canonical token-zero LBA.
  - `bootloader_log:92`, `bootloader_log:95`, `bootloader_log:97`: `blockio | op=write-swallow | reason=reserve-write | p=oplusreserve1 | lba=1080/1088 ... | status=Success`.
- No direct evidence was found for the intended token-zero path in this log set.
  - No `reason=token-zero-write`.
  - No `GBL: intercepted reserve token zeroing` print.
- Recovery/fastboot flow was visible, but no explicit panic/assert/watchdog-reset source was found.
  - Strongest runtime clue from log audit: `UefiLogSaved4.txt:1142-1144` contains `UEFI boot timeout need feedback!`.

## Code audit findings

### Most credible suspect: broad mode-0 reserve write-swallow

- `BlockIoHook.c` currently returns `EFI_SUCCESS` for every write to matched `oplusreserve1`/`opporeserve1` handles.
- The incident logs show swallowed writes at LBAs that do not match the canonical token-zero block (`LastBlock - 0x3a5 = 1114`).
- Prior static writer table showed other legitimate `oplusreserve1` users, including boot feedback / charge / UART / UnlockRecord paths.
- This can plausibly interfere with Phoenix/boot-feedback state and create a repeated boot-count/timeout/fastboot path without a conventional crash.
- Confidence: Medium-high.

### Credible design risk: hook lifetime escapes intended chainload window

- VerifiedBoot, SCM, QSEECOM, SPSS, and BlockIo hooks patch protocol function pointers in place without a restore/uninstall path.
- If `StartImage()` returns or local FastbootLib is entered after a failed chainload, the process continues with hooks still installed.
- Re-entering `oem boot-efi` in the same boot-services session can load a fresh GBL image whose statics do not know that protocol slots are already wrapped.
- This is more credible than ordinary C globals surviving a real warm reset; the higher-risk case is same-session fastboot/chainload relaunch and double wrapping.
- Confidence: Medium.

### Lower-confidence suspects

- Generic memory exhaustion/leak: no direct memory pressure evidence in logs. Hook stacking could act like a leak, but lifecycle/idempotence would be the root cause.
- SCM `TZ_BLOW_SW_FUSE` drop: returning success without fully shaped result words is a correctness risk, but direct connection to the Fastboot splash hang is weaker than BlockIo/lifecycle.
- Partial hook install mixed-state: real design issue, but incident logs show successful hook install, so not the leading explanation for this event.

## Scope decision

- Treat broad chainloading lifecycle hardening as a separate branch/PR from PR #18.
- User decision: keep PR #18 as-is; do not fold broader safety-audit changes or mode-0 policy changes into it.
- Candidate separate PR content:
  - hook uninstall/rollback API;
  - transactional `ProtocolHook_InstallAll()`;
  - same-session install registry/config table or robust already-hooked detection;
  - cleanup before fallback FastbootLib and after `StartImage()` returns;
  - instrumentation for hook slot originals/current pointers, install counts, and fastboot/USB fallback state.
- PR #18 should remain focused on unlock-token preservation and mode policy unless a narrow corrective change is needed to avoid broad mode-0 reserve write-swallow.

## Recommended mitigation split

1. **PR #18:** keep current token-preservation work unchanged.
2. **Separate branch/PR:** lifecycle/idempotence/rollback hardening and instrumentation.
3. **Safety audit:** broader repo audit for safe chainloading practices, including whether mode-0 reserve write policy should be narrowed later.
4. **Diagnostics matrix:** build variants for `mode0-patch-only`, `mode0-log-only`, `mode0-narrow-blockio`, and current all-hook mode-0 control.

## Ruled out / not yet proven

- No log evidence of actual token-zero interception in the incident run.
- No explicit firmware panic/assert line identified.
- No direct memory exhaustion signature identified.
- No evidence that final Fastboot splash hang can produce reliable logs after hard reset escape.

## Next steps

- Open a separate branch for hook lifecycle hardening and repo-wide chainload safety audit when ready.
- Avoid device-side experiments until code blast radius is reduced or diagnostics are intentionally scoped.

## 2026-05-14 implementation start: `feature/hook-lifecycle-safety`

- Created separate branch: `feature/hook-lifecycle-safety`.
- Implemented first hook-lifecycle safety slice:
  - public `ProtocolHook_UninstallAll()` cleanup API;
  - per-hook uninstall routines for VerifiedBoot, SCM, QSEECOM, SPSS, and BlockIo;
  - rollback on required `ProtocolHook_InstallAll()` failures;
  - cleanup after `LoadImage()` failure and after unexpected `StartImage()` return;
  - hooks remain installed for successful `StartImage()` handoff, including ABL FastbootLib diagnostic paths;
  - same-session config-table registry to detect active hooks and avoid blind double-wrapping;
  - cross-mode registry reuse fails closed so mode-0 hooks are not reused for mode-1 or vice versa;
  - registry installation failure fails closed and rolls back hooks;
  - uninstall routines preserve state and leave the registry active if slots no longer point at this image's wrappers, preventing unsafe cleanup under an outer wrapper.
- Added lifecycle lint:
  - `tests/048_hook_lifecycle_lint.sh`;
  - wired into `tests/runall.sh`.
- Validation performed:
  - `git diff --check` passed.
  - `tests/045_mode_taxonomy_lint.sh` passed.
  - `tests/046_mode1_protocol_hook_lint.sh` passed.
  - `tests/047_cleanup_lint.sh` passed.
  - `tests/048_hook_lifecycle_lint.sh` passed.
  - `./scripts/build.sh --mode 0` passed.
  - `./scripts/build.sh --mode 1` passed.
- Review notes:
  - Oracle review flagged missing cross-image idempotence and unsafe state clearing; fixed by adding config-table registry and deferred uninstall behavior.
  - Follow-up review flagged cross-mode registry reuse and ignored registry install status; fixed by failing closed on mode mismatch and rolling back if `InstallConfigurationTable()` fails.
