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
- [ ] Each device-validation run includes the standard debug bundle from `docs/project/debug-vectors.md`: redacted bootloader-fastboot `getvar` context, EFI UefiLog/logfs excerpt, and recovery ZIP log/manifest when a ZIP is involved.
- [ ] Validation matrix records device region/model, firmware build, active slot, target slot, mode/artifact, RAM-load vs persistent install, stock vs custom recovery, clean vs migrated data state, and old `gbl_root_canoe` history if applicable.
- [ ] edk2 issue #11 (`Escape is borked`) is re-tested from both the physical fastboot menu and `fastboot oem escape`; the release-prep edk2 branch routes menu Escape through the same USB-teardown path as the host command.
- [ ] edk2 issue #10 (`OEM unlock allowed doesn't seem to be working`) is re-tested across reboot/system boot; release-prep now flushes and readbacks the FRP write, but do not call the OEM-unlock UI release-ready until the bit survives the reported flow.
- [ ] edk2 issue #9 (`AVB WARNING unsigned:recovery after successful graft`) is re-tested after a current graft ZIP run; release-prep no longer forces `recovery` to `unsigned` in mode-1, so any remaining warning must be backed by exact `vbmeta:*` getvar evidence.
- [ ] The `zip/` submodule is committed first, then the parent branch records the updated submodule pointer.

## Known release notes / caveats

- `Boot ESP` remains experimental and is not release-gated.
- `profile` ZIP mode is not implemented; mode-2 profile generation is currently host-side TOML tooling plus staged test overlay.
- Current `mode-2.efi` behavior for a missing/invalid profile is an honest boot with a warning in the EFI log, not a hard failure. The future profile ZIP should fail closed before install/use when required profile data is stale or missing.
- Custom recovery normal-boot support depends on the graft flow; publish it only after final device validation.
- Autonomous agent/device testing must stay RAM-loaded. Non-HLOS flashing, lock/unlock commands, active-slot switching, and non-HLOS erases remain user-owned manual actions.
- Do not claim universal fixes for Wallet, already-tampered TEE/RKP state, root-hiding module exposure, or future firmware. Public claims must stay scoped to exact validated modes, firmware builds, regions, and install paths.
