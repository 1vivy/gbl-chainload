# Next milestone

Milestone marker: **companion module/tooling suite + mode taxonomy cleanup**.

## Objectives kept for this milestone

### 1. Recovery graft

Goal: make custom recovery compatible with mode-1 normal Android boot by grafting stock recovery AVB metadata onto the custom recovery image.

Deliverables:

- Host-side script that takes a custom recovery image plus stock recovery metadata/source image and writes a patched image.
- Device-side recovery-graft module that performs the same operation from on-device stock metadata after a custom recovery flash.

Acceptance:

- Patched custom recovery normal-boots Android under mode-1.
- Recovery boot still works.
- Failure modes are loud and reversible; no autonomous non-HLOS flashing is introduced.

### 2. Cache-ABL build path and OTA ZIP flow

Goal: keep gbl-chainload viable across OTAs that change ABL or remove the direct GBL/EFISP loader path, without asking the agent or normal user flow to flash non-HLOS partitions directly.

Deliverables:

- Define how a known-good ABL is cached into gbl-chainload as a static patch/payload.
- Update the dynamic patch engine so it deliberately skips patching the already-cached ABL payload.
- Add `scripts/build.sh --cache-abl <path>` to embed the cached ABL path into the build output.
- Produce a gbl-chainload ZIP flow for post-OTA custom-recovery installation.
- Document the user-owned fallback file: `/sdcard/backup_abl.img`.

Status:

- Initial `--cache-abl` build/runtime plumbing is implemented.
- gbl-chainload ZIP packaging with recovery-side `/sdcard/backup_abl.img` verification is implemented.
- recovery-graft host tooling and ZIP packaging are implemented.
- mode-2 profile ZIP packaging is implemented as a separate layer on top of cache-ABL; it installs an included profile, validates an existing profile, or generates `/sdcard/gbl-chainload_profile.xml` from `/sdcard/stock_vbmeta.img`.

Acceptance:

- A build with `--cache-abl /path/to/abl.img` produces a gbl-chainload artifact that can use the cached ABL without re-running dynamic patches against that cached payload.
- Builds without `--cache-abl` keep current behavior.
- The ZIP instructions use custom recovery: flash OTA, flash gbl-chainload ZIP, then flash recovery-graft ZIP.
- User fallback naming is stable and documented as `/sdcard/backup_abl.img`.
- EFISP/artifact capacity assumptions are checked before publishing the flow.
- No direct flash of non-HLOS partitions is required from the agent workflow.

### 3. Mode-2 profiles

Goal: turn mode-2 from mechanism into a maintainable profile-driven flow.

Deliverables:

- Decide and document the parked profile format and naming convention; current placeholder: `/sdcard/gbl-chainload_profile.xml`.
- Build/populate the profile from `/sdcard/stock_vbmeta.img` when the profile does not already exist.
- Produce a separate mode-2 ZIP that layers on top of the cache-ABL work rather than replacing it.
- Keep cache-ABL support in mode-2 builds.
- Provide profile validation and clear stale/missing-profile errors.

Acceptance:

- Profile lifecycle is documented around stock vbmeta, vendor blobs, and security patch level.
- Missing profile behavior either populates from `/sdcard/stock_vbmeta.img` or fails closed with exact user instructions.
- Stale profile behavior fails closed and explains what the user must update.
- Mode-2 ZIP output is separate from the mode-1 gbl-chainload/recovery-graft ZIP flow.

### 4. Drop mode-3

Goal: remove never-implemented mode-3 from roadmap, docs, build expectations, and any user-facing mode taxonomy.

Acceptance:

- README and build help do not advertise mode-3.
- Remaining edk2 user-facing mode-3 strings are removed from `FastbootCmds.c` and `FastbootMenu.c` in the edk2 submodule flow.
- Any code comments or tests that imply mode-3 support are removed or rewritten.
- Modes 0, 1, and 2 remain clearly defined.

## Explicitly de-scoped / dropped

- AVB input façade / userspace partition-read façade.
- Synth/graft fastboot command surface (`synthesize-and-flash`, `graft-from-staged`, `fix-vbmeta-footer`).
- Mode-3 as a future feature.
- Old Phase-1 implementation plans/specs.
- RE session transcripts as durable project documentation.

## Suggested implementation order

1. Documentation/code cleanup for mode-3 removal.
2. Cache-ABL static payload design and `--cache-abl` build flag.
3. gbl-chainload ZIP flow that uses `/sdcard/backup_abl.img` as the stable fallback convention.
4. Recovery graft ZIP, because the preferred OTA path is custom recovery OTA flash followed by gbl-chainload ZIP and recovery-graft ZIP.
5. Mode-2 ZIP/profile flow layered on top of cache-ABL, using `/sdcard/gbl-chainload_profile.xml` and `/sdcard/stock_vbmeta.img` conventions unless superseded by implementation evidence.

Each item should land as one or more feature branches with PRs against `main`.
