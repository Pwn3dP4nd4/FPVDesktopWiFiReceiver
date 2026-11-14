<p align="center"><img src="assets/icons/app-128.png" width="96" alt="logo"></p>

<p align="center"><a href="README.md">English</a> · <b>Español</b></p>

# FPVDesktopWiFiReceiver

Receptor de vídeo FPV analógico de 5,8 GHz para **Windows y Linux** usando un adaptador WiFi USB **RTL8812BU**
como SDR. Es la versión de escritorio (Qt 6) de [kepyur](https://github.com/gilankpam/kepyur) (de
gilankpam): usa **exactamente el mismo núcleo nativo que la app Android** (`native/dsp` para decodificar el
vídeo y `native/radio` con [wifi-sdr](https://github.com/gilankpam/wifi-sdr) +
[devourer](https://github.com/snokvist/devourer) + libusb). La GUI sustituye a la parte JNI/Kotlin de
Android.

Adaptadores: cualquier RTL8812BU — BrosTrend AC1200 (AC1L/AC3L/AC5L, USB `0bda:b812`), TP-Link Archer T4U v3
(`2357:0115`), T3U (`2357:012d`), T3U Plus (`2357:0138`). Probado con un BrosTrend AC1200 (`0bda:b812`).
Hace falta un **puerto USB 3** para 40 MS/s (con USB 2 funciona más limitado, a 20 MS/s o menos).

## Descargar

Windows 10/11 x64: descarga el `.zip` de la última versión en
[Releases](https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver/releases/latest), descomprímelo y abre
`FPVDesktopWiFiReceiver.exe` con doble clic. La primera vez, instala el driver WinUSB del adaptador desde la app: *Archivo → Instalar driver del
adaptador* (ver [Windows 10/11](#windows-1011), paso 3). En Linux hay un binario x64 en el mismo sitio (necesita Qt 6 y
libusb del sistema), o compílalo como se explica abajo.

## Cómo funciona la integración con kepyur (Android)

```
FPVDesktopWiFiReceiver/
├── src/                     GUI Qt: ventana, vídeo, backend real y backend demo
├── third_party/kepyur/      submódulo -> github.com/gilankpam/kepyur  (SIN modificar)
│   ├── native/dsp, native/radio    el núcleo que también compila la app Android
│   └── third_party/wifi-sdr, libusb (submódulos fijados por kepyur)
│       └── external/devourer
├── compat/win32/            shim de 2 funciones POSIX para Windows (en vez de parchear kepyur)
├── scripts/                 build, actualización de dependencias, binutils MIPS, despliegue Windows
├── assets/                  logo (logo.svg) e iconos generados (scripts/make-icons.sh)
├── translations/            traducciones de la interfaz (español: FPVDesktopWiFiReceiver_es.ts)
└── packaging/               Linux: regla udev y .desktop · Windows: icono y versión del .exe
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
build/release/FPVDesktopWiFiReceiver
sudo cmake --install build/release   # opcional: menú con icono + regla udev en /etc/udev/rules.d (recarga udev)
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
   `dist\FPVDesktopWiFiReceiver\` con Qt y libusb incluidos. **Se abre con doble clic en
   `FPVDesktopWiFiReceiver.exe`**, sin consola ni MSYS2; se puede copiar a cualquier PC con Windows 10/11 x64.
   *Archivo → Crear acceso directo en el escritorio* deja un icono para abrirlo.

   (O desde la consola *MSYS2 UCRT64*: `scripts/build.sh --deps`, que también deja la carpeta lista.)

   Sin compilar: cada versión publicada (`git tag v0.2.0 && git push --tags`) deja un `.zip` para Windows en
   [Releases](https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver/releases), y cada push uno en los
   *artifacts* de [Actions](https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver/actions).

3. **Driver WinUSB, una sola vez**: en la app, *Archivo → Instalar driver del adaptador (WinUSB)…* (Windows pide
   permiso de administrador), y después desenchufa y vuelve a enchufar el adaptador. Sin la app:
   `powershell -ExecutionPolicy Bypass -File driver\windows-driver.ps1` como administrador (en el repositorio,
   `scripts\windows-driver.ps1`). `-Status` muestra qué driver tiene el adaptador y `-Uninstall` lo quita todo.

   Por qué no Zadig: Windows trae su propio driver Realtek para este adaptador (`netrtwlanu.inf`), y el WinUSB de
   Zadig empata con él en puntuación y pierde el desempate por fecha (2012 frente a 2019). Por eso Windows volvía a
   poner el Realtek **en cada re-enumeración**: cada vez que se enchufaba y en cada cambio de USB 2 a USB 3, que la
   app hace al conectar. Este paquete (`packaging/windows/driver`) coincide además con el ID más específico del
   adaptador (`USB\VID_0BDA&PID_B812&REV_0210`), así que gana en puntuación al Realtek, y lleva una fecha más reciente
   para los otros modelos. Probado: se mantiene al desenchufar y al cambiar entre USB 2 y USB 3 sin pasos extra, y
   la app se reconecta sola tras desenchufar el adaptador.

   Windows solo instala drivers firmados, así que el script crea un certificado local de firma de código, firma el
   paquete, **borra la clave privada** (el certificado ya no puede firmar nada más) y añade el certificado público a
   los almacenes *Entidades de certificación raíz de confianza* y *Editores de confianza* del equipo, igual que hace
   Zadig. `-RemoveZadig` borra además las copias que deja Zadig en el almacén de drivers (una por cada uso).
   Mientras tiene WinUSB, el adaptador no funciona como tarjeta WiFi normal.

   No instales UsbDk: mientras está instalado falla el cambio de USB 2 a USB 3 (ver `docs/windows-usb.md`).

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

Abre `FPVDesktopWiFiReceiver.exe` (o el icono del menú en Linux): **conecta el adaptador solo** y vuelve al
último canal que estabas viendo, o escanea si es la primera vez. No hace falta ninguna línea de comandos.

La interfaz está en **español e inglés**: menú **Idioma** (*Idioma del sistema*, *English*, *Español*). Cambia al
momento, sin reiniciar, y se recuerda. Por defecto sigue el idioma del sistema (español en un Windows/Linux en
español, inglés en otro caso).

- **Conectar adaptador / Desconectar** (tarda ~5 s en arrancar el chip; si estaba en USB 2 primero lo pasa a USB 3).
  Si al abrir no encuentra el adaptador, lo indica en la zona del vídeo: enchúfalo y pulsa *Conectar adaptador*.
- **Escanear 5.8 GHz**: barre las bandas A/B/E/F/R y L, lista los pilotos y sintoniza el más fuerte.
  Clic en un piloto para verlo, o elige un canal fijo en el desplegable.
- Muestreo 40/20 MS/s y ganancia (AGC o fija); se recuerdan entre sesiones. **Grabar IQ 5 s** guarda
  `Documentos/FPVDesktopWiFiReceiver/rec-*.cs16` (+ `.json`), reproducible con `kp_replay` de kepyur.
- **F** o doble clic en el vídeo: pantalla completa · **O**: OSD · **Esc**: salir de pantalla completa.
- Menú **Opciones**: *Conectar el adaptador al abrir* (activado por defecto) y qué hacer al conectar
  (*Volver al último canal visto* o *Escanear las bandas*). Menú **Archivo**: carpetas de grabaciones y log, acceso directo
  en el escritorio (Windows).
- Ajustes: Windows `%APPDATA%\FPVDesktopWiFiReceiver\FPVDesktopWiFiReceiver.ini`, Linux
  `~/.config/FPVDesktopWiFiReceiver/FPVDesktopWiFiReceiver.ini`.
- Log: Windows `%LOCALAPPDATA%\FPVDesktopWiFiReceiver\FPVDesktopWiFiReceiver.log`, Linux
  `~/.local/share/FPVDesktopWiFiReceiver/FPVDesktopWiFiReceiver.log`.

Opciones de línea de comandos (opcionales, para pruebas): `--demo` sin hardware (barras de color) ·
`--auto` conectar y escanear · `--watch 5800` conectar y ver esa frecuencia · `--rate 20|40` muestreo al ver ·
`--no-connect` no conectar al abrir · `--lang en|es|system` idioma de la interfaz (se guarda) ·
`--verbose` log de depuración.

## Logo

`assets/logo.svg` (un quad FPV emitiendo). Tras modificarlo, `scripts/make-icons.sh` regenera
`assets/icons/app-*.png` y `app.ico` (necesita el módulo Qt SVG solo para eso; el programa no).

## Traducciones

Los textos de la interfaz están en inglés en el código (`tr()`); la traducción al español es
`translations/FPVDesktopWiFiReceiver_es.ts` y va compilada dentro del ejecutable. Tras cambiar textos, actualiza
los `.ts` con `cmake --build build/release --target update_translations` y traduce las entradas nuevas con Qt
Linguist (o cualquier editor). Otro idioma: añade `translations/FPVDesktopWiFiReceiver_<código>.ts` a
`KP_TS_FILES` en `CMakeLists.txt` y una entrada en el menú Idioma (`src/main_window.cpp`, `src/i18n.cpp`).
Para compilar hace falta Qt Linguist (`lrelease`): MSYS2 `qt6-tools`, Debian/Ubuntu `qt6-tools-dev
qt6-l10n-tools`; sin él el programa se compila solo en inglés.

## Licencia

GPL-2.0 (como kepyur y wifi-sdr, que es GPL-2.0-only).
