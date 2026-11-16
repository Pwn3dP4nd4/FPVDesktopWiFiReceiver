// SPDX-License-Identifier: GPL-2.0-only
// radio_sim: fpv::Radio (src/radio) without an adapter. Derived from kepyur's native/tools/kp_live.cpp
// (https://github.com/gilankpam/kepyur, GPL-2.0); it links kepyur's tools/sim_receiver.cpp in place of
// wsdr::Receiver, which plays a recording back as a live, tunable 8812BU (KP_SIM_FILE: .cs16, 40 MS/s, centre
// 5770 MHz; test/digital_scan_test --write makes synthetic ones). Prints status_json() once a second (stderr).
//   radio_sim [--scan | --watch MHZ | --test RATE_SEL SECONDS] [--digital] [--comb] [--seconds S] [--frames] [-v]
//   ... --frames | ffplay -f rawvideo -pixel_format rgb0 -video_size 720x480 -framerate 60 -fflags nobuffer -i -
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

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
  for (int i = 1; i < argc; i++) {
    const std::string k = argv[i];
    if (k == "--scan") mode = "scan";
    else if (k == "--watch" && i + 1 < argc) { mode = "watch"; mhz = std::atof(argv[++i]); }
    else if (k == "--test" && i + 2 < argc) { mode = "test"; rate = std::atoi(argv[++i]); secs = std::atof(argv[++i]); }
    else if (k == "--seconds" && i + 1 < argc) run_s = std::atof(argv[++i]);
    else if (k == "--digital") digital = true;
    else if (k == "--comb") comb = true;
    else if (k == "--frames") frames = true;
    else if (k == "-v") kp::set_log_debug(true);
    else {
      std::fprintf(stderr, "usage: radio_sim [--scan | --watch MHZ | --test RATE_SEL SECONDS] [--digital] [--comb] "
                           "[--seconds S] [--frames] [-v]\n");
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
  if (mode == "scan") radio.scan();
  else if (mode == "watch") radio.watch(mhz * 1e6);
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
  radio.close();
  return 0;
}
