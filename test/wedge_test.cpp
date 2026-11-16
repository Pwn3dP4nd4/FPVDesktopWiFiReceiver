// Banco de pruebas (no se distribuye): emite sin parar a 20 (o 40) MS/s con wifi-sdr directamente (Session +
// Stream8822b, sin fpv::Radio) hasta que el flujo se atasca ("no packet within timeout (bank 0 part 0)": la
// peticion C2H del chip se queda colgada, r294 BIT16) y entonces prueba recuperaciones de menor a mayor coste,
// cada una seguida de un nuevo arranque del flujo, para saber cual es la mas barata que lo devuelve.
//   wedge_test [--secs S] [--rate 20|40] [--mhz F] [--wedges N] [--from K]
//     --secs   tiempo total maximo (600 por defecto)
//     --wedges parar tras N atascos (3)
//     --from K empezar la escalera por el paso K (0 = reintentar sin hacer nada)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <libusb.h>

#include "ChannelFreq.h"
#include "SelectedChannel.h"
#include "logger.h"
#include "receiver.h"
#include "session.h"
#include "stream88xx.h"
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

namespace {
double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

struct Run {
  uint64_t banks = 0;
  std::string error;
  double secs = 0;
};

// Emite hasta `max_s` segundos (o hasta un fallo). Devuelve bancos y error.
Run stream(wsdr::Session &s, int rate_sel, double max_s) {
  wsdr::StreamCfg c;
  c.la.rate_sel = rate_sel;
  c.depth = rate_sel <= 2 ? 256 : 64;
  c.la.igi = -1;
  c.la.no_cca = false;
  c.la.dfir = 0;
  c.la.dma_type = wsdr::kDmaBothPaths8822b;
  Run r;
  const auto t0 = clk::now();
  wsdr::StreamStats ss;
  try {
    ss = wsdr::Stream8822b(s, c).run([&](const uint8_t *, const wsdr::BankInfo &) {
      r.banks++;
      return since(t0) < max_s;
    });
  } catch (const std::exception &e) {
    ss.error = e.what();
  }
  r.error = ss.error;
  r.secs = since(t0);
  return r;
}
}  // namespace

int main(int argc, char **argv) {
  double secs = 600, mhz = 5800;
  int rate = 20, wedges = 3, from = 0;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&] { return i + 1 < argc ? argv[++i] : "0"; };
    if (a == "--secs") secs = std::atof(next());
    else if (a == "--rate") rate = std::atoi(next());
    else if (a == "--mhz") mhz = std::atof(next());
    else if (a == "--wedges") wedges = std::atoi(next());
    else if (a == "--from") from = std::atoi(next());
    else { std::fprintf(stderr, "opcion desconocida %s\n", a.c_str()); return 2; }
  }
#ifdef _WIN32
  timeBeginPeriod(1);
#endif
  const int rate_sel = rate == 40 ? 1 : 2;
  const int width = 40;
  const int ch = wsdr::Receiver::channel_for(mhz * 1e6, width);
  auto log = std::make_shared<Logger>();
  wsdr::Session s(log);
  wsdr::OpenOpts o;
  o.pid = wsdr::kPid8812bu;
  o.any_board = true;
  o.channel = ch;
  o.bw = width;
  o.thermal_track = false;
  auto t = clk::now();
  s.open(o);
  std::printf("open %.1f s, ch %d bw %d, %s, %d MS/s\n", since(t), ch, width, s.superspeed() ? "USB3" : "USB2", rate);
  std::fflush(stdout);
  if (!s.superspeed()) std::printf("AVISO: USB 2, el flujo a 20 MS/s no cabe\n");

  libusb_device_handle *h = s.handle();
  struct Step {
    const char *name;
    std::function<bool()> fn;  // false: la sesion ya no sirve (no seguir)
  };
  const std::vector<Step> steps = {
      {"reintentar", [] { return true; }},
      {"c2h=0 (0x294)", [&] { s.reg().w32(0x0294, 0); return true; }},
      {"clear_halt 0x84", [&] {
         std::printf("    clear_halt rc=%d\n", libusb_clear_halt(h, 0x84));
         return true;
       }},
      {"CR rxdma off/on", [&] {
         const uint16_t cr = s.reg().r16(0x0100);
         s.reg().w16(0x0100, cr & ~uint16_t(0x000A));  // HCI_RXDMA_EN | RXDMA_EN
         std::this_thread::sleep_for(2ms);
         s.reg().w16(0x0100, cr);
         std::printf("    CR %04x\n", s.reg().r16(0x0100));
         return true;
       }},
      {"0x284 RW_RELEASE pulso", [&] {
         s.reg().m32(0x0284, 1u << 18, 1u << 18);
         const auto p = clk::now();
         while (!(s.reg().r32(0x0284) & (1u << 17)) && since(p) < 0.02) {}
         std::printf("    0x284 %08x tras %.1f ms\n", s.reg().r32(0x0284), since(p) * 1e3);
         s.reg().m32(0x0284, 1u << 18, 0);
         return true;
       }},
      {"tune (SetMonitorChannel)", [&] {
         s.tune(ch == 158 ? 159 : ch + 1, width);
         s.tune(ch, width);
         return true;
       }},
      {"libusb_reset_device", [&] {
         const int rc = libusb_reset_device(h);
         std::printf("    reset rc=%d (%s)\n", rc, libusb_error_name(rc));
         if (rc != 0) return false;
         std::printf("    claim rc=%d, CR %04x\n", libusb_claim_interface(h, 0), s.reg().r16(0x0100));
         return true;
       }},
      {"InitWrite completo", [&] {
         s.radio()->InitWrite(SelectedChannel{.Channel = static_cast<uint8_t>(ch), .ChannelOffset = 1,
                                              .ChannelWidth = CHANNEL_WIDTH_40});
         return true;
       }},
  };

  const auto T0 = clk::now();
  int seen = 0;
  uint64_t total = 0;
  while (since(T0) < secs && seen < wedges) {
    Run r = stream(s, rate_sel, secs - since(T0));
    total += r.banks;
    if (r.error.empty()) break;
    std::printf("[%.0f s] FALLO tras %llu bancos (%.1f s): %s\n", since(T0), (unsigned long long)r.banks, r.secs,
                r.error.c_str());
    std::fflush(stdout);
    // escalera de recuperacion: cada paso, luego un arranque de prueba de 1 s
    bool ok = false;
    for (size_t k = from; k < steps.size() && !ok; k++) {
      const auto ts = clk::now();
      bool alive = true;
      try {
        alive = steps[k].fn();
      } catch (const std::exception &e) {
        std::printf("    paso lanzo: %s\n", e.what());
      }
      const double step_s = since(ts);
      if (!alive) {
        std::printf("  paso %zu %-26s: la sesion no sigue, fin\n", k, steps[k].name);
        return 1;
      }
      Run p = stream(s, rate_sel, 1.0);
      ok = p.banks > 1000 && p.error.empty();
      std::printf("  paso %zu %-26s (%.2f s): %llu bancos %s%s\n", k, steps[k].name, step_s,
                  (unsigned long long)p.banks, ok ? "RECUPERA" : "no", p.error.empty() ? "" : (" - " + p.error).c_str());
      std::fflush(stdout);
      if (k == 0 && ok) std::printf("  (fallo pasajero, no un atasco)\n");
      if (k == 0 && ok) seen--;  // no cuenta como atasco
    }
    seen++;
    if (!ok) {
      std::printf("RESULT nada recupera\n");
      return 1;
    }
  }
  std::printf("RESULT %d atasco(s) en %.0f s, %llu bancos\n", seen, since(T0), (unsigned long long)total);
  return 0;
}
