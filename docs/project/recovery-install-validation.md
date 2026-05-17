# Recovery install — build and validation runbook

Build and validation runbook for the on-device GBLP1 payload insertion
feature. Covers everything from a clean checkout to on-device validation
of the cached-ABL boot path. The recovery installer ZIP that orchestrates
this flow unattended is a separate follow-up; see
`docs/project/zip-methodology.md`.

Spec: `docs/superpowers/specs/2026-05-15-on-device-payload-insertion-design.md`

---

## Ordering principle

**No persistent partition write happens until every RAM-only check has
passed, and the recovery escape hatch is understood before any risky
write.** Layers 1 and 2 are fully RAM-only and agent-runnable. Layer 3
requires a human at a real device.

---

## Part A — Build

### A1 — EDK2 EFI

```bash
bash scripts/build.sh --mode 1
```

Output: `dist/mode-1.efi`

Success: script prints `==> Built dist/mode-1.efi (N bytes)` and exits 0.
The file is the base EFI with no payload appended — the GBLP1 overlay is
added at install time, not at build time.

### A2 — Host tools + full host test suite

```bash
bash tests/runall.sh
```

Output: test results to stdout; EDK2 build smoke included.

Success: final line is `ALL TESTS PASS`. Tests 060–070 cover the packer
roundtrip, parser fuzz, efisp-scan gate, PE sanity, end-to-end fixtures,
patch-signature parity, `gbl-commit` atomic-write, BlockIO reader smoke,
config-table override simulation, full-buffer scan, and crypto
conformance (SHA-256 / CRC-32 known-answer vectors).

### A3 — Cross-compiled recovery tools (Docker + Android NDK r27)

```bash
bash scripts/build-recovery-tools.sh
```

Output: `dist/recovery/{fv-unwrap,abl-patcher,gbl-pack,gbl-commit}` +
`dist/recovery/SHA256SUMS`

Success: script prints `ls -la dist/recovery/` listing all four binaries
and exits 0. These are aarch64-Android static binaries; they run in TWRP
without libc.so. The script handles the WSL/Docker-Desktop credential
quirk internally (`DOCKER_CONFIG` export) — a plain invocation is
sufficient.

The cross-compiled tools (A3) are everything the recovery installer ZIP
will bundle; assembling that ZIP is a follow-up (see
`docs/project/zip-methodology.md`). Until it lands, the Layer-3 steps
below drive the install by hand.

---

## Part B — Validation

### Layer 1 — Host CI (no device)

```bash
bash tests/runall.sh
```

Gate: all tests green before any device step. Tests 060–070 exercise the
parser, packer, and every pure-logic path at byte-for-byte parity with the
on-device parser. The EDK2 build smoke is included.

---

### Layer 2 — In-RAM stage tests (no persistent write; agent-safe)

These use `fastboot stage <efi> && fastboot oem boot-efi` (or
`oem boot-efi-blockio`), which is the RAM-only test path defined in
CLAUDE.md. No partition is written. Each step gates the next.

#### B1 — Static pre-check of the concat'd EFI (host-side, no device)

Build the exact binary you will later write to EFISP and confirm the GBLP1
portion parses cleanly before touching any device:

```bash
# If tests/host/.last/060/payload.bin does not exist yet, produce it:
bash tests/host/060_pack_roundtrip.sh

# Concat base EFI + payload into the installable artifact:
cat dist/mode-1.efi tests/host/.last/060/payload.bin > /tmp/installed.efi

# Strip the EFI prefix and check only the GBLP1 portion:
tail -c +$(($(stat -c%s dist/mode-1.efi)+1)) /tmp/installed.efi > /tmp/p.bin
tests/host/helpers/parser_harness find-cached-abl /tmp/p.bin
```

Success: `status=0 size=N`. A non-zero status or missing output means the
payload is malformed — fix packer/fixture issues before any device step.
This is pure host-side static analysis; it catches a bad container before
the device ever sees it.

#### B2 — Stage WITH payload → expect Tier 1 cached

**Bootstrap dependency:** this step requires the gbl-chainload binary
currently serving `oem boot-efi` on EFISP to be PR-version (i.e., the
build that installs the `gGblStagedBufferGuid` configuration table in its
`CmdOemBootEfi` handler). If EFISP still holds a pre-PR build, use the
two-hop procedure in B4 instead.

```bash
fastboot stage /tmp/installed.efi
fastboot oem boot-efi
```

Watch the boot log (UefiLog / on-screen debug build). Expected sequence:

```
gbl-payload: source=staged-buffer base=0x... size=...
BootFlow: loaded ABL via cached (size=N)
```

The config-table path: the PR-version `oem boot-efi` handler installs a
`GBL_STAGED_BUFFER_TABLE` via `gBS->InstallConfigurationTable` pointing at
the staged download buffer. `GblPayloadLib::LocateOverlayBytes` walks
`gST->ConfigurationTable[]`, finds it, and uses the staged bytes. This is
pure in-RAM, single-boot-session — categorically different from
`gRT->SetVariable` (known-dead on this hardware); there is no NV storage
involved.

#### B3 — Stage WITHOUT payload → expect Tier 2 dynamic

```bash
fastboot stage dist/mode-1.efi
fastboot oem boot-efi
```

Expected:

```
BootFlow: cached unavailable (...), trying dynamic patch
BootFlow: loaded ABL via dynamic (size=N)
```

Confirms the Tier-2 dynamic-patch fallback fires cleanly when no overlay
is appended. Also confirms the base EFI boots correctly without a GBLP1
trailer.

#### B4 — Config-table round-trip probe (two-hop)

This step is only needed if EFISP still holds a pre-PR gbl-chainload. The
problem: the binary serving `oem boot-efi` determines whether the config
table is installed. If it is pre-PR, it installs no table, so any staged
child falls through to BlockIO regardless of what you staged.

The two-hop workaround:

1. Stage and boot the PR-version `dist/mode-1.efi` (bare, no overlay)
   from the old handler. The old handler loads it; the new EFI now
   controls FastbootLib.
2. From that FastbootLib session (now running the PR EFI), stage
   `/tmp/installed.efi` and send `oem boot-efi` again. This time the PR
   handler installs the config table before `LoadImage`.

```bash
# Hop 1 — get the PR EFI into FastbootLib control:
fastboot stage dist/mode-1.efi
fastboot oem boot-efi          # old handler, no table; PR EFI boots and
                               # enters its own FastbootLib

# Hop 2 — now PR FastbootLib installs the table:
fastboot stage /tmp/installed.efi
fastboot oem boot-efi
```

Expected in the second-hop log:

```
gbl-payload: source=staged-buffer base=0x... size=...
BootFlow: loaded ABL via cached (size=N)
```

`source=staged-buffer` in the child log is the on-device confirmation that
`InstallConfigurationTable` works on this hardware and that the pointer
survives into the `StartImage`'d child (structural guarantee: one `gST`
per UEFI session).

#### B5 — Forced-BlockIO test (exercises the production reader, RAM-only)

`oem boot-efi-blockio` is identical to `oem boot-efi` except it skips the
config-table install. The staged child finds no config table and falls
through to `ReadEfispRawBytes`, which does a raw `BlockIo->ReadBlocks` of
the live EFISP partition.

```bash
fastboot stage dist/mode-1.efi
fastboot oem boot-efi-blockio
```

Expected:

```
gbl-payload: source=efisp-blockio base=0x... size=...
```

This exercises the production BlockIO reader path from a RAM-only test.
Note: it reads whatever GBLP1 is currently on EFISP, not the staged bytes.
If EFISP has no GBLP1 yet, expect `gbl-payload: bad magic` (or "cannot
locate overlay bytes") and a Tier-2 fallback — that is also a valid result
confirming the reader ran and failed gracefully.

---

### Layer 3 — Persistent writes (user-run, real hardware)

**Run only after all of Layer 1 and Layer 2 pass.** These steps write to
on-device partitions and cannot be run by the agent (CLAUDE.md hard-deny
on non-HLOS partition writes).

#### Know your escape hatch — read this before any write

- **Vol-Up** held at the gbl-chainload boot window reaches FastbootLib.
  This works as long as the EFISP content can load gbl-chainload at all.
- **EFISP restore from TWRP shell:**
  ```bash
  dd if=/sdcard/efisp.bak of=/dev/block/by-name/efisp bs=1M conv=fsync
  sync
  adb reboot
  ```
  Restores the pre-write EFISP exactly. B6 backs up EFISP to
  `/sdcard/efisp.bak` before writing, so it exists by the time any write
  completes.

#### B6 — Single EFISP write test

Validates the production BlockIO read path on real hardware. Smallest
possible persistent write scope: EFISP only.

From a TWRP shell, back up current EFISP, write the concat'd EFI, reboot:

```bash
# On host — push the installable artifact:
adb push /tmp/installed.efi /tmp/installed.efi

# In TWRP shell — backup, write, sync:
dd if=/dev/block/by-name/efisp of=/sdcard/efisp.bak bs=1M
dd if=/tmp/installed.efi of=/dev/block/by-name/efisp bs=1M conv=fsync
sync
adb reboot
```

Expected boot log:

```
gbl-payload: source=efisp-blockio base=0x... size=...
BootFlow: loaded ABL via cached (size=N)
```

Then boot Android and confirm mode-1 fakelock is intact: Keymaster
`SET_ROT` succeeds, normal-boot AVB reports green. This is the same
acceptance bar as any mode-1 boot — the cached path must install the
protocol hooks identically to the dynamic path.

If the log shows `source=efisp-blockio` but then a parse failure (e.g.,
`header_crc32 mismatch`), the payload was corrupted in transit — restore
from `/sdcard/efisp.bak` and re-check the `adb push` step.

---

## Part C — Residual gap

The UEFI protocol calls in `EfispBlockIo.c` (`GetBlkIOHandles`,
`HandleProtocol`, `ReadBlocks`) and the `LocateOverlayBytes`
configuration-table walk in `LocateOverlay.c` are only exercised
end-to-end on real hardware (B5 + B6 for BlockIO; B2/B4 for config table).
The host test suite (tests 067 and 068) exercises the same pure-logic
parser bytes against synthetic images, but the ~30-line EDK2-only IO
wrapper is not covered by any host or QEMU harness.

An OVMF-based EDK2 integration test that loads a synthetic EFISP image
and drives `EfispBlockIo.c` end-to-end would close this gap. It is out of
scope for this PR; add it if production bugs in the BlockIO reader surface.
See spec "Open questions."
