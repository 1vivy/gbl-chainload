# Xiaomi myron (sm8850) — mode-2 bring-up runbook

Device: Redmi myron (K90-family), Snapdragon 8 Elite Gen 5 (sm8850 — same
SoC family as the canoe/OPlus targets), HyperOS OS3.0.23 / Android 16,
SPL 2026-01-01. This is the first non-OPlus mode-2 target.

What mode 2 gives myron: **custom firmware with stock-looking
attestation inputs**. The ABL remains unlocked and retains its unlocked
warning (no orange-state-hiding patch), but its BCC DICE mode is forced
to Normal. The QSEE/SPSS hooks rewrite the KeyMaster / SPU payloads in
flight with values derived from the stock vbmeta (`is_unlocked=0`, color
GREEN, stock ROT/public-key/VBH digests). This is not proof that remote
attestation or RKP will accept the resulting chain. The stock bootloader
flow that the user previously validated via the gbl_root_canoe
"unlockmodefix" static patch only worked on stock firmware; mode 2 is
the custom-ROM path.

Scope note: this is the **host-side flow** (build → package → flash by
hand). The recovery-ZIP / on-device profile lifecycle remains deferred
(see `next-milestone.md` objective 3).

## Host-side facts (already locked in by tests)

- `tests/images/xi-myron-OS3.0.23-abl.img` is the stock OS3.0.23 ABL
  (ELF→FV→LZMA→PE32, byte-identical structure to the OPlus fixtures);
  `gbl unwrap` handles it directly.
- The mandatory `abl_permissive` contract holds on it: patch6 (fastboot
  lock-gate strings) and patch10 (libavb force-success) both apply;
  patch7 (OPlus orange-warning) cleanly misses — asserted by
  `tests/host/088_patch7_multi_abl.sh`.
- Myron's stock vbmeta has only GKI-era `init_boot.*` property
  descriptors (no `boot.*`); `gbl mode2 derive` falls back to
  `init_boot.{os_version,security_patch}` — asserted by
  `tests/host/097_myron_mode2_package.sh`, which also pins the derived
  profile (OS "16" → `0x40000`, SPL "2026-01-01" → `0x9a1`,
  locked/GREEN).
- QSEECOM (`a74862ce-…`) and SPSS (`a322ff2c-…`) protocol GUIDs are both
  present in the myron ABL PE, so the mode-2 hooks have install targets.
- The myron ABL contains an `L"efisp"` partition-name entry — same
  GBL/EFISP load mechanism as canoe. The EFISP deployment itself was
  already proven on this device by the user's gbl_root_canoe
  unlockmodefix flash.

## 1. Build the firmware

Prerequisites: docker, and the `edk2` submodule initialized (a fresh
clone has it empty):

```bash
git submodule update --init --recursive
./scripts/build.sh --board myron
```

Output: `dist/gbl-chainload-myron.efi` (the board folds into the build
name → `getvar gbl-chainload_build` reports `gbl-chainload-myron`, and
`PRODUCT_NAME`/`getvar product` say `myron` instead of `canoe`).
`--auto/--debug/--verbose` compose as usual. Without `--board`, the
default canoe build and artifact names are unchanged.

## 2. Package the mode-2 EFISP image

```bash
python3 scripts/efisp-package.py \
  --abl myron/abl.elf \
  --mode 2 \
  --efi dist/gbl-chainload-myron.efi \
  --stock-vbmeta myron/vbmeta.img \
  --out dist/efisp-payload/myron-mode2.efi
```

Notes:

- `--oem` is deliberately omitted (= `none`): there is no Xiaomi OEM
  patch group and the user explicitly does not want an orange-state /
  unlocked-warning-hiding patch. Mode 2 replaces that entire approach.
- `myron/abl.elf` and `myron/vbmeta.img` are the **stock OS3.0.23**
  images (the local reference drop; the same content is committed as
  test fixtures). When an OTA changes the ABL or vbmeta, re-run with the
  new stock images — the profile is per-OTA by design.
- The chain run: `gbl unwrap` → `gbl patch` (abl_permissive: patch6 +
  patch10, plus both BCC DICE paths forced to Normal on every host-patched
  ABL) → `gbl mode2 derive+compile` (init_boot fallback) →
  `gbl pack --manifest 0x02` → concat. A missing or ambiguous BCC
  anchor aborts packaging without an output.

Verify before flashing:

```bash
gbl inspect dist/efisp-payload/myron-mode2.efi
```

Expect `result: ok`, four entries (cached_abl / source_meta /
mode2_profile 120 B / manifest), and `fakelock_hook=no
profile_spoof=yes` — mode 2 only.

## 3. Flash (user-run)

Per `CLAUDE.md`, persistent writes to non-HLOS partitions are always
user-run. Follow the staged order from
`docs/project/recovery-install-validation.md`:

1. **RAM-only first** (survives a power cycle, writes nothing):
   ```
   fastboot stage dist/efisp-payload/myron-mode2.efi
   fastboot oem boot-efi
   ```
2. **EFISP write from TWRP** (after the RAM test boots cleanly):
   ```bash
   adb push dist/efisp-payload/myron-mode2.efi /tmp/installed.efi
   # TWRP shell — back up first, then write, then sync:
   dd if=/dev/block/by-name/efisp of=/sdcard/efisp.bak bs=1M
   dd if=/tmp/installed.efi of=/dev/block/by-name/efisp bs=1M conv=fsync
   sync
   adb reboot
   ```

Escape hatches: hold **Vol-Up** during the gbl-chainload key window to
reach FastbootLib; restore EFISP from TWRP with
`dd if=/sdcard/efisp.bak of=/dev/block/by-name/efisp bs=1M conv=fsync`.

## 4. On-device validation checkpoints

Watch the boot log (UefiLog / `--debug` build screen). Expected
sequence:

```
gbl-payload: source=efisp-blockio base=0x... size=...
BootFlow: loaded ABL via cached (size=1085440)
BootFlow: mode-2 profile loaded — spoof active
ProtocolHookLib: installed (fakelock=0 profile_spoof=1, vb=1/1 scm=1/1 qsee=1/1 spss=1/1 blockio=1/1)
qsee-km | cmd=0x00000201(SET_ROT) | ... | rotDigest=6e4c4033... | st=0
qsee-km | cmd=0x00000208(SET_BOOT_STATE) | ... | isUnlocked=0 | pubKey=e5c32182... | color=0 | ...
spss-bootstate | cmd=0x208 | ... | unlocked=0 | color=0 | sysVer=0x40000 | sysSpl=0x9a1
```

(The `rotDigest` / `pubKey` prefixes above are the values derived from
the stock OS3.0.23 vbmeta — `gbl mode2 derive myron/vbmeta.img` prints
them; they change with each OTA's stock vbmeta.)

Then in Android: Keymaster attestation should report the stock ROT /
boot state; the mode-2 fastboot screen shows no profile warning.

**Known residual risk — KM/SPSS wire offsets.** The SET_ROT /
SET_BOOT_STATE / SET_VBH struct offsets in `ProfileRewrite.c` were
cross-checked against canoe's `keymaster_wire.h` only (no myron BSP
headers were available). The rewrite is length-gated — it only fires
when the intercepted buffer length matches the expected wire size — so
a layout mismatch degrades to *no rewrite* (honest values, visible as
missing/unaltered `qsee-km`/`spss-bootstate` log lines) rather than
corruption. If the logs show the payloads passing through unrewritten,
the offsets need re-deriving against a myron KeymasterClient.h.

Also expected on myron (not an error):

```
BlockIoHook: no oplus/oppo reserve1 partition — reserve preservation inactive
```

The OPlus reserve-token preservation has no target on a Xiaomi GPT; the
EFISP read/write refusal gate remains active.

## 5. Custom recovery (optional, separate step)

Mode 2 addresses the TA/attestation view. For a **custom recovery to
normal-boot**, userspace AVB must still find satisfiable metadata — use
the existing generic graft tool with the stock recovery as source:

```bash
gbl avb graft \
  --stock myron/recovery.img \
  --custom <custom-recovery.img> \
  --part-size <recovery-partition-bytes> \
  --out grafted-recovery.img
# optional safety check against the device's main vbmeta:
gbl avb check grafted-recovery.img myron/vbmeta.img recovery
```

No myron-specific code is involved; the tooling is partition-level and
generic (see `docs/project/recovery-install-validation.md` background on
the graft approach).
