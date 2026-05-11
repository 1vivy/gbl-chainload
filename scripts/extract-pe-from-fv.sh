#!/usr/bin/env bash
# extract-pe-from-fv.sh — unwrap LinuxLoader.efi from each raw FV in images/
# into images/pe/<fixture>.efi.  Skip fixtures that already have a PE form.
set -euo pipefail

cd "$(dirname "$0")/.."
make -C tools/fv-unwrap

mkdir -p images/pe

# Per-fixture extraction.  Add new entries when more fixtures land.
declare -A FV_TO_PE=(
  ["images/infiniti-EU-16.0.5.703/abl.bin"]="images/pe/infiniti-EU-16.0.5.703.efi"
  ["images/infiniti-IN-16.0.7.201.img"]="images/pe/infiniti-IN-16.0.7.201.efi"
  ["images/fairlady-CN-16.0.7.200.img"]="images/pe/fairlady-CN-16.0.7.200.efi"
)

for src in "${!FV_TO_PE[@]}"; do
  dest="${FV_TO_PE[$src]}"
  if [[ -f "$src" ]]; then
    echo "==> $src → $dest"
    tools/fv-unwrap/fv-unwrap "$src" "$dest" || {
      echo "WARN: failed to extract $src (FV may need LZMA decompression — out of scope)"; continue;
    }
    sha256sum "$dest"
  else
    echo "SKIP: $src (file missing)"
  fi
done

# Copy a pre-extracted myron PE if MYRON_PE points at one
# (e.g. MYRON_PE=/path/to/gbl_root_canoe/tests/extracted/LinuxLoader.efi).
if [[ -n "${MYRON_PE:-}" && -f "$MYRON_PE" ]]; then
  cp "$MYRON_PE" images/pe/myron.efi
  sha256sum images/pe/myron.efi
fi

echo "==> Done. PE fixtures available:"
ls -la images/pe/
