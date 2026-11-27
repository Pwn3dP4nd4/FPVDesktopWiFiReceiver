# Cambios a kepyur que se aplican al compilar, sobre copias en la carpeta de compilacion: el submodulo
# third_party/kepyur no se modifica nunca y scripts/update-deps.sh lo sigue actualizando sin conflictos.
# Cada cambio es "texto de anclaje -> texto nuevo" (sin \r). Si upstream cambia un anclaje, la configuracion
# falla diciendo cual: revisar aqui. Cuando upstream incorpore un conjunto, se borra de esta lista.
#
# Conjuntos:
#   rxdma      wifi-sdr Receiver: tras un "no packet within timeout" del 8822B, apagar y encender el RX DMA
#              (REG_CR) antes de reiniciar el flujo; desatasca el C2H en ~10 ms sin reabrir (test/wedge_test.cpp)
#   gapped     wifi-sdr Receiver: con RxSettings::allow_gapped, a cualquier velocidad se captura a huecos (un
#              banco, parar, enviar, rearmar): el analizador de canales (fpv::Radio::survey) no necesita muestras
#              contiguas y asi el 8812BU no falla por "deadline" al resintonizar sin parar
#   linksys    wifi-sdr model_pid (receiver.h): el Linksys WUSB6300 V2 (13b1:0045) es un RTL8812BU con el ID de
#              Linksys; sin esto wifi-sdr no lo reconoce como adaptador y no lo abre
#
# La radio ya no se parchea: es propia (src/radio) y native/radio de kepyur no se compila.
#
# Uso desde CMake:  include(cmake/kepyur_patches.cmake) y kp_patch_files(<origen> <destino> <conjuntos...>)
# Uso suelto (p.ej. probar un conjunto sobre un clon de kepyur, en el sitio):
#   cmake -DKP_SRC=<kepyur> -DKP_DST=<kepyur> -DKP_SETS=rxdma -P cmake/kepyur_patches.cmake

set(_KP_PATCH_FILES "")

# funcion (no macro: una macro sustituye los argumentos como texto y romperia comillas y barras del C++)
function(_kp_change set file anchor new)
  set(i ${_kp_n})
  set(_KP_ANCHOR_${set}_${file}_${i} "${anchor}" PARENT_SCOPE)
  set(_KP_NEW_${set}_${file}_${i} "${new}" PARENT_SCOPE)
  set(files ${_KP_PATCH_FILES} "${file}")
  set(_KP_PATCH_FILES "${files}" PARENT_SCOPE)
  set(idx ${_KP_IDX_${set}_${file}} ${i})
  set(_KP_IDX_${set}_${file} "${idx}" PARENT_SCOPE)
  math(EXPR i "${i} + 1")
  set(_kp_n ${i} PARENT_SCOPE)
endfunction()
set(_kp_n 0)

# ---- rxdma -------------------------------------------------------------------------------------------------
# En algunos controladores USB 3 (aqui AMD 1022:43D5 en Windows; wifi-sdr lo vio en Linux con 1022:15b9) el
# camino RX DMA -> bulk-IN del 8822B se atasca: la peticion C2H queda colgada (r294 BIT16) y cada reinicio del
# flujo pierde su primer paquete, hasta que wifi-sdr se rinde ("stream keeps failing") y la app reabria el
# adaptador (~10 s). Medido con test/wedge_test.cpp: 3 de 3 atascos vuelven con este ciclo de REG_CR (~10 ms);
# 0x294 = 0, clear_halt del endpoint o reintentar sin mas, no.
_kp_change(rxdma third_party/wifi-sdr/src/receiver.cpp
  [=[    RxState next = RxState::Starting;
    std::string why = "stream fault (restarting): " + ss.error;
]=]
  [=[    // [FPVDesktopWiFiReceiver, added at build time] 8822B on some USB 3 host controllers: the RX DMA ->
    // bulk-IN path locks up (C2H request stuck, r294 BIT16) and every restart then misses its first packet.
    // Turning the RX DMA off and on in REG_CR recovers it in ~10 ms (measured: 3 of 3 lock-ups on Windows,
    // AMD 1022:43D5), where otherwise only a full re-init did. Only after a timeout: the stream is stopped.
    if (!_caps.cu && ss.error.find("no packet within timeout") != std::string::npos && _s->reg().alive()) {
      try {
        const uint16_t cr = _s->reg().r16(0x0100);
        _s->reg().w16(0x0100, static_cast<uint16_t>(cr & ~0x000Au)); // HCI_RXDMA_EN | RXDMA_EN
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        _s->reg().w16(0x0100, cr);
        ss.error += " [rx dma reset]"; // in last_fault: the app's log shows it was done
      } catch (const std::exception &) {
      }
    }
    RxState next = RxState::Starting;
    std::string why = "stream fault (restarting): " + ss.error;
]=])

# ---- gapped ------------------------------------------------------------------------------------------------
# El analizador resintoniza cada ~15 ms y en continuo el firmware del 8812BU termina bancos tarde ("deadline:
# bank N ended X us after due"): unos 3.5 reinicios del flujo por segundo frente a ~1.2 escaneando (medido). A
# huecos no hay plazo que cumplir (wifi-sdr no comprueba el deadline). RxSettings::allow_gapped ya existia
# (velocidades que el enlace no da en continuo); aqui ademas fuerza los huecos a cualquier velocidad. Sin tocar
# receiver.h: solo se parchea receiver.cpp (ver CMakeLists.txt).
_kp_change(gapped third_party/wifi-sdr/src/receiver.cpp
  [=[    c.gapped = cur.rate_sel < _caps.min_rate_sel;
]=]
  [=[    // [FPVDesktopWiFiReceiver, added at build time] allow_gapped: gapped at any rate (channel survey)
    c.gapped = cur.rate_sel < _caps.min_rate_sel || cur.allow_gapped;
]=])

# ---- linksys -----------------------------------------------------------------------------------------------
# model_pid es la unica lista de IDs USB de wifi-sdr (session.cpp, usbmode.cpp y src/usb3_switch.cpp la usan): el
# chip lo distingue devourer por registro, asi que basta con anadir el ID. 13b1:0045 = Linksys WUSB6300 V2
# (RTL8812BU; la V1, 13b1:003f, es un RTL8812AU y no vale). Es un .h: ver en CMakeLists.txt como se compila.
_kp_change(linksys third_party/wifi-sdr/src/receiver.h
  [=[  if (vid == 0x2357 && (pid == 0x0115 || pid == 0x012d || pid == 0x0138))
    return kPid8812bu;
]=]
  [=[  if (vid == 0x2357 && (pid == 0x0115 || pid == 0x012d || pid == 0x0138))
    return kPid8812bu;
  // [FPVDesktopWiFiReceiver, added at build time] Linksys WUSB6300 V2 (RTL8812BU)
  if (vid == 0x13b1 && pid == 0x0045)
    return kPid8812bu;
]=])

list(REMOVE_DUPLICATES _KP_PATCH_FILES)

# kp_patch_files(src dst sets...): escribe en dst cada fichero tocado por esos conjuntos (solo si cambia, para no
# recompilar de mas) y deja en KP_PATCHED_FILES la lista de rutas relativas escritas.
function(kp_patch_files src dst)
  set(sets ${ARGN})
  set(written "")
  foreach(f IN LISTS _KP_PATCH_FILES)
    set(touched FALSE)
    foreach(s IN LISTS sets)
      if(DEFINED _KP_IDX_${s}_${f})
        set(touched TRUE)
      endif()
    endforeach()
    if(NOT touched)
      continue()
    endif()
    file(READ "${src}/${f}" txt)
    string(REPLACE "\r\n" "\n" txt "${txt}")
    foreach(s IN LISTS sets)
      foreach(i IN LISTS _KP_IDX_${s}_${f})
        set(a "${_KP_ANCHOR_${s}_${f}_${i}}")
        string(FIND "${txt}" "${a}" p1)
        string(FIND "${txt}" "${a}" p2 REVERSE)
        if(p1 LESS 0 OR NOT p1 EQUAL p2)
          message(FATAL_ERROR "kepyur ha cambiado ${f}: el anclaje del cambio '${s}' no aparece una sola vez "
                              "(cmake/kepyur_patches.cmake). Anclaje:\n${a}")
        endif()
        string(REPLACE "${a}" "${_KP_NEW_${s}_${f}_${i}}" txt "${txt}")
      endforeach()
    endforeach()
    set(out "${dst}/${f}")
    if(EXISTS "${out}")
      file(READ "${out}" old)
      string(REPLACE "\r\n" "\n" old "${old}")
    else()
      set(old "")
    endif()
    if(NOT old STREQUAL txt)
      file(WRITE "${out}" "${txt}")
    endif()
    list(APPEND written "${f}")
  endforeach()
  set(KP_PATCHED_FILES "${written}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND KP_SRC AND KP_DST AND KP_SETS)
  string(REPLACE "," ";" _sets "${KP_SETS}")
  kp_patch_files("${KP_SRC}" "${KP_DST}" ${_sets})
  message(STATUS "aplicado ${KP_SETS}: ${KP_PATCHED_FILES}")
endif()
