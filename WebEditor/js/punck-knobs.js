// Punck web editor add-on (modulove): knobs, randomise, mutate, undo and presets for the
// "Drum Settings" and "Reverb Settings" pages. Hooks into MIDI.js without changing it:
// every number field keeps working; a knob next to it shows the value in musical units
// and writes the exact raw value into the field, then calls the editor's own send function.
"use strict";
(function () {
  const SR = 48000, LN2 = Math.log(2), LN1000 = Math.log(0.001);
  const CONV = {
    lin:    { toRaw: v => v, fromRaw: r => r },
    omega:  { toRaw: v => v / (SR / 2), fromRaw: r => r * (SR / 2) },
    decay:  { toRaw: v => Math.exp(LN1000 / (SR * v / 1000)), fromRaw: r => r >= 1 ? 99999 : LN1000 / Math.log(r) / SR * 1000 },
    drop:   { toRaw: v => Math.exp(-v * LN2 / SR), fromRaw: r => -SR * Math.log(r) / LN2 },
    rise:   { toRaw: v => Math.exp(v * LN2 / SR), fromRaw: r => SR * Math.log(r) / LN2 },
    attack: { toRaw: v => 1 / (SR / 1000 * v), fromRaw: r => 1 / (SR / 1000 * r) },
    pct:    { toRaw: v => v / 100, fromRaw: r => r * 100 },
    chans:  { toRaw: v => [0, 4, 8][Math.round(v)], fromRaw: r => r >= 8 ? 2 : r >= 4 ? 1 : 0 },
  };
  const NN = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  const noteName = hz => { const n = Math.round(12 * Math.log2(hz / 440) + 69); return NN[(n + 120) % 12] + (Math.floor(n / 12) - 1); };
  function fmt(v, unit) {
    switch (unit) {
      case "Hz": return (v >= 1000 ? (v / 1000).toFixed(2) + "k" : v.toFixed(v < 100 ? 1 : 0)) + " Hz";
      case "Hzn": return v.toFixed(1) + " Hz " + noteName(v);
      case "ms": return v >= 1000 ? (v / 1000).toFixed(2) + " s" : v.toFixed(v < 10 ? 1 : 0) + " ms";
      case "oct/s": return v.toFixed(v < 10 ? 2 : 0) + " oct/s";
      case "%": return v.toFixed(2) + " %";
      case "x": return "×" + v.toFixed(2);
      case "int": return Math.round(v) + "";
      case "chans": return ["0", "4", "8"][Math.round(v)] + " ch";
      default: return v.toFixed(Math.abs(v) < 1 ? 3 : 2);
    }
  }

  // Knob metadata in the same order as kickSettings / snareSettings / ... in MIDI.js: [conv, unit, min, max, scale]
  const L = (min, max) => ["lin", "", min, max, "lin"];
  const META = {
    0: [L(0.05, 0.8), ["lin", "", 0.005, 0.15, "log"], L(0.05, 0.9), ["lin", "Hz", 60, 500, "log"], ["lin", "Hzn", 30, 110, "log"], ["drop", "oct/s", 0, 4, "lin"]],
    1: [L(0, 2), ["decay", "ms", 10, 800, "log"], ["lin", "Hz", 80, 900, "log"], L(0, 3.14), ["decay", "ms", 10, 500, "log"],
        L(-1, 1), L(-1, 1), L(-1, 1), ["lin", "x", 0.5, 2, "log"], ["lin", "x", 1, 5, "log"], ["lin", "x", 1, 6, "log"]],
    2: [["attack", "ms", 0.05, 8, "log"], null, ["omega", "Hz", 300, 15000, "log"], ["omega", "Hz", 1000, 18000, "log"], ["rise", "oct/s", 5, 1500, "log"],
        ["omega", "Hz", 2000, 23500, "log"], ["omega", "Hz", 500, 20000, "log"], ["drop", "oct/s", 0.1, 30, "log"], L(0, 3), ["decay", "ms", 5, 1500, "log"],
        ...Array(6).fill(L(0, 1.5)), ...Array(6).fill(["lin", "Hz", 100, 12000, "log"])],
    3: [["decay", "ms", 20, 2000, "log"], ["decay", "ms", 20, 2000, "log"], L(0.05, 0.9), ["lin", "Hz", 30, 300, "log"], ["lin", "x", 0.5, 3, "log"], ["lin", "x", 0.5, 3, "log"],
        L(0, 1), L(0, 1), ["drop", "oct/s", 0, 8, "lin"]],
    4: [L(0, 10), L(0, 10), ["decay", "ms", 5, 200, "log"], ["decay", "ms", 20, 1500, "log"], ["lin", "Hz", 300, 6000, "log"], ["lin", "", 0.5, 10, "log"], L(0, 0.3)],
    rev: [["pct", "%", 0, 5, "lin"], ["lin", "int", 20, 150, "lin"], ["lin", "int", 0, 3, "lin"], ["chans", "chans", 0, 2, "lin"], ["lin", "Hz", 300, 12000, "log"]],
  };

  // Presets as flat value lists in settings order (Kick 6, Snare 11, HiHat 22, Toms 9, Claps 7, Reverb 5)
  const PRESETS = {
    "Deep techno": [
      [0.4, 0.035, 0.5, 230, 46, 0.999995],
      [1.0, 0.9975, 185, 0.5, 0.9955, 0.55, 0.35, -0.2, 1.0, 1.588, 2.13],
      [0.04, 0.9991, 0.12, 0.34, 1.01, 0.85, 0.3, 0.9999, 1.4, 0.999, 0.55, 0.35, 0.28, 0.28, 0.35, 0.28, 683, 745, 1871, 2467, 3960, 6618],
      [0.9992, 0.999, 0.3, 70, 1.0, 1.588, 1.0, 0.0, 0.99996],
      [5, 3.5, 0.9925, 0.9993, 1300, 2.0, 0.04],
      [0.006, 70, 3, 8, 3000]],
    "Dub techno": [
      [0.22, 0.02, 0.3, 140, 50, 0.999994],
      [0.12, 0.99, 380, 1.2, 0.995, 0.8, 0.45, 0.25, 1.0, 2.2, 3.7],
      [0.02, 0.9991, 0.10, 0.28, 1.01, 0.60, 0.15, 0.99998, 1.0, 0.9997, 0.8, 0.5, 0.4, 0.4, 0.5, 0.4, 569, 621, 1559, 2056, 3300, 5515],
      [0.9992, 0.999, 0.3, 70, 1.0, 1.588, 1.0, 0.0, 0.99992],
      [5, 3, 0.9925, 0.9995, 1150, 2.2, 0.03],
      [0.012, 110, 3, 8, 2200]],
    "Factory": [
      [0.22, 0.015, 0.3, 130, 60, 0.999985],
      [1.0, 0.9982, 150, 0.5, 0.9982, 0.7, 0.5, -0.4, 1.0, 1.588, 1.833],
      [0.005, 0.9991, 0.07, 0.27, 1.01, 0.5, 0.1, 0.99998, 0.99, 0.9998, 0.8, 0.5, 0.4, 0.4, 0.5, 0.4, 569, 621, 1559, 2056, 3300, 5515],
      [0.9995, 0.9993, 0.2, 60, 1.0, 1.588, 1.0, 0.0, 0.99995],
      [5, 4, 0.993, 0.999, 1250, 3, 0.06],
      [0.01, 75, 2, 8, 2400]],
  };
  const FACTORY = PRESETS["Factory"];

  // ------------------------------------------------------------------------------------------
  const knobs = [], undo = [];
  const sendTimers = {};
  const A0 = -135, A1 = 135;
  const polar = (r, a) => { const t = (a - 90) * Math.PI / 180; return [20 + r * Math.cos(t), 20 + r * Math.sin(t)]; };
  const arc = (r, a0, a1) => { const [x0, y0] = polar(r, a0), [x1, y1] = polar(r, a1); return `M${x0},${y0} A${r},${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1},${y1}`; };

  function inputFor(block, s) { return document.getElementById(block === "rev" ? `reverbSettings${s}` : `drumSettings${block}${s}`); }
  function send(block) {
    clearTimeout(sendTimers[block]);
    sendTimers[block] = setTimeout(() => { block === "rev" ? window.updateReverb() : window.updateConfig(block); }, 25);
  }
  function blocksOnPage() { return knobs.length ? [...new Set(knobs.map(k => k.block))] : []; }

  function makeKnob(cell, block, s, meta) {
    const [conv, unit, min, max, scale] = meta, c = CONV[conv], input = inputFor(block, s);
    const wrap = document.createElement("div");
    wrap.style.cssText = "display:flex;align-items:center;gap:6px;user-select:none;touch-action:none";
    wrap.innerHTML = `<svg viewBox="0 0 40 40" width="34" height="34" style="cursor:ns-resize">
      <path d="${arc(15, A0, A1)}" stroke="#3a4049" stroke-width="4" fill="none" stroke-linecap="round"/>
      <path class="fg" stroke="#f0a23a" stroke-width="4" fill="none" stroke-linecap="round"/>
      <line class="ptr" x1="20" y1="20" x2="20" y2="7" stroke="#ddd" stroke-width="2" stroke-linecap="round"/></svg>
      <span class="kv" style="font-size:12px;color:#ccc;font-variant-numeric:tabular-nums;white-space:nowrap"></span>`;
    cell.appendChild(wrap);
    const k = { block, s, input, wrap,
      norm: v => scale === "log" ? Math.log(Math.max(v, min) / min) / Math.log(max / min) : (v - min) / (max - min),
      denorm: t => { t = Math.min(1, Math.max(0, t)); return scale === "log" ? min * Math.pow(max / min, t) : min + t * (max - min); },
      value: () => c.fromRaw(parseFloat(input.value)),
      set(v, doSend = true) {
        v = Math.min(max, Math.max(min, v));
        if (unit === "int" || conv === "chans") v = Math.round(v);
        input.value = +c.toRaw(v).toPrecision(8);
        k.draw(); if (doSend) send(block);
      },
      draw() {
        const v = k.value(); if (!isFinite(v)) { wrap.querySelector(".kv").textContent = ""; return; }
        const t = Math.min(1, Math.max(0, k.norm(v))), a = A0 + t * (A1 - A0);
        wrap.querySelector(".fg").setAttribute("d", t > 0.002 ? arc(15, A0, a) : "");
        const [x, y] = polar(12, a), p = wrap.querySelector(".ptr"); p.setAttribute("x2", x); p.setAttribute("y2", y);
        wrap.querySelector(".kv").textContent = fmt(v, unit);
        const f = FACTORY[block === "rev" ? 5 : block][s];
        wrap.querySelector(".kv").style.color = Math.abs(k.norm(c.fromRaw(f)) - t) > 0.01 ? "#f0a23a" : "#ccc";
      },
      reset() { k.set(c.fromRaw(FACTORY[block === "rev" ? 5 : block][s])); },
    };
    const svg = wrap.querySelector("svg");
    let y0 = 0, t0 = 0;
    svg.addEventListener("pointerdown", e => { pushUndo(); svg.setPointerCapture(e.pointerId); y0 = e.clientY; t0 = k.norm(k.value()); });
    svg.addEventListener("pointermove", e => { if (svg.hasPointerCapture(e.pointerId)) k.set(k.denorm(t0 + (y0 - e.clientY) / (e.shiftKey ? 2000 : 200))); });
    svg.addEventListener("wheel", e => { e.preventDefault(); pushUndo(); k.set(k.denorm(k.norm(k.value()) - Math.sign(e.deltaY) * (e.shiftKey ? 0.005 : 0.03))); }, { passive: false });
    svg.addEventListener("dblclick", () => { pushUndo(); k.reset(); });
    input.addEventListener("input", () => k.draw());
    input.addEventListener("change", () => k.draw());
    knobs.push(k); k.draw();
  }

  function snapshot() { return knobs.map(k => [k.block, k.s, k.input.value]); }
  function pushUndo() { if (knobs.length) { undo.push(snapshot()); if (undo.length > 100) undo.shift(); } }
  function restore(snap) {
    const blocks = new Set();
    for (const [b, s, v] of snap) { const inp = inputFor(b, s); if (inp) { inp.value = v; blocks.add(b); } }
    knobs.forEach(k => k.draw()); blocks.forEach(b => send(b));
  }
  function randomise(block, amount, wild) {
    for (const k of knobs) {
      if (k.block !== block) continue;
      const t = k.norm(k.value());
      k.set(k.denorm(wild ? 0.1 + Math.random() * 0.8 : t + (Math.random() * 2 - 1) * amount), false);
    }
    send(block);
  }
  function amount() { const el = document.getElementById("pkAmount"); return (el ? el.value : 35) / 100 * 0.5; }
  function applyPreset(name) {
    const p = PRESETS[name]; pushUndo();
    for (const k of knobs) { const arr = p[k.block === "rev" ? 5 : k.block]; if (arr && arr[k.s] !== undefined) { k.input.value = arr[k.s]; k.draw(); } }
    blocksOnPage().forEach(b => send(b));
  }

  function toolbar(container, reverb) {
    const bar = document.createElement("div");
    bar.style.cssText = "display:flex;flex-wrap:wrap;gap:8px;align-items:center;padding:8px 10px;margin-bottom:8px;border:1px solid rgba(255,255,255,.15);border-radius:6px";
    bar.innerHTML = `<b style="color:#f0a23a">Sound tools</b>
      ${reverb ? "" : `<button class="topcoat-button" id="pkDiceAll">Dice all</button><button class="topcoat-button" id="pkMutate">Mutate</button>`}
      <span style="color:#aaa">Amount</span><input type="range" id="pkAmount" min="5" max="100" value="35">
      <button class="topcoat-button" id="pkUndo">Undo</button>
      <select id="pkPreset">${Object.keys(PRESETS).map(n => `<option>${n}</option>`).join("")}</select>
      <button class="topcoat-button" id="pkLoad">Load preset</button>
      <a href="tonelab.html" target="_blank" style="color:#5fb8e8;margin-left:auto">Tone Lab (groove loop) ↗</a>
      <span style="color:#888;width:100%;font-size:12px">Knobs: drag up/down (Shift = fine), wheel, double-click = factory. Orange value = changed from factory. Changes are live - use "Save Configuration" to keep them.</span>`;
    container.prepend(bar);
    const on = (id, fn) => { const el = bar.querySelector("#" + id); if (el) el.onclick = fn; };
    on("pkDiceAll", () => { pushUndo(); [0, 1, 2, 3, 4].forEach(b => randomise(b, amount(), false)); });
    on("pkMutate", () => { pushUndo(); [0, 1, 2, 3, 4].forEach(b => randomise(b, 0.06, false)); });
    on("pkUndo", () => { if (undo.length) restore(undo.pop()); });
    on("pkLoad", () => applyPreset(bar.querySelector("#pkPreset").value));
  }

  function enhance(reverb) {
    knobs.length = 0;
    const editor = document.getElementById("drumEditor");
    const grid = editor.querySelector("div[style*='grid']");
    if (!grid) return;
    grid.style.gridTemplateColumns = "250px 110px 190px";
    grid.querySelectorAll("div[style*='grid-column']").forEach(h => h.style.gridColumn = "1 / -1");
    if (reverb) {
      for (let s = 0; s < 5; s++) {
        const inp = inputFor("rev", s); if (!inp) continue;
        const cell = document.createElement("div"); cell.className = "grid-container3";
        inp.parentElement.after(cell); makeKnob(cell, "rev", s, META.rev[s]);
      }
    } else {
      for (let b = 0; b < 5; b++) {
        const head = document.getElementById(`settingsBlock${b}`);
        if (head) {
          const mk = (label, fn) => { const btn = document.createElement("button"); btn.className = "topcoat-button"; btn.style.marginLeft = "10px"; btn.textContent = label; btn.onclick = fn; head.appendChild(btn); };
          mk("Dice", () => { pushUndo(); randomise(b, amount(), false); });
          mk("Mutate", () => { pushUndo(); randomise(b, 0.06, false); });
          mk("Wild", () => { pushUndo(); randomise(b, 0, true); });
        }
        for (let s = 0; s < META[b].length; s++) {
          const inp = inputFor(b, s); if (!inp) continue;
          const cell = document.createElement("div"); cell.className = "grid-container3";
          inp.parentElement.after(cell);
          if (META[b][s]) makeKnob(cell, b, s, META[b][s]);
        }
      }
    }
    toolbar(editor, reverb);
  }

  // Hook into MIDI.js (global function declarations are looked up by name at call time)
  window.addEventListener("DOMContentLoaded", () => {
    const origConfig = window.BuildConfigHtml, origReverb = window.BuildReverbHtml, origMsg = window.getMIDIMessage;
    window.BuildConfigHtml = function () { origConfig.apply(this, arguments); enhance(false); };
    window.BuildReverbHtml = function () { origReverb.apply(this, arguments); enhance(true); };
    window.getMIDIMessage = function (m) { origMsg.apply(this, arguments); knobs.forEach(k => k.draw()); };
    document.addEventListener("keydown", e => {
      if ((e.ctrlKey || e.metaKey) && e.key === "z" && knobs.length && e.target.tagName !== "INPUT") { e.preventDefault(); if (undo.length) restore(undo.pop()); }
    });
  });
})();
