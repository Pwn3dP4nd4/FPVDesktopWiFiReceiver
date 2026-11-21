<p align="center"><img src="assets/icons/app-128.png" width="96" alt="logo"></p>

<p align="center"><b>English</b> · <a href="README.es.md">Español</a></p>

# FPVDesktopWiFiReceiver

Analog 5.8 GHz FPV video receiver for **Windows and Linux** that uses an **RTL8812BU** USB WiFi adapter as an
SDR. It is the desktop (Qt 6) version of [kepyur](https://github.com/gilankpam/kepyur) (by gilankpam): it uses
**exactly the same native core as the Android app** (`native/dsp` decodes the video, `native/radio` drives the
adapter through [wifi-sdr](https://github.com/gilankpam/wifi-sdr) + [devourer](https://github.com/snokvist/devourer)
+ libusb). The GUI replaces the Android app's JNI/Kotlin layer.

Adapters: any RTL8812BU — BrosTrend AC1200 (AC1L/AC3L/AC5L, USB `0bda:b812`), TP-Link Archer T4U v3
(`2357:0115`), T3U (`2357:012d`), T3U Plus (`2357:0138`). Tested with a BrosTrend AC1200 (`0bda:b812`).
A **USB 3 port** is needed for 40 MS/s (on USB 2 it works with less, at 20 MS/s or lower).

**Which USB 3 port:** plug the adapter **straight into a rear USB 3 port on the motherboard** that hangs off the
CPU's USB controller (on AMD boards, one that does not share the chipset's controller), with no hub, extension
cable or case front-panel port. On some USB 3 controllers the adapter locks up every few seconds (the picture
drops for a moment; see [docs/windows-usb.md](docs/windows-usb.md#5-cortes-del-flujo-según-el-puerto-usb-3), in
Spanish). Measured on an AMD PC: on a chipset port shared with mouse, keyboard and USB audio, ~26 lock-ups in
5 minutes; on a port of the CPU's controller, none.

## Download

Windows 10/11 x64: download the `.zip` of the latest version from
[Releases](https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver/releases/latest), unzip it and double-click
`FPVDesktopWiFiReceiver.exe`. The first time, install the adapter's WinUSB driver from the app: *File → Install adapter driver* (see
[Windows 10/11](#windows-1011), step 3). The same page has an x64 Linux binary (it needs the system's Qt 6,
libusb and FFmpeg libraries; FFmpeg is for video recording), or build it as explained below.

## How the integration with kepyur (Android) works

```
FPVDesktopWiFiReceiver/
├── src/                     Qt GUI: window, video, real backend and demo backend
│   ├── radio/               fpv::Radio: the radio (scan, watch, IQ recording), derived from kepyur's
│   └── kp_ext/              digital signal detector (HDZero, experimental)
├── third_party/kepyur/      submodule -> github.com/gilankpam/kepyur  (UNMODIFIED)
│   ├── native/dsp           the analog video decoder the Android app also builds
│   └── third_party/wifi-sdr, libusb (submodules pinned by kepyur)
│       └── external/devourer
├── test/                    test benches; radio_sim.cpp: fpv::Radio without an adapter (recording)
├── compat/win32/            WinUSB on Windows (RAW_IO on the streaming endpoint)
├── scripts/                 build, dependency updates, MIPS binutils, Windows deployment
├── assets/                  logo (logo.svg) and generated icons (scripts/make-icons.sh)
├── translations/            UI translations (Spanish: FPVDesktopWiFiReceiver_es.ts)
└── packaging/               Linux: udev rule and .desktop · Windows: .exe icon and version info
```

- From kepyur we use the hard, well-tuned part: `native/dsp` (the analog video decoder: `FrontEnd`, `Psd`,
  `find_humps`, `check_video`, the channel table `kp::fpv_channels()`), unmodified, plus wifi-sdr + devourer,
  which come as its submodules. It is all built with `add_subdirectory(third_party/kepyur/native)`.
- The radio is our own: `src/radio` (`fpv::Radio`, GPL-2.0-only) is derived from kepyur's
  `native/radio/radio.cpp` and sits directly on `wsdr::Receiver`. It does the same (stepped scan at 40 MS/s, watch
  with the decoder on another thread, 20 MS/s fallback, stream test, IQ recording) and also, as plain code: scanning
  only the chosen channels (`set_scan_plan`), the NTSC chroma comb (`set_comb`), digital signals in the scan
  (`set_digital_scan`, detector in `src/kp_ext/digital_scan.cpp`) and stream gaps in the `.json` of IQ recordings.
  kepyur's `native/radio` is no longer built.
- `src/radio_backend.cpp` does what `native/jni/kepyur_jni.cpp` does on Android: it creates an `fpv::Radio`,
  requests the USB 2 → USB 3 switch (`wsdr::request_usb3`), opens the adapter and runs `scan()` / `watch()`.
  Frames (`kp::Frame`, RGBX 720×480/576) go to the window as a `QImage` instead of to a `Surface`; status is read
  from `Radio::status_json()`, with the same keys as kepyur's.
- The submodules are never modified. The little that still changes in kepyur is applied at build time to copies
  in the build folder, as text replacements listed in `cmake/kepyur_patches.cmake`: `comb` (the comb filter in
  `native/dsp`, proposed to kepyur) and `rxdma` (unwedging the 8822B RX DMA in wifi-sdr). If upstream changes one
  of those lines, CMake stops with a message saying which.
- Without an adapter: `cmake --build build/msys2 --target radio_sim` and
  `KP_SIM_FILE=recording.cs16 build/msys2/radio_sim --scan [--digital]` (or `--watch MHZ`), which plays back a
  40 MS/s recording centred on 5770 MHz as if it were the adapter (kepyur's `sim_receiver.cpp`).
  `digital_scan_test --write` makes synthetic recordings.

## Updating the dependencies when the original repositories improve

```bash
scripts/update-deps.sh          # kepyur -> latest main; wifi-sdr/devourer/libusb -> whatever kepyur pins
scripts/build.sh                # rebuild and test
git commit -am "deps: kepyur $(git -C third_party/kepyur rev-parse --short HEAD)"
```

`scripts/update-deps.sh --latest` also moves wifi-sdr and devourer to the latest version of their branches
(without waiting for kepyur to pin them; experimental). The GitHub CI (`.github/workflows/build.yml`) can also be
run by hand with the "update dependencies" option to check that the latest code still builds on Linux and
Windows, and it does so every Monday on its own.

## Building

First clone with the submodules (don't use `--recursive`: devourer pulls in GBs of reference drivers that are
not needed):

```bash
git clone https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver.git
cd FPVDesktopWiFiReceiver
scripts/update-deps.sh --init
```

### Linux (Debian/Ubuntu; Fedora/Arch also through `--deps`)

```bash
scripts/build.sh --deps         # installs Qt6, libusb, FFmpeg (libavcodec), binutils-mipsel-linux-gnu, xxd... and builds
build/release/FPVDesktopWiFiReceiver
sudo cmake --install build/release   # optional: menu entry with icon + udev rule in /etc/udev/rules.d (reloads udev)
```

USB permissions (once), then unplug and replug the adapter:

```bash
sudo cp packaging/linux/60-kepyur.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
```

The kernel driver `rtw88_8822bu` is detached automatically when the adapter is opened.

The menu bar (*File*, *View*, *Tools → Channel analyzer*...) is always drawn inside the window, also on desktops
with a global menu on the top panel (KDE Plasma's *Global Menu*, as on Garuda): there Qt would otherwise move it to
the panel and the window would show no menu. `KP_GLOBAL_MENU=1` keeps the global menu.

Video recording (MP4) uses the distribution's FFmpeg (`libavformat-dev libavcodec-dev`, installed by `--deps`):
H.264 with libx264 or libopenh264, or MPEG-4 if the distribution has no H.264 encoder. Without FFmpeg it still
builds, with the *Record video* button disabled. On Windows it uses Media Foundation, part of Windows: nothing to
install and no extra DLL in the zip (Windows "N" editions need Microsoft's *Media Feature Pack*).

### Windows 10/11

1. Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`).
2. From PowerShell, in the repository folder:

   ```powershell
   .\scripts\build-windows.ps1 -Deps
   ```

   The first time it builds GNU binutils for `mipsel` (wifi-sdr assembles the program that runs inside the chip
   with them, and MSYS2 does not package them): 5-10 minutes, cached in `tools/`. Result: the portable folder
   `dist\FPVDesktopWiFiReceiver\` with Qt and libusb included. **Open it by double-clicking
   `FPVDesktopWiFiReceiver.exe`**, with no console or MSYS2; the folder can be copied to any Windows 10/11 x64
   PC. *File → Create desktop shortcut* adds an icon to open it. (`build\msys2\` only holds the intermediate build:
   its `.exe` has no DLLs next to it and does not open by double-clicking.)

   (Or from the *MSYS2 UCRT64* console: `scripts/build.sh --deps`, which also leaves the folder ready.)

   Without building: every published version (`git tag v0.2.0 && git push --tags`) leaves a Windows `.zip` in
   [Releases](https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver/releases), and every push one in the
   *artifacts* of [Actions](https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver/actions).

3. **WinUSB driver, once**: in the app, *File → Install adapter driver (WinUSB)…* (Windows asks for
   administrator permission), then unplug and replug the adapter. Without the app:
   `powershell -ExecutionPolicy Bypass -File driver\windows-driver.ps1` as administrator (in the repository,
   `scripts\windows-driver.ps1`). `-Status` shows which driver the adapter has, `-Uninstall` removes everything.

   Why not Zadig: Windows ships its own Realtek driver for this adapter (`netrtwlanu.inf`) and Zadig's WinUSB ties
   with it in rank and loses the tie on date (2012 against 2019), so Windows put the Realtek driver back **on every
   re-enumeration**: every replug and every USB 2 → USB 3 switch, which the app does on connect. This package
   (`packaging/windows/driver`) also matches the adapter's more specific ID (`USB\VID_0BDA&PID_B812&REV_0210`), so
   it ranks better than the Realtek one, and it carries a newer date for the other models. Tested: it survives
   replugs and USB 2/3 switches with no further steps, and the app reconnects by itself after unplugging the adapter.

   Windows only installs signed drivers, so the script creates a local code-signing certificate, signs the package,
   **deletes the private key** (the certificate cannot sign anything else) and adds the public certificate to the
   PC's *Trusted Root* and *Trusted Publishers* stores, as Zadig does. `-RemoveZadig` also deletes the copies Zadig
   leaves in the driver store (one per run). The adapter does not work as a normal WiFi card while it has WinUSB.

   Do not install UsbDk: while it is installed the USB 2 → USB 3 switch fails (see `docs/windows-usb.md`).

### Windows notes

- **WinUSB RAW_IO**: without it, WinUSB hands the bulk reads to the USB stack one at a time and the chip misses
  its deadline (`deadline: bank 0 ended ... us after due`) even at 20 MS/s. `compat/win32/winusb_raw_io.cpp`
  turns it on for the streaming endpoint (0x84) by wrapping `libusb_submit_transfer` at link time
  (`-Wl,--wrap`), without touching wifi-sdr. It needs libusb ≥ 1.0.30 (MSYS2's is). `KP_RAW_IO=0` turns it off
  (diagnostics).
- **20 MS/s by default when watching a channel** (selectable in the GUI or with `--rate 40`): 40 MS/s needs
  ~24,400 USB transfers/s, and on Windows deadline faults show up every few seconds (kepyur then falls back to
  20 MS/s on its own). The picture is the same: the RX filter passes ±10 MHz in both cases. Scanning does run
  at 40 MS/s (short captures) without problems.
- 8822B RX DMA lock-up on some USB 3 controllers (`no packet within timeout`): the stream restart turns the RX
  DMA off and on (`rxdma` patch in `cmake/kepyur_patches.cmake`) and the picture is back in 10-240 ms. If the
  wifi-sdr receiver still gives up after several faults in a row, the app reopens the adapter by itself and goes
  back to the channel (~10 s, at most once every 15 s).
- Tested with a BrosTrend AC1200 (`0bda:b812`) on USB 3, Windows 11: full 29-step scan at 40 MS/s with no
  faults; 2 minutes watching at 20 MS/s with 0 banks dropped and 0 restarts.

## Usage

Open `FPVDesktopWiFiReceiver.exe` (or the menu icon on Linux): it **connects the adapter by itself** and goes
back to the last channel you were watching, or scans the first time. No command line needed.

The interface is available in **English and Spanish**: **Language** menu (*System default*, *English*,
*Español*). It changes at once, without restarting, and is remembered. By default it follows the system
language (Spanish on a Spanish Windows/Linux, English otherwise).

- **Connect adapter / Disconnect** (the chip takes ~5 s to start; if it was on USB 2 it is switched to USB 3
  first). If the adapter is not found at startup, the video area says so: plug it in and press *Connect adapter*.
- **Scan 5.8 GHz**: sweeps the A/B/E/F/R and L bands, lists the pilots and tunes the strongest one. The gear
  (⚙) next to it chooses which channels to scan: the full sweep is 29 steps of 20 MHz (~26 s on Windows) and
  one band alone is about 4 (~3 s). The choice is remembered.
  Click a pilot to watch it, or pick a fixed channel in the drop-down.
  Several bands have channels almost on the same frequency (R5 5806 / A4 5805 / B5 5809, R7 = F8...) and the
  measurement is off by 1-3 MHz, so the frequency alone can't tell which one it is: when several are within ±4 MHz
  all of them are shown (*R5/A4/B5*). **Right-click** the pilot to say which one is yours; it is remembered (also in
  the OSD, file names and the analyzer). Picking a channel in the drop-down counts too.
  With **Options → Experimental mode (HDZero)** on, **digital signals** are listed too, in italics: *HDZero* (27 MHz), *HDZero narrow* (17 MHz, 540p60 mode) or
  another digital signal with its width (DJI, Walksnail...). Their video can't be shown (see
  [HDZero](#hdzero-experimental)); they tell you which channels are taken and can be recorded.
- **Options → Developer mode** (off by default, remembered) shows the tools a normal user doesn't need:
  sample rate 40/20 MS/s and gain (AGC or fixed), **Record IQ 5 s** (saves
  `Documents/FPVDesktopWiFiReceiver/rec-*.cs16` + `.json`, which kepyur's `kp_replay` can play back, and a
  `.sigmf-meta` to open it in inspectrum, GNU Radio or Universal Radio Hacker), *File → Open
  recordings folder* and the receiver's technical data under the buttons (chip, USB, lock, fps, dropped). Turning
  it off puts sample rate and gain back to their defaults (20 MS/s on Windows, 40 on Linux; AGC). Errors and the
  USB 2 warning are shown in both modes.
- **Snapshot** (key **S**): saves the current picture as PNG, as it is shown (shape of the *View* menu and
  cropped borders, without the OSD), in `Pictures/FPVDesktopWiFiReceiver/fpv-YYYYMMDD-HHMMSS-R4.png`.
- **● Record video** (key **R**): records the decoded video to MP4 (H.264, 59.94 fps NTSC / 50 PAL, about
  60 MB per minute) in `Videos/FPVDesktopWiFiReceiver/fpv-YYYYMMDD-HHMMSS-R4.mp4`; press again to stop. While
  recording the button turns red with the elapsed time and the video shows a blinking **● REC**, also in full
  screen. Every decoded frame is recorded, even if the screen skips one; if the signal is lost the recording keeps
  going and the time without picture is skipped. The MP4 is written in fragments of a fraction of a second: if
  the program is closed abruptly (crash, killed, power cut) only the last moment is lost, not the whole file. If
  writing fails or the disk is about to fill up, it stops at once and says why. Both folders can be changed in **File → Snapshots
  folder / Videos folder → Change...** and are remembered.
- **F** or double-click on the video: full screen · **O**: OSD · **S**: snapshot · **R**: record video ·
  **Esc**: leave full screen.
- **View** menu: picture shape 4:3 (analog standard), 16:9 (cameras in widescreen mode) or stretched to the
  window, and *Crop the borders* (on by default: hides 3 % per side, the dirty edges of the analog picture, as a TV
  does). Both are remembered.
- **Options** menu: *Connect the adapter on startup* (on by default) and what to do on connect (*Go back to the
  last channel* or *Scan the bands*). *Developer mode*. **File** menu: snapshot, record video, snapshots/videos and log folders
  (IQ recordings in developer mode), desktop shortcut (Windows).
- Settings: Windows `%APPDATA%\FPVDesktopWiFiReceiver\FPVDesktopWiFiReceiver.ini`, Linux
  `~/.config/FPVDesktopWiFiReceiver/FPVDesktopWiFiReceiver.ini`.
- Log: Windows `%LOCALAPPDATA%\FPVDesktopWiFiReceiver\FPVDesktopWiFiReceiver.log`, Linux
  `~/.local/share/FPVDesktopWiFiReceiver/FPVDesktopWiFiReceiver.log`.

Command-line options (optional, for testing): `--demo` no hardware (colour bars) · `--auto` connect and scan ·
`--watch 5800` connect and watch that frequency · `--rate 20|40` sample rate when watching · `--fullscreen` start in full screen · `--no-connect` don't
connect on startup · `--lang en|es|system` interface language (saved) · `--verbose` debug log.

## Channel analyzer

**Tools → Channel analyzer** recommends which channels to use when several pilots fly together:

1. Choose the **bands** to analyse (A, B, E, F, R, L) and how many **analog** and **HDZero** pilots will fly (and
   whether the HDZero ones use narrow mode, about 17 MHz). The ▾ next to each band leaves out single channels
   (*R  Raceband (6/8)*): they are not recommended, and dropping the ones at either end shortens the sweep.
2. **Start analysis**: the adapter sweeps those bands continuously (no video meanwhile). Anything on the air is
   treated as taken, except pilots on a pinned channel (step 4): switch your other VTXs off or tick them in the
   **On the air** list (for that analysis only): their signal and sidebands do not count as interference.
3. The graph shows the average level over the noise floor, the strongest recent burst and the areas with bursty
   interference (5 GHz WiFi), the pilots already on the air (analog and HDZero) and each channel coloured (green
   clean, yellow some interference, red busy or noisy). Below, the **recommended** channels with their interference,
   the share of time with bursts and notes. It follows roughly the last minute of the band: what appears or
   leaves shows quickly.
4. **Pin** the channels already given to pilots (tick box in the table, or right-click a channel mark): they are
   kept between analyses and the advice only picks the rest. During the race, **Race tracking** (*Race* tab)
   follows each pinned channel (SNR, dropouts, intruders, interference) and, per channel, says whether to move its
   pilot and where (*R3 → F3*) when another free channel would clearly be better. It can start by itself when the
   pilots take off (*Start when the pilots are up*) and keeps a *History* of its warnings to review or save as CSV
   afterwards ([details](docs/wiki/Channel-analyzer.md#pinned-channels-and-race-tracking)).

How it measures: with a fixed gain, so every step of the sweep is on the same scale (if a nearby VTX clips a
step, that step is measured with 10 dB less and corrected). Each step is measured over its
own floor, but that floor may rise at most 2 dB over its neighbours' (the chip's changes slowly: ~8 dB from 5950 to
5640 MHz); a 40/80/160 MHz WiFi that fills a whole step raises its floor suddenly and still shows. For
every 156 kHz it keeps the average, the strongest burst (fading 1 dB per sweep) and how much of the time it is 6 dB
over the floor, as a sliding average of the last sweeps (5, 10, 20 or 40: **Remember**) (`src/radio/survey.cpp`, test bench: `survey_test`).

How it chooses: each channel is scored by the average interference within its width (analog 20 MHz, HDZero 27 or
17 MHz) plus the share of time intermittent signals (WiFi) hit it; a channel where a pilot already is is ruled out.
It also recognises **WiFi networks** (a channel of the WiFi grid that lights up in bursts across nearly all of its
~17 MHz) even while they are quiet: an AP with no traffic only sends beacons and barely touches the channel, but
as soon as someone uses that network it takes the channel over. Since a quiet AP is only caught now and then,
every network seen is **remembered for 5 minutes**. Each network adds according to its strongest burst (up to
6 dB) on the channels it covers and, while no traffic has been seen on it (its width is unknown), half that on the
rest of its block; with traffic, whatever it uses lights up and shows. Blocks follow the chosen **WiFi rules**
(Europe: no channels 144 or 169–177). The graph shows them as *WiFi 149* or *WiFi 100–112*. What it is based on:
[docs/wiki/Channel-analyzer.md](docs/wiki/Channel-analyzer.md).
Between the chosen ones it requires a minimum spacing (25 MHz between analog; with HDZero, edges must not touch),
penalises going under the ideal (37, 40 and 37 MHz), avoids 3rd-order intermodulation (2·f1 − f2 of two analog pilots
on another chosen channel, HDZero too) and, with a
clean band, spreads them as far as possible (4 pilots on Raceband: R1, R3, R6, R8). If they don't fit with safe
spacing, it says so. The maths is in `src/channel_advisor.cpp` (test bench: `channel_advisor_test`).

## HDZero (experimental)

HDZero is a **digital** video system (OFDM about 27 MHz wide, with Divimath's own codec, not publicly
documented). This program **does not decode its video**. Everything in this section is behind **Options →
Experimental mode (HDZero)**, off by default and remembered: when off, the scan is kepyur's, unchanged. What it
does:

1. **Finds it when scanning.** It looks for wide, flat blocks in the spectrum (what an OFDM signal looks like) that
   are there all the time (HDZero transmits continuously; WiFi comes in bursts). By width and frequency they are
   marked *HDZero* (22-31 MHz centred on an HDZero channel: R1-R8, E1, F1, F2, F4, L1-L8), *HDZero narrow* (14-22
   MHz on an HDZero channel) or another digital signal. Each scan step sees ±16.5 MHz and steps are 20 MHz apart,
   so a 27 MHz signal hardly ever fits in one: when a step sees it cut by one edge and the previous step by the
   other, the two are joined. After a scan the strongest **analog** pilot is tuned, never a digital one.
2. **Records its signal** for analysis: click the HDZero pilot (a notice replaces the picture) and, in developer
   mode, **Record IQ 5 s** at **40 MS/s** (at 20 MS/s, the Windows default, only part of the 27 MHz fits). The
   `.json` lists the stream gaps (`gaps`, `gap_at`: the file sample after each one) and the `.sigmf-meta` marks them
   as discontinuities, with the signal annotated. At 40 MS/s that is 160 MB per second.

Known limits: the detector has been tested with synthetic signals (`test/digital_scan_test.cpp`: continuous 27
and 17 MHz OFDM, bursty WiFi, analog FM with bars and with a busy picture), **not with a real HDZero VTX**; the
thresholds may need tuning with real captures. A 17 MHz signal from another system centred on an HDZero channel
would be marked *HDZero narrow*. A channel selection that does not centre a step on the HDZero channel (e.g. the
whole F band), or L1 at the end of the sweep, may miss it.

```bash
cmake --build build/release --target digital_scan_test && build/release/digital_scan_test
```

**Responsible use:** receive only your own equipment, or other pilots' with their permission. Recording or sharing
someone else's video without permission may breach their privacy or the confidentiality of communications.

## Logo

`assets/logo.svg` (an FPV quad transmitting). After changing it, `scripts/make-icons.sh` regenerates
`assets/icons/app-*.png` and `app.ico` (it needs the Qt SVG module only for that; the program doesn't).

## Translations

UI texts are written in English in the code (`tr()`); the Spanish translation is
`translations/FPVDesktopWiFiReceiver_es.ts` and is compiled into the executable. After changing texts, update the
`.ts` files with `cmake --build build/release --target update_translations` (`build/msys2` on Windows) and translate the new entries with Qt
Linguist (or any editor). Another language: add `translations/FPVDesktopWiFiReceiver_<code>.ts` to
`KP_TS_FILES` in `CMakeLists.txt` and an entry in the Language menu (`src/main_window.cpp`, `src/i18n.cpp`).
Building needs Qt Linguist (`lrelease`): MSYS2 `qt6-tools`, Debian/Ubuntu `qt6-tools-dev qt6-l10n-tools`; without
it the program builds in English only.

## License

GPL-2.0 (like kepyur and wifi-sdr, which is GPL-2.0-only).
