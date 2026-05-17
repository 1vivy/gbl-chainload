# ZIP packaging structure — `zip-gbl-chainload`

Date: 2026-05-16
Status: design approved; implementation pending
Companion: `docs/project/zip-methodology.md` — the AnyKernel3 + project-lessons
reference this structure realizes.

## Context

PR #23 landed on-device GBLP1 payload insertion and the recovery toolchain
(`fv-unwrap`, `abl-patcher`, `gbl-pack`, `gbl-commit`). The installer ZIP that
orchestrates those tools unattended was descoped from PR #23 into a separate
line of work — the "ZIP methodology" effort. SP1 of that effort produced
`docs/project/zip-methodology.md`. This spec is **SP2**: the packaging
structure the installer ZIPs are built from. SP3 (the mode-1 gbl-chainload
install ZIP), SP4 (the vbmeta-graft ZIP), and a later mode-2 profile ZIP fill
in the mode bodies this structure defines.

The old layout — a single in-repo `zip/gbl-chainload/` tree assembled by
`scripts/build-recovery-zip.sh` — was removed in commit `848837e`. This spec
replaces it.

## Goal & scope

Deliver the packaging skeleton for all gbl-chainload installer ZIPs: a
dedicated repo, a mode-agnostic installer core, a mode-as-config mechanism,
vendored tool binaries with a refresh-and-verify story, and the build/CI
plumbing. Ship one working no-op mode to prove the core end-to-end.

**In scope (SP2):** the `zip-gbl-chainload` repo and submodule wiring; the
`update-binary` core and `core/*.sh`; the mode-config mechanism;
`update-tools.sh` and the skew guard; the `build-recovery-zip.sh` rework; CI;
the `diag` reference mode.

**Out of scope (later sub-projects):** the `install` mode body (SP3 —
gbl-chainload EFISP install, including the active-slot-ABL vs
`/sdcard/backup_abl.img` selection prompt); the `graft` mode body (SP4 —
vbmeta graft); the `profile` mode body (mode-2). SP2 ships these three as
stubs.

## Architecture

### Repo & submodule

A new repository `zip-gbl-chainload` (GitHub, `1vivy/zip-gbl-chainload`) holds
the ZIP source. It is added to gbl-chainload as a git submodule mounted at
`zip/`, pinned by commit the same way `edk2` is. gbl-chainload's build
consumes the submodule; the submodule never depends on gbl-chainload at the
source level — it only receives built artifacts (see "Vendored binaries").

Rationale for a separate repo over an in-repo tree: the installer is the
user-facing distributable and benefits from an independent version line; the
submodule pin still gives gbl-chainload a single known-good reference. Cost
accepted: a second submodule and the same pointer-push discipline the `edk2`
fork already requires — push the submodule repo before bumping the pointer,
or CI fails.

### Directory layout

```
zip/                                  (= zip-gbl-chainload repo root)
  META-INF/com/google/android/
    update-binary                     mode-agnostic installer core
    updater-script                    one-line dummy marker (recovery
                                      requires the file to exist)
  core/
    env.sh                            BOOTMODE detect, SELinux elevation,
                                      OUTFD / zip / workdir vars
    ota.sh                            OTA-state detection (OTA_POSTINSTALL)
    ui.sh                             ui_print — AK3 /proc/self/fd path-write
    busybox.sh                        per-arch busybox probe + relocate
    partition.sh                      by-name path + active/inactive slot
                                      helpers (project-specific)
    safety.sh                         abort / cleanup / backup-verify wrappers
  modes/                              (SELECTED is build-generated into the
                                      staged tree, not committed)
    diag.conf      diag.sh            no-op reference mode (SP2 ships working)
    install.conf   install.sh         mode-1 EFISP install   — stub; SP3
    graft.conf     graft.sh           vbmeta graft           — stub; SP4
    profile.conf   profile.sh         mode-2 profile         — stub; later
  bin/                                vendored aarch64 tools (committed)
    fv-unwrap  abl-patcher  gbl-pack  gbl-commit  busybox-arm64
    MANIFEST                          provenance: parent commit + per-binary
                                      SHA-256
  base/                               vendored base EFI (committed)
    mode-1.efi
  update-tools.sh                     refreshes bin/ + base/ + MANIFEST from a
                                      gbl-chainload parent checkout
  README.md
  .github/workflows/ci.yml            shellcheck + ZIP-assembly smoke
```

`bin/` and `base/` match the old `zip/gbl-chainload/{bin,base}` layout.

### Mode-agnostic core & mode-as-config

`META-INF/com/google/android/update-binary` contains no mode-specific logic.
Its flow:

1. Parse the recovery contract — `$1` API level, `$2` OUTFD, `$3` zip path
   (methodology doc A1).
2. Unzip itself to a private workdir; `cd` there.
3. Source `core/*.sh` — `env.sh`, `ota.sh`, `ui.sh`, `busybox.sh`,
   `partition.sh`, `safety.sh`.
4. Install the `safety.sh` abort trap.
5. Read `modes/SELECTED` — a one-line file naming the active mode, written at
   ZIP-assembly time. **One ZIP carries exactly one mode**; there is no
   runtime mode prompt, so each ZIP is single-purpose and auditable.
6. Source `modes/<mode>.conf` (declarative configuration) then call
   `modes/<mode>.sh`'s `mode_main`.
7. On success or failure, run cleanup — unmount, restore SELinux context,
   remove the workdir.

`modes/<mode>.conf` is sourced shell holding declarative variables only:
which partitions the mode reads/writes, which tools it invokes, which prompts
it shows. `modes/<mode>.sh` holds the imperative payload step. The split is an
auditability contract — a reviewer reads the `.conf` to learn what a mode can
touch without reading its logic. It mirrors AnyKernel3's split between
`anykernel.sh` configuration and the install commands.

### `core/` provenance — partial AnyKernel3 fork

`core/*.sh` takes AnyKernel3's recovery-environment plumbing close to
verbatim: the `update-binary` arg/workdir/busybox boilerplate, the
per-architecture busybox probe and relocate, `ui_print`'s
`/proc/self/fd/$OUTFD` path-write, `abort` and cleanup, `BOOTMODE` detection,
and SELinux context elevation. This plumbing is battle-tested across many
devices and recovery variants; reproducing it from scratch would risk
edge-case regressions.

AnyKernel3's boot-image machinery — `dump_boot`, `unpack_ramdisk`,
`patch_cmdline`, `patch_fstab`, `flash_boot`, `write_boot`, and the rest — is
**not** taken. gbl-chainload's modes touch raw firmware partitions (`efisp`,
`abl`, `vbmeta`), never a boot image or ramdisk. Carrying unused
partition-flashing code would directly undercut the auditability the
project's safety model depends on.

`partition.sh` (by-name path resolution and active/inactive slot helpers
tuned to `efisp`/`abl`) and the `gbl-commit` backup/verify wrappers in
`safety.sh` are project-specific and written fresh.

Attribution: AnyKernel3 (osm0sis, MIT-style license) is credited in each
forked file's header and in the repo README.

### Vendored binaries, `update-tools.sh`, skew guard

`bin/` holds the aarch64-Android static tool binaries — `fv-unwrap`,
`abl-patcher`, `gbl-pack`, `gbl-commit`, and a static `busybox-arm64` —
committed into the `zip-gbl-chainload` repo (the AnyKernel3 model: a
self-contained installer repo). `base/` holds the base EFI (`mode-1.efi`),
also committed. (`mode-2.efi` is added when the mode-2 profile sub-project
lands `--mode 2` support in `scripts/build.sh`; it is not buildable yet.)

The `bin/` set is purpose-built static tools only. General-purpose binaries
such as `magiskboot` and `avbtool` were considered and rejected:
gbl-chainload's modes never unpack or repack a boot image or ramdisk;
`magiskboot` has no AVB-2.0 vbmeta-descriptor or graft operation (its only
AVB awareness is footer detection while unpacking a boot image); and
`avbtool` is Python (no interpreter ships in recovery) and re-signs rather
than grafts — which `docs/project/vbmeta-graft-vs-construct.md` shows is
impossible without the OEM key. The set grows only as a mode needs it: SP4
is expected to add a purpose-built static `vbmeta-graft` tool to the
recovery toolchain, vendored the same way.

These artifacts are built from gbl-chainload, so the repo carries a refresh
tool and a drift guard:

- `update-tools.sh` runs from inside a gbl-chainload parent checkout. It
  builds the recovery tools (`scripts/build-recovery-tools.sh`) and the base
  EFI (`scripts/build.sh`), copies them into `bin/` and `base/`, and writes
  `bin/MANIFEST`: the parent `git rev-parse HEAD`, a dirty-tree flag, and the
  SHA-256 of every vendored artifact. The operator then commits the submodule
  and bumps its pointer in the parent.
- Skew guard: `scripts/build-recovery-zip.sh` re-hashes `bin/` + `base/` and
  compares against `bin/MANIFEST`; any mismatch is a hard failure
  ("vendored binaries stale — run update-tools.sh"). It also checks that
  `MANIFEST`'s recorded parent commit is the current parent `HEAD` (or an
  ancestor) and refuses a manifest marked dirty-build. gbl-chainload CI runs
  the same check, so a forgotten `update-tools.sh` is caught before a stale
  ZIP can ship.

This is the safety net for the vendored-binary model: binaries live in git
for a self-contained repo, and the MANIFEST + skew guard make drift a loud
build failure rather than a silent stale-ZIP.

### `build-recovery-zip.sh`

`scripts/build-recovery-zip.sh --mode diag|install|graft|profile`:

1. Assert the submodule is checked out; run the skew guard.
2. Stage a temp copy of the submodule tree.
3. Write `modes/SELECTED` with the chosen mode. Prune the other modes'
   `.conf`/`.sh`, and prune any `bin/`/`base/` artifact the chosen mode does
   not declare in its `.conf` (e.g. `graft` ships no base EFI) — each ZIP
   stays minimal.
4. `sha256sum` every staged file into `SHA256SUMS`.
5. `zip -qr dist/gbl-chainload-<mode>.zip .`.

### CI

- `zip-gbl-chainload` repo: `shellcheck` over `update-binary`, `core/*.sh`,
  and `modes/*.sh` in the busybox-`ash` dialect (the methodology doc A3
  portability traps — no `echo -e`, no quoted fd redirection, etc.), plus a
  `build-recovery-zip.sh` dry-run smoke.
- gbl-chainload repo: `build-recovery-zip.sh --mode diag` assembly smoke and
  the `MANIFEST` skew check, added to `tests/runall.sh`'s surface.

## The `diag` reference mode

SP2 ships one fully working mode, `diag`, that exercises the entire core with
zero device writes: it parses args, probes and relocates busybox, emits
`ui_print` output, runs `BOOTMODE` detection, enumerates the by-name
partition table, and exits clean through the normal cleanup path. `diag` is
the end-to-end test vehicle for the core — safe to flash on any device — and
the worked reference for SP3/SP4 mode authors.

## Mode stubs

`install`, `graft`, and `profile` ship as `modes/<m>.{conf,sh}` where
`mode_main` calls `abort "<mode> mode not yet implemented"`. SP3 fills
`install`, SP4 fills `graft`, a later sub-project fills `profile`. The mode
system is open: a new mode is a new `modes/<name>.{conf,sh}` pair plus a
`build-recovery-zip.sh --mode` value — no core change.

The final shape of the `graft` mode — a graft step, a paired before/after
`snapshot`+`graft` arrangement, or dropped entirely — is an SP4 decision,
pending the vbmeta construct-vs-graft analysis in
`docs/project/vbmeta-graft-vs-construct.md`. Because the mode system is open,
adding a `snapshot` mode later costs nothing structurally.

## Data flow — install-time

User flashes `gbl-chainload-<mode>.zip` in custom recovery → recovery invokes
`update-binary` → core sets up the environment and sources the mode →
`mode_main` runs the payload step (writes go through `gbl-commit`'s
backup → write → verify → restore-on-mismatch) → cleanup → recovery reports
status. No host or fastboot involvement, and no agent-side flash — the
project's non-HLOS hard-deny does not gate a user swiping a ZIP in recovery.

## Error handling

`safety.sh` installs an abort trap before any mode runs. On any failure the
mode `abort`s with a human-readable reason; cleanup unmounts, restores the
SELinux context, and removes the workdir; `update-binary` exits non-zero so
recovery shows the failure. The skew guard fails the *build*, not the
install. Per-mode write safety (backup before write, verify after,
auto-restore on mismatch) is the mode body's responsibility via `gbl-commit`
— SP3/SP4 detail it.

## Testing

- Submodule CI: `shellcheck` + assembly dry-run (above).
- Parent CI: `--mode diag` assembly smoke + skew check in `tests/runall.sh`.
- On-device: flash `gbl-chainload-diag.zip` in TWRP/OrangeFox; confirm
  `ui_print` output is visible, the busybox probe succeeds, partition
  enumeration prints, and exit is clean. This validates the core on real
  recovery environments at zero risk before SP3/SP4 add writing modes.

## Out of scope / deferred

- The `install`, `graft`, `profile` mode bodies (SP3, SP4, later).
- The `/sdcard/backup_abl.img` fallback convention and the active-slot-ABL
  selection prompt (SP3).
- Any decision on whether the graft mode survives as designed (SP4, pending
  the vbmeta analysis).
- Publishing tool binaries as GitHub release artifacts — the vendored model
  was chosen instead.
- A CI release job that publishes the assembled `gbl-chainload-<mode>.zip`
  artifacts — deferred until end-user install instructions are formulated,
  then added as the final step of this line of work.

## Open questions

- Should `diag`'s partition enumeration stay quiet about partitions it cannot
  read, or list them with an error marker? Minor; resolve during
  implementation.
- A 32-bit `busybox-arm` is not planned — only `bin/busybox-arm64`. Add a
  32-bit build only if a target recovery is 32-bit; none is known. Defer.
- Whether a `snapshot` "before" mode is worth adding alongside `graft` — see
  the Mode stubs section. Deferred to SP4.
