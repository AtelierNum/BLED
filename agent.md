# agent.md

Guidance for AI coding agents working in this repository.

## Project overview

**ESP32 WebBLE** is a small template that lets a webpage control an ESP32 over Bluetooth Low Energy using the [Web Bluetooth API](https://caniuse.com/web-bluetooth). In this version, the page drives a 64-pixel NeoPixel strip: it turns the strip on or off, sets the fill color, and picks an animation.

There's no build system, no package manager and no tests. The repo has two independent parts that talk to each other only over BLE:

| Path | Role |
| --- | --- |
| [index.html](index.html) | Client: one static page with inline CSS and JS, and no dependencies |
| [ESP32_webBLE/ESP32_webBLE.ino](ESP32_webBLE/ESP32_webBLE.ino) | Firmware: an Arduino sketch that runs a BLE GATT server and drives the NeoPixels |
| [README.md](README.md) | User docs. The YAML front matter (`template`, `title`, `thumbnail`, `tags`…) is template-catalog metadata, so keep it valid |
| [thumbnail.jpg](thumbnail.jpg) | Thumbnail that the README front matter points to |

## BLE protocol (the contract between both sides)

The page and the firmware each hard-code the same identifiers. **If you change any of them, change both files.**

- **Device name:** `ESP32_BLE_Trigger`. The page's `requestDevice` filter uses it, and so does `BLEDevice::init` in the firmware.
- **Service UUID:** `4fafc201-1fb5-459e-8fcc-c5c9c331914b`

| Characteristic | UUID | Payload | Firmware handler |
| --- | --- | --- | --- |
| Trigger (on/off) | `beb5483e-36e1-4688-b7f5-ea07361b26a8` | 1 byte: `1` = on, `0` = off | `MyCallbacks` → `ledOn` |
| Fill color | `154f969f-5195-4552-9aba-85922a7ba713` | 3 bytes: `[r, g, b]` | `fillCallbacks` → `r`, `g`, `b` |
| Animation | `4a9163c8-45ae-42a9-a7d7-e26485ca83e7` | 1 byte: animation index | `animationCallbacks` → `animation` |

All characteristics are `READ | WRITE`. The page only writes to them and never reads or subscribes.

**The animation indices must match on both sides.** The JS array `animations = ["solid", "chaser", "noise", "sin"]` in `index.html` has to stay in the same order as the C enum `ANIMATION { SOLID, CHASER, NOISE, SIN }` in the sketch. To add an animation:
1. Append it to the enum.
2. Add a `case` in the `switch` in `loop()`.
3. Append its name to the JS `animations` array. The buttons are generated from that array automatically.

## Firmware notes

- Target: an ESP32 on **Arduino-ESP32 core 3.x**. In core 3.x, `getValue()` returns an Arduino `String`. Older cores return `std::string`.
- Libraries: the built-in ESP32 BLE (`BLEDevice.h`, `BLEServer.h`, `BLEUtils.h`) and **Adafruit NeoPixel**.
- Pins: the onboard LED is on GPIO 2 and lights up while a client is connected. The NeoPixel data line is on GPIO 27 (`NEO_GRB + NEO_KHZ800`).
- `loop()` runs at about 60 fps (`delay(16)`). Each frame it:
  - restarts advertising after a client disconnects, and
  - renders the current animation from the global state (`ledOn`, `animation`, `r/g/b`).
- **Keep BLE callbacks tiny.** They should only set global variables, and all the real work belongs in `loop()`. The README recommends the same flag pattern.
- The noise animation uses the hand-written 1D Perlin helpers at the bottom of the sketch (`hash1D`, `fade`, `llerp`, `perlin1D`, `perlin1D_normalized`). Arduino IDE generates function prototypes automatically, which is why these can be defined after `loop()`.
- `loop()` prints debug output to Serial at 115200 baud on every frame.

## Web client notes

- It only works in **Chromium and Google Chrome**, on a secure context (`https://` or `localhost`). Web Bluetooth needs a user gesture, so the connection has to start from the "Connect" button click.
- The page is plain vanilla JS with no framework and no bundler. Keep it that way unless asked otherwise.
- Only one client can connect to the ESP32 at a time.
- The UI state follows the connection: `setControlsEnabled(enabled)` turns on the on/off buttons, the color picker and the animation buttons after connecting, and turns them off again on `gattserverdisconnected`. Any new control that writes to a characteristic should be added there, and its write function should check that the characteristic exists first.

## Running

- **Web:** serve `index.html` with any static server, for example `npx serve` or `python -m http.server`, or host it on GitHub Pages. Then open it in Chrome.
- **Firmware:** open `ESP32_webBLE/ESP32_webBLE.ino` in Arduino IDE with the ESP32 board package and the Adafruit NeoPixel library installed, then upload it to the board.
- There are no automated tests. To check a change, flash the board, connect from the page and watch the Serial Monitor.

## Known quirks

- `animationCallbacks` doesn't validate the index. Out-of-range values fall through to the `default` case, which lights pixel 0 dim red.

## Conventions

- Comment the firmware well; readers of this template are learning. Match the existing style of short `// ---` section headers.
- Keep the whole template simple: one HTML file and one sketch. Don't add build steps or dependencies unless asked to.
- If you change the protocol, the hardware or the setup steps, update the README too.
