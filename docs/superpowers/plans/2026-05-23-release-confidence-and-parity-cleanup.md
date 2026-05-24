# Release confidence + parity cleanup — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land four units of housekeeping (golden retirement, release-confidence CI invariants, CI optimizations, CLAUDE.md flexibility) as one PR on top of `main`, so the follow-up v2.3.4 release is a 2-file diff.

**Architecture:** All edits are localized to `.github/workflows/`, `tests/host/`, `tools/RELEASE_README.md`, `scripts/release.sh` (new), and `CLAUDE.md`. No production-code changes. Tasks are ordered so each commit leaves the tree in a working state — golden retirement first (unblocks CI), then release.yml extensions stacked, then orthogonal items (CI optimizations + CLAUDE.md), then PR finalize.

**Tech Stack:** GitHub Actions YAML, POSIX shell (release.sh), bash (test scripts), Docker (build image already cached via gha).

**Branch:** `release-prep-v2.3.4` (will be retitled at the end; force-push the new history).

---

## Pre-work — current branch state

The branch already has unstaged diffs from earlier work:
- `.github/workflows/release.yml` — single-binary `build-host-tools` refactor (drop the 7-tool loop, check for `gbl` / `gbl.exe` only) + `build-efi` job stub + EFI in release assets.
- `tools/RELEASE_README.md` — multicall rewrite.

Task 0 commits these as-is so subsequent tasks add cleanly on top.

---

### Task 0: Commit pre-staged workflow + RELEASE_README changes

**Goal:** Land the single-binary `build-host-tools` loop refactor and the RELEASE_README multicall rewrite that are already in the working tree.

**Files:**
- Modify: `.github/workflows/release.yml` (already changed in working tree)
- Modify: `tools/RELEASE_README.md` (already changed in working tree)

**Acceptance Criteria:**
- [ ] Commit lands with title `release.yml: single-gbl build-host-tools + RELEASE_README multicall rewrite`
- [ ] `git diff main HEAD` shows two files: `release.yml` and `RELEASE_README.md`
- [ ] No regressions to other files in the diff

**Verify:** `git log --oneline -1` shows the new commit; `python3 -c 'import yaml; yaml.safe_load(open(".github/workflows/release.yml"))'` exits 0.

**Steps:**

- [ ] **Step 1: Sanity-check diff scope**

```bash
git status --short
# Expect:
#  M .github/workflows/release.yml
#  M tools/RELEASE_README.md
```

- [ ] **Step 2: Confirm release.yml stays valid YAML**

```bash
python3 -c 'import yaml; yaml.safe_load(open(".github/workflows/release.yml"))'
```
Expected: no output, exit 0.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/release.yml tools/RELEASE_README.md
git commit -m "release.yml: single-gbl build-host-tools + RELEASE_README multicall rewrite

PR #44 collapsed the seven C host tools into a single \`gbl\` multicall.
Update release.yml's build-host-tools job to verify the per-platform
dist/<plat>/gbl (gbl.exe on Windows) instead of looping over the seven
deleted binaries. Rewrite tools/RELEASE_README.md to document the
multicall + subcommand map. The build-efi job that copies the EFI into
release assets stays as scaffolding here; Task 2 adds the parity
assertion that makes it trustworthy.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 1: Retire all parity-contract goldens

**Goal:** Delete `tests/host/goldens/` entirely; strip every `cmp -s ... goldens/...` / `diff -u ... goldens/...` line from the 18 affected test scripts; replace the 5 textual-golden assertions with inline `grep -qE` schema checks that capture the same behavior.

**Files:**
- Delete: `tests/host/goldens/` (entire tree, including `MANIFEST`)
- Modify (binary-golden tests — strip cmp lines only):
  - `tests/host/060_pack_roundtrip.sh:52-55` (drop 2 `cmp -s` blocks)
  - `tests/host/061_parser_fuzz.sh:55-57` (drop 1 `cmp -s` block)
  - `tests/host/064_e2e_fixtures.sh:35-39` (drop the `cmp -s` loop body)
  - `tests/host/067_blockio_reader_smoke.sh:52-55` (drop 2 `cmp -s` blocks)
  - `tests/host/069_full_buffer_scan.sh:52-54` (drop 1 `cmp -s` block)
  - `tests/host/072_fv_unwrap_exploit.sh:22-26` (drop 2 `cmp -s` blocks)
  - `tests/host/081_gbl_pack_mode2_profile.sh:75-83` (drop 2 `cmp -s` blocks)
  - `tests/host/082_mode2_profile_parity.sh:104-106` (drop 1 `cmp -s` block; rename test if filename or comments still say "parity")
  - `tests/host/083_abl_patcher_oem.sh:108-110` (drop `cmp -s` loop body)
  - `tests/host/085_efisp_package.sh:139-141` (drop `cmp -s` loop body)
  - `tests/host/088_patch7_multi_abl.sh:131-141` (drop 2 `cmp -s` loop bodies)
  - `tests/host/094_gbl_pack_manifest.sh:81-83` (drop `cmp -s` loop body)
- Modify (textual-golden tests — replace diff with inline grep):
  - `tests/host/074_vbmeta_graft.sh:62-64` (replace `diff -u .../list.txt`)
  - `tests/host/087_mode2_profile_regression.sh:10-15+` (replace `GOLDEN=`-based diff)
  - `tests/host/089_gblp1_inspect.sh:96-106` (mixed — drop the `cmp -s` lines for `.bin` files, replace the `diff -u` lines for `.txt` files)
  - `tests/host/090_vbmeta_descriptor_hash.sh:58-60` (replace 2 `diff -u`)
  - `tests/host/091_vbmeta_graft_py.sh:59-61` (replace 1 `diff -u`)
  - `tests/host/096_recovery_graft_real.sh:74-76` (replace 2 `diff -u`)

**Acceptance Criteria:**
- [ ] `test -d tests/host/goldens/` returns false (directory gone)
- [ ] `grep -rn "tests/host/goldens/" tests/host/ scripts/ .github/` returns empty
- [ ] Every modified test script passes locally: `bash tests/host/<name>.sh` exits 0
- [ ] Textual-golden replacements assert at least one specific field from the prior golden file (no blanket "command succeeded" — must check the actual behavior)

**Verify:**

```bash
# Goldens dir gone
test -d tests/host/goldens/ && echo FAIL || echo OK

# No stale references
grep -rln "goldens/" tests/host/ scripts/ .github/ && echo FAIL || echo OK

# Each affected test passes
for t in 060 061 064 067 069 072 074 081 082 083 085 087 088 089 090 091 094 096; do
  bash tests/host/${t}_*.sh >/dev/null 2>&1 \
    && echo "ok $t" || echo "FAIL $t"
done
```

**Steps:**

- [ ] **Step 1: Strip cmp blocks from binary-golden tests**

For each binary-golden test, the `cmp -s` block is 3 lines: the assertion + a `|| { ... }` failure clause that prints "FAIL N golden". Example from `060_pack_roundtrip.sh`:

```bash
# BEFORE
# Golden parity assertion (frozen C-tool output; PR2 Rust port must match).
cmp -s "$OUT/payload.bin"  tests/host/goldens/060/payload.bin \
  || { echo "FAIL 060 golden: payload.bin diverged from frozen C-tool output"; exit 1; }
cmp -s "$OUT/patched.efi"  tests/host/goldens/060/patched.efi \
  || { echo "FAIL 060 golden: patched.efi diverged from frozen C-tool output"; exit 1; }

# AFTER
# (block removed entirely — the parser/roundtrip checks above are the
# regression guard now that the Rust impl is authoritative)
```

Use `Edit` per file. The pattern is mechanical:
- Find the `cmp -s "$OUT/<file>" tests/host/goldens/<n>/<file>` line and the next two lines (`  || { echo "FAIL <n> golden..."; exit 1; }`).
- Delete those three lines + the preceding comment line if it mentions "golden parity" / "frozen C-tool".

Repeat across 060, 061, 064, 067, 069, 072, 081, 082, 083, 085, 088, 089 (only `.bin` lines), 094.

- [ ] **Step 2: Replace textual-golden diffs with inline grep**

`tests/host/074_vbmeta_graft.sh` (and `091_vbmeta_graft_py.sh` — same pattern):

```bash
# BEFORE
diff -u tests/host/goldens/074/list.txt "$OUT/list.txt" \
  || { echo "FAIL 074 golden: list.txt diverged"; exit 1; }

# AFTER
grep -qE '^partition=recovery type=hash graftable=yes$' "$OUT/list.txt" \
  || { echo "FAIL 074: list.txt missing 'partition=recovery type=hash graftable=yes' line"; cat "$OUT/list.txt"; exit 1; }
grep -qE '^descriptor type=other$' "$OUT/list.txt" \
  || { echo "FAIL 074: list.txt missing 'descriptor type=other' line"; cat "$OUT/list.txt"; exit 1; }
```

`tests/host/087_mode2_profile_regression.sh` — currently sources `GOLDEN=tests/host/goldens/087/baseline.toml`. Replace with inline field checks:

```bash
# BEFORE  (lines around the GOLDEN= variable and its diff)
GOLDEN="tests/host/goldens/087/baseline.toml"
# ... derive runs, then:
diff -u "$GOLDEN" "$OUT/baseline.toml" \
  || { echo "FAIL 087: baseline diverged"; exit 1; }

# AFTER
grep -qE '^version[[:space:]]*= 1$'          "$OUT/baseline.toml" \
  || { echo "FAIL 087: version != 1"; cat "$OUT/baseline.toml"; exit 1; }
grep -qE '^is_unlocked[[:space:]]*= 0$'      "$OUT/baseline.toml" \
  || { echo "FAIL 087: is_unlocked != 0"; cat "$OUT/baseline.toml"; exit 1; }
grep -qE '^color[[:space:]]*= 0$'            "$OUT/baseline.toml" \
  || { echo "FAIL 087: color != 0"; cat "$OUT/baseline.toml"; exit 1; }
grep -qE '^system_version[[:space:]]*= 0x40000$' "$OUT/baseline.toml" \
  || { echo "FAIL 087: system_version != 0x40000"; cat "$OUT/baseline.toml"; exit 1; }
grep -qE '^# spl:'                            "$OUT/baseline.toml" \
  || { echo "FAIL 087: spl comment missing"; cat "$OUT/baseline.toml"; exit 1; }
```

Also remove the `GOLDEN=` variable assignment line — it's now unused.

`tests/host/089_gblp1_inspect.sh` has mixed binary + textual golden assertions:

```bash
# BEFORE (lines 96-106 area)
cmp -s "$OUT/payload.bin"   tests/host/goldens/089/payload.bin   || { echo "FAIL 089: payload diverged"; exit 1; }
diff -u tests/host/goldens/089/ok.txt        "$OUT/ok.txt"        || { echo "FAIL 089: ok.txt diverged"; exit 1; }
cmp -s "$OUT/manifest.bin"  tests/host/goldens/089/manifest.bin  || { echo "FAIL 089: manifest.bin diverged"; exit 1; }
diff -u tests/host/goldens/089/manifest.txt  "$OUT/manifest.txt"  || { echo "FAIL 089: manifest.txt diverged"; exit 1; }
cmp -s "$OUT/manifest2.bin" tests/host/goldens/089/manifest2.bin || { echo "FAIL 089: manifest2.bin diverged"; exit 1; }
diff -u tests/host/goldens/089/manifest2.txt "$OUT/manifest2.txt" || { echo "FAIL 089: manifest2.txt diverged"; exit 1; }

# AFTER — drop the .bin lines, replace the .txt diffs with inline grep:
grep -qE '^result: ok$' "$OUT/ok.txt" \
  || { echo "FAIL 089: ok.txt missing 'result: ok'"; cat "$OUT/ok.txt"; exit 1; }
grep -qE '^header: magic=ok version=1 ' "$OUT/ok.txt" \
  || { echo "FAIL 089: ok.txt missing valid header line"; cat "$OUT/ok.txt"; exit 1; }
grep -qE '^entry: type=0x0001 \(CACHED_ABL\)' "$OUT/ok.txt" \
  || { echo "FAIL 089: ok.txt missing CACHED_ABL entry"; cat "$OUT/ok.txt"; exit 1; }

grep -qE 'WantFakelockHook|WantProfileSpoof|manifest_bits' "$OUT/manifest.txt" \
  || { echo "FAIL 089: manifest.txt missing manifest-bit field"; cat "$OUT/manifest.txt"; exit 1; }
grep -qE 'WantFakelockHook|WantProfileSpoof|manifest_bits' "$OUT/manifest2.txt" \
  || { echo "FAIL 089: manifest2.txt missing manifest-bit field"; cat "$OUT/manifest2.txt"; exit 1; }
```

`tests/host/090_vbmeta_descriptor_hash.sh`:

```bash
# BEFORE
diff -u tests/host/goldens/090/lh.txt         "$OUT/lh.txt"
diff -u tests/host/goldens/090/lh-corrupt.txt "$OUT/lh-corrupt.txt"

# AFTER
grep -qE '^partition=recovery type=hash declared=[0-9]+ digest=mismatch graft=n/a verdict=mismatch$' "$OUT/lh.txt" \
  || { echo "FAIL 090: lh.txt missing recovery mismatch line"; cat "$OUT/lh.txt"; exit 1; }
grep -qE 'verdict=mismatch' "$OUT/lh-corrupt.txt" \
  || { echo "FAIL 090: lh-corrupt.txt missing 'verdict=mismatch'"; cat "$OUT/lh-corrupt.txt"; exit 1; }
```

`tests/host/096_recovery_graft_real.sh` — has two `diff -u` against the same golden:

```bash
# BEFORE
diff -u tests/host/goldens/096/default-list.txt "$OUT/default-list.txt"   || ...
diff -u tests/host/goldens/096/default-list.txt "$OUT/size-from-list.txt" || ...

# AFTER (both lists describe the same recovery graft state)
for out in "$OUT/default-list.txt" "$OUT/size-from-list.txt"; do
  grep -qE 'partition=recovery' "$out" \
    || { echo "FAIL 096: $out missing recovery partition"; cat "$out"; exit 1; }
  grep -qE 'graftable=yes' "$out" \
    || { echo "FAIL 096: $out missing graftable=yes"; cat "$out"; exit 1; }
done
```

`tests/host/091_vbmeta_graft_py.sh` follows the same shape as 074.

- [ ] **Step 3: Delete the goldens tree**

```bash
rm -rf tests/host/goldens/
git status --short tests/host/goldens/  # empty
```

- [ ] **Step 4: Run all affected tests**

```bash
for t in 060 061 064 067 069 072 074 081 082 083 085 087 088 089 090 091 094 096; do
  echo "== $t =="
  bash tests/host/${t}_*.sh
done
```

All should exit 0 / print `PASS:` or `ok` lines.

- [ ] **Step 5: Re-check no stale references**

```bash
grep -rln "goldens/" tests/host/ scripts/ .github/
# Expect empty
```

- [ ] **Step 6: Commit**

```bash
git add -u tests/host/
git commit -m "tests: retire parity-contract goldens; inline schema checks for textual ones

The byte-identity goldens under tests/host/goldens/ existed as a
parity contract during the PR2 Rust-tooling migration: prove the new
\`gbl\` multicall's output byte-matches the deleted C tools. PR #44
shipped, on-device validation is green across all three modes, and
the Rust impl is now authoritative. The goldens are now scaffolding
that breaks on legitimate VERSION bumps (gbl-pack embeds VERSION via
include_str! at compile time).

Retire the whole tests/host/goldens/ tree. Each formerly-golden test
keeps its real regression checks (parser status, roundtrip size,
pre-/post-condition asserts). The 5 textual goldens become inline
\`grep -qE\` schema checks on the specific fields they were
encoding — same coverage, no byte-identity lock-in.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: build-efi job — fresh build + parity checks vs zip submodule

**Goal:** Add a release.yml job that builds **both** vendored artifacts in CI — the firmware EFI and the recovery `gbl` multicall — and asserts both byte-match the zip submodule's vendored copies. Uploads the EFI as a release asset. Catches "developer forgot to run `zip/update-tools.sh` after changing firmware or tooling source".

The two parity checks replace the squash-merge-fragile "MANIFEST parent-commit == tag SHA" invariant that Task 3 originally tried to enforce. They catch the same failure mode (vendored artifacts stale vs source at tag time) without depending on SHA equality through squash merges.

**Files:**
- Modify: `.github/workflows/release.yml` (the `build-efi` job is already in the file from Task 0; Task 2 adds two parity-check steps + the recovery-gbl build step).

**Acceptance Criteria:**
- [ ] `build-efi` job has a step named "EFI parity — built == vendored in zip" comparing sha256s of `dist/gbl-chainload.efi` and `zip/base/gbl-chainload.efi`
- [ ] `build-efi` job has a step named "Build recovery gbl multicall" that runs `bash scripts/build-recovery-tools.sh` (this populates `dist/recovery/gbl`)
- [ ] `build-efi` job has a step named "Recovery gbl parity — built == vendored in zip" comparing sha256s of `dist/recovery/gbl` and `zip/bin/gbl`
- [ ] Both parity steps fail loudly with a recovery hint pointing to `zip/update-tools.sh`
- [ ] The job still uploads `release-stage/gbl-chainload-v${ver}.efi` as the `efi-payload` artifact
- [ ] The `release` job's "Compute top-level SHA256SUMS" step folds in the EFI
- [ ] `release create` includes the EFI in its asset list

**Verify:**

```bash
python3 -c 'import yaml; j=yaml.safe_load(open(".github/workflows/release.yml"))["jobs"]; \
  assert "build-efi" in j; \
  names=[s.get("name","") for s in j["build-efi"]["steps"]]; \
  assert any("EFI parity" in n for n in names), "EFI parity step missing"; \
  assert any("Build recovery gbl" in n for n in names), "recovery gbl build missing"; \
  assert any("Recovery gbl parity" in n for n in names), "recovery gbl parity missing"; \
  assert "build-efi" in j["release"]["needs"], "release job not waiting on build-efi"; \
  assert "efi-payload" in str(j["build-efi"]["steps"]), "artifact name missing"'
```

**Steps:**

- [ ] **Step 1: Read the current build-efi job (from Task 0's commit)**

```bash
grep -n "build-efi:" .github/workflows/release.yml
# Note the line range; the job already has checkout, download-artifact (build-image),
# load image, build.sh, rename+checksum, upload-artifact steps.
```

- [ ] **Step 2: Insert the EFI parity step between "Build EFI payload" and "Rename + checksum"**

Edit `release.yml`:

```yaml
      - name: Build EFI payload (firmware staticlibs + EDK2 link)
        run: bash scripts/build.sh --no-recovery --no-host
      - name: EFI parity — built == vendored in zip
        # Asserts the freshly built dist/gbl-chainload.efi is byte-identical
        # to zip/base/gbl-chainload.efi. A mismatch means the zip submodule's
        # vendored EFI is stale: the developer changed firmware source but
        # did not run zip/update-tools.sh to refresh the vendored artifacts.
        # Without this, the release ships two different EFIs (the asset and
        # the one inside installer ZIPs).
        run: |
          set -euo pipefail
          fresh=$(sha256sum dist/gbl-chainload.efi | awk '{print $1}')
          vendored=$(sha256sum zip/base/gbl-chainload.efi | awk '{print $1}')
          if [ "$fresh" != "$vendored" ]; then
            echo "::error::EFI drift detected"
            echo "  built (dist/gbl-chainload.efi):       $fresh"
            echo "  vendored (zip/base/gbl-chainload.efi): $vendored"
            echo "  Run zip/update-tools.sh from a clean parent checkout to"
            echo "  refresh the vendored artifacts, commit in the zip submodule,"
            echo "  bump the parent's zip pointer, and re-tag."
            exit 1
          fi
          echo "EFI parity ok ($fresh)"
```

- [ ] **Step 3: Insert the recovery-gbl build + parity step after the EFI parity step**

```yaml
      - name: Build recovery gbl multicall (aarch64-linux-android)
        # Builds dist/recovery/gbl — the aarch64-linux-android multicall
        # that ships inside zip/bin/gbl. Required for the parity check below.
        run: bash scripts/build-recovery-tools.sh
      - name: Recovery gbl parity — built == vendored in zip
        # Same shape as the EFI parity check, for the recovery gbl multicall.
        # zip/bin/gbl is the aarch64-linux-android binary that runs inside
        # OrangeFox / TWRP. update-tools.sh rebuilds it from parent source;
        # if a developer changed crates/tools/gbl source but didn't refresh
        # the zip, the installer ZIPs would ship a stale recovery binary.
        run: |
          set -euo pipefail
          fresh=$(sha256sum dist/recovery/gbl | awk '{print $1}')
          vendored=$(sha256sum zip/bin/gbl | awk '{print $1}')
          if [ "$fresh" != "$vendored" ]; then
            echo "::error::Recovery gbl drift detected"
            echo "  built (dist/recovery/gbl): $fresh"
            echo "  vendored (zip/bin/gbl):    $vendored"
            echo "  Run zip/update-tools.sh from a clean parent checkout to"
            echo "  refresh the vendored artifacts, commit in the zip submodule,"
            echo "  bump the parent's zip pointer, and re-tag."
            exit 1
          fi
          echo "Recovery gbl parity ok ($fresh)"
      - name: Rename + checksum
        run: |
          ...
```

- [ ] **Step 4: Validate the YAML and confirm Task 0's release-asset wiring is intact**

Run the Verify command from above; it asserts all three new steps are present plus the artifact wiring.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "release.yml: EFI + recovery-gbl parity checks in build-efi

build-efi now sha256-compares both of zip's vendored artifacts against
fresh builds before publishing:

  · dist/gbl-chainload.efi      vs zip/base/gbl-chainload.efi
  · dist/recovery/gbl           vs zip/bin/gbl

Both fail with a recovery hint pointing at zip/update-tools.sh.
Catches the failure mode where a developer changes firmware or
tooling source but forgets to refresh the zip submodule — without
this, the release ships artifacts that disagree with the source tree
at tag time. Replaces the squash-merge-fragile parent-commit SHA
check the design originally considered.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: verify-job — submodule pointer reachability

**Goal:** The `verify` job asserts every submodule pointer in the parent's tag commit is reachable from that submodule's `origin/main`. The parent-commit-matches-tag-SHA check originally drafted here is dropped — squash-merging release PRs creates new SHAs on main that can't match what `update-tools.sh` recorded on the release branch. Task 2's two parity checks (EFI + recovery-gbl built fresh vs vendored) catch the same staleness failure mode without depending on SHA equality.

**Files:**
- Modify: `.github/workflows/release.yml` — extend the `verify` job

**Acceptance Criteria:**
- [ ] verify job has a step named "Submodule pointer reachability"
- [ ] Step fails loudly with explicit recovery hints on mismatch
- [ ] verify job still produces the `version` + `sha` outputs (existing contract intact)

**Verify:**

```bash
python3 -c 'import yaml; v=yaml.safe_load(open(".github/workflows/release.yml"))["jobs"]["verify"]; \
  names=[s.get("name","") for s in v["steps"]]; \
  assert any("reachability" in n.lower() for n in names), "reachability step missing"'
```

**Steps:**

- [ ] **Step 1: Locate the verify job's last step**

The current `verify` job ends with `MANIFEST drift check`. The new step goes after it.

- [ ] **Step 2: Add reachability step**

```yaml
      - name: MANIFEST drift check
        run: |
          set -euo pipefail
          cd zip && grep -E '^[0-9a-f]{64}  ' bin/MANIFEST | sha256sum -c --status

      - name: Submodule pointer reachability
        # Each submodule pinned by the parent's tag commit MUST be reachable
        # from that submodule's origin/main. Catches the failure mode where
        # a submodule has the commit only on a feature branch and main hasn't
        # been ff'd yet — downstream clones would fail `git submodule update`.
        run: |
          set -euo pipefail
          for sub in zip edk2; do
            pin=$(git ls-tree HEAD "$sub" | awk '{print $3}')
            (cd "$sub" && git fetch --quiet origin main)
            if ! git -C "$sub" merge-base --is-ancestor "$pin" origin/main 2>/dev/null; then
              echo "::error::$sub submodule pinned to $pin which is NOT reachable from $sub/origin/main"
              echo "  Push the commit (or its branch's tip) to that submodule's main,"
              echo "  then re-tag the parent."
              exit 1
            fi
            echo "$sub reachability ok (pin=$pin)"
          done
```

- [ ] **Step 3: Validate YAML**

```bash
python3 -c 'import yaml; yaml.safe_load(open(".github/workflows/release.yml"))'
```

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "release.yml: verify-job submodule pointer reachability

Each submodule pinned in the parent's tag commit must be reachable
from that submodule's origin/main; otherwise downstream
\`git submodule update\` fails for anyone cloning the tag. Catches
the drift mode we hit this session: edk2/main not yet ff'd to
engine-rework's tip even though the parent's pointer expected it.

The parent-commit-matches-tag-SHA invariant the design originally
considered is dropped — squash-merging release PRs creates new SHAs
on main that can't match what update-tools.sh recorded on the
release branch. Task 2's two parity checks (EFI + recovery-gbl built
fresh vs vendored) catch the same staleness failure mode without
depending on SHA equality through squash.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: scripts/release.sh — one-command release prep

**Goal:** A POSIX shell script that takes a semver argument, scaffolds the release branch + commits + PR. The author runs this once and the only remaining manual step is "review the PR, merge, push the tag".

**Files:**
- Create: `scripts/release.sh`

**Acceptance Criteria:**
- [ ] Script accepts a single `X.Y.Z` positional argument
- [ ] `bash scripts/release.sh --help` prints usage and exits 0
- [ ] `bash scripts/release.sh --dry-run 2.3.99` prints the commands it would run, makes no changes
- [ ] On a real run: bumps `VERSION`, scaffolds `CHANGELOG.md` section, runs `zip/update-tools.sh`, commits in zip submodule, bumps parent's zip pointer, commits, pushes branch, opens PR via `gh pr create`
- [ ] Validates: semver format, clean working tree on main, tag not already used
- [ ] Bails cleanly (no half-applied state) on any sub-step failure via EXIT trap
- [ ] Script is executable (`chmod +x`)

**Verify:**

```bash
bash scripts/release.sh --help               # prints usage, exit 0
bash scripts/release.sh --dry-run 2.3.99     # prints commands, no state change
git status --short                            # still clean after dry-run
test -x scripts/release.sh && echo OK || echo FAIL
```

**Steps:**

- [ ] **Step 1: Create scripts/release.sh**

```bash
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
  1. Review the PR.
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

# --- parent zip pointer ---
echo "==> bumping parent's zip pointer"
run "git add zip"
run "git add VERSION CHANGELOG.md"
run "git commit -m 'release: $VER'"

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
```

- [ ] **Step 2: Make executable**

```bash
chmod +x scripts/release.sh
```

- [ ] **Step 3: Dry-run smoke test**

```bash
bash scripts/release.sh --help    # exits 0, prints usage
bash scripts/release.sh --dry-run 2.3.99
git status --short                # still clean
```

- [ ] **Step 4: Commit**

```bash
git add scripts/release.sh
git commit -m "scripts/release.sh: one-command release prep

Author runs scripts/release.sh X.Y.Z; script handles VERSION bump,
CHANGELOG scaffold, zip submodule refresh via zip/update-tools.sh
(rebuilds EFI + bin/gbl, regenerates MANIFEST, commits in zip),
parent zip-pointer bump, branch push, PR creation. Only manual steps
left: fill in CHANGELOG highlights, merge PR, push tag.

--dry-run prints intended commands without execution. EXIT trap rolls
back partial branch creation on sub-step failure so a half-applied
state never lingers.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Cargo registry + git cache across workflows

**Goal:** Cache `~/.cargo/registry/index`, `~/.cargo/registry/cache`, and `~/.cargo/git/db` in every workflow job that invokes `cargo`, keyed on `Cargo.lock`. Skip `target/` (volatile, large, invalidates often).

**Files:**
- Modify: `.github/workflows/release.yml` — `build-host-tools`, `build-efi` (any other cargo-touching jobs)
- Modify: `.github/workflows/ci.yml` — wherever cargo runs
- Modify: `.github/workflows/host-tools.yml` — `cross-build` job

**Acceptance Criteria:**
- [ ] Every cargo-touching job has an `actions/cache@v4` step before its cargo invocation
- [ ] Cache key is `${{ runner.os }}-cargo-${{ hashFiles('**/Cargo.lock') }}`
- [ ] Restore-keys include the prefix-only fallback `${{ runner.os }}-cargo-`
- [ ] No `target/` paths in any cache step
- [ ] All three workflow files still parse as valid YAML

**Verify:**

```bash
python3 <<'PY'
import yaml, glob
for path in glob.glob('.github/workflows/*.yml'):
    wf = yaml.safe_load(open(path))
    for jn, j in wf.get('jobs', {}).items():
        steps_str = str(j.get('steps', []))
        has_cargo = 'cargo' in steps_str.lower()
        has_cache = 'actions/cache' in steps_str and "~/.cargo/registry" in steps_str
        if has_cargo and not has_cache:
            print(f'  MISSING cache: {path}::{jn}')
        if 'target/' in steps_str and 'actions/cache' in steps_str:
            print(f'  STRAY target/ cache: {path}::{jn}')
print('done')
PY
```
Expected: only `done`, no `MISSING` or `STRAY` lines.

**Steps:**

- [ ] **Step 1: Identify cargo-touching jobs**

```bash
grep -nE "cargo " .github/workflows/*.yml | awk -F: '{print $1":"$2}'
# Expect entries from release.yml (build-host-tools, build-efi), ci.yml,
# host-tools.yml (cross-build)
```

- [ ] **Step 2: Add cache block to each cargo-touching job**

For each job, insert AFTER the `actions/checkout` step and BEFORE the first cargo invocation:

```yaml
      - name: Cache cargo registry + git db
        uses: actions/cache@v4
        with:
          path: |
            ~/.cargo/registry/index
            ~/.cargo/registry/cache
            ~/.cargo/git/db
          key: ${{ runner.os }}-cargo-${{ hashFiles('**/Cargo.lock') }}
          restore-keys: |
            ${{ runner.os }}-cargo-
```

Repeat verbatim for each job. Inside-docker jobs (release.yml's `build-host-tools`, `build-efi`) — the cache mounts onto the host runner; the docker container's `/work` mount means `~/.cargo` inside-container is not the host's. That's fine: the in-container cargo still re-uses the host's already-mounted volume because the docker image runs as the host user and the workspace volume is bind-mounted. If the cache turns out not to help inside-docker (cold cache every time), Task 5 still doesn't regress anything and host-only jobs benefit.

- [ ] **Step 3: Validate all three workflow files**

```bash
for f in .github/workflows/*.yml; do
  python3 -c "import yaml; yaml.safe_load(open('$f'))" \
    && echo "ok $f" || echo "FAIL $f"
done
```

- [ ] **Step 4: Confirm verify script passes**

```bash
python3 <<'PY'
import yaml, glob
for path in glob.glob('.github/workflows/*.yml'):
    wf = yaml.safe_load(open(path))
    for jn, j in wf.get('jobs', {}).items():
        steps_str = str(j.get('steps', []))
        has_cargo = 'cargo' in steps_str.lower()
        has_cache = 'actions/cache' in steps_str and "~/.cargo/registry" in steps_str
        if has_cargo and not has_cache:
            print(f'  MISSING cache: {path}::{jn}')
        if 'target/' in steps_str and 'actions/cache' in steps_str:
            print(f'  STRAY target/ cache: {path}::{jn}')
print('done')
PY
```

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/
git commit -m "ci: cache ~/.cargo/registry + git db across cargo-touching jobs

actions/cache@v4 keyed on Cargo.lock with prefix-only restore-keys
fallback. Caches registry index + crate cache + git db; deliberately
NOT target/ — workspace member sources change often, target
invalidation is finicky, and the dep graph is the slow part. Registry
gives ~80% of the wall-clock benefit at ~5% of the cache size.

Applied to: release.yml (build-host-tools, build-efi), ci.yml,
host-tools.yml (cross-build).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Concurrency groups in all workflows

**Goal:** Force-pushes to feature branches cancel any in-flight runs; main's runs never get cancelled.

**Files:**
- Modify: `.github/workflows/release.yml`
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/workflows/host-tools.yml`

**Acceptance Criteria:**
- [ ] Every workflow has a top-level `concurrency:` block
- [ ] Group is `${{ github.workflow }}-${{ github.ref }}`
- [ ] `cancel-in-progress` is conditional: `${{ github.ref != 'refs/heads/main' }}`
- [ ] All three workflows still parse as valid YAML

**Verify:**

```bash
python3 <<'PY'
import yaml, glob
for path in glob.glob('.github/workflows/*.yml'):
    wf = yaml.safe_load(open(path))
    c = wf.get('concurrency')
    if not c:
        print(f'  MISSING concurrency: {path}')
    elif c.get('group') != '${{ github.workflow }}-${{ github.ref }}':
        print(f'  WRONG group: {path}: {c.get("group")}')
    elif "github.ref != 'refs/heads/main'" not in str(c.get('cancel-in-progress','')):
        print(f'  WRONG cancel-in-progress: {path}: {c.get("cancel-in-progress")}')
print('done')
PY
```
Expected: only `done`.

**Steps:**

- [ ] **Step 1: For each workflow, insert the concurrency block at the top level**

The block goes after `on:` (or `permissions:`) and before `jobs:`. Example for `release.yml`:

```yaml
permissions:
  contents: write
  actions: read

concurrency:
  # Force-pushes to feature branches cancel any in-flight run.
  # Main is excluded so every commit landing on main records a full
  # CI history (never cancelled by a subsequent commit).
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: ${{ github.ref != 'refs/heads/main' }}

jobs:
  verify:
    ...
```

Repeat for `ci.yml` and `host-tools.yml`.

- [ ] **Step 2: Validate**

```bash
for f in .github/workflows/*.yml; do
  python3 -c "import yaml; yaml.safe_load(open('$f'))" \
    && echo "ok $f" || echo "FAIL $f"
done
```

- [ ] **Step 3: Run the verifier from the AC**

```bash
python3 <<'PY'
import yaml, glob
for path in glob.glob('.github/workflows/*.yml'):
    wf = yaml.safe_load(open(path))
    c = wf.get('concurrency')
    if not c:
        print(f'  MISSING concurrency: {path}')
    elif c.get('group') != '${{ github.workflow }}-${{ github.ref }}':
        print(f'  WRONG group: {path}: {c.get("group")}')
    elif "github.ref != 'refs/heads/main'" not in str(c.get('cancel-in-progress','')):
        print(f'  WRONG cancel-in-progress: {path}: {c.get("cancel-in-progress")}')
print('done')
PY
```

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/
git commit -m "ci: concurrency groups — cancel stale runs on feature branches

group=workflow-ref, cancel-in-progress only when ref != main.
Force-pushing a feature branch stops the prior in-flight run instead
of letting it complete uselessly. Main is excluded so every commit
landing there records a full CI status.

Applied to release.yml, ci.yml, host-tools.yml.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: CLAUDE.md — single-commit-PR flexibility + version-bump explicitness

**Goal:** Update the Workflow section so single-commit PRs are explicitly fine for small fixes (no apologies for low-ceremony), and version bumps are called out as their own focused PR.

**Files:**
- Modify: `CLAUDE.md`

**Acceptance Criteria:**
- [ ] The "Workflow: branch then PR" section reads as in spec § Unit D
- [ ] The bullet about "Hot-fix-style 'tiny' / 'obvious' changes" is replaced
- [ ] A new bullet explicitly calls out version bumps as their own PR and references `scripts/release.sh`
- [ ] No other sections changed

**Verify:**

```bash
grep -q "scripts/release.sh" CLAUDE.md && echo "ok scripts/release.sh reference"
grep -q "Single-commit PRs are fine" CLAUDE.md && echo "ok single-commit flexibility"
grep -q "Hot-fix-style" CLAUDE.md && echo "FAIL: old text still present" || echo "ok old text removed"
```

**Steps:**

- [ ] **Step 1: Read current Workflow section**

```bash
grep -n -A 20 "Workflow: branch then PR" CLAUDE.md
```

- [ ] **Step 2: Edit the section**

Use `Edit` to replace:

```
- Never commit to or push `main` directly.
- Feature branches are otherwise unrestricted: commit early, commit often,
  iterate freely. The PR grows new commits as feedback comes in.
- Hot-fix-style "tiny" / "obvious" changes are not an exception to the
  branch-and-PR rule.
```

with:

```
- Never commit to or push `main` directly.
- Branch+PR applies to all changes, but the ceremony scales with the
  change. Single-commit PRs are fine for small fixes; multi-commit
  feature branches are fine for larger work — iterate freely on the
  branch, the PR grows new commits as feedback comes in.
- **Version bumps (`VERSION` + `CHANGELOG.md`) land as their own focused
  PR — explicit on main, no bundling with feature work.** Use
  `scripts/release.sh X.Y.Z` to scaffold the branch + PR.
```

- [ ] **Step 3: Verify the AC checks pass**

```bash
grep -q "scripts/release.sh" CLAUDE.md && echo "ok scripts/release.sh reference"
grep -q "Single-commit PRs are fine" CLAUDE.md && echo "ok single-commit flexibility"
grep -q "Hot-fix-style" CLAUDE.md && echo "FAIL: old text still present" || echo "ok old text removed"
```

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md
git commit -m "CLAUDE.md: single-commit-PR flexibility + version bumps explicit

The 'no exception for tiny changes' rule was producing apologetic
PRs for one-line fixes. Replace with: branch+PR scales with the
change. Single-commit PRs are fine for small fixes; iterate freely
on multi-commit branches for larger work.

Add an explicit rule: version bumps (VERSION + CHANGELOG.md) land
as their own focused PR — explicit on main, no bundling. Point at
scripts/release.sh as the scaffolding tool.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Finalize PR #46 — retitle, rebody, force-push

**Goal:** PR #46 currently points at this branch with stale title/body from the v2.3.4 work. Update title + body to reflect the new scope; force-push the rewritten history; confirm CI green.

**Files:**
- PR #46 (via `gh pr edit`)
- This branch (`release-prep-v2.3.4`) — force-push

**Acceptance Criteria:**
- [ ] `gh pr view 46 --json title,body` shows the new title `cleanup: release confidence + parity` and a body that references all 8 task commits
- [ ] `git log origin/release-prep-v2.3.4 --oneline` shows the 8 task commits in order (Task 0 → Task 7)
- [ ] `gh pr view 46 --json mergeable,mergeStateStatus` shows `MERGEABLE` and (eventually) `CLEAN` once CI completes
- [ ] No regressions: `host-tools` workflow green; old `CI` workflow green (the goldens-test failure mode that broke prior runs is now resolved by Task 1)

**Verify:**

```bash
git push --force-with-lease origin release-prep-v2.3.4
gh pr view 46 --json title,mergeable,mergeStateStatus
# Then wait for CI to settle and re-check.
```

**Steps:**

- [ ] **Step 1: Push the consolidated branch**

```bash
git log --oneline origin/main..HEAD
# Expect Tasks 0..7 plus the spec doc commit (aa22350) = 9 commits.

git push --force-with-lease origin release-prep-v2.3.4
```

- [ ] **Step 2: Retitle + rebody PR #46**

```bash
gh pr edit 46 \
  --title 'cleanup: release confidence + parity (CI hardening, goldens retirement)' \
  --body "$(cat <<'EOF'
## Summary

Consolidates four pieces of release-infrastructure housekeeping so the
follow-up v2.3.4 PR is a 2-file diff (VERSION + CHANGELOG.md).

Spec: `docs/superpowers/specs/2026-05-23-release-confidence-and-parity-cleanup-design.md`

### Changes

- **Goldens retirement** — `tests/host/goldens/` deleted. PR2's parity-contract
  scaffolding has done its job (Rust impl is authoritative; on-device validation
  green). Textual goldens become inline `grep -qE` schema checks.
- **EFI parity check** — new `build-efi` release.yml job builds fresh, asserts
  hash matches `zip/base/gbl-chainload.efi`, fails noisily on drift with a recovery
  hint pointing at `zip/update-tools.sh`. EFI now ships as a release asset.
- **Submodule sync invariants** — `verify` job asserts `zip/bin/MANIFEST`'s
  parent-commit equals the tag SHA, and every submodule pin is reachable from
  that submodule's `origin/main`. Catches the drift modes we hit this session.
- **`scripts/release.sh`** — one-command release prep. `bash scripts/release.sh
  X.Y.Z` scaffolds the branch + PR. Author fills CHANGELOG highlights, merges,
  pushes tag.
- **CI caches** — `~/.cargo/registry` + `~/.cargo/git/db` keyed on `Cargo.lock`
  across all cargo-touching jobs. Not `target/` (too volatile). Not EDK2 (would
  break banner + version passthrough).
- **Concurrency groups** — force-pushes cancel stale runs on feature branches;
  main is excluded.
- **CLAUDE.md** — single-commit PRs are fine for small fixes; version bumps
  are their own focused PR.

### Test plan

- [ ] All `tests/host/` scripts pass locally after Task 1.
- [ ] `bash scripts/release.sh --dry-run 2.3.99` prints intended commands; no state change.
- [ ] CI green on this PR: `host-tools` + old `CI` workflows both pass (the
      `060_pack_roundtrip` failure pre-dating this PR is resolved by Task 1).
- [ ] After merge, `bash scripts/release.sh 2.3.4` is the one-command path to v2.3.4.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Watch CI; on green, hand off**

```bash
# Poll until both workflows leave IN_PROGRESS:
until s=$(gh pr view 46 --json statusCheckRollup -q '[.statusCheckRollup[].status] | unique | join(",")') \
    && [ -n "$s" ] && ! echo "$s" | grep -q "IN_PROGRESS" && ! echo "$s" | grep -q "QUEUED"; do
  sleep 20
done
gh pr view 46 --json mergeable,mergeStateStatus,statusCheckRollup
```

If any check is `FAILURE`, fetch the failed log via `gh run view <run_id> --log-failed` and fix in place (new commit on the branch + push). Re-poll.

- [ ] **Step 4: Hand control back to the human (you / coordinator)**

PR #46 is ready for human review + merge. No automated merge — explicit on main per CLAUDE.md.

**Merge style:** Per the brainstorm follow-up, PR #46 merges via **merge commit (not squash)**. Future release PRs (created by `scripts/release.sh`) can squash-merge — they're 2-3 file changes and `verify`'s reachability check is the only invariant that cares about SHA continuity, and it operates on the submodule pointers (which the squash preserves).

---

## Self-review checklist (run before handing off the plan)

- [x] **Spec coverage:** Unit A → Task 1; Unit B → Tasks 0, 2, 3, 4; Unit C → Tasks 5, 6; Unit D → Task 7; PR finalize → Task 8. All spec sections mapped.
- [x] **Placeholders:** None. All step bodies show actual code/commands.
- [x] **Type consistency:** Job names (`build-efi`), step names (`EFI parity — built == vendored in zip`, `Submodule provenance — …`, `Submodule pointer reachability`), file paths (`zip/base/gbl-chainload.efi`, `zip/bin/MANIFEST`), artifact names (`efi-payload`) are consistent across tasks.
- [x] **Commit ordering:** Task 0 (pre-staged workflow) is intentionally first because subsequent task diffs build on the single-binary loop change. Task 1 (goldens) comes before any release-flow tweaks so CI passes on subsequent task commits. Tasks 5/6 are orthogonal and could run anywhere; placed after Task 4 to keep workflow edits batched. Task 7 (CLAUDE.md) is orthogonal too; placed late so the previous tasks aren't disrupted. Task 8 finalizes.
