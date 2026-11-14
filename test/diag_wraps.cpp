// Envoltorios solo para el banco de pruebas (test/reopen_test.cpp), enlazados con -Wl,--wrap=...
#include <libusb.h>

#include <cstdlib>

#include "log.h"

// Diagnostico (solo el banco de pruebas lo enlaza): KP_NO_RESET=1 convierte el reset USB de devourer en no-op.
extern "C" int LIBUSB_CALL __real_libusb_reset_device(libusb_device_handle *h);
extern "C" int LIBUSB_CALL __wrap_libusb_reset_device(libusb_device_handle *h) {
  if (const char *e = std::getenv("KP_NO_RESET"); e && e[0] == '1') {
    KP_LOGI("usb: reset omitido (KP_NO_RESET=1)");
    return 0;
  }
  return __real_libusb_reset_device(h);
}
