# Current state

Progress marker: **v2 shim is usable for mode-0 and mode-1; next milestone is the companion module/tooling suite plus mode taxonomy cleanup.**

## Shipped / working

- `mode-0.efi`: honest unlocked observation plus universal preservation baseline.
- `mode-1.efi`: protocol-hook fakelock through `QCOM_VERIFIEDBOOT_PROTOCOL`; ABL sees locked/green when stock images verify cleanly.
- Universal preservation baseline drops TZ soft-fuse advancement and protects reserve-token zeroing paths without relying on persistent flashing of gbl-chainload itself.
- Mode-1 supports the **stock recovery + custom system** use case by default.
- Reserve token preservation is backed by static RE and user-provided locked/unlocked `oplusreserve1` diffs.

## Delivered in this branch / ready for PR

- **Cache-ABL build/runtime plumbing exists.** `scripts/build.sh --cache-abl <abl.img>` host-unwraps and host-patches a supplied OTA ABL/FV image, embeds the patched PE into gbl-chainload, and the runtime deliberately skips dynamic patching for that cached payload. Cache generation fails closed if the prepared payload still contains LinuxLoader's UTF-16 `efisp` loader label, preventing ABL -> GBL -> cached ABL recursion.
- **Recovery graft host tooling exists.** `scripts/graft-recovery-vbmeta.py` grafts stock recovery AVB metadata/footer onto a custom recovery image. Recovery ZIP packaging has been removed from this PR and should be handled in a separate delivery PR.
- **Mode-3 is dropped from the roadmap.** It was never implemented and should be removed from user-facing expectations.

## Known limits

- ZIP/recovery installation workflows are intentionally not part of this PR. On-device cached ABL/profile insertion needs a separate design and PR after researching insert methods that are testable before flashing.

## Repo state notes

- Branch policy: feature branches and PRs only; no direct work on `main`.
- Current docs are consolidated under `docs/project/`.
- Historical docs, old Phase-1 plans, and RE session transcripts have been deleted after distillation; `.re-notes/README.md` remains only as a redirect for RE-agent discovery.
- Test fixture documentation remains in `tests/images/README.md` because it describes live test assets rather than project planning.
