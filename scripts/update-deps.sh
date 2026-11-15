#!/usr/bin/env bash
# Dependencias upstream (submodulos git, sin forks; los anadidos a kepyur se aplican al compilar sobre copias,
# ver cmake/kepyur_patches.cmake, sin tocar el submodulo):
#   third_party/kepyur                                  -> github.com/gilankpam/kepyur (main)
#     third_party/libusb, third_party/wifi-sdr          -> las versiones que kepyur tiene fijadas
#       external/devourer                               -> la version que wifi-sdr tiene fijada
# Solo kepyur se sigue "a la ultima"; wifi-sdr/devourer/libusb van en las versiones que kepyur ha probado.
#
#   scripts/update-deps.sh --init     primera vez tras clonar (versiones fijadas en este repo)
#   scripts/update-deps.sh            traer lo ultimo de kepyur (y lo que el fije), mostrar los cambios
#   scripts/update-deps.sh --latest   ademas, wifi-sdr y devourer a la ultima de sus ramas (experimental)
#
# Despues: recompilar y, si funciona, git commit -am "deps: kepyur <hash>".
# No se inicializan los submodulos reference/* de devourer (drivers de referencia, GBs, no hacen falta).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
MODE=${1:-update}
K=third_party/kepyur

nested() {
  git -C "$K" submodule sync --quiet
  git -C "$K" submodule update --init third_party/libusb third_party/wifi-sdr
  git -C "$K/third_party/wifi-sdr" submodule sync --quiet
  git -C "$K/third_party/wifi-sdr" submodule update --init external/devourer
}

case "$MODE" in
  --init)
    git submodule sync --quiet
    git submodule update --init "$K"
    nested ;;
  update|--latest)
    old=$(git -C "$K" rev-parse --short HEAD 2>/dev/null || echo none)
    git submodule update --init --remote "$K"
    nested
    if [ "$MODE" = --latest ]; then
      git -C "$K/third_party/wifi-sdr" fetch origin
      br=$(git config -f "$K/.gitmodules" submodule.third_party/wifi-sdr.branch || echo kepyur-android)
      git -C "$K/third_party/wifi-sdr" checkout --quiet "origin/$br"
      git -C "$K/third_party/wifi-sdr/external/devourer" fetch origin
      git -C "$K/third_party/wifi-sdr/external/devourer" checkout --quiet origin/HEAD
      echo "AVISO: wifi-sdr/devourer fuera de las versiones fijadas por kepyur."
    fi
    new=$(git -C "$K" rev-parse --short HEAD)
    echo "kepyur: $old -> $new"
    [ "$old" != none ] && [ "$old" != "$new" ] && git -C "$K" log --oneline "$old..$new" | head -30
    git submodule status --recursive 2>/dev/null | grep -v reference/ || true ;;
  *) echo "uso: $0 [--init | --latest]" >&2; exit 2 ;;
esac
