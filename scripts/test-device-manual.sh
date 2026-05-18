#!/usr/bin/env bash
# test-device-manual.sh — pull logs from a device that is ALREADY booted,
# either in recovery (TWRP) or in the Android system.
#
# Context detection: the script probes ro.bootmode after adb comes up and
# sets DEVICE_CONTEXT=recovery or DEVICE_CONTEXT=system automatically.
# In system, root commands are prefixed with 'su -c' when su is available
# (e.g., Magisk/KernelSU with shell rule already granted). If su is not
# available, root-required pulls (bootloader_log, dmesg, logfs partition)
# will print a WARN and be skipped — unprivileged reads still succeed.
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

# ---------------------------------------------------------------------------
# Detect whether device is in system or recovery context.
# In recovery, getprop ro.bootmode returns "recovery" or "factory_recovery";
# in system it returns the normal bootmode (e.g., "normal").
DEVICE_BOOTMODE="$(adb shell getprop ro.bootmode 2>/dev/null | tr -d '\r')"
DEVICE_BUILD_TYPE="$(adb shell getprop ro.build.type 2>/dev/null | tr -d '\r')"
case "$DEVICE_BOOTMODE" in
  recovery|factory_recovery|*recovery*)
    DEVICE_CONTEXT="recovery"
    ;;
  *)
    DEVICE_CONTEXT="system"
    ;;
esac

SU_PREFIX=""
if [[ "$DEVICE_CONTEXT" == "system" ]]; then
  # Probe if su works.
  if adb shell 'su -c id' 2>/dev/null | grep -q 'uid=0'; then
    SU_PREFIX='su -c '
    echo "    su available: root pulls will use 'adb shell su -c'"
  else
    SU_PREFIX=""
    echo "    WARN: su not available — root-required pulls (logfs partition, dmesg, /proc/bootloader_log) may fail"
  fi
fi
echo "    device context: $DEVICE_CONTEXT"
echo "    bootmode: $DEVICE_BOOTMODE"
echo "    build type: $DEVICE_BUILD_TYPE"

# ---------------------------------------------------------------------------
# Step 2 ----------------------------------------------------------------
echo
echo ">>> [2/2] capturing logs into $LOG_DIR"

# oplus kernel module exposes the bootloader log + bootconfig via /proc.
# /proc/bootloader_log needs root in system (kernel.dmesg_restrict neighbours).
if [[ "$DEVICE_CONTEXT" == "recovery" ]]; then
  adb pull /proc/bootloader_log "$LOG_DIR/bootloader_log" 2>/dev/null \
    || echo "    /proc/bootloader_log not present — skipping"
else
  adb shell ${SU_PREFIX}'cat /proc/bootloader_log' > "$LOG_DIR/bootloader_log" 2>/dev/null || true
  if [[ -s "$LOG_DIR/bootloader_log" ]]; then
    tr -d '\r' < "$LOG_DIR/bootloader_log" > "$LOG_DIR/bootloader_log.tmp" \
      && mv "$LOG_DIR/bootloader_log.tmp" "$LOG_DIR/bootloader_log"
  else
    rm -f "$LOG_DIR/bootloader_log"
    echo "    /proc/bootloader_log not present or root required — skipping"
  fi
fi

# /proc/bootconfig — try unprivileged pull first; fall back via su if needed.
if [[ "$DEVICE_CONTEXT" == "recovery" ]]; then
  adb pull /proc/bootconfig "$LOG_DIR/bootconfig" 2>/dev/null \
    || echo "    /proc/bootconfig not present — skipping"
else
  if ! adb pull /proc/bootconfig "$LOG_DIR/bootconfig" 2>/dev/null; then
    adb shell ${SU_PREFIX}'cat /proc/bootconfig' > "$LOG_DIR/bootconfig" 2>/dev/null || true
    if [[ -s "$LOG_DIR/bootconfig" ]]; then
      tr -d '\r' < "$LOG_DIR/bootconfig" > "$LOG_DIR/bootconfig.tmp" \
        && mv "$LOG_DIR/bootconfig.tmp" "$LOG_DIR/bootconfig"
    else
      rm -f "$LOG_DIR/bootconfig"
      echo "    /proc/bootconfig not present — skipping"
    fi
  fi
fi

# /proc/cmdline — try unprivileged pull first; user-build system blocks this so
# fall back via su, mirroring the /proc/bootconfig logic above.
if [[ "$DEVICE_CONTEXT" == "recovery" ]]; then
  adb pull /proc/cmdline "$LOG_DIR/cmdline" 2>/dev/null \
    || echo "    /proc/cmdline not present — skipping"
else
  if ! adb pull /proc/cmdline "$LOG_DIR/cmdline" 2>/dev/null; then
    adb shell ${SU_PREFIX}'cat /proc/cmdline' > "$LOG_DIR/cmdline" 2>/dev/null || true
    if [[ -s "$LOG_DIR/cmdline" ]]; then
      tr -d '\r' < "$LOG_DIR/cmdline" > "$LOG_DIR/cmdline.tmp" \
        && mv "$LOG_DIR/cmdline.tmp" "$LOG_DIR/cmdline"
    else
      rm -f "$LOG_DIR/cmdline"
      echo "    /proc/cmdline not present — skipping"
    fi
  fi
fi

# Snapshot kernel's flattened device tree view. Pulling the tree directly can
# be slow/noisy over adb because many properties are binary, so archive it on
# device first when possible.
adb shell 'if [ -d /proc/device-tree ]; then tar -C /proc -cf /tmp/device-tree.tar device-tree; elif [ -d /sys/firmware/devicetree/base ]; then tar -C /sys/firmware/devicetree -cf /tmp/device-tree.tar base; else exit 1; fi' \
  >/dev/null 2>&1 && \
  adb pull /tmp/device-tree.tar "$LOG_DIR/device-tree.tar" >/dev/null 2>&1 && \
  adb shell 'rm -f /tmp/device-tree.tar' >/dev/null 2>&1 \
  || echo "    device tree snapshot not present — skipping"

# Kernel ring buffer. dmesg is restricted in system (kernel.dmesg_restrict=1).
if [[ "$DEVICE_CONTEXT" == "recovery" ]]; then
  adb shell dmesg > "$LOG_DIR/dmesg.txt" 2>/dev/null \
    || echo "    dmesg failed — skipping"
else
  adb shell ${SU_PREFIX}dmesg > "$LOG_DIR/dmesg.txt" 2>/dev/null || true
  if [[ -s "$LOG_DIR/dmesg.txt" ]]; then
    tr -d '\r' < "$LOG_DIR/dmesg.txt" > "$LOG_DIR/dmesg.txt.tmp" \
      && mv "$LOG_DIR/dmesg.txt.tmp" "$LOG_DIR/dmesg.txt"
  else
    rm -f "$LOG_DIR/dmesg.txt"
    echo "    dmesg failed (restricted?) — skipping"
  fi
fi

# Bootloader-set Android props.
adb shell 'getprop | grep -E "^\[ro\.boot\.|^\[ro\.bootmode|^\[ro\.bootloader"' \
  > "$LOG_DIR/getprop.boot.txt" 2>/dev/null || true

# Mount + pull logfs partition (UefiLogN.txt plus rotated UefiLogSavedN.txt).
# Mounting requires root in system.
LOGFS_MOUNTED=false
if [[ "$DEVICE_CONTEXT" == "recovery" ]]; then
  if adb shell 'mkdir -p /logfs && mount -t vfat /dev/block/by-name/logfs /logfs' \
      2>/dev/null; then
    LOGFS_MOUNTED=true
  else
    echo "    logfs mount failed — already mounted? skipping mount"
  fi
else
  if [[ -n "$SU_PREFIX" ]]; then
    # Android system mounts / read-only, so /logfs can't be created. Use a
    # writable path under /data/local/tmp. The compound command must run
    # entirely under su, so both halves of the && chain go inside one
    # single-quoted su -c argument (outer double quotes for variable expansion).
    LOGFS_MOUNTPOINT=/data/local/tmp/logfs
    LOGFS_MOUNT_LOG="$LOG_DIR/logfs-mount.stderr"
    adb shell "su -c 'mkdir -p $LOGFS_MOUNTPOINT && mount -t vfat /dev/block/by-name/logfs $LOGFS_MOUNTPOINT'" \
        >"$LOGFS_MOUNT_LOG" 2>&1 || true
    if adb shell ${SU_PREFIX}"mountpoint -q $LOGFS_MOUNTPOINT" >/dev/null 2>&1; then
      LOGFS_MOUNTED=true
      rm -f "$LOGFS_MOUNT_LOG"
    else
      echo "    WARN: logfs mount failed — see logfs-mount.stderr (first lines below)"
      head -3 "$LOGFS_MOUNT_LOG" | sed 's/^/      /'
    fi
  else
    echo "    WARN: su not available — skipping logfs mount and pull"
  fi
fi

mkdir -p "$LOG_DIR/logfs"
if [[ "$LOGFS_MOUNTED" == "true" ]]; then
  if [[ "$DEVICE_CONTEXT" == "recovery" ]]; then
    adb pull /logfs/. "$LOG_DIR/logfs/" 2>/dev/null \
      || echo "    logfs pull failed — partition unmapped?"
  else
    # System: adb pull on $LOGFS_MOUNTPOINT requires root ADB; pipe individual files via su.
    for logfs_file in UefiLog1.txt UefiLog2.txt UefiLogSaved1.txt UefiLogSaved2.txt; do
      adb shell "su -c 'cat $LOGFS_MOUNTPOINT/${logfs_file}'" \
        > "$LOG_DIR/logfs/${logfs_file}" 2>/dev/null || true
      if [[ -s "$LOG_DIR/logfs/${logfs_file}" ]]; then
        tr -d '\r' < "$LOG_DIR/logfs/${logfs_file}" > "$LOG_DIR/logfs/${logfs_file}.tmp" \
          && mv "$LOG_DIR/logfs/${logfs_file}.tmp" "$LOG_DIR/logfs/${logfs_file}"
      else
        rm -f "$LOG_DIR/logfs/${logfs_file}"
      fi
    done
  fi
  if [[ "$DEVICE_CONTEXT" == "recovery" ]]; then
    adb shell 'umount /logfs && rmdir /logfs' 2>/dev/null || true
  else
    adb shell "su -c 'umount $LOGFS_MOUNTPOINT && rmdir $LOGFS_MOUNTPOINT'" 2>/dev/null || true
  fi
fi

if [[ "$DEVICE_CONTEXT" == "recovery" ]]; then
    # In recovery, also pull the TWRP log if it exists and is non-empty.
    adb pull /tmp/recovery.log "$LOG_DIR/recovery.log" 2>/dev/null || true
    if [[ -s "$LOG_DIR/recovery.log" ]]; then
        tr -d '\r' < "$LOG_DIR/twrp-recovery.log" > "$LOG_DIR/recovery.log.tmp" \
        && mv "$LOG_DIR/recovery.log.tmp" "$LOG_DIR/recovery.log"
    else
        rm -f "$LOG_DIR/recovery.log"
    fi
    fi

echo
echo "======================================================================"
echo "  captured into $LOG_DIR:"
( cd "$LOG_DIR" && find . -maxdepth 2 -type f -printf '    %p  %s bytes\n' )
echo "======================================================================"
