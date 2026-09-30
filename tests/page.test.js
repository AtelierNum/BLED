// Page tests: loads ../index.html in jsdom with a fake navigator.bluetooth that
// records every byte written to each characteristic, then drives the UI
// (strips list, manual tabs, timelines, Play / Play all) and checks the writes.
const { JSDOM } = require("jsdom");
const fs = require("fs");
const path = require("path");
const html = fs.readFileSync(process.argv[2] || path.join(__dirname, "..", "index.html"), "utf8");

const writes = [];
const names = {
  "beb5483e-36e1-4688-b7f5-ea07361b26a8": "power",
  "154f969f-5195-4552-9aba-85922a7ba713": "fill",
  "4a9163c8-45ae-42a9-a7d7-e26485ca83e7": "anim",
  "d3c6f0a2-5e1b-4f7a-9c38-2b7e4a1d9f60": "strips",
};

const dom = new JSDOM(html, {
  runScripts: "dangerously",
  pretendToBeVisual: true,
  beforeParse(window) {
    window.navigator.bluetooth = {
      requestDevice: async () => ({
        addEventListener() {},
        gatt: {
          connect: async () => ({
            getPrimaryService: async () => ({
              getCharacteristic: async (uuid) => ({
                writeValue: async (buf) => writes.push([names[uuid], [...buf]]),
              }),
            }),
          }),
        },
      }),
    };
  },
});
const w = dom.window;
const $ = (s) => w.document.querySelector(s);
const $$ = (s) => [...w.document.querySelectorAll(s)];
const flush = () => new Promise((r) => setTimeout(r, 20));
let failures = 0;
function check(label, cond) {
  console.log((cond ? "ok   " : "FAIL ") + label);
  if (!cond) failures++;
}
const take = async () => { await flush(); return writes.splice(0); };

(async () => {
  check("initial: 1 strip row", $$(".stripRow").length === 1);
  check("initial: tab GPIO 27 selected", $$("#stripTabs button").map((b) => b.textContent.trim()).join() === "GPIO 27" && $("#stripTabs button").classList.contains("selected"));
  check("initial: remove disabled on last strip", $(".stripRow button").disabled);
  check("initial: 1 track", $$(".track").length === 1);
  check("initial: play disabled while disconnected", $("#play-0").disabled);

  $("#connectBtn").click();
  let wr = await take();
  check("connect sends strip config [27,0,64]", JSON.stringify(wr) === JSON.stringify([["strips", [27, 0, 64]]]));
  check("connect enables play", !$("#play-0").disabled);

  $("#addStripBtn").click();
  $("#addStripBtn").click();
  wr = await take();
  check("add strips -> config lists 3 strips (27, 4, 5)", JSON.stringify(wr.at(-1)) === JSON.stringify(["strips", [27, 0, 64, 4, 0, 64, 5, 0, 64]]));
  check("3 rows / 3 tabs / 3 tracks", $$(".stripRow").length === 3 && $$("#stripTabs button").length === 3 && $$(".track").length === 3);
  check("tabs in list order", $$("#stripTabs button").map((b) => b.textContent.trim()).join() === "GPIO 27,GPIO 4,GPIO 5");
  const row2Select = $$(".stripRow select")[1];
  check("used pins disabled in other dropdowns", row2Select.querySelector('option[value="27"]').disabled && row2Select.querySelector('option[value="5"]').disabled && !row2Select.querySelector('option[value="4"]').disabled);

  // select second tab and use manual controls
  $$("#stripTabs button")[1].click();
  check("tab 2 selected (radio)", $$("#stripTabs button").map((b) => b.classList.contains("selected")).join() === "false,true,false");
  $("#onBtn").click();
  $$("#anims button")[2].click();
  $("#fill").value = "#ff0000";
  $("#fill").dispatchEvent(new w.Event("change"));
  wr = await take();
  check("manual commands go to GPIO 4", JSON.stringify(wr) === JSON.stringify([["power", [4, 1]], ["anim", [4, 2]], ["fill", [4, 255, 0, 0, 1, 244]]]));

  // switching tab updates the color picker to that strip's color
  $$("#stripTabs button")[0].click();
  check("picker shows strip 1 color", $("#fill").value === "#007800");
  $$("#stripTabs button")[1].click();
  check("picker shows strip 2 color", $("#fill").value === "#ff0000");

  // change LED count
  const count = $$(".stripRow input")[2];
  count.value = "5000";
  count.dispatchEvent(new w.Event("change"));
  wr = await take();
  check("count clamped to 1000 and sent", JSON.stringify(wr.at(-1)) === JSON.stringify(["strips", [27, 0, 64, 4, 0, 64, 5, 3, 232]]) && $$(".stripRow input")[2].value === "1000");

  // change pin of strip 2 (GPIO 4 -> 13): old pin off, config, state carried over
  const sel = $$(".stripRow select")[1];
  sel.value = "13";
  sel.dispatchEvent(new w.Event("change"));
  wr = await take();
  check("re-pin: off old, config, carry state", JSON.stringify(wr) === JSON.stringify([
    ["power", [4, 0]],
    ["strips", [27, 0, 64, 13, 0, 64, 5, 3, 232]],
    ["power", [13, 1]],
    ["fill", [13, 255, 0, 0, 0, 0]],
    ["anim", [13, 2]],
  ]));
  check("tab + track label follow new pin, order kept", $$("#stripTabs button").map((b) => b.textContent.trim()).join() === "GPIO 27,GPIO 13,GPIO 5" && $$(".track h3").map((h) => h.textContent).join() === "GPIO 27,GPIO 13,GPIO 5");
  check("selection kept after re-render", $$("#stripTabs button")[1].classList.contains("selected"));

  // timeline on strip id 2 (GPIO 5): add keyframe, play
  const s2 = w.eval("stripById(2)");
  w.eval("addKeyframe(stripById(2), 5)");
  w.eval("updateKeyframe(2, stripById(2).tl.keyframes[1].id, 'color', '#0000ff')");
  w.eval("updateKeyframe(2, stripById(2).tl.keyframes[1].id, 'anim', '1')");
  check("track 3 has 3 keyframe rows, others 2", $$("#keys-2 .tlKey").length === 3 && $$("#keys-0 .tlKey").length === 2);
  $("#play-2").click();
  wr = await take();
  check("play: power on, then segment 1 (green -> blue over 5s, start color included, solid) on GPIO 5", JSON.stringify(wr) === JSON.stringify([["power", [5, 1]], ["fill", [5, 0, 0, 255, 19, 136, 0, 120, 0]], ["anim", [5, 0]]]));
  const t0 = s2.tl.start;
  w.eval(`tick(${t0 + 10})`);
  wr = await take();
  check("segment 1 does not fire twice", wr.length === 0);
  w.eval(`tick(${t0 + 5100})`);
  wr = await take();
  check("segment 2 fires: blue -> green (start color included), anim chaser", JSON.stringify(wr) === JSON.stringify([["fill", [5, 0, 120, 0, 19, 136, 0, 0, 255]], ["anim", [5, 1]]]));
  check("only track 3 playing", !w.eval("stripById(0).tl.playing") && w.eval("stripById(2).tl.playing"));
  w.eval(`tick(${t0 + 10100})`);
  check("stops at end without loop", !w.eval("stripById(2).tl.playing") && !$("#play-2").disabled && $("#stop-2").disabled);

  // restart while playing
  $("#play-2").click();
  await take();
  const firstStart = s2.tl.start;
  w.eval(`tick(${firstStart + 5100})`); // into segment 2
  await take();
  check("mid-play: Play stays enabled", !$("#play-2").disabled && s2.tl.next === 2);
  await new Promise((r) => setTimeout(r, 5));
  $("#play-2").click();
  wr = await take();
  check("Play while playing restarts: power on first", JSON.stringify(wr[0]) === JSON.stringify(["power", [5, 1]]));
  check("restart resets clock and keeps playing", s2.tl.start > firstStart && s2.tl.playing && s2.tl.next <= 1);

  // Play all while one is playing: all (re)start on the same instant
  $("#startAllBtn").click();
  await flush();
  const starts = w.eval("strips.map((s) => s.tl.start)");
  check("Play all: every track playing", w.eval("strips.every((s) => s.tl.playing)"));
  check("Play all: same start time for every track", new Set(starts).size === 1);
  check("Play all: button still enabled", !$("#startAllBtn").disabled);
  $("#startAllBtn").click();
  await flush();
  check("Play all again restarts all", w.eval("strips.map((s) => s.tl.start)")[0] > starts[0] && new Set(w.eval("strips.map((s) => s.tl.start)")).size === 1);
  w.eval("strips.forEach((s) => stopTrack(s.id))");
  await take();

  // remove selected strip
  w.eval("tlRaf = null");
  $$(".stripRow button")[1].click();
  wr = await take();
  check("remove: off + config without it", JSON.stringify(wr) === JSON.stringify([["power", [13, 0]], ["strips", [27, 0, 64, 5, 3, 232]]]));
  check("remove: selection falls back to first", $$("#stripTabs button")[0].classList.contains("selected") && $$(".track").length === 2);

  // max strips
  for (let i = 0; i < 10; i++) $("#addStripBtn").click();
  check("capped at 6 strips, add disabled", $$(".stripRow").length === 6 && $("#addStripBtn").disabled);
  await take();

  // --- config: serialize / load ---
  const saved = JSON.parse(w.eval("serializeConfig()"));
  check("serialize: version, fade and every strip", saved.bled === 1 && saved.fade === 500 && saved.strips.length === 6);
  check("serialize: a strip carries pin, count, color, anim and its timeline", JSON.stringify(saved.strips[0]) === JSON.stringify({
    pin: 27, count: 64, color: "#007800", anim: 0,
    timeline: {
      length: 10, loop: false,
      keyframes: [
        { time: 0, anim: 0, color: "#007800", fixed: "start" },
        { time: 10, anim: 0, color: "#007800", fixed: "end" },
      ],
    },
  }));

  $("#cfgLoadBtn").click();
  check("load: modal opens empty", $("#cfgModal").classList.contains("open") && $("#cfgText").value === "");
  $("#cfgText").value = JSON.stringify({
    bled: 1,
    fade: 250,
    strips: [
      {
        pin: 14, count: 30, color: "#ff0000", anim: 2,
        timeline: {
          length: 4, loop: true,
          keyframes: [
            { time: 0, anim: 1, color: "#000000", fixed: "start" },
            { time: 2, anim: 3, color: "#00ff00" },
            { time: 9, anim: 0, color: "#123456" }, // past the end: dropped
            { time: 4, anim: 0, color: "#0000ff", fixed: "end" },
          ],
        },
      },
      { pin: 99, count: 5000 }, // unknown pin -> first free one, count clamped
      { pin: 14, count: 10 }, // duplicate pin -> dropped
    ],
  });
  $("#cfgApplyBtn").click();
  wr = await take();
  check("load: modal closes", !$("#cfgModal").classList.contains("open"));
  check("load: bad pin fixed up, duplicate dropped", w.eval("strips.map((s) => s.pin).join()") === "14,4");
  check("load: all three zones redrawn, first strip selected", $$(".stripRow").length === 2 && $$(".track").length === 2 && $$("#stripTabs button")[0].classList.contains("selected"));
  check("load: fade applied", $("#fade").value === "250");
  check("load: timeline length, loop and keyframes", w.eval("JSON.stringify(stripById(strips[0].id).tl.keyframes.map((k) => [k.time, k.anim, k.color, k.fixed || null]))") === JSON.stringify([[0, 1, "#000000", "start"], [2, 3, "#00ff00", null], [4, 0, "#0000ff", "end"]]) && w.eval("strips[0].tl.length") === 4 && w.eval("strips[0].tl.loop") === true);
  check("load: defaults for the strip the file left blank", w.eval("strips[1].count") === 1000 && w.eval("strips[1].color") === "#007800" && w.eval("strips[1].tl.length") === 10);
  check("load: new strip list sent", JSON.stringify(wr.find((x) => x[0] === "strips")) === JSON.stringify(["strips", [14, 0, 30, 4, 3, 232]]));
  check("load: each strip's color and animation replayed", JSON.stringify(wr.slice(-4)) === JSON.stringify([
    ["fill", [14, 255, 0, 0, 0, 0]],
    ["anim", [14, 2]],
    ["fill", [4, 0, 120, 0, 0, 0]],
    ["anim", [4, 0]],
  ]));
  check("load: dropped pins switched off", wr.filter((x) => x[0] === "power").every((x) => x[1][1] === 0) && wr.some((x) => JSON.stringify(x) === JSON.stringify(["power", [27, 0]])));

  // a setup that can't be read leaves the page alone and says so
  $("#cfgLoadBtn").click();
  $("#cfgText").value = "{ not json";
  $("#cfgApplyBtn").click();
  await flush();
  check("bad setup: modal stays open with a message, strips untouched", $("#cfgModal").classList.contains("open") && $("#cfgMsg").textContent.startsWith("Couldn't read") && w.eval("strips.length") === 2);
  $("#cfgText").value = JSON.stringify({ bled: 1, strips: [] });
  $("#cfgApplyBtn").click();
  await flush();
  check("empty setup: refused", $("#cfgModal").classList.contains("open") && w.eval("strips.length") === 2);
  $("#cfgCancelBtn").click();
  check("cancel closes the modal", !$("#cfgModal").classList.contains("open"));
  await take();

  // a file (dropped or picked with the file button) fills the textarea
  $("#cfgLoadBtn").click();
  check("file button sits in the drop zone and asks for json", $("#cfgDrop input[type=file]") === $("#cfgFile") && $("#cfgFile").accept.includes("json"));
  const fileText = JSON.stringify({ bled: 1, strips: [{ pin: 22, count: 12 }] });
  await w.eval(`readConfigFile({ text: async () => ${JSON.stringify(fileText)} })`);
  check("file lands in the textarea instead of loading straight away", $("#cfgText").value === fileText && w.eval("strips.length") === 2);
  $("#cfgApplyBtn").click();
  await take();
  check("loading the file's setup applies it", w.eval("strips.map((s) => [s.pin, s.count]).join()") === "22,12");

  console.log(failures ? `\n${failures} FAILED` : "\nall passed");
  process.exit(failures ? 1 : 0);
})();


