// SPDX-License-Identifier: GPL-2.0-only
// radio_sim / radio_live: fpv::Radio (src/radio) without an adapter (radio_sim) or with one (radio_live). Derived from kepyur's native/tools/kp_live.cpp
// (https://github.com/gilankpam/kepyur, GPL-2.0); it links kepyur's tools/sim_receiver.cpp in place of
// wsdr::Receiver, which plays a recording back as a live, tunable 8812BU (KP_SIM_FILE: .cs16, 40 MS/s, centre
// 5770 MHz; test/digital_scan_test --write makes synthetic ones). Prints status_json() once a second (stderr).
//   radio_sim [--scan | --survey LO HI | --watch MHZ | --test RATE_SEL SECONDS] [--digital] [--comb] [--rate 1|2]
//             [--record S PREFIX] [--seconds S] [--frames] [-v]
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

#ifndef RADIO_NS
#define RADIO_NS fpv
#endif

static std::atomic<bool> g_stop{false};

int main(int argc, char **argv) {
  std::string mode = "scan";
  double mhz = 5769, secs = 60, run_s = 0;
  int rate = 1;
  bool frames = false, digital = false, comb = false;
  int watch_rate = 0;
  double rec_s = 0, survey_lo = 0, survey_hi = 0;
  std::string rec_prefix;
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
    else if (k == "-v") kp::set_log_debug(true);
    else {
      std::fprintf(stderr, "usage: radio_sim [--scan | --survey LO HI | --watch MHZ | --test RATE_SEL SECONDS] [--digital] [--comb] "
                           "[--rate 1|2] [--record S PREFIX] [--seconds S] [--frames] [-v]\n");
      return 2;
    }
  }
  std::signal(SIGINT, [](int) { g_stop = true; });
  std::signal(SIGTERM, [](int) { g_stop = true; });
  RADIO_NS::Radio radio([&](const kp::Frame &f) {
    if (frames) {
      std::fwrite(f.data, 1, static_cast<size_t>(f.width) * f.height * 4, stdout);
      std::fflush(stdout);
    }
  });
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
    std::vector<double> plan;  // como survey_plan_for() (src/scan_plan.cpp): pasos cada 30 MHz
    for (double c = survey_lo + 15; c - 15 < survey_hi; c += 30) plan.push_back(c * 1e6);
    radio.set_scan_plan(plan);
    radio.survey();
  } else if (mode == "scan") radio.scan();
  else if (mode == "watch") {
    radio.watch(mhz * 1e6);
    if (rec_s > 0) radio.record(rec_s, rec_prefix);
  }
  else radio.stream_test(rate, secs);
  const auto t0 = std::chrono::steady_clock::now();
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const std::string st = radio.status_json();
    std::fprintf(stderr, "%s\n", st.c_str());
    const auto el = std::chrono::steady_clock::now() - t0;
    if (mode == "test" && st.find("\"done\":true") != std::string::npos) break;
    if (mode == "test" && el > std::chrono::duration<double>(secs + 30)) break;
    if (run_s > 0 && el > std::chrono::duration<double>(run_s)) break;
  }
  if (mode == "survey") {
    int sweeps = 0;
    const auto bins = radio.survey_spectrum(&sweeps);
    std::fprintf(stderr, "survey: %d sweep(s), %zu bins\n", sweeps, bins.size());
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
  }
  radio.close();
  return 0;
}
