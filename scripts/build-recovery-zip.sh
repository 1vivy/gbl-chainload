#!/usr/bin/env bash
# scripts/build-recovery-zip.sh — assemble a single-mode installer ZIP
# from the zip-gbl-chainload submodule.
#
#   build-recovery-zip.sh --mode diag|graft|mode-0-install|mode-1-install|mode-2-install
#
# Hard-fails if the submodule's vendored binaries have drifted from
# zip/bin/MANIFEST (run zip/update-tools.sh to refresh).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

MODE=""
[ "${1:-}" = --mode ] && MODE="${2:-}"
case "$MODE" in
  diag|graft|mode-0-install|mode-1-install|mode-2-install) ;;
  *) echo "usage: $0 --mode diag|graft|mode-0-install|mode-1-install|mode-2-install" >&2; exit 2 ;;
esac

SUB=zip
[ -f "$SUB/META-INF/com/google/android/update-binary" ] \
  || { echo "error: submodule '$SUB' not checked out (git submodule update --init)" >&2; exit 1; }

# --- skew guard -----------------------------------------------------
MAN="$SUB/bin/MANIFEST"
[ -f "$MAN" ] || { echo "error: $MAN missing - run $SUB/update-tools.sh" >&2; exit 1; }

( cd "$SUB" && grep -E '^[0-9a-f]{64}  ' bin/MANIFEST | sha256sum -c --status ) \
  || { echo "error: vendored binaries are stale vs $MAN - run $SUB/update-tools.sh" >&2; exit 1; }

# disk -> MANIFEST: every file in bin/ and base/ must be tracked by the
# MANIFEST. The check above only covers MANIFEST -> disk; this catches an
# unmanifested, unverified binary dropped into bin/ without update-tools.sh.
have=$( cd "$SUB" && find bin base -type f ! -name MANIFEST | sort )
want=$( grep -E '^[0-9a-f]{64}  ' "$MAN" | sed -E 's/^[0-9a-f]{64}  //' | sort )
[ "$have" = "$want" ] \
  || { echo "error: $SUB/bin or $SUB/base has files not in $MAN - run $SUB/update-tools.sh" >&2; exit 1; }

PDIRTY=$(sed -n 's/^# parent-dirty: //p' "$MAN")
[ "$PDIRTY" = 0 ] \
  || { echo "error: $MAN marks a dirty-tree build - re-run update-tools.sh on a clean tree" >&2; exit 1; }

PCOMMIT=$(sed -n 's/^# parent-commit: //p' "$MAN")
if git cat-file -e "${PCOMMIT}^{commit}" 2>/dev/null; then
  git merge-base --is-ancestor "$PCOMMIT" HEAD 2>/dev/null \
    || { echo "error: $MAN parent-commit $PCOMMIT is not an ancestor of HEAD - re-run update-tools.sh" >&2; exit 1; }
else
  echo "warning: $MAN parent-commit $PCOMMIT not in local history (shallow clone?) - ancestor check skipped" >&2
fi

# --- stage, select the mode, prune ----------------------------------
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -r "$SUB"/. "$STAGE"/
rm -rf "$STAGE/update-tools.sh" "$STAGE/README.md"
# strip VCS/agent dotfiles at every level (.git, .github, .gitignore,
# .omc, ...) - the cp above pulls in the submodule's nested state.
find "$STAGE" -mindepth 1 -name '.*' -prune -exec rm -rf {} +

# --- substitute project VERSION placeholder in update-binary ---
GBL_VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
[ -n "$GBL_VERSION" ] || { echo "error: empty VERSION at $ROOT/VERSION" >&2; exit 1; }
sed -i "s|__GBL_VERSION__|${GBL_VERSION}|g" \
    "$STAGE/META-INF/com/google/android/update-binary"
# Belt-and-braces: refuse to ship a ZIP that still contains the placeholder.
if grep -q '__GBL_VERSION__' "$STAGE/META-INF/com/google/android/update-binary"; then
  echo "error: placeholder __GBL_VERSION__ still in update-binary after substitution" >&2
  exit 1
fi

echo "$MODE" > "$STAGE/modes/SELECTED"

# read the selected mode's declared artifact needs (MODE_TOOLS / MODE_EFI for
# the bin/ + base/ prune below; MODE_LIB for the modes/ prune just after).
MODE_TOOLS=""; MODE_EFI=""; MODE_LIB=""
# shellcheck disable=SC1090
. "$SUB/modes/$MODE.conf"

# Prune the non-selected modes. A real mode is a <name>.sh + <name>.conf pair.
# A .sh with no sibling .conf is a shared mode lib (e.g. install-common.sh,
# which the three mode-N-install modes source): keep it only when the selected
# mode's .conf declares it as MODE_LIB, so diag/graft ZIPs do not gain dead
# libs. This is declarative (parallel to MODE_TOOLS/MODE_EFI) rather than
# grepping the mode's script source for a source-path string.
# Mode stems are enumerated up front (a real mode = a .conf file), before any
# deletion, so removing mode X's .conf does not hide mode Y's pairing.
MODE_STEMS=""
for c in "$STAGE"/modes/*.conf; do
  [ -f "$c" ] || continue
  b=$(basename "$c"); MODE_STEMS="$MODE_STEMS ${b%.conf}"
done
for f in "$STAGE"/modes/*.conf "$STAGE"/modes/*.sh; do
  [ -f "$f" ] || continue
  b=$(basename "$f"); m=${b%.*}
  [ "$m" = "$MODE" ] && continue
  case "$f" in
    *.sh) case " $MODE_STEMS " in
            *" $m "*) ;;          # a paired mode script - removable
            *)                    # no sibling .conf: a shared mode lib -
                                  # keep only if the selected mode declares it
              [ "$b" = "$MODE_LIB" ] && continue
              ;;
          esac ;;
  esac
  rm -f "$f"
done

# prune bin/: keep MANIFEST (shipped for on-device provenance) and
# busybox-arm64 (core infrastructure, always bundled - not a per-mode
# tool); keep tools the mode declares in MODE_TOOLS, drop the rest.
for t in "$STAGE"/bin/*; do
  b=$(basename "$t")
  [ "$b" = MANIFEST ] && continue
  [ "$b" = busybox-arm64 ] && continue
  case " $MODE_TOOLS " in *" $b "*) ;; *) rm -f "$t" ;; esac
done
# prune base/: keep only MODE_EFI (if any)
for e in "$STAGE"/base/*; do
  [ -e "$e" ] || continue
  b=$(basename "$e")
  [ "$b" = "$MODE_EFI" ] || rm -f "$e"
done
rmdir "$STAGE/base" 2>/dev/null || true

# --- checksums + zip ------------------------------------------------
# shellcheck disable=SC2094  # SHA256SUMS excluded from find; no read/write conflict
( cd "$STAGE" && find . -type f ! -name SHA256SUMS -exec sha256sum {} + \
    | sed 's#  \./#  #' > SHA256SUMS )
mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/gbl-chainload-$MODE.zip"
rm -f "$OUT"
( cd "$STAGE" && zip -qr "$OUT" . )
echo "==> $OUT"
unzip -l "$OUT"
