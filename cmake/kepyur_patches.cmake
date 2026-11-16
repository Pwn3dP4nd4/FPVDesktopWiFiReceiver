# Cambios a kepyur que se aplican al compilar, sobre copias en la carpeta de compilacion: el submodulo
# third_party/kepyur no se modifica nunca y scripts/update-deps.sh lo sigue actualizando sin conflictos.
# Cada cambio es "texto de anclaje -> texto nuevo" (sin \r). Si upstream cambia un anclaje, la configuracion
# falla diciendo cual: revisar aqui. Cuando upstream incorpore un conjunto, se borra de esta lista.
#
# Conjuntos:
#   comb       DecoderOpts::comb: filtro peine adaptativo de 3 lineas para la croma NTSC en native/dsp (video.h,
#              video.cpp) y --comb en kp_replay (propuesto a kepyur: gilankpam/kepyur PR 2). Lo activa
#              fpv::Radio::set_comb() (src/radio)
#   rxdma      wifi-sdr Receiver: tras un "no packet within timeout" del 8822B, apagar y encender el RX DMA
#              (REG_CR) antes de reiniciar el flujo; desatasca el C2H en ~10 ms sin reabrir (test/wedge_test.cpp)
#
# La radio ya no se parchea: es propia (src/radio) y native/radio de kepyur no se compila.
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

# ---- comb --------------------------------------------------------------------------------------------------
# (generado del diff de la rama ntsc-chroma-comb del PR: es el mismo codigo que propone)
_kp_change(comb native/dsp/video.h
  [=[  bool colour = true;          // decode colour (falls back to grey when there is no usable burst)
]=]
  [=[  bool colour = true;          // decode colour (falls back to grey when there is no usable burst)
  bool comb = false;           // NTSC: adaptive 3-line comb for chroma (fewer rainbows on fine detail and
                               // OSD text; ~+15 % decode CPU)
]=])
_kp_change(comb native/dsp/video.cpp
  [=[  std::vector<cf> chroma_ring;  // chroma at baseband (U/V before the burst rotation)
]=]
  [=[  std::vector<cf> chroma_ring;  // chroma at baseband (U/V before the burst rotation)
  // kepyur: NTSC chroma comb (DecoderOpts::comb). The subcarrier is 227.5 x the line rate, so at the same
  // place one line away chroma comes back with the subcarrier's sign flipped while luma detail does not:
  // combing cancels the luma near the subcarrier that the 1.2 MHz chroma filter lets through as rainbows.
  // It works on the baseband chroma already in chroma_ring, inside render_line: each line is drawn one line
  // late, once the line below is in, and combed with both neighbours (centred, so chroma stays on luma's
  // line). Adaptive: per pixel a neighbour only counts as far as its luma there matches this line's, so the
  // top and bottom edges of OSD glyphs fall back to the plain filter instead of keeping half the rainbow.
  // Luma keeps the notch. PAL (2H subcarrier pattern) is unchanged.
  struct CombLine {
    double ls = -1;
    int64_t k = -1;
    int field = -1;
    bool col = false;  // drawn in colour
    bool ok = false;   // usable as a neighbour: locked, in colour, burst on the PLL's predicted phase
    uint64_t epoch = 0;
  };
  static constexpr float kCombFull = 0.08f, kCombNone = 0.25f;  // luma mismatch, of black-to-white
  static constexpr float kCombMaxErr = 0.35f;  // rad: a burst further off means the stream jumped
  bool comb = false;
  double comb_w = 0;           // the mixing oscillator's frequency, rad per sample
  uint64_t comb_epoch = 0;     // bumped where the sample stream may have jumped: lines across it don't mix
  CombLine comb_up, comb_mid;  // the two lines before the current one
  float burst_e = 0;           // |phase error| of the last burst against the PLL's prediction
  std::vector<float> comb_m;   // draw_line: the comb on every other pixel (see there)
  std::vector<cf> comb_o;
]=])
_kp_change(comb native/dsp/video.cpp
  [=[    luma_notch.notch(fsc, fs, 1.5);  // the subcarrier out of the picture (also the B/W herringbone)
]=]
  [=[    luma_notch.notch(fsc, fs, 1.5);  // the subcarrier out of the picture (also the B/W herringbone)
    comb = o.comb && &s != &kPal;
    comb_w = a;
    comb_epoch++;
]=])
_kp_change(comb native/dsp/video.cpp
  [=[    last_stray = -1;
]=]
  [=[    last_stray = -1;
    comb_epoch++;
]=])
_kp_change(comb native/dsp/video.cpp
  [=[    }
    if (c_last_line < 0) {
]=]
  [=[    }
    burst_e = static_cast<float>(std::fabs(e));
    if (c_last_line < 0) {
]=])
_kp_change(comb native/dsp/video.cpp
  [=[    const int64_t k = static_cast<int64_t>(line_idx) - field_line0 - std_->skip;
]=]
  [=[    const int64_t k = static_cast<int64_t>(line_idx) - field_line0 - std_->skip;
    if (comb) {
      comb_line(ls, k, locked_line);
      return;
    }
]=])
_kp_change(comb native/dsp/video.cpp
  [=[    const bool col = o.colour && locked_line && burst(ls, pal_v);
]=]
  [=[    const bool col = o.colour && locked_line && burst(ls, pal_v);
    draw_line(ls, k, col, pal_v, false, false);
  }

  // NTSC comb: line k is measured now (levels, burst) and the previous one, k - 1, is drawn, combed with
  // k - 2 and k. A neighbour is used only if both lines are locked and in colour, in the same field and on
  // the same side of any jump in the stream (comb_epoch, and a burst on its predicted phase).
  void comb_line(double ls, int64_t k, bool locked_line) {
    CombLine cur;
    cur.ls = ls;
    cur.k = k;
    cur.field = cur_field;
    cur.epoch = comb_epoch;
    if (k >= 0 && k < std_->active) {
      if (locked_line) measure_levels(ls);
      bool pal_v = false;
      cur.col = o.colour && locked_line && burst(ls, pal_v);
      cur.ok = cur.col && burst_e < kCombMaxErr;
    }
    const CombLine &m = comb_mid;
    if (m.field == cur_field && m.k == k - 1 && m.k >= 0 && m.k < std_->active) {
      const auto pair = [&](const CombLine &x, int64_t dk) {
        return m.ok && x.ok && x.field == m.field && x.k == m.k + dk && x.epoch == m.epoch;
      };
      draw_line(m.ls, m.k, m.col, false, pair(comb_up, -1), pair(cur, 1));
    }
    comb_up = comb_mid;
    comb_mid = cur;
  }

  // up / dn: comb this line's chroma with the line above / below (see comb_line)
  void draw_line(double ls, int64_t k, bool col, bool pal_v, bool up, bool dn) {
]=])
_kp_change(comb native/dsp/video.cpp
  [=[    const bool pal = std_ == &kPal;
]=]
  [=[    const bool pal = std_ == &kPal;
    // the comb's neighbours sit one line period T (Ti whole samples and Tf more) away, turned onto this
    // line's subcarrier phase: the oscillator runs on the absolute sample index, so what is mixed down one
    // line away comes out turned by w*T
    if (dn && ls + a0 + aw + cdel + T + 2 >= static_cast<double>(n)) dn = false;
    if (up && static_cast<double>(n) - (ls + a0 - T) > static_cast<double>(ring_mask) - 8) up = false;
    // The comb, on every other pixel (chroma is 1.2 MHz wide; per pixel cost twice as much for nothing
    // visible) and interpolated in between below: chroma becomes c * (1 - comb_m) - comb_o.
    const bool comb_on = col && (up || dn);
    if (comb_on) {
      const int64_t Ti = static_cast<int64_t>(T);
      const float Tf = static_cast<float>(T - Ti);
      const cf turn = std::polar(1.f, static_cast<float>(-comb_w * T));  // the line above; below: conj
      const float wk = std::fabs(yscale) / (kCombNone - kCombFull), w1 = kCombNone / (kCombNone - kCombFull);
      const int nc = (o.width + 1) / 2;
      comb_m.resize(nc + 1);
      comb_o.resize(nc + 1);
      const double step = 2 * aw / o.width;
      for (int h = 0; h < nc; h++) {
        comb_m[h] = 0;
        comb_o[h] = cf{};
        const double p = ls + a0 + step * h, pc = p + cdel;
        const int64_t i0 = static_cast<int64_t>(p + 0.5), j0 = static_cast<int64_t>(pc);
        if (j0 + Ti + 3 >= static_cast<int64_t>(n) || static_cast<int64_t>(n) - (i0 - Ti) > static_cast<int64_t>(ring_mask) - 8) continue;
        // a neighbour's weight: 1 while its luma here is within kCombFull of this line's, 0 past kCombNone
        const float y0 = luma_ring[i0 & ring_mask];
        const float wu = up ? std::clamp(w1 - std::fabs(luma_ring[(i0 - Ti) & ring_mask] - y0) * wk, 0.f, 1.f) : 0.f;
        const float wd = dn ? std::clamp(w1 - std::fabs(luma_ring[(i0 + Ti) & ring_mask] - y0) * wk, 0.f, 1.f) : 0.f;
        if (wu + wd <= 0) continue;
        // A neighbour turned onto this line has chroma -c and the same luma detail, so c + the neighbours' mean
        // is twice the detail: take out half of it per unit of the best weight (all of it at 1).
        const float m = 0.5f * std::max(wu, wd), sc = m / (wu + wd);
        const float fc = static_cast<float>(pc - j0);
        // chroma_ring at j + t (0 <= t < 2), in plain floats: branch-free, and std::complex cost more here
        float orr = 0, oi = 0;
        const auto take = [&](int64_t j, float t, float rr, float ri) {
          const int iw = static_cast<int>(t);
          j += iw;
          t -= static_cast<float>(iw);
          const cf a = chroma_ring[j & ring_mask], b = chroma_ring[(j + 1) & ring_mask];
          const float xr = a.real() + (b.real() - a.real()) * t, xi = a.imag() + (b.imag() - a.imag()) * t;
          orr += xr * rr - xi * ri;
          oi += xr * ri + xi * rr;
        };
        if (wu > 0) take(j0 - Ti - 1, fc + 1 - Tf, turn.real() * sc * wu, turn.imag() * sc * wu);
        if (wd > 0) take(j0 + Ti, fc + Tf, turn.real() * sc * wd, -turn.imag() * sc * wd);
        comb_m[h] = m;
        comb_o[h] = cf(orr, oi);
      }
      comb_m[nc] = comb_m[nc - 1];
      comb_o[nc] = comb_o[nc - 1];
    }
]=])
_kp_change(comb native/dsp/video.cpp
  [=[        const cf c = (c0 + (c1 - c0) * fc) * rot * k_uv;
]=]
  [=[        cf c = c0 + (c1 - c0) * fc;
        if (comb_on) {
          const int h = x >> 1;
          if (x & 1) c = c * (1 - 0.5f * (comb_m[h] + comb_m[h + 1])) - 0.5f * (comb_o[h] + comb_o[h + 1]);
          else c = c * (1 - comb_m[h]) - comb_o[h];
        }
        c *= rot * k_uv;
]=])
_kp_change(comb native/dsp/video.cpp
  [=[      rephases++;
]=]
  [=[      rephases++;
      comb_epoch++;
]=])
_kp_change(comb native/tools/kp_replay.cpp
  [=[//   kp_replay REC.cs16 [--offset HZ] [--start S] [--seconds S] [--cnr DB] [--frames] [--woven] [-v]
]=]
  [=[//   kp_replay REC.cs16 [--offset HZ] [--start S] [--seconds S] [--cnr DB] [--frames] [--woven] [--comb] [-v]
]=])
_kp_change(comb native/tools/kp_replay.cpp
  [=[  bool frames = false, woven = false, as20 = false;
]=]
  [=[  bool frames = false, woven = false, as20 = false, comb = false;
]=])
_kp_change(comb native/tools/kp_replay.cpp
  [=[    else if (k == "-v") set_log_debug(true);
    else if (k[0] != '-') file = k;
    else {
      std::fprintf(stderr, "usage: kp_replay REC.cs16 [--offset HZ] [--start S] [--seconds S] [--cnr DB] [--frames] [--woven] [-v]\n");
]=]
  [=[    else if (k == "--comb") comb = true;
    else if (k == "-v") set_log_debug(true);
    else if (k[0] != '-') file = k;
    else {
      std::fprintf(stderr, "usage: kp_replay REC.cs16 [--offset HZ] [--start S] [--seconds S] [--cnr DB] [--frames] [--woven] [--comb] [-v]\n");
]=])
_kp_change(comb native/tools/kp_replay.cpp
  [=[  o.fields = !woven;
]=]
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
