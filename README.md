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
[Windows 10/11](#windows-1011), step 3). The same page has an x64 Linux binary (it needs the system's Qt 6 and
libusb), or build it as explained below.

## How the integration with kepyur (Android) works

```
FPVDesktopWiFiReceiver/
├── src/                     Qt GUI: window, video, real backend and demo backend
│   └── radio/               fpv::Radio: the radio (scan, watch, IQ recording), derived from kepyur's
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
  with the decoder on another thread, 20 MS/s fallback, stream test, IQ recording) and also, as plain code,
  scanning only the chosen channels (`set_scan_plan`) and the NTSC chroma comb (`set_comb`). kepyur's
  `native/radio` is no longer built.
- `src/radio_backend.cpp` does what `native/jni/kepyur_jni.cpp` does on Android: it creates an `fpv::Radio`,
  requests the USB 2 → USB 3 switch (`wsdr::request_usb3`), opens the adapter and runs `scan()` / `watch()`.
  Frames (`kp::Frame`, RGBX 720×480/576) go to the window as a `QImage` instead of to a `Surface`; status is read
  from `Radio::status_json()`, with the same keys as kepyur's.
- The submodules are never modified. The little that still changes in kepyur is applied at build time to copies
  in the build folder, as text replacements listed in `cmake/kepyur_patches.cmake`: `comb` (the comb filter in
  `native/dsp`, proposed to kepyur) and `rxdma` (unwedging the 8822B RX DMA in wifi-sdr). If upstream changes one
  of those lines, CMake stops with a message saying which.
- Without an adapter: `cmake --build build/msys2 --target radio_sim` and
  `KP_SIM_FILE=recording.cs16 build/msys2/radio_sim --scan` (or `--watch MHZ`), which plays back a 40 MS/s
  recording centred on 5770 MHz as if it were the adapter (kepyur's `sim_receiver.cpp`).

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
- **Options → Developer mode** (off by default, remembered) shows the tools a normal user doesn't need:
  sample rate 40/20 MS/s and gain (AGC or fixed), **Record IQ 5 s** (saves
  `Documents/FPVDesktopWiFiReceiver/rec-*.cs16` + `.json`, which kepyur's `kp_replay` can play back), *File → Open
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
