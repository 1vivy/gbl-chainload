# Recovery install — device validation runbook

User-driven validation of the on-device GBLP1 payload insertion flow.
These steps require real hardware and persistent partition writes; they
are NOT agent-runnable (CLAUDE.md keeps the agent on the RAM-only
`fastboot stage` test path).

Spec: `docs/superpowers/specs/2026-05-15-on-device-payload-insertion-design.md`

## Layer 1 — host CI (already automated)

`bash tests/runall.sh` covers host tests 060–068 plus the EDK2 build
smoke. Green before this PR merges. No device needed.

## Layer 2 — agent stage smoke (already exercised)

`fastboot stage dist/mode-1.efi && fastboot oem boot-efi` exercises the
Tier-2 dynamic-patch path (no overlay present). RAM-only, no persistent
write. The agent runs this; it does not validate the EFISP read path.

## Layer 3 — user device validation (this runbook)

### Step 1: EFISP BlockIO read path (smallest persistent write)

The one thing Layer 2 cannot reach: the production `ReadEfispRawBytes`
BlockIO path. Validate it before promoting the installer ZIP.

On the host, build a concat'd test EFI containing a known payload:

```bash
# Run test 060 to generate the payload (if not already present):
bash tests/host/060_pack_roundtrip.sh

# Concat the base EFI with the golden test payload:
cat dist/mode-1.efi tests/host/.last/060/payload.bin > /tmp/installed.efi
```

In TWRP shell, write it to EFISP and reboot:

```bash
adb push /tmp/installed.efi /tmp/installed.efi
adb shell 'dd if=/tmp/installed.efi of=/dev/block/by-name/efisp bs=1M conv=fsync; sync'
adb reboot
```

Expected in the boot log (UefiLog / on-screen if debug build): a line
`gbl-payload: source=efisp-blockio` followed by `BootFlow: loaded ABL via cached`.
That confirms the BlockIO reader + GBLP1 parser + PE-sanity + LoadImage
chain works on real EFISP.

### Step 2: mode-1 fakelock survives the cached path

Boot Android. Confirm Keymaster `SET_ROT` succeeds and normal-boot AVB
reports green — i.e., the protocol hooks installed correctly after the
cached ABL was chainloaded. Same acceptance bar as a normal mode-1 boot.

### Step 3: full installer ZIP, post-OTA

After a real OTA flashed from custom recovery:

```bash
adb push dist/gbl-chainload-installer.zip /sdcard/
# In TWRP: Install -> gbl-chainload-installer.zip -> swipe.
```

Confirm the ZIP completes all 7 steps, reboots, and Android boots with
the cached ABL. Requires `/sdcard/backup_abl.img` present beforehand
(a previously-saved working ABL binary, usually saved from a known-good
boot session before any gbl-chainload installation).

The ZIP writes three targets:
  - `/dev/block/by-name/efisp` (gbl-chainload EFI + cached ABL overlay)
  - `/sdcard/efisp.bak` (pre-write backup of EFISP)
  - `/dev/block/by-name/abl_<inactive>` (loader ABL restored from `/sdcard/backup_abl.img`)

### Step 4: recovery escape hatch

Confirm Vol-Up at the gbl-chainload window reaches FastbootLib, and that
`dd /sdcard/efisp.bak -> /dev/block/by-name/efisp` from a TWRP shell
restores a known-good EFISP if anything goes wrong.

From TWRP shell:

```bash
dd if=/sdcard/efisp.bak of=/dev/block/by-name/efisp bs=1M conv=fsync; sync
adb reboot
```

Confirm the device boots with the restored EFISP (either to Android or
back to the prior boot state).

## Residual gap

The EDK2 `EfispBlockIo.c` BlockIO call and the `LocateOverlay.c`
configuration-table walk are only exercised end-to-end on real hardware
(Step 1 + the agent stage smoke respectively). No host/QEMU harness
covers the live UEFI protocol calls. An OVMF-based EDK2 test could close
this; it is out of scope for this PR (see spec "Open questions").
