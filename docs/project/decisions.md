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

Decision: prefer custom-recovery ZIP deliverables over an autonomous device-side OTA-slot patcher.

User flow:

1. User flashes the OTA from custom recovery.
2. User flashes the gbl-chainload installer ZIP (orchestrates patch + EFISP write).
3. User flashes the recovery-graft ZIP.
4. User keeps a known-good fallback ABL at `/sdcard/backup_abl.img`.

Implementation:

- Cache ABL is NOT a build-time static embed. It is an on-device-generated GBLP1 container appended to `gbl-chainload.efi` on the EFISP raw partition, written via `dd` inside the installer ZIP.
- Runtime locator: `GblPayloadLib` checks for a configuration-table entry installed by the `fastboot oem boot-efi` handler (test path), then falls back to a raw BlockIO read of the `L"efisp"` partition (production path). Single parser, both sources.
- `BootFlow.c` tries the cached payload first (Tier 1), falls through to dynamic patching (Tier 2), falls through to `EnterFastboot` (Tier 3) via `Entry.c`.
- The `--cache-abl <path>` build flag in `scripts/build.sh` is deprecated. The EFI no longer accepts a build-time payload; the GBLP1 container is produced on-device by `tools/gbl-pack` during ZIP installation.
- `DynamicPatchLib`'s post-patch efisp byte-scan gate (Tier 2) and the packer-side efisp scan in `tools/gbl-pack` (Tier 1 / cache path) together enforce the efisp-invariant: any patched ABL that still contains UTF-16 LE `efisp` bytes is rejected before it can cause recursion.

Rationale: on-device generation means the cached ABL is always built from the OTA's actual `abl_<inactive>` bytes, with no host build step required after an OTA. Non-HLOS writes remain user-driven (TWRP ZIP swipe), keeping the agent workflow safe.

Status: the EFI runtime (`GblPayloadLib`, `BootFlow.c`) and the cross-compiled toolchain (`tools/gbl-pack`, `tools/gbl-commit`, the `fv-unwrap`/`abl-patcher` aarch64 targets) ship in the on-device-payload-insertion PR. The installer ZIP that orchestrates steps 1–3 of the user flow above is descoped to a follow-up — the ZIP-methodology line of work; see `docs/project/zip-methodology.md`. Until it lands, the flow is driven manually per `docs/project/recovery-install-validation.md`.

## Cache-ABL container format

Decision: GBLP1 v1 — a versioned TLV container appended to the installed gbl-chainload PE on EFISP.

Format reference: `docs/superpowers/specs/2026-05-15-on-device-payload-insertion-design.md` (GBLP1 v1 byte layout, type codes, runtime validation order, failure-category log lines).

Key properties:

- 8-byte magic (`GBLP1\0\0\0`), 28-byte header with CRC-32, per-entry SHA-256, 8-byte `GBLP1END` footer.
- Type `0x0001` (`cached_abl`) is required; type `0x0010` (`mode2_profile`) is reserved for a future PR.
- Container size cap: 16 MiB. Parser validates header, CRC, footer, per-entry SHA, and PE sanity (AArch64, `EFI_APPLICATION`, `.text` bounds) before returning bytes to `BootFlow.c`.
- Produced by `tools/gbl-pack`; committed to EFISP by `tools/gbl-commit`.

## Mode-2 delivery model

Decision: mode-2 should be a separate ZIP layered on top of cache-ABL work.

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
