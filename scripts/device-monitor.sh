#!/usr/bin/env bash

device_monitor_start() {
  local log_dir="${1:-}"
  local label="${2:-device}"
  local interval="${DEVICE_MONITOR_INTERVAL:-5}"
  local stamp file

  if [[ -n "$log_dir" && -d "$log_dir" && -w "$log_dir" ]]; then
    stamp="$(date +%Y%m%d-%H%M%S)"
    file="$log_dir/${stamp}_device-monitor.log"
  else
    stamp="$(date +%Y%m%d-%H%M%S)"
    file="${TMPDIR:-/tmp}/${label}-${stamp}.device-monitor.log"
  fi

  : > "$file"
  DEVICE_MONITOR_FASTBOOT_LOCK="${file}.fastboot.lock"

  (
    while :; do
      ts="$(date '+%Y-%m-%d %H:%M:%S')"
      state=""
      fb=""

      if command -v adb >/dev/null 2>&1; then
        state="$(adb get-state 2>/dev/null || true)"
      else
        state="missing"
      fi

      if command -v fastboot >/dev/null 2>&1; then
        if mkdir "$DEVICE_MONITOR_FASTBOOT_LOCK" 2>/dev/null; then
          fb="$(fastboot devices 2>/dev/null || true)"
          rmdir "$DEVICE_MONITOR_FASTBOOT_LOCK" 2>/dev/null || true
          fb="${fb//$'\n'/ }"
          while [[ "$fb" == *' ' ]]; do fb="${fb% }"; done
        else
          fb="busy"
        fi
      else
        fb="missing"
      fi

      printf '%s adb=%s fastboot=%s\n' "$ts" "${state:-unknown}" "${fb:-unknown}" >> "$file"
      sleep "$interval"
    done
  ) &

  DEVICE_MONITOR_PID=$!
  DEVICE_MONITOR_LOG="$file"
}

device_monitor_fastboot() {
  if [[ -z "${DEVICE_MONITOR_FASTBOOT_LOCK:-}" ]]; then
    fastboot "$@"
    return $?
  fi

  while ! mkdir "$DEVICE_MONITOR_FASTBOOT_LOCK" 2>/dev/null; do
    sleep 0.2
  done
  fastboot "$@"
  local rc=$?
  rmdir "$DEVICE_MONITOR_FASTBOOT_LOCK" 2>/dev/null || true
  return "$rc"
}

device_monitor_adb_state() {
  local line serial state

  adb devices 2>/dev/null | while read -r serial state _; do
    case "$serial" in
      ""|List) continue ;;
    esac
    case "$state" in
      device|recovery|sideload|rescue)
        printf '%s\n' "$state"
        return 0
        ;;
    esac
  done
}

device_monitor_wait_for_adb_state() {
  local timeout_s="${1:-360}"
  local deadline=$(( SECONDS + timeout_s ))
  local state

  while (( SECONDS < deadline )); do
    state="$(device_monitor_adb_state | head -n 1)"
    if [[ -n "$state" ]]; then
      printf '%s\n' "$state"
      return 0
    fi
    sleep 2
  done

  return 1
}

device_monitor_wait_for_fastboot() {
  local timeout_s="${1:-90}"
  local deadline=$(( SECONDS + timeout_s ))

  while (( SECONDS < deadline )); do
    if device_monitor_fastboot devices 2>/dev/null | grep -q fastboot; then
      return 0
    fi
    sleep 2
  done

  return 1
}

device_monitor_stop() {
  if [[ -n "${DEVICE_MONITOR_PID:-}" ]]; then
    kill "$DEVICE_MONITOR_PID" 2>/dev/null || true
    wait "$DEVICE_MONITOR_PID" 2>/dev/null || true
    unset DEVICE_MONITOR_PID
  fi
  if [[ -n "${DEVICE_MONITOR_FASTBOOT_LOCK:-}" ]]; then
    rmdir "$DEVICE_MONITOR_FASTBOOT_LOCK" 2>/dev/null || true
  fi
}

# Phoenix watchdog timer.
#
# OnePlus's bootloader runs a 60s watchdog from "device entered fastboot
# mode". If our test cycle takes too long between entering fastboot and
# rebooting back to bootloader, Phoenix drops the device to stock fastboot
# and our in-flight EFI session is gone.
#
# Usage:
#   device_monitor_phoenix_start          # snapshot start time
#   device_monitor_phoenix_check          # WARN at 45s, ABORT at 55s
#
# Variables (script-globals, no subshell):
#   _PHOENIX_T0   epoch seconds when started, empty = inactive
#   _PHOENIX_WARN seconds elapsed at which to warn
#   _PHOENIX_KILL seconds elapsed at which to fatal
_PHOENIX_T0=""
_PHOENIX_WARN=45
_PHOENIX_KILL=55

device_monitor_phoenix_start () {
  _PHOENIX_T0=$(date +%s)
}

device_monitor_phoenix_elapsed () {
  [[ -z "$_PHOENIX_T0" ]] && { echo 0; return; }
  echo $(( $(date +%s) - _PHOENIX_T0 ))
}

device_monitor_phoenix_check () {
  local elapsed
  elapsed=$(device_monitor_phoenix_elapsed)
  if (( elapsed >= _PHOENIX_KILL )); then
    echo "    FATAL: ${elapsed}s elapsed since fastboot entry — Phoenix watchdog will fire imminently. Aborting." >&2
    return 1
  fi
  if (( elapsed >= _PHOENIX_WARN )); then
    echo "    WARN: ${elapsed}s elapsed since fastboot entry — Phoenix 60s watchdog approaching." >&2
  fi
  return 0
}

device_monitor_phoenix_stop () {
  _PHOENIX_T0=""
}

# Quick state probes — return 0 if device is in the named state within 3s.

# Is the device responding in fastboot right now?
device_monitor_in_fastboot_quick () {
  local out
  out="$(timeout 3 fastboot devices 2>/dev/null | grep -c 'fastboot')"
  [[ "$out" -ge 1 ]]
}

# Is adb up with a normal device state right now?
device_monitor_in_adb_quick () {
  local out
  out="$(timeout 3 adb get-state 2>/dev/null)"
  [[ -n "$out" && "$out" != "unknown" ]]
}

# Did the device drop to stock fastboot? Detects by product string mismatch.
# Stock OnePlus fastboot returns a specific product (e.g. "infiniti" or
# "canoe"); our patched FastbootLib returns a different one. Caller
# specifies the expected product substring; mismatch => stock.
device_monitor_dropped_to_stock () {
  local expected="$1"
  local product
  product="$(timeout 3 fastboot getvar product 2>&1 | grep -i product | head -1 || true)"
  if [[ -z "$product" ]]; then
    return 1  # can't tell
  fi
  if echo "$product" | grep -qi "$expected"; then
    return 1  # matched expected, not stock
  fi
  return 0  # mismatch = dropped to stock
}

# Is the current fastboot device our gbl-chainload FastbootLib?
#
# Signature: our FastbootLib publishes `boot-mode` as a getvar — values are
# `gbl-mode-0`, `gbl-mode-1`, `gbl-mode-2`, `gbl-mode-3`, `gbl-mode-unknown`,
# or `gbl-mode-undef`. Stock fastboot returns FAILED/unknown for unknown vars.
device_monitor_is_our_fastbootlib () {
  local out
  out="$(timeout 3 fastboot getvar boot-mode 2>&1 || true)"
  echo "$out" | grep -qi "gbl-mode-" && return 0
  return 1
}

# Print the gbl-mode string (e.g. "gbl-mode-1") if device is in our FastbootLib,
# empty string otherwise.
device_monitor_gbl_mode () {
  local out
  out="$(timeout 3 fastboot getvar boot-mode 2>&1 || true)"
  echo "$out" | grep -oE 'gbl-mode-[a-z0-9-]+' | head -1
}
