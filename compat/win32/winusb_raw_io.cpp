// Windows: WinUSB RAW_IO on the C2H bulk-IN endpoint (0x84) that wifi-sdr streams IQ from.
//
// Without RAW_IO, WinUSB queues read requests internally and hands them to the host controller one at a time,
// so the chip finds no URB posted between two transfers and misses its bank deadline at 40 MS/s
// ("deadline: bank 0 ended ... us after due"). With RAW_IO the posted transfers go straight to the USB stack,
// like usbfs on Linux/Android. libusb >= 1.0.30 exposes it (libusb_endpoint_set_raw_io).
//
// wifi-sdr/devourer are compiled unmodified: the link wraps libusb_submit_transfer (-Wl,--wrap) and turns
// RAW_IO on the first time a stream transfer is submitted on a handle (InOrderReader::start(), with nothing
// in flight on 0x84). RAW_IO rules: length a multiple of the max packet size and <= the max RAW_IO size;
// wifi-sdr's stream reads are 16384 bytes. Other lengths on 0x84 leave RAW_IO off for that handle.
#include <libusb.h>

#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "log.h"

extern "C" int LIBUSB_CALL __real_libusb_submit_transfer(struct libusb_transfer *t);
extern "C" void LIBUSB_CALL __real_libusb_close(libusb_device_handle *h);

namespace {
constexpr unsigned char kStreamEp = 0x84;
std::mutex g_m;
std::unordered_map<libusb_device_handle *, bool> g_seen;  // handle -> RAW_IO decision taken

void maybe_enable_raw_io(libusb_device_handle *h, int length) {
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x0100010C
  {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_seen.emplace(h, true).second) return;
  }
  if (const char *e = std::getenv("KP_RAW_IO"); e && e[0] == '0') {
    KP_LOGI("winusb: RAW_IO disabled by KP_RAW_IO=0");
    return;
  }
  if (libusb_endpoint_supports_raw_io(h, kStreamEp) != 1) {
    KP_LOGW("winusb: RAW_IO not supported on ep 0x84 (driver not WinUSB?)");
    return;
  }
  const int mps = libusb_get_max_packet_size(libusb_get_device(h), kStreamEp);
  const int maxraw = libusb_get_max_raw_io_transfer_size(h, kStreamEp);
  if (mps <= 0 || length % mps != 0 || (maxraw > 0 && length > maxraw)) {
    KP_LOGW("winusb: RAW_IO left off (len %d, max packet %d, max raw %d)", length, mps, maxraw);
    return;
  }
  const int rc = libusb_endpoint_set_raw_io(h, kStreamEp, 1);
  KP_LOGI("winusb: RAW_IO on ep 0x84 (len %d, max packet %d): %s", length, mps, rc == 0 ? "on" : libusb_error_name(rc));
#else
  (void)h;
  (void)length;
  static bool warned = false;
  if (!warned) {
    warned = true;
    KP_LOGW("winusb: libusb < 1.0.30 has no RAW_IO; 40 MS/s will likely fail on Windows");
  }
#endif
}
}  // namespace

extern "C" int LIBUSB_CALL __wrap_libusb_submit_transfer(struct libusb_transfer *t) {
  if (t && t->dev_handle && t->endpoint == kStreamEp && t->type == LIBUSB_TRANSFER_TYPE_BULK)
    maybe_enable_raw_io(t->dev_handle, t->length);
  return __real_libusb_submit_transfer(t);
}

extern "C" void LIBUSB_CALL __wrap_libusb_close(libusb_device_handle *h) {
  {
    std::lock_guard<std::mutex> l(g_m);
    g_seen.erase(h);
  }
  __real_libusb_close(h);
}
