cmake_minimum_required(VERSION 3.22)
project(FPVDesktopWiFiReceiver VERSION 0.3.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(MSVC)
  message(FATAL_ERROR "kepyur/wifi-sdr/devourer usan extensiones GCC/Clang: en Windows compila con MSYS2 UCRT64 "
                      "(scripts/build.sh o scripts/build-windows.ps1), no con MSVC.")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(Qt6 QUIET COMPONENTS LinguistTools)  # traducciones (lupdate/lrelease)

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
if(WIN32)
  # GCC/MinGW comprueba printf con las reglas de msvcrt y avisa de %zu, que UCRT si soporta: falso positivo.
  foreach(t kepyur_dsp kepyur_radio)
    if(TARGET ${t})
      target_compile_options(${t} PRIVATE -Wno-format -Wno-format-extra-args)
    endif()
  endforeach()
endif()

# ---- GUI ----------------------------------------------------------------------------------------------------
add_executable(FPVDesktopWiFiReceiver
  src/main.cpp
  src/main_window.cpp src/main_window.h
  src/video_widget.cpp src/video_widget.h
  src/backend.h
  src/stub_backend.cpp src/stub_backend.h
  src/i18n.cpp src/i18n.h
  assets/icons.qrc
)
target_link_libraries(FPVDesktopWiFiReceiver PRIVATE Qt6::Widgets kepyur_dsp)
target_compile_definitions(FPVDesktopWiFiReceiver PRIVATE KP_VERSION="${PROJECT_VERSION}")

# ---- Idiomas: textos fuente en ingles; espanol en translations/*.ts, compilado (.qm) dentro del ejecutable ----
# Tras cambiar textos: cmake --build build/release (Windows: build/msys2) --target update_translations (actualiza los .ts con lupdate)
set(KP_TS_FILES translations/FPVDesktopWiFiReceiver_es.ts)
if(TARGET Qt6::lrelease)
  qt_add_translations(FPVDesktopWiFiReceiver TS_FILES ${KP_TS_FILES} RESOURCE_PREFIX "/i18n"
                      LUPDATE_OPTIONS -no-obsolete -locations none)
else()
  message(WARNING "Qt LinguistTools no encontrado: la interfaz solo estara en ingles "
                  "(MSYS2: pacman -S mingw-w64-ucrt-x86_64-qt6-tools; Debian/Ubuntu: apt install qt6-tools-dev qt6-l10n-tools)")
endif()
if(KP_WITH_RADIO)
  target_sources(FPVDesktopWiFiReceiver PRIVATE src/radio_backend.cpp src/radio_backend.h src/usb3_switch.cpp src/usb3_switch.h)
  target_link_libraries(FPVDesktopWiFiReceiver PRIVATE kepyur_radio)
  target_compile_definitions(FPVDesktopWiFiReceiver PRIVATE KP_WITH_RADIO=1)
  if(WIN32)
    # WinUSB RAW_IO en el endpoint de streaming (sin el, 40 MS/s pierde el deadline del chip): se envuelve
    # libusb_submit_transfer en el enlazado, wifi-sdr/devourer quedan sin modificar.
    target_sources(FPVDesktopWiFiReceiver PRIVATE compat/win32/winusb_raw_io.cpp)
    target_link_options(FPVDesktopWiFiReceiver PRIVATE -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_close)
  endif()
endif()

if(WIN32)
  # Aplicacion de ventana (doble clic, sin consola) con icono y version en el .exe
  enable_language(RC)
  configure_file(packaging/windows/app.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" @ONLY)
  configure_file(packaging/windows/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
  target_sources(FPVDesktopWiFiReceiver PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
  set_target_properties(FPVDesktopWiFiReceiver PROPERTIES WIN32_EXECUTABLE ON)
  target_link_libraries(FPVDesktopWiFiReceiver PRIVATE winmm)  # timeBeginPeriod
  # Ejecutable autocontenido respecto al runtime de GCC (Qt y libusb se copian con scripts/deploy-windows.sh)
  target_link_options(FPVDesktopWiFiReceiver PRIVATE -static-libgcc -static-libstdc++)
endif()

# ---- Banco de pruebas de reapertura (no se instala ni se compila por defecto) --------------------------------
# cmake --build build/msys2 --target reopen_test  ->  build/msys2/reopen_test.exe --help (ver test/)
if(KP_WITH_RADIO)
  add_executable(reopen_test EXCLUDE_FROM_ALL test/reopen_test.cpp src/usb3_switch.cpp)
  target_include_directories(reopen_test PRIVATE src)
  target_link_libraries(reopen_test PRIVATE kepyur_radio)
  if(WIN32)
    target_sources(reopen_test PRIVATE compat/win32/winusb_raw_io.cpp compat/win32/usb_backend.cpp test/diag_wraps.cpp)
    target_include_directories(reopen_test PRIVATE compat/win32)
    target_link_libraries(reopen_test PRIVATE winmm)
    target_link_options(reopen_test PRIVATE -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_close
                        -Wl,--wrap=libusb_init -Wl,--wrap=libusb_reset_device
                        -static-libgcc -static-libstdc++)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS FPVDesktopWiFiReceiver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
  install(FILES packaging/linux/FPVDesktopWiFiReceiver.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  foreach(n 16 24 32 48 64 128 256 512)
    install(FILES assets/icons/app-${n}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${n}x${n}/apps
            RENAME FPVDesktopWiFiReceiver.png)
  endforeach()
  # udev solo lee /etc/udev/rules.d, /usr/lib/udev/rules.d y /run: nunca <prefijo>/lib/udev (con el prefijo
  # por defecto /usr/local la regla quedaria ignorada). Ruta absoluta, independiente de CMAKE_INSTALL_PREFIX.
  set(KP_UDEV_RULES_DIR "/etc/udev/rules.d" CACHE PATH "Donde instalar la regla udev (paquetes: /usr/lib/udev/rules.d)")
  install(FILES packaging/linux/60-kepyur.rules DESTINATION "${KP_UDEV_RULES_DIR}")
  install(CODE [[
    find_program(_udevadm udevadm PATHS /usr/bin /bin /sbin /usr/sbin)
    if(_udevadm AND "$ENV{DESTDIR}" STREQUAL "")
      execute_process(COMMAND "${_udevadm}" control --reload RESULT_VARIABLE _r1 OUTPUT_QUIET ERROR_QUIET)
      execute_process(COMMAND "${_udevadm}" trigger --subsystem-match=usb RESULT_VARIABLE _r2 OUTPUT_QUIET ERROR_QUIET)
      if(_r1 EQUAL 0 AND _r2 EQUAL 0)
        message(STATUS "udev recargado: desenchufa y vuelve a enchufar el adaptador")
      else()
        message(STATUS "No se pudo recargar udev (sin root?): sudo udevadm control --reload && sudo udevadm trigger")
      endif()
    endif()
  ]])
endif()
