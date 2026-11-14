# FPVDesktopWiFiReceiver (kepyur-desktop)

Receptor de vídeo FPV analógico de 5,8 GHz para **Windows y Linux** usando un adaptador WiFi USB **RTL8812BU**
como SDR. Es la versión de escritorio (Qt 6) de [kepyur](https://github.com/gilankpam/kepyur) (de
gilankpam): usa **exactamente el mismo núcleo nativo que la app Android** (`native/dsp` para decodificar el
vídeo y `native/radio` con [wifi-sdr](https://github.com/gilankpam/wifi-sdr) +
[devourer](https://github.com/snokvist/devourer) + libusb). La GUI sustituye a la parte JNI/Kotlin de
Android.

Adaptadores: cualquier RTL8812BU — BrosTrend AC1200 (AC1L/AC3L/AC5L, USB `0bda:b812`), TP-Link Archer T4U v3
(`2357:0115`), T3U (`2357:012d`), T3U Plus (`2357:0138`). Probado con un BrosTrend AC1200 (`0bda:b812`).
Hace falta un **puerto USB 3** para 40 MS/s (con USB 2 funciona más limitado, a 20 MS/s o menos).

## Cómo funciona la integración con kepyur (Android)

```
kepyur-desktop/
├── src/                     GUI Qt: ventana, vídeo, backend real y backend demo
├── third_party/kepyur/      submódulo -> github.com/gilankpam/kepyur  (SIN modificar)
│   ├── native/dsp, native/radio    el núcleo que también compila la app Android
│   └── third_party/wifi-sdr, libusb (submódulos fijados por kepyur)
│       └── external/devourer
├── compat/win32/            shim de 2 funciones POSIX para Windows (en vez de parchear kepyur)
├── scripts/                 build, actualización de dependencias, binutils MIPS, despliegue Windows
└── packaging/linux/         regla udev y .desktop
```

- `src/radio_backend.cpp` hace lo mismo que `native/jni/kepyur_jni.cpp` en Android: crea un `kp::Radio`,
  pide el cambio USB 2 → USB 3 (`wsdr::request_usb3`), abre el adaptador, y lanza `scan()` / `watch()`.
  Los fotogramas (`kp::Frame`, RGBX 720×480/576) salen como `QImage` hacia la ventana en vez de a una
  `Surface`; el estado se lee de `Radio::status_json()` igual que la app.
- La tabla de canales (`kp::fpv_channels()`) y la detección de pilotos son las de kepyur.
- Ninguna dependencia está forkeada ni parcheada: todo upstream se compila tal cual con
  `add_subdirectory(third_party/kepyur/native)`. Lo único específico de Windows es `compat/win32/sys/resource.h`
  (`gettid`/`setpriority` → `SetThreadPriority`), que solo entra en el include path en Windows.

## Actualizar las dependencias cuando haya mejoras en los repos originales

```bash
scripts/update-deps.sh          # kepyur -> último main; wifi-sdr/devourer/libusb -> lo que kepyur fije
scripts/build.sh                # recompilar y probar
git commit -am "deps: kepyur $(git -C third_party/kepyur rev-parse --short HEAD)"
```

`scripts/update-deps.sh --latest` además lleva wifi-sdr y devourer a la última versión de sus ramas (sin
esperar a que kepyur las fije; experimental). El CI de GitHub (`.github/workflows/build.yml`) también se puede
lanzar a mano con la opción "actualizar dependencias" para comprobar que lo último sigue compilando en
Linux y Windows.

## Compilar

Primero clona con los submódulos (no uses `--recursive`: devourer arrastra GBs de drivers de referencia
que no hacen falta):

```bash
git clone https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver.git
cd FPVDesktopWiFiReceiver
scripts/update-deps.sh --init
```

### Linux (Debian/Ubuntu; Fedora/Arch también vía `--deps`)

```bash
scripts/build.sh --deps         # instala Qt6, libusb, binutils-mipsel-linux-gnu, xxd... y compila
build/release/kepyur-desktop
```

Permisos USB (una vez), y desenchufa/enchufa el adaptador:

```bash
sudo cp packaging/linux/60-kepyur.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
```

El driver del kernel `rtw88_8822bu` se desconecta solo al abrir el adaptador.

### Windows 10/11

1. Instala [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`).
2. Desde PowerShell, en la carpeta del repo:

   ```powershell
   .\scripts\build-windows.ps1 -Deps
   ```

   La primera vez compila GNU binutils para `mipsel` (wifi-sdr ensambla el programa que corre dentro del chip
   con ellas y MSYS2 no las empaqueta): 5-10 minutos, se guarda en `tools/`. Resultado: carpeta portable
   `dist\kepyur-desktop\kepyur-desktop.exe` con Qt y libusb incluidos.

   (O desde la consola *MSYS2 UCRT64*: `scripts/build.sh --deps && scripts/deploy-windows.sh`.)

3. **Driver WinUSB** con [Zadig](https://zadig.akeo.ie): *Options → List All Devices* → el adaptador
   ("Realtek 8812BU Wireless LAN 802.11ac USB NIC", USB ID `0BDA B812`) → **WinUSB** → *Replace Driver*.

   **Hay que hacerlo dos veces la primera vez**: el RTL8812BU arranca en USB 2; al pulsar *Conectar*, la app
   lo pasa a USB 3 (igual que la app Android) y Windows lo ve como un dispositivo *nuevo*, otra vez con el
   driver Realtek. La app avisa con un mensaje: repite Zadig sin desenchufar y vuelve a pulsar *Conectar*.
   Después ya queda WinUSB para ese puerto USB (si cambias de puerto, puede tocar repetirlo).

   El adaptador deja de funcionar como tarjeta WiFi normal en ese PC; para volver atrás: Administrador de
   dispositivos → el dispositivo → Desinstalar (marcando eliminar el driver) y reenchufar.

### Notas de Windows

- **WinUSB RAW_IO**: sin él, WinUSB entrega las lecturas bulk de una en una y el chip pierde su deadline
  (`deadline: bank 0 ended ... us after due`) incluso a 20 MS/s. `compat/win32/winusb_raw_io.cpp` lo activa en
  el endpoint de streaming (0x84) envolviendo `libusb_submit_transfer` al enlazar (`-Wl,--wrap`), sin tocar
  wifi-sdr. Necesita libusb ≥ 1.0.30 (la de MSYS2 lo es). `KP_RAW_IO=0` lo desactiva (diagnóstico).
- **20 MS/s por defecto al ver un canal** (seleccionable en la GUI o con `--rate 40`): a 40 MS/s hacen falta
  ~24.400 transferencias USB/s y en Windows aparecen fallos de deadline cada pocos segundos (kepyur entonces
  cae a 20 MS/s solo). La imagen es la misma: el filtro RX deja ±10 MHz en ambos casos. El escaneo sí va a
  40 MS/s (capturas cortas) sin problemas.
- Si el receptor de wifi-sdr se rinde tras varios fallos seguidos, la app reabre el adaptador sola y vuelve al
  canal (como mucho una vez cada 15 s).
- Probado con BrosTrend AC1200 (`0bda:b812`) en USB 3, Windows 11: escaneo completo de 29 pasos a 40 MS/s sin
  fallos; 2 minutos viendo a 20 MS/s con 0 bancos perdidos y 0 reinicios.

## Uso

- **Conectar adaptador** (tarda ~5 s en arrancar el chip; si estaba en USB 2 primero lo pasa a USB 3).
- **Escanear 5.8 GHz**: barre las bandas A/B/E/F/R y L, lista los pilotos y sintoniza el más fuerte.
  Clic en un piloto para verlo, o elige un canal fijo en el desplegable.
- Muestreo 40/20 MS/s y ganancia (AGC o fija). **Grabar IQ 5 s** guarda `Documentos/kepyur/rec-*.cs16`
  (+ `.json`), que se puede reproducir con `kp_replay` de kepyur.
- **F** o doble clic: pantalla completa · **O**: OSD · **Esc**: salir de pantalla completa.
- `kepyur-desktop --demo`: sin hardware (barras de color). `--auto`: conecta y escanea al arrancar. `--watch 5800`: conecta y sintoniza esa frecuencia. `--rate 20|40`: muestreo al ver. `--verbose`: log
  de depuración.
- Log: Windows `%LOCALAPPDATA%\kepyur-desktop\kepyur.log`, Linux `~/.local/share/kepyur-desktop/kepyur.log`.

## Licencia

GPL-2.0 (como kepyur y wifi-sdr, que es GPL-2.0-only).
