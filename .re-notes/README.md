# gbl-chainload RE notes

## Session index

- `sessions/2026-05-07-qseecom-callgraph.md` — QSEE StartApp/SendCmd slot mapping and TA caller inventory.
- `sessions/2026-05-08-km-revalidation.md` — canoe KeyMaster cmd-id revalidation, TA inventory, SPSS/KeyMint follow-ups.
- `sessions/2026-05-08-repo-audit-fastboot-t2-readiness.md` — repo audit, fastboot/logfs retrieval, watchdog, EFISP flashing, T2 readiness.
- `sessions/2026-05-13-abl-unlockrecord.md` — ABL UnlockRecord hash coverage, callers, write-back behavior, and gbl-chainload implications.
- `sessions/2026-05-13-fastboot-loss-state.md` — Post-relock fastboot-loss state isolation: token binding, fastboot gate caller, RPMB boot_info model field.
- `sessions/2026-05-13-oplussec-handover.md` — Handover for OplusSec/RPMB boot_info payload status, mode-0/mode-1 log evidence, and next probes.
- `sessions/2026-05-13-fastboot-gate-static-close.md` — Static close-out for fastboot lock writes, SCM security-state bits, warning-menu selector 8 token-zero path, and preserve-vector spec.
- `sessions/2026-05-13-mode1-writeblock-static-final.md` — Final static prep for mode-1 `oplusreserve1` write-swallow: complete writer table, UnlockRecord init failure path, return-value handling, and hook scope decision.
- `sessions/2026-05-13-user-oplusreserve1-locked-unlocked-diff.md` — Diff of user-provided locked/unlocked `oplusreserve1` images; confirms canonical token block zeroing and adds `scripts/dump-oplusreserve1.py`.
- `sessions/2026-05-14-warm-reset-fastboot-hang-audit.md` — Incident audit for soft-reset Fastboot splash hang; ranks broad mode-0 reserve write-swallow and hook lifecycle/idempotence as leading suspects.
