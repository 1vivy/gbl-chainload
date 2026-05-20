# gbl-chainload host tools

This bundle ships with a `VERSION` file (also printed by each tool via
`<tool> --version` or `efisp-package.py --version`).

## What's inside

| Tool | Purpose |
|---|---|
| `fv-unwrap` | Extract PE32 payload from EDK-II FV container (`abl.img` → `.efi`) |
| `abl-patcher` | Apply mode-N patches to a stock ABL binary |
| `gbl-pack` | Build a GBLP1 overlay payload |
| `gbl-commit` | Inspect / validate a packed payload |
| `vbmeta-graft` | Graft a stock OEM vbmeta onto a custom image; `list-hash` subcmd |
| `mode2-profile` | Compile mode-2 user profile (TOML → binary container) |
| `gblp1-inspect` | Parse + verify a GBLP1 container; per-entry SHA256 check |

## Verifying the download

```bash
sha256sum -c SHA256SUMS
```

All seven binaries should report `OK`. If any line reports `FAILED`, do not use the bundle — re-download.

## Using the tools

`efisp-package.py` chains the tools to produce a flash-ready EFISP payload off-device. From the unzipped bundle:

```bash
python3 efisp-package.py --help
python3 efisp-package.py --version
```

The script auto-discovers binaries in the adjacent `bin/` directory. Override with `--bin-dir <path>` if needed.

Individual tools accept `--version` and print a usage banner without arguments.

## Project

Repository: https://github.com/1vivy/gbl-chainload
