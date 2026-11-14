#!/usr/bin/env bash
# Windows (MSYS2 UCRT64): copia en DIR/licenses el texto de licencia del paquete MSYS2 de cada DLL de DIR (y del
# runtime de GCC, enlazado estaticamente en el .exe) y escribe DIR/THIRD-PARTY-NOTICES.txt con la lista.
#   scripts/collect-licenses.sh DIR [--no-versions]
# Falla si algun paquete no tiene texto de licencia: no se debe distribuir asi.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
VERSIONS=1
[ "${2:-}" = --no-versions ] && VERSIONS=0
LIC="$OUT/licenses"
rm -rf "$LIC" && mkdir -p "$LIC"
pkgs=$(
  {
    find "$OUT" -name '*.dll' | while read -r f; do
      rel=${f#"$OUT"/}
      if [ -f "/ucrt64/bin/$rel" ]; then echo "/ucrt64/bin/$rel"
      elif [ -f "/ucrt64/share/qt6/plugins/$rel" ]; then echo "/ucrt64/share/qt6/plugins/$rel"
      else echo "AVISO: origen desconocido de $rel" >&2
      fi
    done
    echo /ucrt64/bin/libstdc++-6.dll
    echo /ucrt64/bin/libgcc_s_seh-1.dll
  } | xargs pacman -Qoq 2>/dev/null | sort -u
)
list=""
missing=()
for p in $pkgs; do
  name=${p#mingw-w64-ucrt-x86_64-}
  src=/ucrt64/share/licenses/$name
  # ICU no instala sus licencias en share/licenses: estan junto a sus datos (share/icu/<version>/)
  [ -d "$src" ] || src=$(ls -d /ucrt64/share/"$name"/*/ 2>/dev/null | head -1 || true)
  if [ -n "$src" ] && [ -d "$src" ]; then
    mkdir -p "$LIC/$name"
    find "$src" -maxdepth 1 -type f \( -iname '*licen*' -o -iname '*copying*' -o -iname '*.txt' -o -iname 'NOTICE*' \) \
      -exec cp {} "$LIC/$name/" \;
    [ -n "$(ls -A "$LIC/$name")" ] || { rmdir "$LIC/$name"; missing+=("$name"); }
  else
    missing+=("$name")
  fi
  if [ "$VERSIONS" = 1 ]; then
    list+=$(printf '  %-28s %s' "$name" "$(pacman -Q "$p" | awk '{print $2}')")$'\n'
  else
    list+="  $name"$'\n'
  fi
done
{ cat "$ROOT/packaging/THIRD-PARTY-NOTICES.txt"; printf '%s' "$list"; } > "$OUT/THIRD-PARTY-NOTICES.txt"
cp "$ROOT/LICENSE" "$OUT/LICENSE"
if [ ${#missing[@]} -gt 0 ]; then
  echo "ERROR: sin texto de licencia para: ${missing[*]} (anadelo antes de distribuir)" >&2
  exit 1
fi
echo "licencias: $(echo "$pkgs" | wc -w) paquetes en $LIC"
