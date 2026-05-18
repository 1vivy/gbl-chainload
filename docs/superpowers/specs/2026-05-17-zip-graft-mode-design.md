# vbmeta graft ZIP — the `graft` mode

Date: 2026-05-17
Status: design approved; implementation pending
Companion: `docs/project/vbmeta-graft-vs-construct.md` (why graft is the only
path), `docs/project/zip-methodology.md`, the SP2 packaging spec
`docs/superpowers/specs/2026-05-16-zip-packaging-structure-design.md`, and
the SP3 install spec `docs/superpowers/specs/2026-05-17-zip-install-mode-design.md`.

## Context

SP2 delivered the `zip-gbl-chainload` packaging skeleton with `graft` as an
`abort` stub. SP3 filled `install`. SP4 fills the **`graft` mode** — the
flashable ZIP that grafts stock OEM-signed vbmeta onto a user's modified
partition image so that partition survives mode-1's userspace AVB
re-verification, and normal Android boot keeps working.

`docs/project/vbmeta-graft-vs-construct.md` established the foundation: a
modified partition's AVB metadata cannot be synthesized without the OEM
private key. The only path is to substitute a whole, already-OEM-signed
stock vbmeta blob — the graft. This mode does exactly that.

## Goal & scope

Implement `modes/graft.{conf,sh}` in the `zip-gbl-chainload` submodule and a
new aarch64 recovery tool `vbmeta-graft`. The mode is general: it grafts any
footer'd partition (`recovery`, `boot`, `init_boot`, `vendor_boot`, …) the
user has a custom image for. It also realizes the `diag` vbmeta walk
deferred from SP3.

**In scope:** `modes/graft.{conf,sh}`; the `vbmeta-graft` tool
(`list` / `check` / `graft`); its recovery-toolchain build and
re-vendoring; the `diag` vbmeta-walk extension; `tests/host` coverage.

**Out of scope:** partitions with no graftable `AvbFooter` (hashtree
partitions — `system`, `vendor`, …); a host-side graft script
(`next-milestone` mentions one — separate work).

## The graft, in one paragraph

For a partition `X`, a graft produces

```
grafted_X = [custom X content] ++ [pad to 4 KiB]
            ++ [stock OEM-signed vbmeta blob] ++ [zero pad] ++ [AvbFooter]
```

where the 64-byte `AvbFooter` at the partition's end records
`vbmeta_offset = round_up(custom_content_size, 4096)`. The custom content
is the user's modified `X`; the stock vbmeta blob is lifted verbatim from a
stock `X` partition, OEM signature intact. At boot, `avb_vbmeta_image_verify()`
returns `OK` on the intact stock blob; the descriptor hash check then
mismatches (custom content ≠ stock hash) — a recoverable
`ERROR_VERIFICATION` that mode-1 handles — so both the modified partition
and normal Android boot work. This is the natural-offset graft from project
memory `graft_at_natural_offset_wins`, confirmed booting on infiniti.

## Architecture

`modes/graft.conf` — declarative:

```sh
MODE_NAME="graft"
MODE_DESC="graft stock vbmeta onto a custom partition (mode-1 AVB coexistence)"
MODE_WRITES="the selected slot of each grafted partition"
MODE_TOOLS="vbmeta-graft gbl-commit"
MODE_EFI=""
```

`modes/graft.sh` — one mode file, `mode_main` plus isolated helpers
(`pick_slot`, `find_custom_images`, `resolve_stock`, `do_graft`,
`commit_graft`). It defines only these functions — `update-binary` does the
bootstrap, `core/*.sh` sourcing, and EXIT trap.

### The `vbmeta-graft` tool (new, aarch64, `tools/vbmeta-graft/`)

A new recovery tool, built by `scripts/build-recovery-tools.sh` and vendored
into `zip/bin/` via `zip/update-tools.sh` (the SP3 `fv-unwrap` precedent).
Three subcommands:

- `vbmeta-graft list <vbmeta-image>` — parse a vbmeta image; print the
  partitions its descriptors cover, with descriptor type (hash / hashtree /
  chain). Used by `diag`'s vbmeta walk.
- `vbmeta-graft check <candidate-partition-img> <device-main-vbmeta> <part>` —
  exit 0 iff the candidate has a valid `AvbFooter` + embedded vbmeta **and**
  is *suitable* to graft for `<part>`: signed by the key the device main
  vbmeta's chain descriptor for `<part>` names, with a rollback index that
  descriptor will accept. This is a per-candidate validity gate — it catches
  a wrong or older-OTA `/sdcard/stock_<part>.img`, or a candidate slot whose
  vbmeta footer is missing/invalid.
- `vbmeta-graft graft --stock <stock-partition-img> --custom <custom-img>
  --out <grafted-img>` — read the stock partition's `AvbFooter`, extract the
  OEM-signed vbmeta blob, determine the custom image's content size, and
  assemble the grafted image (the layout above).

### Inputs

- **Custom image(s):** `/sdcard/gbl_<part>.img` — each is a user's modified
  partition, and the `<part>` in the filename names the target partition
  (`gbl_recovery.img` → `recovery`). One run grafts **every** such file
  present; the common case is one file. No partition menu — the filename is
  authoritative.
- **Slot:** in recovery, the one up-front prompt picks the slot `S`; under
  `BOOTMODE`, `S` is the inactive slot (see below). `S` is the graft + flash
  target for every grafted partition, **and** the priority-1 stock-vbmeta
  candidate.
- **Stock vbmeta:** per partition `<part>`, the first candidate that passes
  `vbmeta-graft check`, in priority order:
  1. `<part>_S` — the user's chosen slot. The human's pick leads, because
     auto-scanning which slot carries the right/newest stock vbmeta is
     unreliable (a slot's vbmeta can be partially updated post-OTA).
  2. `/sdcard/stock_<part>.img` — if the user supplied one.
  3. `<part>_<other slot>`.
  No candidate passes → `abort`.
- **Output:** the grafted image for each `<part>`, flashed to `<part>_S`.

### Slot selection

- **Recovery (`BOOTMODE=false`):** one up-front prompt —
  > `Please select slot to perform graft and flash on [A/B]? (If OTA was`
  > `flashed from recovery or you know it's on the inactive, select that one.)`

  Vol-UP = A, Vol-DOWN = B. The picked slot guides candidate selection (its
  `<part>` is priority-1), so picking the freshly-OTA'd slot grafts onto the
  newest OTA image; picking the current slot is a fine "re-install / fix my
  slot" path — user's call.
- **`BOOTMODE=true` (booted Android):** no prompt — assume an OTA applied
  by `update_engine`, so `S` = the **inactive** slot. The partitions come
  from the `/sdcard/gbl_<part>.img` filenames; everything is silent and
  `ui_print`'d.

### Recovery / BOOTMODE flow

```
1. Pre-flight: A/B slot resolves; at least one /sdcard/gbl_<part>.img
   present; every named <part> is a footer'd, vbmeta-covered partition.

2. Slot S:  recovery -> the prompt above;  BOOTMODE -> inactive slot.

3. For each /sdcard/gbl_<part>.img:
     a. Resolve the stock vbmeta: try candidates 1..3 (above) in order,
        first to pass `vbmeta-graft check` against vbmeta_S wins.
        None pass -> abort.
     b. `vbmeta-graft graft` -- chosen stock vbmeta onto the custom image
        -> grafted image in the workdir.
     c. commit_verified <grafted> /dev/block/by-name/<part>_S \
                        /sdcard/<part>_S.bak   (backup + write + verify).

4. Done -- reboot. Slot S now carries the grafted custom partition(s),
   which survive mode-1 userspace AVB.
```

### diag vbmeta-walk

The SP3-deferred item: with `vbmeta-graft list` now available, `diag` is
extended to run `vbmeta-graft list` on `vbmeta_<slot>` and print the covered
partitions. SP3 left a marker comment in `modes/diag.sh` for exactly this.

## Pre-flight gates

Before any write, all validated — `abort` on any failure leaves the device
untouched: A/B slot resolves; at least one `/sdcard/gbl_<part>.img`; each
named `<part>` is a footer'd vbmeta-covered partition; `vbmeta_S` readable;
for each `<part>`, a stock-vbmeta candidate passes `vbmeta-graft check`.

## Error handling

All via `core/safety.sh`: `abort` on any failure (loud `ui_print`,
cleanup, exit 1); the EXIT trap clears the workdir and restores the SELinux
context. Each partition write goes through `commit_verified` → `gbl-commit`
auto-restores its backup on a verify mismatch; backups are
`/sdcard/<part>_S.bak`. Every gate and step is `ui_print`'d. When several
`gbl_<part>.img` are grafted, a later partition's failure does not roll back
an earlier partition already written-and-verified — each is independently
backed up at `/sdcard/<part>_S.bak`.

## Testing

`tests/host/` coverage (auto-discovered by `tests/runall.sh`):

- A `vbmeta-graft` tool test: `list` enumerates descriptors on the committed
  `tests/images/*-abl.img` fixtures and on `images/grafted-recovery.img`;
  `check` accepts a matching stock candidate and rejects a mismatched one;
  `graft` produces an image whose `AvbFooter` records the expected
  `round_up(content, 4096)` offset and whose embedded vbmeta is the stock
  blob byte-for-byte.
- A `--mode graft` ZIP-assembly test: assemble `gbl-chainload-graft.zip`,
  assert it carries `vbmeta-graft` + `gbl-commit` + `graft.{conf,sh}`, and
  `shellcheck -s sh` the staged `graft.sh`.

The pure-logic `vbmeta-graft` paths are host-testable against fixtures. The
slot prompt and the partition writes are Layer-3 on-device validation
(user-run), like SP2's `diag` and SP3's `install`: flash
`gbl-chainload-graft.zip`, confirm the grafted partition boots and normal
Android boot survives mode-1.

## Open questions

- `vbmeta-graft check`'s rollback/version comparison is the mechanism that
  rejects an old-OTA stock candidate; the exact rollback-index handling is
  an implementation detail for the plan.
- Whether `recovery` on infiniti is chain-descriptor'd (own embedded vbmeta,
  graftable) or hash-described in the main vbmeta is verified during
  implementation via `vbmeta-graft list`; only footer'd partitions are
  graftable by this mode.
