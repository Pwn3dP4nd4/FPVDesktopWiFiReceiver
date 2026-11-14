# Windows y USB: investigación (2026-11-14)

Adaptador: BrosTrend AC1200, RTL8812BU `0bda:b812`, puerto USB 3, Windows 11, libusb 1.0.30 (MSYS2).
Banco de pruebas: `test/reopen_test.cpp` (`cmake --build build/release --target reopen_test`).

## 1. Windows reinstala su driver Realtek en cada enchufado

- El driver que gana es `netrtwlanu.inf`, de serie en Windows y firmado por Microsoft: no se puede borrar
  del almacén de drivers y Windows lo prefiere al WinUSB de Zadig (que queda en el almacén como
  `oem100..103.inf`, proveedor libwdi).
- Comprobado: tras desenchufar y enchufar, el dispositivo vuelve a tener `RtlWlanu`. Hay que pasar Zadig
  dos veces (instancia USB 2 y, tras el cambio a USB 3 que hace la app, instancia USB 3).
- **Causa** (`pnputil /enum-devices /instanceid "USB\VID_0BDA&PID_B812S456" /drivers`): el driver
  Realtek y el WinUSB de Zadig empatan en clasificación (`00FF0001`, mismo ID coincidente). En empate
  Windows elige el de fecha más reciente: Realtek 2019-07-29 frente a WinUSB de libwdi 2012-06-02 (la
  fecha de WinUSB que Zadig copia en el INF). Por eso **cualquier re-enumeración** (desenchufar, o el cambio
  USB 2 / USB 3) vuelve a poner el Realtek. Cada pasada de Zadig deja otra copia en el almacén
  (`oem100.inf`...`oem109.inf`).
- Solución posible: un paquete WinUSB propio con fecha posterior a 2019, instalado una vez; Windows lo
  preferiría en cada enumeración y no haría falta Zadig nunca más.

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
- **Con UsbDk instalado (aunque no se use) el cambio a USB 3 deja de funcionar**: el chip se re-enumera en
  el puerto SuperSpeed y Windows no puede leer su descriptor ("Dispositivo USB desconocido (Error de
  solicitud de descriptor de dispositivo)"). Tras desinstalar UsbDk y reiniciar, el cambio vuelve a
  funcionar al instante.

## 3. Fallo intermitente al arrancar el stream

Síntoma: `no first packet ... state 1 seq 0 ... r7c0 800000cb`: el programa del chip corre y el analizador
está armado, pero no envía ningún bloque; wifi-sdr reintenta y se rinde (`stream keeps failing`).

| Prueba | Correctas |
|---|---|
| UsbDk, aperturas en el mismo proceso (5 tandas, 35 ciclos) | 24/35 |
| UsbDk, idem sin reset USB / 12 s entre ciclos / temporizador a 1 ms | 7/10, 6/8, 2/10 |
| WinUSB (sin UsbDk instalado), 6 procesos nuevos seguidos | 6/6 |
| WinUSB, 6 reaperturas en el mismo proceso | 6/6 |
| WinUSB, 20 s a 40 MS/s (4 fallos de stream) y 4 reaperturas en el mismo proceso | 4/4 |
| WinUSB, reconexión en el mismo proceso tras desenchufar en pleno stream (dos veces, con y sin UsbDk instalado) | 0/2 |
| WinUSB, proceso nuevo después de esa reconexión fallida | 0/3 |

- Los fallos en rachas eran de UsbDk: con WinUSB, reabrir en el mismo proceso funciona (10/10), también
  después de fallos de stream.
- **Desenchufar en pleno stream deja el chip mal** (no el proceso): tras reconectar, la app recibió 85
  bloques, llegó un paquete truncado (15.528 bytes) y desde ahí ninguno; un proceso nuevo tampoco recibe
  nada. Relanzar la app no lo arregla. Lo único comprobado que lo recupera es cortar la alimentación
  (desenchufar). Probar un "apagado por software" (cambio USB 3 -> USB 2 -> USB 3) no fue posible porque
  la re-enumeración vuelve a poner el driver Realtek.
- Descartado como causa: el reset USB, la espera entre cierre y apertura, la resolución del temporizador,
  el canal sintonizado.

## Recomendación actual

- Mantener WinUSB + RAW_IO (lo único estable medido) y no activar UsbDk en la app.
- No instalar UsbDk: además de no ser fiable, rompe el cambio a USB 3 mientras está instalado.
- La recuperación tras fallos de stream (reabrir en el mismo proceso) es válida con WinUSB.
- Relanzar la app para recuperar no sirve (descartado con datos).
- Lo que desbloquearía la reconexión automática es el paquete WinUSB con fecha reciente: con él, la app
  podría recuperar el chip con el cambio USB 2 / USB 3 sin que Windows le cambie el driver.
