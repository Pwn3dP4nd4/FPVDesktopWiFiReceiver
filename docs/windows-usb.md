# Windows y USB: investigación (2026-11-14)

Adaptador: BrosTrend AC1200, RTL8812BU `0bda:b812`, puerto USB 3, Windows 11, libusb 1.0.30 (MSYS2).
Banco de pruebas: `test/reopen_test.cpp` (`cmake --build build/release --target reopen_test`).

## 1. Windows reinstala su driver Realtek en cada enchufado

- El driver que gana es `netrtwlanu.inf`, de serie en Windows y firmado por Microsoft: no se puede borrar
  del almacén de drivers y Windows lo prefiere al WinUSB de Zadig (que queda en el almacén como
  `oem100..103.inf`, proveedor libwdi).
- Comprobado: tras desenchufar y enchufar, el dispositivo vuelve a tener `RtlWlanu`. Hay que pasar Zadig
  dos veces (instancia USB 2 y, tras el cambio a USB 3 que hace la app, instancia USB 3).

## 2. UsbDk como alternativa a Zadig

[UsbDk](https://github.com/daynix/UsbDk) (Daynix) es un driver de filtro: libusb toma el dispositivo sin
cambiarle el driver. Se activa con `compat/win32/usb_backend.cpp`, que envuelve `libusb_init`
(`-Wl,--wrap`) para poner `LIBUSB_OPTION_USE_USBDK` en cada contexto.

- **No sirve la opción global** `libusb_set_option(NULL, LIBUSB_OPTION_USE_USBDK)` en libusb 1.0.30: la
  aplica antes de iniciar el backend de Windows, así que falla todo `libusb_init` (si UsbDk aún no se ha
  detectado en el proceso) o el backend la sobrescribe con WinUSB.
- **Funciona sin Zadig**, con el driver Realtek instalado: cambio a USB 3, arranque del chip y streaming a
  20 MS/s a ritmo completo (unos 2.445 bloques/s, 0 pérdidas).
- **Problema serio**: al cerrar, UsbDk devuelve el adaptador al driver Realtek, que intenta inicializar
  el chip y falla (registro de Sistema, RtlWlanu 5006: "el número de la versión no es correcto"). Dos veces
  el adaptador acabó cayéndose del bus y solo volvió desenchufándolo. Con WinUSB el driver de Windows no
  toca el chip y no se observó ninguna caída.

## 3. Fallo intermitente al arrancar el stream

Síntoma: `no first packet ... state 1 seq 0 ... r7c0 800000cb`: el programa del chip corre y el analizador
está armado, pero no envía ningún bloque; wifi-sdr reintenta y se rinde (`stream keeps failing`).

| Prueba (aperturas dentro del mismo proceso) | Correctas |
|---|---|
| UsbDk, 4 ciclos | 4/4 |
| UsbDk, vuelta a USB 2 y cambio a USB 3, 3 ciclos | 2/3 |
| UsbDk, 10 ciclos | 5/10 |
| UsbDk, 10 ciclos sin reset USB (`KP_NO_RESET=1`) | 7/10 |
| UsbDk, 8 ciclos con 12 s entre cierre y apertura | 6/8 |
| UsbDk, 10 ciclos con temporizador de Windows a 1 ms | 2/10 (y el adaptador se cayó del bus) |
| WinUSB, procesos nuevos (una apertura cada uno, lanzamientos de la app) | 8/8 |
| WinUSB, reapertura en el mismo proceso tras fallos o desenchufado | 0/3 |

- La primera apertura de cada proceso no ha fallado nunca (12/12 entre ambos backends).
- Descartado como causa: el reset USB, la espera entre cierre y apertura, la resolución del temporizador,
  el canal sintonizado.
- No resuelto: la causa está en el arranque del streamer de wifi-sdr. Su FINDINGS describe un bloqueo
  parecido al arrancar el CPU del chip ("issue #2"), del orden de 1 vez de cada 20, que wifi-sdr mitiga
  con reintentos; aquí aparece en rachas.
- Sin comprobar: si un proceso nuevo lanzado justo después de cerrar el anterior falla igual (la prueba
  quedó a medias porque el adaptador se cayó del bus).

## Recomendación actual

- Mantener WinUSB + RAW_IO (lo único estable medido) y no activar UsbDk en la app.
- Evitar reabrir dentro del mismo proceso: la app ya abre una vez por sesión; la recuperación y la
  reconexión son las que reabren y son las que fallan.
