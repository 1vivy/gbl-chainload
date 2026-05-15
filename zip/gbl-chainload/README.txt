gbl-chainload-installer.zip
===========================

Prerequisites:
  - /sdcard/backup_abl.img must exist (a previously-saved working ABL
    that loads gbl-chainload from EFISP).
  - You are running custom recovery (TWRP).
  - You have either:
      a) just flashed an OTA from custom recovery, OR
      b) rebooted to recovery before letting system-OTA's mid-boot
         finalization run.

Install:
  In TWRP: Install -> gbl-chainload-installer.zip -> swipe.
  At the abort prompt, vol-DOWN within 5s to abort, anything else continues.

This ZIP writes:
  - /dev/block/by-name/efisp          (gbl-chainload + cached ABL overlay)
  - /sdcard/efisp.bak                 (pre-write backup of EFISP)
  - /dev/block/by-name/abl_<inactive> (loader ABL restored from
                                       /sdcard/backup_abl.img)

The EFISP and ABL writes are user-driven (you, in TWRP, swiping to
install this ZIP). The agent-side fastboot-flash hard-deny in this
project's CLAUDE.md does not gate this user action.

Recovery:
  - If EFISP write fails verification, gbl-commit automatically restores
    /sdcard/efisp.bak to EFISP and aborts. Same state you started in.
  - If a future boot fails, hold Vol-Up at the gbl-chainload window to
    reach FastbootLib.
  - If gbl-chainload itself fails to load (corrupted EFISP), hold Vol-Up
    at ABL boot to reach the OEM's native fastboot menu, then either:
      a) fastboot reboot recovery, and re-run this ZIP, or
      b) dd /sdcard/efisp.bak -> /dev/block/by-name/efisp from TWRP shell.
