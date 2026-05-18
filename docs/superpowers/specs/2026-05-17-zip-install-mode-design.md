# gbl-chainload install ZIP — the `install` mode

Date: 2026-05-17
Status: design approved; implementation pending
Companion: `docs/project/zip-methodology.md`; SP2 packaging spec
`docs/superpowers/specs/2026-05-16-zip-packaging-structure-design.md`.

## Context

SP2 delivered the `zip-gbl-chainload` packaging skeleton with `install`,
`graft`, and `profile` modes as `abort` stubs. SP3 fills the **`install`
mode** — the flashable ZIP that installs gbl-chainload onto the EFISP
partition. It is the third sub-project of the ZIP-methodology line and
stacks on the SP2 branch (`feature/zip-methodology`); it rebases onto
`main` once the SP2 PR merges.

## Goal & scope

Implement `modes/install.{conf,sh}` in the `zip-gbl-chainload` submodule: a
ZIP that (1) caches a patched ABL into gbl-chainload's GBLP1 overlay on
EFISP, and (2) writes a known-vulnerable loader ABL onto the target slot's
`abl` partition so that slot loads gbl-chainload. Supported: a recovery
flow with two scenarios (post-OTA install, re-install) and a silent
booted-Android (`BOOTMODE`) flow.

**In scope:** `modes/install.{conf,sh}`; an exploit-marker check added to
the `fv-unwrap` tool; `tests/host/072_install_mode.sh`; re-vendoring the
updated `fv-unwrap` into the submodule.

**Out of scope:** the `graft` and `profile` modes; extracting a shared
install library (deferred — the mode-2 `profile` sub-project does it); a
proactive EFISP capacity pre-check.

## Two ABLs — the core distinction

The install handles two distinct ABL roles. Conflating them was the main
design pitfall, so they are named explicitly throughout:

- **Cached ABL** — patched by `abl-patcher` and packed into the GBLP1
  overlay on EFISP. This is the ABL gbl-chainload *chainloads and runs*.
  Its source is automatic (the target slot's ABL — see the scenario
  table). It need not be "vulnerable": `abl-patcher` neutralises its
  EFISP loader path as part of patching.
- **Restore ABL** — written verbatim onto a slot's `abl` partition. This
  on-disk ABL must itself *load gbl-chainload from EFISP*, so it MUST be
  **vulnerable** — retain the GBL/EFISP loader path.

A "vulnerable" ABL's PE contains the UTF-16LE `efisp` marker (the loader
path references `efisp`); `abl-patcher` removes that marker (the existing
DynamicPatchLib post-patch gate confirms this). The exploit-check is
therefore: does the `fv-unwrap`-extracted PE contain the `efisp` marker.

## Architecture

`modes/install.conf` — declarative:

```sh
MODE_NAME="install"
MODE_DESC="install gbl-chainload onto EFISP (cache ABL + loader-ABL restore)"
MODE_WRITES="efisp abl"
MODE_TOOLS="fv-unwrap abl-patcher gbl-pack gbl-commit"
MODE_EFI="mode-1.efi"
```

`modes/install.sh` — one mode file: `mode_main` plus isolated helper
functions (`pick_scenario`, `resolve_restore_source`, `build_payload`,
`commit_efisp`, `restore_abl`, `save_backup_abl`). Written cleanly enough
for the future mode-2 `profile` mode to extract shared functions from, but
no shared library is pre-built. It defines only these functions —
`update-binary` already performed bootstrap, `core/*.sh` sourcing, and
installed the EXIT trap.

### Scenarios & slot logic

The **target slot** is the slot the device will next boot. The cached ABL
is that slot's ABL, patched; the restore writes a vulnerable ABL onto that
same slot's `abl` partition.

| Scenario | selected by | cache source → EFISP | restore target | restore source |
|---|---|---|---|---|
| OTA install | P1 prompt, or auto when `OTA_POSTINSTALL` | `abl_<inactive>` (new OTA ABL) | `abl_<inactive>` | active-slot ABL if vulnerable, else `/sdcard/backup_abl.img` |
| Re-install | P1 prompt | `abl_<active>` | `abl_<active>` | active-slot ABL if vulnerable, else `/sdcard/backup_abl.img` |

`SLOT` / `INACTIVE` come from `core/partition.sh`. In the OTA-install
scenario the device is still on the old slot; the OTA's new ABL sits on
the inactive slot, which becomes active after the next reboot.

### Restore-source resolution

Candidate **X = the active-slot ABL** — it is currently running
gbl-chainload, so it is the natural known-vulnerable source. The
precedence differs by environment:

- **Recovery:** `fv-unwrap` X and exploit-check it. If X is vulnerable, P3
  defaults to X and lets the user pick `/sdcard/backup_abl.img` instead;
  if X is not vulnerable, the source is `/sdcard/backup_abl.img`.
- **BOOTMODE:** no prompt — use `/sdcard/backup_abl.img` if it is present,
  otherwise the active-slot ABL X. A pre-placed backup is treated as a
  deliberate operator choice.

In every case the **final** restore source is exploit-checked: **if no
vulnerable restore source can be found, the install `abort`s** — recovery
and `BOOTMODE` alike. The install never writes a non-vulnerable ABL onto
an `abl` partition.

### Recovery flow (`BOOTMODE=false`)

```
P1  Scenario  — shown only if OTA_POSTINSTALL is NOT detected.
               (If OTA_POSTINSTALL is set, the scenario is auto "OTA install".)
      Vol-UP   = OTA install   → target slot = INACTIVE
      Vol-DOWN = re-install    → target slot = ACTIVE
      timeout  → re-install (the conservative default: operate on the
                 currently-booted, known-working slot). The chosen
                 scenario is ui_print'd.

PRE-FLIGHT — before any write; abort here leaves the device untouched:
   - A/B slot resolves (SLOT / INACTIVE non-empty).
   - /dev/block/by-name/efisp exists and currently begins with a PE 'MZ'
     (read 2 bytes via `od -An -tx1`, never `xxd` — A3). Sanity that the
     write target is a PE, not the wrong partition.
   - base/mode-1.efi is present in the ZIP.
   - The cache-source ABL (target-slot abl) is readable.
   - Restore-source resolution runs (above). No vulnerable source → ABORT.

P3  Loader-ABL restore — MANDATORY:
      X (active-slot ABL) vulnerable:
        "Restore the active-slot ABL to abl_<target>? Confirmed vulnerable.
         Vol-UP = yes ;  Vol-DOWN = use /sdcard/backup_abl.img instead"
      X not vulnerable:
        ui_print "active-slot ABL not vulnerable — using /sdcard/backup_abl.img"
      (If the resolved restore source is missing/not vulnerable the run
      already aborted in pre-flight.)

[WORK] — all writes happen here, after P1 + P3 consent:
   cache:   fv-unwrap abl_<target> → abl-patcher --in/--out →
            gbl-pack --cached-abl/--source/--extracted/--out →
            cat base/mode-1.efi payload.bin > installed.efi →
            commit_verified installed.efi /dev/block/by-name/efisp /sdcard/efisp.bak
   restore: commit_verified <restore source> /dev/block/by-name/abl_<target> \
                            /sdcard/abl_<target>.bak

P4  "Save the exploit ABL just used to /sdcard/backup_abl.img?"
      Vol-UP = yes, else skip.
    Auto-skipped if the restore source was already /sdcard/backup_abl.img.
```

### BOOTMODE flow (`BOOTMODE=true`, booted Android)

A booted-Android (Magisk/KernelSU module) install has no recovery screen,
so it runs silently with no prompts. It assumes a post-OTA install:

- Target slot = inactive. Cache `abl_<inactive>` → EFISP.
- Restore source: `/sdcard/backup_abl.img` if present, else the
  active-slot ABL (see Restore-source resolution); restore target =
  `abl_<inactive>`.
- Exploit-check the restore source; **abort if not vulnerable**.
- If the restore source was the active-slot ABL and `/sdcard/backup_abl.img`
  is absent, auto-save it there (the P4 intent, applied silently).
- Every decision and action is `ui_print`'d.

### The exploit-check and `fv-unwrap`

The exploit-check must inspect binary content for a 12-byte UTF-16LE
pattern — unreliable in busybox-`ash` (`grep -P` may be absent; null
bytes break `grep -F`). Per methodology A3, binary inspection belongs in a
bundled C tool. `fv-unwrap` already extracts the PE from an ABL ELF/FV, so
it is extended to also report whether the extracted PE contains the
`efisp` marker — a distinct exit status (and a stdout line), reusing
`tools/shared/efisp_scan.h`'s `gbl_contains_utf16_efisp`. `install.sh`
calls `fv-unwrap` on the restore candidate and gates on that result.

The marker is only visible *after* `fv-unwrap` — it lives inside the ABL's
firmware volume, so a raw scan of the `abl` partition shows nothing (a raw
scan of `images/infiniti-IN-16.0.7.201.img` returns zero matches). The
extended `fv-unwrap` must be verified to report **vulnerable** for the
16.0.5.703 ABL fixture and **not vulnerable** for the 16.0.7.201 ABL
fixture under `images/`.

`fv-unwrap` lives in the parent repo (`tools/fv-unwrap/`); changing it
means rebuilding the recovery tools and re-running `zip/update-tools.sh`
so the submodule's vendored `bin/fv-unwrap` is the marker-aware version.

### Payload build & EFISP write

`build_payload` (cache pipeline): `fv-unwrap` the target-slot ABL →
`abl-patcher --in extracted.efi --out patched.efi` → `gbl-pack
--cached-abl patched.efi --source <abl.img> --extracted extracted.efi
--out payload.bin` → `cat base/mode-1.efi payload.bin > installed.efi`.
`commit_efisp`: `commit_verified installed.efi /dev/block/by-name/efisp
/sdcard/efisp.bak`. No proactive capacity gate — `gbl-pack`'s 16 MiB cap,
`gbl-commit --verify`, and the `/sdcard/efisp.bak` backup are the safety
net (methodology A6).

### Loader-ABL restore

`restore_abl`: `commit_verified <restore source> /dev/block/by-name/abl_<target>
/sdcard/abl_<target>.bak`. The same backup → write → verify →
restore-on-mismatch path as the EFISP write. When the restore source is
the active-slot ABL and the target *is* the active slot (re-install with a
vulnerable `abl_active`), the write is idempotent (identical bytes) and
harmless.

### Error handling

All via `core/safety.sh`: `abort` on any failure (loud `ui_print`,
cleanup, exit 1); the EXIT trap clears the workdir and restores the
SELinux context. Both partition writes go through `commit_verified` →
`gbl-commit` auto-restores its backup on a verify mismatch. Backups:
`/sdcard/efisp.bak`, `/sdcard/abl_<target>.bak`. Every gate and write is
`ui_print`'d with a `[step/total]` prefix.

### Testing

`tests/host/072_install_mode.sh` (parent repo, auto-discovered by
`tests/runall.sh`): assemble `gbl-chainload-install.zip`
(`build-recovery-zip.sh --mode install`); assert it carries `update-binary`,
`core/*.sh`, `install.{conf,sh}`, the four tools, and `base/mode-1.efi`;
`shellcheck -s sh` the staged `install.sh`. The `fv-unwrap` exploit-check
gets a host test verifying it distinguishes the 703 (vulnerable) and 201
(not-vulnerable) ABL fixtures. The `fv-unwrap → abl-patcher → gbl-pack`
pure-logic pipeline is already covered by host tests 060–069.

Device prompts and partition writes are Layer-3 on-device validation
(user-run), like SP2's `diag` — the install ZIP flashed in recovery,
confirming the GBLP1 overlay lands and gbl-chainload boots via the cached
payload (the B6-style check in `docs/project/recovery-install-validation.md`).

## Open questions

- P1's timeout default is re-install (active slot). If on-device testing
  shows users expect otherwise, revisit.
- First-ever install onto an EFISP that does not currently hold a PE would
  fail the `MZ` pre-flight gate. The supported scenarios (post-OTA install,
  re-install) both presume gbl-chainload is already present or being
  placed post-OTA; a true bare-first-install is not a target scenario.
