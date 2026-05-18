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
(`next-milestone` mentions one — separate work); `BOOTMODE` (graft is
recovery-only).

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
MODE_WRITES="<selected partition>"
MODE_TOOLS="vbmeta-graft gbl-commit"
MODE_EFI=""
```

`modes/graft.sh` — one mode file, `mode_main` plus isolated helpers
(`pick_slot`, `resolve_partition`, `select_stock`, `do_graft`,
`commit_graft`). It defines only these functions — `update-binary` does the
bootstrap, `core/*.sh` sourcing, and EXIT trap.

### The `vbmeta-graft` tool (new, aarch64, `tools/vbmeta-graft/`)

A new recovery tool, built by `scripts/build-recovery-tools.sh` and vendored
into `zip/bin/` via `zip/update-tools.sh` (the SP3 `fv-unwrap` precedent).
Three subcommands:

- `vbmeta-graft list <vbmeta-image>` — parse a vbmeta image; print the
  partitions its descriptors cover, with descriptor type (hash / hashtree /
  chain) and which are footer'd-graftable. Used by the suitability check and
  by `diag`'s vbmeta walk.
- `vbmeta-graft check <candidate-partition-img> <device-main-vbmeta> <part>` —
  exit 0 iff the candidate is a self-consistent stock `<part>` partition
  **and** is *suitable* to graft: its embedded vbmeta is signed by the key
  the device main vbmeta's chain descriptor for `<part>` names, and its
  rollback index satisfies that descriptor's rollback location. Prints a
  match summary (key match, rollback delta, version) so the mode can rank
  candidates. This catches a wrong or older-OTA `/sdcard/stock_<part>.img`.
- `vbmeta-graft graft --stock <stock-partition-img> --custom <custom-img>
  --out <grafted-img>` — read the stock partition's `AvbFooter`, extract the
  OEM-signed vbmeta blob, determine the custom image's content size, and
  assemble the grafted image (the layout above).

### Inputs

- **Custom image:** `/sdcard/gbl_<part>.img` — the user's modified
  partition. The `<part>` in the filename names the target partition
  (`gbl_recovery.img` → `recovery`).
- **Stock vbmeta:** the best-matching *suitable* candidate among
  `/sdcard/stock_<part>.img` (if present), `<part>_a`, and `<part>_b` —
  selected by `vbmeta-graft check` (see "Suitability"). `/sdcard/stock_<part>.img`
  lets the user pin a specific stock version or supply stock when no slot
  holds it.
- **Slot:** chosen by the one up-front prompt (see the flow). The picked
  slot `S` is the graft + flash target.
- **Output:** the grafted image, flashed to `<part>_S`.

### Recovery flow (`BOOTMODE=false`)

```
1. Pre-flight: A/B slot resolves; recovery only (BOOTMODE -> abort);
   at least one /sdcard/gbl_<part>.img present.

2. Slot prompt (the one up-front prompt):
     "Please select slot to perform graft and flash on [A/B]?
      (If OTA was flashed from recovery or you know it's on the
      inactive, select that one.)"
   -> slot S.   Vol-UP = A, Vol-DOWN = B.

3. Partition: taken from the /sdcard/gbl_<part>.img filename.
   One such file -> <part>. Multiple -> a vol-key cycle pick among the
   files. <part> must be a footer'd, vbmeta-covered partition
   (validated via `vbmeta-graft list` on vbmeta_S) -> else abort.

4. Stock-vbmeta selection: gather candidates -- /sdcard/stock_<part>.img
   (if present), <part>_a, <part>_b. Run `vbmeta-graft check` on each
   against vbmeta_S. Rank the passing candidates by match quality; pick
   the best. No suitable candidate -> abort ("no suitable stock vbmeta;
   provide /sdcard/stock_<part>.img matching this OTA").

5. Graft: `vbmeta-graft graft` -- chosen stock vbmeta onto
   /sdcard/gbl_<part>.img -> grafted image in the workdir.

6. Flash: commit_verified <grafted> /dev/block/by-name/<part>_S \
                          /sdcard/<part>_S.bak   (backup + verify).

7. Done -- reboot. Slot S now carries the grafted custom <part>, which
   survives mode-1 userspace AVB.
```

### Suitability check & candidate ranking

A graft fails at boot if the stock vbmeta does not satisfy the device's
main vbmeta. The main `vbmeta_S` carries, for partition `<part>`, a chain
descriptor naming the public key `<part>`'s vbmeta must be signed with and a
rollback-index location. `vbmeta-graft check` verifies a candidate against
that: the candidate's embedded vbmeta must be signed by the named key and
carry a rollback index the device will accept. An old-OTA
`/sdcard/stock_<part>.img` typically fails the rollback test; a wrong-image
file fails the key/structure test. Among candidates that pass, the mode
picks the one most consistent with `vbmeta_S` (closest version / rollback).
The check is a distinct `vbmeta-graft` subcommand so it runs as an explicit
pre-graft gate.

### BOOTMODE

`graft` is recovery-only. Under `BOOTMODE=true` it `abort`s with a message
to flash from recovery — the slot prompt and partition pick need a screen,
and graft is a deliberate, surgical operation.

### diag vbmeta-walk

The SP3-deferred item: with `vbmeta-graft list` now available, `diag` is
extended to run `vbmeta-graft list vbmeta_<slot>` and print the covered
partitions. SP3 left a marker comment in `modes/diag.sh` for exactly this.

## Pre-flight gates

Before any write, all validated — `abort` on any failure leaves the device
untouched: A/B slot resolves; not `BOOTMODE`; at least one
`/sdcard/gbl_<part>.img`; the resolved `<part>` is a footer'd
vbmeta-covered partition; `vbmeta_S` and the candidate partitions are
readable; a suitable stock-vbmeta candidate exists.

## Error handling

All via `core/safety.sh`: `abort` on any failure (loud `ui_print`,
cleanup, exit 1); the EXIT trap clears the workdir and restores the SELinux
context. The single partition write goes through `commit_verified` →
`gbl-commit` auto-restores its backup on a verify mismatch; the backup is
`/sdcard/<part>_S.bak`. Every gate and step is `ui_print`'d.

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
device prompts and the partition write are Layer-3 on-device validation
(user-run), like SP2's `diag` and SP3's `install`: flash
`gbl-chainload-graft.zip`, confirm the grafted partition boots and normal
Android boot survives mode-1.

## Open questions

- The partition cycle-pick (flow step 3) only appears when multiple
  `/sdcard/gbl_*.img` files are present; the common single-file case has no
  partition prompt, so the slot prompt is genuinely the only prompt.
- `vbmeta-graft check`'s ranking among multiple passing candidates is
  "closest to `vbmeta_S`"; the exact tie-break (identical blobs are common
  since both slots run the same OTA build) is an implementation detail for
  the plan.
- Whether `recovery` on infiniti is chain-descriptor'd (own embedded vbmeta,
  graftable) or hash-described in the main vbmeta is verified during
  implementation via `vbmeta-graft list`; only footer'd partitions are
  graftable by this mode.
