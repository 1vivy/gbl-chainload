#!/usr/bin/env bash
# test-device-automatic.sh — automated end-to-end cycle for testing a
# gbl-chainload EFI payload.
#
# Model (clean rewrite, 2026-05-10):
#   * Device starts in OUR gbl-chainload FastbootLib (any GBL_MODE).
#     The flashed/chainloaded EFI auto-boots into FastbootLib regardless of
#     the boot reason — reaching it is a side-effect of `fastboot reboot
#     bootloader` (or any reboot to fastboot). If you're in stock bootloader
#     fastboot, this script will tell you to reboot first.
#   * Stage the test payload, run it via `oem boot-efi`.
#   * Detect one of three outcomes via a 60s post-boot-efi state machine:
#       A — payload boots through to recovery (adb up). Pull logs.
#       B — payload crashes, device reboots into our FastbootLib again.
#           Issue `oem escape` to chain via patched ABL into recovery,
#           pull crash logs.
#       C — neither for 60s. Likely powered off. User assistance required.
#   * Return device to bootloader fastboot for the next iteration.
#     `adb reboot bootloader` triggers Phoenix-safe transition; our flashed
#     EFI auto-relands us in FastbootLib by the time we return.
#
# Phoenix watchdog: OnePlus's bootloader runs a 60s watchdog from "entered
# fastboot". If pulls take too long the device drops to stock fastboot and
# the in-flight EFI session is gone. The Phoenix stopwatch monitors and
# bails before Phoenix fires.
#
# Usage:
#   ./scripts/test-device-automatic.sh                          # default payload
#   ./scripts/test-device-automatic.sh dist/<my-efi>.efi        # specific
#   GBL_TEST_RETURN_TO_FASTBOOT=0 ./scripts/test-device-automatic.sh
#     (leave device in adb state after pull, don't reboot back to bootloader)
#
# Output: logs/<timestamp>_auto_<label>_v<version>/

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO_ROOT/scripts/device-monitor.sh"

# Default payload — pick the first that exists.
PAYLOAD=""
for candidate in \
  "$REPO_ROOT/dist/mode-debug.efi" \
  "$REPO_ROOT/dist/mode-1-auto-debug-verbose.efi" \
  "$REPO_ROOT/dist/mode-1-auto-debug.efi" \
  "$REPO_ROOT/dist/mode-1.efi" \
  "$REPO_ROOT/dist/gbl-chainload.efi"; do
  if [[ -f "$candidate" ]]; then
    PAYLOAD="$candidate"
    break
  fi
done

RETURN_TO_FASTBOOT="${GBL_TEST_RETURN_TO_FASTBOOT:-1}"

# Positional arg = explicit payload override.
if [[ $# -ge 1 ]]; then
  case "$1" in
    --*)
      echo "error: unknown option: $1" >&2
      exit 1
      ;;
    *)
      PAYLOAD="$1"
      ;;
  esac
fi

if [[ -z "$PAYLOAD" || ! -f "$PAYLOAD" ]]; then
  echo "error: no payload found. Pass a path or build a default first:" >&2
  echo "       ./scripts/build.sh --mode 1 --auto --debug --verbose" >&2
  exit 1
fi

TS="$(date +%Y%m%d-%H%M%S)"

# ---------------------------------------------------------------------------
# Step 1 — confirm device is in OUR FastbootLib (no log dir yet; we read the
# build name from the device and use IT to name the log dir).

echo "======================================================================"
echo "  test-device-automatic.sh"
echo "  payload  : $PAYLOAD ($(stat -c%s "$PAYLOAD") bytes)"
echo "  return   : $RETURN_TO_FASTBOOT"
echo "======================================================================"

echo
echo ">>> [1/4] confirming device is in our FastbootLib (via getvar build-name)"
if ! device_monitor_in_fastboot_quick; then
  echo "error: no fastboot device detected. Power on into bootloader and rerun." >&2
  exit 1
fi
DEVICE_BUILD_NAME="$(device_monitor_build_name)"
if [[ -z "$DEVICE_BUILD_NAME" ]]; then
  echo "error: device responded but is NOT our FastbootLib (getvar build-name" >&2
  echo "       returned nothing recognizable). Recovery options:" >&2
  echo "         1) \`fastboot reboot bootloader\` — our flashed chainloader EFI" >&2
  echo "            should auto-boot into FastbootLib." >&2
  echo "         2) If our EFI isn't flashed: flash a mode-* build to uefi_a/uefi_b" >&2
  echo "            and try again." >&2
  exit 1
fi

# Log dir derives its label from the device's build-name (single source of
# truth). version from git for traceability.
LABEL="$DEVICE_BUILD_NAME"
VERSION=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
LOG_DIR="$REPO_ROOT/logs/${TS}_auto_${LABEL}_v${VERSION}"
mkdir -p "$LOG_DIR"
device_monitor_start "$LOG_DIR" "test-device-automatic"
trap 'device_monitor_stop' EXIT INT TERM

echo "    in our FastbootLib: $DEVICE_BUILD_NAME"
echo "    log dir            : $LOG_DIR"

# ---------------------------------------------------------------------------
# Step 2 — stage + oem boot-efi

echo
echo ">>> [2/4] fastboot stage + oem boot-efi  ($(basename "$PAYLOAD"))"
device_monitor_fastboot stage "$PAYLOAD"

# Task 9 (edk2 dc32586345+): oem boot-efi replies OKAY started BEFORE
# StartImage so fastboot client exits 0. Older firmware drops USB without
# replying ("Status read failed (No such device)") — also valid.
BOOTEFI_LOG="$LOG_DIR/fastboot-oem-boot-efi.txt"
device_monitor_fastboot oem boot-efi 2>&1 \
  | tee "$BOOTEFI_LOG" \
  | grep -v "Status read failed" \
  || true
if grep -qi "OKAY.*started" "$BOOTEFI_LOG"; then
  echo "    boot-efi: OKAY started — clean handoff"
elif grep -q "Status read failed" "$BOOTEFI_LOG"; then
  echo "    boot-efi: USB drop on handoff (older firmware path)"
elif grep -q FAILED "$BOOTEFI_LOG"; then
  echo "error: oem boot-efi FAILED before StartImage. Check $BOOTEFI_LOG." >&2
  exit 1
fi

device_monitor_phoenix_start
echo "    Phoenix stopwatch started — must reach bootloader within ${_PHOENIX_KILL}s"

# ---------------------------------------------------------------------------
# Step 3 — post-boot-efi state machine

echo
echo ">>> [3/4] waiting for outcome (A=adb up | B=back in our FastbootLib | C=power off)"

WAIT_LIMIT=60
WAIT_ELAPSED=0
OUTCOME=""
while (( WAIT_ELAPSED < WAIT_LIMIT )); do
  if device_monitor_in_adb_quick; then
    OUTCOME="A"
    break
  fi
  if device_monitor_in_fastboot_quick; then
    if device_monitor_is_our_fastbootlib; then
      OUTCOME="B"
    else
      OUTCOME="B-stock"
    fi
    break
  fi
  sleep 3
  WAIT_ELAPSED=$((WAIT_ELAPSED + 3))
  echo "    waiting (${WAIT_ELAPSED}s/${WAIT_LIMIT}s)..."
done

case "$OUTCOME" in
  A)
    echo "    outcome A: payload booted to adb."
    ;;
  B)
    BNAME="$(device_monitor_build_name)"
    echo "    outcome B: payload crashed, back in our FastbootLib ($BNAME)."
    echo "    issuing oem escape → patched ABL → recovery..."
    if ! device_monitor_phoenix_check; then
      echo "error: Phoenix watchdog deadline — bailing before stock-fastboot wedge" >&2
      exit 1
    fi
    device_monitor_fastboot oem escape 2>&1 | tee "$LOG_DIR/fastboot-oem-escape.txt" | head -5 || true
    echo "    waiting for adb (recovery) after escape..."
    if ! device_monitor_wait_for_adb_state 120 >/dev/null; then
      echo "error: outcome B but adb did not come up after oem escape within 120s." >&2
      exit 1
    fi
    ;;
  B-stock)
    echo "error: outcome B-stock: device fell to stock fastboot (Phoenix or hard crash)." >&2
    echo "       power off, power on into bootloader, rerun." >&2
    exit 1
    ;;
  "")
    echo "error: outcome C: no fastboot AND no adb for ${WAIT_LIMIT}s — device powered off." >&2
    echo "       power on into bootloader, rerun." >&2
    exit 1
    ;;
esac

# ---------------------------------------------------------------------------
# Step 4 — pull logs

echo
echo ">>> [4/4] pulling logs from $(adb get-state 2>/dev/null) state"
if ! device_monitor_phoenix_check; then
  echo "error: Phoenix watchdog deadline — skipping pulls to avoid wedge" >&2
  exit 1
fi

adb pull /proc/bootloader_log "$LOG_DIR/bootloader_log" 2>/dev/null \
  || echo "    /proc/bootloader_log not present — skipping"
adb pull /proc/bootconfig "$LOG_DIR/bootconfig" 2>/dev/null \
  || echo "    /proc/bootconfig not present — skipping"
adb pull /proc/cmdline "$LOG_DIR/cmdline" 2>/dev/null \
  || echo "    /proc/cmdline not present — skipping"
adb shell 'if [ -d /proc/device-tree ]; then tar -C /proc -cf /tmp/device-tree.tar device-tree; elif [ -d /sys/firmware/devicetree/base ]; then tar -C /sys/firmware/devicetree -cf /tmp/device-tree.tar base; else exit 1; fi' \
  >/dev/null 2>&1 && \
  adb pull /tmp/device-tree.tar "$LOG_DIR/device-tree.tar" >/dev/null 2>&1 && \
  adb shell 'rm -f /tmp/device-tree.tar' >/dev/null 2>&1 \
  || echo "    device-tree snapshot not available — skipping"
adb shell dmesg > "$LOG_DIR/dmesg.txt" 2>/dev/null || echo "    dmesg failed — skipping"
adb shell 'getprop | grep -E "^\[ro\.boot\.|^\[ro\.bootmode|^\[ro\.bootloader"' \
  > "$LOG_DIR/getprop.boot.txt" 2>/dev/null || true

# Recovery context: '/' is writable, no su needed, by-name symlinks work
# directly. (System-context handling lives in scripts/test-device-manual.sh.)
adb shell 'mkdir -p /logfs && mount -t vfat /dev/block/by-name/logfs /logfs' \
  2>/dev/null \
  || echo "    logfs mount may have failed (already mounted? no vfat?) — pulling anyway"
mkdir -p "$LOG_DIR/logfs"
adb pull /logfs/. "$LOG_DIR/logfs/" 2>/dev/null \
  || echo "    logfs pull failed — partition unmapped or empty"
adb shell 'umount /logfs && rmdir /logfs' 2>/dev/null || true

# ---------------------------------------------------------------------------
# Return to bootloader for the next iteration (unless suppressed).

echo
if [[ "$RETURN_TO_FASTBOOT" == "0" ]]; then
  echo "    leaving device in current adb state (GBL_TEST_RETURN_TO_FASTBOOT=0)."
  exit 0
fi

echo "    rebooting to bootloader for next iteration..."
adb reboot bootloader 2>/dev/null || true
echo "    waiting for our FastbootLib to come back up (auto-boot from flashed EFI)..."
if device_monitor_wait_for_fastboot 90; then
  device_monitor_phoenix_stop
  POST_NAME="$(device_monitor_build_name)"
  if [[ -n "$POST_NAME" ]]; then
    echo "    back in our FastbootLib: $POST_NAME."
  else
    echo "    fastboot is up but not our FastbootLib (build-name missing). May be stock." >&2
  fi
else
  echo "error: fastboot did not come back within 90s — device may need manual recovery." >&2
  exit 1
fi

echo
echo "======================================================================"
echo "  done. logs in: $LOG_DIR"
echo "======================================================================"
