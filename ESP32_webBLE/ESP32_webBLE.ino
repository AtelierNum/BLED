#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// Use the same UUIDs as the HTML/JS
#define DEVICE_NAME "ESP32_BLE_Trigger"
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define FILL_CHARACTERISTIC_UUID "154f969f-5195-4552-9aba-85922a7ba713"
#define ANIMATION_CHARACTERISTIC_UUID "4a9163c8-45ae-42a9-a7d7-e26485ca83e7"
#define STRIPS_CHARACTERISTIC_UUID "d3c6f0a2-5e1b-4f7a-9c38-2b7e4a1d9f60"

const int ledPin = 2;  // Onboard LED, lights up while a client is connected

// --- STRIPS ---
// The page decides how many strips there are, which pin each one is on and
// how many LEDs it has, and sends that list over BLE. Every other command
// names its strip by GPIO pin, so a strip keeps its state when others are
// added or removed.
#define MAX_STRIPS 6  // 6 strips x 3 bytes fits in one default-size BLE write
#define MAX_LEDS_PER_STRIP 1000
#define NUM_GPIO 40

// GPIOs that can drive a strip on a classic ESP32 DevKit
// (keep in sync with STRIP_PINS in index.html). Left out on purpose:
//   0        BOOT button
//   1, 3     USB serial (uploads and the Serial Monitor)
//   2        onboard LED, used to show the connection
//   6..11    wired to the flash memory
//   12       must stay low at boot or the board may not start
//   34..39   input only
// Note: 16 and 17 are taken by PSRAM on WROVER modules (fine on WROOM DevKits).
const uint8_t STRIP_PINS[] = { 4, 5, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 };

typedef enum {
  SOLID,
  CHASER,
  NOISE,
  SIN,
} ANIMATION;

// --- COLOR TRANSITION ---
// The page sends a target color and a duration. currentColor (what the
// pixels actually show) fades from startHSV to targetHSV over that time,
// so every animation doubles as a transition.
// The fade runs in HSV rather than RGB so a hue change stays saturated
// (red -> green sweeps through yellow instead of dimming through olive).
struct HSV {
  uint16_t h;  // 0..65535, wraps (same scale as Adafruit_NeoPixel::ColorHSV)
  uint8_t s;   // 0..255
  uint8_t v;   // 0..255
};

// Everything the page controls about one strip. It's stored per GPIO pin
// rather than per strip, so it survives the strip list being rebuilt.
struct PinState {
  bool on = false;
  uint8_t animation = SOLID;
  uint8_t currentColor[3] = { 0, 120, 0 };
  HSV currentHSV = { 21845, 255, 120 };  // green, matches currentColor
  HSV startHSV = { 21845, 255, 120 };
  HSV targetHSV = { 21845, 255, 120 };
  int32_t hueDelta = 0;                  // signed shortest-arc distance start -> target
  unsigned long transitionStart = 0;     // millis() when the fade began
  unsigned long transitionDuration = 0;  // ms, 0 = snap to target
};

// Explicit prototypes: Arduino would otherwise auto-generate them above the
// struct definitions and fail to compile
HSV rgbToHsv(uint8_t r, uint8_t g, uint8_t b);
void updateColorTransition(PinState &st);
void renderStrip(Adafruit_NeoPixel &pixels, const PinState &st);

PinState pinStates[NUM_GPIO];

Adafruit_NeoPixel *strips[MAX_STRIPS];
uint8_t stripCount = 0;

// The BLE callbacks run on a different task than loop(). This lock stops
// them from changing a strip's state while loop() is reading it.
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

// A new strip list waiting for loop() to apply it
uint8_t pendingConfig[MAX_STRIPS * 3];
size_t pendingConfigLen = 0;
bool configPending = false;

// --- NEW VARIABLES FOR CONNECTION STATE ---
BLEServer *pServer = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

bool isStripPin(int pin) {
  for (uint8_t p : STRIP_PINS) {
    if (p == pin) return true;
  }
  return false;
}

// --- NEW SERVER CALLBACKS ---
// This handles Connect and Disconnect events
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnected = true;
    Serial.println("Client Connected!");
    digitalWrite(ledPin, HIGH);
  };

  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
    Serial.println("Client Disconnected!");
    digitalWrite(ledPin, LOW);
  }
};

// Callback classes to handle incoming data. Every command starts with the
// GPIO pin of the strip it's meant for.
// Note: In ESP32 Core 3.x, getValue() returns a String object
class powerCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue();

    // Payload: [pin, 1 = on | 0 = off]
    if (value.length() < 2 || !isStripPin((uint8_t)value[0])) return;
    uint8_t pin = (uint8_t)value[0];
    uint8_t receivedVal = (uint8_t)value[1];

    portENTER_CRITICAL(&stateMux);
    if (receivedVal == 1) {
      pinStates[pin].on = true;
    } else if (receivedVal == 0) {
      pinStates[pin].on = false;
    }
    portEXIT_CRITICAL(&stateMux);
  }
};

class fillCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue();

    // Payload:
    //   [pin, r, g, b]                                      snap to the color
    //   [pin, r, g, b, durationHi, durationLo]              fade from the current color (ms)
    //   [pin, r, g, b, durationHi, durationLo, r0, g0, b0]  fade from r0/g0/b0
    // The timeline uses the last form: each segment says where its fade starts,
    // so BLE delays can't leave it starting from a slightly-off color.
    if (value.length() < 4 || !isStripPin((uint8_t)value[0])) return;
    uint8_t pin = (uint8_t)value[0];
    HSV target = rgbToHsv((uint8_t)value[1], (uint8_t)value[2], (uint8_t)value[3]);
    unsigned long duration = 0;
    if (value.length() >= 6) {
      duration = ((uint8_t)value[4] << 8) | (uint8_t)value[5];
    }
    bool hasStart = value.length() >= 9;
    HSV start;
    if (hasStart) {
      start = rgbToHsv((uint8_t)value[6], (uint8_t)value[7], (uint8_t)value[8]);
    }

    portENTER_CRITICAL(&stateMux);
    PinState &st = pinStates[pin];
    if (hasStart) {
      st.startHSV = start;
    } else {
      // Fade from wherever we are now. Bring the fade up to date first:
      // loop() only updates currentHSV when it draws, so after a command that
      // just arrived it could still be stale.
      updateColorTransition(st);
      st.startHSV = st.currentHSV;
    }
    st.targetHSV = target;

    // Borrow what a color doesn't really have from the other end:
    // - black has no hue or saturation, so fading to/from black only changes
    //   brightness (instead of washing out through grey/white)
    // - white and grey have no hue, so fading to/from them doesn't sweep
    //   through the rainbow
    if (st.targetHSV.v == 0) {
      st.targetHSV.h = st.startHSV.h;
      st.targetHSV.s = st.startHSV.s;
    } else if (st.targetHSV.s == 0) {
      st.targetHSV.h = st.startHSV.h;
    }
    if (st.startHSV.v == 0) {
      st.startHSV.h = st.targetHSV.h;
      st.startHSV.s = st.targetHSV.s;
    } else if (st.startHSV.s == 0) {
      st.startHSV.h = st.targetHSV.h;
    }

    // Shortest way around the hue circle
    st.hueDelta = (int32_t)st.targetHSV.h - (int32_t)st.startHSV.h;
    if (st.hueDelta > 32768) st.hueDelta -= 65536;
    if (st.hueDelta < -32768) st.hueDelta += 65536;

    st.transitionDuration = duration;
    st.transitionStart = millis();
    portEXIT_CRITICAL(&stateMux);
  }
};

class animationCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue();

    // Payload: [pin, animation index]
    if (value.length() < 2 || !isStripPin((uint8_t)value[0])) return;

    portENTER_CRITICAL(&stateMux);
    pinStates[(uint8_t)value[0]].animation = (uint8_t)value[1];
    portEXIT_CRITICAL(&stateMux);
  }
};

class stripsCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue();

    // Payload: [pin, countHi, countLo] once per strip, in the page's order.
    // Rebuilding the strips can't happen here (loop() might be drawing them),
    // so the list is stored and loop() applies it.
    size_t len = value.length();
    if (len > sizeof(pendingConfig)) len = sizeof(pendingConfig);

    portENTER_CRITICAL(&stateMux);
    for (size_t i = 0; i < len; i++) {
      pendingConfig[i] = (uint8_t)value[i];
    }
    pendingConfigLen = len;
    configPending = true;
    portEXIT_CRITICAL(&stateMux);
  }
};

void setup() {
  Serial.begin(115200);

  pinMode(ledPin, OUTPUT);

  // Until the page sends its own list: one 64-LED strip on GPIO 27
  const uint8_t defaultConfig[] = { 27, 0, 64 };
  memcpy(pendingConfig, defaultConfig, sizeof(defaultConfig));
  pendingConfigLen = sizeof(defaultConfig);
  configPending = true;
  applyPendingConfig();

  // Initialize BLE Device and give it a name
  BLEDevice::init(DEVICE_NAME);

  // Create Server and assign the callbacks
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create Characteristics
  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

  BLECharacteristic *fillCharacteristic = pService->createCharacteristic(
    FILL_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

  BLECharacteristic *animationCharacteristic = pService->createCharacteristic(
    ANIMATION_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

  BLECharacteristic *stripsCharacteristic = pService->createCharacteristic(
    STRIPS_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

  // Assign the data callbacks
  pCharacteristic->setCallbacks(new powerCallbacks());
  fillCharacteristic->setCallbacks(new fillCallbacks());
  animationCharacteristic->setCallbacks(new animationCallbacks());
  stripsCharacteristic->setCallbacks(new stripsCallbacks());

  // Start Service
  pService->start();

  // Start Advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);

  // High performance/compatibility settings
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);

  BLEDevice::startAdvertising();

  Serial.println("BLE Device Ready. Waiting for Chrome connection...");
}

void loop() {
  // --- NEW CONNECTION MANAGEMENT LOGIC ---

  // If the device just disconnected
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);                   // Give the bluetooth stack the chance to get things ready
    pServer->startAdvertising();  // Restart advertising
    Serial.println("Restarting advertising... Ready for new connection.");
    oldDeviceConnected = deviceConnected;
  }

  // If the device just connected
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  applyPendingConfig();

  // --- NEOPIXEL LOGIC ---
  for (uint8_t i = 0; i < stripCount; i++) {
    Adafruit_NeoPixel *px = strips[i];

    // Advance the fade, then draw from a copy so the lock is held only briefly
    portENTER_CRITICAL(&stateMux);
    PinState &live = pinStates[px->getPin()];
    updateColorTransition(live);
    PinState st = live;
    portEXIT_CRITICAL(&stateMux);

    renderStrip(*px, st);
  }

  printStatus();

  delay(16);  // Small delay to prevent watchdog issues
}

// Draws one frame of a strip's animation
void renderStrip(Adafruit_NeoPixel &pixels, const PinState &st) {
  const int numpixels = pixels.numPixels();

  // The animations below read r/g/b, which is always the current (faded) color
  uint8_t r = st.currentColor[0];
  uint8_t g = st.currentColor[1];
  uint8_t b = st.currentColor[2];

  pixels.clear();

  if (st.on) {
    switch (st.animation) {
      case SOLID:
        for (int i = 0; i < numpixels; i++) {
          pixels.setPixelColor(i, pixels.Color(r, g, b));
        }
        break;
      case CHASER:
        for (int i = 0; i < numpixels; i++) {
          pixels.setPixelColor((int)(millis() * 0.01 + i) % numpixels, pixels.Color(r * easeInCubic((float)i / numpixels), g * easeInCubic((float)i / numpixels), b * easeInCubic((float)i / numpixels)));
        }
        break;
      case NOISE:
        for (int i = 0; i < numpixels; i++) {
          float noiseVal = perlin1D_normalized((millis() * 0.0001f * i) + (i * 0.1f));

          pixels.setPixelColor(i, pixels.gamma32(pixels.Color(
                                    (uint8_t)(r * noiseVal),
                                    (uint8_t)(g * noiseVal),
                                    (uint8_t)(b * noiseVal))));
        }
        break;
      case SIN:
        for (int i = 0; i < numpixels; i++) {
          pixels.setPixelColor(i, pixels.gamma32(pixels.Color(r * abs(sin(millis() * 0.001)), g * abs(sin(millis() * 0.001)), b * abs(sin(millis() * 0.001)))));
        }
        break;
      default: pixels.setPixelColor(0, 0x110000);
    }
  }

  pixels.show();
}

// Rebuilds the strips from the latest list sent by the page (if any).
// Strips that keep the same pin and LED count are reused, so they don't flicker.
void applyPendingConfig() {
  uint8_t config[sizeof(pendingConfig)];
  size_t len = 0;

  portENTER_CRITICAL(&stateMux);
  bool pending = configPending;
  if (pending) {
    len = pendingConfigLen;
    memcpy(config, pendingConfig, len);
    configPending = false;
  }
  portEXIT_CRITICAL(&stateMux);

  if (!pending) return;

  // 1. Switch off and free the strips that aren't in the new list. This must
  //    happen before creating new ones: freeing a strip releases its pin.
  for (uint8_t i = 0; i < stripCount; i++) {
    bool keep = false;
    for (size_t o = 0; o + 3 <= len; o += 3) {
      uint16_t count = (config[o + 1] << 8) | config[o + 2];
      if (strips[i]->getPin() == config[o] && strips[i]->numPixels() == count) keep = true;
    }
    if (!keep) {
      strips[i]->clear();
      strips[i]->show();
      delete strips[i];
      strips[i] = NULL;
    }
  }

  // 2. Build the new list in the page's order, reusing the strips kept above
  Adafruit_NeoPixel *next[MAX_STRIPS];
  uint8_t nextCount = 0;

  for (size_t o = 0; o + 3 <= len && nextCount < MAX_STRIPS; o += 3) {
    uint8_t pin = config[o];
    uint16_t count = (config[o + 1] << 8) | config[o + 2];
    if (!isStripPin(pin) || count == 0 || count > MAX_LEDS_PER_STRIP) continue;

    bool pinTaken = false;
    for (uint8_t j = 0; j < nextCount; j++) {
      if (next[j]->getPin() == pin) pinTaken = true;
    }
    if (pinTaken) continue;

    Adafruit_NeoPixel *strip = NULL;
    for (uint8_t i = 0; i < stripCount; i++) {
      if (strips[i] && strips[i]->getPin() == pin && strips[i]->numPixels() == count) {
        strip = strips[i];
        strips[i] = NULL;
      }
    }
    if (!strip) {
      strip = new Adafruit_NeoPixel(count, pin, NEO_GRB + NEO_KHZ800);
      strip->begin();
    }
    next[nextCount++] = strip;
  }

  memcpy(strips, next, sizeof(next[0]) * nextCount);
  stripCount = nextCount;

  Serial.print("Strips:");
  for (uint8_t i = 0; i < stripCount; i++) {
    Serial.printf(" GPIO%d x%d", strips[i]->getPin(), strips[i]->numPixels());
  }
  Serial.println();
}

// Prints every strip's state to the Serial Monitor a few times per second:
// GPIO  on  animation  r  g  b
void printStatus() {
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint < 250) return;
  lastPrint = millis();

  for (uint8_t i = 0; i < stripCount; i++) {
    int pin = strips[i]->getPin();
    portENTER_CRITICAL(&stateMux);
    PinState st = pinStates[pin];
    portEXIT_CRITICAL(&stateMux);

    Serial.printf("GPIO%d\t%d\t%d\t%d\t%d\t%d\t| ", pin, st.on, st.animation,
                  st.currentColor[0], st.currentColor[1], st.currentColor[2]);
  }
  Serial.println();
}

// Moves currentHSV along the startHSV -> targetHSV fade and rebuilds currentColor
void updateColorTransition(PinState &st) {
  float t = 1.0f;
  if (st.transitionDuration > 0) {
    t = (float)(millis() - st.transitionStart) / (float)st.transitionDuration;
    if (t > 1.0f) t = 1.0f;
  }

  st.currentHSV.h = (uint16_t)((int32_t)st.startHSV.h + (int32_t)(st.hueDelta * t));  // wraps naturally
  st.currentHSV.s = (uint8_t)llerp(st.startHSV.s, st.targetHSV.s, t);
  st.currentHSV.v = (uint8_t)llerp(st.startHSV.v, st.targetHSV.v, t);

  // ColorHSV packs 0x00RRGGBB; no gamma here, the animations apply their own
  uint32_t c = Adafruit_NeoPixel::ColorHSV(st.currentHSV.h, st.currentHSV.s, st.currentHSV.v);
  st.currentColor[0] = (c >> 16) & 0xFF;
  st.currentColor[1] = (c >> 8) & 0xFF;
  st.currentColor[2] = c & 0xFF;
}

// RGB (0..255) -> HSV with hue on the 0..65535 scale ColorHSV expects
HSV rgbToHsv(uint8_t r, uint8_t g, uint8_t b) {
  HSV out;
  uint8_t mx = max(r, max(g, b));
  uint8_t mn = min(r, min(g, b));
  uint8_t delta = mx - mn;

  out.v = mx;
  out.s = (mx == 0) ? 0 : (uint8_t)((255UL * delta) / mx);

  if (delta == 0) {
    out.h = 0;  // grey: hue is meaningless, caller decides what to do with it
    return out;
  }

  // Hue in sixths of the circle, each sixth being 65536/6 wide
  float hue;
  if (mx == r) {
    hue = (float)(g - b) / delta;  // -1..1
    if (hue < 0) hue += 6.0f;
  } else if (mx == g) {
    hue = 2.0f + (float)(b - r) / delta;
  } else {
    hue = 4.0f + (float)(r - g) / delta;
  }
  out.h = (uint16_t)(hue * (65536.0f / 6.0f));
  return out;
}

float easeInCubic(float x) {
  return x * x * x;
}

// 1. Fast Pseudo-Random Hash Function
// Takes an integer and returns a gradient (slope) between -1.0f and 1.0f
float hash1D(int x) {
  int n = x * 57;
  n = (n << 13) ^ n;
  int nn = (n * (n * n * 60493 + 19990303) + 1376312589) & 0x7fffffff;
  return 1.0f - ((float)nn / 1073741824.0f);
}

// 2. Ken Perlin's Smootherstep Fade Function
// Smooths the transition between gradients: 6t^5 - 15t^4 + 10t^3
float fade(float t) {
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// 3. Linear Interpolation
float llerp(float a, float b, float t) {
  return a + t * (b - a);
}

// 4. Core 1D Perlin Noise Function
// Returns a value strictly between -0.5 and 0.5
float perlin1D(float x) {
  // Find the integer unit boundaries
  int xi = (int)floorf(x);

  // Find the fractional distance into the cell [0.0, 1.0]
  float xf = x - (float)xi;

  // Smooth the fractional distance
  float u = fade(xf);

  // Compute the dot product of the gradients and the distance vectors
  float grad0 = hash1D(xi) * xf;
  float grad1 = hash1D(xi + 1) * (xf - 1.0f);

  // Interpolate to get the final noise value
  return llerp(grad0, grad1, u);
}

// 5. Utility: Normalized 1D Perlin Noise
// Returns a value mapped cleanly between 0.0 and 1.0 (great for LEDs, servos, etc.)
float perlin1D_normalized(float x) {
  return perlin1D(x) + 0.5f;
}
