#!/usr/bin/env bash
# Regenera assets/icons/* desde assets/logo.svg. Necesita Qt 6 con el modulo SVG
# (MSYS2: pacman -S mingw-w64-ucrt-x86_64-qt6-svg; Debian: apt install qt6-svg-dev).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
command -v cygpath >/dev/null && ROOT=$(cygpath -m "$ROOT")  # CMake nativo en Windows
B="$ROOT/build/make-icons"
mkdir -p "$B" "$ROOT/assets/icons"
cat > "$B/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.22)
project(make_icons CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Gui Svg)
add_executable(make_icons "$ROOT/scripts/make-icons/make_icons.cpp")
target_link_libraries(make_icons PRIVATE Qt6::Gui Qt6::Svg)
EOF
cmake -S "$B" -B "$B/out" -G Ninja >/dev/null
cmake --build "$B/out" >/dev/null
QT_QPA_PLATFORM=offscreen "$B/out/make_icons" "$ROOT/assets/logo.svg" "$ROOT/assets/icons"
