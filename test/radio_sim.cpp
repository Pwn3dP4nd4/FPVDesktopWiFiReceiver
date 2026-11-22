// SPDX-License-Identifier: GPL-2.0-only
// radio_sim / radio_live: fpv::Radio (src/radio) without an adapter (radio_sim) or with one (radio_live). Derived from kepyur's native/tools/kp_live.cpp
// (https://github.com/gilankpam/kepyur, GPL-2.0); it links kepyur's tools/sim_receiver.cpp in place of
// wsdr::Receiver, which plays a recording back as a live, tunable 8812BU (KP_SIM_FILE: .cs16, 40 MS/s, centre
// 5770 MHz; test/digital_scan_test --write makes synthetic ones). Prints status_json() once a second (stderr).
//   radio_sim [--scan | --survey LO HI | --watch MHZ | --test RATE_SEL SECONDS] [--digital] [--comb] [--rate 1|2]
//             [--record S PREFIX] [--seconds S] [--frames] [--dump FILE] [--plan MHZ,MHZ...] [--usb2] [-v]
//   --plan: escanear solo esos centros de paso (MHz), p. ej. para dejar una senal entre dos pasos
//   radio_live pasa el adaptador a USB 3 antes de abrirlo, como la app; --usb2: dejarlo como este
//   --dump FILE: survey, al acabar escribe cada bin (mhz,mean_db,peak_db,duty) en FILE (CSV), para probar el
//                recomendador sin adaptador
//   ... --frames | ffplay -f rawvideo -pixel_format rgb0 -video_size 720x480 -framerate 60 -fflags nobuffer -i -
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <algorithm>

#include "log.h"
#include "radio.h"
#include "scan_plan.h"
#include "channel_advisor.h"
#ifdef KP_LIVE_ADAPTER
#include "usb3_switch.h"
#endif

#ifndef RADIO_NS
#define RADIO_NS fpv
#endif

static std::atomic<bool> g_stop{false};

int main(int argc, char **argv) {
  std::string mode = "scan";
  double mhz = 5769, secs = 60, run_s = 0;
  int rate = 1;
  bool frames = false, digital = false, comb = false, usb2 = false;
  int watch_rate = 0;
  double rec_s = 0, survey_lo = 0, survey_hi = 0;
  std::string rec_prefix, dump;
  std::vector<double> plan;  // --plan: centros de paso del escaneo (Hz); vacio = el plan completo
  for (int i = 1; i < argc; i++) {
    const std::string k = argv[i];
    if (k == "--scan") mode = "scan";
    else if (k == "--survey" && i + 2 < argc) { mode = "survey"; survey_lo = std::atof(argv[++i]); survey_hi = std::atof(argv[++i]); }
    else if (k == "--watch" && i + 1 < argc) { mode = "watch"; mhz = std::atof(argv[++i]); }
    else if (k == "--test" && i + 2 < argc) { mode = "test"; rate = std::atoi(argv[++i]); secs = std::atof(argv[++i]); }
    else if (k == "--seconds" && i + 1 < argc) run_s = std::atof(argv[++i]);
    else if (k == "--digital") digital = true;
    else if (k == "--rate" && i + 1 < argc) watch_rate = std::atoi(argv[++i]);
    else if (k == "--record" && i + 2 < argc) { rec_s = std::atof(argv[++i]); rec_prefix = argv[++i]; }
    else if (k == "--comb") comb = true;
    else if (k == "--frames") frames = true;
    else if (k == "--dump" && i + 1 < argc) dump = argv[++i];
    else if (k == "--usb2") usb2 = true;
    else if (k == "--plan" && i + 1 < argc) {
      for (const char *p = argv[++i]; *p;) {
        char *end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) break;
        plan.push_back(v * 1e6);
        p = *end == ',' ? end + 1 : end;
      }
    }
    else if (k == "-v") kp::set_log_debug(true);
    else {
      std::fprintf(stderr, "usage: radio_sim [--scan | --survey LO HI | --watch MHZ | --test RATE_SEL SECONDS] [--digital] [--comb] "
                           "[--rate 1|2] [--record S PREFIX] [--seconds S] [--frames] [--dump FILE] [--plan MHZ,MHZ...] "
                           "[--usb2] [-v]\n");
      return 2;
    }
  }
#ifndef KP_LIVE_ADAPTER
  (void)usb2;  // radio_sim: no hay adaptador que pasar a USB 3
#endif
  std::signal(SIGINT, [](int) { g_stop = true; });
  std::signal(SIGTERM, [](int) { g_stop = true; });
  RADIO_NS::Radio radio([&](const kp::Frame &f) {
    if (frames) {
      std::fwrite(f.data, 1, static_cast<size_t>(f.width) * f.height * 4, stdout);
      std::fflush(stdout);
    }
  });
#ifdef KP_LIVE_ADAPTER
  // el RTL8812BU arranca en USB 2: como la app (src/radio_backend.cpp), pedirle USB 3 antes de abrirlo
  if (!usb2) {
    const auto sw = kpd::request_usb3_first_adapter();
    std::fprintf(stderr, "usb3 switch: %s\n", sw.text.c_str());
    bool ss = false;
    if (sw.requested && !kpd::wait_adapter(10000, &ss)) std::fprintf(stderr, "usb3 switch: the adapter did not come back\n");
  }
#endif
  try {
    radio.open(-1);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "open failed: %s\n", e.what());
    return 1;
  }
  radio.set_digital_scan(digital);
  radio.set_comb(comb);
  if (watch_rate) radio.set_watch_rate(watch_rate);
  if (mode == "survey") {
    radio.set_scan_plan(survey_plan_for(survey_lo, survey_hi));  // el mismo plan que la app
    radio.survey();
  } else if (mode == "scan") {
    if (!plan.empty()) radio.set_scan_plan(plan);
    radio.scan();
  }
  else if (mode == "watch") {
    radio.watch(mhz * 1e6);
  }
  else radio.stream_test(rate, secs);
  const auto t0 = std::chrono::steady_clock::now();
  std::string wifi_last;  // survey: redes WiFi vistas (advisor::find_wifi), se imprime cuando cambia
  int loops = 0;
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    // --record: con el canal ya sintonizado (pedido junto con watch(), el cambio de modo lo descarta: abort_record)
    if (mode == "watch" && rec_s > 0 && ++loops == 2) radio.record(rec_s, rec_prefix);
    const std::string st = radio.status_json();
    std::fprintf(stderr, "%s\n", st.c_str());
    if (mode == "survey") {
      int sw = 0;
      std::vector<advisor::Bin> ab;
      for (const auto &b : radio.survey_spectrum(&sw)) ab.push_back(advisor::Bin{b.mhz, b.mean_db, b.peak_db, b.duty});
      std::string w;
      for (const auto &x : advisor::find_wifi(ab)) w += " " + std::to_string(x.ch);
      if (w != wifi_last) {
        std::fprintf(stderr, "wifi @ %.0f s, sweep %d:%s\n",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), sw,
                     w.empty() ? " (none)" : w.c_str());
        wifi_last = w;
      }
    }
    const auto el = std::chrono::steady_clock::now() - t0;
    if (mode == "test" && st.find("\"done\":true") != std::string::npos) break;
    if (mode == "test" && el > std::chrono::duration<double>(secs + 30)) break;
    if (run_s > 0 && el > std::chrono::duration<double>(run_s)) break;
  }
  if (mode == "survey") {
    int sweeps = 0;
    const auto bins = radio.survey_spectrum(&sweeps);
    std::fprintf(stderr, "survey: %d sweep(s), %zu bins\n", sweeps, bins.size());
    if (!dump.empty()) {
      if (FILE *f = std::fopen(dump.c_str(), "w")) {
        std::fprintf(f, "mhz,mean_db,peak_db,duty\n");
        for (const auto &b : bins) std::fprintf(f, "%.5f,%.3f,%.3f,%.5f\n", b.mhz, b.mean_db, b.peak_db, b.duty);
        std::fclose(f);
        std::fprintf(stderr, "survey: %zu bins -> %s\n", bins.size(), dump.c_str());
      }
    }
    for (double f = std::floor(survey_lo / 10) * 10; f < survey_hi; f += 10) {  // cada 10 MHz: media, pico, duty
      double m = -1e9, pk = -1e9, du = 0;
      int n = 0;
      for (const auto &b : bins)
        if (b.mhz >= f && b.mhz < f + 10) {
          m = std::max<double>(m, b.mean_db);
          pk = std::max<double>(pk, b.peak_db);
          du += b.duty;
          n++;
        }
      if (n) std::fprintf(stderr, "  %4.0f-%4.0f MHz: mean max %5.1f dB, peak %5.1f dB, duty %.2f\n", f, f + 10, m, pk, du / n);
    }
    // cada piloto en el aire: como puntua el recomendador su canal y los de al lado, sin y con "es mio"
    // (advisor::mask_signal, lo que hace la ventana al marcarlo)
    std::vector<advisor::Bin> ab;
    for (const auto &b : bins) ab.push_back(advisor::Bin{b.mhz, b.mean_db, b.peak_db, b.duty});
    // redes WiFi y lo que pesan en cada canal de Raceband (y la recomendacion para 4 analogicos, sin pilotos)
    const auto wifi = advisor::find_wifi(ab);
    for (const auto &w : wifi)
      std::fprintf(stderr, "wifi ch %d (%.0f MHz): peak %.1f dB, duty %.3f, block %.0f-%.0f MHz\n", w.ch, w.mhz,
                   w.peak_db, w.duty, w.block_lo, w.block_hi);
    for (const auto &c : advisor::analog_channels("R")) {
      const auto r = advisor::rate(c, ab, {}, wifi);
      std::fprintf(stderr, "  %-3s %4.0f MHz: noise %4.1f dB, burst %3.0f %%, wifi %4.1f dB%s%s, cost %4.1f\n",
                   c.name.c_str(), c.mhz, r.noise_db, r.burst * 100, r.wifi_cost,
                   r.wifi >= 0 ? (" (ch " + std::to_string(wifi[r.wifi].ch) + ")").c_str() : "",
                   r.wifi_block ? " block" : "", r.cost);
    }
    {
      const auto a = advisor::advise(ab, {}, advisor::analog_channels("R"), {}, 4, 0);
      std::string s;
      for (const auto &p : a.picks) s += " " + p.r.ch.name;
      std::fprintf(stderr, "advice, 4 analog on R (ignoring pilots):%s\n", s.c_str());
    }
    const auto chans = advisor::analog_channels("ABEFR");
    for (const auto &p : radio.pilots()) {
      const advisor::Signal sig{p.mhz, p.bw_mhz, !p.digital.empty(), p.name};
      auto masked = ab;
      advisor::mask_signal(masked, sig);
      std::fprintf(stderr, "pilot %s %.1f MHz (%.0f dB): channels within 45 MHz, noise without / with \"mine\"\n",
                   p.name.c_str(), p.mhz, p.snr_db);
      for (const auto &c : chans)
        if (std::fabs(c.mhz - p.mhz) < 45)
          std::fprintf(stderr, "  %-3s %4.0f MHz: %5.1f dB -> %5.1f dB\n", c.name.c_str(), c.mhz,
                       advisor::rate(c, ab, {}).noise_db, advisor::rate(c, masked, {}).noise_db);
    }
  }
  radio.close();
  return 0;
}
