#!/usr/bin/env bash
# Linux: carpeta dist/FPVDesktopWiFiReceiver-linux con el binario, licencia, avisos, regla udev, .desktop e icono.
#   scripts/package-linux.sh [build/release]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build/release}
OUT=$ROOT/dist/FPVDesktopWiFiReceiver-linux
rm -rf "$OUT" && mkdir -p "$OUT"
install -m 755 "$BUILD/FPVDesktopWiFiReceiver" "$OUT/"
cp "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/README.es.md" "$ROOT/packaging/linux/60-kepyur.rules" \
   "$ROOT/packaging/linux/FPVDesktopWiFiReceiver.desktop" "$OUT/"
cp "$ROOT/assets/icons/app-256.png" "$OUT/FPVDesktopWiFiReceiver.png"
cat > "$OUT/THIRD-PARTY-NOTICES.txt" <<'TXT'
FPVDesktopWiFiReceiver - GPL-2.0 (LICENSE). Source / codigo fuente:
  https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver (tag of this release / etiqueta de esta version), which
  includes kepyur, wifi-sdr and devourer (GPL-2.0) as git submodules.
Qt 6 (LGPL-3.0), libusb (LGPL-2.1) and FFmpeg (libavcodec/libavformat, LGPL-2.1+ or GPL depending on the
distribution's build) are the system's own shared libraries (not included here).
Qt 6 (LGPL-3.0), libusb (LGPL-2.1) y FFmpeg (libavcodec/libavformat, LGPL-2.1+ o GPL segun la compilacion de la
distribucion) son las bibliotecas compartidas del sistema (no se incluyen aqui).
TXT
echo "OK: $OUT"
