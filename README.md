# PWR-REACTOR MK.II

A retro military CRT/VFD desktop widget for Linux that shows the battery
and power status of every connected device - laptop battery, phones,
wireless mice/keyboards, headsets and the AC line.

Everything is drawn procedurally in a single C file with SDL2: phosphor
dot-matrix VFD font with additive glow, seven-segment ghost digits, CRT
scanlines/vignette/flicker, incandescent dome lamps, heavy-duty toggle
switches, a scrolling charge-trend scope and an analog bus-load meter.

![green phosphor](screenshots/panel-green.png)

![amber phosphor](screenshots/panel-amber.png)

## Features

- **Live telemetry** for every power source the system knows about,
  rescanned every 2 seconds. Primary source is `upower --dump` (proper
  model names, phone/mouse/keyboard batteries over USB and Bluetooth),
  with a raw `/sys/class/power_supply` fallback.
- **Per-device row**: status lamp, model name, type tag, charge state,
  voltage/wattage, 15-segment bar gauge and big 7-segment percent
  readout. Charging devices get a blinking amber lamp and a sweep
  animation; below 15% the row blinks red.
- **Charge trend scope**: last ~6 minutes of charge history for every
  battery as phosphor-persistence traces (older samples fade), plus a
  dim histogram of total bus load along the bottom.
- **Analog bus-load meter**: damped needle, auto-ranging scale and a
  red peak-hold marker.
- **Tray resident**: closing the window hides it to a tray icon - a
  green radiation trefoil (StatusNotifier via `libayatana-appindicator`,
  loaded with `dlopen` so no dev package is required). Menu:
  Show Panel / Quit.
- **Pops up on plug-in**: when a new device battery appears, the panel
  raises itself automatically.
- **systemd user service** included, so it starts with your session.

## Controls

| Control    | Key | Action                              |
|------------|-----|-------------------------------------|
| HOLD       | H   | freeze telemetry scanning           |
| LAMP TST   | L   | light every indicator lamp          |
| PHOSPHOR   | P   | switch green / amber phosphor       |
|            | ESC | hide to tray                        |
|            | Q   | quit                                |

Toggles are clickable. CLI flags: `--hidden` (start in tray),
`--amber` (start with amber phosphor).

## Build

```sh
sudo apt install libsdl2-dev libgtk-3-dev   # gtk only needed for tray
make
./power_reactor
```

Without GTK dev headers the app still builds - it just runs without a
tray icon.

## Run as a service

```sh
make install-service     # enable + start systemd user service
make uninstall-service
```

The service starts the panel hidden in the tray at login; it pops up
whenever a device with a battery is plugged in.

## Supported devices

Anything that reports battery state through a standard Linux interface
shows up automatically:

- **USB HID power devices**: wireless mouse/keyboard receivers,
  gamepads (DualShock/DualSense, Xbox via xpadneo), styluses - the
  kernel exposes them in `/sys/class/power_supply` and upower picks
  them up.
- **Logitech Unifying / HID++** receivers (via upower).
- **Bluetooth devices** advertising the GATT battery service: phones,
  earbuds, headsets, mice, keyboards (via BlueZ + upower).
- **iPhones over USB**: charge level comes from `upower` + `usbmuxd`;
  the kernel's `apple_mfi_fastcharge` driver alone does not expose it.
  Pair/trust the phone once for telemetry to appear.
- **Android over USB**: no standard kernel interface exists, so the
  scanner falls back to `adb shell dumpsys battery`. Install
  `platform-tools` (adb) and enable USB debugging on the phone; it then
  appears with an `ANDROID` tag like any other device. Bluetooth-paired
  Androids work without adb.
- **UPS units** speaking the USB HID power-device class (via upower).

Devices that expose no charge data show dashed digits and a
`NO TELEMETRY` tag.
