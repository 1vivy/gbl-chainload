# gbl-chainload host tools

Release bundle layout:

```text
efisp-package.py
vbmeta-graft.py
bin/gbl          (or bin/gbl.exe on Windows)
SHA256SUMS
VERSION
```

The seven C tools that previous releases shipped (`fv-unwrap`, `abl-patcher`,
`gbl-pack`, `gbl-commit`, `vbmeta-graft`, `mode2-profile`, `gblp1-inspect`)
are now subcommands of a single Rust multicall `gbl`. Each subcommand keeps
the original CLI argv shape and exit codes:

| Subcommand        | Replaces         |
|-------------------|------------------|
| `gbl unwrap …`    | `fv-unwrap`      |
| `gbl patch …`     | `abl-patcher`    |
| `gbl pack …`      | `gbl-pack`       |
| `gbl commit …`    | `gbl-commit`     |
| `gbl avb …`       | `vbmeta-graft`   |
| `gbl mode2 …`     | `mode2-profile`  |
| `gbl inspect …`   | `gblp1-inspect`  |

`efisp-package.py` and `vbmeta-graft.py` auto-discover `bin/gbl` and shell
out via subcommand dispatch.

## Quick checks

```bash
sha256sum -c SHA256SUMS
python3 efisp-package.py --version
./bin/gbl --help
./bin/gbl commit --help     # any subcommand prints its own clap usage
```

If `sha256sum` reports `FAILED`, do not use the bundle.

## Package an EFISP image

The same single `gbl-chainload.efi` is used for every install profile —
mode-N selection is now a runtime GBLP1 manifest bit rather than a
per-binary compile flag. The release ships `gbl-chainload-vX.Y.Z.efi`
at the top level.

```bash
# Mode 0 (universal patches only)
python3 efisp-package.py \
  --abl      stock_abl.img \
  --manifest 0x00 \
  --efi      gbl-chainload-vX.Y.Z.efi \
  --out      installed-mode0.efi

# Mode 1 (fakelock — adds WantFakelockHook = 0x0001)
python3 efisp-package.py \
  --abl      stock_abl.img \
  --manifest 0x01 \
  --efi      gbl-chainload-vX.Y.Z.efi \
  --out      installed-mode1.efi

# Mode 2 (profile spoof — adds WantProfileSpoof = 0x0002)
python3 efisp-package.py \
  --abl          stock_abl.img \
  --manifest     0x02 \
  --efi          gbl-chainload-vX.Y.Z.efi \
  --stock-vbmeta stock_vbmeta.img \
  --oem          oplus \
  --out          installed-mode2.efi
```

Before booting, inspect the appended GBLP1 payload:

```bash
./bin/gbl inspect installed-mode1.efi
```

Device testing is RAM-only:

```bash
fastboot stage installed-mode1.efi
fastboot oem boot-efi
```

Do not flash firmware partitions while iterating on host-tool output.

## Graft a partition image

`vbmeta-graft.py` is the convenience path for partition images such as
`system`, `vendor`, `product`, `dtbo`, or other AVB-chained partitions. It
defaults the final partition size to `custom_partition.img`'s file size,
which is normally what you want when that file is the full image you intend
to write. Use `--part-size` or `--size-from` when the custom image is
trimmed/bare and the destination partition size differs.

```bash
python3 vbmeta-graft.py \
  --stock  stock_partition.img \
  --custom custom_partition.img \
  --out    grafted_partition.img
```

The raw subcommand is still available for inspection and fully manual
control:

```bash
./bin/gbl avb list      stock_partition.img
./bin/gbl avb list-hash stock_partition.img
./bin/gbl avb graft \
  --stock     stock_partition.img \
  --custom    custom_partition.img \
  --part-size <target-partition-bytes> \
  --out       grafted_partition.img

./bin/gbl avb check grafted_partition.img vbmeta.img <partition-name>
```

`graft` does not need the main `vbmeta.img`; `check` does.
