// Banco de pruebas (no se distribuye): abre el adaptador, ve un canal unos segundos, cierra y repite, midiendo
// si llegan bancos de muestras en cada ciclo. Sirve para diagnosticar la reapertura dentro del mismo proceso y
// comparar los backends de libusb en Windows (WinUSB con RAW_IO frente a UsbDk).
//   reopen_test [--cycles N] [--secs S] [--rate 20|40] [--mhz F] [--reuse] [--usbdk] [--stress S]
//     --reuse    reutiliza el mismo kp::Radio (como la app); si no, uno nuevo por ciclo
//     --usbdk    backend UsbDk de libusb (no necesita Zadig; requiere UsbDk instalado)
//     --stress S antes del primer cierre, S segundos a 40 MS/s para provocar fallos de stream
//     --usb2     devolver antes el chip a USB 2 (para reproducir "cambio a USB 3 y abrir en el mismo proceso")
//     --settle S esperar S segundos tras el cambio a USB 3 antes de abrir
//     --dummy    tras el cambio a USB 3, abrir y cerrar una vez sin ver nada antes del primer ciclo
//     --gap S    segundos de espera entre cerrar y volver a abrir (1 por defecto)
//     --timer1   resolucion del temporizador de Windows a 1 ms todo el proceso (timeBeginPeriod)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <libusb.h>

#include "log.h"
#include "radio.h"
#include "usb3_switch.h"
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#include "usb_backend.h"
#endif

using namespace std::chrono_literals;

namespace {
long long json_num(const std::string &js, const char *key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto p = js.find(k);
  return p == std::string::npos ? -1 : std::atoll(js.c_str() + p + k.size());
}
std::string json_str(const std::string &js, const char *key) {
  const std::string k = std::string("\"") + key + "\":\"";
  const auto p = js.find(k);
  if (p == std::string::npos) return "?";
  const auto e = js.find('"', p + k.size());
  return js.substr(p + k.size(), e - p - k.size());
}
}  // namespace

int main(int argc, char **argv) {
  int cycles = 5, rate = 20;
  double secs = 6, mhz = 5800, stress = 0;
  bool reuse = false, usbdk = false, usb2 = false, dummy = false;
  double settle = 0, gap = 1;
  bool timer1 = false;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&] { return i + 1 < argc ? argv[++i] : "0"; };
    if (a == "--cycles") cycles = std::atoi(next());
    else if (a == "--secs") secs = std::atof(next());
    else if (a == "--rate") rate = std::atoi(next());
    else if (a == "--mhz") mhz = std::atof(next());
    else if (a == "--reuse") reuse = true;
    else if (a == "--usbdk") usbdk = true;
    else if (a == "--stress") stress = std::atof(next());
    else if (a == "--usb2") usb2 = true;
    else if (a == "--settle") settle = std::atof(next());
    else if (a == "--dummy") dummy = true;
    else if (a == "--gap") gap = std::atof(next());
    else if (a == "--timer1") timer1 = true;
    else { std::fprintf(stderr, "opcion desconocida %s\n", a.c_str()); return 2; }
  }
#ifdef _WIN32
  if (timer1) std::printf("timeBeginPeriod(1): %u\n", timeBeginPeriod(1));
  if (usbdk) {
    if (!kpd::usbdk_installed()) {
      std::printf("RESULT usbdk=no-disponible\n");
      return 3;
    }
    kpd::set_usb_backend(kpd::UsbBackend::UsbDk);
  }
#endif
  std::printf("config: cycles=%d secs=%.0f rate=%d mhz=%.0f reuse=%d usbdk=%d stress=%.0f\n", cycles, secs, rate,
              mhz, reuse, usbdk, stress);
  std::fflush(stdout);

  if (usb2) {
    std::string d;
    const bool ok2 = kpd::request_usb2_first_adapter(&d);
    std::printf("vuelta a USB 2: %s %s\n", ok2 ? "pedida" : "no", d.c_str());
    for (int i = 0; i < 40; i++) {  // esperar a que reaparezca (en USB 2)
      std::this_thread::sleep_for(250ms);
      if (i > 6 && kpd::adapter_present()) break;
    }
    std::this_thread::sleep_for(1000ms);
  }
  const auto sw = kpd::request_usb3_first_adapter();
  std::printf("cambio a USB 3: %s\n", sw.text.c_str());
  if (sw.requested) kpd::wait_adapter(10000);
  if (settle > 0) std::this_thread::sleep_for(std::chrono::duration<double>(settle));
  if (dummy) {
    kp::Radio r([](const kp::Frame &) {});
    try { r.open(-1); r.close(); std::printf("apertura previa sin ver: ok\n"); }
    catch (const std::exception &e) { std::printf("apertura previa: %s\n", e.what()); }
    for (int i = 0; i < 40 && !kpd::adapter_present(); i++) std::this_thread::sleep_for(250ms);
    std::this_thread::sleep_for(1000ms);
  }

  std::unique_ptr<kp::Radio> radio;
  int ok = 0;
  for (int c = 1; c <= cycles; c++) {
    if (!radio || !reuse) radio = std::make_unique<kp::Radio>([](const kp::Frame &) {});
    const auto t0 = std::chrono::steady_clock::now();
    try {
      radio->open(-1);
    } catch (const std::exception &e) {
      std::printf("ciclo %d: OPEN FALLA: %s\n", c, e.what());
      std::fflush(stdout);
      continue;
    }
    const double open_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (c == 1 && stress > 0) {
      radio->set_watch_rate(1);
      radio->watch(mhz * 1e6);
      std::this_thread::sleep_for(std::chrono::duration<double>(stress));
      const std::string js = radio->status_json();
      std::printf("  stress 40 MS/s %.0f s: restarts=%lld rx=%s fallback=%s\n", stress, json_num(js, "restarts"),
                  json_str(js, "rx_state").c_str(), js.find("\"fallback\":true") != std::string::npos ? "si" : "no");
    }
    radio->set_watch_rate(rate == 40 ? 1 : 2);
    radio->watch(mhz * 1e6 + c * 1e6);  // otra frecuencia cada ciclo: fuerza el retune
    std::this_thread::sleep_for(1500ms);
    const long long b0 = json_num(radio->status_json(), "banks");
    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
    const std::string js = radio->status_json();
    const long long b1 = json_num(js, "banks");
    const double bps = (b1 - b0) / secs;
    const bool good = bps > 1000 && json_str(js, "rx_state") == "streaming";
    ok += good;
    std::printf("ciclo %d: open %.1f s, %s, %.0f bancos/s, restarts=%lld dropped=%lld rx=%s %s\n", c, open_s,
                js.find("\"usb3\":true") != std::string::npos ? "USB3" : "USB2", bps, json_num(js, "restarts"),
                json_num(js, "dropped"), json_str(js, "rx_state").c_str(), good ? "OK" : "FALLA");
    std::fflush(stdout);
    radio->close();
    // UsbDk devuelve el dispositivo a su driver al cerrar y se re-enumera: esperar a que vuelva a estar
    for (int i = 0; i < 40 && !kpd::adapter_present(); i++) std::this_thread::sleep_for(250ms);
    std::this_thread::sleep_for(std::chrono::duration<double>(gap));
  }
  radio.reset();
  std::printf("RESULT %d/%d ciclos con datos\n", ok, cycles);
  return ok == cycles ? 0 : 1;
}
