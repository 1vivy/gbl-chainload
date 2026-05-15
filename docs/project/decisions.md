# Decisions

## Docs consolidation

Decision: `docs/project/` is the single source of truth for project documentation, RE findings, and milestone planning.

Consequences:

- Old `docs/re/`, `docs/superpowers/plans/`, `docs/superpowers/specs/`, and `.re-notes/sessions/` content is distilled here and then removed. `.re-notes/README.md` remains only as a pointer into `docs/project/`.
- Future agent-orchestrated work should update `docs/project/current-state.md`, `docs/project/next-milestone.md`, `docs/project/re-findings.md`, or this file instead of creating isolated plan piles.

## Recovery fix path

Decision: fix custom recovery normal-boot by disk-side recovery AVB metadata grafting.

Rejected:

- In-memory AVB input façade.
- Userspace partition-read façade from the bootloader shim.
- Bootconfig/cmdline digest rewriting as the primary fix.

Rationale: the failing read happens in userspace AVB / first-stage init after ABL. Reshaping the on-disk recovery image addresses the data userspace reads rather than adding an unreachable bootloader-stage hook.

## Synth/graft fastboot commands

Decision: drop the abandoned fastboot command surface (`synthesize-and-flash`, `graft-from-staged`, `fix-vbmeta-footer`).

Rationale: the path was exploratory, did not land as the selected product surface, and is superseded by host/device recovery graft deliverables.

## Mode taxonomy

Decision: modes 0, 1, and 2 remain; mode-3 is dropped.

Rationale: mode-3 was never implemented and should not consume roadmap or user-facing documentation space.

## OTA / cache-ABL delivery model

Decision: keep this PR limited to host-built cache-ABL build/runtime support and safety gates. ZIP/recovery delivery and on-device ABL/profile insertion are deferred to a separate PR.

Implementation direction:

- Cache ABL into gbl-chainload as a static patch/payload.
- Teach the dynamic patch engine to deliberately skip the cached ABL payload.
- Add `scripts/build.sh --cache-abl <path>` to produce cache-ABL builds.

Rationale: the static cache-ABL path is validated enough for build/runtime review. On-device cache/profile insertion still needs research into methods that can be tested before any non-HLOS write.

Safety gate: every cached ABL preparation path must verify that the final prepared ABL payload no longer contains the UTF-16 `efisp` loader label. If the label remains after patching, the flow must abort before producing the EFI, because LinuxLoader's EFISP GBL probe can otherwise recurse through gbl-chainload.

BootFlow ordering: load/register the nested patched ABL image before installing protocol hooks, then install hooks immediately before `StartImage`. This keeps nested-ABL `LoadImage` failures from dirtying global protocol tables before gbl-chainload falls into its recovery surface. A future hook rollback/uninstall path is still desirable for hook-install failures or unexpected `StartImage` returns.

## Mode-2 delivery model

Decision: mode-2 profile insertion should be handled in the same later on-device insertion PR as OTA cache-ABL delivery, not in this PR.

Conventions to validate during implementation:

- Park profile at `/sdcard/gbl-chainload_profile.xml`.
- If no profile exists, build/populate it from `/sdcard/stock_vbmeta.img`.
- Keep cache-ABL support in mode-2 builds.

Rationale: mode-2 needs the same ABL survival path as mode-1, plus profile-specific setup and validation.

## RE notes policy

Decision: keep distilled facts, not session transcripts.

Rationale: transcripts are useful during investigation but harmful as long-term source of truth because they preserve stale hypotheses beside resolved findings.

## Safety boundary

Decision: agent-run testing stays RAM-loaded.

Allowed test path:

```text
fastboot stage dist/<artifact>.efi
fastboot oem boot-efi
```

Rejected for autonomous agent execution: flashing non-HLOS partitions, lock/unlock commands, active-slot switching, and non-HLOS erases.
