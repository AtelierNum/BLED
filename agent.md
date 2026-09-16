# agent.md

Guidance for AI coding agents working in this repository.

## Project overview

**ESP32 WebBLE** (the page is titled "BLED") is a small ateliernum template that lets a webpage control an ESP32 over Bluetooth Low Energy using the [Web Bluetooth API](https://caniuse.com/web-bluetooth). The page drives **up to 6 NeoPixel strips**. For each strip it can turn it on or off, set a fill color with a fade, pick an animation, and play a keyframe timeline. The page decides which strips exist: the pin and LED count for each strip are set in the page, not in the firmware.

There's no build system and no package manager for the template itself. Optional headless tests live in `tests/` (see [Running](#running)). The template has two independent parts that talk to each other only over BLE:

| Path | Role |
| --- | --- |
| [index.html](index.html) | Client: one static page with inline CSS and JS, and no dependencies |
| [ESP32_webBLE/ESP32_webBLE.ino](ESP32_webBLE/ESP32_webBLE.ino) | Firmware: an Arduino sketch that runs a BLE GATT server and drives the NeoPixels |
| [README.md](README.md) | User docs, written for design students with no technical background. Keep that tone. The YAML front matter (`template`, `title`, `thumbnail`, `tags`…) is template-catalog metadata, so keep it valid |
| [thumbnail.jpg](thumbnail.jpg) | Thumbnail that the README front matter points to |

## BLE protocol (the contract between both sides)

The page and the firmware each hard-code the same identifiers. **If you change any of them, change both files.**

- **Device name:** `ESP32_BLE_Trigger`. The page's `requestDevice` filter uses it, and so does `BLEDevice::init` in the firmware.
- **Service UUID:** `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- **Strips are addressed by GPIO pin.** Every command starts with the pin of the strip it targets, so adding or removing strips never shifts addresses.

| Characteristic | UUID | Payload | Firmware handler |
| --- | --- | --- | --- |
| Power (on/off) | `beb5483e-36e1-4688-b7f5-ea07361b26a8` | `[pin, 1 = on \| 0 = off]` | `powerCallbacks` → `pinStates[pin].on` |
| Fill color | `154f969f-5195-4552-9aba-85922a7ba713` | `[pin, r, g, b]` (snap), `[pin, r, g, b, durHi, durLo]` (fade from the current color; duration in ms, big-endian uint16), or `[pin, r, g, b, durHi, durLo, r0, g0, b0]` (fade from `r0/g0/b0`) | `fillCallbacks` → `pinStates[pin]` transition fields |
| Animation | `4a9163c8-45ae-42a9-a7d7-e26485ca83e7` | `[pin, animation index]` | `animationCallbacks` → `pinStates[pin].animation` |
| Strips | `d3c6f0a2-5e1b-4f7a-9c38-2b7e4a1d9f60` | `[pin, countHi, countLo]` once per strip, in list order | `stripsCallbacks` → `pendingConfig`, applied in `loop()` |

All characteristics are `READ | WRITE`. The page only writes to them and never reads or subscribes.

**Lists that must match on both sides:**
- **Animations:** the JS array `animations = ["solid", "chaser", "noise", "sin"]` has to stay in the same order as the C enum `ANIMATION { SOLID, CHASER, NOISE, SIN }`. To add an animation:
  1. Append it to the enum.
  2. Add a `case` in the `switch` in `renderStrip()`.
  3. Append its name to the JS `animations` array. The buttons and dropdowns are generated from that array.
- **Allowed pins:** `STRIP_PINS` in both files. The firmware's comment explains why each missing pin is left out (boot/strapping, USB serial, flash, input-only, and GPIO 2 because it's the connection LED).
- **Limits:** `MAX_STRIPS = 6` and `MAX_LEDS_PER_STRIP = 1000`. The strip count is capped at 6 so the whole strips payload (6 × 3 bytes) fits in one default-size BLE write (20 bytes). If you raise it, check the write size.

## Firmware notes

- Target: an ESP32 on **Arduino-ESP32 core 3.x**. In core 3.x, `getValue()` returns an Arduino `String`. Older cores return `std::string`.
- Libraries: the built-in ESP32 BLE (`BLEDevice.h`, `BLEServer.h`, `BLEUtils.h`) and **Adafruit NeoPixel**.
- The onboard LED on GPIO 2 lights up while a client is connected. Strips use `NEO_GRB + NEO_KHZ800`. At boot, before any page connects, there's one 64-LED strip on GPIO 27.
- **State lives per pin, not per strip:** `PinState pinStates[40]` holds `on`, `animation` and the color-transition fields. The strip objects (`Adafruit_NeoPixel *strips[MAX_STRIPS]`) only know their pin and length, so rebuilding the strip list never loses state.
- **Threading:** BLE callbacks run on a different FreeRTOS task than `loop()`.
  - Every read or write of `pinStates` and of the pending config happens inside `portENTER_CRITICAL(&stateMux)`. Keep those sections short, with no `Serial` or `delay` inside.
  - The strips callback never touches the strip objects. It copies the payload into `pendingConfig`, and `applyPendingConfig()` rebuilds the strips in `loop()`.
  - That rebuild reuses strips whose pin and count haven't changed, so they don't flicker. It clears and frees the strips that were removed **before** creating new ones, because the `Adafruit_NeoPixel` destructor releases its pin.
- `loop()` runs at about 60 fps (`delay(16)`). Each frame it:
  - restarts advertising after a client disconnects,
  - applies a pending strip list,
  - for each strip, locks, calls `updateColorTransition(pinStates[pin])`, copies the state, unlocks, then `renderStrip(pixels, state)`,
  - calls `printStatus()`, which logs every strip's `GPIO on animation r g b` to Serial (115200 baud) every 250 ms.
- **Colors are transitions, interpolated in HSV.**
  - A fill write converts the RGB target with `rgbToHsv` into `targetHSV`. The fade's start depends on the payload:
    - **With a start color** (9-byte payload), `startHSV` is that color. The timeline always sends this form, so every segment starts exactly on its keyframe, even when BLE delays cut the previous fade short.
    - **Without one**, the firmware calls `updateColorTransition` to bring the current fade up to date, then captures `startHSV` from `currentHSV`, so the fade starts from where the pixels are. The update step matters: `loop()` only refreshes `currentHSV` when it draws, so right after another command it could be stale.
  - Hue takes the shortest arc (`hueDelta`, signed).
  - Endpoints borrow what they don't really have from the other end:
    - **black** (`v == 0`) borrows hue **and** saturation, so fading to or from black only changes brightness instead of washing out through grey or white;
    - **white or grey** (`s == 0`) borrows hue, so the fade doesn't sweep through the rainbow.
  - The page's `mixColors` mirrors these rules exactly; keep the two in sync.
  - `updateColorTransition` lerps H, S and V and rebuilds `currentColor` via `Adafruit_NeoPixel::ColorHSV`.
  - Animations should only ever read `r/g/b` (copied from `currentColor`), never the HSV state.
- **Explicit prototypes:** `rgbToHsv`, `updateColorTransition` and `renderStrip` use the `HSV` / `PinState` structs, so they have explicit prototypes after the struct definitions. Don't remove them: Arduino's auto-generated prototypes could land above the structs, and the sketch would stop compiling.
- The noise animation uses the hand-written 1D Perlin helpers at the bottom of the sketch (`hash1D`, `fade`, `llerp`, `perlin1D`, `perlin1D_normalized`).

## Web client notes

- It only works in **Chromium and Google Chrome**, on a secure context (`https://` or `localhost`). Web Bluetooth needs a user gesture, so the connection has to start from the "Connect" button click.
- The page is plain vanilla JS with no framework and no bundler. Keep it that way unless asked otherwise.
- Only one client can connect to the ESP32 at a time.
- The UI has three zones:
  - **Setup:** the connect button, the status line, and the strip list (`renderStripList`).
  - **Manual:** the strip selector buttons (`renderStripTabs`), on/off, the animation buttons, and color + fade.
  - **Timeline:** one track per strip (`renderTracks` / `renderTrack`).
- **Strips model:** `strips` is an array of objects created by `createStrip(pin, count)`. Each one holds:
  - `id`, the page-side identity (the ESP32 only sees `pin`),
  - `pin` and `count`,
  - the last state sent to it: `on`, `anim`, `color`,
  - `tl`, its own timeline: `length`, `loop`, `playing`, `start`, `next`, `keyframes`.
  
  `renderAllStrips()` redraws all three zones. Call it whenever a strip is added, removed or moved to another pin, and follow up with `sendStripConfig()`.
  - Each pin dropdown greys out the pins used by other strips.
  - The last strip can't be removed, and "+ Add strip" is disabled at `MAX_STRIPS`.
  - `setStripPin` switches the old pin off, sends the new list, then re-sends the strip's `on/color/anim` to the new pin so its state follows it.
  - `removeStrip` switches the strip's pin off first.
  - The page sends the strip list on connect.
- **Manual zone:** the selector buttons are radio-like and stay in list order. `selectedStripId` is the strip the manual controls act on. Selecting a strip sets the color picker to that strip's last color.
- **All BLE writes go through `queueWrite(char, bytes)`.** Web Bluetooth rejects a write while another is in flight, so writes are chained one after the other. `queueWrite` also no-ops when the characteristic is `undefined`. Commands are sent with `sendPower(strip, on)`, `sendFill(strip, color, ms)`, `sendAnim(strip, i)` and `sendStripConfig()`. The first three also record the state on the strip object. Because the queue is serial and each write waits for a response, many strips firing keyframes at the same instant arrive slightly staggered.
- `setControlsEnabled(enabled)` sets `connected`, stops every track on disconnect, and toggles the manual controls and each track's Play/Stop (`updateTrackButtons`).
- **Timeline (per strip):**
  - **Keyframes:** `strip.tl.keyframes` is an array of `{ id, time, anim, color, fixed? }`, where `color` is `"#rrggbb"`. A keyframe's color is the color **at** that time.
  - **Segments:** the segment between keyframes *i* and *i+1* runs `keyframes[i].anim` while fading from `keyframes[i].color` to `keyframes[i+1].color`.
  - **Drawing:** each track's bar (`#bar-<id>`) draws every segment as a rectangle (`.tlSeg`), with a marker (`.tlMark`) at every keyframe. CSS can't interpolate in HSV, so each segment's `linear-gradient` is made of 13 stops sampled from `mixColors`.
  - **Preview color:** `mixColors(a, b, t)` reproduces the firmware's fade (HSV, shortest hue arc, black borrows hue and saturation, white/grey borrow hue), and `colorAt(strip, time)` uses it. The bar therefore shows exactly the colors the strip fades through.
  - **Pinned keyframes:** two always exist, `fixed: "start"` at 0 s and `fixed: "end"` at `tl.length`. They can be recolored but not moved or removed. `setTrackLength` keeps the end one on the new length. The end keyframe has no animation control, since no segment follows it.
  - **New keyframes** take `colorAt(time)`, so inserting one doesn't change the output.
  - **Playback:** tracks play independently, and a single `requestAnimationFrame` loop (`tick`) advances every playing track (`tickTrack`).
    - `playTrack(id, now)` always plays from the start, so pressing Play on a track that's already playing restarts it. Play stays enabled while playing.
    - "Play all" (`playAllTracks`) calls `playTrack` for every strip with one shared `now`, so all tracks (re)start on the same instant.
    - Each pass starts with `startPass(strip)`, which only powers the strip on. The first segment carries the start color.
    - When segment *i* begins, the page sends `sendFill(strip, keyframes[i+1].color, segmentDuration, keyframes[i].color)` and `sendAnim(strip, keyframes[i].anim)`. That's two writes per segment. The ESP32 renders the animation and the fade itself; nothing is streamed.
    - At the end, the track loops if `tl.loop` is set, otherwise it stops.

## Running

- **Web:** serve `index.html` with any static server, for example `npx serve` or `python -m http.server`, or host it on GitHub Pages. Then open it in Chrome.
- **Firmware:** open `ESP32_webBLE/ESP32_webBLE.ino` in Arduino IDE with the ESP32 board package and the Adafruit NeoPixel library installed, then upload it to the board.
- **Tests** (`tests/`, Node + jsdom, no browser or board needed). Run `npm test` from `tests/`. It has two files:
  - `page.test.js` loads `index.html` with a fake `navigator.bluetooth` that records every byte written to each characteristic. It then drives the UI (strip list, manual tabs, timelines, Play / Play all) and checks the exact payloads.
  - `color.test.js` records the fill writes the page sends while playing a timeline and delivers them 20–60 ms late, like real BLE. It replays them through a **JS port of the firmware's color code** (`fillCallbacks`, `updateColorTransition`, `rgbToHsv`, `ColorHSV`) and checks the pixel colors frame by frame: exact green/black/magenta thirds, and fades through black that don't wash out. **If you change the firmware's color logic, update the port too**, or this test no longer means anything.
  - The repo lives in a Google Drive-synced folder, so avoid `npm install` inside it (`node_modules` would sync). Install jsdom somewhere outside the repo and point Node at it instead, e.g. `$env:NODE_PATH = "<dir>\node_modules"; node page.test.js; node color.test.js`.
  - After changing a test's expectations, check that the test still fails on the bug it guards (for example, temporarily revert the fix).
- On hardware: flash the board, connect from the page and watch the Serial Monitor. The tests don't cover the firmware's BLE, threading or NeoPixel output, only its color math.

## Known quirks

- `animationCallbacks` doesn't validate the index. Out-of-range values fall through to the `default` case, which lights pixel 0 dim red.
- Tracks aren't locked together. "Play all" starts them on the same instant, but tracks with different lengths or loop settings drift apart after their first pass.

## Conventions

- Comment the firmware well; readers of this template are learning. Match the existing style of short `// ---` section headers.
- Keep the whole template simple: one HTML file and one sketch. Don't add build steps or dependencies unless asked to.
- If you change the protocol, the hardware or the setup steps, update the README too.
