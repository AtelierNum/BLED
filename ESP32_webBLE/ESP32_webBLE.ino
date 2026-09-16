#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// Use the same UUIDs as the HTML/JS
#define DEVICE_NAME "ESP32_BLE_Trigger"
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

bool ledOn = false;
const int ledPin = 2;  // Onboard LED

const unsigned int numpixels = 64;
Adafruit_NeoPixel pixels(numpixels, 27, NEO_GRB + NEO_KHZ800);

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
// Explicit prototype: Arduino would otherwise auto-generate one above the
// struct definition and fail to compile
HSV rgbToHsv(uint8_t r, uint8_t g, uint8_t b);

uint8_t currentColor[3] = { 0, 120, 0 };
HSV currentHSV = { 21845, 255, 120 };  // green, matches currentColor
HSV startHSV = currentHSV;
HSV targetHSV = currentHSV;
int32_t hueDelta = 0;                  // signed shortest-arc distance start -> target
unsigned long transitionStart = 0;     // millis() when the fade began
unsigned long transitionDuration = 0;  // ms, 0 = snap to target

typedef enum {
  SOLID,
  CHASER,
  NOISE,
  SIN,
} ANIMATION;

uint8_t animation = SOLID;

// --- NEW VARIABLES FOR CONNECTION STATE ---
BLEServer *pServer = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

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

// Callback class to handle incoming data
class MyCallbacks : public BLECharacteristicCallbacks {
  // Note: In ESP32 Core 3.x, getValue() returns a String object
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue();

    if (value.length() > 0) {
      uint8_t receivedVal = (uint8_t)value[0];

      Serial.print("Received Value: ");
      Serial.println(receivedVal);

      if (receivedVal == 1) {
        ledOn = true;
      } else if (receivedVal == 0) {
        ledOn = false;
      }
    }
  }
};

class fillCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue();

    // Payload: [r, g, b] or [r, g, b, durationHi, durationLo] (ms)
    if (value.length() >= 3) {
      startHSV = currentHSV;  // fade from wherever we are now
      targetHSV = rgbToHsv((uint8_t)value[0], (uint8_t)value[1], (uint8_t)value[2]);

      // Black, white and grey have no hue of their own: borrow it from the
      // other end so fading to/from them doesn't sweep through the rainbow
      if (targetHSV.s == 0 || targetHSV.v == 0) targetHSV.h = startHSV.h;
      if (startHSV.s == 0 || startHSV.v == 0) startHSV.h = targetHSV.h;

      // Shortest way around the hue circle
      hueDelta = (int32_t)targetHSV.h - (int32_t)startHSV.h;
      if (hueDelta > 32768) hueDelta -= 65536;
      if (hueDelta < -32768) hueDelta += 65536;

      transitionDuration = 0;
      if (value.length() >= 5) {
        transitionDuration = ((uint8_t)value[3] << 8) | (uint8_t)value[4];
      }
      transitionStart = millis();
    }
  }
};

class animationCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue();

    if (value.length() > 0) {
      animation = (uint8_t)value[0];
    }
  }
};

void setup() {
  Serial.begin(115200);

  pinMode(ledPin, OUTPUT);
  pixels.begin();

  // Initialize BLE Device and give it a name
  BLEDevice::init(DEVICE_NAME);

  // Create Server and assign the callbacks
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());  // <-- ADDED

  // Create Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create Characteristic
  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

  BLECharacteristic *fillCharacteristic = pService->createCharacteristic(
    "154f969f-5195-4552-9aba-85922a7ba713",
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

  BLECharacteristic *animationCharacteristic = pService->createCharacteristic(
    "4a9163c8-45ae-42a9-a7d7-e26485ca83e7",
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

  // Assign the data callback
  pCharacteristic->setCallbacks(new MyCallbacks());
  fillCharacteristic->setCallbacks(new fillCallbacks());
  animationCharacteristic->setCallbacks(new animationCallbacks());

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
  updateColorTransition();
  // The animations below read r/g/b, which is always the current (faded) color
  uint8_t r = currentColor[0];
  uint8_t g = currentColor[1];
  uint8_t b = currentColor[2];

  Serial.print(ledOn);
  Serial.print("\t");
  Serial.print(animation);
  Serial.print("\t");
  Serial.print(r);
  Serial.print("\t");
  Serial.print(g);
  Serial.print("\t");
  Serial.print(b);
  Serial.println();


  // --- NEW CONNECTION MANAGEMENT LOGIC ---

  // If the device just disconnected
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);                   // Give the bluetooth stack the chance to get things ready
    pServer->startAdvertising();  // Restart advertising
    Serial.println("Restarting advertising... Ready for new connection.");
    oldDeviceConnected = deviceConnected;

    // Optional: Turn off the LED when disconnected
    // ledOn = false;
  }

  // If the device just connected
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  // --- NEOPIXEL LOGIC ---
  pixels.clear();

  if (ledOn) {
    switch (animation) {
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

  delay(16);  // Small delay to prevent watchdog issues
}

// Moves currentHSV along the startHSV -> targetHSV fade and rebuilds currentColor
void updateColorTransition() {
  float t = 1.0f;
  if (transitionDuration > 0) {
    t = (float)(millis() - transitionStart) / (float)transitionDuration;
    if (t > 1.0f) t = 1.0f;
  }

  currentHSV.h = (uint16_t)((int32_t)startHSV.h + (int32_t)(hueDelta * t));  // wraps naturally
  currentHSV.s = (uint8_t)llerp(startHSV.s, targetHSV.s, t);
  currentHSV.v = (uint8_t)llerp(startHSV.v, targetHSV.v, t);

  // ColorHSV packs 0x00RRGGBB; no gamma here, the animations apply their own
  uint32_t c = pixels.ColorHSV(currentHSV.h, currentHSV.s, currentHSV.v);
  currentColor[0] = (c >> 16) & 0xFF;
  currentColor[1] = (c >> 8) & 0xFF;
  currentColor[2] = c & 0xFF;
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