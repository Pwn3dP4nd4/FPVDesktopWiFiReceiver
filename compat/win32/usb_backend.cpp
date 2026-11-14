#include "usb_backend.h"

#include <libusb.h>

#include <atomic>

#include "log.h"

extern "C" int LIBUSB_CALL __real_libusb_init(libusb_context **ctx);

namespace kpd {
namespace {
std::atomic<UsbBackend> g_want{UsbBackend::WinUsb};
std::atomic<int> g_in_use{0};  // 0 WinUSB, 1 UsbDk
std::atomic<int> g_usbdk{-1};  // -1 sin comprobar, 0 no, 1 si
}  // namespace

void set_usb_backend(UsbBackend b) { g_want = b; }
UsbBackend usb_backend() { return g_want; }
const char *usb_backend_in_use() { return g_in_use ? "UsbDk" : "WinUSB"; }

bool usbdk_installed() {
  if (g_usbdk < 0) {
    libusb_context *ctx = nullptr;
    int ok = 0;
    if (__real_libusb_init(&ctx) == 0) {
      ok = libusb_set_option(ctx, LIBUSB_OPTION_USE_USBDK) == LIBUSB_SUCCESS;
      libusb_exit(ctx);
    }
    g_usbdk = ok;
  }
  return g_usbdk == 1;
}
}  // namespace kpd

// Todos los libusb_init del programa (wifi-sdr, devourer y los nuestros) pasan por aqui.
extern "C" int LIBUSB_CALL __wrap_libusb_init(libusb_context **ctx) {
  const int r = __real_libusb_init(ctx);
  if (r != 0 || !ctx || !*ctx) return r;
  int dk = 0;
  if (kpd::g_want == kpd::UsbBackend::UsbDk) {
    // Justo tras libusb_init y antes de usar el contexto, como pide libusb.
    dk = libusb_set_option(*ctx, LIBUSB_OPTION_USE_USBDK) == LIBUSB_SUCCESS;
    if (!dk) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) KP_LOGW("usb: UsbDk pedido pero no disponible; se usa WinUSB");
    }
  }
  if (kpd::g_in_use.exchange(dk) != dk) KP_LOGI("usb: backend %s", dk ? "UsbDk" : "WinUSB");
  return r;
}

