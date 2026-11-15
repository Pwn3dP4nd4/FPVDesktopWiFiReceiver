# Cambios a kepyur que se aplican al compilar, sobre copias en la carpeta de compilacion: el submodulo
# third_party/kepyur no se modifica nunca y scripts/update-deps.sh lo sigue actualizando sin conflictos.
# Cada cambio es "texto de anclaje -> texto nuevo" (sin \r). Si upstream cambia un anclaje, la configuracion
# falla diciendo cual: revisar aqui. Cuando upstream incorpore un conjunto, se borra de esta lista.
#
# Conjuntos:
#   scan_plan  kp::Radio::set_scan_plan(): escanear solo los canales elegidos (propio de esta app)
#   comb       DecoderOpts::comb + kp::Radio::set_comb(): filtro peine 1H de croma NTSC (propuesto a kepyur)
#
# Uso desde CMake:  include(cmake/kepyur_patches.cmake) y kp_patch_files(<origen> <destino> <conjuntos...>)
# Uso suelto (p.ej. preparar el PR sobre un clon de kepyur, en el sitio):
#   cmake -DKP_SRC=<kepyur> -DKP_DST=<kepyur> -DKP_SETS=comb -P cmake/kepyur_patches.cmake

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

# ---- scan_plan ---------------------------------------------------------------------------------------------
_kp_change(scan_plan native/radio/radio.h "  void scan(double prefer_hz = 0);\n" [=[  void scan(double prefer_hz = 0);
  // [FPVDesktopWiFiReceiver, added at build time] step centres for scan() in Hz (40 MS/s, +-16.5 MHz each);
  // empty (the default) = the full plan. Takes effect on the next scan().
  void set_scan_plan(std::vector<double> centres_hz) {
    std::lock_guard<std::mutex> l(_m);
    _scan_plan_override = std::move(centres_hz);
  }

private:
  std::vector<double> _scan_plan_override;

public:
]=])
_kp_change(scan_plan native/radio/radio.cpp "        _plan = scan_plan();\n" [=[        {
          std::lock_guard<std::mutex> l(_m);
          _plan = _scan_plan_override.empty() ? scan_plan() : _scan_plan_override;
        }
]=])

# ---- comb --------------------------------------------------------------------------------------------------
_kp_change(comb native/dsp/video.h
  "  bool colour = true;          // decode colour (falls back to grey when there is no usable burst)\n"
  [=[  bool colour = true;          // decode colour (falls back to grey when there is no usable burst)
  bool comb = false;           // NTSC: 1H comb for chroma (fewer rainbows on fine detail and OSD text; ~+15 % decode)
]=])
_kp_change(comb native/dsp/video.cpp
  "  std::vector<cf> chroma_ring;  // chroma at baseband (U/V before the burst rotation)\n"
  [=[  std::vector<cf> chroma_ring;  // chroma at baseband (U/V before the burst rotation)
  // NTSC 1H comb for chroma (DecoderOpts::comb): the subcarrier is 227.5 x the line rate, so chroma flips
  // sign from one line to the next while picture detail mostly repeats. (d - d[-1H]) / 2 keeps chroma and
  // cancels luma detail near the subcarrier, which the 1.2 MHz chroma filter alone lets through as rainbows
  // on fine patterns and on the edges of white OSD text. Luma keeps the notch. PAL (2H pattern) is unchanged.
  std::vector<float> comp_ring;  // discriminator output, for the one-line delay
  bool comb = false;
  double comb_T = -1;            // T the interpolator below was set up for
  int64_t comb_back = 0;         // whole samples back to the interpolation's second tap
  float comb_w[4] = {};          // 4-tap cubic (Catmull-Rom) weights: linear would leave ~15 % at 3.58 MHz
]=])
_kp_change(comb native/dsp/video.cpp
  "    chroma_ring.assign(8192, cf{});\n"
  [=[    chroma_ring.assign(8192, cf{});
    comp_ring.assign(8192, 0);
]=])
_kp_change(comb native/dsp/video.cpp
  "    luma_notch.notch(fsc, fs, 1.5);  // the subcarrier out of the picture (also the B/W herringbone)\n"
  [=[    luma_notch.notch(fsc, fs, 1.5);  // the subcarrier out of the picture (also the B/W herringbone)
    comb = o.comb && &s != &kPal;
]=])
_kp_change(comb native/dsp/video.cpp
  "      // the oscillator's phase belongs to the absolute sample index: burst and picture share it\n      const cf c = d * std::conj(lo);\n"
  [=[      float cd = d;
      if (comb) {
        comp_ring[n & ring_mask] = d;
        if (T != comb_T) {  // T moves a little once a line: the weights only change then
          comb_T = T;
          const double back = std::ceil(T);  // sample n - T lies between n - back and n - back + 1
          comb_back = static_cast<int64_t>(back);
          const float t = static_cast<float>(back - T), t2 = t * t, t3 = t2 * t;
          comb_w[0] = 0.5f * (-t3 + 2 * t2 - t);
          comb_w[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
          comb_w[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
          comb_w[3] = 0.5f * (t3 - t2);
        }
        const int64_t i1 = static_cast<int64_t>(n) - comb_back;
        const float prev = comb_w[0] * comp_ring[(i1 - 1) & ring_mask] + comb_w[1] * comp_ring[i1 & ring_mask] +
                           comb_w[2] * comp_ring[(i1 + 1) & ring_mask] + comb_w[3] * comp_ring[(i1 + 2) & ring_mask];
        cd = 0.5f * (d - prev);
      }
      // the oscillator's phase belongs to the absolute sample index: burst and picture share it
      const cf c = cd * std::conj(lo);
]=])
_kp_change(comb native/radio/radio.h
  "  void set_gain(double db);\n"
  [=[  void set_gain(double db);
  // NTSC chroma comb (DecoderOpts::comb) from the next watch(): fewer rainbows on fine detail and OSD text,
  // ~15 % more decode CPU. Off by default.
  void set_comb(bool on);
]=])
_kp_change(comb native/radio/radio.h
  "  double _gain_db = -1;\n"
  [=[  double _gain_db = -1;
  bool _comb = false;
]=])
_kp_change(comb native/radio/radio.cpp
  "void Radio::record(double seconds, const std::string &prefix) {\n"
  [=[void Radio::set_comb(bool on) {
  std::lock_guard<std::mutex> l(_m);
  _comb = on;
}

void Radio::record(double seconds, const std::string &prefix) {
]=])
_kp_change(comb native/radio/radio.cpp
  "    _fe = std::make_shared<FrontEnd>(b.fs, b.shift_hz, _frames);\n"
  [=[    DecoderOpts o;
    {
      std::lock_guard<std::mutex> l(_m);
      o.comb = _comb;
    }
    _fe = std::make_shared<FrontEnd>(b.fs, b.shift_hz, _frames, o);
]=])
_kp_change(comb native/tools/kp_replay.cpp
  "//   kp_replay REC.cs16 [--offset HZ] [--start S] [--seconds S] [--cnr DB] [--frames] [--woven] [-v]\n"
  [=[//   kp_replay REC.cs16 [--offset HZ] [--start S] [--seconds S] [--cnr DB] [--frames] [--woven] [--comb] [-v]
]=])
_kp_change(comb native/tools/kp_replay.cpp
  "  bool frames = false, woven = false, as20 = false;\n"
  "  bool frames = false, woven = false, as20 = false, comb = false;\n")
_kp_change(comb native/tools/kp_replay.cpp
  "    else if (k == \"--woven\") woven = true;\n"
  [=[    else if (k == "--woven") woven = true;
    else if (k == "--comb") comb = true;
]=])
_kp_change(comb native/tools/kp_replay.cpp
  "[--frames] [--woven] [-v]\\n\");\n"
  [=[[--frames] [--woven] [--comb] [-v]\n");
]=])
_kp_change(comb native/tools/kp_replay.cpp
  "  o.fields = !woven;\n"
  [=[  o.fields = !woven;
  o.comb = comb;
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
