#!/usr/bin/env bash
# test-device-manual.sh — pull logs from a device that is ALREADY in
# recovery, after a manual gbl-chainload boot.
#
# Use case: you ran `fastboot boot dist/<branch>.efi` yourself, the
# payload chain-loaded into recovery (or you adb-rebooted into recovery
# after some other manual flow), and now you just want the captures
# without the full test-device.sh ceremony — no rebuild, no fastboot
# step, no key-window prompt.
#
# Usage:
#   ./scripts/test-device-manual.sh                # auto label, current dist/.efi
#   ./scripts/test-device-manual.sh sibling        # labels logs as sibling
#   ./scripts/test-device-manual.sh mode-debug v0.2-step2a
#
# Output:
#   logs/<timestamp>_manual_<label>[_v<version>]/
#     bootloader_log
#     bootconfig
#     cmdline
#     device-tree.tar
#     dmesg.txt
#     getprop.boot.txt
#     recovery.props
#     logfs/...

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:-manual}"
VERSION="${2:-}"
source "$REPO_ROOT/scripts/device-monitor.sh"

# If no explicit version, sniff the most recently built .efi for a slug.
if [[ -z "$VERSION" ]]; then
  EFI_CANDIDATE=""
  for f in "$REPO_ROOT/dist/${LABEL}.efi" "$REPO_ROOT/dist/gbl-chainload.efi"; do
    if [[ -f "$f" ]]; then EFI_CANDIDATE="$f"; break; fi
  done
  if [[ -n "$EFI_CANDIDATE" ]]; then
    VERSION=$(strings "$EFI_CANDIDATE" 2>/dev/null \
                | grep -E '^[0-9]+\.[0-9]+(-[0-9a-z]+)?$' \
                | head -1 || true)
  fi
  if [[ -z "$VERSION" ]]; then
    VERSION=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
  fi
fi

TS="$(date +%Y%m%d-%H%M%S)"
LOG_DIR="$REPO_ROOT/logs/${TS}_manual_${LABEL}_v${VERSION}"
mkdir -p "$LOG_DIR"
device_monitor_start "$LOG_DIR" "test-device-manual"
trap 'device_monitor_stop' EXIT INT TERM

echo "======================================================================"
echo "  test-device-manual.sh"
echo "  label    : $LABEL"
echo "  version  : $VERSION"
echo "  log dir  : $LOG_DIR"
echo "  monitor  : ${DEVICE_MONITOR_LOG:-n/a}"
echo "======================================================================"

# Confirm any adb state (device / recovery / sideload / rescue). The
# stock `adb wait-for-device` defaults to STATE=device and hangs forever
# when the target is in recovery — that's exactly the path we use here,
# so use the helper that accepts any non-empty adb state.
echo
echo ">>> [1/2] confirming adb (recovery / device)"
if ! ADB_STATE="$(device_monitor_wait_for_adb_state 180)"; then
  echo "error: no adb device within 180s. If device is mid-boot, wait and rerun." >&2
  exit 1
fi
echo "    adb up: state=$ADB_STATE"
adb shell 'getprop ro.bootloader; getprop ro.bootmode; \
           getprop ro.boot.slot_suffix; getprop ro.build.fingerprint' \
  | tee "$LOG_DIR/recovery.props"

# Step 2 ----------------------------------------------------------------
echo
echo ">>> [2/2] capturing logs into $LOG_DIR"

# oplus kernel module exposes the bootloader log + bootconfig via /proc.
adb pull /proc/bootloader_log "$LOG_DIR/bootloader_log" 2>/dev/null \
  || echo "    /proc/bootloader_log not present — skipping"
adb pull /proc/bootconfig     "$LOG_DIR/bootconfig"     2>/dev/null \
  || echo "    /proc/bootconfig not present — skipping"
adb pull /proc/cmdline        "$LOG_DIR/cmdline"        2>/dev/null \
  || echo "    /proc/cmdline not present — skipping"

# Snapshot kernel's flattened device tree view. Pulling the tree directly can
# be slow/noisy over adb because many properties are binary, so archive it on
# device first when possible.
adb shell 'if [ -d /proc/device-tree ]; then tar -C /proc -cf /tmp/device-tree.tar device-tree; elif [ -d /sys/firmware/devicetree/base ]; then tar -C /sys/firmware/devicetree -cf /tmp/device-tree.tar base; else exit 1; fi' \
  >/dev/null 2>&1 && \
  adb pull /tmp/device-tree.tar "$LOG_DIR/device-tree.tar" >/dev/null 2>&1 && \
  adb shell 'rm -f /tmp/device-tree.tar' >/dev/null 2>&1 \
  || echo "    device tree snapshot not present — skipping"

# Kernel ring buffer.
adb shell dmesg > "$LOG_DIR/dmesg.txt" 2>/dev/null \
  || echo "    dmesg failed — skipping"

# Bootloader-set Android props.
adb shell 'getprop | grep -E "^\[ro\.boot\.|^\[ro\.bootmode|^\[ro\.bootloader"' \
  > "$LOG_DIR/getprop.boot.txt" 2>/dev/null || true

# Mount + pull logfs partition (UefiLogN.txt + GblChainload_BootN.txt).
adb shell 'mkdir -p /logfs && mount -t vfat /dev/block/by-name/logfs /logfs' \
  2>/dev/null || echo "    logfs mount failed — already mounted? skipping mount"
mkdir -p "$LOG_DIR/logfs"
adb pull /logfs/. "$LOG_DIR/logfs/" 2>/dev/null \
  || echo "    logfs pull failed — partition unmapped?"
adb shell 'umount /logfs && rmdir /logfs' 2>/dev/null || true

echo
echo "======================================================================"
echo "  captured into $LOG_DIR:"
( cd "$LOG_DIR" && find . -maxdepth 2 -type f -printf '    %p  %s bytes\n' )
echo "======================================================================"
