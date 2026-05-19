# Current state

Progress marker: **mode-0/mode-1 are usable; mode-2 mechanism and TOML profile tooling are present; release readiness is tracked in `docs/project/release-checklist.md`.**

## Shipped / working

- `mode-0.efi`: honest unlocked observation plus universal preservation baseline.
- `mode-1.efi`: protocol-hook fakelock through `QCOM_VERIFIEDBOOT_PROTOCOL`; ABL sees locked/green when stock images verify cleanly.
- `mode-2.efi`: TA-payload spoof mechanism; profile injection uses a 120-byte `gbl_mode2_profile` packed as GBLP1 type `0x0010`.
- `tools/mode2-profile`: derives a human-editable TOML profile from stock `vbmeta.img` and compiles it to the 120-byte binary profile.
- Universal preservation baseline drops TZ soft-fuse advancement and protects reserve-token zeroing paths without relying on persistent flashing of gbl-chainload itself.
- Mode-1 supports the **stock recovery + custom system** use case by default.
- Reserve token preservation is backed by static RE and user-provided locked/unlocked `oplusreserve1` diffs.

## Known limits

- **Custom recovery + normal Android boot is not fixed yet.** The selected fix is a disk-side recovery vbmeta/footer graft, delivered by host tooling and/or an on-device module.
- **Cache-ABL: the on-device GBLP1 overlay model is implemented.** `GblPayloadLib` reads the appended overlay from the EFISP raw partition (production) or a staged configuration-table buffer (test path); `BootFlow.c` tries the cached payload (Tier 1) before falling back to dynamic patching (Tier 2). The host packer (`tools/gbl-pack`), the EFISP writer (`tools/gbl-commit`), and the aarch64-Android cross-compiled toolchain (NDK r27, `scripts/build-recovery-tools.sh`) are built and tested. On-device validation (B2–B6) confirmed both overlay-source readers and the cached-ABL boot path.
- **Recovery ZIP release status is per-mode.** The ZIP core and diag mode are safe/no-write. Install and graft modes exist in the `zip/` submodule and need final device release validation. Profile mode remains the mode-2 follow-up.
- **Mode-2 profile lifecycle is TOML-based.** Host tooling uses `gbl-chainload_profile.toml`. Current EFI behavior for missing/invalid profile data is an honest boot with a warning; the future release profile ZIP should consume or populate `/sdcard/gbl-chainload_profile.toml` from `/sdcard/stock_vbmeta.img` and fail closed before install/use on stale or missing profile data.
- **Release/debug evidence is standardized.** Use `docs/project/debug-vectors.md` for fastboot context, EFI logfs excerpts, recovery ZIP manifests, graft evidence, and mode-2 profile evidence before making public stability claims.
- **Mode-3 is dropped from the roadmap.** It was never implemented and should be removed from user-facing expectations.

## Repo state notes

- Branch policy: feature branches and PRs only; no direct work on `main`.
- Current docs are consolidated under `docs/project/`.
- Historical docs, old Phase-1 plans, and RE session transcripts have been deleted after distillation; `.re-notes/README.md` remains only as a redirect for RE-agent discovery.
- Test fixture documentation remains in `tests/images/README.md` because it describes live test assets rather than project planning.
