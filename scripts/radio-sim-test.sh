#!/usr/bin/env bash
# Comprueba fpv::Radio (src/radio) sin adaptador: radio_sim reproduce una grabacion sintetica como si fuera el
# 8812BU (test/radio_sim.cpp, KP_SIM_FILE; digital_scan_test --write la genera) y se mira lo que lista. Cubre el
# escaneo (src/radio/radio.cpp: handle_scan, scan_step_done) y el analizador (survey, con la comprobacion de
# sincronismo y la de senales digitales tomando el paso de nuevo sin huecos). Sale con error si alguno falla.
# Tras scripts/build.sh, en la misma carpeta de compilacion (con KP_WITH_RADIO, el backend real):
#   scripts/radio-sim-test.sh                 (build/release en Linux, build/msys2 en la consola MSYS2 UCRT64)
#   scripts/radio-sim-test.sh build/demo      (otra carpeta)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
case "$(uname -s)" in
  MINGW*|MSYS*) BUILD=${1:-build/msys2} ;;
  *) BUILD=${1:-build/release} ;;
esac
[ -f "$BUILD/CMakeCache.txt" ] || { echo "No hay compilacion en $BUILD: ejecuta antes scripts/build.sh" >&2; exit 2; }
grep -q '^KP_WITH_RADIO:BOOL=ON' "$BUILD/CMakeCache.txt" || { echo "$BUILD no lleva la radio (--demo): nada que probar"; exit 0; }
cmake --build "$BUILD" --target radio_sim digital_scan_test

SIM=$(mktemp -d)
trap 'rm -rf "$SIM"' EXIT
"$BUILD/digital_scan_test" --write "$SIM/fm.cs16" fm 5769 20 >/dev/null
"$BUILD/digital_scan_test" --write "$SIM/hdz.cs16" hdzero 5769 15 >/dev/null

failed=()
# caso: nombre, grabacion, segundos, lo que tiene que salir en la ultima linea de estado (grep -E), argumentos
run() {
  local name=$1 rec=$2 secs=$3 want=$4
  shift 4
  echo "==== $name"
  local last
  last=$(KP_SIM_FILE="$SIM/$rec" timeout $((secs + 30)) "$BUILD/radio_sim" "$@" --seconds "$secs" 2>&1 |
         grep '^{"open"' | tail -1 || true)
  echo "${last:0:400}"
  if grep -qE "$want" <<<"$last"; then echo "ok"; else echo "FALLO: se esperaba $want"; failed+=("$name"); fi
}
# FM en R4: el escaneo lo lista (sincronismo de video) y se queda viendolo, enganchado
run "escaneo: FM en R4" fm.cs16 20 '"mode":"watch".*"locked":true.*"pilots":\[\{"mhz":576[89][^}]*"name":"R4","std":"NTSC"' --scan
# el analizador lo lista sin sintonizarlo (el paso, otra vez sin huecos para el sincronismo)
run "analizador: FM en R4" fm.cs16 25 '"mode":"scan".*"pilots":\[\{"mhz":576[89][^}]*"name":"R4","std":"NTSC"' \
  --survey 5640 5950
# HDZero en R4 con las senales digitales: su tiempo de trama, tambien con el paso tomado otra vez sin huecos
run "analizador: HDZero en R4" hdz.cs16 25 '"pilots":\[\{"mhz":57[67][0-9][^}]*"digital":"hdzero' \
  --survey 5640 5950 --digital
run "escaneo: HDZero en R4" hdz.cs16 20 '"pilots":\[\{"mhz":57[67][0-9][^}]*"digital":"hdzero' --scan --digital

echo
if [ ${#failed[@]} -gt 0 ]; then
  echo "FALLAN: ${failed[*]}" >&2
  exit 1
fi
echo "OK: radio_sim"
