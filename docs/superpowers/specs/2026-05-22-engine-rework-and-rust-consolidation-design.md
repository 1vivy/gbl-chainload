# Engine rework + Rust tooling consolidation — coordinated PR plan

**Date:** 2026-05-22
**Status:** Design (pre-implementation)

This document coordinates the execution of two adjacent designs as two
sequenced PRs. It does **not** restate their internals — those live in the
originals and stay authoritative for everything they cover.

- `docs/superpowers/specs/2026-05-22-engine-rework-design.md`
- `docs/superpowers/specs/2026-05-22-rust-tooling-consolidation-design.md`

What this doc adds: a locked-in naming scheme, the integration contract
between the two PRs, refinements to the engine-rework manifest after a
design pass (smaller wire format; binary patches drop off the wire entirely),
and the worktree / sequencing plan.

## 1. Why two PRs, in this order

**PR1 — engine rework (C, firmware-side).** Capability-driven hooks, single
EFI, manifest, BlockIo EFISP gate, patch group restructure. Lands first.

**PR2 — Rust tooling consolidation.** Inverts the host C tools and the
firmware's parser libraries to Rust crates plus a single `tools/gbl`
multi-call binary. Captures goldens from PR1's tools as the parity baseline,
then ports — picking up the engine-reworked naming and structure from day
one, no second migration.

This sequence keeps each PR independently reviewable, lets PR1 ship as a
release on its own if PR2 hits friction, and makes PR2's goldens reflect
the intended post-rework behavior of the C tools that survive (`avb-parse`,
`mode2-profile-core`, `fv-unwrap`, `vbmeta-graft`, `gbl-commit`).
PR1's "wasted C" is ~50 lines (the new manifest entry in `PayloadParse.c`
and the `--manifest` flag in `gbl-pack`); everything else in PR1 is
firmware-side and stays C — `ProtocolHookLib` is never Rust-ified.

## 2. Naming scheme (locked in, applies to both PRs)

**Principle.** Names describe **effect**, not **mode**. "Mode 0/1/2"
remains as a *host-side install preset* (UX-stable). The engine speaks
effects only.

### Capability flags (on-device runtime hook gating — the entire vocabulary)

| Manifest bit | C field | Drives | Active when |
|---|---|---|---|
| bit 0 | `WantFakelockHook` | `FakelockOverlay` mutations in `VerifiedBootHook` + `QseecomHook` (mode-1 paths) | host packed mode-1 |
| bit 1 | `WantProfileSpoof` | `ProfileRewrite` mutations in `SpssHook` + `QseecomHook` (mode-2 paths) | host packed mode-2 |

That's it — two bits. No advisory bits, no `Oem` byte on the wire. Patch
group selection and OEM patch selection are pure host-side concerns
(see §5).

### Mutation helpers (in `ProtocolHookLib` — stay C, never Rust-ified)

| Today | Renamed |
|---|---|
| `Mode1Overlay.{c,h}` | `FakelockOverlay.{c,h}` |
| `Mode1Overlay_ClearUnlockBits` | `FakelockOverlay_ClearUnlockBits` |
| `Mode2Rewrite.{c,h}` | `ProfileRewrite.{c,h}` |
| `Mode2Policy_RewriteSpss` (and friends) | `ProfileRewrite_Spss` etc. |

### Protocol hook files (unchanged)

`BlockIoHook` / `ScmHook` / `VerifiedBootHook` / `QseecomHook` / `SpssHook`
— named by protocol, already clean.

### Patch groups (file-per-patch, brand directory above)

```
DynamicPatchLib/
  abl_permissive/                           (was: mode_1/)
    libavb_force_success.c                  (was: patch10)
    fastboot_lock_gates.c                   (was: patch6)
    Signatures.h
  oem/                                       host-only — never compiled into firmware
    oplus/                                  (was: oem/oneplus_canoe.c)
      bypass_warning.c                       (was: patch7)
      Signatures.h
  retired/                                   documentation only — not in any patch table
    block_efisp_recursion.c                 (was: patch1)
    Signatures.h
```

Containment unit = **patch**. Each patch is a self-contained quad
(signatures, anchor logic, apply fn, why-it-works comment). Group
directories collect patches that ship together. Splitting patch10
(libavb-internal) from patch6 (ABL fastboot-dispatcher) reflects that
they're a *pair* — both needed when paired with `FakelockHook` for mode-1
to fakelock end-to-end — but they live in different binaries' codepaths
conceptually, and they're also independently useful for boot-reliability +
recovery flexibility on every install mode.

**Group name `abl_permissive/`** reflects the patches' *effect*: they make
ABL maximally permissive (libavb returns OK for any AVB chain; fastboot
gates relax). The patches do not themselves spoof lock state — the
`FakelockHook` does, and it relies on `abl_permissive` being applied for
its mutations to land cleanly. Naming the patches after their effect
(`abl_permissive`) rather than their pairing (`fakelock_patches`) makes
their universal-application story honest: mode-0 / mode-1 / mode-2 all
benefit from boot reliability, only mode-1 adds the lock-state spoof on top.

`SCOPE_UNIVERSAL` (in `PatchScope.h`) → `SCOPE_ABL_PERMISSIVE`.

**OEM is host-only.** `oem/oplus/` patches are selected by `--oem` at host
packing time and compiled into `abl-patcher` (and PR2's `tools/gbl patch`),
never into the firmware EFI. PR1's `GblChainloadPkg.dsc` excludes the
`oem/` subtree from the firmware `DynamicPatchLib.inf` build; PR2's
`crates/patch-engine` puts `oem/*` modules behind `#[cfg(feature = "host")]`
so the `aarch64-unknown-uefi` build doesn't pull them in. Reasoning lives
in §4 (future OEM additions may have less reliable anchors; misapply risk
at autonomous boot time > miss risk at consenting host time).

**OEM brand naming: `oplus`**, not `oneplus`. The OnePlus / Oppo / Realme
tree is unified enough in practice that the broader brand fits today's
patches; reflects user experience working with these binaries.

### `retired/block_efisp_recursion.c` — holdover

The file stays in tree as documentation / fallback reference. The patch is
**not** registered in any patch table. Top-of-file comment notes the
retirement and points at the `BlockIoHook` EFISP gate that supersedes it.
`tests/host/062_efisp_scan_gate.sh` deleted.

Cost: ~60 lines of unused C. Benefit: reference implementation of the
UTF-16 byte-pattern-anchor style + a zero-rewrite fallback path if the
`BlockIoHook` ever needs revisiting.

### Terminology — "safety hooks" vs patch groups

- **Safety hooks** (or **always-on hooks**) = the `ProtocolHookLib` hooks
  that fire unconditionally in every install: `BlockIoHook` EFISP gate +
  oplusreserve1 gate, `ScmHook` soft-fuse-blow drop. These are what makes
  fallback boot safe regardless of which binary patches landed.
- **`DynamicPatchLib/abl_permissive/`** = the binary patch group applied to
  every cached_abl at host time **and** to slot ABL by the boot-time
  fallback. Distinct from "safety hooks" — these mutate the ABL PE, not
  protocol responses at runtime.

## 3. Refined manifest

```
+0  uint32_t  magic              = 'GMAN'
+4  uint16_t  schema_version     = 1
+6  uint16_t  capability_bits
       bit 0: want_fakelock_hook       (active)
       bit 1: want_profile_spoof       (active)
       bits 2-15: reserved (must be 0)
+8  uint8_t   reserved_pad[8]    = 0
```

16 bytes, two active bits. Validation:

- Unknown bits set → reject (`GBL_PAYLOAD_BAD_MANIFEST`)
- `schema_version != 1` → reject
- Reserved/pad nonzero → reject
- Per-entry SHA-256 (existing GBLP1 mechanism) integrity-checks the whole
  entry

Absence of a manifest entry == all-zero bits == "mode-0 / pure observation"
(safe default; old EFI keeps working with new GBLP1; new EFI keeps working
with old GBLP1).

This supersedes the manifest layout in the engine-rework spec § 2 (which
had an extra advisory bit plus an `Oem` byte). The reasoning is in §4.

## 4. No-cached-abl fallback — dynamic abl_permissive

```
1. EnumeratePartitions, LogFsInit
2. GblPayload_LoadCachedAbl  →  if present: use it
                              else: RunDynamicPatchOnSlotAbl:
                                    read slot ABL → apply abl_permissive group → use result
3. GblPayload_LoadManifest    →  on absent: gManifest = all-zero
4. if (gManifest.WantProfileSpoof) Mode2_SetProfile (if profile present; warn-and-skip
   if absent — existing engine-rework spec § Runtime)
5. ProtocolHook_InstallAll:
     ALWAYS install (safety hooks):
       - BlockIoHook       efisp gate + oplusreserve1 gate
       - ScmHook           soft-fuse-blow drop
     CONDITIONALLY install (gated on manifest only — abl_permissive is now applied
     either way, so the hooks have a coherent ABL to mutate against):
       - VerifiedBootHook  if WantFakelockHook
       - QseecomHook       if (WantFakelockHook || WantProfileSpoof)
       - SpssHook          if WantProfileSpoof
6. LogFsClose; LoadImage(abl); StartImage
```

The fallback applies the `abl_permissive` patch group **unconditionally** and
**only** that group — no OEM patch trying-then-missing on device. Reasoning:

- `abl_permissive` (patch10 + patch6) has solid string anchors verified across
  every test fixture; misapplication risk is bounded.
- OEM groups may grow patches with weaker anchors over time. Even with solid
  anchors, "applied wrongly" is a worse outcome than "cleanly missed and absent."
  The host packer is a consenting context (user picked `--oem`, sees stderr,
  can re-run); the boot-time fallback is autonomous and should stick to the
  rock-solid set. OEM application stays host-only.
- `abl_permissive` is also harmless on every install mode (see §5) — patch10 is
  boot-reliability insurance, patch6 is fastboot recovery flexibility, neither
  touches HLOS's view of lock state. Mode-2's "keep ABL honest" goal is about
  *runtime VerifiedBoot reports*, owned by the absent `FakelockHook` — not by
  these patches.

### Implications

- **Mode-0 = dynamic-patched slot ABL with `abl_permissive` + safety hooks
  + no runtime mutations.** Boots reliably on any AVB-chain state; HLOS
  observes truthfully because no hook fires.
- **Mode-1 cached_abl loss degrades gracefully.** Dynamic-patch reproduces the
  patch state that cached_abl would have carried, `FakelockHook` still works.
- **Mode-2 cached_abl loss degrades gracefully too**, modulo OEM patches
  (orange-state warning + 5s delay re-appears on Oplus until cached_abl is
  restored). Acceptable failure mode — not a boot failure.
- **No "AND cached_abl present" guard** on the runtime hooks. The dynamic-patch
  path reproduces the patches the hooks assume, so the guard is unnecessary.

### What this drops from the engine-rework spec § 2 manifest

- `Oem` byte on the wire — gone. OEM is purely a host-side input to
  `abl-patcher`; the boot-time fallback only ever applies `abl_permissive`.
- `want_fakelock_patches` advisory bit — gone. The patches are no longer
  conditional on a wire bit; they're always applied by the host (mode-0/1/2)
  and unconditionally applied by the fallback.

`RunDynamicPatchOnSlotAbl` stays. `DynamicPatchLib` stays in the firmware
link, restricted to the `abl_permissive` group (oem subtree excluded from
the firmware `.inf` `[Sources]`).

## 5. PR1 scope (engine rework)

**Branch:** `engine-rework` off `main`.
**Worktree:** `.claude/worktrees/engine-rework`.

In scope (delta against the engine-rework design, with the manifest
refinements from §3 and the structural cleanups from §4):

- Single EFI: drop `GBL_MODE` define, single `dist/gbl-chainload.efi`,
  collapse per-mode build loops.
- Manifest type `GBLP1_TYPE_MANIFEST = 0x0020` with the 16-byte payload
  from §3; `GblPayload_LoadManifest()` API; absence → all-zero default.
- Capability-gated hook installs in `ProtocolHook_InstallAll` per §4
  (gated on manifest bits only — no `cached_abl present` clause).
- `BlockIoHook` EFISP gate returning `EFI_NO_MEDIA` (already designed in
  engine-rework spec § 6).
- Mutation helper renames: `Mode1Overlay` → `FakelockOverlay`,
  `Mode2Rewrite` → `ProfileRewrite`. Public API renamed accordingly.
- Patch group restructure (file-per-patch) per §2.
  - `mode_1/` → `abl_permissive/{libavb_force_success.c,fastboot_lock_gates.c}`
  - `oem/oneplus_canoe.c` → `oem/oplus/bypass_warning.c` (host-only — see below)
  - `universal/universal.c` → `retired/block_efisp_recursion.c` (holdover; not
    registered)
- `SCOPE_UNIVERSAL` → `SCOPE_ABL_PERMISSIVE` in `PatchScope.h`.
- `RunDynamicPatchOnSlotAbl` **stays** — applies the `abl_permissive` group
  unconditionally when cached_abl is absent. Per §4, no OEM group attempted
  on-device.
- `DynamicPatchLib` **stays** in firmware link. The `oem/` subtree is
  excluded from the firmware `DynamicPatchLib.inf` `[Sources]` (host tools
  consume it separately via their own makefiles). `EnsureInitScoped` is no
  longer `__HOST_BUILD__`-toggled — same code compiles for both targets,
  but the firmware build only sees `abl_permissive/` patches.
- Drop the post-patch `efisp` invariant scan in `PatchEngine.c` and the
  `efisp_scan.h` warning in `gbl-pack` (defense-in-depth preserved via the
  hook).
- **`abl-patcher` simplifications:** drops `--no-mode1` / `--no-libavb-bypass`
  flags entirely (`abl_permissive` is now always applied to cached_abl on
  every mode — see §4). Keeps `--oem <id>` for OEM patch group selection,
  with `--oem oplus` canonical and `--oem oneplus` as a deprecation alias
  for one release. Mode-0 install scripts stop passing any "skip patches"
  flag.
- `gbl-pack --manifest <bits>` (no oem suffix); emits the 16-byte manifest
  entry.
- `efisp-package.py`: decouple `--oem` from `--mode` (was mode-2-only);
  emit single base EFI + GBLP1 overlay with manifest. Mode-0 install
  continues to pack no cached_abl; the boot-time fallback delivers
  `abl_permissive` (see §4).
- `detect_oem` moved from `mode-2-install.sh` into `install-common.sh`.
- Tests: new `tests/host/` case for the manifest parser
  (absence/present/unknown-bits/bad-schema); delete patch1 / efisp-
  invariant tests (`062_efisp_scan_gate.sh` and any patch1 asserts);
  delete `abl-patcher --no-mode1` argv coverage in `083_abl_patcher_oem.sh`
  (flag is gone — test reduces to `--oem` argv coverage only).
  `088_patch7_multi_abl.sh` rewires to the new path
  (`oem/oplus/bypass_warning.c`) but keeps its three-PE cross-build
  coverage.

Out of scope (deferred to PR2 or beyond):

- Per-patch renames from numbered (`patch6` / `patch7` / `patch10`) to
  descriptive: deferred to PR2 where they become struct names in
  `crates/patch-engine`. The C files in PR1 keep their numbered patch
  name strings inside `kPatches[]` entries; only the file names and group
  directories change.
- Adding new OEM groups beyond `oplus`.

Acceptance:

- `cargo`-less; existing EDK2 + host C builds green.
- New manifest parser tests green.
- On-device boot of mode 0 / 1 / 2 presets against the single-EFI build,
  verified by user.

## 6. PR2 scope (Rust tooling consolidation)

**Branch:** existing `spike/rust-tooling-pilot` (PR #44) — rebased onto
`engine-rework` while in flight, then onto `main` after PR1 merges.
**Worktree:** existing `.claude/worktrees/rust-tooling-pilot`.

Scope is exactly the rust-consolidation design. Inheriting from PR1:

- Manifest support in `crates/gblp1` from day one (entry type 0x0020,
  16-byte payload, same validation rules). 1:1 port of PR1's C parser.
- `crates/patch-engine` ships with the renamed structure
  (`abl_permissive/`, `oem/oplus/`, `retired/`), file-per-patch.
  Per-patch renames from numbered to descriptive names happen here
  inside the crate (e.g., the `kPatches[]` entries become `Patch`
  structs with names like `libavb_force_success`, `fastboot_lock_gates`,
  `bypass_warning`).
- **`crates/patch-engine` builds for both `aarch64-unknown-uefi` and host
  targets** (`crate-type = ["rlib", "staticlib"]`). The firmware staticlib
  excludes the `oem/*` modules via `#[cfg(feature = "host")]` so the EFI
  never compiles in OEM patches it can't safely apply (see §4 reasoning).
  The `retired/` module is feature-gated `host` too — documentation only,
  not in any patch table on either target.
- `tools/gbl pack --manifest <bits>` (no oem suffix — matches PR1).
- `tools/gbl inspect` pretty-prints the manifest's two bits.
- `tools/gbl patch` drops `--no-mode1` / `--no-libavb-bypass` (matches
  PR1 — `abl_permissive` always applied; only `--oem` remains).
- `tools/gbl avb`, `mode2`, `unwrap`, `commit` as in the rust spec.

Goldens captured after PR1 reaches feature-completeness on its branch
(single-EFI build green, manifest emitted/parsed). PR2's parity contract
is against post-rework C output. Refresh allowed if PR1 changes interfaces.

Acceptance unchanged from rust-consolidation spec § 7 pre-merge checklist.

## 7. Integration contract

Items that must match across the two PRs:

1. **Manifest wire format** byte-identical. Two bits, 16 bytes,
   magic / schema / pad rules per §3.
2. **Capability vocabulary** preserved across C and Rust. `WantFakelockHook`
   and `WantProfileSpoof` in C; `want_fakelock_hook` / `want_profile_spoof`
   in `gbl inspect` output and Rust crate APIs.
3. **Single-EFI assumption.** PR1 collapses 3→1; PR2's `scripts/build.sh`
   orchestrates one EDK2 build pass.
4. **`DynamicPatchLib` is split: `abl_permissive` builds for firmware +
   host; `oem/*` is host-only.** PR1's `DynamicPatchLib.inf` `[Sources]`
   excludes `oem/`; PR2's `crates/patch-engine` `#[cfg]`-gates `oem/*` for
   `feature = "host"` only. Both PRs respect this split identically.
5. **On-device binary patching applies `abl_permissive` only.** PR1's
   `RunDynamicPatchOnSlotAbl` is restricted to the `SCOPE_ABL_PERMISSIVE`
   group; PR2 mirrors that restriction in the firmware-target build of
   `crates/patch-engine` (oem modules excluded by cfg).
6. **Patch-group + file naming** locked in per §2. PR2 reproduces this
   structure inside `crates/patch-engine/src/`.

## 8. Goldens timing

PR2 captures goldens from `engine-rework` HEAD once PR1 reaches feature-
completeness (single-EFI build green, manifest round-trips, on-device
mode 0/1/2 verified). Captured into `tests/host/goldens/`. Refresh allowed
if PR1 makes a wire-format or CLI change after the first capture.

Capturing earlier risks freezing goldens for outputs that PR1 then
changes (single-EFI orchestration, manifest emission). Capturing strictly
after PR1 merges means PR2 can't start golden work in parallel; capturing
at PR1 feature-complete is the middle ground.

## 9. Worktree setup

```
.claude/worktrees/engine-rework         NEW — branch engine-rework off main
.claude/worktrees/rust-tooling-pilot    EXISTS — rebase base from main → engine-rework
```

The existing `rust-tooling-pilot` worktree (commit `2b99af8`, the PR #44
spike) gets its base moved from `main` to `engine-rework`. As PR1 lands
commits, the Rust worktree rebases — daily-ish cadence at most, only when
PR1 touches a Rust-relevant file (parsers, host tools, build).

PR1 commits are additive while PR2 depends on the branch (no force-pushes
on already-pushed engine-rework commits). If PR1 wants to rewrite history
for review, coordinate so PR2 isn't mid-rebase.

These mechanics are not load-bearing on the design — they'll be revisited
during plan execution if they start costing real time.

## 10. Out of scope (combined)

Each original spec's "Out of scope" / "Deferred follow-ups" / "Open
questions" lists are unchanged; this doc adds none. In particular:

- Adding non-Oplus OEM groups (Xiaomi, Samsung, etc.).
- `busybox-arm64` replacement (per rust spec).
- Performance work on the Rust port (per rust spec).
- Adding new mutations or behaviours beyond what the C code already
  expresses.

## 11. Done definitions

- **PR1 ready to merge:** user-driven on-device boot of mode 0/1/2 presets
  against the single-EFI build (engine-rework spec § Rollout); new
  manifest parser tests green; existing host tests green (with deletions
  per §5).
- **PR2 ready to merge:** rust-consolidation spec § 7 pre-merge checklist
  (cargo lock review, parity report against goldens, EDK2 firmware sizes
  plausible, full host-test suite green).
- **Combined work done:** both PRs merged; this combined spec retires;
  any deferred follow-ups (per §10) enter the issue tracker.
