# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

gbl-chainload is an EFI System Partition (EFISP) chainloader for OnePlus/Oppo devices using Qualcomm's GBL/EFISP load mechanism: it patches the active-slot ABL in memory, installs targeted protocol hooks, and hands off to the patched ABL. Modes: mode-0 (unlocked observation + universal preservation baseline), mode-1 (protocol-hook fakelock via `QCOM_VERIFIEDBOOT_PROTOCOL`), mode-2 (TA-payload spoof at QSEE/SPSS boundaries). Since the v2.3.4 engine rework there is **one EFI** — mode activation is decided at runtime from GBLP1 manifest bits (`WantFakelockHook` 0x0001, `WantProfileSpoof` 0x0002) baked into the EFISP overlay, not from compile flags. (The README's `build.sh --mode N` examples predate this; the flag is gone.)

## Safety: never flash non-HLOS images

**Do not autonomously run** any of:

- `fastboot flash <X>` where `<X>` is **not** one of
  `system`, `vendor`, `product`, `system_ext`, `odm`, `userdata`, `cache`,
  `metadata` (with optional `_a` / `_b` slot suffix).
- `fastboot oem unlock` / `fastboot oem lock`
- `fastboot flashing unlock` / `fastboot flashing lock`
- `fastboot flashing unlock_critical` / `fastboot flashing lock_critical`
- `fastboot --set-active <slot>`
- `fastboot erase <non-HLOS partition>`

These are device-bricking commands. To test gbl-chainload revisions, use
**only**:

```
fastboot stage dist/<artifact>.efi
fastboot oem boot-efi
```

That path is a one-shot RAM load that survives a power cycle without
touching any persistent partition. If a non-HLOS flash is genuinely needed,
surface the proposed command and let the user run it themselves in a real
shell (`! <command>` from the input box).

Where installed, a PreToolUse hook (`.claude/hooks/block-non-hlos-flash.py`,
declared in `.claude/settings.json` `autoMode.hard_deny`) blocks these
patterns regardless of mode — but `.claude/` is gitignored and machine-local,
so the rule above applies everywhere, hook or no hook. It is documented here
so the model doesn't waste turns trying to work around it.

## Workflow: branch then PR

Work happens on feature branches; landing on `main` is via PR.

- Ceremony scales with the change. Use the lightest workflow that fits:
  - **Direct commit to `main`** is fine for a single-commit trivial fix —
    a typo, a one-line shell fix, a comment-only doc tweak — where the
    diff is self-evidently safe. Push directly; no PR.
  - **Single-commit PR** for anything touching multiple files or changing
    behavior in a non-obvious way, even when small.
  - **Multi-commit feature branch + PR** for larger work — iterate freely
    on the branch, the PR grows new commits as feedback comes in.
- Never force-push `main`, even for a typo fix. Push a follow-up commit.
- **Version bumps (`VERSION` + `CHANGELOG.md`) always land as their own
  focused PR — explicit on main, no bundling with feature work.** Use
  `scripts/release.sh X.Y.Z` to scaffold the branch + PR.

This applies regardless of mode. Auto mode does not opt out.

## Commands

```bash
# Full build — docker required (image gbl-chainload-build:latest is built
# on demand on first run). Four phases: firmware staticlibs → EDK2 EFI →
# recovery cross-build → host multicall.
./scripts/build.sh                                   # → dist/gbl-chainload.efi, dist/recovery/gbl, dist/host/gbl
./scripts/build.sh --auto --debug --verbose          # suffixed artifact dist/gbl-chainload-auto-debug-verbose.efi
./scripts/build.sh --no-recovery --no-host           # firmware only

# Host `gbl` multicall only — no docker needed:
cargo build --release -p gbl                         # → target/release/gbl

# Full host CI surface (what CI runs; ordered fast → slow):
bash tests/runall.sh

# Single tests:
bash tests/host/060_pack_roundtrip.sh                # host-tool regression (builds `gbl` itself)
bash tests/047_cleanup_lint.sh                       # top-level lint / patch-harness suites
make -C tests/avb                                    # C unit tests linking the Rust avb-parse staticlib

# Release prep (VERSION bump + CHANGELOG scaffold + zip submodule refresh + PR):
scripts/release.sh X.Y.Z
```

- **Submodules are required**: `git submodule update --init --recursive`. `edk2/` (the fork `1vivy/edk2-gbl-chainload`, providing QcomModulePkg — FastbootLib, BootLib, AvbLib, …) is needed for any firmware build; `zip/` holds the recovery-ZIP deliverable with vendored artifacts.
- Tests **skip cleanly** when a precondition is missing (no docker, PE fixture not yet extracted via `scripts/extract-pe-from-fv.sh`), so a locally green `runall.sh` can still be partial — check for SKIP lines. Large fixtures in `tests/images/` are LFS-tracked.
- CI (`.github/workflows/ci.yml`) is exactly `bash tests/runall.sh` on ubuntu-latest with `liblzma-dev` + `shellcheck`.

## Architecture

**One Rust core, two targets.** The five crates in `crates/` — `pe-utils`, `gblp1`, `mode2-profile-core`, `patch-engine`, `avb-parse` — compile both as host rlibs (linked by the `gbl` multicall in `tools/gbl/`) and as `aarch64-unknown-none` staticlibs linked into the EDK2 firmware through the `*_RustShim.c` FFI shims in `GblChainloadPkg/Library/`. The DSC wires the staticlibs via `DLINK2_FLAGS` (they must land *after* the `--start-group`/`--end-group` block; DLINK_FLAGS is prepended and leaves references unresolved), and `--allow-multiple-definition` lets the per-crate `rust_begin_unwind` panic handlers coexist. The `unknown-none` (ELF) triple — not `unknown-uefi` (COFF) — is required because EDK2's GCC path links with GNU ld. Edit firmware-relevant logic in the crates, not in C shims.

**Firmware** (`GblChainloadPkg/`, an EDK2 UEFI application whose library mappings mirror QcomModulePkg.dsc):

- `Application/GblChainload/Entry.c` — dispatcher: 3s key window, then `BootFlowChainLoad()`; any failure falls through to FastbootLib (the bundled, trimmed edk2 FastbootLib with gbl-chainload getvars and `oem escape` / `oem boot-efi` commands).
- `Application/GblChainload/BootFlow.c` — three-tier chain-load orchestrator:
  - **Tier 1**: `GblPayloadLib` reads a GBLP1 overlay appended after the base EFI — raw BlockIO read of the `efisp` partition in production, or a staged configuration-table buffer on the test path — and boots the cached pre-patched ABL.
  - **Tier 2**: `AblUnwrapLib` extracts the PE from live `abl_<slot>` (arm32-ELF wrapper → FV → LZMA section → nested PE32), then `DynamicPatchLib` (the Rust patch-engine) applies the patch table. A mandatory-patch miss aborts the tier.
  - **Tier 3**: fall through to FastbootLib.
  - Hook-lifecycle ordering is load-bearing: `InstallAll → LogFsClose → LoadImage → StartImage`.
- `Library/ProtocolHookLib/` — hooks layered over ABL's protocol calls: `UniversalBaseline.c` (every build: drop TZ soft-fuse advancement), `BlockIoHook.c` (EFISP read/write refusal always; `oplusreserve1`/`opporeserve1` token-zero write swallow where those partitions exist — inactive on non-oplus devices like Xiaomi myron), `FakelockOverlay.c` (mode-1 locked-view mutation), `ProfileOverlay.c` (mode-2 typed-struct profile), plus QSEECOM/SCM/SPSS/VerifiedBoot hooks.
- `Library/DynamicPatchLib` is scoped by applicability: `abl_permissive/` (always-on, mandatory), `oem/` (host-only, fail-safe), `retired/` — the crate's module tree mirrors this; only `abl_permissive` ships in the device EFI (`--no-default-features` staticlib build).
- `--auto` / `--debug` / `--verbose` are orthogonal build-tuning flags (`GBL_AUTO`/`GBL_DEBUG`/`GBL_VERBOSE` → `-D` defines) affecting only interaction surface and log verbosity — never behavior selection.

**Host tooling.** `tools/gbl` is a single clap multicall that replaced the seven legacy C tools: `gbl unwrap|patch|pack|commit|inspect|mode2 {derive,compile,build}|avb {list,check,graft,list-hash}`, preserving argv shape, exit codes, and byte-for-byte output against captured goldens. `scripts/efisp-package.py` chains unwrap → patch → pack into a ready-to-stage EFISP image. Full subcommand docs live in `tools/README.md`.

**Test layout**: `tests/0??_*.sh` top-level (lints, patch harness, build smoke), `tests/host/0??_*.sh` (host-tool regression; run by the `runall.sh` glob), `tests/avb/` (C unit tests), `tests/host/helpers/` (C harnesses), `tests/images/` (real ABL/recovery/vbmeta fixtures). Rust crates carry their own `tests/parity.rs` locking behavior against the pre-Rust C-tool goldens.

## Logging (`GblChainloadPkg/Include/Library/GblLog.h`)

Three tiers — full classification rules in README:

- `Print(L"...")` — must-see user prompts / fatal errors. Screen + UefiLog, always.
- `GBL_INFO("...")` — semantic hook events (mutations, swallows, crypto outputs on intercepted commands). UefiLog always; screen only with `--debug`.
- `VERBOSE("...")` — raw pass-through traces (every call, hex dumps). Compile-stripped unless `--verbose`.

Format strings are CHAR8 ASCII — no `L` prefix. `%s` consumes `CHAR8*`; `%a` is always ASCII; convert `CHAR16*` via `UnicodeStrToAsciiStrS` into a local buffer first.

## Docs and file locations

`docs/project/` is the single source of truth for durable project documentation: `current-state.md`, `next-milestone.md`, `re-findings.md`, `decisions.md`. Update those files instead of creating new plan piles; extract facts from session transcripts rather than keeping the transcripts (`.re-notes/` is only a redirect there). Test-fixture docs stay in `tests/images/README.md` because they describe live test assets.

Tooling defaults are fine — when a skill, plugin, or external tool has an opinion about where it writes files (e.g., the `superpowers:brainstorming` skill writing specs to `docs/superpowers/specs/`), use that default location. Don't relocate files purely to fit the `docs/project/` "single source of truth" norm; that norm describes durable hand-curated project planning and RE distillations, not tool-managed artifacts.
