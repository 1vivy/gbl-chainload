# Current state

Progress marker: **v2 shim is usable for mode-0 and mode-1; next milestone is the companion module/tooling suite plus mode taxonomy cleanup.**

## Shipped / working

- `mode-0.efi`: honest unlocked observation plus universal preservation baseline.
- `mode-1.efi`: protocol-hook fakelock through `QCOM_VERIFIEDBOOT_PROTOCOL`; ABL sees locked/green when stock images verify cleanly.
- Universal preservation baseline drops TZ soft-fuse advancement and protects reserve-token zeroing paths without relying on persistent flashing of gbl-chainload itself.
- Mode-1 supports the **stock recovery + custom system** use case by default.
- Reserve token preservation is backed by static RE and user-provided locked/unlocked `oplusreserve1` diffs.

## Known limits

- **Custom recovery + normal Android boot is not fixed yet.** The selected fix is a disk-side recovery vbmeta/footer graft, delivered by host tooling and/or an on-device module.
- **Cache-ABL: the on-device GBLP1 overlay model is implemented.** `GblPayloadLib` reads the appended overlay from the EFISP raw partition (production) or a staged configuration-table buffer (test path); `BootFlow.c` tries the cached payload (Tier 1) before falling back to dynamic patching (Tier 2). The host packer (`tools/gbl-pack`), the EFISP writer (`tools/gbl-commit`), and the aarch64-Android cross-compiled toolchain (NDK r27, `scripts/build-recovery-tools.sh`) are built and tested. On-device validation (B2–B6) confirmed both overlay-source readers and the cached-ABL boot path.
- **The recovery installer ZIP is a follow-up.** It orchestrates the post-OTA flow (read `abl_<inactive>` → fv-unwrap → patch → pack → concat → SHA-verified `dd` to EFISP) unattended. Descoped from the on-device-payload-insertion PR to its own line of work; see `docs/project/zip-methodology.md`. Until it lands, the flow is driven by hand per `docs/project/recovery-install-validation.md`.
- **Mode-2 profile lifecycle is not implemented yet.** Mode-2 should consume a parked profile such as `/sdcard/gbl-chainload_profile.xml`; if missing, the mode-2 ZIP can build/populate it from `/sdcard/stock_vbmeta.img`.
- **Mode-3 is dropped from the roadmap.** It was never implemented and should be removed from user-facing expectations.

## Repo state notes

- Branch policy: feature branches and PRs only; no direct work on `main`.
- Current docs are consolidated under `docs/project/`.
- Historical docs, old Phase-1 plans, and RE session transcripts have been deleted after distillation; `.re-notes/README.md` remains only as a redirect for RE-agent discovery.
- Test fixture documentation remains in `tests/images/README.md` because it describes live test assets rather than project planning.
