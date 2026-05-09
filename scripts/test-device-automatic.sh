#!/usr/bin/env bash
# test-device-automatic.sh — hands-free iteration loop, sibling-EFISP-aware.
#
# Assumes sibling.efi is the FLASHED EFISP. Sibling's escape table makes
# our FastbootLib the default landing zone, so every reboot drops back
# into fastboot without a key press. This script:
#
#   1. boot-efi the payload (default: dist/mode-debug.efi) into RAM, or
#      --escape-recovery: reboot recovery, wait for AUTO_DEBUG fastboot, then
#      send `oem escape` so stock ABL observes the recovery reboot reason
#   2. wait for adb (mode-debug chain-loads → patched ABL → Linux/recovery)
#   3. pull every log surface (logfs, /proc/bootloader_log, dmesg, props)
#   4. reboot back into sibling's fastboot for the next iteration
#
# The "reboot back to fastboot" lap is what makes this an iteration loop:
# host scripts can run this in a `for` loop with different payloads, no
# physical key press anywhere in the cycle.
#
# Usage:
#   ./scripts/test-device-automatic.sh                    # mode-debug.efi
#   ./scripts/test-device-automatic.sh dist/mode-debug.efi
#   ./scripts/test-device-automatic.sh dist/main.efi
#   ./scripts/test-device-automatic.sh --no-return dist/gbl-chainload.efi
#   ./scripts/test-device-automatic.sh --escape-recovery
#
# Env:
#   GBL_TEST_RETURN_TO_FASTBOOT=0   stop after log capture instead of rebooting
#   GBL_TEST_ESCAPE_RECOVERY=1   use reboot-recovery + oem escape, no BCB
#   GBL_TEST_ESCAPE_DELAY=5      delay after AUTO_DEBUG fastboot reappears
#
# Output:
#   logs/<timestamp>_auto_<label>_v<version>/

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAYLOAD="$REPO_ROOT/dist/mode-debug.efi"
RETURN_TO_FASTBOOT="${GBL_TEST_RETURN_TO_FASTBOOT:-1}"
ESCAPE_RECOVERY="${GBL_TEST_ESCAPE_RECOVERY:-0}"
ESCAPE_WITH_PAYLOAD="${GBL_TEST_ESCAPE_WITH_PAYLOAD:-0}"
ESCAPE_DELAY="${GBL_TEST_ESCAPE_DELAY:-5}"
source "$REPO_ROOT/scripts/device-monitor.sh"

for arg in "$@"; do
  case "$arg" in
    --no-return)
      RETURN_TO_FASTBOOT=0
      ;;
    --return-to-fastboot)
      RETURN_TO_FASTBOOT=1
      ;;
    --escape-recovery)
      ESCAPE_RECOVERY=1
      ;;
    --escape-with-payload)
      ESCAPE_WITH_PAYLOAD=1
      ;;
    --bcb=*)
      echo "error: BCB flow is abandoned; use --escape-recovery instead" >&2
      exit 1
      ;;
    --*)
      echo "error: unknown option: $arg" >&2
      exit 1
      ;;
    *)
      PAYLOAD="$arg"
      ;;
  esac
done

if [[ "$ESCAPE_RECOVERY" == "1" && "$ESCAPE_WITH_PAYLOAD" == "1" ]]; then
  echo "error: --escape-recovery and --escape-with-payload are mutually exclusive" >&2
  exit 1
fi

if [[ "$ESCAPE_RECOVERY" != "1" && ! -f "$PAYLOAD" ]]; then
  echo "error: payload not found: $PAYLOAD" >&2
  exit 1
fi

if [[ "$ESCAPE_RECOVERY" == "1" ]]; then
  LABEL="escape-recovery"
  VERSION=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
else
  LABEL="$(basename "$PAYLOAD" .efi)"
  VERSION=$(strings "$PAYLOAD" 2>/dev/null \
              | grep -E '^[0-9]+\.[0-9]+(-[0-9a-z]+)?$' \
              | head -1 || true)
fi
if [[ -z "$VERSION" ]]; then
  VERSION=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
fi

TS="$(date +%Y%m%d-%H%M%S)"
LOG_DIR="$REPO_ROOT/logs/${TS}_auto_${LABEL}_v${VERSION}"
mkdir -p "$LOG_DIR"
device_monitor_start "$LOG_DIR" "test-device-automatic"
trap 'device_monitor_stop' EXIT INT TERM

capture_fastboot_fallback() {
  local reason="${1:-unknown}"
  local out="$LOG_DIR/fastboot-fallback.txt"

  {
    printf 'reason: %s\n' "$reason"
    printf 'time: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')"
    printf '\n[fastboot devices]\n'
    device_monitor_fastboot devices 2>&1 || true
    printf '\n[fastboot oem efi-status]\n'
    device_monitor_fastboot oem efi-status 2>&1 || true
  } > "$out"

  echo "    fastboot fallback captured: $out" >&2
}

wait_for_adb_or_fastboot_fallback() {
  local timeout_s="${1:-360}"
  local deadline=$(( SECONDS + timeout_s ))
  local state=""

  while (( SECONDS < deadline )); do
    state="$(device_monitor_adb_state | head -n 1)"
    if [[ -n "$state" ]]; then
      printf 'adb:%s\n' "$state"
      return 0
    fi

    if device_monitor_fastboot devices 2>/dev/null | grep -q fastboot; then
      printf 'fastboot\n'
      return 2
    fi

    sleep 2
  done

  return 1
}

echo "======================================================================"
echo "  test-device-automatic.sh"
if [[ "$ESCAPE_RECOVERY" == "1" ]]; then
  echo "  payload  : (none; recovery escape flow)"
else
  echo "  payload  : $PAYLOAD ($(stat -c%s "$PAYLOAD") bytes)"
fi
echo "  label    : $LABEL"
echo "  version  : $VERSION"
echo "  log dir  : $LOG_DIR"
echo "  monitor  : ${DEVICE_MONITOR_LOG:-n/a}"
echo "  escape   : $ESCAPE_RECOVERY (with-payload=$ESCAPE_WITH_PAYLOAD)"
echo "  return   : $RETURN_TO_FASTBOOT"
echo "======================================================================"

# Step 1 — confirm fastboot ----------------------------------------------
echo
echo ">>> [1/5] confirming fastboot device"
device_monitor_fastboot devices
if ! device_monitor_fastboot devices | grep -q fastboot; then
  echo "error: no fastboot device detected. Sibling EFISP should land us in" >&2
  echo "       fastboot by default; if it doesn't, the device may be elsewhere." >&2
  exit 1
fi

# Step 2 — stage + oem boot-efi the payload, or escape to recovery --------
# `fastboot boot` would treat the EFI as an Android boot.img and route
# through AVB, which rejects unsigned EFIs. Our FastbootLib instead
# implements `oem boot-efi` (edk2/.../FastbootCmds.c:4228) which
# LoadImage+StartImage's the staged buffer directly. `fastboot stage`
# is the wire-equivalent of `download:` — drops bytes into mUsbDataBuffer
# without invoking the boot-image path. Pair stage + oem boot-efi.
echo
if [[ "$ESCAPE_RECOVERY" == "1" ]]; then
  echo ">>> [2/5] reboot recovery + wait for AUTO_DEBUG fastboot + oem escape"
  device_monitor_fastboot reboot recovery 2>/dev/null || true
  echo "    waiting for AUTO_DEBUG fastboot after reboot recovery..."
  if ! device_monitor_wait_for_fastboot 120; then
    echo "error: AUTO_DEBUG fastboot did not appear after reboot recovery" >&2
    exit 1
  fi
  echo "    waiting ${ESCAPE_DELAY}s for AUTO_DEBUG key window to settle..."
  sleep "$ESCAPE_DELAY"
  ESCAPE_LOG="$LOG_DIR/fastboot-oem-escape.txt"
  device_monitor_fastboot oem escape 2>&1 \
    | tee "$ESCAPE_LOG" \
    | grep -v "Status read failed" \
    || true
  if grep -q "Status read failed" "$ESCAPE_LOG"; then
    echo "    (USB drop on escape handoff — expected)"
  elif grep -q FAILED "$ESCAPE_LOG"; then
    echo "error: oem escape failed. See above." >&2
    exit 1
  fi
elif [[ "$ESCAPE_WITH_PAYLOAD" == "1" ]]; then
  echo ">>> [2/5] reboot recovery + stage + oem boot-efi + oem escape  ($PAYLOAD)"
  # Combined flow for mode-debug payloads that mirror auto-debug routing
  # (default → enter FastbootLib, await `oem escape`):
  #   1) `fastboot reboot recovery` → BCB now has boot-recovery, sibling
  #      AUTO_DEBUG comes back up holding the cookie.
  #   2) stage + oem boot-efi PAYLOAD → mode-debug runs and lands in its
  #      own FastbootLib (hooks not yet installed).
  #   3) oem escape → CmdOemEscape → BootFlowChainLoad installs hooks +
  #      LoadImage(patched ABL); patched ABL reads BCB → recovery.
  device_monitor_fastboot reboot recovery 2>/dev/null || true
  echo "    waiting for sibling AUTO_DEBUG fastboot after reboot recovery..."
  if ! device_monitor_wait_for_fastboot 120; then
    echo "error: AUTO_DEBUG fastboot did not reappear after reboot recovery" >&2
    exit 1
  fi
  echo "    waiting ${ESCAPE_DELAY}s for AUTO_DEBUG key window to settle..."
  sleep "$ESCAPE_DELAY"

  echo "    fastboot stage $PAYLOAD"
  device_monitor_fastboot stage "$PAYLOAD"

  echo "    fastboot oem boot-efi (run staged payload as mode-debug)"
  BOOTEFI_LOG="$LOG_DIR/fastboot-oem-boot-efi.txt"
  device_monitor_fastboot oem boot-efi 2>&1 \
    | tee "$BOOTEFI_LOG" \
    | grep -v "Status read failed" \
    || true
  if grep -q "Status read failed" "$BOOTEFI_LOG"; then
    echo "    (USB drop on StartImage — expected handoff into mode-debug)"
  elif grep -q FAILED "$BOOTEFI_LOG"; then
    echo "error: oem boot-efi failed for non-handoff reason. See above." >&2
    exit 1
  fi

  echo "    waiting for mode-debug FastbootLib to appear (key window + init)..."
  if ! device_monitor_wait_for_fastboot 60; then
    echo "error: mode-debug FastbootLib did not appear after oem boot-efi" >&2
    exit 1
  fi
  sleep "$ESCAPE_DELAY"

  echo "    fastboot oem escape (BootFlowChainLoad → patched ABL → recovery)"
  ESCAPE_LOG="$LOG_DIR/fastboot-oem-escape.txt"
  device_monitor_fastboot oem escape 2>&1 \
    | tee "$ESCAPE_LOG" \
    | grep -v "Status read failed" \
    || true
  if grep -q "Status read failed" "$ESCAPE_LOG"; then
    echo "    (USB drop on escape handoff — expected)"
  elif grep -q FAILED "$ESCAPE_LOG"; then
    echo "error: oem escape failed. See above." >&2
    exit 1
  fi
else
  echo ">>> [2/5] fastboot stage + oem boot-efi  ($PAYLOAD)"
  device_monitor_fastboot stage "$PAYLOAD"
# `oem boot-efi` succeeds by NOT returning — StartImage transfers control
# to the staged payload, the USB endpoint drops, and the fastboot client
# emits "Status read failed (No such device)" with a non-zero exit. That's
# the success signature for a clean handoff. Suppress the failure so the
# script keeps going.
  BOOTEFI_LOG="$LOG_DIR/fastboot-oem-boot-efi.txt"
  device_monitor_fastboot oem boot-efi 2>&1 \
    | tee "$BOOTEFI_LOG" \
    | grep -v "Status read failed" \
    || true
  if grep -q "Status read failed" "$BOOTEFI_LOG"; then
    echo "    (USB drop on StartImage — expected handoff)"
  elif grep -q FAILED "$BOOTEFI_LOG"; then
    echo "error: oem boot-efi failed for non-handoff reason. See above." >&2
    exit 1
  fi
fi

# Step 3 — wait for adb --------------------------------------------------
# mode-debug's BootFlowChainLoad → patched ABL. For --escape-recovery, the
# `fastboot reboot recovery` reason is preserved until `oem escape` starts ABL.
# Either way, adb comes up. Cap the wait so a hang doesn't block forever.
echo
echo ">>> [3/5] waiting for adb (Linux or recovery)..."
# `adb wait-for-device` defaults to STATE=device on some platform-tools
# combinations and can miss/break on recovery. Poll `adb devices` instead
# and accept explicit states: device / recovery / rescue / sideload.
#
# 360s budget: chain-load + ABL handoff + AVB walk + kernel boot + adb
# authorization. Recovery boot can take 4-5 minutes on this device. If a
# regression reboots/powers back into sibling FastbootLib instead, detect and
# log that state immediately instead of waiting for manual interpretation.
set +e
WAIT_RESULT="$(wait_for_adb_or_fastboot_fallback 360)"
WAIT_RC=$?
set -e
case "$WAIT_RC" in
  0)
    ADB_STATE="${WAIT_RESULT#adb:}"
    echo "    adb up: state=$ADB_STATE"
    ;;
  2)
    capture_fastboot_fallback "adb-timeout-hit-fastboot-after-chainload"
    echo "error: expected adb/recovery, but device is back in fastboot." >&2
    echo "       This usually means the payload failed to boot Linux/recovery and" >&2
    echo "       reset/powered back into sibling FastbootLib." >&2
    exit 2
    ;;
  *)
    echo "error: adb did not come up within 360s. Device may have hung," >&2
    echo "       powered off, or — under WSL — usbipd lost the" >&2
    echo "       passthrough across the fastboot→recovery re-enumeration." >&2
    exit 1
    ;;
esac
sleep 2

# Capture which mode we ended up in
adb shell 'getprop ro.bootloader; getprop ro.bootmode; \
           getprop ro.boot.slot_suffix; getprop ro.build.fingerprint' \
  | tee "$LOG_DIR/recovery.props" 2>/dev/null \
  || echo "(getprop failed)" > "$LOG_DIR/recovery.props"

# Step 4 — pull logs -----------------------------------------------------
echo
echo ">>> [4/5] capturing logs into $LOG_DIR"

adb pull /proc/bootloader_log "$LOG_DIR/bootloader_log" 2>/dev/null \
  || echo "    /proc/bootloader_log not present — skipping"
adb pull /proc/bootconfig     "$LOG_DIR/bootconfig"     2>/dev/null \
  || echo "    /proc/bootconfig not present — skipping"
adb pull /proc/cmdline        "$LOG_DIR/cmdline"        2>/dev/null \
  || echo "    /proc/cmdline not present — skipping"

adb shell dmesg > "$LOG_DIR/dmesg.txt" 2>/dev/null \
  || echo "    dmesg failed — skipping"

adb shell 'getprop | grep -E "^\[ro\.boot\.|^\[ro\.bootmode|^\[ro\.bootloader"' \
  > "$LOG_DIR/getprop.boot.txt" 2>/dev/null || true

# Mount + pull logfs (UefiLogN.txt + GblChainload_BootN.txt). Best-effort:
# logfs may already be mounted somewhere, or the device may not have
# vfat tools available in recovery. If mount fails, try via raw block.
adb shell 'mkdir -p /logfs && mount -t vfat /dev/block/by-name/logfs /logfs' \
  2>/dev/null \
  || echo "    logfs mount may have failed (already mounted? no vfat?) — pulling anyway"
mkdir -p "$LOG_DIR/logfs"
adb pull /logfs/. "$LOG_DIR/logfs/" 2>/dev/null \
  || echo "    logfs pull failed — partition unmapped or empty"
adb shell 'umount /logfs && rmdir /logfs' 2>/dev/null || true

# Step 5 — return to fastboot for next iteration ------------------------
echo
if [[ "$RETURN_TO_FASTBOOT" == "0" ]]; then
  echo ">>> [5/5] leaving device in current adb state (return disabled)"
  exit 0
fi

echo ">>> [5/5] rebooting back to sibling's fastboot for next iteration"
# `adb reboot bootloader` → allows us to escape watchdog. we can escape
adb reboot bootloader 2>/dev/null || true
echo "    waiting for sibling's fastboot to come back up..."
# Some fastboot clients don't support `fastboot wait-for-device`; poll
# `fastboot devices` instead. Sibling boots fast (~5s) but allow up to 90s.
if device_monitor_wait_for_fastboot 90; then
  echo "    sibling fastboot is back."
else
  echo "    fastboot didn't come back within 90s — device may need manual recovery" >&2
fi

echo
echo "======================================================================"
echo "  captured into $LOG_DIR:"
( cd "$LOG_DIR" && find . -maxdepth 2 -type f -printf '    %p  %s bytes\n' )
echo "======================================================================"
