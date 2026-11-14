cmake_minimum_required(VERSION 3.22)
project(kepyur_desktop VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(MSVC)
  message(FATAL_ERROR "kepyur/wifi-sdr/devourer usan extensiones GCC/Clang: en Windows compila con MSYS2 UCRT64 "
                      "(scripts/build.sh o scripts/build-windows.ps1), no con MSVC.")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# ---- upstream kepyur (git submodule third_party/kepyur, sin modificar) -----------------------------------------
# native/dsp (decodificador de video) y native/radio (wifi-sdr + devourer + libusb) son exactamente los
# mismos que usa la app Android de kepyur: la GUI de escritorio sustituye a native/jni.
set(KEPYUR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/kepyur" CACHE PATH "Arbol de kepyur (submodulo)")
if(NOT EXISTS "${KEPYUR_DIR}/native/CMakeLists.txt")
  message(FATAL_ERROR "Falta ${KEPYUR_DIR}. Ejecuta: scripts/update-deps.sh --init")
endif()

option(KP_WITH_RADIO "Compilar el backend real (adaptador RTL8812BU por USB)" ON)

if(KP_WITH_RADIO)
  if(NOT EXISTS "${KEPYUR_DIR}/third_party/wifi-sdr/external/devourer/CMakeLists.txt")
    message(FATAL_ERROR "Faltan los submodulos de kepyur (wifi-sdr/devourer). Ejecuta: scripts/update-deps.sh --init")
  endif()
  # binutils mipsel para el firmware on-chip de wifi-sdr. KEPYUR_MIPS_PREFIX (entorno o -D) = ".../mipsel-linux-gnu-"
  if(NOT KEPYUR_MIPS_PREFIX AND DEFINED ENV{KEPYUR_MIPS_PREFIX})
    set(KEPYUR_MIPS_PREFIX "$ENV{KEPYUR_MIPS_PREFIX}")
  endif()
  if(NOT KEPYUR_MIPS_PREFIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mipsel-binutils/bin")
    set(KEPYUR_MIPS_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/tools/mipsel-binutils/bin/mipsel-linux-gnu-")
  endif()
  if(KEPYUR_MIPS_PREFIX)
    set(MIPS_AS "${KEPYUR_MIPS_PREFIX}as${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "")
    set(MIPS_LD "${KEPYUR_MIPS_PREFIX}ld${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "")
    set(MIPS_OBJCOPY "${KEPYUR_MIPS_PREFIX}objcopy${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "")
  endif()
  find_program(MIPS_AS mipsel-linux-gnu-as)
  if(NOT MIPS_AS)
    message(FATAL_ERROR "No encuentro mipsel-linux-gnu-as. Ejecuta scripts/get-mips-binutils.sh "
                        "(Linux: sudo apt install binutils-mipsel-linux-gnu) o compila con -DKP_WITH_RADIO=OFF.")
  endif()
  # wifi-sdr (via devourer) usa pkg-config para libusb-1.0; xxd para los arrays de firmware.
  find_program(XXD xxd)
  if(NOT XXD)
    message(FATAL_ERROR "Falta xxd (Debian: apt install xxd; MSYS2: pacman -S vim)")
  endif()
endif()

set(KEPYUR_RADIO ${KP_WITH_RADIO} CACHE BOOL "" FORCE)
# EXCLUDE_FROM_ALL: solo se compila lo que enlaza la GUI (los kp_* de escritorio de kepyur no hacen falta).
add_subdirectory("${KEPYUR_DIR}/native" kepyur_native EXCLUDE_FROM_ALL)

if(KP_WITH_RADIO AND WIN32)
  # radio.cpp incluye <sys/resource.h> + gettid()/setpriority(): shim en compat/win32 (sin tocar upstream)
  target_include_directories(kepyur_radio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat/win32")
endif()

# ---- GUI ----------------------------------------------------------------------------------------------------
add_executable(kepyur-desktop
  src/main.cpp
  src/main_window.cpp src/main_window.h
  src/video_widget.cpp src/video_widget.h
  src/backend.h
  src/stub_backend.cpp src/stub_backend.h
)
target_link_libraries(kepyur-desktop PRIVATE Qt6::Widgets kepyur_dsp)
target_compile_definitions(kepyur-desktop PRIVATE KP_VERSION="${PROJECT_VERSION}")
if(KP_WITH_RADIO)
  target_sources(kepyur-desktop PRIVATE src/radio_backend.cpp src/radio_backend.h src/usb3_switch.cpp src/usb3_switch.h)
  target_link_libraries(kepyur-desktop PRIVATE kepyur_radio)
  target_compile_definitions(kepyur-desktop PRIVATE KP_WITH_RADIO=1)
  if(WIN32)
    # WinUSB RAW_IO en el endpoint de streaming (sin el, 40 MS/s pierde el deadline del chip): se envuelve
    # libusb_submit_transfer en el enlazado, wifi-sdr/devourer quedan sin modificar.
    target_sources(kepyur-desktop PRIVATE compat/win32/winusb_raw_io.cpp)
    target_link_options(kepyur-desktop PRIVATE -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_close)
  endif()
endif()

if(WIN32)
  set_target_properties(kepyur-desktop PROPERTIES WIN32_EXECUTABLE ON)
  # Ejecutable autocontenido respecto al runtime de GCC (Qt y libusb se copian con scripts/deploy-windows.sh)
  target_link_options(kepyur-desktop PRIVATE -static-libgcc -static-libstdc++)
endif()

include(GNUInstallDirs)
install(TARGETS kepyur-desktop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
  install(FILES packaging/linux/kepyur-desktop.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES packaging/linux/60-kepyur.rules DESTINATION lib/udev/rules.d)
endif()
