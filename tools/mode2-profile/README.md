# mode2-profile

Host tooling for gbl-chainload mode-2 profiles.

    python3 mode2-profile.py derive  <vbmeta.img>   -o profile.toml
    python3 mode2-profile.py compile <profile.toml> -o profile.bin

    # or the C tool (same semantics, cross-compilable aarch64-static):
    tools/mode2-profile/mode2-profile compile <profile.toml> -o profile.bin

`derive` needs `avbtool.py` — set `AVBTOOL=/path/to/avbtool.py` or place it
at `~/avbtool.py`.

`profile.bin` is the 120-byte `gbl_mode2_profile` struct
(`tools/shared/gbl_mode2_profile.h`); pass it to `gbl-pack --mode2-profile`
to build a GBLP1 `0x0010` overlay.

## Vendored dependencies

- `vendor/tomlc99/` — [tomlc99](https://github.com/cktan/tomlc99) by CK Tan,
  MIT License. Single-file C99 TOML parser (`toml.c` + `toml.h`).

See `docs/superpowers/specs/2026-05-18-mode-2-profile-tooling-design.md`.
