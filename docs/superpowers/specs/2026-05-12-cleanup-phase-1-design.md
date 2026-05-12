# Cleanup Phase 1 — Lock In Findings, Shed Dead Surface

**Date:** 2026-05-12
**Status:** Draft for review
**Scope:** Three independent sub-PRs that close the recovery-fix exploration cycle and prepare the tree for the Phase-2 on-device module suite (separate spec).

---

## Why this exists

The recovery-failure investigation (`docs/re/aosp-early-avb-bootflow.md`, 2026-05-10) reached a root cause: when mode-1 successfully fakes vbmeta for ABL, **AOSP `first_stage_init` re-walks the vbmeta descriptor tree from disk** and fails on the custom-recovery HASH mismatch. This is not a bootloader bug we can patch from our shim; it is an AOSP-init constraint that has to be fixed by re-shaping the on-disk recovery image *before* first-stage init reads it.

The fix exists, in two committed forms:

- **Host-side:** `scripts/graft-vbmeta-from-stock.py` — already implemented and validated on infiniti (`memory/graft_at_natural_offset_wins.md`). Pastes stock recovery vbmeta at `round_up(custom_image_size, 4 KiB)`; with patch10 in the boot path, ABL emits `verify_result_local=OK` for recovery and patch10 catches the slot-level recoverable error.
- **Device-side:** a Phase-2 on-device companion module that performs the same graft against the on-device stock vbmeta, so users don't need a host every time custom recovery changes.

In addition we built three *exploratory* paths inside the bootloader as we converged on the above:

- `oem synthesize-and-flash` — synthesize unsigned-but-shaped vbmeta on device, flash it.
- `oem graft-from-staged` — paste stock vbmeta bytes at a natural offset (the bootloader-side dual of the host script).
- `oem fix-vbmeta-footer` — in-place footer fix-up.

These on-device fastboot commands served their purpose as experiments. The long-term home for the graft operation is the host script (which already works) and the Phase-2 on-device module (which will). The bootloader shim has no reason to keep carrying the fastboot variants.

This phase removes the now-redundant fastboot surface, re-files what we learned into a status doc that names the committed fix paths, and untangles the log stream — before the Phase-2 module spec begins.

---

## Sub-PRs

This spec covers three sub-PRs, each landing as its own feature branch against `main` per the project's PR-only workflow (`CLAUDE.md`).

### PR-a: Recovery-failure decision doc (P1)

**Goal:** Promote the recovery investigation to an authoritative status doc that names the two committed fix paths (host script + Phase-2 on-device module) and explicitly records why the bootloader shim does not carry the fix.

**Changes:**

1. **Rename** `docs/re/aosp-early-avb-bootflow.md` → `docs/re/recovery-normal-boot-fix-paths.md`.

2. **Add a status banner** at the top:
   > **Status:** Confirmed. Custom-recovery + normal-boot under mode-1 fails in AOSP `first_stage_init` (libfs_avb), which re-walks the vbmeta descriptor tree from disk after our shim has already lied to ABL's KM. The fix has to re-shape the on-disk recovery image *before* first-stage init reads it. Two committed paths: **host-side** `scripts/graft-vbmeta-from-stock.py` (already implemented, validated 2026-05-12 on infiniti), and **device-side** Phase-2 on-device recovery-graft module (TBD spec). The bootloader shim does not carry the fix because the read path it would need to intercept lives in userspace libfs_avb, outside our chainload domain.

3. **Prepend a new "Committed fix paths" section** above the existing "Recommended Mitigations" analysis:

   > #### Host path (available now)
   > `scripts/graft-vbmeta-from-stock.py --partition recovery --in <custom>.img --stock <stock-recovery>.img --out <patched>.img`
   > Pastes stock recovery vbmeta at `round_up(custom_image_size, 4 KiB)`. Patch10 in the boot path turns the slot-level recoverable error into final OK; ABL emits `verify_result_local=OK` for recovery. Required for every custom-recovery flash until the device-side path lands.
   >
   > #### Device path (Phase 2)
   > On-device companion module that performs the same graft against the on-device stock vbmeta, automatically, post-recovery-flash. Lives next to the OTA-cached-ABL module. Spec TBD.

4. **Cross-link from README.md** in the Status section: one sentence noting that mode-1 covers "stock recovery + custom system" by default; if you flash a custom recovery, use `scripts/graft-vbmeta-from-stock.py` (or wait for the Phase-2 device-side module).

5. **Cross-link from `docs/re/avb-input-facade.md`** at the top so anyone landing on the facade doc knows the partition-read facade idea did not graduate into shim code; the path that did graduate is the disk-side graft (host script + Phase-2 module).

6. **Update the existing "Recommended Mitigations" section** in place: under "(a) AVB Partition-Read Facade", add a one-line subsection — *"Not pursued in shim because the read path lives in userspace libfs_avb. Effectively superseded by the disk-side graft; the facade idea remains noted here as a design alternative considered."*

**Out of scope for PR-a:**
- Any code change.
- README architecture rework (deferred to Phase 4).

**Acceptance:**
- `git log --oneline docs/re/recovery-normal-boot-fix-paths.md` shows a single rename commit (rename + edits in one commit is fine).
- README links to the new path.
- No dangling references to the old filename remain (`rg aosp-early-avb-bootflow` returns nothing).

---

### PR-b: Deprecate synthesize/graft fastboot surface (P2)

**Goal:** Remove the synthesize/graft/fix-vbmeta-footer fastboot path from the bootloader, delete `scripts/synthesize-vbmeta.py` (whose only consumer is the on-device synthesize path), and clean the FastbootMenu UI. Keep `scripts/graft-vbmeta-from-stock.py` (the committed host fix path for PR-a) and `tools/abl-patcher`, `tools/fv-unwrap` (Phase-2 module worker tools).

**Changes — edk2 submodule (branch `fastboot/synthesize-and-flash`):**

1. **`edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`** — remove handlers for:
   - `oem synthesize-and-flash`
   - `oem graft-from-staged`
   - `oem fix-vbmeta-footer`
   - Any helper functions that become unreachable (the "synthesize core" extracted in commit `1a41b8f43b`).

2. **FastbootMenu** (also in edk2): remove the menu entries added in `06af9a026e` — *"Fix recovery vbmeta footer"* and *"Escape"* (the latter only existed to bail out of the fix-footer flow).

3. **Keep** the diagnostic surface that the KSU module's testing will still need:
   - `oem boot-efi` (the only sanctioned non-flash test path; see `CLAUDE.md`).
   - `oem vbmeta-status` and the `getvar vbmeta:<p>:expected` family — these read-only helpers remain useful for verifying KSU-cached images.
   - `oem getvar mode`, `oem getvar build-date`, `oem getvar build-flags`.

4. **Submodule bump** in the main repo: bump `edk2` to the new commit; commit message follows the existing `edk2: bump submodule to fastboot/...` style.

**Changes — main repo:**

1. **Delete `scripts/synthesize-vbmeta.py`.** Its only consumer was the on-device `oem synthesize-and-flash` handler being removed in this PR. The graft-from-stock path (which actually works) lives in `scripts/graft-vbmeta-from-stock.py` and is unaffected.

2. **Keep `scripts/graft-vbmeta-from-stock.py`.** It is the host-side committed fix path documented in PR-a. After PR-b lands, this script is the *only* surviving way to graft recovery vbmeta until the Phase-2 on-device module ships.

3. **Keep `tools/abl-patcher/`, `tools/fv-unwrap/`.** Both are Phase-2 module worker tools; do not touch in this PR.

4. **Update `tests/patches/Makefile`** if it references `synthesize-vbmeta.py` (grep first; project memory shows this is a hot file). The graft script does not need test-Makefile updates.

5. **Memory updates** in `memory/`:
   - Mark `graft_at_natural_offset_wins.md` with a one-line banner at the top: *"Confirmed 2026-05-12. Fastboot surface removed in Cleanup Phase 1; technique lives on in `scripts/graft-vbmeta-from-stock.py` and Phase-2 device module."*
   - Mark `avb_constructed_verify_blocked.md` similarly historical: *"Resolved by graft path (see `graft_at_natural_offset_wins.md`); kept here for context on why constructed verify was abandoned."*

**Out of scope for PR-b:**
- Renaming the edk2 branch or force-pushing its history (deferred to Phase 4).
- Touching `abl-patcher` or `fv-unwrap` to be Android-runnable (Phase-2's job).
- Any change to `DynamicPatchLib`, patches 1–10, or the mode-1 protocol hooks.
- Deleting `scripts/graft-vbmeta-from-stock.py` — it is the *primary* host fix path until Phase 2 lands.

**Acceptance:**
- `./scripts/build.sh --mode 1` still builds clean `dist/mode-1.efi`.
- `./scripts/build.sh --mode 0` still builds clean `dist/mode-0.efi`.
- Booting either build via `fastboot stage` + `fastboot oem boot-efi` still works on the test device — that is the only command we use from now on.
- `rg "synthesize-and-flash|graft-from-staged|fix-vbmeta-footer" .` returns hits only inside `docs/`, `memory/`, and `.re-notes/` (history), never in code or build glue.
- `tests/` still passes (`make -C tests` or equivalent).

---

### PR-c: Logging stream split (P3)

**Goal:** Stop flooding `UefiLogX.txt` with our verbose output. `UefiLogX.txt` is owned by QCOM BDS and carries the PBL → XBL → ABL → BDS prelude that is otherwise lost; our DEBUG/INFO spam currently truncates that prelude. Emit our stream into a dedicated file.

**Background:** Memory note `active_investigation_log_flush.md` documents the two-stream design and the `LogFsWrite` auto-flush behavior. This PR completes that design.

**Changes — main repo:**

1. **`GblChainloadPkg/Library/LogFsLib/`** — add a second log handle `gGblBootLogHandle` alongside the existing UEFI-log handle. File naming mirrors UefiLog rotation: `gbl-chainload_Boot{N}.txt`, where `N` is the same boot index UefiLog uses (read it once at init from the UefiLog directory listing, increment alongside).

2. **Route our verbose sinks** (`DEBUG ((EFI_D_INFO, ...))`, our `LOG_PROGRESS`, patch-engine per-byte traces) to `gGblBootLogHandle`.

3. **Keep on UefiLog only:**
   - Errors that should survive a logfs corruption (single-line `EFI_D_ERROR`).
   - The two-line "gbl-chainload entered" / "gbl-chainload exiting (rc=...)" markers, so a reader landing on UefiLog can see we ran without having to also open our file.

4. **Flush contract** stays as documented in `canoe_simplefs_flush_contract.md` — flush both handles on any fastboot-bound path.

5. **`scripts/build.sh`** — no flag changes. `--verbose` keeps verbose; it just lands in a different file now.

**Out of scope for PR-c:**
- Changing log levels (verbosity audit is a separate spec, `2026-05-10-logfs-verbosity-audit-design.md`, which can now actually proceed against the new file).
- Touching the canoe SimpleFS flush mechanism.
- Renaming the existing UefiLog files retroactively.

**Acceptance:**
- After a boot run with `--debug --verbose`, `logs/<run>/logfs/` contains both `UefiLog0.txt` and `gbl-chainload_Boot0.txt`.
- `UefiLog0.txt` shows the QCOM BDS prefix intact (PBL → XBL → ABL framing visible at the top), followed only by our two boundary markers.
- `gbl-chainload_Boot0.txt` carries the patch-engine and protocol-hook verbose output.
- `oem boot-efi` followed by power cycle: both files survive to the host on next fastboot pull.

---

## Cross-cutting

### Order of landing

**PR-a → PR-b → PR-c.** PR-a is doc-only, lands in minutes, and gives PR-b's commit message a `Refs:` target. PR-b is the riskier change (touches the active edk2 fastboot path) — landing it before PR-c keeps the logging change isolated from a build-surface regression. PR-c is internal plumbing; if it regresses, the dev capture is broken but the dev flow is not.

### Branch hygiene

Each PR is its own feature branch off `main`:
- `feature/cleanup-p1-recovery-decision-doc`
- `feature/cleanup-p2-deprecate-synthesize-graft`
- `feature/cleanup-p3-log-stream-split`

The edk2 submodule keeps moving forward on `fastboot/synthesize-and-flash` for PR-b; the branch will be renamed (or merged into edk2 main) in Phase 4, not now.

### Safety

All three PRs respect `CLAUDE.md`:
- No fastboot flash commands.
- No `oem (un)lock`, no `flashing (un)lock_*`.
- Verification path stays `fastboot stage dist/<artifact>.efi` + `fastboot oem boot-efi`.
- Each PR can be smoke-tested against the test device with that two-command pair only.

### What this phase deliberately does NOT do — forward-look at Phases 2–4

**Phase 2 — On-device module suite (separate spec).** Two distinct modules, riding together but conceptually independent:

- **OTA-cached-ABL module.** On post-OTA boot, reads the new ABL from the OTA slot (`/dev/block/by-name/abl_<inactive>`), runs `abl-patcher` against it, and caches the patched output in EFISP for the next chainload. If patches fail, the module fails loud and the user falls back to a manually-stashed backup ABL (e.g., `/data/local/tmp/backup_abl` — module documents how/when the user creates this). Required for the eventual A16→A17 class of OTAs where an ABL may ship with the GBL/EFISP loader path removed; the cached patched ABL is the only way gbl-chainload keeps running across that kind of upgrade.
- **Recovery-graft module.** Performs `scripts/graft-vbmeta-from-stock.py`'s operation on-device, against the on-device stock vbmeta. Triggered when the user flashes a custom recovery. This is PR-a's device-side fix path; the host script remains the workaround until this module lands.

Tools needed (already in tree, no new ones): `tools/abl-patcher` (becomes Android-runnable via NDK/PIE static — Phase-2 work, not Phase-1), `tools/fv-unwrap` (same), `scripts/graft-vbmeta-from-stock.py` (already runs in CPython; Phase-2 reuses its grafting logic in a native module).

**EFISP capacity** is not a constraint at Phase-2 scope. Confirmed `wc -c </dev/block/by-name/efisp = 3,145,728` (exactly 3 MiB) on infiniti. Current `dist/gbl-chainload.efi` is ~564 KiB; stock infiniti ABL is 272 KiB; mode-1 cache (gbl-chainload + cached ABL) totals ~836 KiB, leaving ~2.2 MiB free. Diff/patch encoding for the cached ABL is **deferred** as future optimization, gated on first contradicting size data from another device variant.

**Phase 3 — Mode-2 profiles (separate spec).** Mode-2's TZ-spoof mechanism is already done; what it needs is **per-OTA profiles**: a human-readable (yaml or toml) description of the RoT data mode-2 should assert. Profiles are produced by a host-side build tool and consumed by a new on-device **profile-manager module** that caches them in EFISP next to the cached ABL. Profile lifecycle mirrors vendor blobs + security patch level: when LineageOS (or any Custom ROM) bumps vendor, the profile bumps. Phase 3 also formally **drops mode-3** from `README.md`, `scripts/build.sh`, and any code paths — never implemented, no users. Modes 0, 1, 2 stay.

**Phase 4 — Tree and history cleanup (separate spec).** Final pass after Phases 2 and 3 stabilize:

- Move `docs/` out of the main repo (separate repo, or `.gitignored` notes locally). Our docs lean on personal-rule ambiguity that doesn't belong in version history.
- Rebase `main` to squash exploratory commits.
- Collapse `README.md` to a single short file that reflects the post-Phase-2/3 state.
- Force-push tidy on `edk2` submodule branch: rename `fastboot/synthesize-and-flash` to a name that matches its post-PR-b purpose (or merge to edk2 main).
- Review remaining `FastbootMenu` entries and trim anything not useful after Phase 2 lands.

**Anti-scope for this phase (Phase 1):**

- Does not start any of Phase 2's modules.
- Does not touch mode-2, mode-3, or the build matrix.
- Does not move docs, rebase main, or rewrite README beyond a single Status-section sentence in PR-a.
- Does not force-push or rename the edk2 submodule branch.

---

## Risks

1. **edk2 build break.** Removing `oem synthesize-and-flash` and friends touches the same translation unit as `oem boot-efi`. Mitigation: PR-b runs the full `mode-0` + `mode-1` builds in CI before merge; smoke-test on device via `stage` + `oem boot-efi` before merging.
2. **Hidden caller of `scripts/synthesize-vbmeta.py`.** Mitigation: `rg synthesize-vbmeta` across main repo + edk2 + `tests/` before deleting. The tool is only ~3 commits old (`f1629c5`, `c6a905a`, `8f5796b`); blast radius is small. Do *not* accidentally delete `scripts/graft-vbmeta-from-stock.py` — similar name, very different role.
3. **Log file index race.** If `UefiLog{N}.txt` and `gbl-chainload_Boot{N}.txt` derive `N` independently they can desynchronize after a crash. Mitigation: derive `N` once at init by scanning the logfs root for the highest `UefiLog*` index, then use `N+1` for both files in the new boot.
4. **Memory notes drift.** Marking notes "historical" without deleting them risks future-me trusting stale advice. Mitigation: every historical note gets a one-line banner that names this spec's date, so a reader can find the context.

---

## Verification plan

After all three PRs merge:

1. `./scripts/build.sh --mode 0` and `--mode 1` build clean.
2. `fastboot stage dist/mode-1.efi && fastboot oem boot-efi` boots the test device into Android with KM 0x208 green (same as before this phase).
3. `logs/<latest>/logfs/UefiLog0.txt` shows the QCOM BDS prefix; `gbl-chainload_Boot0.txt` exists and carries our verbose output.
4. `rg -l 'synthesize-and-flash|graft-from-staged|fix-vbmeta-footer'` returns only files under `docs/`, `memory/`, `.re-notes/`.
5. README's Status section links to `recovery-normal-boot-fix-paths.md` and names `scripts/graft-vbmeta-from-stock.py` as the custom-recovery workaround.
6. `git log --oneline main..feature/cleanup-p3-log-stream-split` shows the three feature branches merged in order with their corresponding submodule bumps.

---

## References

- `docs/re/aosp-early-avb-bootflow.md` (to be renamed to `recovery-normal-boot-fix-paths.md`) — recovery failure root cause + two committed fix paths.
- `scripts/graft-vbmeta-from-stock.py` — host-side fix path (PR-a names this; PR-b explicitly preserves it).
- `scripts/synthesize-vbmeta.py` — host-side dead surface (PR-b deletes this).
- `memory/graft_at_natural_offset_wins.md` — graft technique confirmation 2026-05-12.
- `memory/active_investigation_log_flush.md` — two-stream logging design rationale.
- `memory/canoe_simplefs_flush_contract.md` — flush semantics that PR-c must honor.
- `CLAUDE.md` — safety and PR-only workflow rules that bound this phase.
- `tests/images/op15-infiniti-201-abl.img` (278,528 B) — stock ABL size reference for EFISP capacity analysis.
- EFISP partition size on infiniti: `wc -c </dev/block/by-name/efisp = 3,145,728` (3 MiB exactly) — confirmed 2026-05-12.
- Recent edk2 commits (`43e53788e2`, `06af9a026e`, `1a41b8f43b`, `dbb36aca96`) — the surface PR-b removes.
- Recent main commits (`b26686e`, `19aad06`, `9599bc9`, `fc7b332`, `7a7e8e7`) — the host-side surface PR-b prunes.
