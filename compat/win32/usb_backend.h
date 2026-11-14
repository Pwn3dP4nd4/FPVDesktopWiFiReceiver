#pragma once
// Windows: backend de libusb para el adaptador.
//   WinUSB: necesita sustituir el driver con Zadig (y Windows reinstala su driver Realtek en cada enchufado).
//   UsbDk:  driver de filtro de Daynix; libusb toma el dispositivo sin cambiar su driver (no hace falta Zadig).
// libusb 1.0.30 no aplica bien LIBUSB_OPTION_USE_USBDK como opcion por defecto en Windows (la aplica antes de
// iniciar el backend), asi que se activa contexto a contexto envolviendo libusb_init al enlazar
// (-Wl,--wrap=libusb_init): wifi-sdr y devourer se compilan sin cambios.
namespace kpd {
enum class UsbBackend { WinUsb, UsbDk };
// Preferencia para los contextos que se creen a partir de ahora. Con UsbDk no instalado se usa WinUSB.
void set_usb_backend(UsbBackend b);
UsbBackend usb_backend();
bool usbdk_installed();           // UsbDk instalado y utilizable por libusb
const char *usb_backend_in_use();  // el del ultimo contexto creado: "WinUSB" / "UsbDk"
}  // namespace kpd
