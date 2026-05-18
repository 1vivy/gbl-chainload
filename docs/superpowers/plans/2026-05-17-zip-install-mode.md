# gbl-chainload install ZIP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fill the `install` mode of the `zip-gbl-chainload` submodule — the flashable ZIP that caches a patched ABL into gbl-chainload's GBLP1 overlay on EFISP and writes a known-vulnerable loader ABL onto the target slot.

**Architecture:** `fv-unwrap` gains an `efisp-marker:` report so the installer can confirm an ABL is exploitable. `modes/install.sh` is a mode body of isolated functions (`pick_scenario` / `preflight` / `resolve_restore_source` / `build_payload` / `commit_efisp` / `restore_abl` / `save_backup_abl`), run by the SP2 `update-binary`. The updated `fv-unwrap` is re-vendored into the submodule's `bin/`.

**Tech Stack:** C (`fv-unwrap`, links `liblzma`), POSIX `sh` / busybox-`ash` (`install.{conf,sh}`), Bash (host tests), `git submodule`, `shellcheck`, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-05-17-zip-install-mode-design.md`
**Branch:** `feature/zip-install-mode` (parent), stacked on `feature/zip-methodology` — PR #25.

---

## Spec reconciliations

- The spec names one test `072_install_mode.sh`; this plan splits it into `tests/host/072_fv_unwrap_exploit.sh` (the marker check) and `tests/host/073_install_assembly.sh` (the ZIP assembly) — two distinct concerns, cleaner.
- `tests/host/072` is the first host test to build `fv-unwrap`, which links `liblzma`. The parent CI workflow gains a `liblzma-dev` install step (not in the spec — a build-environment necessity).
- The exploit-marker fixtures are the committed `tests/images/op15-infiniti-703-abl.img` (vulnerable) and `op15-infiniti-201-abl.img` (not) — the spec referred loosely to `images/`, which is git-ignored.

---

## File Structure

### Parent repo (`gbl-chainload`)

| File | Change | Responsibility |
|------|--------|----------------|
| `tools/fv-unwrap/fv-unwrap.c` | Modify | After extracting the PE, print `efisp-marker: present\|absent` on stdout. |
| `.github/workflows/ci.yml` | Modify | Install `liblzma-dev` before `tests/runall.sh`. |
| `tests/host/072_fv_unwrap_exploit.sh` | Create | Verify `fv-unwrap` reports the marker (703 present / 201 absent). |
| `tests/host/073_install_assembly.sh` | Create | Assemble `gbl-chainload-install.zip`; assert contents; lint staged `install.sh`. |
| `zip` | Modify | Submodule gitlink — bumped after re-vendoring. |

### Submodule (`zip-gbl-chainload`, at `zip/`)

| File | Change | Responsibility |
|------|--------|----------------|
| `modes/install.conf` | Modify (was a stub) | Declarative config for the install mode. |
| `modes/install.sh` | Modify (was a stub) | The install mode body. |
| `bin/fv-unwrap`, `bin/MANIFEST` | Modify | Re-vendored marker-aware `fv-unwrap` + refreshed manifest. |

---

## Conventions

- Parent files commit on `feature/zip-install-mode` (never `main`, never switch branch).
- Submodule files edit under `zip/`, commit with `git -C zip ...` on the submodule's `main`.
- `shellcheck` clean on every script. The installer core is busybox-`ash` → `shellcheck -s sh`; host tests are Bash → plain `shellcheck`. If a warning is genuinely unavoidable add a minimal `# shellcheck disable=` directive and report it.
- Never run `fastboot` flash/oem/flashing of non-HLOS partitions (project safety rule).

---

### Task 1: `fv-unwrap` exploit-marker report

**Files:**
- Create: `tests/host/072_fv_unwrap_exploit.sh`
- Modify: `tools/fv-unwrap/fv-unwrap.c`
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Write the failing test**

Create `tests/host/072_fv_unwrap_exploit.sh`:

```bash
#!/usr/bin/env bash
# tests/host/072_fv_unwrap_exploit.sh — fv-unwrap reports the UTF-16 'efisp'
# loader-path marker: present for a vulnerable ABL, absent for a clean one.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/fv-unwrap

OUT=tests/host/.last/072
rm -rf "$OUT"; mkdir -p "$OUT"

tools/fv-unwrap/fv-unwrap tests/images/op15-infiniti-703-abl.img "$OUT/703.efi" 2>/dev/null \
  | grep -qx 'efisp-marker: present' \
  || { echo "FAIL: 703 ABL should report 'efisp-marker: present'"; exit 1; }

tools/fv-unwrap/fv-unwrap tests/images/op15-infiniti-201-abl.img "$OUT/201.efi" 2>/dev/null \
  | grep -qx 'efisp-marker: absent' \
  || { echo "FAIL: 201 ABL should report 'efisp-marker: absent'"; exit 1; }

echo "PASS: 072 fv-unwrap exploit-marker"
```

- [ ] **Step 2: Run it — verify it fails**

Run: `bash tests/host/072_fv_unwrap_exploit.sh`
Expected: FAIL — `fv-unwrap` builds and runs but prints nothing on stdout, so the `grep -qx 'efisp-marker: present'` fails.

- [ ] **Step 3: Add the include to `fv-unwrap.c`**

In `tools/fv-unwrap/fv-unwrap.c`, add the shared header include directly after the existing `#include <lzma.h>` line:

```c
#include <lzma.h>
#include "../shared/efisp_scan.h"
```

- [ ] **Step 4: Print the marker in `main`**

In `tools/fv-unwrap/fv-unwrap.c`, in `main`, replace this block:

```c
  fwrite (pe.data, 1, pe.size, o);
  fclose (o);
  fprintf (stderr, "wrote 0x%zx (%zu) bytes to %s\n", pe.size, pe.size, argv[2]);
  free (pe.data);
  return 0;
```

with:

```c
  fwrite (pe.data, 1, pe.size, o);
  fclose (o);
  fprintf (stderr, "wrote 0x%zx (%zu) bytes to %s\n", pe.size, pe.size, argv[2]);
  /* Report whether the extracted PE retains the UTF-16 'efisp' loader-path
     marker. A "vulnerable" (GBL-loadable) ABL has it; abl-patcher removes
     it. Printed on stdout (diagnostics go to stderr) for shell consumers. */
  printf ("efisp-marker: %s\n",
          gbl_contains_utf16_efisp (pe.data, pe.size) ? "present" : "absent");
  free (pe.data);
  return 0;
```

- [ ] **Step 5: Rebuild and run the test — verify it passes**

Run: `make -s -C tools/fv-unwrap && bash tests/host/072_fv_unwrap_exploit.sh`
Expected: final line `PASS: 072 fv-unwrap exploit-marker`.

- [ ] **Step 6: Add `liblzma-dev` to parent CI**

In `.github/workflows/ci.yml`, add an install step before the "Run all tests" step so the CI runner can build `fv-unwrap`:

```yaml
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      - name: Install build deps
        run: sudo apt-get update && sudo apt-get install -y liblzma-dev shellcheck
      - name: Run all tests
        run: bash tests/runall.sh
```

- [ ] **Step 7: Commit (parent)**

```bash
git add tools/fv-unwrap/fv-unwrap.c tests/host/072_fv_unwrap_exploit.sh .github/workflows/ci.yml
git commit -m "fv-unwrap: report the UTF-16 efisp loader-path marker

After extracting the PE, fv-unwrap prints 'efisp-marker: present|absent'
on stdout so the install ZIP can confirm an ABL is exploitable (retains
the GBL/EFISP loader path). Test 072 verifies it against the committed
703 (vulnerable) and 201 (non-vulnerable) ABL fixtures. CI gains
liblzma-dev so the runner can build fv-unwrap.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: `modes/install.conf`

**Files:**
- Modify: `zip/modes/install.conf` (currently the SP2 abort-stub conf)

- [ ] **Step 1: Replace the file**

Overwrite `zip/modes/install.conf` with:

```sh
# shellcheck shell=sh
# shellcheck disable=SC2034
# modes/install.conf — declarative config for the install mode.
# install caches a patched ABL into the GBLP1 overlay on EFISP and writes a
# known-vulnerable loader ABL onto the target slot. See the SP3 design spec
# docs/superpowers/specs/2026-05-17-zip-install-mode-design.md.

MODE_NAME="install"
MODE_DESC="install gbl-chainload onto EFISP (cache ABL + loader-ABL restore)"
MODE_WRITES="efisp abl"
MODE_TOOLS="fv-unwrap abl-patcher gbl-pack gbl-commit"
MODE_EFI="mode-1.efi"
```

- [ ] **Step 2: Lint**

Run: `shellcheck -s sh zip/modes/install.conf`
Expected: no output, exit 0.

- [ ] **Step 3: Commit (submodule)**

```bash
git -C zip add modes/install.conf
git -C zip commit -m "modes: install.conf - declare tools, base EFI, write scope"
```

---

### Task 3: `modes/install.sh`

**Files:**
- Modify: `zip/modes/install.sh` (currently the SP2 abort-stub)

- [ ] **Step 1: Replace the file**

Overwrite `zip/modes/install.sh` with the complete mode body:

```sh
# shellcheck shell=sh
# shellcheck disable=SC2154
# modes/install.sh — install gbl-chainload onto EFISP.
#
# Caches a patched ABL into the GBLP1 overlay appended to gbl-chainload on
# EFISP, and writes a known-vulnerable loader ABL onto the target slot so
# that slot loads gbl-chainload. See the SP3 design spec.
#
# Two ABLs, kept distinct:
#   cached  ABL -> patched + packed into the EFISP overlay; gbl-chainload runs it.
#   restore ABL -> written verbatim to abl_<target>; MUST be vulnerable (retain
#                  the GBL/EFISP loader path) so that on-disk ABL loads
#                  gbl-chainload.

BACKUP=/sdcard/backup_abl.img

# vol_key <timeout-seconds> -> echoes UP | DOWN | TIMEOUT
vol_key() {
  _k=$(timeout "$1" getevent -lqc 5 2>/dev/null \
         | grep -m1 -oE 'KEY_(VOLUMEUP|VOLUMEDOWN)' || true)
  case "$_k" in
    KEY_VOLUMEUP)   echo UP ;;
    KEY_VOLUMEDOWN) echo DOWN ;;
    *)              echo TIMEOUT ;;
  esac
}

# abl_marker <abl-image> <scratch-pe-out> -> echoes present | absent.
# Aborts if the image has no extractable PE. The bundled fv-unwrap prints
# 'efisp-marker: present|absent' on stdout after a successful extraction.
abl_marker() {
  _m=$(fv-unwrap "$1" "$2" 2>/dev/null) \
    || abort "fv-unwrap failed on $1 (unrecognised ABL format?)"
  case "$_m" in
    *"efisp-marker: present"*) echo present ;;
    *)                         echo absent  ;;
  esac
}

# pick_scenario -> sets SCENARIO (ota|reinstall) and TARGET (slot suffix).
pick_scenario() {
  [ -n "$SLOT" ] && [ -n "$INACTIVE" ] || abort "not an A/B device (no slot suffix)"
  if $BOOTMODE; then
    SCENARIO=ota
    ui_print "[*] booted-Android install - assuming post-OTA"
  elif $OTA_POSTINSTALL; then
    SCENARIO=ota
    ui_print "[*] update_engine postinstall detected - OTA install"
  else
    ui_print "Which install is this?"
    ui_print "  Vol-UP   = OTA install (an OTA was just flashed)"
    ui_print "  Vol-DOWN = re-install gbl-chainload   (no key in 10s = re-install)"
    case "$(vol_key 10)" in
      UP) SCENARIO=ota ;;
      *)  SCENARIO=reinstall ;;
    esac
  fi
  if [ "$SCENARIO" = ota ]; then TARGET="$INACTIVE"; else TARGET="$SLOT"; fi
  ui_print "[*] scenario=$SCENARIO  target slot=$TARGET"
}

# preflight -> resolves device paths and gates everything before any write.
preflight() {
  TARGET_DEV=$(byname "abl_$TARGET")
  EFISP_DEV=$(byname efisp)
  ACTIVE_DEV=$(byname "abl_$SLOT")
  [ -n "$TARGET_DEV" ] || abort "abl_$TARGET partition not found"
  [ -n "$EFISP_DEV" ]  || abort "efisp partition not found"
  [ -n "$ACTIVE_DEV" ] || abort "abl_$SLOT partition not found"
  [ -f "$WORKDIR/base/mode-1.efi" ] || abort "base/mode-1.efi missing from ZIP"
  _mz=$(dd if="$EFISP_DEV" bs=1 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
  [ "$_mz" = 4d5a ] || abort "EFISP does not currently hold a PE (got '$_mz')"
}

# resolve_restore_source -> sets RESTORE_SRC and SAVED_FROM_BACKUP.
# Candidate X = the active-slot ABL; exploit-check it; P3 prompt; fall to
# /sdcard/backup_abl.img; abort if no vulnerable source exists.
resolve_restore_source() {
  ui_print "[*] checking active-slot ABL (abl_$SLOT) for the GBL loader path"
  dd if="$ACTIVE_DEV" of="$WORKDIR/active_abl.img" bs=1M 2>/dev/null \
    || abort "failed to read abl_$SLOT"
  if [ "$(abl_marker "$WORKDIR/active_abl.img" "$WORKDIR/active_pe.efi")" = present ]; then
    _xvuln=true
    ui_print "    active-slot ABL: vulnerable"
  else
    _xvuln=false
    ui_print "    active-slot ABL: NOT vulnerable"
  fi

  if $_xvuln; then
    if $BOOTMODE; then
      if [ -f "$BACKUP" ]; then
        RESTORE_SRC="$BACKUP"
      else
        RESTORE_SRC="$WORKDIR/active_abl.img"
      fi
    else
      ui_print "Restore the active-slot ABL to abl_$TARGET? (confirmed vulnerable)"
      ui_print "  Vol-UP = yes   Vol-DOWN = use /sdcard/backup_abl.img instead"
      case "$(vol_key 10)" in
        DOWN) RESTORE_SRC="$BACKUP" ;;
        *)    RESTORE_SRC="$WORKDIR/active_abl.img" ;;
      esac
    fi
  else
    ui_print "[*] active-slot ABL not vulnerable - using /sdcard/backup_abl.img"
    RESTORE_SRC="$BACKUP"
  fi

  if [ "$RESTORE_SRC" = "$BACKUP" ]; then
    [ -f "$BACKUP" ] || abort "no vulnerable restore source: /sdcard/backup_abl.img is missing"
    [ "$(abl_marker "$BACKUP" "$WORKDIR/backup_pe.efi")" = present ] \
      || abort "/sdcard/backup_abl.img is not a vulnerable ABL"
    SAVED_FROM_BACKUP=true
  else
    SAVED_FROM_BACKUP=false
  fi
  ui_print "[*] restore source: $RESTORE_SRC"
}

# build_payload -> reads abl_<target>, builds the GBLP1 overlay, produces
# $WORKDIR/installed.efi (base mode-1 EFI + payload).
build_payload() {
  ui_print "[1/4] reading abl_$TARGET (cache source)"
  dd if="$TARGET_DEV" of="$WORKDIR/cache_abl.img" bs=1M 2>/dev/null \
    || abort "failed to read abl_$TARGET"
  ui_print "[2/4] fv-unwrap + abl-patcher + gbl-pack"
  fv-unwrap "$WORKDIR/cache_abl.img" "$WORKDIR/extracted.efi" >/dev/null 2>&1 \
    || abort "fv-unwrap failed on the cache-source ABL"
  abl-patcher --in "$WORKDIR/extracted.efi" --out "$WORKDIR/patched.efi" \
    || abort "abl-patcher failed (no matching signatures?)"
  gbl-pack --cached-abl "$WORKDIR/patched.efi" \
           --source "$WORKDIR/cache_abl.img" \
           --extracted "$WORKDIR/extracted.efi" \
           --out "$WORKDIR/payload.bin" \
    || abort "gbl-pack failed"
  cat "$WORKDIR/base/mode-1.efi" "$WORKDIR/payload.bin" > "$WORKDIR/installed.efi"
}

# commit_efisp -> verified write of installed.efi onto EFISP.
commit_efisp() {
  ui_print "[3/4] writing EFISP (backup + verify)"
  commit_verified "$WORKDIR/installed.efi" "$EFISP_DEV" /sdcard/efisp.bak
}

# restore_abl -> verified write of the restore source onto abl_<target>.
restore_abl() {
  ui_print "[4/4] restoring loader ABL to abl_$TARGET (backup + verify)"
  commit_verified "$RESTORE_SRC" "$TARGET_DEV" "/sdcard/abl_$TARGET.bak"
}

# save_backup_abl -> P4: offer to save the exploit ABL to /sdcard/backup_abl.img.
save_backup_abl() {
  $SAVED_FROM_BACKUP && return 0
  if $BOOTMODE; then
    [ -f "$BACKUP" ] && return 0
    cp "$RESTORE_SRC" "$BACKUP" && ui_print "[*] saved exploit ABL to $BACKUP"
  else
    ui_print "Save the exploit ABL just used to /sdcard/backup_abl.img?"
    ui_print "  Vol-UP = yes, else skip"
    if [ "$(vol_key 10)" = UP ]; then
      cp "$RESTORE_SRC" "$BACKUP" && ui_print "[*] saved exploit ABL to $BACKUP"
    fi
  fi
}

mode_main() {
  ui_print "install: gbl-chainload installer"
  ui_print ""
  pick_scenario
  preflight
  resolve_restore_source
  build_payload
  commit_efisp
  restore_abl
  save_backup_abl
  ui_print ""
  ui_print "install: done - reboot to use the cached ABL."
}
```

- [ ] **Step 2: Lint**

Run: `shellcheck -s sh zip/modes/install.sh`
Expected: no output, exit 0. If `shellcheck` reports an unavoidable warning (e.g. SC2034 on a cross-function global it cannot trace), add a minimal `# shellcheck disable=` directive to the file header and note which and why — exactly as SP2's core files did.

- [ ] **Step 3: Commit (submodule)**

```bash
git -C zip add modes/install.sh
git -C zip commit -m "modes: install.sh - cache ABL to EFISP + loader-ABL restore"
```

---

### Task 4: Re-vendor the marker-aware `fv-unwrap`

**Files:**
- Modify: `zip/bin/fv-unwrap`, `zip/bin/MANIFEST` (and any other rebuilt artifact)
- Modify: `zip` (parent gitlink)

> **Environment note:** runs `zip/update-tools.sh`, which builds the recovery tools in Docker (Android NDK r27) and the base EFI. Docker must be available. If it is not, stop and report BLOCKED.

- [ ] **Step 1: Re-vendor**

```bash
cd /home/vivy/gbl-chainload/zip
./update-tools.sh
cd /home/vivy/gbl-chainload
```

Expected: ends with `==> done. ...`. `zip/bin/fv-unwrap` is rebuilt from the Task 1 source; `zip/bin/MANIFEST` is rewritten with `# parent-dirty: 0`.

- [ ] **Step 2: Verify the rebuilt fv-unwrap carries the marker code**

```bash
( cd zip && grep -E '^[0-9a-f]{64}  ' bin/MANIFEST | sha256sum -c )
strings zip/bin/fv-unwrap | grep -c 'efisp-marker:'
```

Expected: `sha256sum -c` prints `OK` for every line; the `strings` count is `1` (the marker format string is present in the rebuilt binary).

- [ ] **Step 3: Commit the submodule and push**

```bash
git -C zip add bin
git -C zip commit -m "bin: re-vendor marker-aware fv-unwrap + refresh MANIFEST"
git -C zip push origin HEAD:main
```

- [ ] **Step 4: Bump the parent submodule pointer**

```bash
git add zip
git commit -m "build: advance zip submodule - install mode + marker-aware fv-unwrap

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: `tests/host/073_install_assembly.sh`

**Files:**
- Create: `tests/host/073_install_assembly.sh`

- [ ] **Step 1: Write the test**

Create `tests/host/073_install_assembly.sh`:

```bash
#!/usr/bin/env bash
# tests/host/073_install_assembly.sh — assemble gbl-chainload-install.zip,
# verify its contents, and lint the staged install.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=tests/host/.last/073
rm -rf "$OUT"; mkdir -p "$OUT"

bash scripts/build-recovery-zip.sh --mode install >/dev/null
ZIP=dist/gbl-chainload-install.zip
[ -f "$ZIP" ] || { echo "FAIL: $ZIP not produced"; exit 1; }

for e in META-INF/com/google/android/update-binary \
         META-INF/com/google/android/updater-script \
         core/ui.sh core/env.sh core/ota.sh core/busybox.sh core/partition.sh core/safety.sh \
         modes/SELECTED modes/install.conf modes/install.sh \
         bin/fv-unwrap bin/abl-patcher bin/gbl-pack bin/gbl-commit bin/busybox-arm64 \
         base/mode-1.efi SHA256SUMS; do
  unzip -l "$ZIP" | grep -q "[ /]$e\$" || { echo "FAIL: $ZIP missing $e"; exit 1; }
done

unzip -p "$ZIP" modes/SELECTED | grep -qx install \
  || { echo "FAIL: SELECTED is not 'install'"; exit 1; }
if unzip -l "$ZIP" | grep -qE 'modes/(diag|graft|profile)'; then
  echo "FAIL: non-selected modes were not pruned"; exit 1
fi

unzip -o "$ZIP" -d "$OUT/x" >/dev/null
( cd "$OUT/x" && sha256sum -c --status SHA256SUMS ) \
  || { echo "FAIL: SHA256SUMS mismatch"; exit 1; }
shellcheck -s sh "$OUT/x/modes/install.sh" \
  || { echo "FAIL: staged install.sh fails shellcheck"; exit 1; }

echo "PASS: 073 install assembly"
```

- [ ] **Step 2: Run it — verify it passes**

Run: `bash tests/host/073_install_assembly.sh`
Expected: final line `PASS: 073 install assembly`.

- [ ] **Step 3: Run the full suite**

Run: `bash tests/runall.sh`
Expected: `072_fv_unwrap_exploit` and `073_install_assembly` both appear; final line `ALL TESTS PASS`. This is slow (Docker build smoke) — use a generous timeout (e.g. 600000 ms) or run in the background.

- [ ] **Step 4: Commit (parent)**

```bash
git add tests/host/073_install_assembly.sh
git commit -m "test: 073 install-mode ZIP assembly"
```

---

### Task 6: Finalize

**Files:** none (integration only)

- [ ] **Step 1: Push the branch**

```bash
git push origin feature/zip-install-mode
```

- [ ] **Step 2: Confirm CI on PR #25**

Run: `gh pr checks 25 --watch --interval 20`
Expected: the `test` check passes (runs `tests/runall.sh`, which now includes 072 and 073).

- [ ] **Step 3: On-device acceptance (user-run, manual — not agent-run)**

`scripts/build-recovery-zip.sh --mode install` builds `dist/gbl-chainload-install.zip`. The user flashes it in recovery and confirms the GBLP1 overlay lands and gbl-chainload boots via the cached ABL — the B6-style validation in `docs/project/recovery-install-validation.md`. The agent cannot run this (device prompts + non-HLOS writes).

---

## Self-Review

**Spec coverage:**

- `modes/install.{conf,sh}` → Tasks 2, 3.
- Two-ABLs model (cached vs restore) → `install.sh` `build_payload` vs `restore_abl` (Task 3).
- Scenarios + slot logic (OTA install / re-install; target slot) → `pick_scenario` (Task 3).
- Recovery flow P1 / pre-flight / P3 / work / P4 → `pick_scenario` / `preflight` / `resolve_restore_source` / `build_payload`+`commit_efisp`+`restore_abl` / `save_backup_abl` (Task 3).
- BOOTMODE silent flow → the `$BOOTMODE` branches in `pick_scenario` / `resolve_restore_source` / `save_backup_abl` (Task 3).
- Restore-source resolution + abort-if-none-vulnerable → `resolve_restore_source` (Task 3).
- Exploit-check via `fv-unwrap` reporting the `efisp` marker → Task 1; consumed by `abl_marker` (Task 3).
- EFISP `MZ` pre-flight gate via `od` → `preflight` (Task 3).
- Verified writes via `commit_verified` → `commit_efisp`, `restore_abl` (Task 3).
- Re-vendoring the updated `fv-unwrap` → Task 4.
- Testing (`072` exploit-marker, `073` assembly) → Tasks 1, 5.

**Placeholder scan:** none — every file has complete content. Task 3 Step 2 permits adding `shellcheck` disable directives only if a warning is genuinely forced (the SP2 precedent), not as a placeholder.

**Type/name consistency:** `mode_main` (defined in `install.sh`, called by `update-binary`); helpers `vol_key` / `abl_marker` / `pick_scenario` / `preflight` / `resolve_restore_source` / `build_payload` / `commit_efisp` / `restore_abl` / `save_backup_abl` (defined and called within `install.sh`); cross-function globals `SCENARIO` / `TARGET` / `TARGET_DEV` / `EFISP_DEV` / `ACTIVE_DEV` / `RESTORE_SRC` / `SAVED_FROM_BACKUP` / `BACKUP`; core-provided `ui_print` / `abort` / `commit_verified` / `byname` / `WORKDIR` / `BOOTMODE` / `OTA_POSTINSTALL` / `SLOT` / `INACTIVE` — all consistent. `efisp-marker: present|absent` is the exact string emitted by `fv-unwrap` (Task 1) and matched by `abl_marker` and test `072` (Tasks 1, 3). `MODE_TOOLS`/`MODE_EFI` in `install.conf` (Task 2) match the artifacts test `073` asserts (Task 5).
