# ZIP Packaging Structure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the `zip-gbl-chainload` submodule — a mode-agnostic flashable-ZIP core with a mode-as-config mechanism, vendored tools with a drift guard, and the parent-side assembler — and ship one working no-op `diag` mode that proves the core end-to-end.

**Architecture:** A new `zip-gbl-chainload` GitHub repo, added to gbl-chainload as a submodule at `zip/`. Its `update-binary` contains zero mode logic: it unzips itself, sources `core/*.sh`, reads `modes/SELECTED`, and dispatches to that one mode. Tool binaries are vendored in-repo (AnyKernel3 model) and refreshed by `update-tools.sh`; a `bin/MANIFEST` + skew guard in the parent's `build-recovery-zip.sh` makes stale binaries a hard build failure. `diag` exercises the whole core with no device writes; `install`/`graft`/`profile` ship as `abort` stubs for SP3/SP4/later.

**Tech Stack:** POSIX `sh` / busybox-`ash` (the installer core), Bash (`update-tools.sh`, `build-recovery-zip.sh`), `git submodule`, `gh` CLI, `shellcheck`, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-05-16-zip-packaging-structure-design.md`
**Companion:** `docs/project/zip-methodology.md` (Part A conventions, Part B skeleton).

---

## Spec reconciliations

Three points where this plan tightens loose wording in the spec — all clarifications, no design change:

1. **`core/busybox.sh` does no multi-arch probe.** The methodology doc A3 establishes that targets are aarch64-only, so `busybox.sh` is bundled-tool PATH setup, not AnyKernel3's per-arch probe/relocate.
2. **Submodule CI is `shellcheck` only.** The assembly smoke cannot live in the submodule repo — `build-recovery-zip.sh` is a parent-repo script. The assembly + skew-guard smoke runs in the parent via `tests/host/071_zip_assembly.sh`, which the parent CI already executes through `tests/runall.sh`.
3. **Dirty-build manifest:** `update-tools.sh` *warns* and records `parent-dirty: 1`; `build-recovery-zip.sh` *hard-fails* on `parent-dirty: 1`. This reconciles the spec's two phrasings.

---

## File Structure

### New repo: `zip-gbl-chainload` (submodule, mounted at `zip/`)

| File | Responsibility |
|------|----------------|
| `META-INF/com/google/android/update-binary` | Mode-agnostic orchestrator: unzip self, source core, dispatch to the selected mode. |
| `META-INF/com/google/android/updater-script` | One-line dummy marker (recovery `stat`s it). |
| `core/ui.sh` | `ui_print` — canonical recovery on-screen output. |
| `core/env.sh` | `BOOTMODE`/`DIR` detection; SELinux elevation. |
| `core/ota.sh` | OTA-state detection (`OTA_POSTINSTALL` from `/postinstall`). |
| `core/busybox.sh` | Make bundled `bin/` tools executable and first on `PATH`. |
| `core/partition.sh` | `BYNAME` resolution, `byname()`, A/B slot (`SLOT`/`INACTIVE`). |
| `core/safety.sh` | `restore_env`/`cleanup`/`abort`, the `EXIT` trap, `commit_verified()`. |
| `modes/diag.conf` + `modes/diag.sh` | Working no-op diagnostic mode. |
| `modes/install.{conf,sh}` | Stub — `abort` (SP3 fills it). |
| `modes/graft.{conf,sh}` | Stub — `abort` (SP4 fills it). |
| `modes/profile.{conf,sh}` | Stub — `abort` (later). |
| `bin/{fv-unwrap,abl-patcher,gbl-pack,gbl-commit}` | Vendored aarch64 project tools. |
| `bin/busybox-arm64` | Vendored static busybox (bootstrap-acquired, not rebuilt). |
| `bin/MANIFEST` | Provenance: parent commit + per-artifact SHA-256. |
| `base/mode-1.efi` | Vendored base EFI (mode-2.efi added later — not buildable yet). |
| `update-tools.sh` | Refresh `bin/`+`base/`+`MANIFEST` from a parent checkout. |
| `README.md` | Usage + AnyKernel3 attribution. |
| `.gitignore` | `modes/SELECTED` (build-generated, never committed). |
| `.github/workflows/ci.yml` | `shellcheck` over the core + modes + scripts. |

### Parent repo: `gbl-chainload`

| File | Responsibility |
|------|----------------|
| `.gitmodules` | Modified — add the `zip` submodule entry. |
| `zip` | New submodule gitlink. |
| `scripts/build-recovery-zip.sh` | New — assembler: skew guard, stage, prune to one mode, checksum, zip. |
| `tests/host/071_zip_assembly.sh` | New — assembly + skew-guard test (auto-run by `tests/runall.sh`). |

---

## Conventions for every task

- Submodule files are edited in the `zip/` working tree. Commit them with `git -C zip ...`.
- Parent files are committed from the repo root. The parent branch is `feature/zip-methodology` (already checked out) — never `main`.
- `shellcheck` dialect: the installer core is busybox-`ash`, checked as `shellcheck -s sh`. `update-tools.sh` and `build-recovery-zip.sh` are Bash, checked with plain `shellcheck`.
- If `shellcheck` is missing: `sudo apt-get update && sudo apt-get install -y shellcheck`.

---

### Task 1: Bootstrap — create the repo and add the submodule

**Files:**
- Create: GitHub repo `1vivy/zip-gbl-chainload`
- Modify: `.gitmodules`
- Create: `zip` (submodule gitlink)

- [ ] **Step 1: Create the GitHub repo, matching gbl-chainload's visibility**

```bash
VIS=$(gh repo view 1vivy/gbl-chainload --json visibility -q .visibility)
gh repo create "1vivy/zip-gbl-chainload" --"$VIS" --add-readme \
  --description "Flashable-ZIP packaging for gbl-chainload (submodule)"
```

Expected: prints `https://github.com/1vivy/zip-gbl-chainload`.

- [ ] **Step 2: Add it as a submodule at `zip/`**

```bash
cd /home/vivy/gbl-chainload
git submodule add https://github.com/1vivy/zip-gbl-chainload.git zip
```

Expected: `zip/` is created containing the auto-generated `README.md`; `.gitmodules` gains a `[submodule "zip"]` block.

- [ ] **Step 3: Verify the submodule wiring**

Run: `git submodule status`
Expected: two lines — one for `edk2`, one for `zip` (a 40-hex SHA followed by `zip`).

- [ ] **Step 4: Commit the submodule addition (parent)**

```bash
git add .gitmodules zip
git commit -m "build: add zip-gbl-chainload submodule at zip/

First step of SP2 (ZIP packaging structure). The submodule holds the
mode-agnostic installer core; gbl-chainload pins it the way it pins edk2.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: `core/ui.sh`

**Files:**
- Create: `zip/core/ui.sh`

- [ ] **Step 1: Write the file**

```sh
# shellcheck shell=sh
# shellcheck disable=SC2154
# core/ui.sh — canonical recovery on-screen output.
# Sourced by update-binary. See gbl-chainload docs/project/zip-methodology.md A2.
# Plumbing pattern from AnyKernel3 (osm0sis, MIT-style license).
#
# Writes to the recovery's OUTFD via its /proc path — never `>&$fd`
# (busybox ash rejects a quoted fd; an unquoted one varies across builds).
# The literal newline inside the quoted string is the blank spacer line;
# never `echo -e` (busybox echo -e support is inconsistent).

ui_print() {
  echo "ui_print $1
ui_print" >> /proc/self/fd/"$OUTFD"
}
```

- [ ] **Step 2: Lint it**

Run: `shellcheck -s sh zip/core/ui.sh`
Expected: no output, exit 0.

- [ ] **Step 3: Commit (submodule)**

```bash
git -C zip add core/ui.sh
git -C zip commit -m "core: ui_print via /proc/self/fd path-write"
```

---

### Task 3: `core/env.sh` and `core/ota.sh`

**Files:**
- Create: `zip/core/env.sh`
- Create: `zip/core/ota.sh`

- [ ] **Step 1: Write `core/env.sh`**

```sh
# shellcheck shell=sh
# shellcheck disable=SC2154
# core/env.sh — environment detection and SELinux setup.
# Sourced by update-binary. See zip-methodology.md A4.
# Plumbing patterns from AnyKernel3 (osm0sis, MIT-style license).

# Boot mode: zygote in the process list => full Android is running
# (flash-from-system); otherwise we are in recovery.
BOOTMODE=false
ps | grep zygote | grep -v grep >/dev/null 2>&1 && BOOTMODE=true
$BOOTMODE || ps -A 2>/dev/null | grep zygote | grep -v grep >/dev/null 2>&1 && BOOTMODE=true

# Working directory for any mode output.
DIR=/sdcard
$BOOTMODE || DIR=$(dirname "$ZIPFILE")
[ "$DIR" = /sideload ] && DIR=/tmp

# SELinux: elevate so raw block writes are permitted under a booted
# enforcing system; a harmless no-op in a permissive recovery. The
# original context is saved in $shcon and restored by core/safety.sh.
shcon=$(cat /proc/self/attr/current 2>/dev/null)
echo "u:r:su:s0" > /proc/self/attr/current 2>/dev/null
```

- [ ] **Step 2: Write `core/ota.sh`**

```sh
# shellcheck shell=sh
# core/ota.sh — OTA-state detection.
# Sourced by update-binary. See zip-methodology.md A4.
# Pattern adapted from AnyKernel3 (osm0sis, MIT-style license).
#
# /postinstall is the mount point AOSP update_engine uses during the
# post-install phase of an A/B OTA; /postinstall/tmp present means a
# system OTA has been applied to the inactive slot and is in its
# post-install window. This is a signal only — modes decide how to act
# on it. It does NOT detect an OTA .zip flashed by hand in recovery.

OTA_POSTINSTALL=false
[ -d /postinstall/tmp ] && OTA_POSTINSTALL=true
```

- [ ] **Step 3: Lint both files**

Run: `shellcheck -s sh zip/core/env.sh zip/core/ota.sh`
Expected: no output, exit 0.

- [ ] **Step 4: Commit (submodule)**

```bash
git -C zip add core/env.sh core/ota.sh
git -C zip commit -m "core: environment + OTA-state detection"
```

---

### Task 4: `core/busybox.sh`

**Files:**
- Create: `zip/core/busybox.sh`

- [ ] **Step 1: Write the file**

```sh
# shellcheck shell=sh
# shellcheck disable=SC2154
# core/busybox.sh — bundled-tool setup.
# Sourced by update-binary. See zip-methodology.md A3.
#
# Targets are aarch64-only, so there is no multi-arch probe (cf.
# AnyKernel3): the bundled aarch64 static tools in bin/ are made
# executable and put first on PATH. The recovery's own busybox is
# relied on for ubiquitous applets; bin/busybox-arm64, when present,
# backstops applets a given recovery's busybox may lack.

if [ -d "$WORKDIR/bin" ]; then
  chmod 755 "$WORKDIR"/bin/* 2>/dev/null
  PATH="$WORKDIR/bin:$PATH"
  export PATH
fi
```

- [ ] **Step 2: Lint it**

Run: `shellcheck -s sh zip/core/busybox.sh`
Expected: no output, exit 0.

- [ ] **Step 3: Commit (submodule)**

```bash
git -C zip add core/busybox.sh
git -C zip commit -m "core: bundled-tool PATH setup (aarch64-only, no arch probe)"
```

---

### Task 5: `core/partition.sh`

**Files:**
- Create: `zip/core/partition.sh`

- [ ] **Step 1: Write the file**

```sh
# shellcheck shell=sh
# core/partition.sh — by-name partition resolution and A/B slot helpers.
# Sourced by update-binary. See zip-methodology.md A5.
# Plumbing patterns from AnyKernel3 (osm0sis, MIT-style license).

# BYNAME: directory of by-name partition symlinks.
BYNAME=/dev/block/by-name
[ -d "$BYNAME" ] || BYNAME=/dev/block/bootdevice/by-name
[ -d "$BYNAME" ] || BYNAME=$(find /dev/block/platform -type d -name by-name 2>/dev/null | head -1)

# byname <partition>: echo the block-device path, or nothing if absent.
byname() {
  [ -n "$BYNAME" ] && [ -e "$BYNAME/$1" ] && echo "$BYNAME/$1"
}

# A/B slot. SLOT and INACTIVE are "a"/"b", or empty on a non-A/B device.
SLOT=$(getprop ro.boot.slot_suffix 2>/dev/null)
SLOT=${SLOT#_}
case "$SLOT" in
  a) INACTIVE=b ;;
  b) INACTIVE=a ;;
  *) SLOT=""; INACTIVE="" ;;
esac
```

- [ ] **Step 2: Lint it**

Run: `shellcheck -s sh zip/core/partition.sh`
Expected: no output, exit 0.

- [ ] **Step 3: Commit (submodule)**

```bash
git -C zip add core/partition.sh
git -C zip commit -m "core: by-name resolution + A/B slot helpers"
```

---

### Task 6: `core/safety.sh`

**Files:**
- Create: `zip/core/safety.sh`

- [ ] **Step 1: Write the file**

```sh
# shellcheck shell=sh
# shellcheck disable=SC2154
# core/safety.sh — abort / cleanup / verified-write, and the EXIT trap.
# Sourced by update-binary. See zip-methodology.md A6.
# Plumbing patterns from AnyKernel3 (osm0sis, MIT-style license).

# restore_env: undo the SELinux elevation core/env.sh applied.
restore_env() {
  [ -n "$shcon" ] && echo "$shcon" > /proc/self/attr/current 2>/dev/null
}

# cleanup: runs on every exit, success or failure.
cleanup() {
  restore_env
  [ -n "$WORKDIR" ] && rm -rf "$WORKDIR"
}

# abort: the loud failure path.
abort() {
  ui_print "ABORT: $1"
  cleanup
  exit 1
}

trap cleanup EXIT

# commit_verified <src-file> <dst-block> <backup-path>
# backup -> write -> verify -> restore-on-mismatch, via the bundled
# gbl-commit. A writing mode MUST use this, never a bare dd. SP2 ships
# no writing mode; this is the contract SP3/SP4 build on.
commit_verified() {
  command -v gbl-commit >/dev/null 2>&1 || abort "gbl-commit not on PATH"
  gbl-commit --src "$1" --dst "$2" --backup "$3" --verify \
    || abort "verified write to $2 failed (backup at $3)"
}
```

- [ ] **Step 2: Lint it**

Run: `shellcheck -s sh zip/core/safety.sh`
Expected: no output, exit 0.

- [ ] **Step 3: Commit (submodule)**

```bash
git -C zip add core/safety.sh
git -C zip commit -m "core: abort/cleanup/EXIT-trap + commit_verified wrapper"
```

---

### Task 7: `update-binary` and `updater-script`

**Files:**
- Create: `zip/META-INF/com/google/android/update-binary`
- Create: `zip/META-INF/com/google/android/updater-script`

- [ ] **Step 1: Write `update-binary`**

```sh
#!/sbin/sh
# shellcheck shell=sh
# shellcheck disable=SC1090,SC2154
#
# zip-gbl-chainload — mode-agnostic flashable-ZIP core.
# Recovery (or a rooted booted system) execs:
#     update-binary  <api>  <OUTFD>  <zipfile>
# Conventions: gbl-chainload docs/project/zip-methodology.md
#
# This file holds NO mode-specific logic. It sets up the environment,
# sources core/*.sh, then dispatches to the single mode named in
# modes/SELECTED. One ZIP carries exactly one mode.

OUTFD=$2
ZIPFILE="$3"

# --- bootstrap ui_print / abort: usable BEFORE core/ is extracted.
#     core/ui.sh and core/safety.sh redefine these canonically once
#     the ZIP is unpacked. (zip-methodology A2) ---
ui_print() {
  echo "ui_print $1
ui_print" >> /proc/self/fd/"$OUTFD"
}
abort() { ui_print "ABORT: $1"; exit 1; }

# --- unpack ourselves into a private scratch dir ---
WORKDIR=/tmp/gbl-zip.$$
mkdir -p "$WORKDIR" || abort "cannot create workdir"
unzip -o "$ZIPFILE" -d "$WORKDIR" >/dev/null 2>&1 || abort "unzip failed"

# --- source the core; canonical ui_print/abort/cleanup land here, and
#     core/safety.sh installs the EXIT trap ---
for f in env ota ui busybox partition safety; do
  [ -f "$WORKDIR/core/$f.sh" ] || abort "core/$f.sh missing from ZIP"
  . "$WORKDIR/core/$f.sh"
done

# --- resolve the mode ---
[ -f "$WORKDIR/modes/SELECTED" ] || abort "modes/SELECTED missing"
MODE=$(cat "$WORKDIR/modes/SELECTED")
[ -n "$MODE" ] || abort "modes/SELECTED is empty"
[ -f "$WORKDIR/modes/$MODE.conf" ] || abort "modes/$MODE.conf missing"
[ -f "$WORKDIR/modes/$MODE.sh" ]   || abort "modes/$MODE.sh missing"
. "$WORKDIR/modes/$MODE.conf"
. "$WORKDIR/modes/$MODE.sh"

ui_print "gbl-chainload installer - mode: $MODE"
ui_print "======================================"

command -v mode_main >/dev/null 2>&1 || abort "modes/$MODE.sh defines no mode_main"
mode_main

ui_print ""
ui_print "DONE."
exit 0
```

- [ ] **Step 2: Write `updater-script`**

```
# scripted via update-binary - see ./update-binary (zip-methodology.md A1)
```

- [ ] **Step 3: Lint `update-binary`**

Run: `shellcheck -s sh zip/META-INF/com/google/android/update-binary`
Expected: no output, exit 0.

- [ ] **Step 4: Make `update-binary` executable**

Run: `chmod 755 zip/META-INF/com/google/android/update-binary`
Expected: no output.

- [ ] **Step 5: Commit (submodule)**

```bash
git -C zip add META-INF/com/google/android/update-binary \
               META-INF/com/google/android/updater-script
git -C zip commit -m "core: mode-agnostic update-binary orchestrator"
```

---

### Task 8: The `diag` mode

**Files:**
- Create: `zip/modes/diag.conf`
- Create: `zip/modes/diag.sh`

- [ ] **Step 1: Write `modes/diag.conf`**

```sh
# shellcheck shell=sh
# modes/diag.conf — declarative config for the diag mode.
# diag is a no-op diagnostic: it writes nothing and bundles no tools.
# Declarative vars only; build-recovery-zip.sh reads MODE_TOOLS/MODE_EFI
# to prune the ZIP. A reviewer reads this file to know what diag touches.

MODE_NAME="diag"
MODE_DESC="no-op environment diagnostic (no writes)"
MODE_WRITES=""        # partitions this mode writes - none
MODE_TOOLS=""         # bundled bin/ tools this mode needs - none
MODE_EFI=""           # base/ EFI this mode needs - none
```

- [ ] **Step 2: Write `modes/diag.sh`**

```sh
# shellcheck shell=sh
# shellcheck disable=SC2154,SC2012
# modes/diag.sh — no-op diagnostic mode.
# Exercises the whole core (env, ui, busybox, partition, safety) with
# zero device writes. Safe to flash on any device. It is also the
# worked reference for SP3/SP4 mode authors: define mode_main, use the
# core helpers, never write outside commit_verified.

mode_main() {
  ui_print "diag: environment report"
  ui_print ""

  if $BOOTMODE; then
    ui_print "  boot mode  : booted Android (flash-from-system)"
  else
    ui_print "  boot mode  : recovery"
  fi
  ui_print "  work dir   : $DIR"

  bb=$(command -v busybox 2>/dev/null)
  if [ -n "$bb" ]; then
    ui_print "  busybox    : $bb"
  else
    ui_print "  busybox    : (none on PATH)"
  fi

  if [ -n "$SLOT" ]; then
    ui_print "  slot       : active=$SLOT inactive=$INACTIVE"
  else
    ui_print "  slot       : not an A/B device"
  fi

  if $OTA_POSTINSTALL; then
    ui_print "  ota state  : update_engine postinstall ACTIVE"
  else
    ui_print "  ota state  : no postinstall window"
  fi

  if [ -n "$BYNAME" ]; then
    n=$(ls "$BYNAME" 2>/dev/null | wc -l)
    ui_print "  by-name dir: $BYNAME ($n partitions)"
    for p in efisp abl_a abl_b recovery vbmeta; do
      if [ -e "$BYNAME/$p" ]; then
        ui_print "    [present] $p"
      else
        ui_print "    [absent ] $p"
      fi
    done
  else
    ui_print "  by-name dir: NOT FOUND"
  fi

  ui_print ""
  ui_print "diag: core OK - no writes performed"
}
```

- [ ] **Step 3: Lint both files**

Run: `shellcheck -s sh zip/modes/diag.conf zip/modes/diag.sh`
Expected: no output, exit 0.

- [ ] **Step 4: Commit (submodule)**

```bash
git -C zip add modes/diag.conf modes/diag.sh
git -C zip commit -m "modes: diag - working no-op core-exercising reference mode"
```

---

### Task 9: Stub modes and `.gitignore`

**Files:**
- Create: `zip/modes/install.conf`, `zip/modes/install.sh`
- Create: `zip/modes/graft.conf`, `zip/modes/graft.sh`
- Create: `zip/modes/profile.conf`, `zip/modes/profile.sh`
- Create: `zip/.gitignore`

- [ ] **Step 1: Write the three stub `.conf` files**

`zip/modes/install.conf`:

```sh
# shellcheck shell=sh
# modes/install.conf — STUB. The install mode body is SP3 (gbl-chainload
# ZIP rework). MODE_TOOLS/MODE_EFI are empty here because the stub aborts
# immediately and bundles nothing; SP3 sets the real artifact needs.

MODE_NAME="install"
MODE_DESC="gbl-chainload EFISP install (SP3 - not yet implemented)"
MODE_WRITES=""
MODE_TOOLS=""
MODE_EFI=""
```

`zip/modes/graft.conf`:

```sh
# shellcheck shell=sh
# modes/graft.conf — STUB. The graft mode body is SP4 (vbmeta graft).
# MODE_TOOLS/MODE_EFI are empty here because the stub aborts immediately;
# SP4 sets the real artifact needs (and adds a vbmeta-graft tool).

MODE_NAME="graft"
MODE_DESC="recovery vbmeta graft (SP4 - not yet implemented)"
MODE_WRITES=""
MODE_TOOLS=""
MODE_EFI=""
```

`zip/modes/profile.conf`:

```sh
# shellcheck shell=sh
# modes/profile.conf — STUB. The profile mode body is a later sub-project
# (mode-2 profile lifecycle). MODE_TOOLS/MODE_EFI empty; the stub aborts.

MODE_NAME="profile"
MODE_DESC="mode-2 profile build/populate (not yet implemented)"
MODE_WRITES=""
MODE_TOOLS=""
MODE_EFI=""
```

- [ ] **Step 2: Write the three stub `.sh` files**

`zip/modes/install.sh`:

```sh
# shellcheck shell=sh
# modes/install.sh — STUB. Implemented in SP3 (gbl-chainload ZIP rework).
mode_main() {
  abort "install mode not yet implemented (SP3)"
}
```

`zip/modes/graft.sh`:

```sh
# shellcheck shell=sh
# modes/graft.sh — STUB. Implemented in SP4 (vbmeta graft).
mode_main() {
  abort "graft mode not yet implemented (SP4)"
}
```

`zip/modes/profile.sh`:

```sh
# shellcheck shell=sh
# modes/profile.sh — STUB. Implemented in a later sub-project.
mode_main() {
  abort "profile mode not yet implemented"
}
```

- [ ] **Step 3: Write `zip/.gitignore`**

```
# modes/SELECTED is generated into the staged tree by build-recovery-zip.sh
# at assembly time; it is never committed.
modes/SELECTED
```

- [ ] **Step 4: Lint the stub files**

Run: `shellcheck -s sh zip/modes/install.conf zip/modes/install.sh zip/modes/graft.conf zip/modes/graft.sh zip/modes/profile.conf zip/modes/profile.sh`
Expected: no output, exit 0.

- [ ] **Step 5: Commit (submodule)**

```bash
git -C zip add modes/install.conf modes/install.sh \
               modes/graft.conf modes/graft.sh \
               modes/profile.conf modes/profile.sh .gitignore
git -C zip commit -m "modes: install/graft/profile abort-stubs + .gitignore SELECTED"
```

---

### Task 10: `update-tools.sh`

**Files:**
- Create: `zip/update-tools.sh`

- [ ] **Step 1: Write the file**

```bash
#!/usr/bin/env bash
# update-tools.sh — refresh the vendored tool binaries and the base EFI
# from a gbl-chainload parent checkout, and (re)write bin/MANIFEST.
#
# Run from inside the submodule checkout. The parent gbl-chainload repo
# is the directory containing this submodule; override with --parent.
#
#   ./update-tools.sh [--parent /path/to/gbl-chainload]
#
# bin/busybox-arm64 is vendored once at bootstrap and NOT rebuilt here;
# it is preserved and re-hashed into the manifest.
set -euo pipefail

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
PARENT=$(cd "$SELF_DIR/.." && pwd)
if [ "${1:-}" = --parent ]; then
  PARENT=$(cd "$2" && pwd); shift 2
fi

[ -f "$PARENT/scripts/build-recovery-tools.sh" ] \
  || { echo "error: $PARENT is not a gbl-chainload checkout" >&2; exit 1; }

echo "==> building recovery tools from $PARENT"
bash "$PARENT/scripts/build-recovery-tools.sh"
echo "==> building the base EFI"
bash "$PARENT/scripts/build.sh" --mode 1

echo "==> copying artifacts into bin/ and base/"
mkdir -p "$SELF_DIR/bin" "$SELF_DIR/base"
for t in fv-unwrap abl-patcher gbl-pack gbl-commit; do
  cp "$PARENT/dist/recovery/$t" "$SELF_DIR/bin/$t"
done
cp "$PARENT/dist/mode-1.efi" "$SELF_DIR/base/mode-1.efi"

[ -f "$SELF_DIR/bin/busybox-arm64" ] \
  || { echo "error: bin/busybox-arm64 missing - vendor it once at bootstrap" >&2; exit 1; }

echo "==> writing bin/MANIFEST"
PCOMMIT=$(git -C "$PARENT" rev-parse HEAD)
# Dirty = the parent's own tracked sources are modified (tools/EFI built
# from an uncommitted state). The zip submodule is excluded: update-tools.sh
# writes into zip/bin and zip/base, so it is dirty by construction here and
# is not a build input.
if git -C "$PARENT" diff --quiet -- ':!zip' \
   && git -C "$PARENT" diff --cached --quiet -- ':!zip'; then
  PDIRTY=0
else
  PDIRTY=1
  echo "WARNING: parent tree is dirty - MANIFEST marked parent-dirty: 1" >&2
fi
{
  echo "# zip-gbl-chainload vendored-artifact manifest"
  echo "# parent-commit: $PCOMMIT"
  echo "# parent-dirty: $PDIRTY"
  ( cd "$SELF_DIR" && sha256sum \
      bin/fv-unwrap bin/abl-patcher bin/gbl-pack bin/gbl-commit \
      bin/busybox-arm64 base/mode-1.efi )
} > "$SELF_DIR/bin/MANIFEST"

echo "==> done. Review, commit the submodule, and bump its pointer in the parent."
```

- [ ] **Step 2: Lint it**

Run: `shellcheck zip/update-tools.sh`
Expected: no output, exit 0.

- [ ] **Step 3: Make it executable and commit (submodule)**

```bash
chmod 755 zip/update-tools.sh
git -C zip add update-tools.sh
git -C zip commit -m "tools: update-tools.sh - refresh vendored bins + MANIFEST"
```

---

### Task 11: Vendor the binaries

**Files:**
- Create: `zip/bin/busybox-arm64`
- Create: `zip/bin/{fv-unwrap,abl-patcher,gbl-pack,gbl-commit}`
- Create: `zip/base/mode-1.efi`
- Create: `zip/bin/MANIFEST`

> **Environment note:** this task runs `scripts/build-recovery-tools.sh` (Docker + Android NDK r27) and `scripts/build.sh` (EDK2). Both must be runnable in the execution environment. If Docker is unavailable, stop and surface this to the user.

- [ ] **Step 1: Vendor a static aarch64 busybox**

`bin/busybox-arm64` is the one third-party binary not built from the parent. Obtain a static aarch64 busybox — the recommended source is a known-good static build from the AnyKernel3 ecosystem (osm0sis static-busybox) or Magisk's `busybox` (arm64, static). Place it and mark it executable:

```bash
# place the obtained binary, then:
chmod 755 zip/bin/busybox-arm64
file zip/bin/busybox-arm64
```

Expected: `file` reports `ELF 64-bit LSB ... ARM aarch64 ... statically linked`.
If no static aarch64 busybox can be sourced in this environment, stop and ask the user to provide one — it is the only artifact this plan cannot produce from the repo.

- [ ] **Step 2: Run `update-tools.sh` to build and copy the project tools + EFIs**

```bash
cd /home/vivy/gbl-chainload/zip
./update-tools.sh
cd /home/vivy/gbl-chainload
```

Expected: ends with `==> done. Review, commit the submodule, ...`. `zip/bin/` now holds the four project tools, `zip/base/` holds `mode-1.efi`, `zip/bin/MANIFEST` exists.

- [ ] **Step 3: Verify the vendored set**

```bash
ls -la zip/bin zip/base
head -3 zip/bin/MANIFEST
( cd zip && sha256sum -c <(grep -E '^[0-9a-f]{64}  ' bin/MANIFEST) )
```

Expected: five files in `bin/` (four tools + busybox-arm64 + MANIFEST), one `.efi` in `base/` (`mode-1.efi`); the `MANIFEST` header shows `# parent-commit:` and `# parent-dirty: 0`; `sha256sum -c` prints `OK` for every line.

- [ ] **Step 4: Commit the vendored artifacts (submodule)**

```bash
git -C zip add bin base
git -C zip commit -m "bin: vendor recovery tools, busybox-arm64, base EFI + MANIFEST"
```

---

### Task 12: Submodule `README.md` and CI

**Files:**
- Modify: `zip/README.md` (overwrite the auto-generated one)
- Create: `zip/.github/workflows/ci.yml`

- [ ] **Step 1: Write `zip/README.md`**

```markdown
# zip-gbl-chainload

Flashable-ZIP packaging for [gbl-chainload](https://github.com/1vivy/gbl-chainload).
This repo is a **submodule** of gbl-chainload, mounted at `zip/`.

## What this is

A single mode-agnostic installer core (`META-INF/com/google/android/update-binary`
+ `core/*.sh`) plus per-mode config. One ZIP carries one mode, named in
`modes/SELECTED` (generated at assembly time). Modes:

- `diag` — no-op environment diagnostic (no writes). Working.
- `install` — gbl-chainload EFISP install. Stub (SP3).
- `graft` — recovery vbmeta graft. Stub (SP4).
- `profile` — mode-2 profile. Stub (later).

## Building a ZIP

ZIPs are assembled from the **parent** gbl-chainload checkout:

```
scripts/build-recovery-zip.sh --mode diag    # -> dist/gbl-chainload-diag.zip
```

## Refreshing vendored tools

`bin/` (recovery tools, busybox) and `base/` (the base EFI) are committed.
After a gbl-chainload change that affects the tools or EFIs:

```
cd zip && ./update-tools.sh        # rebuilds + rewrites bin/MANIFEST
```

then commit this submodule and bump its pointer in the parent. The parent's
`build-recovery-zip.sh` hard-fails if the vendored binaries drift from
`bin/MANIFEST`.

## Attribution

The recovery-environment plumbing in `core/*.sh` (the `ui_print` path-write,
`BOOTMODE` detection, SELinux elevation, by-name resolution) is a partial
fork of [AnyKernel3](https://github.com/osm0sis/AnyKernel3) by osm0sis,
used under its MIT-style license. AnyKernel3's boot-image machinery is not
used — gbl-chainload's modes touch only raw firmware partitions. See
`docs/project/zip-methodology.md` in the parent repo.
```

- [ ] **Step 2: Write `zip/.github/workflows/ci.yml`**

```yaml
name: CI
on:
  push:
  pull_request:
jobs:
  shellcheck:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: shellcheck installer core (busybox-ash dialect)
        run: |
          shellcheck -s sh \
            META-INF/com/google/android/update-binary \
            core/*.sh modes/*.conf modes/*.sh
      - name: shellcheck bash scripts
        run: shellcheck update-tools.sh
```

- [ ] **Step 3: Commit (submodule)**

```bash
git -C zip add README.md .github/workflows/ci.yml
git -C zip commit -m "ci+docs: shellcheck workflow + README with AK3 attribution"
```

---

### Task 13: Push the submodule and bump the parent pointer

**Files:**
- Modify: `zip` (parent gitlink)

- [ ] **Step 1: Push the submodule to GitHub**

```bash
git -C zip push -u origin HEAD:main
```

Expected: the submodule's `main` on GitHub now has all Task 2–12 commits.

- [ ] **Step 2: Verify submodule CI is green**

Run: `gh run list --repo 1vivy/zip-gbl-chainload --limit 1`
Expected: the latest run's status is `completed` / `success` (wait for it if still in progress).

- [ ] **Step 3: Bump the parent pointer**

```bash
cd /home/vivy/gbl-chainload
git add zip
git commit -m "build: advance zip submodule to the SP2 core

Brings in the mode-agnostic update-binary, core/*.sh, the diag mode,
the install/graft/profile stubs, vendored tools, and update-tools.sh.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 14: Write the assembly test (it must fail first)

**Files:**
- Create: `tests/host/071_zip_assembly.sh`

- [ ] **Step 1: Write the test**

```bash
#!/usr/bin/env bash
# tests/host/071_zip_assembly.sh — build-recovery-zip.sh assembly + skew guard.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=tests/host/.last/071
rm -rf "$OUT"; mkdir -p "$OUT"

# --- 1. assemble the diag ZIP ---------------------------------------
bash scripts/build-recovery-zip.sh --mode diag >/dev/null
ZIP=dist/gbl-chainload-diag.zip
[ -f "$ZIP" ] || { echo "FAIL: $ZIP not produced"; exit 1; }

for e in META-INF/com/google/android/update-binary \
         META-INF/com/google/android/updater-script \
         core/ui.sh core/env.sh core/ota.sh core/busybox.sh core/partition.sh core/safety.sh \
         modes/SELECTED modes/diag.conf modes/diag.sh SHA256SUMS; do
  unzip -l "$ZIP" | grep -q "[ /]$e\$" \
    || { echo "FAIL: $ZIP missing $e"; exit 1; }
done

# SELECTED names diag; other modes are pruned out
unzip -p "$ZIP" modes/SELECTED | grep -qx diag \
  || { echo "FAIL: SELECTED is not 'diag'"; exit 1; }
if unzip -l "$ZIP" | grep -qE 'modes/(install|graft|profile)'; then
  echo "FAIL: non-selected modes were not pruned"; exit 1
fi

# SHA256SUMS verifies
unzip -o "$ZIP" -d "$OUT/diag" >/dev/null
( cd "$OUT/diag" && sha256sum -c --status SHA256SUMS ) \
  || { echo "FAIL: SHA256SUMS mismatch"; exit 1; }

# --- 2. skew guard fires on a tampered MANIFEST ---------------------
SUB=zip
cp "$SUB/bin/MANIFEST" "$OUT/MANIFEST.orig"
trap 'cp "$OUT/MANIFEST.orig" "$SUB/bin/MANIFEST"' EXIT
# zero the first hash line so the recomputed hash will not match
sed '0,/^[0-9a-f]\{64\}  /s//0000000000000000000000000000000000000000000000000000000000000000  /' \
  "$OUT/MANIFEST.orig" > "$SUB/bin/MANIFEST"
if bash scripts/build-recovery-zip.sh --mode diag >/dev/null 2>&1; then
  echo "FAIL: skew guard did not fire on a tampered MANIFEST"; exit 1
fi
cp "$OUT/MANIFEST.orig" "$SUB/bin/MANIFEST"

echo "PASS: 071 zip assembly + skew guard"
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `bash tests/host/071_zip_assembly.sh`
Expected: FAIL — `scripts/build-recovery-zip.sh` does not exist yet (the run errors out before any `PASS` line).

- [ ] **Step 3: Commit (parent)**

```bash
git add tests/host/071_zip_assembly.sh
git commit -m "test: 071 zip assembly + skew guard (failing - assembler next)"
```

---

### Task 15: `scripts/build-recovery-zip.sh`

**Files:**
- Create: `scripts/build-recovery-zip.sh`

- [ ] **Step 1: Write the assembler**

```bash
#!/usr/bin/env bash
# scripts/build-recovery-zip.sh — assemble a single-mode installer ZIP
# from the zip-gbl-chainload submodule.
#
#   build-recovery-zip.sh --mode diag|install|graft|profile
#
# Hard-fails if the submodule's vendored binaries have drifted from
# zip/bin/MANIFEST (run zip/update-tools.sh to refresh).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

MODE=""
[ "${1:-}" = --mode ] && MODE="${2:-}"
case "$MODE" in
  diag|install|graft|profile) ;;
  *) echo "usage: $0 --mode diag|install|graft|profile" >&2; exit 2 ;;
esac

SUB=zip
[ -f "$SUB/META-INF/com/google/android/update-binary" ] \
  || { echo "error: submodule '$SUB' not checked out (git submodule update --init)" >&2; exit 1; }

# --- skew guard -----------------------------------------------------
MAN="$SUB/bin/MANIFEST"
[ -f "$MAN" ] || { echo "error: $MAN missing - run $SUB/update-tools.sh" >&2; exit 1; }

( cd "$SUB" && grep -E '^[0-9a-f]{64}  ' bin/MANIFEST | sha256sum -c --status ) \
  || { echo "error: vendored binaries are stale vs $MAN - run $SUB/update-tools.sh" >&2; exit 1; }

PDIRTY=$(sed -n 's/^# parent-dirty: //p' "$MAN")
[ "$PDIRTY" = 0 ] \
  || { echo "error: $MAN marks a dirty-tree build - re-run update-tools.sh on a clean tree" >&2; exit 1; }

PCOMMIT=$(sed -n 's/^# parent-commit: //p' "$MAN")
if git cat-file -e "${PCOMMIT}^{commit}" 2>/dev/null; then
  git merge-base --is-ancestor "$PCOMMIT" HEAD 2>/dev/null \
    || { echo "error: $MAN parent-commit $PCOMMIT is not an ancestor of HEAD - re-run update-tools.sh" >&2; exit 1; }
else
  echo "warning: $MAN parent-commit $PCOMMIT not in local history (shallow clone?) - ancestor check skipped" >&2
fi

# --- stage, select the mode, prune ----------------------------------
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -r "$SUB"/. "$STAGE"/
rm -rf "$STAGE/.git" "$STAGE/.github" "$STAGE/.gitignore" \
       "$STAGE/update-tools.sh" "$STAGE/README.md"

echo "$MODE" > "$STAGE/modes/SELECTED"
for f in "$STAGE"/modes/*.conf "$STAGE"/modes/*.sh; do
  b=$(basename "$f"); m=${b%.*}
  [ "$m" = "$MODE" ] || rm -f "$f"
done

# read the selected mode's declared artifact needs
MODE_TOOLS=""; MODE_EFI=""
# shellcheck disable=SC1090
. "$SUB/modes/$MODE.conf"

# prune bin/: keep MANIFEST, busybox-arm64, and tools listed in MODE_TOOLS
for t in "$STAGE"/bin/*; do
  b=$(basename "$t")
  [ "$b" = MANIFEST ] && continue
  [ "$b" = busybox-arm64 ] && continue
  case " $MODE_TOOLS " in *" $b "*) ;; *) rm -f "$t" ;; esac
done
# prune base/: keep only MODE_EFI (if any)
for e in "$STAGE"/base/*; do
  [ -e "$e" ] || continue
  b=$(basename "$e")
  [ "$b" = "$MODE_EFI" ] || rm -f "$e"
done
rmdir "$STAGE/base" 2>/dev/null || true

# --- checksums + zip ------------------------------------------------
( cd "$STAGE" && find . -type f ! -name SHA256SUMS -exec sha256sum {} + \
    | sed 's#  \./#  #' > SHA256SUMS )
mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/gbl-chainload-$MODE.zip"
rm -f "$OUT"
( cd "$STAGE" && zip -qr "$OUT" . )
echo "==> $OUT"
unzip -l "$OUT"
```

- [ ] **Step 2: Lint it**

Run: `shellcheck scripts/build-recovery-zip.sh`
Expected: no output, exit 0.

- [ ] **Step 3: Make it executable**

Run: `chmod 755 scripts/build-recovery-zip.sh`
Expected: no output.

- [ ] **Step 4: Run test 071 and confirm it now passes**

Run: `bash tests/host/071_zip_assembly.sh`
Expected: final line `PASS: 071 zip assembly + skew guard`.

- [ ] **Step 5: Run the full host suite to confirm nothing regressed**

Run: `bash tests/runall.sh`
Expected: `071_zip_assembly` appears in the output and the final line is `ALL TESTS PASS`.

- [ ] **Step 6: Commit (parent)**

```bash
git add scripts/build-recovery-zip.sh
git commit -m "build: build-recovery-zip.sh - single-mode ZIP assembler + skew guard"
```

---

### Task 16: Finalize — push and open the PR

**Files:** none (integration only)

- [ ] **Step 1: Push the parent branch**

```bash
git push -u origin feature/zip-methodology
```

- [ ] **Step 2: Open the PR**

```bash
gh pr create --base main --head feature/zip-methodology \
  --title "ZIP methodology + SP2 packaging structure" \
  --body "$(cat <<'EOF'
Adds the ZIP-methodology reference (SP1) and the zip-gbl-chainload
packaging structure (SP2), plus the vbmeta graft-vs-construct analysis.

- docs/project/zip-methodology.md — AK3 + project-lessons reference (SP1)
- docs/project/vbmeta-graft-vs-construct.md — cryptographic reference
- docs/superpowers/specs + plans — SP2 design and this plan
- zip/ — new zip-gbl-chainload submodule: mode-agnostic update-binary,
  core/*.sh, working diag mode, install/graft/profile stubs, vendored
  tools + update-tools.sh
- scripts/build-recovery-zip.sh — single-mode assembler with skew guard
- tests/host/071_zip_assembly.sh — assembly + skew-guard test

SP3 (install mode), SP4 (graft mode), and the mode-2 profile mode fill
the stubs in follow-up work.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Confirm CI**

Run: `gh pr checks`
Expected: the `test` check passes (it runs `tests/runall.sh`, which includes `071`).

- [ ] **Step 4: On-device validation (user-run, manual — not agent-run)**

This is the SP2 acceptance gate and must be run by the user on real hardware:

```
scripts/build-recovery-zip.sh --mode diag      # build the diag ZIP
# flash dist/gbl-chainload-diag.zip in TWRP/OrangeFox
```

Expected on the recovery screen: the `diag: environment report` block —
boot mode, work dir, busybox path, slot, and the by-name partition list —
followed by `diag: core OK - no writes performed` and a successful
install. This confirms the core (`ui_print`, busybox setup, env
detection, partition enumeration, clean exit) works on a real recovery
with zero device risk.

---

## Self-Review

**Spec coverage:**

- Repo & submodule at `zip/` → Task 1.
- Mode-agnostic `update-binary` + mode-as-config (`SELECTED`, `.conf`/`.sh`) → Task 7, 8, 9.
- `core/*.sh` partial AK3 fork with attribution → Tasks 2–6 (headers), Task 12 (README).
- OTA-state detection (`core/ota.sh` → `OTA_POSTINSTALL`) → Task 3; reported by `diag` → Task 8.
- Vendored `bin/`/`base/` + `update-tools.sh` + `MANIFEST` → Tasks 10, 11.
- Skew guard → Task 15 (`build-recovery-zip.sh`), tested in Task 14.
- `build-recovery-zip.sh --mode` with prune → Task 15.
- CI: submodule `shellcheck` → Task 12; parent assembly/skew smoke → Task 14/15 via `runall.sh`.
- Working `diag` mode → Task 8; stubs → Task 9.
- On-device `diag` acceptance → Task 16 Step 4.

**Placeholder scan:** none — every file has complete content. `bin/busybox-arm64` acquisition (Task 11 Step 1) is the one step that may need the user; this is flagged explicitly, not left as a TODO.

**Type/name consistency:** `mode_main` (defined in every `modes/*.sh`, called by `update-binary`), `MODE_TOOLS`/`MODE_EFI`/`MODE_WRITES`/`MODE_NAME`/`MODE_DESC` (set in every `.conf`, read by `build-recovery-zip.sh`), `ui_print`/`abort`/`cleanup`/`commit_verified`/`byname` (core), `BYNAME`/`SLOT`/`INACTIVE`/`BOOTMODE`/`DIR`/`shcon`/`WORKDIR`/`OUTFD`/`ZIPFILE` — all consistent across tasks. `bin/MANIFEST` format (`# parent-commit:`/`# parent-dirty:` + `sha256sum` lines) is written by `update-tools.sh` (Task 10) and consumed identically by `build-recovery-zip.sh` (Task 15).
