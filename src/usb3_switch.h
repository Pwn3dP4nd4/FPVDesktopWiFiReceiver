#pragma once
#include <string>

namespace kpd {
struct Usb3Result {
  bool requested = false;  // el chip se esta re-enumerando como USB 3: esperar unos segundos antes de abrir
  std::string text;
};
// Como hace la app Android antes de abrir: pasar el primer RTL8812BU del bus de USB 2 a USB 3 si hace falta.
Usb3Result request_usb3_first_adapter();
// Tras un cambio pedido: esperar a que el adaptador reaparezca (true si esta en el bus; *superspeed = USB 3).
bool wait_adapter(int timeout_ms, bool *superspeed = nullptr);
}  // namespace kpd
