---
template: true
title: ESP32 <-BLE-> webpage
thumbnail: thumbnail.jpg
description: Control an LED strip from a webpage over Bluetooth, and design light sequences on a timeline with colors, fades and animations
language: en
tags:
  - esp32
  - BLE
  - webBLE
  - neopixel
  - led
  - timeline
---

# ESP32 WebBLE: light sequences from your browser

**Level** : ![](https://img.shields.io/badge/Level-Intermediate-yellow)

_An ateliernum template_

## What does it do? ✨

You plug an LED strip into a small board called an **ESP32**. Then you open a webpage in Chrome and control the lights **over Bluetooth**, with no cables between your computer and the lights.

The webpage has three zones:

- **Connection:** connect your computer to the board.
- **Manual:** turn the lights on or off, pick a color and switch between animations in real time.
- **Timeline:** design a light sequence, like a mini video editor for light. Place **keyframes** on a bar, give each one a color and an animation, and press **Play**. The lights fade smoothly from one color to the next.

Once you press Play, the board does the animating on its own. The webpage only tells it _"now switch to this"_ at each keyframe.

## What hardware do I need? 💾 🔌

| What | Notes |
| --- | --- |
| An **ESP32** board | The common "ESP32 DevKit" type is perfect |
| A **NeoPixel LED strip** (WS2812B) | The code expects **64 LEDs**. You can change that, see [Make it yours](#make-it-yours-) |
| A **USB cable** | Must be a _data_ cable. Some cheap cables only charge, and those won't work |
| 3 **jumper wires** | To connect the strip to the board |
| A **computer with Chrome** | Or any Chromium browser: Edge, Brave, Arc… |
| _(Optional)_ A **5V power supply** | For long strips or bright white light, see the warning below |

> ⚠️ **iPhones and iPads won't work**, not even with Chrome on them. Apple blocks Bluetooth in the browser. Use a laptop (Windows, Mac or Linux) or an Android phone with Chrome. Safari and Firefox won't work either.

### Wiring

The LED strip has 3 wires. Look for the little arrows or the **DIN** label on the strip, and connect the end where the arrows _start_.

| LED strip | ESP32 |
| --- | --- |
| **5V** (often red) | **5V** or **VIN** |
| **GND** (often white or black) | **GND** |
| **DIN** / data (often green) | **GPIO 27** (sometimes labeled **D27** or just **27**) |

> ⚡ **Power warning:** the strip gets its power from your computer's USB port, and USB can't give much. 64 LEDs at full white can ask for far more than that. The board may then keep restarting, or the colors may look wrong. If that happens, use darker colors, or power the strip from a separate 5V supply. In that case, **connect the supply's GND to the ESP32's GND too**. Ask the atelier team if you're unsure.

## What software do I need? 🌈 📂

You only do this once.

1. **Install the [Arduino IDE](https://www.arduino.cc/en/software)**. This is the app that sends the code to the board.
2. **Add ESP32 support.** In the Arduino IDE, open the **Boards Manager** (the second icon on the left sidebar). Search for **esp32** and install **"esp32 by Espressif Systems"**.
   - If it doesn't show up, follow [this step-by-step guide](https://randomnerdtutorials.com/installing-the-esp32-board-in-arduino-ide-windows-instructions/).
3. **Install the LED library.** Open the **Library Manager** (the third icon on the left sidebar). Search for **Adafruit NeoPixel** and click **Install**.

## How to run it? 🚀

### Step 1: send the code to the board

1. Plug the ESP32 into your computer with the USB cable.
2. In the Arduino IDE, open the file `ESP32_webBLE/ESP32_webBLE.ino`.
3. At the top of the window, click the board dropdown and choose **ESP32 Dev Module** and your board's **port**:
   - Windows: something like `COM3`
   - Mac: something like `/dev/cu.usbserial…`
4. Click the **→ Upload** button and wait for **"Done uploading"**.

> 💡 **Stuck on `Connecting......_____`?** Hold down the **BOOT** button on the board until the upload starts, then let go.

### Step 2: open the webpage

Open `index.html` in **Chrome**. You can drag the file into a Chrome window.

> If **Connect** does nothing or shows an error, the page may need to be opened from a local server instead of as a file:
> - In **VS Code**, install the **Live Server** extension, right-click `index.html` and choose **Open with Live Server**.
> - Or put the page online, for example with GitHub Pages.

### Step 3: connect and play

1. Click **Connect to ESP32**. A Chrome popup lists nearby devices. Pick **ESP32_BLE_Trigger** and click **Pair**.
2. When the status says **Connected!**, all the buttons unlock.
3. **Try the Manual zone:**
   - Click **Trigger: ON** to turn the lights on.
   - Pick a color. **Fade (ms)** sets how long the fade to the new color takes: `1000` = 1 second, `0` = instant.
   - Click an animation name to switch to it.
4. **Build a sequence in the Timeline zone:**
   - Set the **Length** in seconds.
   - **Click on the colored bar** to add a keyframe. Each keyframe gets its own row below, where you can change its time, color and animation, or delete it with ✕.
   - The colored bar shows your sequence. Each block fades from one keyframe's color to the next.
   - Your sequence always has a **start** and an **end** keyframe. You can change their colors, but you can't move or delete them.
   - Press **Play**. The lights turn on and follow your sequence. Tick **Loop** to make it repeat.

### The animations

| Name | What it looks like |
| --- | --- |
| **solid** | All LEDs in one flat color |
| **chaser** | A light with a fading tail runs along the strip |
| **noise** | Soft, organic flickering, a bit like a flame or water |
| **sin** | The whole strip slowly "breathes" in and out |

## Make it yours 🔩 🔨

All of these changes are in `ESP32_webBLE.ino`. Upload the code again after each change.

- **A different number of LEDs:** change `64` in the line `const unsigned int numpixels = 64;`.
- **A different data pin:** change `27` in the line `Adafruit_NeoPixel pixels(numpixels, 27, …)`.
- **Several boards in the same room (important in class!):** by default every board is called `ESP32_BLE_Trigger`, so you won't know which one is yours in the popup. Give yours a unique name like `ESP32_Lea`. Change it in **both** files, or the page won't find your board:
  - `ESP32_webBLE.ino`: `#define DEVICE_NAME "ESP32_BLE_Trigger"`
  - `index.html`: `filters: [{ name: "ESP32_BLE_Trigger" }]`
- **Create your own animation:** in the `.ino` file, look at the `switch (animation)` block inside `loop()` to see how the existing animations are made. A new animation needs changes in both files, so ask the atelier team or an AI assistant for help. The `agent.md` file in this folder explains the project to AI assistants.

## Be careful ⚠️

- **Chrome (or another Chromium browser) only**, and **no iPhone or iPad**.
- **Only one computer at a time** can connect to a board. If you can't find it in the popup, someone else may be connected. Ask them to close their tab.
- **Keep the tab open** while a sequence plays. If you close it or your computer goes to sleep, the lights keep the last animation but the sequence stops.
- **Watch the power** on long strips or bright white light (see the wiring section).

## Troubleshooting 🆘

| Problem | Try this |
| --- | --- |
| The board doesn't show up in the Arduino IDE port list | Try another USB cable (yours may be charge-only) or another USB port. On Windows, you may need the **CP210x** or **CH340** driver. Search for the name printed on the small chip next to the USB port |
| The upload fails with `Connecting......_____` | Hold the **BOOT** button during the upload |
| My board isn't in the Chrome popup | Make sure Bluetooth is on and the upload finished. Unplug the board and plug it back in. Check that nobody else is connected to it |
| Connected, but the lights stay off | Click **Trigger: ON** or press **Play**. Check the 3 wires, and make sure DIN is on the right end of the strip (where the arrows start) |
| The lights flicker, the colors are wrong or the board keeps restarting | Not enough power: use darker colors or an external 5V supply |
| The status says **Disconnected** out of nowhere | Move closer to the board, then click **Connect** again |
