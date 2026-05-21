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
  export BUNDLE_ROOT="$envdir/sdcard"      # tar.gz lands here (= /sdcard on device)
  export BUNDLE_WORKDIR="$envdir/work"     # staging dir (= /tmp on device)
  mkdir -p "$BUNDLE_WORKDIR"
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

  # Assert the expected confidence tier headline appears in report.txt.
  if ! grep -q "confidence   : $expect" "$bundle/report.txt"; then
    echo "FAIL [$scenario]: expected tier '$expect' not in report.txt"
    echo "--- report.txt ---"
    cat "$bundle/report.txt"
    echo "--- stdout.txt ---"
    cat "$envdir/stdout.txt"
    return 1
  fi

  # Verify the 2026-05-20 UI amendment: no `logfs history` line; the old
  # two-line `graft needed`/`fakelock req` shape was replaced by a single
  # mode-aware `action req` line (post-2026-05-20 v2 correction); legacy
  # "NO" / "YES" labels are gone.
  if grep -q '^[[:space:]]*logfs history' "$bundle/report.txt"; then
    echo "FAIL [$scenario]: report.txt still contains a logfs history UI line"
    grep '^[[:space:]]*logfs history' "$bundle/report.txt"
    return 1
  fi
  if grep -qE '^[[:space:]]*(graft needed|fakelock req)[[:space:]]*:' "$bundle/report.txt"; then
    echo "FAIL [$scenario]: report.txt still uses legacy graft needed/fakelock req lines"
    grep -E 'graft needed|fakelock req' "$bundle/report.txt"
    return 1
  fi
  if ! grep -qE '^[[:space:]]*action req[[:space:]]*:' "$bundle/report.txt"; then
    echo "FAIL [$scenario]: report.txt missing the action req line"
    cat "$bundle/report.txt"
    return 1
  fi
  if grep -qE 'action req[[:space:]]*:[[:space:]]*(YES|NO)\b' "$bundle/report.txt"; then
    echo "FAIL [$scenario]: action req uses legacy YES/NO labels (expected 'none' or partition list)"
    grep -E 'action req' "$bundle/report.txt"
    return 1
  fi

  echo "OK [$scenario] -> $expect"
}

run_one high   HIGH
run_one medium MEDIUM
run_one low    LOW
run_one none   NONE

echo "PASS: 086 diag dryrun"
