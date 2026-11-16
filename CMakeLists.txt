cmake_minimum_required(VERSION 3.22)
project(FPVDesktopWiFiReceiver VERSION 0.7.0 LANGUAGES C CXX)

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
# De kepyur se usa native/dsp (el decodificador de video analogico, el mismo que la app Android) y wifi-sdr +
# devourer (sus submodulos). La radio (src/radio, fpv::Radio) es propia: deriva de native/radio de kepyur.
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

# ---- cambios a kepyur aplicados al compilar (cmake/kepyur_patches.cmake) ---------------------------------------
# Copias en el build con los cambios: el submodulo no se toca.
#   comb: filtro peine de croma NTSC en native/dsp (propuesto a kepyur)
#   rxdma: desatascar el RX DMA del 8822B al reiniciar el flujo (wifi-sdr receiver.cpp)
#   gapped: allow_gapped captura a huecos a cualquier velocidad, para el analizador (wifi-sdr receiver.cpp)
include(cmake/kepyur_patches.cmake)
set(_kp_out "${CMAKE_BINARY_DIR}/kepyur_patched")
set(_kp_sets comb)
if(KP_WITH_RADIO)
  list(APPEND _kp_sets rxdma gapped)
endif()
kp_patch_files("${KEPYUR_DIR}" "${_kp_out}" ${_kp_sets})
foreach(_d dsp)
  file(GLOB _fs RELATIVE "${KEPYUR_DIR}" "${KEPYUR_DIR}/native/${_d}/*.h" "${KEPYUR_DIR}/native/${_d}/*.cpp")
  foreach(_f IN LISTS _fs)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KEPYUR_DIR}/${_f}")
    if(NOT _f IN_LIST KP_PATCHED_FILES)  # los no tocados, tal cual (para que los #include "x.h" vean la copia)
      configure_file("${KEPYUR_DIR}/${_f}" "${_kp_out}/${_f}" COPYONLY)
    endif()
  endforeach()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kepyur_patches.cmake")
# usar las copias: fuentes y directorios de include de los targets de kepyur
function(_kp_use_copy target dir)
  get_target_property(_src ${target} SOURCES)
  list(TRANSFORM _src REPLACE "^(.*/)?${dir}/" "${_kp_out}/native/${dir}/")
  set_property(TARGET ${target} PROPERTY SOURCES ${_src})
  foreach(_p INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(_inc ${target} ${_p})
    list(FILTER _inc EXCLUDE REGEX "/native/${dir}/?$")
    set_property(TARGET ${target} PROPERTY ${_p} "${_kp_out}/native/${dir}" ${_inc})
  endforeach()
endfunction()
_kp_use_copy(kepyur_dsp dsp)
if(TARGET wsdrcore)  # rxdma: solo receiver.cpp (sus #include "x.h" siguen viendo wifi-sdr/src por el include PUBLIC)
  get_target_property(_src wsdrcore SOURCES)
  list(TRANSFORM _src REPLACE "^(.*/)?src/receiver\.cpp$" "${_kp_out}/third_party/wifi-sdr/src/receiver.cpp")
  set_property(TARGET wsdrcore PROPERTY SOURCES ${_src})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KEPYUR_DIR}/third_party/wifi-sdr/src/receiver.cpp")
endif()
if(TARGET kp_replay)  # banco de kepyur (no se compila por defecto): con --comb
  set_property(TARGET kp_replay PROPERTY SOURCES "${_kp_out}/native/tools/kp_replay.cpp")
endif()

# ---- radio propia (src/radio): fpv::Radio sobre wsdr::Receiver, con el DSP de kepyur ----------------------------
# Deriva de native/radio de kepyur; lleva como codigo normal lo que antes eran parches: plan de escaneo, peine,
# senales digitales (src/kp_ext) y huecos en la grabacion de IQ. native/radio de kepyur ya no se compila.
if(KP_WITH_RADIO)
  set(_fpv_radio_src src/radio/radio.cpp src/radio/radio.h src/radio/survey.cpp src/radio/survey.h src/kp_ext/digital_scan.cpp src/kp_ext/digital_scan.h)
  add_library(fpv_radio STATIC ${_fpv_radio_src})
  target_include_directories(fpv_radio PUBLIC src/radio PRIVATE src/kp_ext)
  target_link_libraries(fpv_radio PUBLIC kepyur_dsp wsdrcore)
  target_compile_options(fpv_radio PRIVATE -O2 -Wall -Wextra)
  # radio_sim: fpv::Radio sin adaptador (KP_SIM_FILE=grabacion.cs16), como kp_sim de kepyur: sim_receiver.cpp
  # da todos los simbolos de wsdr::Receiver, asi que el receiver.o de wsdrcore no se enlaza.
  #   cmake --build build/msys2 --target radio_sim  ->  radio_sim --scan [--digital] (ver test/radio_sim.cpp)
  find_package(Threads REQUIRED)
  add_executable(radio_sim EXCLUDE_FROM_ALL test/radio_sim.cpp src/scan_plan.cpp src/channel_advisor.cpp ${_fpv_radio_src} "${KEPYUR_DIR}/native/tools/sim_receiver.cpp")
  target_include_directories(radio_sim PRIVATE src src/radio src/kp_ext)
  target_link_libraries(radio_sim PRIVATE kepyur_dsp wsdrcore Threads::Threads)
  target_compile_options(radio_sim PRIVATE -O2 -Wall -Wextra)
  if(WIN32)
    target_link_options(radio_sim PRIVATE -static-libgcc -static-libstdc++)
  endif()
  # radio_live: lo mismo con el adaptador de verdad (p.ej. grabar IQ desde la consola)
  #   radio_live --watch 5658 --rate 1 --record 5 rec/hdzero --seconds 12
  add_executable(radio_live EXCLUDE_FROM_ALL test/radio_sim.cpp src/scan_plan.cpp src/channel_advisor.cpp)
  target_include_directories(radio_live PRIVATE src)
  target_link_libraries(radio_live PRIVATE fpv_radio Threads::Threads)
  target_compile_options(radio_live PRIVATE -O2 -Wall -Wextra)
  if(WIN32)
    target_sources(radio_live PRIVATE compat/win32/winusb_raw_io.cpp)
    target_link_libraries(radio_live PRIVATE winmm)
    target_link_options(radio_live PRIVATE -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_close
                        -static-libgcc -static-libstdc++)
  endif()
endif()
if(WIN32)
  # GCC/MinGW comprueba printf con las reglas de msvcrt y avisa de %zu, que UCRT si soporta: falso positivo.
  foreach(t kepyur_dsp fpv_radio radio_sim radio_live)
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
  src/scan_plan.cpp src/scan_plan.h
  src/scan_channels_dialog.cpp src/scan_channels_dialog.h
  src/channel_analyzer.cpp src/channel_analyzer.h src/channel_advisor.cpp src/channel_advisor.h
  src/i18n.cpp src/i18n.h
  src/video_recorder.cpp src/video_recorder.h src/video_encoder.h
  assets/icons.qrc
)
target_link_libraries(FPVDesktopWiFiReceiver PRIVATE Qt6::Widgets kepyur_dsp)

# ---- Grabacion de video (MP4 H.264): el codificador del sistema -----------------------------------------------
# Windows: Media Foundation (viene con Windows, ninguna DLL que copiar). Linux: FFmpeg (libavcodec/libavformat de
# la distro, opcional: sin el, el boton de grabar video sale desactivado). Las capturas PNG van siempre.
if(WIN32)
  target_sources(FPVDesktopWiFiReceiver PRIVATE src/video_encoder_mf.cpp)
  target_link_libraries(FPVDesktopWiFiReceiver PRIVATE mfplat mfreadwrite mfuuid ole32)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBAV IMPORTED_TARGET libavformat libavcodec libavutil)
  endif()
  if(LIBAV_FOUND)
    target_sources(FPVDesktopWiFiReceiver PRIVATE src/video_encoder_av.cpp)
    target_link_libraries(FPVDesktopWiFiReceiver PRIVATE PkgConfig::LIBAV)
  else()
    target_sources(FPVDesktopWiFiReceiver PRIVATE src/video_encoder_none.cpp)
    message(WARNING "FFmpeg (libavformat/libavcodec) no encontrado: sin grabacion de video "
                    "(Debian/Ubuntu: apt install libavformat-dev libavcodec-dev; Fedora: ffmpeg-free-devel)")
  endif()
endif()
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
  target_link_libraries(FPVDesktopWiFiReceiver PRIVATE fpv_radio)
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
# Detector de senales digitales con senales sinteticas (sin adaptador): cmake --build <dir> --target digital_scan_test
add_executable(digital_scan_test EXCLUDE_FROM_ALL test/digital_scan_test.cpp src/kp_ext/digital_scan.cpp)
target_include_directories(digital_scan_test PRIVATE src/kp_ext)
target_link_libraries(digital_scan_test PRIVATE kepyur_dsp)
if(WIN32)
  target_link_options(digital_scan_test PRIVATE -static-libgcc -static-libstdc++)
endif()

# Captura del analizador con el backend de demo: QT_QPA_PLATFORM=offscreen analyzer_shot salida.png [N M [s]]
add_executable(analyzer_shot test/analyzer_shot.cpp src/channel_analyzer.cpp src/channel_analyzer.h
               src/channel_advisor.cpp src/stub_backend.cpp src/stub_backend.h src/backend.h src/video_widget.cpp)
target_include_directories(analyzer_shot PRIVATE src)
target_link_libraries(analyzer_shot PRIVATE Qt6::Widgets kepyur_dsp)

# Medida del analizador (src/radio/survey.cpp) con IQ sinteticos: cmake --build <dir> --target survey_test
add_executable(survey_test EXCLUDE_FROM_ALL test/survey_test.cpp src/radio/survey.cpp)
target_include_directories(survey_test PRIVATE src/radio)
target_link_libraries(survey_test PRIVATE kepyur_dsp)
target_compile_options(survey_test PRIVATE -O2 -Wall -Wextra)
if(WIN32)
  target_link_options(survey_test PRIVATE -static-libgcc -static-libstdc++)
endif()

# Recomendador de canales (analizador) con espectros sinteticos: cmake --build <dir> --target channel_advisor_test
add_executable(channel_advisor_test EXCLUDE_FROM_ALL test/channel_advisor_test.cpp src/channel_advisor.cpp)
target_include_directories(channel_advisor_test PRIVATE src)
target_link_libraries(channel_advisor_test PRIVATE kepyur_dsp)
target_compile_options(channel_advisor_test PRIVATE -Wall -Wextra)
if(WIN32)
  target_link_options(channel_advisor_test PRIVATE -static-libgcc -static-libstdc++)
endif()

if(KP_WITH_RADIO)
  add_executable(reopen_test EXCLUDE_FROM_ALL test/reopen_test.cpp src/usb3_switch.cpp)
  target_include_directories(reopen_test PRIVATE src)
  target_link_libraries(reopen_test PRIVATE fpv_radio)
  if(WIN32)
    target_sources(reopen_test PRIVATE compat/win32/winusb_raw_io.cpp compat/win32/usb_backend.cpp test/diag_wraps.cpp)
    target_include_directories(reopen_test PRIVATE compat/win32)
    target_link_libraries(reopen_test PRIVATE winmm)
    target_link_options(reopen_test PRIVATE -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_close
                        -Wl,--wrap=libusb_init -Wl,--wrap=libusb_reset_device
                        -static-libgcc -static-libstdc++)
  endif()
  # Atasco del flujo USB (C2H): cmake --build build/msys2 --target wedge_test  (ver test/wedge_test.cpp)
  add_executable(wedge_test EXCLUDE_FROM_ALL test/wedge_test.cpp)
  target_link_libraries(wedge_test PRIVATE kepyur_dsp wsdrcore)
  if(WIN32)
    target_sources(wedge_test PRIVATE compat/win32/winusb_raw_io.cpp compat/win32/usb_backend.cpp)
    target_include_directories(wedge_test PRIVATE compat/win32)
    target_link_libraries(wedge_test PRIVATE winmm)
    target_link_options(wedge_test PRIVATE -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_close -Wl,--wrap=libusb_init
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
