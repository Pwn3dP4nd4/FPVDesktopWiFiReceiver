#!/usr/bin/env bash
# Compila FPVDesktopWiFiReceiver. Linux, o Windows dentro de la consola "MSYS2 UCRT64".
#   scripts/build.sh            backend real (adaptador RTL8812BU)
#                               Linux:   build/release/FPVDesktopWiFiReceiver
#                               Windows: dist/FPVDesktopWiFiReceiver/FPVDesktopWiFiReceiver.exe (compila en build/msys2,
#                                        cuyo .exe no lleva las DLL al lado y solo abre desde la consola de MSYS2)
#   scripts/build.sh --demo     solo GUI + backend simulado (sin binutils MIPS ni libusb)
#   scripts/build.sh --deps     instalar antes las dependencias del sistema (apt / pacman)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
RADIO=ON
DEPS=0
for a in "$@"; do
  case "$a" in
    --demo) RADIO=OFF ;;
    --deps) DEPS=1 ;;
    *) echo "uso: $0 [--demo] [--deps]" >&2; exit 2 ;;
  esac
done

case "$(uname -s)" in
  MINGW*|MSYS*) OS=windows ;;
  Linux) OS=linux ;;
  *) OS=other ;;
esac

if [ "$DEPS" = 1 ]; then
  if [ "$OS" = windows ]; then
    [ "${MSYSTEM:-}" = UCRT64 ] || { echo "Abre la consola 'MSYS2 UCRT64'" >&2; exit 1; }
    pacman -S --needed --noconfirm git make curl tar xz vim \
      mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,qt6-base,qt6-tools,qt6-translations,libusb}
  elif command -v apt-get >/dev/null; then
    sudo apt-get update
    sudo apt-get install -y build-essential cmake ninja-build pkg-config git xxd qt6-base-dev \
      qt6-tools-dev qt6-l10n-tools qt6-translations-l10n libusb-1.0-0-dev binutils-mipsel-linux-gnu libgl1-mesa-dev
  elif command -v dnf >/dev/null; then
    sudo dnf install -y gcc-c++ cmake ninja-build pkgconf git vim-common qt6-qtbase-devel qt6-linguist qt6-qttranslations libusb1-devel \
      make curl texinfo
  elif command -v pacman >/dev/null; then
    sudo pacman -S --needed --noconfirm base-devel cmake ninja pkgconf git vim qt6-base qt6-tools qt6-translations libusb
  else
    echo "Gestor de paquetes no reconocido: instala Qt6 (Widgets), libusb-1.0, cmake, ninja, xxd." >&2
  fi
fi

[ -f third_party/kepyur/native/CMakeLists.txt ] || scripts/update-deps.sh --init

ARGS=(-DKP_WITH_RADIO=$RADIO)
if [ "$RADIO" = ON ]; then
  PREFIX=$(scripts/get-mips-binutils.sh || BUILD_ANYWAY=1 scripts/get-mips-binutils.sh)
  ARGS+=(-DKEPYUR_MIPS_PREFIX="$PREFIX")
fi
BUILD=build/release
[ "$OS" = windows ] && BUILD=build/msys2  # no "release": su .exe no es el que se abre con doble clic (ese va a dist/)
[ "$RADIO" = OFF ] && BUILD=build/demo
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release "${ARGS[@]}"
cmake --build "$BUILD" --target FPVDesktopWiFiReceiver
if [ "$OS" = windows ]; then
  # Carpeta autocontenida (Qt, libusb, runtime): el .exe se abre con doble clic, sin MSYS2 ni consola
  OUTDIR=dist/FPVDesktopWiFiReceiver
  [ "$RADIO" = OFF ] && OUTDIR=dist/FPVDesktopWiFiReceiver-demo
  scripts/deploy-windows.sh "$BUILD" "$OUTDIR"
  echo
  echo "OK: $ROOT/$OUTDIR/FPVDesktopWiFiReceiver.exe  (doble clic para abrir)"
else
  echo
  echo "OK: $ROOT/$BUILD/FPVDesktopWiFiReceiver   (instalar con menu e icono: sudo cmake --install $BUILD)"
fi
exit 0
