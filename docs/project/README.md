# gbl-chainload project notes

This folder is the source of truth for project documentation, reverse-engineering findings, and milestone planning.

## Files

- [`current-state.md`](current-state.md) — what works today, known limits, and repo progress marker.
- [`next-milestone.md`](next-milestone.md) — current milestone objectives, explicit de-scope list, and acceptance criteria.
- [`re-findings.md`](re-findings.md) — distilled reverse-engineering facts that should survive beyond session notes.
- [`decisions.md`](decisions.md) — durable decisions and rejected paths.

## Documentation policy

- Keep current state and next objectives here first.
- Do not keep agent transcripts as durable docs; extract facts into `re-findings.md` or decisions into `decisions.md`.
- Superseded plans are deleted after useful facts are migrated.
- Device-risky test flows remain bounded by `CLAUDE.md`: RAM-load with `fastboot stage dist/<artifact>.efi` and `fastboot oem boot-efi`; do not flash non-HLOS images autonomously.
