#!/usr/bin/env bash
# wifi-sdr assembles its on-chip MIPS16e2 programs with GNU binutils for mipsel-linux-gnu.
#   Linux (Debian/Ubuntu): sudo apt install binutils-mipsel-linux-gnu   (this script only checks for it)
#   Windows (MSYS2 UCRT64): no package exists, so this script builds GNU binutils once into
#                           tools/mipsel-binutils/ (about 5-10 minutes, ~150 MB while building).
# Prints the prefix to use (".../mipsel-linux-gnu-") on stdout; the build scripts pass it to CMake.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${BINUTILS_VERSION:-2.43.1}
DEST="$ROOT/tools/mipsel-binutils"

if command -v mipsel-linux-gnu-as >/dev/null 2>&1; then
  echo "$(dirname "$(command -v mipsel-linux-gnu-as)")/mipsel-linux-gnu-"
  exit 0
fi
if [ -x "$DEST/bin/mipsel-linux-gnu-as" ] || [ -x "$DEST/bin/mipsel-linux-gnu-as.exe" ]; then
  echo "$DEST/bin/mipsel-linux-gnu-"
  exit 0
fi

case "$(uname -s)" in
  Linux)
    echo "Falta mipsel-linux-gnu-as. Instala: sudo apt install binutils-mipsel-linux-gnu" \
         "(Fedora: dnf install binutils-mips64-linux-gnu no sirve; usa este script con BUILD_ANYWAY=1)" >&2
    [ "${BUILD_ANYWAY:-0}" = 1 ] || exit 1 ;;
esac

for t in make gcc curl tar; do
  command -v $t >/dev/null || { echo "Falta '$t' (MSYS2: pacman -S make mingw-w64-ucrt-x86_64-gcc)" >&2; exit 1; }
done

WORK="$ROOT/tools/.binutils-build"
mkdir -p "$WORK"
cd "$WORK"
TARBALL="binutils-$VER.tar.xz"
if [ ! -f "$TARBALL" ]; then
  echo "Descargando $TARBALL de ftp.gnu.org ..." >&2
  curl -fL --retry 3 -o "$TARBALL.part" "https://ftp.gnu.org/gnu/binutils/$TARBALL" >&2
  mv "$TARBALL.part" "$TARBALL"
fi
rm -rf "binutils-$VER" obj
# MSYS2 tar may warn about symlinks in test dirs on Windows; they are not needed.
tar -xf "$TARBALL" 2>/dev/null || true
mkdir obj && cd obj
echo "Compilando binutils $VER (target mipsel-linux-gnu) ..." >&2
"../binutils-$VER/configure" --target=mipsel-linux-gnu --prefix="$DEST" \
  --disable-nls --disable-werror --disable-gdb --disable-gdbserver --disable-sim --disable-gprofng \
  --disable-libctf --disable-gold --without-zstd --disable-plugins MAKEINFO=true \
  CFLAGS="-O2 -std=gnu17" >&2  # GCC >= 15 compila en C23 por defecto; binutils 2.43 necesita gnu17
make -j"$(nproc)" MAKEINFO=true all-gas all-ld all-binutils >&2
make MAKEINFO=true install-gas install-ld install-binutils >&2
cd "$ROOT" && rm -rf "$WORK"
echo "$DEST/bin/mipsel-linux-gnu-"
