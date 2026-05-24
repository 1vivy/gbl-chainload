# Changelog

## v2.3.4 — 2026-05-24

Highlights:

- **Engine rework — one EFI for every install profile.** The three per-mode
  base EFIs (`mode-0.efi`, `mode-1.efi`, `mode-2.efi`) collapse into a
  single `gbl-chainload.efi`. Mode-N selection is now a runtime GBLP1
  manifest bit (`WantFakelockHook` 0x0001, `WantProfileSpoof` 0x0002) read
  by the loader instead of a per-binary compile flag (`GBL_MODE`).
  DynamicPatchLib restructured by intent: `abl_permissive/` (always-on,
  mandatory), `oem/oplus/` (host-only, fail-safe), `retired/`. Patch1's
  EFISP recursion guard retires in favour of a runtime `BlockIoHook` that
  refuses reads/writes on EFISP partitions outright.
- **Rust tooling consolidation.** The seven C host tools (`fv-unwrap`,
  `abl-patcher`, `gbl-pack`, `gbl-commit`, `vbmeta-graft`, `mode2-profile`,
  `gblp1-inspect`) collapse into a single Rust multicall `gbl` binary with
  subcommand dispatch (`gbl <unwrap|patch|pack|commit|avb|mode2|inspect>`).
  Core libraries (PE sanity, GBLP1 parser, AVB walk, DynamicPatchLib's
  always-on `abl_permissive` set, mode-2 profile codec) are now Rust
  crates linked into both the EFI staticlib and the host multicall.
- **`gbl-chainload.efi` is now a release artifact.** Releases ship the
  firmware payload alongside the installer ZIPs and host-tool bundles, so
  fastboot `stage` + `oem boot-efi` testing no longer requires a local
  build.
- **Release confidence checks.** CI now verifies parity between
  freshly-built and zip-vendored artifacts (EFI + recovery `gbl`), plus
  submodule pointer reachability from each submodule's `origin/main`.
  Catches stale vendored artifacts and unpushed submodule commits before
  they ship.
- **One-command release prep.** `scripts/release.sh X.Y.Z` orchestrates
  the VERSION bump, CHANGELOG scaffold, zip submodule refresh, and
  branch + PR creation. Author fills in highlights, merges, pushes tag.

Fixes:

- `gbl commit` now reads the destination back through an uncached
  (`posix_fadvise(POSIX_FADV_DONTNEED)`) path before declaring success.
  Catches blocked writes from kernel write guards (e.g. Baseband Guard
  LSM) that otherwise mask non-persisting writes with cache hits.

Upgrade notes:

- The `--no-mode1` flag and `GBL_MODE=N` build flag are removed; pick the
  install profile via the ZIP (mode-0/1/2-install).
- Host tools: `gbl <sub>` replaces seven binaries. Old per-tool argv shape
  is preserved 1:1 (each subcommand keeps the original CLI / exit codes).
- Test fixtures: `tests/host/goldens/` removed — the Rust impl is
  authoritative post-migration; tests now use parser/roundtrip/schema
  regression checks instead of byte-identity parity.

## v2.2.2 — 2026-06-21

Highlights:

- All ZIPs: Reworked UI, user prompts, and changed up device directory.
- ZIP mode-1 installer: Auto graft recovery.
- Add host tool instructions, and vbmeta graft python wrapper.

Fixes:

- Fastboot menu warns user correctly about ungrafted partitions now.

## v2.2.1 — 2026-05-20

Highlights:

- Diagnostic mode reworked: mode is now read from the GBLP1 overlay (a `MODE2_PROFILE` entry means mode-2) instead of a per-build base-EFI SHA-256 list, so the `unknown-base` label is gone and diag no longer needs a vendored-tool rebuild when an EFI changes.
- diag: removed the `confidence` headline; the `EFISP` / `loader-ABL` / `avb chain` lines (plus the in-bundle raw checks) stand on their own.
- diag: the action line is now a descriptive `avb chain` — `ok` for mode-2/clean, else `<parts> fail verified-boot — could require graft (mode-1 only)`. Stock chained sub-vbmeta (`vbmeta_system`, `vbmeta_vendor`) are excluded since they are never grafted.

Fixes:

- mode-2 installer `detect_oem` is recovery-safe: prefers `getprop`, falls back to `/prop.default`, `/default.prop`, then mounted `build.prop` (recovery has no mounted `system`/`vendor`).

## v2.2.0 — 2026-05-20

Highlights:

- Single-source `VERSION` file drives every consumer (`.dsc`, host tool Makefiles, installer `ui_print`, fastboot menu row, `gbl-chainload_version` getvar, `efisp-package.py --version`, on-screen banner via EDK II `-D` build macro).
- Linux x86_64 host-tool builds (zig musl-static) added alongside Windows/macOS.
- Diagnostic mode shipped: pre-reboot EFISP install confidence + `/sdcard/` bundle, new `gblp1-inspect` tool, `vbmeta-graft list-hash` subcommand.
- Universal TZ rollback-bump drop.
- AVB parser consolidation onto AvbParseLib.
- Release workflow: tag (`v*`) and dispatch triggered, draft GitHub Release with curated + auto-generated notes.

Upgrade notes:

- The hardcoded `GBL_CHAINLOAD_VERSION = 2.0` in `.dsc` is gone — builds now require the `VERSION` file at repo root.
- Host tools accept `--version`; `gbl-pack` no longer self-identifies as `1.0.0`.
- `--tools-dir` flag on `efisp-package.py` is preserved as an alias of the new `--bin-dir`.

## v2.1.0

Mode-2 ZIP implementation, TOML profile migration, EDK II escape fix.

## v2.0.0

Initial 2.x foundation: mode-0/1/2 build pipeline, GBLP1 overlay format, ABL patching toolchain.
