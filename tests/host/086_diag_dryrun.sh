#!/usr/bin/env bash
# tests/host/086_diag_dryrun.sh — drive zip/modes/diag.sh against a
# synthetic environment in each EFISP state (valid / corrupt-GBLP1 / not-a-PE).
set -euo pipefail
cd "$(dirname "$0")/../.."

# 060 + zip/base/gbl-chainload.efi feed the diag_fake_byname helper. 060 SKIPs
# when the upstream PE fixture is absent (gitignored — e.g. on CI), so
# propagate the SKIP rather than ERRORing downstream.
if [ ! -f tests/host/.last/060/payload.bin ]; then
  bash tests/host/060_pack_roundtrip.sh
  [ -f tests/host/.last/060/payload.bin ] || {
    echo "SKIP: 086 — upstream payload.bin absent (PE fixture missing)"; exit 0;
  }
fi
[ -f zip/base/gbl-chainload.efi ] || {
  echo "SKIP: 086 — zip/base/gbl-chainload.efi absent"; exit 0;
}

# Build the multicall binary. PR2 Task 9 rewrote zip/modes/diag.sh to
# call `gbl <sub>` directly (no shims needed); we stage `gbl` itself
# onto PATH for this dry-run.
cargo build --release --quiet -p gbl

OUT=tests/host/.last/086
rm -rf "$OUT"; mkdir -p "$OUT"

# Stage the gbl multicall on a per-test PATH dir. diag.sh calls
# `gbl inspect / gbl unwrap / gbl avb {list,list-hash}` directly
# post Task 9.
SHIM_DIR="$OUT/bin"
mkdir -p "$SHIM_DIR"
cp "$PWD/target/release/gbl" "$SHIM_DIR/gbl"

run_one() {
  local scenario="$1" expect="$2"
  local envdir="$OUT/$scenario"

  bash tests/host/helpers/diag_fake_byname.sh "$envdir" "$scenario"

  # Fake recovery environment variables.
  export WORKDIR="$envdir/zip"
  export BYNAME="$envdir/byname"
  export BUNDLE_ROOT="$envdir/sdcard"      # tar.gz lands here (= /sdcard on device)
  export BUNDLE_WORKDIR="$envdir/work"     # staging dir (= /tmp on device)
  mkdir -p "$BUNDLE_WORKDIR"
  export SLOT=a
  export INACTIVE=b
  export OTA_POSTINSTALL=false

  # Open OUTFD=9 for screen output (recovery I/O stand-in).
  exec 9>"$envdir/screen.txt"
  export OUTFD=9

  # PATH so diag.sh can call `gbl` by bare name (the staged binary
  # lives in $SHIM_DIR; legacy per-tool shims are gone after PR2 Task 9).
  export PATH="$SHIM_DIR:$PATH"

  # Source diag.sh and call mode_main in a sub-shell that carries all
  # exported env.  We define stub functions for the recovery core
  # helpers that diag.sh relies on.
  local rc=0
  bash -c '
    # Core stubs.
    byname() { [ -e "$BYNAME/$1" ] && echo "$BYNAME/$1" || true; }
    abort()   { echo "ABORT: $*" >&2; exit 1; }
    export BOOTMODE=false   # always "recovery" in tests

    . "$WORKDIR/modes/diag.sh"
    mode_main
  ' > "$envdir/stdout.txt" 2>&1 || rc=$?

  exec 9>&-

  # Assert the sub-shell exited cleanly.
  [ "$rc" = 0 ] || {
    echo "FAIL [$scenario]: diag exited rc=$rc"
    cat "$envdir/stdout.txt"
    return 1
  }

  # Working dir was at $BUNDLE_WORKDIR/gbl-chainload-diag-<ts>; diag.sh
  # removes it after a successful tar. The persistent artifact is the
  # tar.gz on $BUNDLE_ROOT — that's our source of truth for assertions.
  local tgz
  tgz=$(ls "$BUNDLE_ROOT"/gbl-chainload-diag-*.tar.gz 2>/dev/null | head -1 || true)
  if [ -z "$tgz" ] || [ ! -f "$tgz" ]; then
    echo "FAIL [$scenario]: no tar.gz produced at $BUNDLE_ROOT/"
    cat "$envdir/stdout.txt"
    return 1
  fi

  # Verify the working dir was cleaned up — leaving it behind would
  # waste tmpfs/sdcard on a real device.
  if ls -d "$BUNDLE_WORKDIR"/gbl-chainload-diag-* >/dev/null 2>&1; then
    echo "FAIL [$scenario]: working dir not removed after tar:"
    ls -d "$BUNDLE_WORKDIR"/gbl-chainload-diag-*
    return 1
  fi

  # Extract the tarball into a scratch dir and verify its contents.
  local extract="$envdir/extracted"
  rm -rf "$extract"; mkdir -p "$extract"
  if ! tar -xzf "$tgz" -C "$extract" 2>/dev/null; then
    echo "FAIL [$scenario]: tar -xzf failed on $tgz"
    return 1
  fi
  local bundle
  bundle=$(ls -d "$extract"/gbl-chainload-diag-* 2>/dev/null | head -1 || true)
  if [ -z "$bundle" ]; then
    echo "FAIL [$scenario]: tarball didn't unpack to a gbl-chainload-diag-* dir"
    tar -tzf "$tgz" | head -5
    return 1
  fi

  # Assert every required file is present in the unpacked bundle.
  local missing=0
  for f in report.txt env.txt getprop.boot.txt efisp.img abl_a.img abl_b.img \
            vbmeta_a.img vbmeta_b.img logfs.img \
            gblp1-inspect.txt loader-abl.txt vbmeta-descriptors.txt \
            graft-verdict.txt; do
    if [ ! -e "$bundle/$f" ]; then
      echo "FAIL [$scenario]: missing bundle file: $f"
      missing=1
    fi
  done
  [ "$missing" = 0 ] || { cat "$envdir/stdout.txt"; return 1; }

  # New diag shape (v2.2.1): no confidence tier, no base-EFI fingerprint; the
  # action line is a descriptive `avb chain`. Assert the EFISP state line for
  # this scenario plus the structural invariants.
  if ! grep -q "EFISP        : $expect" "$bundle/report.txt"; then
    echo "FAIL [$scenario]: expected EFISP line '$expect' not in report.txt"
    echo "--- report.txt ---"
    cat "$bundle/report.txt"
    echo "--- stdout.txt ---"
    cat "$envdir/stdout.txt"
    return 1
  fi

  # The confidence headline and base-EFI fingerprint label are gone.
  if grep -qE '^[[:space:]]*confidence[[:space:]]*:' "$bundle/report.txt"; then
    echo "FAIL [$scenario]: report.txt still has a confidence line"
    cat "$bundle/report.txt"; return 1
  fi
  if grep -q 'unknown-base' "$bundle/report.txt"; then
    echo "FAIL [$scenario]: report.txt still emits unknown-base"
    cat "$bundle/report.txt"; return 1
  fi

  # Legacy action/graft/logfs lines are gone; the descriptive `avb chain`
  # line replaces the old `action req`.
  if grep -qE '^[[:space:]]*(action req|graft needed|fakelock req|logfs history)[[:space:]]*:' "$bundle/report.txt"; then
    echo "FAIL [$scenario]: report.txt still uses a legacy action/graft/logfs line"
    grep -E 'action req|graft needed|fakelock req|logfs history' "$bundle/report.txt"
    return 1
  fi
  if ! grep -qE '^[[:space:]]*avb chain[[:space:]]*:' "$bundle/report.txt"; then
    echo "FAIL [$scenario]: report.txt missing the avb chain line"
    cat "$bundle/report.txt"
    return 1
  fi

  echo "OK [$scenario] -> $expect"
}

# EFISP state line per synthetic scenario (no confidence tier any more):
#   high / medium — base EFI + valid GBLP1 overlay  -> "GBLP1 v1 ok"
#   low           — PE present but the GBLP1 is corrupt -> "PE present, GBLP1 ..."
#   none          — EFISP doesn't start with MZ        -> "not a PE"
run_one high   "GBLP1 v1 ok"
run_one medium "GBLP1 v1 ok"
run_one low    "PE present, GBLP1"
run_one none   "not a PE"

echo "PASS: 086 diag dryrun"
