#!/usr/bin/env bash
# scripts/release.sh — one-command release prep.
#
# Usage:
#   scripts/release.sh X.Y.Z
#   scripts/release.sh --dry-run X.Y.Z
#   scripts/release.sh --help
#
# Does the dance: bumps VERSION, scaffolds a CHANGELOG section, refreshes
# the zip submodule's vendored artifacts via zip/update-tools.sh, bumps
# the parent's zip pointer, commits, pushes the release branch, and opens
# a PR. After merge, you push the tag (`git push origin vX.Y.Z`) and
# release.yml drafts the GitHub release.
#
# Hard requirements before invocation:
#   · clean working tree on main, up to date with origin/main
#   · X.Y.Z is valid semver
#   · vX.Y.Z is not already a tag
#   · gh CLI authenticated; zip submodule initialized

set -euo pipefail

DRY_RUN=0
VER=""

usage() {
  cat <<'EOF'
Usage: scripts/release.sh [--dry-run] X.Y.Z

Prep a release branch + PR for version X.Y.Z. After this script:
  1. Review the PR (CHANGELOG highlights are stubbed — fill them in).
  2. Merge it.
  3. git push origin vX.Y.Z   (triggers release.yml → draft release)

Options:
  --dry-run    Print commands without executing.
  -h, --help   Show this message.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run)  DRY_RUN=1; shift ;;
    -h|--help)  usage; exit 0 ;;
    *)
      if [ -z "$VER" ]; then VER="$1"; shift
      else echo "error: unexpected argument '$1'"; usage; exit 2; fi ;;
  esac
done

if [ -z "$VER" ]; then echo "error: version required"; usage; exit 2; fi
if ! echo "$VER" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
  echo "error: '$VER' is not X.Y.Z semver"; exit 2
fi

run() {
  if [ "$DRY_RUN" -eq 1 ]; then printf '  %s\n' "$*"; else eval "$*"; fi
}

# Trap state for rollback on failure.
ORIG_BRANCH=$(git rev-parse --abbrev-ref HEAD)
CREATED_BRANCH=0
cleanup() {
  rc=$?
  if [ "$rc" -ne 0 ] && [ "$CREATED_BRANCH" -eq 1 ] && [ "$DRY_RUN" -eq 0 ]; then
    echo "==> release prep failed; cleaning up release-prep-v$VER branch"
    git checkout "$ORIG_BRANCH" >/dev/null 2>&1 || true
    git branch -D "release-prep-v$VER" >/dev/null 2>&1 || true
  fi
  exit $rc
}
trap cleanup EXIT

# --- pre-flight ---
echo "==> pre-flight"
if [ "$DRY_RUN" -eq 0 ]; then
  command -v gh >/dev/null 2>&1 \
    || { echo "error: gh CLI not installed (https://cli.github.com/)"; exit 1; }
  [ "$ORIG_BRANCH" = main ] \
    || { echo "error: must run from main (currently '$ORIG_BRANCH')"; exit 1; }
  [ -z "$(git status --porcelain)" ] \
    || { echo "error: working tree not clean"; git status --short; exit 1; }
  git fetch --quiet origin main
  [ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] \
    || { echo "error: local main not at origin/main"; exit 1; }
  if git rev-parse "v$VER" >/dev/null 2>&1; then
    echo "error: tag v$VER already exists"; exit 1
  fi
fi

# --- branch ---
BRANCH="release-prep-v$VER"
echo "==> creating $BRANCH"
run "git checkout -b '$BRANCH'"
CREATED_BRANCH=1

# --- VERSION ---
echo "==> bumping VERSION to $VER"
run "echo '$VER' > VERSION"

# --- CHANGELOG scaffold ---
echo "==> scaffolding CHANGELOG.md section"
TODAY=$(date -u +%Y-%m-%d)
if [ "$DRY_RUN" -eq 0 ]; then
  {
    head -1 CHANGELOG.md  # "# Changelog"
    printf '\n## v%s — %s\n\nHighlights:\n\n- TODO\n\nFixes:\n\n- TODO\n' "$VER" "$TODAY"
    tail -n +2 CHANGELOG.md
  } > CHANGELOG.md.tmp && mv CHANGELOG.md.tmp CHANGELOG.md
  echo "    -> filled in stubs; open CHANGELOG.md to write the highlights"
else
  printf '  prepend section "## v%s — %s" to CHANGELOG.md\n' "$VER" "$TODAY"
fi

# --- parent commit FIRST (VERSION + CHANGELOG) ---
# This MUST happen before update-tools.sh so the parent tree is clean
# when the zip submodule's update-tools.sh probes it. Otherwise MANIFEST
# gets stamped `parent-dirty: 1` and tests/host/071_zip_assembly's skew
# guard fires (cf. release.yml `build-recovery-zip.sh` line 28-29).
echo "==> committing VERSION + CHANGELOG on the release branch"
run "git add VERSION CHANGELOG.md"
run "git commit -m 'release: $VER'"

# --- zip refresh ---
# Submodule is typically in detached HEAD after a fresh clone / submodule
# update. We need the commit to land on zip's main and reach origin/main
# so the parent's bumped pointer is reachable (verify-job reachability
# check enforces this at release time).
echo "==> preparing zip submodule (checkout main, fast-forward from origin)"
run "( cd zip && git fetch --quiet origin main )"
run "( cd zip && git checkout main )"
run "( cd zip && git merge --ff-only origin/main )"

echo "==> refreshing zip submodule artifacts (rebuilds EFI + bin/gbl)"
run "( cd zip && bash update-tools.sh )"
run "git -C zip add -A"
run "git -C zip commit -m 'release: $VER — refresh vendored artifacts'"

echo "==> pushing zip's main so the new commit is reachable"
run "git -C zip push origin main"

# --- amend parent's release commit with the zip pointer ---
# Fold the zip submodule pointer bump into the prior `release: $VER`
# commit so the branch ends up with ONE focused commit. Reviewers see
# VERSION + CHANGELOG + zip pointer in a single diff.
echo "==> folding zip pointer bump into the release commit"
run "git add zip"
run "git commit --amend --no-edit"

# --- push + PR ---
echo "==> pushing branch + opening PR"
run "git push -u origin '$BRANCH'"
run "gh pr create --base main --head '$BRANCH' \
  --title 'release: $VER' \
  --body 'Single-purpose release PR for v$VER. After merge, push tag v$VER to trigger the draft release.'"

CREATED_BRANCH=0  # success — disarm rollback

cat <<EOF

==> done. Next steps:
  1. Review the PR (CHANGELOG highlights are stubbed — fill them in).
  2. Merge the PR.
  3. git push origin v$VER
     (release.yml will draft the GitHub release after CI is green.)
EOF
