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
├── third_party/kepyur/      submodule -> github.com/gilankpam/kepyur  (UNMODIFIED)
│   ├── native/dsp, native/radio    the core the Android app also builds
│   └── third_party/wifi-sdr, libusb (submodules pinned by kepyur)
│       └── external/devourer
├── compat/win32/            shim for 2 POSIX functions on Windows (instead of patching kepyur)
├── scripts/                 build, dependency updates, MIPS binutils, Windows deployment
├── assets/                  logo (logo.svg) and generated icons (scripts/make-icons.sh)
├── translations/            UI translations (Spanish: FPVDesktopWiFiReceiver_es.ts)
└── packaging/               Linux: udev rule and .desktop · Windows: .exe icon and version info
```

- `src/radio_backend.cpp` does what `native/jni/kepyur_jni.cpp` does on Android: it creates a `kp::Radio`,
  requests the USB 2 → USB 3 switch (`wsdr::request_usb3`), opens the adapter and runs `scan()` / `watch()`.
  Frames (`kp::Frame`, RGBX 720×480/576) go to the window as a `QImage` instead of to a `Surface`; status is
  read from `Radio::status_json()`, as the app does.
- The channel table (`kp::fpv_channels()`) and pilot detection are kepyur's.
- No dependency is forked or patched: all upstream code is built as is with
  `add_subdirectory(third_party/kepyur/native)`. The only Windows-specific piece is `compat/win32/sys/resource.h`
  (`gettid`/`setpriority` → `SetThreadPriority`), which is only on the include path on Windows.

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
scripts/build.sh --deps         # installs Qt6, libusb, binutils-mipsel-linux-gnu, xxd... and builds
build/release/FPVDesktopWiFiReceiver
sudo cmake --install build/release   # optional: menu entry with icon + udev rule in /etc/udev/rules.d (reloads udev)
```

USB permissions (once), then unplug and replug the adapter:

```bash
sudo cp packaging/linux/60-kepyur.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
```

The kernel driver `rtw88_8822bu` is detached automatically when the adapter is opened.

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
   PC. *File → Create desktop shortcut* adds an icon to open it.

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
- If the wifi-sdr receiver gives up after several faults in a row, the app reopens the adapter by itself and
  goes back to the channel (at most once every 15 s).
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
- **Scan 5.8 GHz**: sweeps the A/B/E/F/R and L bands, lists the pilots and tunes the strongest one.
  Click a pilot to watch it, or pick a fixed channel in the drop-down.
- Sample rate 40/20 MS/s and gain (AGC or fixed), remembered between sessions. **Record IQ 5 s** saves
  `Documents/FPVDesktopWiFiReceiver/rec-*.cs16` (+ `.json`), which kepyur's `kp_replay` can play back.
- **F** or double-click on the video: full screen · **O**: OSD · **Esc**: leave full screen.
- **Options** menu: *Connect the adapter on startup* (on by default) and what to do on connect (*Go back to the
  last channel* or *Scan the bands*). **File** menu: recordings and log folders, desktop shortcut (Windows).
- Settings: Windows `%APPDATA%\FPVDesktopWiFiReceiver\FPVDesktopWiFiReceiver.ini`, Linux
  `~/.config/FPVDesktopWiFiReceiver/FPVDesktopWiFiReceiver.ini`.
- Log: Windows `%LOCALAPPDATA%\FPVDesktopWiFiReceiver\FPVDesktopWiFiReceiver.log`, Linux
  `~/.local/share/FPVDesktopWiFiReceiver/FPVDesktopWiFiReceiver.log`.

Command-line options (optional, for testing): `--demo` no hardware (colour bars) · `--auto` connect and scan ·
`--watch 5800` connect and watch that frequency · `--rate 20|40` sample rate when watching · `--no-connect` don't
connect on startup · `--lang en|es|system` interface language (saved) · `--verbose` debug log.

## Logo

`assets/logo.svg` (an FPV quad transmitting). After changing it, `scripts/make-icons.sh` regenerates
`assets/icons/app-*.png` and `app.ico` (it needs the Qt SVG module only for that; the program doesn't).

## Translations

UI texts are written in English in the code (`tr()`); the Spanish translation is
`translations/FPVDesktopWiFiReceiver_es.ts` and is compiled into the executable. After changing texts, update the
`.ts` files with `cmake --build build/release --target update_translations` and translate the new entries with Qt
Linguist (or any editor). Another language: add `translations/FPVDesktopWiFiReceiver_<code>.ts` to
`KP_TS_FILES` in `CMakeLists.txt` and an entry in the Language menu (`src/main_window.cpp`, `src/i18n.cpp`).
Building needs Qt Linguist (`lrelease`): MSYS2 `qt6-tools`, Debian/Ubuntu `qt6-tools-dev qt6-l10n-tools`; without
it the program builds in English only.

## License

GPL-2.0 (like kepyur and wifi-sdr, which is GPL-2.0-only).
