#!/usr/bin/env bash
# Build a recovery ZIP that flashes a host-generated grafted recovery image.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: scripts/make-recovery-graft-zip.sh \
  --recovery-image out/recovery-grafted.img \
  --out dist/recovery-graft.zip
EOF
  exit 2
}

IMAGE=""
OUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --recovery-image) IMAGE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done
[[ -n "$IMAGE" && -n "$OUT" ]] || usage
[[ -f "$IMAGE" ]] || { echo "missing recovery image: $IMAGE" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$ROOT/Build/zip/recovery-graft"
rm -rf "$WORK"
mkdir -p "$WORK/payload" "$WORK/META-INF/com/google/android" "$(dirname "$OUT")"
cp "$ROOT/zip/recovery-graft/META-INF/com/google/android/update-binary" "$WORK/META-INF/com/google/android/update-binary"
cp "$ROOT/zip/recovery-graft/META-INF/com/google/android/updater-script" "$WORK/META-INF/com/google/android/updater-script"
chmod +x "$WORK/META-INF/com/google/android/update-binary"
cp "$IMAGE" "$WORK/payload/recovery-grafted.img"
cat > "$WORK/payload/manifest" <<EOF
type=recovery-graft
recovery_sha256=$(sha256sum "$IMAGE" | awk '{print $1}')
recovery_size=$(stat -c%s "$IMAGE")
EOF

python3 - "$WORK" "$OUT" <<'PY'
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as z:
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            z.write(path, os.path.relpath(path, root))
PY
echo "wrote $OUT"
