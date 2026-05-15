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
- **Cache-ABL build/runtime plumbing exists.** `scripts/build.sh --cache-abl <abl.img>` host-unwraps and host-patches a supplied ABL/FV image, embeds the patched PE into gbl-chainload, and the runtime deliberately skips dynamic patching for that cached payload. ZIP delivery and on-device validation are still pending.
- **gbl-chainload ZIP packaging exists.** `scripts/make-gbl-chainload-zip.sh` builds a recovery ZIP from a cache-ABL EFI and a host-provided fallback ABL. The recovery installer verifies `/sdcard/backup_abl.img` by SHA-256 before installing the EFI to EFISP.
- **Recovery graft host tooling and ZIP packaging exist.** `scripts/graft-recovery-vbmeta.py` grafts stock recovery AVB metadata/footer onto a custom recovery image; `scripts/make-recovery-graft-zip.sh` packages a grafted recovery image for user-run custom recovery installation.
- **Mode-2 profile ZIP packaging exists.** `scripts/make-mode2-profile-zip.sh` builds a separate mode-2 profile ZIP. The recovery installer requires the cache-ABL fallback convention `/sdcard/backup_abl.img`, consumes an included or existing `/sdcard/gbl-chainload_profile.xml`, and can generate that profile from `/sdcard/stock_vbmeta.img` when missing.
- **Mode-3 is dropped from the roadmap.** It was never implemented and should be removed from user-facing expectations.

## Repo state notes

- Branch policy: feature branches and PRs only; no direct work on `main`.
- Current docs are consolidated under `docs/project/`.
- Historical docs, old Phase-1 plans, and RE session transcripts have been deleted after distillation; `.re-notes/README.md` remains only as a redirect for RE-agent discovery.
- Test fixture documentation remains in `tests/images/README.md` because it describes live test assets rather than project planning.
