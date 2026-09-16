// Color regression test: a timeline that's one third green, one third black,
// one third magenta must show exactly those colors on the strip.
//
// 1) Drives the real page headlessly and records every write, with the time
//    it was issued.
// 2) Replays the writes, each delayed 20..60 ms like real BLE, through a JS
//    port of the firmware's color code (fillCallbacks, updateColorTransition,
//    rgbToHsv, Adafruit_NeoPixel::ColorHSV), frame by frame.
//
// The port must stay in sync with ESP32_webBLE.ino. It guards against two past
// bugs: fades to/from black washing out through grey/white, and fades starting
// from a stale or cut-short color.
const { JSDOM } = require("jsdom");
const fs = require("fs");
const path = require("path");
const html = fs.readFileSync(process.argv[2] || path.join(__dirname, "..", "index.html"), "utf8");

// ---- firmware port (keep in sync with ESP32_webBLE.ino) ----

// Adafruit_NeoPixel::ColorHSV, same integer math
function ColorHSV(hue, sat, val) {
  let r, g, b;
  hue = ((hue & 0xffff) * 1530 + 32768) >>> 16;
  if (hue < 510) { b = 0; if (hue < 255) { r = 255; g = hue; } else { r = 510 - hue; g = 255; } }
  else if (hue < 1020) { r = 0; if (hue < 765) { g = 255; b = hue - 510; } else { g = 1020 - hue; b = 255; } }
  else if (hue < 1530) { g = 0; if (hue < 1275) { r = hue - 1020; b = 255; } else { r = 255; b = 1530 - hue; } }
  else { r = 255; g = 0; b = 0; }
  const v1 = 1 + val, s1 = 1 + sat, s2 = 255 - sat;
  return [r, g, b].map((c) => (((((c * s1) >> 8) + s2) * v1) >> 8) & 0xff);
}

function rgbToHsv(r, g, b) {
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b), delta = mx - mn;
  const out = { v: mx, s: mx === 0 ? 0 : Math.trunc((255 * delta) / mx), h: 0 };
  if (delta === 0) return out;
  let hue;
  if (mx === r) { hue = (g - b) / delta; if (hue < 0) hue += 6; }
  else if (mx === g) hue = 2 + (b - r) / delta;
  else hue = 4 + (r - g) / delta;
  out.h = Math.trunc(hue * (65536 / 6)) & 0xffff;
  return out;
}

function createPinState() {
  const green = { h: 21845, s: 255, v: 120 };
  return { cur: { ...green }, start: { ...green }, target: { ...green }, hueDelta: 0, tStart: 0, dur: 0, rgb: [0, 120, 0] };
}

function updateColorTransition(st, now) {
  let t = 1;
  if (st.dur > 0) t = Math.min(1, (now - st.tStart) / st.dur);
  const lerp = (a, b) => Math.trunc(a + t * (b - a)) & 0xff;
  st.cur = { h: (st.start.h + Math.trunc(st.hueDelta * t)) & 0xffff, s: lerp(st.start.s, st.target.s), v: lerp(st.start.v, st.target.v) };
  st.rgb = ColorHSV(st.cur.h, st.cur.s, st.cur.v);
}

function onFill(st, bytes, now) {
  const [, r, g, b, dh, dl, r0, g0, b0] = bytes;
  if (bytes.length >= 9) {
    st.start = rgbToHsv(r0, g0, b0);
  } else {
    updateColorTransition(st, now);
    st.start = { ...st.cur };
  }
  st.target = rgbToHsv(r, g, b);
  if (st.target.v === 0) { st.target.h = st.start.h; st.target.s = st.start.s; }
  else if (st.target.s === 0) st.target.h = st.start.h;
  if (st.start.v === 0) { st.start.h = st.target.h; st.start.s = st.target.s; }
  else if (st.start.s === 0) st.start.h = st.target.h;
  st.hueDelta = st.target.h - st.start.h;
  if (st.hueDelta > 32768) st.hueDelta -= 65536;
  if (st.hueDelta < -32768) st.hueDelta += 65536;
  st.dur = bytes.length >= 6 ? (dh << 8) | dl : 0;
  st.tStart = now;
}

// ---- scenario ----

// expected: [fromMs, toMs, (rgb) => boolean, description]
async function run(layoutName, extraKeyframes, expected) {
  const dom = new JSDOM(html, {
    runScripts: "dangerously",
    beforeParse(w) {
      const fakeChar = { writeValue: async () => {} };
      w.navigator.bluetooth = { requestDevice: async () => ({ addEventListener() {}, gatt: { connect: async () => ({ getPrimaryService: async () => ({ getCharacteristic: async () => fakeChar }) }) } }) };
    },
  });
  const w = dom.window;
  w.document.getElementById("connectBtn").click();
  await new Promise((r) => setTimeout(r, 20));

  // Record each write synchronously with the fake clock's time.
  // Characteristics are all the same fake object, so tell them apart by payload length.
  let clock = 0;
  const writes = [];
  w.__rec = (bytes) => writes.push({ at: clock, bytes: [...bytes] });
  w.eval("queueWrite = (c, b) => { if (c === fillCharacteristic && b.length >= 4) __rec(b); }");

  const L = 9;
  w.eval(`setTrackLength(0, ${L})`);
  w.eval("updateKeyframe(0, stripById(0).tl.keyframes[0].id, 'color', '#00ff00')");
  w.eval("updateKeyframe(0, stripById(0).tl.keyframes.at(-1).id, 'color', '#ff00ff')");
  for (const [t, c] of extraKeyframes) {
    w.eval(`(() => { const s = stripById(0); s.tl.keyframes.push({ id: nextKeyId++, time: ${t}, anim: 0, color: '${c}' }); renderTrack(s); })()`);
  }

  w.eval("performance.now = () => 0; requestAnimationFrame = () => 1;");
  w.eval("playTrack(0)");
  for (clock = 0; clock <= L * 1000; clock += 16) w.eval(`tickTrack(stripById(0), ${clock})`);

  // Deliver each write 20..60 ms late, keeping the order (deterministic pseudo-random)
  let seed = 7;
  const rand = () => (seed = (seed * 16807) % 2147483647) / 2147483647;
  let arrival = 0;
  for (const x of writes) x.at = arrival = Math.max(arrival, x.at + 20 + rand() * 40);

  const st = createPinState();
  let failures = 0;
  let wi = 0;
  for (let now = 0; now <= L * 1000; now += 16) {
    while (wi < writes.length && writes[wi].at <= now) onFill(st, writes[wi++].bytes, now);
    updateColorTransition(st, now);
    for (const [from, to, ok, desc] of expected) {
      if (now >= from && now <= to && !ok(st.rgb)) {
        if (failures < 5) console.log(`FAIL ${layoutName}: t=${(now / 1000).toFixed(2)}s expected ${desc}, got (${st.rgb})`);
        failures++;
      }
    }
  }
  console.log((failures ? "FAIL " : "ok   ") + layoutName);
  return failures;
}

const is = (rgb) => (c) => c.join() === rgb.join();
const THIRDS = [
  [100, 2850, is([0, 255, 0]), "green"],
  [3150, 5850, is([0, 0, 0]), "black"],
  [6250, 8900, is([255, 0, 255]), "magenta"],
];

(async () => {
  let failures = 0;
  // Green / black / magenta thirds must be exact...
  // ...with hard cuts (two keyframes at the same time on each boundary)
  failures += await run("thirds, hard cuts", [[3, "#00ff00"], [3, "#000000"], [6, "#000000"], [6, "#ff00ff"]], THIRDS);
  // ...and with short 0.1 s transitions on each boundary
  failures += await run("thirds, 0.1s transitions", [[2.9, "#00ff00"], [3, "#000000"], [6, "#000000"], [6.1, "#ff00ff"]], THIRDS);
  // Slow fades through black only change brightness: green -> black stays pure
  // green, black -> magenta stays pure magenta (no grey/white on the way)
  failures += await run("slow fades through black don't wash out", [[4.5, "#000000"]], [
    [100, 4400, ([r, g, b]) => r === 0 && b === 0, "pure green (r = b = 0)"],
    [4700, 8900, ([r, g, b]) => g === 0 && r === b, "pure magenta (g = 0, r = b)"],
  ]);
  console.log(failures ? "\nFAILED" : "\nall passed");
  process.exit(failures ? 1 : 0);
})();
