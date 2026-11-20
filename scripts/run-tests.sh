#!/usr/bin/env bash
# Compila y ejecuta los bancos de prueba que no necesitan adaptador (senales sinteticas). Sale con error si alguno
# falla. Tras scripts/build.sh, en la misma carpeta de compilacion:
#   scripts/run-tests.sh                 (build/release en Linux, build/msys2 en la consola MSYS2 UCRT64)
#   scripts/run-tests.sh build/demo      (otra carpeta)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
case "$(uname -s)" in
  MINGW*|MSYS*) BUILD=${1:-build/msys2} ;;
  *) BUILD=${1:-build/release} ;;
esac
[ -f "$BUILD/CMakeCache.txt" ] || { echo "No hay compilacion en $BUILD: ejecuta antes scripts/build.sh" >&2; exit 2; }
TESTS=(channel_advisor_test survey_test digital_scan_test)
cmake --build "$BUILD" --target "${TESTS[@]}"
failed=()
for t in "${TESTS[@]}"; do
  echo "==== $t"
  "$BUILD/$t" || failed+=("$t")
done
echo
if [ ${#failed[@]} -gt 0 ]; then
  echo "FALLAN: ${failed[*]}" >&2
  exit 1
fi
echo "OK: ${TESTS[*]}"
