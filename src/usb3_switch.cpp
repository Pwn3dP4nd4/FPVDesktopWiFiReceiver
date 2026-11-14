// Sin Qt a proposito: las cabeceras de devourer (via wifi-sdr) usan identificadores que Qt define como
// macros (emit, signals, slots...).
#include "usb3_switch.h"

#include <libusb.h>

#include <chrono>
#include <thread>

#include "log.h"
#include "receiver.h"
#include "usbmode.h"

namespace kpd {

namespace {
// Primer adaptador RTL8812BU del bus: bus, ruta de puertos ("1.2") y si ya va a SuperSpeed.
bool find_8812bu(int &bus, std::string &port, bool &superspeed) {
  libusb_context *ctx = nullptr;
  if (libusb_init(&ctx) != 0) return false;
  libusb_device **list = nullptr;
  const ssize_t n = libusb_get_device_list(ctx, &list);
  bool found = false;
  for (ssize_t i = 0; i < n && !found; i++) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
    if (wsdr::model_pid(dd.idVendor, dd.idProduct) != wsdr::kPid8812bu) continue;
    uint8_t p[8];
    const int np = libusb_get_port_numbers(list[i], p, sizeof p);
    port.clear();
    for (int k = 0; k < np; k++) port += (k ? "." : "") + std::to_string(p[k]);
    bus = libusb_get_bus_number(list[i]);
    superspeed = libusb_get_device_speed(list[i]) >= LIBUSB_SPEED_SUPER;
    found = true;
  }
  if (list) libusb_free_device_list(list, 1);
  libusb_exit(ctx);
  return found;
}
}  // namespace

Usb3Result request_usb3_first_adapter() {
  Usb3Result out;
  int bus = -1;
  std::string port;
  bool ss = false;
  if (!find_8812bu(bus, port, ss)) {
    out.text = "no hay ningun RTL8812BU en el bus";
  } else if (ss) {
    out.text = "ya en USB 3";
  } else {
    std::string detail;
    const auto r = wsdr::request_usb3(wsdr::kPid8812bu, bus, port, &detail);
    out.requested = r == wsdr::Usb3Switch::Requested;
    out.text = wsdr::usb3_switch_name(r);
    if (!detail.empty()) out.text += ": " + detail;
  }
  KP_LOGI("usb3 switch (bus %d port %s): %s", bus, port.c_str(), out.text.c_str());
  return out;
}

bool wait_adapter(int timeout_ms, bool *superspeed) {
  const auto t_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // dejar que desaparezca el dispositivo USB 2
  do {
    int bus;
    std::string port;
    bool ss = false;
    if (find_8812bu(bus, port, ss) && ss) {
      if (superspeed) *superspeed = true;
      KP_LOGI("adaptador re-enumerado en USB 3 (bus %d port %s)", bus, port.c_str());
      std::this_thread::sleep_for(std::chrono::milliseconds(500));  // driver WinUSB / udev
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  } while (std::chrono::steady_clock::now() < t_end);
  int bus;
  std::string port;
  bool ss = false;
  const bool any = find_8812bu(bus, port, ss);
  if (superspeed) *superspeed = ss;
  KP_LOGW("el adaptador no volvio en USB 3 (%s): puerto o cable USB 2?", any ? "sigue en USB 2" : "no aparece");
  return any;
}

}  // namespace kpd
