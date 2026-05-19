# Debug vectors

These are the standard evidence bundles for release validation and user bug
reports. They are diagnostic-only: collecting them must not require non-HLOS
flashing, lock/unlock commands, active-slot switching, or non-HLOS erases by an
autonomous agent.

## Host / fastboot context

Capture before every RAM-load or ZIP validation run:

- `fastboot getvar all` with serial numbers, IMEI, and account identifiers redacted.
- Explicitly record:
  - bootloader fastboot vs userspace fastbootd (`is-userspace` when present);
  - `current-slot`, `slot-count`, `secure`, `unlocked`, `critical-unlocked`;
  - `product`, `variant`, and firmware build;
  - `has-slot:abl`, `has-slot:vbmeta`, and recovery slot layout if exposed.

This separates real bootloader-fastboot tests from fastbootd mistakes and helps
catch wrong-slot or region/model mismatch reports.

## EFI / logfs evidence

For `mode-0.efi`, `mode-1.efi`, and `mode-2.efi`, capture the relevant
`UefiLog*.txt` / logfs excerpt after RAM-load or persistent install testing.
Useful lines include:

- selected mode (`mode=0/1/2`);
- BootFlow tier: cached ABL path vs dynamic patch path vs fastboot fallback;
- ABL source and size when logged;
- patch summary: applied/missed counts, mandatory misses, worst outcome;
- hook install status;
- payload parse status: no overlay, bad magic, parse failure, cached ABL loaded;
- mode-2 profile status: loaded, missing, invalid, or absent from the overlay;
- fatal path if any: ABL not found, mandatory patch missed, hook install failed,
  `LoadImage` failed, or `StartImage` returned.

These lines diagnose EFISP recursion, OTA/ABL patch drift, stale cached payloads,
and mode-2 profile confusion without changing boot behavior.

## Recovery ZIP environment evidence

For `diag`, `install`, and `graft` ZIP validation, preserve the recovery console
log or saved ZIP log. It should include:

- recovery vs booted-Android context;
- active slot, inactive slot, target slot, and OTA/postinstall state;
- by-name directory used by the ZIP;
- presence and sizes of touched partitions: `efisp`, `abl_a`, `abl_b`,
  `vbmeta_a`, `vbmeta_b`, recovery target partitions, and reserve partitions
  such as `oplusreserve1` / `opporeserve1` when present;
- bundled tool versions or hashes when available.

This catches partition-layout differences, recovery environment problems,
wrong-slot writes, and regional/super-flasher residue.

## Install ZIP manifest items

For install-mode validation, record:

- scenario: OTA install, reinstall, or recovery from backup;
- active slot, inactive slot, and target slot;
- cached ABL source partition and restore target partition;
- whether active/backup ABL contains the EFISP loader marker;
- backup paths created, especially `/sdcard/efisp.bak`, `/sdcard/abl_<slot>.bak`,
  and `/sdcard/backup_abl.img`;
- tool step results for `fv-unwrap`, `abl-patcher`, `gbl-pack`, and
  `gbl-commit verify`.

These fields diagnose OTA survival failures, stale cached ABLs, wrong vulnerable
loader restoration, and EFISP commit/verify failures.

## Graft ZIP manifest items

For recovery-graft validation, record:

- target slot and target partition;
- custom image path and size;
- target block device and partition size;
- main vbmeta source (`vbmeta_<slot>`);
- stock metadata candidate chosen: target partition, `/sdcard/stock_<part>.img`,
  or other slot;
- candidate check result;
- graft output size and offset if the tool reports it;
- post-graft check/list output;
- backup path created.

These fields diagnose custom-recovery normal-boot failures, AVB WARNING reports,
wrong-slot grafts, bad stock metadata sources, and oversized images.

## Mode-2 profile evidence

For host-side mode-2 tooling or the future profile ZIP, record:

- source stock vbmeta path and hash;
- profile version;
- derived lock/color state, OS version, and SPL;
- root-of-trust digest, public-key digest, and VBH digest prefixes or full hashes;
- compiled profile size, which must be 120 bytes;
- compiled profile hash;
- GBLP1 entry type (`0x0010`) when packed;
- EFI log line showing whether the profile was loaded or rejected.

This separates stale-profile issues from Android-side Wallet/root-hiding issues.

## Bug-report minimum

Ask users to include:

- device model, region, and exact firmware build;
- mode/artifact used and whether it was RAM-loaded or persistently installed;
- whether ZIP `diag`, `install`, `graft`, or mode-2 profile tooling was used;
- fastboot context bundle;
- EFI logfs excerpt;
- recovery ZIP log/manifest when applicable;
- slot information;
- whether `/data` was formatted;
- whether the setup came from old `gbl_root_canoe` 3.x/4.x;
- whether custom recovery was installed/grafted;
- whether the symptom is bootchain failure, Play Integrity failure, Wallet-only
  failure, app root detection, or TEE/RKP/Keymaster tamper signal.

Do not treat Wallet-only failures as proof of bootchain failure without the
bootchain and Android-side evidence above.
