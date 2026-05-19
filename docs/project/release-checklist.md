# Release checklist

This is the release gate for `gbl-chainload` and its `zip/` submodule. Keep it
short and update it as mode-2 and ZIP validation finishes.

## Release artifacts

- EFI: `dist/mode-0.efi`, `dist/mode-1.efi`, `dist/mode-2.efi`.
- Recovery ZIPs: `dist/gbl-chainload-diag.zip`, `dist/gbl-chainload-install.zip`, `dist/gbl-chainload-graft.zip`.
- Mode-2 profile helper: `tools/mode2-profile/mode2-profile.py` (`derive` TOML, `compile` binary).

## Must pass before tagging

- [ ] `git submodule update --init --recursive` from a clean parent checkout.
- [ ] `bash tests/runall.sh` passes on the release branch.
- [ ] `scripts/build-recovery-zip.sh --mode diag` produces a no-write diagnostic ZIP and validates `SHA256SUMS`.
- [ ] `scripts/build-recovery-zip.sh --mode install` and `--mode graft` assemble with a fresh `zip/bin/MANIFEST`.
- [ ] Device validation logs are captured for mode-0, mode-1, and mode-2 RAM-load tests using only:
  `fastboot stage dist/<artifact>.efi` then `fastboot oem boot-efi`.
- [ ] Device validation logs are captured for ZIP `diag`, `install`, and `graft` on the target recovery environment.
- [ ] The `zip/` submodule is committed first, then the parent branch records the updated submodule pointer.

## Known release notes / caveats

- `Boot ESP` remains experimental and is not release-gated.
- `profile` ZIP mode is not implemented; mode-2 profile generation is currently host-side TOML tooling plus staged test overlay.
- Custom recovery normal-boot support depends on the graft flow; publish it only after final device validation.
- Autonomous agent/device testing must stay RAM-loaded. Non-HLOS flashing, lock/unlock commands, active-slot switching, and non-HLOS erases remain user-owned manual actions.
