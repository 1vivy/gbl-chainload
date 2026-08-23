# Current state

Progress marker: **v2 shim is usable for mode-0 and mode-1; mode-2 ships host-side for Xiaomi myron (sm8850); next milestone is the companion module/tooling suite plus mode taxonomy cleanup.**

## Shipped / working

- `mode-0.efi`: honest unlocked observation plus universal preservation baseline.
- `mode-1.efi`: protocol-hook fakelock through `QCOM_VERIFIEDBOOT_PROTOCOL`; ABL sees locked/green when stock images verify cleanly.
- **Mode-2 on Xiaomi myron (sm8850), host-side flow** — see [`myron-mode2.md`](myron-mode2.md). One-EFI + GBLP1 overlay (`WantProfileSpoof` only) built via `scripts/build.sh --board myron` + `scripts/efisp-package.py --mode 2`; the mandatory `abl_permissive` contract (patch6 + patch10) is asserted on the myron ABL by test 088, and the full derive/package chain by test 097. The oplus-specific firmware hard requirements were relaxed for non-oplus devices (BlockIo reserve preservation deactivates when no oplus reserve partition exists; a missing SPSS protocol degrades to QSEE-KM-only spoof). On-device validation is pending the user run per the runbook.
- Universal preservation baseline drops TZ soft-fuse advancement and protects reserve-token zeroing paths without relying on persistent flashing of gbl-chainload itself.
- Mode-1 supports the **stock recovery + custom system** use case by default.
- Reserve token preservation is backed by static RE and user-provided locked/unlocked `oplusreserve1` diffs.

## Known limits

- **Custom recovery + normal Android boot is not fixed yet.** The selected fix is a disk-side recovery vbmeta/footer graft, delivered by host tooling and/or an on-device module.
- **Cache-ABL: the on-device GBLP1 overlay model is implemented.** `GblPayloadLib` reads the appended overlay from the EFISP raw partition (production) or a staged configuration-table buffer (test path); `BootFlow.c` tries the cached payload (Tier 1) before falling back to dynamic patching (Tier 2). The host packer (`tools/gbl-pack`), the EFISP writer (`tools/gbl-commit`), and the aarch64-Android cross-compiled toolchain (NDK r27, `scripts/build-recovery-tools.sh`) are built and tested. On-device validation (B2–B6) confirmed both overlay-source readers and the cached-ABL boot path.
- **The recovery installer ZIP is a follow-up.** It orchestrates the post-OTA flow (read `abl_<inactive>` → fv-unwrap → patch → pack → concat → SHA-verified `dd` to EFISP) unattended. Descoped from the on-device-payload-insertion PR to its own line of work; see `docs/project/zip-methodology.md`. Until it lands, the flow is driven by hand per `docs/project/recovery-install-validation.md`.
- **Mode-2 profile lifecycle is not implemented yet.** Mode-2 should consume a parked profile such as `/sdcard/gbl-chainload_profile.xml`; if missing, the mode-2 ZIP can build/populate it from `/sdcard/stock_vbmeta.img`. (Mode-2 itself works host-side today — the derive→pack→flash flow on canoe and myron — what is missing is the on-device ZIP lifecycle.)
- **Myron mode-2 on-device validation is pending.** The host-side flow is complete and test-locked, but the KM/SPSS wire offsets were cross-checked against canoe headers only; the runbook's log checkpoints cover the residual risk.
- **Mode-3 is dropped from the roadmap.** It was never implemented and should be removed from user-facing expectations.

## Repo state notes

- Branch policy: feature branches and PRs only; no direct work on `main`.
- Current docs are consolidated under `docs/project/`.
- Historical docs, old Phase-1 plans, and RE session transcripts have been deleted after distillation; `.re-notes/README.md` remains only as a redirect for RE-agent discovery.
- Test fixture documentation remains in `tests/images/README.md` because it describes live test assets rather than project planning.
