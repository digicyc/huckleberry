# HuckleBerry

Phone-style firmware for the LilyGo T-Deck (Plus): a home screen with a clock and app icons,
MeshCore LoRa messaging that also works as a companion radio for the MeshCore phone app, offline
maps you can download on the device, GPS, weather, alarms, Wi-Fi, SD card and settings. It's the
T-Watch WatchApp (`../T-Watch/WatchApp`) reworked for the T-Deck's 320×240 screen, keyboard and
trackball.

## Installing on your T-Deck

Download the firmware from the [latest release](https://github.com/digicyc/huckleberry/releases/latest):

- **`huckleberry-<version>-install.bin`**, flashed at offset **`0x0`**, for the first install (over
  Meshtastic, MeshCore or the factory firmware). If you're reinstalling over HuckleBerry, it resets
  HuckleBerry's settings (Wi-Fi, brightness and so on) but keeps chats and the MeshCore identity.
- **`huckleberry-<version>-update.bin`**, flashed at offset **`0x10000`**, to update a T-Deck that
  already runs HuckleBerry. Everything is kept.

Your old firmware's settings and messages don't carry over. To go back to it later, flash it again
the way its project describes (for example with its web flasher).

### In the browser (Chrome or Edge on a computer)

1. Connect the T-Deck with a USB-C data cable and switch it on.
2. Open [Espressif's web flasher](https://espressif.github.io/esptool-js/), click **Connect**, and
   choose the T-Deck: **USB JTAG/serial debug unit** (`ttyACM…` on Linux, `cu.usbmodem…` on macOS,
   `COM…` on Windows).
3. Under **Program**, set **Flash Address** to `0x0` (it starts as `0x1000`, which is wrong for the
   T-Deck), choose `huckleberry-<version>-install.bin`, and click **Program**. To update instead,
   use `0x10000` and the update file.
4. When it finishes, click **Disconnect** and switch the T-Deck off and on.

### From the command line

With Python installed, on Linux, macOS or Windows:

```sh
pip install esptool
esptool --chip esp32s3 write-flash 0x0 huckleberry-<version>-install.bin       # first install
esptool --chip esp32s3 write-flash 0x10000 huckleberry-<version>-update.bin    # updating
```

esptool finds the T-Deck by itself; with several boards plugged in, add `--port` and the port
(`/dev/ttyACM0`, `/dev/cu.usbmodem…` or `COM3`). To check the downloads first, run
`sha256sum -c SHA256SUMS` (Linux) or `shasum -a 256 -c SHA256SUMS` (macOS) in their folder.

### If the T-Deck isn't found

Put it in download mode: switch it off, hold the trackball pressed down, switch it on (or plug in
the USB cable), and let go. Then connect or run esptool again. After flashing, switch it off and on
to start HuckleBerry.

### First start

Tap the screen to get past the logo. The radio starts on MeshCore's **USA** preset; anywhere else,
open the **LoRa** app and choose your region's preset before sending. To use the MeshCore phone app,
open **Bluetooth** for the PIN.

## Building from source

Built with `arduino-cli`, LilyGoLib's T-Deck board package, `../T-Watch/LilyGoLib` and
`../T-Watch/LilyGoLib-ThirdParty`. The code is in `src/` (`setup()` and `loop()` are in
`src/main.cpp`); `HuckleBerry.ino` is only the stub file Arduino requires, `lib/MeshCore` is the
copy of MeshCore it builds with, and `tools/` has the scripts that generate the images in `src/`.
Arduino needs the project's folder to be named `HuckleBerry`, like the `.ino`, so clone it with
`git clone git@github.com:digicyc/huckleberry.git HuckleBerry`.

```sh
make deps               # once: board package and Arduino libraries
make build              # compile into ./build
make upload             # flash (finds the T-Deck's USB port, or pass PORT=/dev/ttyACMx)
make monitor            # serial console at 115200
make flash              # build + upload + monitor
make release            # firmware files for a GitHub release, in dist/ (see the Makefile)
make settime            # set the clock from this computer (GPS, Wi-Fi and the phone app also set it)
make screenshot         # save the screen to screenshot.png (OUT=file.png); needs uv
```

**Releases:** pushing a new `HUCKLEBERRY_VERSION` in `src/version.h` to `main` makes GitHub Actions
(`.github/workflows/release.yml`) build the `make release` files and publish them as release
`v<version>`. It can also be run from the Actions tab; versions that already have a release are
skipped. It fetches LilyGoLib at the commits pinned in the workflow, so update those when the
`../T-Watch` copies move.

## Using it

- **Boot screen.** At power-on the HuckleBerry logo and version show while the apps start, and
  stay until you tap the screen. The version is in `src/version.h` (also shown at the bottom of
  Settings). The logo is `tools/logo.png`; `uv run tools/make_logo.py > src/logo.c` regenerates the
  firmware's copy.
- **Keyboard and trackball.** Typing goes into the screen's text field; Enter sends or presses.
  Hold **Shift** for capitals and **Sym** for the symbols printed on the keys; **Shift + $** types
  `&`, which isn't on any key. Backspace goes back when there's no text to delete. The keyboard's
  own ESP32-C3 firmware on this T-Deck predates June 2025, so it only sends characters; with newer C3 firmware (flashed through the 6-pin header
  next to RST with a USB-serial adapter, from LilyGo's `Keyboard_ESP32C3` example), Shift and Sym
  also work tapped just before a key, Backspace repeats when held, and the **mic key** goes back.

  **Keyboard light:** C3 firmware from before 25 December 2024, as on this T-Deck, starts with the
  light off and takes no commands for it; press **Alt+B** to turn it on or off. Newer C3 firmware
  also lets HuckleBerry set it: it follows Settings and turns off with the screen.

  Rolling the trackball moves an outline between the buttons and rows on screen (or scrolls when
  there's nothing that way); clicking it presses the outlined item, and holding it for a moment
  goes home. On the alarm time rollers, up and down turn them and a click moves to the next
  one. Touch works everywhere too.
- **Status bar** (every screen): time, an envelope for unread messages, Bluetooth (blue while the
  phone is connected), Wi-Fi signal, GPS satellites (green with a fix) and battery.
- **Screen timeout** is set in Settings (1 min by default). Any key, trackball move or tap wakes the
  screen where you left it; the waking tap doesn't press anything. The keyboard light turns off with
  the screen (with newer keyboard firmware). New messages and alarms wake it.
- **Messages** (MeshCore; radio settings in the LoRa app): the Public channel, your group channels,
  and a chat for each companion that has advertised. **+** joins a hashtag channel (`#name`; the key
  comes from the name, so anyone using the same name can talk), creates a private group (random
  key; the chat's info screen shows the key to share), or joins one with its 32-hex-digit key. It
  also lists the people and nodes heard nearby (tap one to message it) and has **Advertise**. The
  **This T-Deck** card sets the name others see (TDeck-XXXX by default) and whether your adverts
  include your GPS position. New messages chime (Settings can silence it) and show a banner over
  any app. Direct messages show Sent / Delivered / Failed, and received messages how many
  repeaters they came through ("direct" or "3 hops"). Chats are kept on the FFat partition.
  Emoji in messages and names show in colour: every single emoji in Noto Color Emoji, with skin
  tones drawn in the default yellow, flags as their two letters, and combined emoji (👨‍🌾) as their
  parts (👨🌾). `tools/make_emoji.py` generates them into `src/emoji_glyphs.c`.
- **Bluetooth** lets the MeshCore iPhone app use the T-Deck as its radio. It shows the name the app
  lists (**MeshCore-** plus the node name) and the six-digit PIN: in the app tap Connect, pick the
  T-Deck and enter the PIN. Channels and contacts are shared with the app; channels you add in the
  app appear in Messages within a couple of seconds, and messages sent from the phone show in the
  chats here.
- **LoRa** has the MeshCore app's radio settings: a region preset (the app's list, from
  `api.meshcore.nz`; USA by default), or the frequency, bandwidth, spreading factor, coding rate and
  transmit power; the **path hash size** (1, 2 or 3 bytes per repeater in a message's path; floods
  reach 64, 32 or 21 hops); extra delivery ACKs; and whether every node heard is added as a
  contact. Changes apply right away and are MeshCore's own saved prefs, so the phone app shows the
  same values. It also shows the noise floor, the last packet's signal, packet counts and airtime.
- **Map** shows offline tiles from the SD card with your position as a blue dot and the MeshCore
  nodes whose adverts carry a position: repeaters as cyan diamonds, companions and other nodes as
  purple dots, labeled with their names where the labels don't overlap. The trackball pans; Enter,
  `+` or `i` zooms in, `-` or `o` zooms out, space or `c` re-centers, and `d` (or the download
  button) opens **Download map**: pick a radius (2-25 km) and detail (zoom 13-16) around the map's
  center, and it fetches USGS Topo tiles (public domain, US only) over Wi-Fi into
  `/maps/<z>/<x>/<y>.jpg`, skipping tiles already on the card. **All US** instead fetches a
  low-detail map of the whole country (lower 48, Alaska, Hawaii, Puerto Rico; zoom 3-8, about
  2,750 tiles and 35-40 MB, roughly 25 minutes). Wherever a tile isn't on the card, the map enlarges
  the nearest lower-zoom one (up to 8 levels out), so with All US on the card it's never blank:
  detailed tiles where you downloaded them, a blurrier overview everywhere else.
  `tools/download_map_tiles.py` does the same from a computer for bigger areas (`--us-overview`
  for All US). Today's GPS track is drawn as a trail colored by activity.
- **GPS**: on/off switch, position, altitude, speed, HDOP, and signal bars per satellite. A fix sets
  the clock and feeds your position to MeshCore. **Record track** saves a point every 10 m to
  `/tracks/YYYY-MM-DD.csv` on the card, labeled walking/running/cycling/driving from your speed (the
  T-Deck has no motion sensor). The receiver is found at whatever baud rate it's set to; a u-blox
  sleeps when switched off.
- **Weather** gets an Open-Meteo forecast (no key) for your position over Wi-Fi every 30 minutes;
  the home screen shows the current temperature.
- **Alarms**: up to 8, with repeat days; they ring on the speaker for a minute (Snooze = 5 min).
- **Wi-Fi** joins a network with the keyboard and reconnects to it at boot; connecting also sets
  the clock from internet time.
- **SD Card**: size and free space, a read/write test, a file browser and text viewer, and Format
  (FAT32) for a card that can't be read.
- **Settings**: brightness, screen timeout, message sound, keyboard light, battery.

The T-Deck has no RTC, so after a reboot the clock reads `--:--` until GPS, Wi-Fi, the phone app or
`make settime` sets it.

## Testing over USB serial

The firmware takes one-line commands on the USB serial port (115200): `T<unix time>` sets the clock,
`S` sends a screenshot (what `tools/screenshot.py` reads), `K<text>` types UTF-8 text, emoji
included (`\n` Enter, `\b` Backspace, `\e` the mic key), `B<dx>,<dy>` rolls the trackball, `C` /
`L` click / hold it, and `P<x>,<y>` taps the screen. Don't change DTR or RTS on the port: that resets the ESP32-S3.

## MeshCore

`lib/MeshCore` is a pinned copy of the parts of [MeshCore](https://github.com/meshcore-dev/MeshCore)
this uses, including the companion radio node the phone app talks to; `tools/sync-meshcore.sh`
updates it (see its header). The Makefile's `MESH_FLAGS` set the radio defaults for a new node;
after that, the LoRa app (or the phone app) changes them.
`SX126X_DIO2_AS_RF_SWITCH` is `true`, as in Meshtastic's T-Deck build (MeshCore's own T-Deck build
uses `false`); receiving is confirmed, but if your sent messages aren't heard, try `false`.

## Restoring the original firmware

`backup/tdeck-original-flash.bin` is a full 16 MB image of the flash as it came (the Ripple UI
MeshCore firmware). To put it back:

```sh
~/.arduino15/packages/esp32/tools/esptool_py/5.3.0/esptool --port /dev/ttyACM2 write-flash 0 backup/tdeck-original-flash.bin
```
