# diag mode — pre-reboot EFISP install confidence + state capture

**Spec date:** 2026-05-19
**Branch:** `diag-confidence` (worktree at `../gbl-chainload-diag-confidence`)
**Status:** implemented in PR #32

## 1. Problem

After installing a gbl-chainload payload to EFISP via one of the
`mode-N-install` ZIPs, the operator has to reboot to find out whether
the install actually works. The current `zip/modes/diag.sh` reports the
recovery *environment* (slot, boot-mode, by-name presence, active
vbmeta descriptor list) but does **not** inspect what is on EFISP, the
loader-ABL state, the logfs history, or whether any chained partition
needs `graft` mode. The first reboot is therefore a leap.

This spec extends diag into a **no-write pre-reboot confidence check
and state collector**: it answers "is EFISP healthy enough to reboot
into?" with a single headline confidence tier, and it captures all
relevant raw state to a `/sdcard/` bundle for off-device review when
the on-screen summary is not enough.

## 2. Scope

In:

- Read-only inspection of EFISP, both `abl_*` slots, both `vbmeta_*`
  slots, the active vbmeta's chained-partition descriptors, and the
  `logfs` partition.
- A four-tier confidence verdict printed on-screen.
- A per-partition graft-required verdict.
- A single `/sdcard/gbl-chainload-diag-<ts>.tar.gz` archive (the
  working dir was originally co-located on `/sdcard/`; per the
  2026-05-20 amendment in §11, it now lives transiently on `/tmp/`
  and is removed once the tarball is sealed).

Out:

- Any partition write. diag stays no-op.
- Any `fastboot stage` / `oem boot-efi` orchestration. Recovery has no
  fastboot, and host-driven RAM-load is covered by
  `scripts/test-device-*.sh`, which this spec leaves alone.
- Any change to install modes, graft mode, or the EDK2 / on-device
  chainload code.
- Convergence with `test-device-manual.sh`'s output layout beyond the
  trivial naming overlap noted in §6.

## 3. Threat model / non-goals

This is not a cryptographic attestation. The signals here verify
*structural integrity and configuration*, not authenticity. A
sufficiently determined adversary who can write EFISP can also forge
any of the structural fields diag checks (GBLP1 magic, footer, header
CRC, payload SHAs). The intended user is the operator who just ran one
of *our* install ZIPs and wants to know whether *their own* install
worked before rebooting — not a defender trying to detect a malicious
EFISP rewrite.

## 4. On-screen output

A terse summary printed via `ui_print`. All detail goes to the bundle.

> **Amended 2026-05-20** — see §11 for rationale. Working dir now lives
> in `/tmp/` (recovery tmpfs) and is deleted once the tarball is in
> place; only the `.tar.gz` persists on `/sdcard/`. The `logfs history`
> UI line is gone; `logfs.img` is still in the bundle. EFISP now breaks
> out its GBLP1 entries on sub-lines. The old `graft needed` / `fakelock
> req` pair was collapsed (v2 correction in §11) into a single
> mode-aware `action req` line answering the operator's actual question:
> "is anything about this install going to stop normal boot?". New shape:

```
diag: pre-reboot install confidence
  EFISP        : mode-1 + GBLP1 v1 ok
                 - cached patched ABL: attached
                 - source metadata: attached
  loader-ABL   : abl_a retains loader path ; abl_b retains loader path
  action req   : none
  confidence   : HIGH — safe to reboot into chainload

  bundle saved : /sdcard/gbl-chainload-diag-20260520-203015.tar.gz
```

Note on terminology: the on-disk ABL is **always** OEM-signed
(XBL verifies it before handoff), so "stock vs patched" is the wrong
axis. The axis that matters is whether the OEM-signed ABL build that
happens to be on disk is one of the **vulnerable** builds that scans
EFISP for a PE and loads it (the "loader path"). The installer
deliberately writes a vulnerable build to the target slot. A
current/hardened OEM ABL on disk would not load EFISP at all,
regardless of what we put on EFISP — hence "WON'T LOAD EFISP" is the
operationally correct phrasing, never "stock".

### 4.1 Confidence tiers

The tiers are pre-reboot only. Because we have not yet booted from the
install, logfs is treated as a history channel, not a fresh-install
verifier.

| Tier   | Rule                                                                                                                                                |
|--------|-----------------------------------------------------------------------------------------------------------------------------------------------------|
| HIGH   | GBLP1 valid; base-EFI fingerprint matches a known mode-N hash from `zip/bin/MANIFEST`; at least one slot's ABL retains the loader path.             |
| MEDIUM | GBLP1 valid; base-EFI fingerprint matches; neither slot's ABL retains the loader path, so EFISP will not be loaded on reboot.                    |
| LOW    | EFISP holds a PE but GBLP1 is missing, the header CRC fails, or any entry's SHA-256 mismatches.                                                     |
| NONE   | EFISP does not start with `MZ`, or is empty/unreadable.                                                                                             |

The `loader-ABL` line is informational and does not alter the tier by
itself except as above. (The original `logfs history` UI line was
removed in the 2026-05-20 amendment — see §11; `logfs.img` is still
captured into the bundle.)

### 4.2 `action req` verdict (mode-aware)

> Heavily rewritten in the 2026-05-20 v2 correction (§11). The previous
> `graft needed` / `fakelock req` pair was collapsed into a single
> mode-aware `action req` line keyed on `BASE_EFI_MODE`, and the
> underlying chain-partition check was rewritten to walk the AvbFooter
> instead of scanning a tail window for OEM-key-matched AVB0 magic.
> Raw per-partition rows still live in `graft-verdict.txt`.

The check, per descriptor in the active vbmeta, mirrors what
AOSP first-stage init's libavb actually does (see
`docs/project/vbmeta-graft-vs-construct.md` §2b):

- **Chain descriptor.** Open the chained partition, read the
  `AvbFooter` from the last 64 bytes, follow `VbmetaOffset` to the
  embedded vbmeta blob, parse the header, and compare the embedded
  public key to the chain descriptor's pubkey. Three possible outcomes:
  - `graft=ok` — init's libavb will accept it.
  - `graft=key_mismatch` — vbmeta exists but the key won't verify
    against the parent chain descriptor → init's sig-verify rejects.
  - `graft=no_vbmeta` — no `AvbFooter` (or footer points at
    non-vbmeta bytes) → init returns `ok_not_signed`.
- **Hash descriptor.** Compute `SHA-256(salt || partition_bytes[0..image_size))`
  and compare to the descriptor's `digest` field. Two outcomes:
  `digest=ok` or `digest=mismatch`.

Then bucketed per the **per-mode boot-blocker matrix**, derived from
the actual AVB verify flow:

| Bucket → mode    | `mode-0` / `mode-2` | `mode-1` |
|------------------|---------------------|----------|
| Chain `graft=ok` | fine                | fine     |
| Chain `graft=key_mismatch` | tolerated (orange-state) | **blocker** |
| Chain `graft=no_vbmeta`    | tolerated (orange-state) | **blocker** (init `ok_not_signed`) |
| Hash `digest=ok`           | fine                | fine     |
| Hash `digest=mismatch`     | tolerated (orange-state) | tolerated (`patch10` + init's locked-state skim) |

Rationale by mode:

- **mode-0 and mode-2** both keep ABL honest about the real unlocked
  state, so libavb's `allow_verification_error=true` lets AVB return
  orange-state on any mismatch and ABL boots regardless. mode-0 stops
  there (debug-observation build; no KM rewrite, so attestation will
  be red); mode-2 additionally rewrites the KM/SPSS RoT payload at
  the TA boundary for a coherent green attestation downstream.
  Neither has any AVB-related boot blocker — both collapse to a
  single "always `none`" UI bucket from diag's point of view.
- **mode-1** has a libavb patch (`patch10`) that forces ABL-side
  AVB to return success. But AOSP first-stage init runs a fresh
  unpatched libavb instance and re-verifies the on-disk vbmeta;
  `patch10` does not reach it. Therefore chain partitions must have
  an OEM-signed vbmeta blob on disk (the graft), or init aborts.
  Content-hash mismatches inside that vbmeta are tolerated because
  the green/locked DeviceInfo that mode-1 fakes makes init treat
  the descriptor walk as a skim (see
  `docs/project/vbmeta-graft-vs-construct.md` §2b).

UI rendering of the `action req` line:

| `BASE_EFI_MODE`   | When clean                            | When dirty                                         |
|-------------------|---------------------------------------|----------------------------------------------------|
| `mode-0`          | `none`                                | `none` (always — orange-state tolerates)           |
| `mode-2`          | `none`                                | `none` (always — orange-state tolerates)           |
| `mode-1`          | `none`                                | `graft <chain-broken list>`                        |
| unknown           | `none (mode unknown — assumed mode-1)`| `graft <chain> (mode unknown — assumed mode-1)`    |
| no active vbmeta  | `unknown (no active vbmeta)`          | same                                               |

Unknown framing intentionally adopts mode-1 semantics (the most common
pre-`zip/bin/MANIFEST`-fix install) so the operator gets actionable
info; the `(mode unknown — assumed mode-1)` suffix is the disclaimer.

## 5. Bundle layout

> Updated 2026-05-20 (§11): the staging directory was originally
> persisted at `/sdcard/gbl-chainload-diag-<ts>/` alongside the
> tarball. It now lives at `$BUNDLE_WORKDIR/gbl-chainload-diag-<ts>/`
> (default `/tmp/` — recovery tmpfs, lost on reboot) and is removed
> once the `.tar.gz` is sealed; the only persistent artifact on
> `$BUNDLE_ROOT` (default `/sdcard/`) is the tarball.

A single `/sdcard/gbl-chainload-diag-<ts>.tar.gz`, archived with
busybox `tar` + `gzip` (both reliably present in TWRP/OrangeFox;
literal `.zip` would require bundling a static `zip` binary and is
not worth it). Layout once extracted:

```
gbl-chainload-diag-<ts>/
├── report.txt                # ui_print stream, tee'd live
├── env.txt                   # slot, BOOTMODE, by-name listing, busybox -v
├── getprop.boot.txt          # `getprop`, full dump  (naming aligns with test-device-manual.sh)
├── efisp.img                 # raw dd of efisp partition
├── abl_a.img                 # raw dd
├── abl_b.img                 # raw dd
├── vbmeta_a.img              # raw dd
├── vbmeta_b.img              # raw dd
├── logfs.img                 # raw dd of logfs partition
├── gblp1-inspect.txt         # output of new gblp1-inspect tool on efisp.img
├── loader-abl.txt            # fv-unwrap + UTF-16-LE "efisp" scan results per slot
├── vbmeta-descriptors.txt    # `vbmeta-graft list` of active vbmeta
└── graft-verdict.txt         # per-partition match/mismatch from new vbmeta-graft list-hash
```

Raw `dd` is chosen for logfs over mount-and-tar: the canoe BDS keeps
its own SimpleFS view open and mounting from recovery has historically
been unreliable. A raw image is bit-exact and trivially loopback-
mountable on the host.

## 6. Layout-naming overlap with test-device-manual.sh

`scripts/test-device-manual.sh` is the host-driven adb-pulled-state
collector. It captures different things (`bootloader_log`, `dmesg.txt`,
`device-tree.tar`, etc.) but where filenames *do* overlap, diag uses
the same name so a future unified off-device analyzer can ingest
either layout:

- `getprop.boot.txt` (not `props.txt`).
- `logfs/` directory is **not** produced by diag — diag uses a single
  `logfs.img` raw partition image instead. Both forms are equally
  loopback-mountable on the host, so this is not a true conflict.

No edits to `test-device-manual.sh` itself. Full layout convergence is
deliberately deferred to a future spec if a unified analyzer is ever
written.

## 7. Components

### 7.1 `zip/modes/diag.sh` rewrite

Grows from ~60 to ~200 lines, broken into helpers that each:

1. Run their check.
2. Tee both human-readable summary to `ui_print` *and* a longer raw
   dump to a file in the bundle directory.

Functions:

- `prepare_bundle` — make `$BUNDLE_WORKDIR/gbl-chainload-diag-<ts>/`
  (default `/tmp/…`; see §11 amendment) and set the `$BUNDLE_DIR`
  env. Redefine `ui_print` locally to tee to
  `$BUNDLE_DIR/report.txt`.
- `collect_env` — write `env.txt`, `getprop.boot.txt`.
- `collect_efisp` — `dd` EFISP to `$BUNDLE/efisp.img`. Quick PE check
  (`MZ` first 2 bytes). Run `gblp1-inspect` against the image, capture
  to `gblp1-inspect.txt`. Fingerprint the base-EFI region (bytes
  before GBLP1) by SHA-256 and match against the three hashes in
  `zip/bin/MANIFEST`. Result becomes the EFISP summary line.
- `collect_abl` — for each slot: `dd` to `abl_<slot>.img`, `fv-unwrap`
  to a tmp PE, scan for the 10-byte UTF-16 LE `efisp` signature from
  `tools/shared/patch_signatures.h`. Presence of the signature means
  the OEM-signed ABL on disk is a vulnerable build that retains the
  EFISP loader path; absence means EFISP will not be loaded from that
  slot. Result becomes the loader-ABL summary line. Writes detail to
  `loader-abl.txt`.
- `collect_vbmeta` — for each slot: `dd` to `vbmeta_<slot>.img`. Run
  `vbmeta-graft list` against the active slot's image, capture to
  `vbmeta-descriptors.txt`.
- `collect_logfs` — resolve logfs partition by GPT label, `dd` to
  `logfs.img`. Then scan the raw image for `GblChainload_Boot` ASCII
  occurrences (a FAT directory entry will store the filename in plain
  ASCII; a busybox `grep -aoE 'GblChainload_Boot[0-9]+\.txt'` over the
  raw image gives a usable count without mounting). Newest is the one
  with the highest trailing integer.
- `check_graft` — invoke the new `vbmeta-graft list-hash`
  subcommand against the active vbmeta and the by-name dir. Bucket
  the per-partition `verdict=mismatch` rows into two groups by their
  graft state: `graft=missing` rows go in the "graft mode required"
  list; `graft=n/a` (plain-hash mismatch) rows go in the "needs
  fakelock / mode-1" list. Both lists feed the summary lines. Writes
  full detail to `graft-verdict.txt`.
- `decide_tier` — pure function of the four already-computed booleans
  (efisp_pe_present, gblp1_valid, base_efi_matches_mode_n,
  any_slot_loader_path). Prints the headline.
- `finalize_bundle` — `cd /sdcard && tar -czf
  gbl-chainload-diag-<ts>.tar.gz gbl-chainload-diag-<ts>/`. Print both
  paths.

### 7.2 `zip/modes/diag.conf`

```sh
MODE_NAME="diag"
MODE_DESC="pre-reboot install-confidence diagnostic (no writes); writes a bundle to /sdcard/"
MODE_WRITES=""        # no partition writes
MODE_TOOLS="vbmeta-graft fv-unwrap gblp1-inspect"
MODE_EFI=""
MODE_LIB=""
```

The previously empty `MODE_TOOLS` line is filled in so
`build-recovery-zip.sh` packages the tools into `bin/`.

### 7.3 New host tool: `tools/gblp1-inspect/`

Pure-C, mirrors `tools/gbl-pack` build conventions (host + aarch64-
Android static). Reuses `tools/shared/gblp1.h`. Signature:

```
gblp1-inspect <efisp.img>
```

Output (one entry per line, machine-greppable, plus a summary at the
end):

```
header: magic=ok version=1 header_crc32=ok total_size=4096 entry_count=3
entry: type=0x0001 (CACHED_ABL)    offset=0x80   size=2048 sha256=ok
entry: type=0x0002 (SOURCE_META)   offset=0x880  size=64   sha256=ok
entry: type=0x0010 (MODE2_PROFILE) offset=0x900  size=120  sha256=ok
footer: GBLP1END=ok
result: ok
```

Failure modes emit one terminal line: `result: bad_magic` /
`bad_crc` / `entry_sha_mismatch` / `truncated` / `not_a_gblp1` /
`empty`. Exit code is 0 only on `result: ok`.

The packer adds a base-EFI region in front of the GBLP1 container; the
tool finds the start of GBLP1 by scanning for the magic, then
validates from there. Bytes before the magic are not validated by this
tool — that is done by the diag script's separate
fingerprint-vs-MANIFEST step.

### 7.4 New `vbmeta-graft` subcommand: `list-hash`

```
vbmeta-graft list-hash <active-vbmeta.img> <byname-dir>
```

For every descriptor in `<active-vbmeta.img>`: resolve
`<byname-dir>/<part_name>_<slot>` (slot derived from the vbmeta
filename) and produce two independent results.

**1. Raw digest check** (for hash descriptors only). Compute the
digest exactly as `libavb` does — `SHA-256(salt || image_bytes[0 ..
image_size))` where `salt` and `image_size` come from the descriptor
— and compare to the descriptor's `digest` field. Reported as
`digest=ok` / `digest=mismatch`. Chain descriptors report `digest=n/a`
(no hash to check at this layer; the work is delegated).

**2. Graft probe** (for any partition). Scan the on-disk partition
for a valid AVB vbmeta blob at the graft-natural offset
(`round_up(custom_content_size, 4K)`) and validate its signature
against the OEM public key embedded in the candidate's auxiliary
block. Reported as `graft=ok` (a stock-OEM-signed vbmeta lives at
the natural offset — AOSP init will accept this partition via the
active vbmeta's chain descriptor), `graft=missing` (chain descriptor
exists but no valid graft), or `graft=n/a` (descriptor is a plain
hash; the graft mechanism does not apply here).

**Verdict** (the diag-consumable column) is the boot-pass-fail
answer, derived from the two results above:

- Hash descriptor + `digest=ok` → `verdict=match`.
- Hash descriptor + `digest=mismatch` → `verdict=mismatch` (graft
  mode cannot fix a direct hash descriptor; this is the mode-1
  fakelock's responsibility).
- Chain descriptor + `graft=ok` → `verdict=match` (AOSP init follows
  the chain, lands on the grafted OEM-signed vbmeta, accepts).
- Chain descriptor + `graft=missing` → `verdict=mismatch` (graft
  mode required for this partition).

Output, one line per partition:

```
partition=system        type=hash  declared=8589934592 digest=ok       graft=n/a    verdict=match
partition=vbmeta_system type=chain declared=-          digest=n/a      graft=ok     verdict=match
partition=recovery      type=chain declared=-          digest=n/a      graft=missing verdict=mismatch
partition=vendor        type=hash  declared=805306368  digest=mismatch graft=n/a    verdict=mismatch
```

The graft-needed tally in the diag summary counts partitions where
`verdict=mismatch` **and** `graft=missing` (i.e. graft mode can
actually help). Plain-hash mismatches are reported on a separate
line as "needs fakelock / mode-1" since `graft` mode will not change
their outcome.

Reuses the existing descriptor walker; ~120 added lines of C
(descriptor walk + graft probe + dual-column emit).

Output, one line per partition:

```
partition=system   type=hash    declared=8589934592 digest=ok   verdict=match
partition=recovery type=hash    declared=104857600  digest=mismatch verdict=mismatch
partition=vendor   type=hash    declared=805306368  digest=mismatch verdict=mismatch
```

`verdict` is the diag-consumable column. Reuses the existing
descriptor walker; ~60 added lines of C.

### 7.5 Build / packaging

- Both new artifacts (`gblp1-inspect`, the extended `vbmeta-graft`)
  build via the existing `scripts/build-recovery-tools.sh` Docker +
  NDK r27 flow. No new build infrastructure.
- `zip/update-tools.sh` refreshes `zip/bin/` and rewrites
  `zip/bin/MANIFEST`. Parent commit bumps the submodule pointer.
- `scripts/build-recovery-zip.sh --mode diag` produces the updated
  ZIP at `dist/gbl-chainload-diag.zip`.

## 8. Testing

Host-side, additive, no device required:

- `tests/host/084_gblp1_inspect.sh` — pack a known overlay via
  `gbl-pack`, run `gblp1-inspect`, assert mode/entries/SHAs and exit
  code; corrupt one entry's SHA and assert the failure line.
- `tests/host/085_vbmeta_descriptor_hash.sh` — using existing
  `images/grafted-recovery.img`-style fixtures (or a small synthetic
  pair built inside the test), assert `match` for an in-spec image and
  `mismatch` for a perturbed one.
- `tests/host/086_diag_dryrun.sh` — drive `diag.sh` against a fake
  `BYNAME` tree built from `dd if=/dev/zero` + the pack output above.
  Stub `ui_print` to print to stdout. Assert: bundle dir created, all
  expected files present, headline confidence tier matches the
  scripted scenario for each tier (HIGH / MEDIUM / LOW / NONE), exit
  code 0 in every tier.

`tests/runall.sh` picks these up automatically because it globs the
`tests/host/0*_*.sh` set.

No on-device test required for landing; the mode is no-write. A
post-merge sanity flash is reasonable but not gated by this spec.

## 9. Risks

- **Bundle size.** `efisp.img` is typically small (~1 MiB), `abl_*`
  ~8 MiB each, `vbmeta_*` ~64 KiB each, `logfs.img` partition size
  (typically <= a few MiB on canoe). Total uncompressed should be
  comfortably under 50 MiB; `tar -czf` reduces this. `/sdcard/` free
  space is not gated by diag (no writes elsewhere) but the script will
  abort early with a clear message if `df` shows less than 100 MiB
  free.
- **busybox tar/gzip presence.** Methodology doc claims both are
  reliably present in our target recoveries. If a recovery is missing
  `gzip`, fall back to plain `tar` (uncompressed) and warn.
- **logfs filename scan via raw grep.** FAT short-name vs long-name
  encoding may cause a long filename to appear with embedded NULs in
  the raw image. The grep uses `-a` and a regex that tolerates this
  on the common case; if a future canoe logfs uses a different FS, the
  fallback is "unknown — see logfs.img" — non-fatal.
- **Loader-path scan false negatives on heavily-customised ABLs.**
  The 10-byte UTF-16 LE pattern is the same one the on-device patcher
  uses; if it does not appear, our chainload won't run regardless, so
  a negative is correct by construction.

## 10. PR plumbing

- Worktree: `../gbl-chainload-diag-confidence` on branch
  `diag-confidence` off `main`. Already created.
- `zip/` is a submodule. Sequence: edit + commit in `zip/`, then
  parent commit bumps the submodule pointer in the same PR.
- New tools live in `tools/gblp1-inspect/` (parent repo, not the
  submodule).
- PR target: `main`.
- All host tests must be green before opening the PR.
- No device test required to land.

## 11. Amendment 2026-05-20 — UI cleanup

After the first real on-device run of the diag mode (mode-2 ZIP on
infiniti, 2026-05-20 15:00 UTC), the operator's feedback identified
four issues with the original on-screen output:

1. **Storage hygiene.** The working bundle dir was being left on
   `/sdcard/` alongside the `.tar.gz`. The dir is duplicate data —
   the tar is the artifact. The dir now lives at
   `$BUNDLE_WORKDIR/gbl-chainload-diag-<ts>` (default `/tmp/`,
   recovery tmpfs) and is `rm -rf`'d once the tarball is in place.
   Only the `.tar.gz` lands on `$BUNDLE_ROOT` (default `/sdcard/`).
   `finalize_bundle` only skips the cleanup if `tar` itself failed
   outright; on the gzip-absent fallback it still removes the dir
   (the plain `.tar` has the data).

2. **`logfs history` UI line was noise.** Operators consult the
   uefilog rotation files off-device; the on-screen "N prior boots"
   tally is not actionable pre-reboot. The line is removed; the raw
   `logfs.img` is still in the bundle.

3. **GBLP1 entries collapsed onto one line.** The EFISP headline
   used to read `EFISP : mode-X + GBLP1 v1 ok (3 entries, all
   sha-verified)`. The operator wants to see *which* entries are
   present at a glance. The headline now breaks each entry out on a
   sub-line keyed off the type-name in `gblp1-inspect`'s output:
   `CACHED_ABL → cached patched ABL`, `SOURCE_META → source
   metadata`, `MODE2_PROFILE → mode-2 profile`.

4. **`graft needed : YES` was misleading on mode-2.** On a
   Magisk-patched mode-2 device, boot/dtbo/recovery legitimately
   have no OEM-keyed vbmeta footer, but the operator's actual boot
   path works fine because mode-2 keeps ABL honest (orange state)
   and AVB tolerates the missing chain-vbmetas under that path.
   The original "needs graft / needs fakelock" verdicts answered
   "what would *unaided* stock boot need" — which is not useful
   to an operator who has just installed mode-2. The verdict is now
   mode-aware (see §4.2 table); the raw rows are still in
   `graft-verdict.txt` for anyone who wants the full data. The
   literal value "NO" is renamed to "none" for legibility, and
   reserved for "clean"; "unknown" continues to mean "no active
   vbmeta was readable".

Implementation: `zip/modes/diag.sh` and the host dryrun test
(`tests/host/086_diag_dryrun.sh`) only. No tool changes — the
underlying `vbmeta-graft list-hash` output format is unchanged;
the script just buckets and renders it differently.

### 11.1 v2 correction — same day, operator's second pass

The first on-device run with the v1 changes revealed the bucketing
described above had the mode-1 capability matrix backwards. The
operator (mode-1 + Magisk-patched boot + stock-vbmeta-grafted custom
recovery) saw `graft needed : boot dtbo recovery` and pushed back: on
their device, recovery has been grafted from stock, boot/dtbo are
stock-equivalent, and the device boots fine. The diag was wrong on
two axes.

**Axis 1 — capability matrix.** v1 limited mode-aware suppression to
mode-2 on the assumption that mode-1's fakelock targets only
DeviceInfo and is "downstream of AVB hash verification". That's only
half right. Mode-1 also ships `patch10` (a libavb-side patch — see
`docs/project/re-findings.md`) that forces ABL-stage AVB to return
success regardless of what the on-disk vbmeta says. The boot-blocker
under mode-1 is **not** descriptor mismatches; it's AOSP first-stage
init's **userspace re-verify** of on-disk vbmeta — which `patch10`
cannot reach (see `docs/project/vbmeta-graft-vs-construct.md` §2b).
That re-verify is satisfied by an **OEM-signed vbmeta blob** being
present on the partition (the graft); content-hash mismatches are
tolerated by init's locked-state skim, but `ok_not_signed` (no vbmeta
at all) or a sig mismatch aborts boot.

The corrected per-mode boot-blocker matrix lives in §4.2.

**Axis 2 — the chain-graft check itself was buggy.** `probe_graft` in
`vbmeta-graft list-hash` scanned only the **last 4 MiB** of the
partition for AVB0 magic. A stock-vbmeta graft for custom recovery
sits at `round_up(custom_content_size, 4K)` — for ~60 MiB of custom
recovery in a 100 MiB partition, that's ~37 MiB, well outside any
tail window. The graft was there; the probe couldn't see it.

Fix: replace `probe_graft` with `probe_partition_for_graft`, which
does what libavb actually does — reads the `AvbFooter` from the last
64 bytes, follows `VbmetaOffset` to the embedded vbmeta blob, parses
the header, compares the pubkey to the chain descriptor's key.
Three buckets: `graft=ok | key_mismatch | no_vbmeta`. Verified against
the `tests/images/grafted-recovery.img` fixture: now correctly
reports `graft=ok verdict=match` where v1 reported `graft=missing`.

**UI consequence — collapse to one `action req` line.** With the
matrix corrected, the old "graft needed" / "fakelock req" naming was
misleading on mode-1 (where "fakelock" is what mode-1 *does*, not
what's required of the operator) and noisy on mode-2 (always two
`none`s). The two lines collapsed into a single `action req` whose
content is keyed off the mode — `none` for mode-2 always, `graft
<list>` for mode-1 when init would reject the chain, `graft …; hash …`
for mode-0 when anything mismatches at all. The §4.2 table is the
authoritative reference.

Implementation touches:

- `tools/vbmeta-graft/vbmeta-graft.c` — new
  `probe_partition_for_graft`; the legacy buffer-taking `probe_graft`
  and `find_avb0` retired. New `graft=` values emitted by the
  `list-hash` chain rows: `ok | key_mismatch | no_vbmeta`.
- `zip/modes/diag.sh` — single `action req` line; bucketing keys off
  `type=chain` + new `graft=` values for chain rows, and `type=hash`
  + `digest=mismatch` for hash rows.
- `tests/host/086_diag_dryrun.sh` — guards updated for the new line
  shape, with negative guards for legacy `graft needed` / `fakelock
  req` strings to prevent silent re-introduction.
- Regression tests `074_vbmeta_graft.sh` and
  `085_vbmeta_descriptor_hash.sh` still pass on the corrected tool.

Open follow-up (out of scope for this PR): on-device validation that
the new `action req` line matches reality on a mode-1 + grafted
recovery setup. The host fixture exercises the footer-walk against
the grafted-recovery image and confirms `graft=ok`, but the
on-device report shape needs an operator pass.
