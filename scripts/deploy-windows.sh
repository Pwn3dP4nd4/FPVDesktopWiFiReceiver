#!/usr/bin/env bash
# Windows (MSYS2 UCRT64): carpeta portable dist/kepyur-desktop con el .exe, Qt y libusb.
#   scripts/deploy-windows.sh [build/release]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build/release}
OUT=$ROOT/dist/kepyur-desktop
rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BUILD/kepyur-desktop.exe" "$OUT/"
windeployqt6 --release --no-translations --no-system-d3d-compiler --no-opengl-sw "$OUT/kepyur-desktop.exe" 2>/dev/null \
  || windeployqt --release --no-translations "$OUT/kepyur-desktop.exe"
# DLL que windeployqt no copia (libusb y dependencias de MinGW): las que ldd resuelve en /ucrt64
for i in 1 2 3; do
  ldd "$OUT"/*.exe "$OUT"/*.dll 2>/dev/null | awk '/\/ucrt64\// {print $3}' | sort -u | while read -r d; do
    [ -f "$OUT/$(basename "$d")" ] || cp "$d" "$OUT/"
  done
done
cp "$ROOT/README.md" "$OUT/"
echo "OK: $OUT"
