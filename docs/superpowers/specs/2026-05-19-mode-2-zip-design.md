# Mode-2 ZIP design (slice 5)

Status: design approved 2026-05-19.

Parent design: `docs/superpowers/specs/2026-05-17-mode-2-design.md` (§5/§6) and
`docs/superpowers/specs/2026-05-18-mode-2-profile-tooling-design.md`. The
mode-2 EFI runtime and the host profile tooling already shipped (PRs #28, #29).
This is the recovery-flashable installer that delivers mode-2 to a device.

## 1. Goal & scope

Build out the `mode-2-install` mode of the `zip/` framework into a
recovery-flashable, **self-contained per-OTA** mode-2 installer. On flash it
detects the OEM from `build.prop`, derives the mode-2 profile on-device from the
stock vbmeta, builds an OEM-patched cached ABL, packs the
`cached_abl + mode2_profile` GBLP1 overlay onto the mode-2 EFI, and writes EFISP
plus the loader ABL.

The two install modes (`install` → `mode-1-install`, `profile` →
`mode-2-install`) are restructured into **three parallel install modes** —
`mode-0-install`, `mode-1-install`, `mode-2-install` — that share a common body
(`zip/modes/install-common.sh`) and differ only in the base EFI, the
`abl-patcher` flags, and (mode-2 only) the profile derive/compile step.

In scope: a multi-platform C `mode2-profile` tool, `abl-patcher` OEM patchsets,
the `mode-2-install` mode script, a shared-install-machinery refactor, the
bundled `zip/base/mode-2.efi`.

Out of scope: Windows tool builds (the deferred slice-4 "solidify host tools"
PR); non-OnePlus OEMs (the mechanism is extensible; v1 populates OnePlus only);
the `images/`-drop host orchestration (slice 4).

## 2. Components

### a. Profile tooling — TOML, two-step, C + Python

The profile intermediate is a flat **`.toml`** file (§3 schema). Two
implementations live in `tools/mode2-profile/`, both producing the identical
`.toml` and the identical 120-byte `gbl_mode2_profile` binary
(`tools/shared/gbl_mode2_profile.h`, from PR #28):

- **C tool (`mode2-profile`)** — the portable, shippable implementation.
  Multi-target Makefile: `make` → linux host binary; `make android` →
  aarch64-static binary bundled in `zip/bin/`. (Windows is a slice-4
  follow-up.) Two subcommands:
  - `derive <vbmeta.img> -o <profile.toml>` — a plain-C AVB vbmeta reader
    (256-byte header, authentication + auxiliary blocks, public-key blob,
    property descriptors) computes `rot_digest = SHA256(pubkey ‖ 0x00)`,
    `pubkey_digest = SHA256(pubkey)`, `vbh = SHA256(header+auth+aux)`, and the
    bootloader-domain `system_version`/`system_spl` encodings, then writes the
    `.toml`.
  - `compile <profile.toml> -o <profile.bin>` — reads the `.toml` through the
    vendored **`tomlc99`** library (`toml.c` + `toml.h`, MIT, statically
    linked), validates it (§5), and packs the 120-byte binary.
- **Python tool (`mode2-profile.py`)** — the existing host tool (PR #29),
  migrated **XML → TOML**: `derive` writes `.toml`; `compile` reads it via
  `tomllib` (the Python 3.11+ stdlib module — the tool requires Python
  3.11+). Kept as the fast dev-iteration path — edit-and-run, no compile
  cycle. Not shipped in the ZIP.

A host test cross-checks the C tool's output against the Python tool's,
byte-for-byte, on a real vbmeta fixture — that is what keeps the two
implementations honest.

### b. `abl-patcher` OEM patchsets

`abl-patcher`'s compiled-in patches are organised into three scopes (§4):
`universal`, `mode_1`, and per-OEM `oem/<id>`. A new `--oem <id>` flag selects
the OEM group. v1 adds the `oneplus` group. New OEM = C code + a rebuild of the
bundled aarch64 binary.

### c. `mode-2-install` mode

`zip/modes/mode-2-install.{sh,conf}`:

- `mode-2-install.conf`: `MODE_NAME=mode-2-install`, `MODE_DESC` updated,
  `MODE_WRITES="efisp abl"`, `MODE_EFI="mode-2.efi"`,
  `MODE_TOOLS="fv-unwrap abl-patcher gbl-pack gbl-commit mode2-profile"`.
- `mode-2-install.sh`: a thin wrapper that sources `modes/install-common.sh`,
  declares the mode-2 parameters, and overrides the `mode_preflight` /
  `mode_prepare` hooks to add the stock-vbmeta gate and the OEM-detect +
  derive/compile step. The shared orchestration is in §3.

### d. Shared install machinery — three parallel install modes

The two install modes are restructured into **three parallel install modes**:
`mode-0-install` (honest), `mode-1-install` (was `install`), `mode-2-install`
(was `profile`). They share ~90% of their body, factored into
`zip/modes/install-common.sh` — a mode-specific shared lib (it lives in
`modes/`, not `core/`, because an install-mode body is gbl-chainload-specific
whereas `core/` is generic framework infra). `install-common.sh` defines
`preflight`, `build_payload`, `commit_efisp`, `mode_main`, plus two no-op hooks
(`mode_preflight`, `mode_prepare`) a mode may override. Each
`modes/mode-N-install.sh` is a thin file that sources `install-common.sh` and
declares parameters (`M_EFI`, `M_PATCHER_ARGS`, `M_PACK_ARGS`, `M_LABEL`,
`M_WANT_PROFILE`). The truly generic loader-ABL logic — `pick_scenario`,
`resolve_restore_source`, `restore_abl`, `save_backup_abl` — remains in
`zip/core/install_abl.sh`. This is a behaviour-preserving refactor of
`install.sh`/`profile.sh`; the install-mode tests pass unchanged.

### e. Bundled `zip/base/mode-2.efi`

A built mode-2 EFI, vendored at `zip/base/mode-2.efi` like the existing
`base/mode-1.efi`, produced by `zip/update-tools.sh`.

## 3. The `mode-2-install` mode flow

```
preflight      /sdcard/stock_vbmeta.img REQUIRED (else abort); slot resolved;
               abl_<target> / efisp readable; EFISP currently holds a PE.
detect OEM     mount system/vendor read-only, read build.prop ro.product.*
               -> OEM id (+ device); unknown OEM -> abort, quoting the props seen.
derive         mode2-profile derive /sdcard/stock_vbmeta.img
                 -> /sdcard/gbl-chainload_profile.toml   (saved for inspection;
                    a pre-existing edited .toml there is honored over re-derive)
compile        mode2-profile compile <profile.toml> -> profile.bin (120 B)
cache ABL      dd abl_<target> -> fv-unwrap -> abl-patcher --oem <id> -> patched.efi
pack overlay   gbl-pack --cached-abl patched.efi --mode2-profile profile.bin
                 -> overlay.bin   (the cached_abl + mode2_profile ec=3 container)
concat         base/mode-2.efi + overlay.bin -> installed.efi
commit EFISP   commit_verified installed.efi -> efisp   (backup + verify)
restore ABL    loader-ABL-to-slot via the shared install machinery (backup + verify)
```

Scenario selection (OTA vs reinstall → target slot) and the loader-ABL source
come from the shared machinery (§2d), identical to `mode-0-install` /
`mode-1-install`. The
mode-2-specific steps are: *detect OEM*, *derive*, *compile*, the `--oem` patch
flag, the `--mode2-profile` pack flag, and the `mode-2.efi` base.

## 4. `abl-patcher` patch taxonomy

Three patch scopes:

- **`universal`** — always applied. The efisp-recursion guard, TZ soft-fuse
  drop, and OEM-survival patches every mode needs.
- **`mode_1`** — mode-1 fakelock mechanics (`patch6` lock-state gate, `patch10`
  libavb force-OK). Applied only for mode-1 installs.
- **`oem/<id>`** — OEM-specific patches, selected by `--oem <id>`.

Selection: the mode-2 cached ABL gets **`universal` + `oem/<id>`** (via
`abl-patcher --oem <id>`); the `mode-1-install` ZIP gets
`universal + mode_1 + oem/<id>`. So a mode-2 cached ABL no longer carries
mode-1's fakelock patches — it does not need them, and that was a flagged loose
end from the empty-payload on-device test. No `mode_2` patch group exists in v1
(the DICE-mode patch stays deferred per the mode-2 design §7).

## 5. Error handling & profile sanitization

All destructive writes follow the ZIP methodology's order — pre-flight gates,
backup-before-write, write, verify-after-write, auto-restore-on-mismatch
(`gbl-commit` / `commit_verified`). No failure path leaves a half-written
partition without a backup.

Abort conditions (all before any write): `/sdcard/stock_vbmeta.img` missing;
`mode2-profile derive` failure (unreadable, unsigned, or descriptor-less
vbmeta); unknown OEM (abort quoting the detected `ro.product.*` values);
slot/partition resolution failure.

**Profile sanitization is a first-class, two-layer, fail-closed concern** — the
`compile` step never emits a partial or invalid binary:

1. **Structural** — the TOML library (`tomlc99` / `tomllib`) rejects malformed
   syntax, duplicate keys, wrong value types, and truncation. Identical
   guarantees on both implementations.
2. **Semantic** — `compile` validates the parsed values: every required key
   present; `version == 1`; `is_unlocked ∈ {0,1}`; `color ∈ 0..3`; each digest
   exactly 64 lowercase-hex characters; `system_version`/`system_spl` fit
   `u32`; no unknown keys (rejected — catches typos in a hand-edited file).
   Each failure aborts with a precise message naming the offending key.

This makes the on-device `.toml` safe to hand-edit: a bad override fails the
flash loudly at `compile`, before anything is written.

### TOML schema

`gbl-chainload_profile.toml` — flat, every spoofed value visible, `#` comments
for provenance:

```toml
# generated by mode2-profile derive
# source: /sdcard/stock_vbmeta.img
# sha256: <hex>   os_version: '16' -> 0x40000   spl: '2026-05-01' -> 0x9a5
version        = 1
is_unlocked    = 0
color          = 0          # 0 = GREEN
system_version = 0x40000
system_spl     = 0x9a5
rot_digest     = "44149b5d…c7"   # 64 hex = SHA256(pubkey ‖ 0x00)
pubkey_digest  = "8d897f62…bb"   # 64 hex = SHA256(pubkey)
vbh            = "e33289e2…bc"   # 64 hex = SHA256(root vbmeta header+auth+aux)
```

## 6. Testing

Host tests (`tests/host/`):

- **C↔Python parity** — `mode2-profile` (C, linux build) `derive`+`compile` and
  `mode2-profile.py` `derive`+`compile`, run on the
  `images/vbmeta-infiniti-IN-16.0.7.201.img` fixture, produce byte-identical
  `.toml` and byte-identical 120-byte binaries.
- **Sanitization** — `compile` rejects malformed TOML, missing/unknown keys,
  out-of-range scalars, and wrong-length digests; never writes output on
  failure.
- **`abl-patcher --oem oneplus`** — applies the `universal` + `oneplus` groups
  and *not* `mode_1`; existing dynamic-patch tests still pass.
- **`gbl-pack`** — the `cached_abl + mode2_profile` (`ec=3`) container is
  already covered by `tests/host/081`.
- **install-mode regression** — the install-mode tests pass unchanged
  after the shared-machinery refactor into the three `mode-N-install` modes.

The `mode-2-install` flow itself is validated on-device by a recovery flash — the
user-run acceptance step, not automated.

## 7. Implementation decomposition (PR slices)

1. **TOML profile tooling** — the TOML schema; the new multi-platform C
   `mode2-profile` (`derive` + `compile`, `tomlc99` vendored, linux + android
   targets); migrate the Python tool XML→TOML; the C↔Python parity + the
   sanitization host tests.
2. **`abl-patcher` OEM patchsets** — the `universal`/`mode_1`/`oem` taxonomy,
   the `--oem` flag, the `oneplus` group.
3. **The install modes** — the shared-install-machinery refactor of
   `install.sh`/`profile.sh` into `zip/modes/install-common.sh` (on top of
   `zip/core/install_abl.sh`); the three thin `zip/modes/mode-N-install.{sh,conf}`
   modes; `zip/base/mode-{0,1,2}.efi`;
   `build-recovery-zip.sh --mode mode-{0,1,2}-install`.

Slices land as separate feature branches / PRs against `main`. Slice 3 depends
on slices 1 and 2 (it bundles the C tool and invokes `abl-patcher --oem`).
