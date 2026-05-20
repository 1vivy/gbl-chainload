#!/usr/bin/env bash
# tests/host/086_diag_dryrun.sh — drive zip/modes/diag.sh against a
# synthetic environment in each confidence tier.
set -euo pipefail
cd "$(dirname "$0")/../.."

# 060 + zip/base/mode-1.efi feed the diag_fake_byname helper. 060 SKIPs when
# the upstream PE fixture is absent (gitignored — e.g. on CI), so propagate
# the SKIP rather than ERRORing downstream.
if [ ! -f tests/host/.last/060/payload.bin ]; then
  bash tests/host/060_pack_roundtrip.sh
  [ -f tests/host/.last/060/payload.bin ] || {
    echo "SKIP: 086 — upstream payload.bin absent (PE fixture missing)"; exit 0;
  }
fi
[ -f zip/base/mode-1.efi ] || {
  echo "SKIP: 086 — zip/base/mode-1.efi absent"; exit 0;
}

# Build native tools (used via PATH inside diag.sh).
make -s -C tools/gblp1-inspect
make -s -C tools/vbmeta-graft
make -s -C tools/fv-unwrap

OUT=tests/host/.last/086
rm -rf "$OUT"; mkdir -p "$OUT"

run_one() {
  local scenario="$1" expect="$2"
  local envdir="$OUT/$scenario"

  bash tests/host/helpers/diag_fake_byname.sh "$envdir" "$scenario"

  # Fake recovery environment variables.
  export WORKDIR="$envdir/zip"
  export BYNAME="$envdir/byname"
  export BUNDLE_ROOT="$envdir/sdcard"
  export SLOT=a
  export INACTIVE=b
  export OTA_POSTINSTALL=false

  # Open OUTFD=9 for screen output (recovery I/O stand-in).
  exec 9>"$envdir/screen.txt"
  export OUTFD=9

  # PATH so diag.sh can call tools by bare name.
  local tool_path
  tool_path="$PWD/tools/gblp1-inspect:$PWD/tools/vbmeta-graft:$PWD/tools/fv-unwrap"
  export PATH="$tool_path:$PATH"

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

  # Locate the bundle directory.
  local bundle
  bundle=$(ls -d "$BUNDLE_ROOT"/gbl-chainload-diag-* 2>/dev/null | grep -v '\.tar\.gz' | head -1 || true)
  if [ -z "$bundle" ]; then
    echo "FAIL [$scenario]: no bundle directory created"
    cat "$envdir/stdout.txt"
    return 1
  fi

  # Assert every required file is present.
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

  # Assert sibling tar.gz exists.
  if [ ! -f "$bundle.tar.gz" ]; then
    echo "FAIL [$scenario]: sibling tar.gz missing ($bundle.tar.gz)"
    cat "$envdir/stdout.txt"
    return 1
  fi

  # Assert tar.gz lists every bundled file.
  local tgz_ok=1
  local tgz_listing
  tgz_listing=$(tar -tzf "$bundle.tar.gz" 2>/dev/null)
  for f in report.txt env.txt getprop.boot.txt efisp.img abl_a.img abl_b.img \
            vbmeta_a.img vbmeta_b.img logfs.img \
            gblp1-inspect.txt loader-abl.txt vbmeta-descriptors.txt \
            graft-verdict.txt; do
    if ! echo "$tgz_listing" | grep -qF "/$f"; then
      echo "FAIL [$scenario]: tar.gz does not list: $f"
      tgz_ok=0
    fi
  done
  [ "$tgz_ok" = 1 ] || return 1

  # Assert the expected confidence tier headline appears in report.txt.
  if ! grep -q "confidence   : $expect" "$bundle/report.txt"; then
    echo "FAIL [$scenario]: expected tier '$expect' not in report.txt"
    echo "--- report.txt ---"
    cat "$bundle/report.txt"
    echo "--- stdout.txt ---"
    cat "$envdir/stdout.txt"
    return 1
  fi

  echo "OK [$scenario] -> $expect"
}

run_one high   HIGH
run_one medium MEDIUM
run_one low    LOW
run_one none   NONE

echo "PASS: 086 diag dryrun"
