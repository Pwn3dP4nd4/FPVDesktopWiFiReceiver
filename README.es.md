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

**Qué puerto USB 3 usar:** conecta el adaptador **directamente a un puerto USB 3 trasero de la placa base** que
dependa del controlador USB de la CPU (en placas AMD, los que no comparten controlador con el chipset), sin hub,
alargador ni puertos frontales de la caja. En algunos controladores USB 3 el adaptador se atasca cada pocos
segundos (la imagen se corta un instante; ver [docs/windows-usb.md](docs/windows-usb.md#5-cortes-del-flujo-según-el-puerto-usb-3)).
Medido en un PC con placa AMD: en un puerto del chipset, compartido con ratón, teclado y audio USB, ~26 atascos
en 5 minutos; en un puerto del controlador de la CPU, ninguno.

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
│   ├── radio/               fpv::Radio: la radio (escaneo, vista, grabación IQ), derivada de la de kepyur
│   └── kp_ext/              detector de señales digitales (HDZero, experimental)
├── third_party/kepyur/      submódulo -> github.com/gilankpam/kepyur  (SIN modificar)
│   ├── native/dsp           el decodificador de vídeo analógico que también compila la app Android
│   └── third_party/wifi-sdr, libusb (submódulos fijados por kepyur)
│       └── external/devourer
├── test/                    bancos de prueba; radio_sim.cpp: fpv::Radio sin adaptador (grabación)
├── compat/win32/            WinUSB en Windows (RAW_IO en el endpoint de streaming)
├── scripts/                 build, actualización de dependencias, binutils MIPS, despliegue Windows
├── assets/                  logo (logo.svg) e iconos generados (scripts/make-icons.sh)
├── translations/            traducciones de la interfaz (español: FPVDesktopWiFiReceiver_es.ts)
└── packaging/               Linux: regla udev y .desktop · Windows: icono y versión del .exe
```

- De kepyur se usa lo difícil y bien ajustado: `native/dsp` (el decodificador de vídeo analógico: `FrontEnd`,
  `Psd`, `find_humps`, `check_video`, la tabla de canales `kp::fpv_channels()`), sin modificar, y wifi-sdr +
  devourer, que llegan como submódulos suyos. Todo se compila con `add_subdirectory(third_party/kepyur/native)`.
- La radio es propia: `src/radio` (`fpv::Radio`, GPL-2.0-only) deriva de `native/radio/radio.cpp` de kepyur y va
  directamente sobre `wsdr::Receiver`. Hace lo mismo (escaneo por pasos a 40 MS/s, vista con el decodificador en
  otro hilo, caída a 20 MS/s, prueba de flujo, grabación IQ) y además, como código normal: escanear solo los
  canales elegidos (`set_scan_plan`), el filtro peine de croma NTSC (`set_comb`), las señales digitales en el
  escaneo (`set_digital_scan`, detector en `src/kp_ext/digital_scan.cpp`) y los huecos del flujo en el `.json` de
  las grabaciones IQ. `native/radio` de kepyur ya no se compila.
- `src/radio_backend.cpp` hace lo mismo que `native/jni/kepyur_jni.cpp` en Android: crea un `fpv::Radio`, pide el
  cambio USB 2 → USB 3 (`wsdr::request_usb3`), abre el adaptador y lanza `scan()` / `watch()`. Los fotogramas
  (`kp::Frame`, RGBX 720×480/576) salen como `QImage` hacia la ventana en vez de a una `Surface`; el estado se lee
  de `Radio::status_json()`, con las mismas claves que en kepyur.
- Los submódulos no se modifican nunca. Lo poco que sigue cambiando en kepyur se aplica al compilar sobre copias
  en la carpeta de compilación, como sustituciones de texto listadas en `cmake/kepyur_patches.cmake`: `comb` (el
  filtro peine en `native/dsp`, propuesto a kepyur) y `rxdma` (desatascar el RX DMA del 8822B en wifi-sdr). Si
  upstream cambia alguna de esas líneas, CMake se para diciendo cuál.
- Sin adaptador: `cmake --build build/msys2 --target radio_sim` y
  `KP_SIM_FILE=grabacion.cs16 build/msys2/radio_sim --scan [--digital]` (o `--watch MHZ`), que reproduce una
  grabación de 40 MS/s centrada en 5770 MHz como si fuera el adaptador (`sim_receiver.cpp` de kepyur).
  `digital_scan_test --write` genera grabaciones sintéticas.

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
scripts/build.sh --deps         # instala Qt6, libusb, FFmpeg (libavcodec), binutils-mipsel-linux-gnu, xxd... y compila
build/release/FPVDesktopWiFiReceiver
sudo cmake --install build/release   # opcional: menú con icono + regla udev en /etc/udev/rules.d (recarga udev)
```

Permisos USB (una vez), y desenchufa/enchufa el adaptador:

```bash
sudo cp packaging/linux/60-kepyur.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
```

El driver del kernel `rtw88_8822bu` se desconecta solo al abrir el adaptador.

La grabación de vídeo (MP4) usa el FFmpeg de la distribución (`libavformat-dev libavcodec-dev`, lo instala
`--deps`): H.264 con libx264 o libopenh264, o MPEG-4 si la distribución no trae codificador H.264. Sin FFmpeg
compila igual, con el botón *Grabar vídeo* desactivado. En Windows usa Media Foundation, que viene con Windows:
nada que instalar y ninguna DLL más en el zip (las ediciones "N" de Windows necesitan el *Media Feature Pack* de
Microsoft).

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
   *Archivo → Crear acceso directo en el escritorio* deja un icono para abrirlo. (`build\msys2\` es solo la
   compilación intermedia: su `.exe` no lleva las DLL al lado y no abre con doble clic.)

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
- Atasco del RX DMA del 8822B en algunos controladores USB 3 (`no packet within timeout`): al reiniciar el flujo
  se apaga y enciende el RX DMA (parche `rxdma` en `cmake/kepyur_patches.cmake`) y la imagen vuelve en
  10-240 ms. Si aun así el receptor de wifi-sdr se rinde tras varios fallos seguidos, la app reabre el adaptador
  sola y vuelve al canal (~10 s, como mucho una vez cada 15 s).
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
- **Escanear 5.8 GHz**: barre las bandas A/B/E/F/R y L, lista los pilotos y sintoniza el más fuerte. El engranaje
  (⚙) de al lado elige qué canales escanear: el barrido completo son 29 pasos de 20 MHz (~26 s en Windows) y una
  sola banda unos 4 (~3 s). La selección se recuerda.
  Clic en un piloto para verlo, o elige un canal fijo en el desplegable.
  Con **Opciones → Modo experimental (HDZero)** activado, las **señales digitales** también salen en la lista, en cursiva: *HDZero* (27 MHz), *HDZero estrecho* (17 MHz,
  modo 540p60) u otra señal digital con su ancho (DJI, Walksnail...). Su vídeo no se puede ver (ver
  [HDZero](#hdzero-experimental)); sirve para saber qué canales están ocupados y para grabar su señal.
- **Opciones → Modo desarrollador** (desactivado por defecto, se recuerda) muestra lo que un usuario normal no
  necesita: muestreo 40/20 MS/s y ganancia (AGC o fija), **Grabar IQ 5 s** (guarda
  `Documentos/FPVDesktopWiFiReceiver/rec-*.cs16` + `.json`, reproducible con `kp_replay` de kepyur, y un
  `.sigmf-meta` para abrirla en inspectrum, GNU Radio o Universal Radio Hacker), *Archivo →
  Abrir carpeta de grabaciones* y los datos técnicos del receptor bajo los botones (chip, USB, enganche, fps,
  perdidos). Al desactivarlo, muestreo y ganancia vuelven a sus valores por defecto (20 MS/s en Windows, 40 en
  Linux; AGC). Los errores y el aviso de USB 2 se ven en los dos modos.
- **Captura** (tecla **S**): guarda la imagen actual en PNG tal como se ve (forma del menú *Ver* y bordes
  recortados, sin el OSD) en `Imágenes/FPVDesktopWiFiReceiver/fpv-AAAAMMDD-HHMMSS-R4.png`.
- **● Grabar vídeo** (tecla **R**): graba el vídeo decodificado en MP4 (H.264, 59,94 fps NTSC / 50 PAL, unos
  60 MB por minuto) en `Vídeos/FPVDesktopWiFiReceiver/fpv-AAAAMMDD-HHMMSS-R4.mp4`; se pulsa otra vez para parar.
  Mientras graba el botón se pone rojo con el tiempo transcurrido y en el vídeo parpadea **● REC**, también en
  pantalla completa. Se graban todos los fotogramas decodificados aunque la pantalla se salte alguno; si se pierde
  la señal la grabación sigue y el rato sin imagen se salta. El MP4 se escribe en trozos de una fracción de
  segundo: si el programa se cierra de golpe (cuelgue, proceso matado, corte de luz) solo se pierde el último
  instante, no todo el archivo. Si falla la escritura o el disco está a punto de llenarse, para en el acto y
  avisa. Las dos carpetas se cambian en
  **Archivo → Carpeta de capturas / Carpeta de vídeos → Cambiar...** y se recuerdan.
- **F** o doble clic en el vídeo: pantalla completa · **O**: OSD · **S**: captura · **R**: grabar vídeo ·
  **Esc**: salir de pantalla completa.
- Menú **Ver**: forma de la imagen 4:3 (analógico estándar), 16:9 (cámaras en modo panorámico) o estirada a la
  ventana, y *Recortar los bordes* (activo por defecto: oculta un 3 % por lado, los bordes sucios de la imagen
  analógica, como hace un televisor). Ambos se recuerdan.
- Menú **Opciones**: *Conectar el adaptador al abrir* (activado por defecto) y qué hacer al conectar
  (*Volver al último canal visto* o *Escanear las bandas*). *Modo desarrollador*. Menú **Archivo**: captura, grabar vídeo, carpetas de
  capturas, vídeos y log (grabaciones IQ en modo desarrollador), acceso directo en el escritorio (Windows).
- Ajustes: Windows `%APPDATA%\FPVDesktopWiFiReceiver\FPVDesktopWiFiReceiver.ini`, Linux
  `~/.config/FPVDesktopWiFiReceiver/FPVDesktopWiFiReceiver.ini`.
- Log: Windows `%LOCALAPPDATA%\FPVDesktopWiFiReceiver\FPVDesktopWiFiReceiver.log`, Linux
  `~/.local/share/FPVDesktopWiFiReceiver/FPVDesktopWiFiReceiver.log`.

Opciones de línea de comandos (opcionales, para pruebas): `--demo` sin hardware (barras de color) ·
`--auto` conectar y escanear · `--watch 5800` conectar y ver esa frecuencia · `--rate 20|40` muestreo al ver · `--fullscreen` arrancar en pantalla completa ·
`--no-connect` no conectar al abrir · `--lang en|es|system` idioma de la interfaz (se guarda) ·
`--verbose` log de depuración.

## Analizador de canales

**Herramientas → Analizador de canales** recomienda qué canales usar cuando varios pilotos vuelan a la vez:

1. Elige las **bandas** a analizar (A, B, E, F, R, L) y cuántos pilotos **analógicos** y **HDZero** van a volar.
2. **Empezar el análisis**: el adaptador barre esas bandas sin parar (no se ve vídeo mientras tanto). Apaga tus
   propios VTX: todo lo que esté en el aire se toma como ocupado.
3. La gráfica muestra el nivel medio sobre el ruido, la ráfaga más fuerte vista y las zonas con interferencia a
   ráfagas (WiFi de 5 GHz), los pilotos que ya están en el aire (analógicos y HDZero) y cada canal coloreado (verde
   limpio, amarillo con algo de interferencia, rojo ocupado o con ruido). Debajo, los canales **recomendados** con
   su interferencia y notas. Se actualiza con cada barrido: cuanto más tiempo, más ráfagas de WiFi capta.

Cómo elige: cada canal se puntúa por la interferencia dentro de su ancho (analógico 20 MHz, HDZero 27 MHz), con
más peso para las ráfagas; un canal donde ya hay un piloto queda descartado. Entre los elegidos exige una
separación mínima (20 MHz entre analógicos, 24 analógico-HDZero, 27 entre HDZero), penaliza quedar por debajo de
la ideal (37, 40 y 37 MHz), evita la intermodulación de 3.er orden entre analógicos (2·f1 − f2 sobre otro canal) y,
con la banda limpia, los separa todo lo posible (4 pilotos en Raceband: R1, R3, R6, R8). Si no caben con
separación segura, lo dice. El cálculo está en `src/channel_advisor.cpp` (banco: `channel_advisor_test`).

## HDZero (experimental)

HDZero es un sistema de vídeo **digital** (OFDM de unos 27 MHz, con un códec propio de Divimath, sin
documentación pública). Este programa **no decodifica su vídeo**. Todo lo de esta sección va en **Opciones → Modo
experimental (HDZero)**, desactivado por defecto y que se recuerda: apagado, el escaneo es el de kepyur sin cambios.
Lo que hace:

1. **Lo detecta al escanear.** Busca bloques anchos y planos en el espectro (así es una señal OFDM), presentes todo
   el tiempo (HDZero emite sin parar; el WiFi va a ráfagas). Por ancho y frecuencia los marca como *HDZero*
   (22-31 MHz centrado en un canal de HDZero: R1-R8, E1, F1, F2, F4, L1-L8), *HDZero estrecho* (14-22 MHz en un canal
   de HDZero) u otra señal digital. Cada paso del escaneo ve ±16,5 MHz y los pasos van cada 20 MHz, así que una
   señal de 27 MHz casi nunca cabe en uno: si un paso la ve cortada por un borde y el anterior por el otro, se
   unen. Al terminar el escaneo se sintoniza el piloto **analógico** más fuerte, nunca uno digital.
2. **Graba su señal** para analizarla: clic en el piloto HDZero (en lugar de imagen sale un aviso) y, en modo
   desarrollador, **Grabar IQ 5 s** a **40 MS/s** (a 20 MS/s, lo que usa Windows por defecto, solo cabe parte de los
   27 MHz). El `.json` anota los huecos del flujo (`gaps`, `gap_at`: muestra del fichero tras cada uno) y el
   `.sigmf-meta` los marca como discontinuidades, con la señal anotada. A 40 MS/s son 160 MB por segundo.

Límites conocidos: el detector se ha probado con señales sintéticas (`test/digital_scan_test.cpp`: OFDM continuo
de 27 y 17 MHz, WiFi a ráfagas, FM analógica con barras e imagen cargada), **no con un VTX HDZero real**; los
umbrales pueden necesitar ajuste con capturas de verdad. Una señal de 17 MHz de otro sistema centrada en un canal de
HDZero se marcaría como *HDZero estrecho*. Con una selección de canales que no centre un paso en el canal HDZero
(p. ej. la banda F entera) o en L1, en el extremo del barrido, puede no verse.

```bash
cmake --build build/release --target digital_scan_test && build/release/digital_scan_test
```

**Uso responsable:** recibe solo tus propios equipos, o los de otros pilotos con su permiso. Grabar o difundir el
vídeo de otra persona sin permiso puede vulnerar su intimidad o el secreto de las comunicaciones.

## Logo

`assets/logo.svg` (un quad FPV emitiendo). Tras modificarlo, `scripts/make-icons.sh` regenera
`assets/icons/app-*.png` y `app.ico` (necesita el módulo Qt SVG solo para eso; el programa no).

## Traducciones

Los textos de la interfaz están en inglés en el código (`tr()`); la traducción al español es
`translations/FPVDesktopWiFiReceiver_es.ts` y va compilada dentro del ejecutable. Tras cambiar textos, actualiza
los `.ts` con `cmake --build build/release --target update_translations` (`build/msys2` en Windows) y traduce las entradas nuevas con Qt
Linguist (o cualquier editor). Otro idioma: añade `translations/FPVDesktopWiFiReceiver_<código>.ts` a
`KP_TS_FILES` en `CMakeLists.txt` y una entrada en el menú Idioma (`src/main_window.cpp`, `src/i18n.cpp`).
Para compilar hace falta Qt Linguist (`lrelease`): MSYS2 `qt6-tools`, Debian/Ubuntu `qt6-tools-dev
qt6-l10n-tools`; sin él el programa se compila solo en inglés.

## Licencia

GPL-2.0 (como kepyur y wifi-sdr, que es GPL-2.0-only).
