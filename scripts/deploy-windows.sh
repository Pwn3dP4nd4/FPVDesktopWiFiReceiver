#!/usr/bin/env bash
# Windows (MSYS2 UCRT64): carpeta portable dist/FPVDesktopWiFiReceiver con el .exe, Qt, libusb y las licencias
# de todo lo que se incluye (GPL/LGPL: el texto de cada licencia debe acompanar al binario).
#   scripts/deploy-windows.sh [build/release] [dist/FPVDesktopWiFiReceiver]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BUILD=${1:-$ROOT/build/release}
OUT=${2:-$ROOT/dist/FPVDesktopWiFiReceiver}
rm -rf "$OUT" && mkdir -p "$OUT"
cp "$BUILD/FPVDesktopWiFiReceiver.exe" "$OUT/"
windeployqt6 --release --translations es --no-system-d3d-compiler --no-opengl-sw "$OUT/FPVDesktopWiFiReceiver.exe" >/dev/null 2>&1 \
  || windeployqt --release --translations es "$OUT/FPVDesktopWiFiReceiver.exe" >/dev/null
# DLL que windeployqt no copia (libusb y dependencias de MinGW): las que ldd resuelve en /ucrt64
for i in 1 2 3; do
  ldd "$OUT"/*.exe "$OUT"/*.dll 2>/dev/null | awk '/\/ucrt64\// {print $3}' | sort -u | while read -r d; do
    [ -f "$OUT/$(basename "$d")" ] || cp "$d" "$OUT/"
  done
done
cp "$ROOT/README.md" "$ROOT/README.es.md" "$ROOT/LICENSE" "$OUT/"
# Driver WinUSB del adaptador (sustituye a Zadig): Archivo > Instalar driver del adaptador
mkdir -p "$OUT/driver"
cp "$ROOT/scripts/windows-driver.ps1" "$ROOT/packaging/windows/driver/fpvdesktopwifireceiver_winusb.inf" "$OUT/driver/"

scripts/collect-licenses.sh "$OUT"
echo "OK: $OUT"
