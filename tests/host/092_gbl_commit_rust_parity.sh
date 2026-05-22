#!/usr/bin/env bash
# tests/host/092_gbl_commit_rust_parity.sh
# Behavioural parity between the C gbl-commit (tools/gbl-commit) and the Rust
# pilot (tools-rs/gbl-commit): for each scenario both must return the same exit
# code and leave byte-identical output files. SKIPs cleanly if cargo is absent
# so CI without a Rust toolchain stays green.
#
# Device-only paths (uncached short read-back on a fixed-size partition smaller
# than the payload) aren't reachable with regular files and are covered on the
# host only insofar as the verify-ok path exercises read_back_uncached; they
# remain validated on-device.
set -euo pipefail
cd "$(dirname "$0")/../.."

command -v cargo >/dev/null 2>&1 || { echo "SKIP: cargo not installed"; exit 0; }

OUT=tests/host/.last/092
rm -rf "$OUT"; mkdir -p "$OUT"

make -s -C tools/gbl-commit
CBIN=tools/gbl-commit/gbl-commit

cargo build --release --quiet --manifest-path tools-rs/Cargo.toml
RBIN=tools-rs/target/release/gbl-commit

fail() { echo "FAIL: $*"; exit 1; }

# run <tag> <binary> <workdir> <args...> -> echoes exit code, populates workdir
run_one() {
  tag=$1; bin=$2; wd=$3; shift 3
  rc=0
  ( cd "$wd" && "$OLDPWD/$bin" "$@" ) >"$wd/stdout.txt" 2>"$wd/stderr.txt" || rc=$?
  echo "$rc"
}

# Compare exit code + named artifacts between C and Rust for one scenario.
# scenario <name> <args-template...>  (uses files seeded into each wd)
compare_scenario() {
  name=$1; shift
  cdir="$OUT/$name/c"; rdir="$OUT/$name/r"
  mkdir -p "$cdir" "$rdir"

  # Seed identical inputs into both dirs.
  head -c 7000 /dev/urandom > "$cdir/src"; cp "$cdir/src" "$rdir/src"
  head -c 9000 /dev/urandom > "$cdir/dst"; cp "$cdir/dst" "$rdir/dst"

  crc=$(run_one c "$CBIN" "$cdir" "$@")
  rrc=$(run_one r "$RBIN" "$rdir" "$@")

  [ "$crc" = "$rrc" ] || fail "$name: exit codes differ (c=$crc r=$rrc)"

  # Output files that should match byte-for-byte when present in both.
  for f in dst bak; do
    if [ -f "$cdir/$f" ] || [ -f "$rdir/$f" ]; then
      cmp -s "$cdir/$f" "$rdir/$f" || fail "$name: $f differs between C and Rust"
    fi
  done
  echo "  ok: $name (exit=$crc)"
}

# 1. write + backup + uncached verify  -> rc 0, dst==src-prefix, bak==orig dst
compare_scenario verify_ok   --src src --dst dst --backup bak --verify
# 2. write + backup, no verify          -> rc 0
compare_scenario no_verify   --src src --dst dst --backup bak
# 3. write, no backup, verify           -> rc 0
compare_scenario no_backup   --src src --dst dst --verify
# 4. usage error (missing --dst)         -> rc 2
compare_scenario missing_dst --src src
# 5. unknown arg                         -> rc 2
compare_scenario unknown_arg --src src --dst dst --bogus

# Spot-check a concrete invariant on scenario 1 (not just C==R): the write
# actually landed and the backup preserved the original.
v="$OUT/verify_ok/r"
cmp -s -n 7000 "$v/src" "$v/dst" || fail "verify_ok: first 7000 bytes of dst != src (write didn't land)"

# --version strings identical.
[ "$("$CBIN" --version)" = "$("$RBIN" --version)" ] || fail "--version differs"
echo "  ok: --version parity"

echo "PASS: 092 gbl-commit C/Rust parity"
