# gbl-chainload ZIP methodology

A relied-upon methodology for every flashable ZIP this project ships
(`gbl-chainload` installer, the future mode-2 profile ZIP, the future
recovery/vbmeta graft ZIP).

**Audience:** a contributor — or a future Claude session — with no
recovery-scripting background. Read Part A to understand the domain; copy
Part B to start a new ZIP.

**Grounding:** the patterns here are lifted from
[AnyKernel3](https://github.com/osm0sis/AnyKernel3) (osm0sis) — the
de-facto gold standard for portable flashable ZIPs — and from this
project's own on-device experience. We take AK3's *methodology,
portability, and safety patterns*; we do **not** take AK3's kernel- and
ramdisk-patching machinery (out of scope here).

**Boundaries.** This doc provides a skeleton (Part B). It does **not**
decide where ZIPs live or how they're packaged — that is a separate
sub-project. Sub-projects that build actual ZIPs (gbl-chainload rework,
mode-2, graft) instantiate Part B's skeleton and are written against
Part A's conventions.

---

# Part A — Reference (the why)

## A1. The update-binary contract

A flashable ZIP is just a ZIP whose `META-INF/com/google/android/`
directory contains an `update-binary`. When the user installs the ZIP,
the recovery extracts that one file and executes it:

```
update-binary  <api_version>  <OUTFD>  <zipfile>
```

- `$1` — recovery updater API version (typically `3`). Usually unused.
- `$2` — **OUTFD**: the numeric file descriptor the recovery reads
  back for on-screen commands (see A2).
- `$3` — absolute path to the ZIP being flashed.

`update-binary` may be an ELF (the classic `updater` + edify) **or** a
shell script. Modern TWRP / OrangeFox detect a non-ELF and run it as a
shell script directly — that is what we do. Exit `0` = success; any
non-zero exit makes the recovery report the install as failed/aborted.

`META-INF/com/google/android/updater-script` is the legacy edify script.
With a shell `update-binary` it carries no logic, but some recovery code
paths still `stat` it, so keep a one-line stub (see Part B).

## A2. ui_print & recovery I/O

The recovery hands `update-binary` an open file descriptor (`OUTFD`) and
reads command lines back from it. The only command we use:

```
ui_print <text>     # render <text> on the recovery screen
ui_print            # render a blank line
```

**The portable idiom (AK3's, and what we prescribe):**

```sh
ui_print() { echo "ui_print $1
ui_print" >> /proc/self/fd/$OUTFD; }
```

Two things make this portable *by construction*:

1. **Write to `/proc/self/fd/$OUTFD` as a path**, not `>&$OUTFD`.
   A path always works. The fd-duplication redirection `>&` is fragile:
   `>&"$OUTFD"` (quoted) is rejected outright by busybox `ash`, and even
   unquoted `>&$OUTFD` varies across `ash` builds.
2. **A literal newline inside the quoted string**, not `echo -e "\n"`.
   busybox `echo` builtin `-e` support is inconsistent — some builds
   print a literal `-e`.

**Worked example — the bug this doc exists to prevent.** The first
`gbl-chainload` installer used:

```sh
ui_print() { echo -e "ui_print $1\nui_print" >&"$OUTFD"; }   # WRONG
```

On OrangeFox's busybox `ash` every call produced
`/tmp/updater[N]: >&54 : illegal file descriptor name`. The install
still completed (the failed redirection was non-fatal) but the user saw
*zero* progress output. The PR-#23 installer was fixed to the unquoted-fd
+ plain-`echo` form; this doc prescribes going one better — AK3's
`/proc/self/fd` path-write, which cannot hit the trap at all.

## A3. Shell & busybox portability

`#!/sbin/sh` in a recovery is **busybox `ash`** (occasionally `mksh`) —
it is **not** bash. Write to the POSIX-sh / busybox-ash intersection:

| Avoid | Use instead |
|---|---|
| `echo -e "...\n..."` | a literal newline in the string, or `printf` |
| `>&"$fd"` / `>&$fd` | `>> /proc/self/fd/$fd` (path) |
| `[[ ... ]]` | `[ ... ]` |
| bash arrays | positional params, or space-lists + `for` |
| `${var^^}`, `${var//a/b}` (bash) | `tr`, `sed` |
| assuming `set -o pipefail` | works on recent busybox; don't rely on it |

`local` *is* supported by busybox `ash` and is fine to use.

**Applet availability is not guaranteed.** A recovery's busybox is built
with a subset of applets. `dd`, `getprop`, `getevent`, `mount`, `dirname`,
`grep`, `unzip` are reliably present. `xxd` is frequently **absent** —
prefer `od` (`od -An -tx1`) or do binary inspection inside a bundled C
tool. `sha256sum` may be absent — we avoid depending on it by doing
hash verification inside our own `gbl-commit` tool.

**Bundling tools.** AK3 ships per-arch tool dirs (`tools/arm`,
`tools/x86`), probes which busybox runs, relocates the matching arch into
`tools/`, and prepends it to `PATH`. Our targets are aarch64-only, so we
bundle aarch64 static binaries directly under `bin/` and do not need the
arch probe. Principle: **bundle every tool whose behaviour is
load-bearing** (our `fv-unwrap`, `abl-patcher`, `gbl-pack`, `gbl-commit`
are static aarch64 C binaries — they behave identically regardless of the
recovery's environment); rely on the recovery's busybox only for
ubiquitous applets.

## A4. Boot-mode detection & flash-from-system

A ZIP can be flashed from **custom recovery** *or* straight from a
**booted, rooted Android system** (a Magisk/KSU module-style install).
AK3 supports both; so should we.

**Detect the environment:**

```sh
BOOTMODE=false
ps | grep zygote | grep -v grep >/dev/null 2>&1 && BOOTMODE=true
$BOOTMODE || ps -A 2>/dev/null | grep zygote | grep -v grep >/dev/null 2>&1 && BOOTMODE=true
```

`zygote` in the process list ⇒ a full Android is running ⇒ flash-from-
system. **Pick the working/scratch directory accordingly:**

```sh
DIR=/sdcard
$BOOTMODE || DIR=$(dirname "$ZIPFILE")
[ "$DIR" = /sideload ] && DIR=/tmp
```

**Feasibility for our ZIPs — honest assessment.** Our installers write
`efisp` and `abl` (and, later, recovery/vbmeta). Those are
**bootloader-stage partitions: the running kernel and OS never touch them
at runtime**, so a one-shot `dd` to them is fundamentally as safe live as
it is in recovery. Flash-from-system is therefore a legitimate path for
this project — with three requirements the ZIP must honour:

1. **Root.** `/dev/block/by-name/` access needs root. Flash-from-system
   only happens via a rooted boot (Magisk/KSU). In recovery, root is
   implicit.
2. **SELinux context.** A booted system runs SELinux *enforcing*; a raw
   block-device write may be denied even as root. Elevate the context
   for the duration, then restore it (AK3's trick):

   ```sh
   shcon=$(cat /proc/self/attr/current 2>/dev/null)
   echo "u:r:su:s0" > /proc/self/attr/current 2>/dev/null   # elevate
   # ... block-device work ...
   [ -n "$shcon" ] && echo "$shcon" > /proc/self/attr/current 2>/dev/null  # restore
   ```

   Recovery is typically permissive, so this is a no-op there — harmless
   to run unconditionally.
3. **No interactive prompt under `BOOTMODE`.** Our recovery ZIPs use a
   `getevent` vol-key abort prompt — that needs a recovery screen the
   user is watching. From a booted-system module install there is no such
   screen. **When `$BOOTMODE`, skip the interactive prompt**: installing
   the module is itself the consent. Print the plan via `ui_print` and
   proceed. Keep the prompt for the `!BOOTMODE` (recovery) path.

**Status.** Flash-from-system is *designed for* (Part B's skeleton
handles it) but **not yet validated on-device** — every on-device install
to date has been from recovery. Treat the booted-system path as
implemented-but-unverified until a real Magisk/KSU-module install is
tested.

## A5. Partition & slot helpers

**Resolve a partition to a block device** via its by-name symlink:

```sh
/dev/block/by-name/<part>
```

If that directory is absent on a given device, fall back the way AK3's
`mount_all` does:

```sh
BYNAME=/dev/block/bootdevice/by-name
[ -d $BYNAME ] || BYNAME=$(find /dev/block/platform -type d -name by-name 2>/dev/null | head -1)
```

**A/B slots.** Slotted devices suffix partitions `_a` / `_b`:

```sh
SLOT=$(getprop ro.boot.slot_suffix)   # "_a" or "_b"
SLOT=${SLOT#_}                        # "a" or "b"
case "$SLOT" in
  a) INACTIVE=b ;;
  b) INACTIVE=a ;;
  *) abort "no slot suffix — not an A/B device?" ;;
esac
```

`abl_$SLOT` is the live-firmware ABL; `abl_$INACTIVE` is the post-OTA
slot's ABL. Which one a given ZIP reads/writes is a per-ZIP decision
(documented in that ZIP's design), not a methodology default.

## A6. Safety discipline

Every destructive ZIP follows the same order. The invariant:
**no failure path ever leaves a half-written partition with no backup.**

1. **Pre-flight gates — before the first write.** Validate everything:
   slot known, source partitions readable, required `/sdcard/*` inputs
   present, the write target is what we expect (e.g. `efisp` currently
   begins with a PE `MZ`). If any gate fails, `abort` — nothing has been
   written, so the device is untouched.
2. **Backup-before-write.** `dd` the target partition to a backup on
   `/sdcard` *before* overwriting it. The backup must be **byte-exact** —
   an earlier bug truncated the EFISP backup by up to ~1 MiB because
   `count` was computed by integer-dividing the partition size by `1M`.
   Back up the exact byte count.
3. **Verify-after-write.** Re-read the target, SHA-256 compare against
   the intended bytes, and **auto-restore the backup on mismatch**. Our
   `gbl-commit --backup --verify` does backup + write + verify +
   restore-on-mismatch in one step — prefer it over hand-rolled `dd`.
4. **`abort()`.** On any failure: `ui_print` the reason, (optionally)
   emit a debug archive of the working dir, restore SELinux context and
   any mounts, conditionally clean up the scratch dir, `exit 1`.

On `set -euo pipefail`: `set -u` is unambiguously good. `set -e` is fine
but know its edges — a failed redirection on a simple command may or may
not abort depending on the shell, and `||`-guarded commands never do.
Don't *rely* on `-e` to catch a destructive command's failure; check the
ones that matter explicitly.

## A7. vbmeta / AVB — flag-flip vs. descriptor graft

Two different things share the word "vbmeta"; do not conflate them.

- **vbmeta flag flip** — AK3's `PATCH_VBMETA_FLAG`. A vbmeta image's
  header has "disable verity" / "disable verification" flag bits; flipping
  them is a 1–2 byte edit. AK3 exposes `PATCH_VBMETA_FLAG=auto|0|1` and
  `NO_VBMETA_PARTITION_PATCH=1`. Small, generic.
- **AVB descriptor graft** — taking *stock* AVB metadata (hash /
  hashtree descriptors + the signed vbmeta footer) and grafting it onto a
  modified partition image so the partition still verifies as stock. This
  is what the recovery-graft sub-project needs. This project's working
  approach (see project memory `graft-at-natural-offset-wins`): paste the
  stock OEM vbmeta bytes at `round_up(custom_content_size, 4K)`.

This doc only draws the distinction. The descriptor-graft mechanism is
designed in the recovery-graft sub-project, not here.

---

# Part B — Annotated skeleton

Copy `update-binary` below to start a new ZIP. Replace the two clearly
marked placeholder blocks; leave the scaffolding intact. It is written to
run under both recovery and a booted rooted system (A4).

### `META-INF/com/google/android/update-binary`

```sh
#!/sbin/sh
#
# Flashable-ZIP bootstrap. Recovery (or a rooted booted system) execs:
#     update-binary  <api>  <OUTFD>  <zipfile>
# Conventions: docs/project/zip-methodology.md

OUTFD=$2            # fd the recovery reads on-screen commands from
ZIPFILE="$3"        # absolute path to this ZIP

# --- ui_print: write to the fd via its /proc path (A2). Never >&$fd;
#     never echo -e. A literal newline gives the blank spacer line. ---
ui_print() {
  echo "ui_print $1
ui_print" >> /proc/self/fd/"$OUTFD"
}

# --- cleanup runs on every exit; abort is the loud failure path (A6) ---
WORKDIR=""
restore_env() {
  [ -n "$shcon" ] && echo "$shcon" > /proc/self/attr/current 2>/dev/null
}
cleanup() {
  restore_env
  [ -n "$WORKDIR" ] && rm -rf "$WORKDIR"
}
abort() {
  ui_print "ABORT: $1"
  cleanup
  exit 1
}
trap cleanup EXIT

# --- environment detection (A4) ---
BOOTMODE=false
ps | grep zygote | grep -v grep >/dev/null 2>&1 && BOOTMODE=true
$BOOTMODE || ps -A 2>/dev/null | grep zygote | grep -v grep >/dev/null 2>&1 && BOOTMODE=true

DIR=/sdcard
$BOOTMODE || DIR=$(dirname "$ZIPFILE")
[ "$DIR" = /sideload ] && DIR=/tmp

# --- SELinux: elevate so raw block writes are permitted under a booted
#     enforcing system; no-op (harmless) in a permissive recovery (A4) ---
shcon=$(cat /proc/self/attr/current 2>/dev/null)
echo "u:r:su:s0" > /proc/self/attr/current 2>/dev/null

# --- unzip ourselves into a private scratch dir ---
WORKDIR=/tmp/gbl-zip.$$
mkdir -p "$WORKDIR"
unzip -o "$ZIPFILE" -d "$WORKDIR" >/dev/null 2>&1 || abort "unzip failed"
chmod 755 "$WORKDIR"/bin/* 2>/dev/null   # bundled aarch64 static tools
export PATH="$WORKDIR/bin:$PATH"

ui_print "<zip name> installer"
ui_print "===================="

# === PRE-FLIGHT GATES =======================================
# ZIP-specific. Validate EVERYTHING before any destructive write:
# slot resolved, source partitions readable, required /sdcard/*
# inputs present, write target is as expected. abort on any failure
# — nothing is written yet, so the device is untouched. (A5, A6)
#
#   SLOT=$(getprop ro.boot.slot_suffix); SLOT=${SLOT#_}
#   [ -e /dev/block/by-name/<part> ] || abort "<part> missing"
#   ...
# ============================================================

# --- interactive abort prompt: recovery only; skipped under BOOTMODE
#     because there is no screen to watch — the module install is the
#     consent (A4) ---
if ! $BOOTMODE; then
  ui_print "Vol-DOWN within 5s to ABORT; anything else continues."
  KEY=$(timeout 5 getevent -lqc 5 2>/dev/null \
          | grep -m1 -oE 'KEY_(VOLUMEUP|VOLUMEDOWN)' || true)
  [ "$KEY" = KEY_VOLUMEDOWN ] && abort "user aborted"
fi

# === WORK ===================================================
# ZIP-specific steps. Follow the safety order (A6):
#   backup-before-write  ->  write  ->  verify-after-write
# Prefer the bundled gbl-commit (--backup --verify) over hand dd.
# ui_print a "[N/total]" line before each step.
# ============================================================

ui_print ""
ui_print "DONE."
exit 0
```

### `META-INF/com/google/android/updater-script`

```
# scripted via update-binary — see ./update-binary (A1)
```

---

## How the other sub-projects use this

- **Packaging sub-project** — decides where ZIP sources live and how
  they're templated; the skeleton above is the per-ZIP starting point.
- **gbl-chainload ZIP rework** — instantiates Part B; its pre-flight and
  work blocks do extract → patch → pack → concat → backup → `dd`+verify;
  it adds the active-slot-ABL vs `/sdcard/backup_abl.img` choice.
- **Recovery/vbmeta graft ZIP** — instantiates Part B; its work block
  does AVB descriptor grafting (A7), designed in that sub-project.

When a ZIP deviates from a Part A convention, that ZIP's own design doc
must say so and why.
