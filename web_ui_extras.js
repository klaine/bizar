/* Bizar Remote UI extras: sequencer / arpeggiator editor and the
 * 16-step Shaper LFO designer. Spliced inline into web_ui.html by
 * scripts/inline_extras.py (edit this file, not the copy in web_ui.html).
 *
 * Everything here talks to the module only through window.schwungRemote and
 * uses keys that are not in chain_params (state-only), exactly as the DSP
 * accepts them: seq_* (including the step-automation lanes,
 * seq_laneN_{dest,smooth,data}) and lfoN_shp_{steps,ampK,curveK,slopeK,onK}.
 */
(function () {
  "use strict";

  var SEQ_MODES = ["Off", "Arp", "Seq"];
  var ARP_MODES = ["Up", "Down", "Up/Down", "Random", "Played", "Random 2", "Chord", "Poly"];
  var DIVISIONS = ["1/2D", "1/2", "1/2T", "1/4D", "1/4", "1/4T", "1/8D", "1/8", "1/8T",
    "1/16D", "1/16", "1/16T", "1/32D", "1/32", "1/32T"];
  var MAX_STEPS = 64, MAX_LAYERS = 6, SH_STEPS = 16, AUTO_LANES = 4;
  /* Modulation destinations in the DSP's DEST_* order (seq_laneN_dest holds
   * the index). */
  var DESTS = ["Off", "Pitch", "Cutoff", "Shape 1", "Shape 2", "Volume 2", "Resonance", "Output",
    "Timbre 1", "Timbre 2", "Wave 1", "Wave 2", "Pitch 1", "Pitch 2", "Volume 1", "VCA", "Env Amount",
    "Glide", "Attack", "Decay", "Sustain", "Release", "LFO 1 Rate", "LFO 2 Rate", "LFO 1 Amp",
    "LFO 2 Amp", "CycEnv Amp", "FX 1 Time", "FX 1 Intensity", "FX 1 Amount", "FX 2 Time",
    "FX 2 Intensity", "FX 2 Amount", "FX 3 Time", "FX 3 Intensity", "FX 3 Amount",
    "Macro 1", "Macro 2", "Macro 3", "Macro 4", "CycEnv Rise", "CycEnv Fall", "CycEnv Hold"];
  var NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

  var R = null, comp = "synth", setPanel = null, onShaper = null, shaperFrame = 0;
  function k(key) { return comp + ":" + key; }
  function make(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
  function noteName(n) { return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1); }
  function isBlack(n) { return [1, 3, 6, 8, 10].indexOf(n % 12) >= 0; }
  function debounce(fn, ms) {
    var t = null;
    return function () { clearTimeout(t); t = setTimeout(fn, ms); };
  }
  function select(options, onChange, extraClass) {
    var s = make("select", "sfx-input " + (extraClass || ""));
    options.forEach(function (o, i) { var op = make("option", "", o); op.value = String(i); s.appendChild(op); });
    s.addEventListener("change", function () { onChange(Number(s.value)); });
    return s;
  }
  function field(label, control) {
    var w = make("label", "sfx-field");
    w.appendChild(make("span", "", label));
    w.appendChild(control);
    return w;
  }
  function button(text, onClick, title) {
    var b = make("button", "sfx-btn", text);
    b.type = "button";
    if (title) b.title = title;
    b.addEventListener("click", onClick);
    return b;
  }
  function slider(min, max, step, onInput) {
    var s = make("input", "sfx-range");
    s.type = "range"; s.min = min; s.max = max; s.step = step;
    s.addEventListener("input", function () { onInput(Number(s.value)); });
    return s;
  }
  function pointerDrag(el, onDown, onMove) {
    el.addEventListener("pointerdown", function (e) {
      if (e.button !== 0) return;
      el.setPointerCapture(e.pointerId);
      var r = el.getBoundingClientRect();
      onDown(e.clientX - r.left, e.clientY - r.top, e);
      function move(ev) { var rr = el.getBoundingClientRect(); onMove(ev.clientX - rr.left, ev.clientY - rr.top, ev); }
      function up() { el.removeEventListener("pointermove", move); el.removeEventListener("pointerup", up); el.removeEventListener("pointercancel", up); }
      el.addEventListener("pointermove", move);
      el.addEventListener("pointerup", up);
      el.addEventListener("pointercancel", up);
    });
  }
  function cssVar(name, fallback) {
    var v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return v || fallback;
  }
  function prep(canvas, w, h) {
    var ratio = window.devicePixelRatio || 1;
    canvas.style.width = w + "px"; canvas.style.height = h + "px";
    canvas.width = Math.round(w * ratio); canvas.height = Math.round(h * ratio);
    var c = canvas.getContext("2d");
    c.setTransform(ratio, 0, 0, ratio, 0, 0);
    return c;
  }

  /* ---------------------------------------------------------------- Shaper */

  function newShaper() {
    var s = { mode: 0, steps: 8, amp: [], curve: [], slope: [], on: [] };
    var base = [-1, -0.5, 0, 0.5, 1, 0.5, 0, -0.5];
    for (var i = 0; i < SH_STEPS; i++) { s.amp.push(i < 8 ? base[i] : 0); s.curve.push(0.5); s.slope.push(1); s.on.push(1); }
    return s;
  }
  var shapers = [newShaper(), newShaper()];

  /* Mirrors shaper_sample() in src/modulation.c so the drawing is the sound. */
  function bend(t, curve) {
    var kk = (curve - 0.5) * 8;
    if (kk > -0.01 && kk < 0.01) return t;
    return (1 - Math.exp(-kk * t)) / (1 - Math.exp(-kk));
  }
  /* MiniFreak mode, mirroring shaper_mf_sample(): each step draws its own
   * segment from 0 to its level; slope is a four-way menu, on 0/1/2 is
   * rest/play/tie, and curve is straight between 0.35 and 0.65. */
  var MF_K = [-29.3, -21.5, -15.2, -10.2, -6.5, -3.76, -1.94, 0, 0, 0, 0, 0, 0, 0,
    1.89, 3.7, 6.4, 10.17, 15.23, 21.76, 29.93];
  function mfBend(x, curve) {
    var pos = clamp(curve, 0, 1) * 20, i = Math.min(19, Math.floor(pos));
    var kk = MF_K[i] + (MF_K[i + 1] - MF_K[i]) * (pos - i);
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    if (kk > -0.01 && kk < 0.01) return x;
    return (1 - Math.exp(-kk * x)) / (1 - Math.exp(-kk));
  }
  function mfShaperSample(s, phase) {
    var n = clamp(s.steps, 1, SH_STEPS);
    var pos = phase * n, step = Math.min(n - 1, Math.max(0, Math.floor(pos)));
    var start = step, back = 0;
    while (s.on[start] === 2 && back < n - 1) { start = (start + n - 1) % n; back++; }
    var len = back + 1;
    while (len < n && s.on[(start + len) % n] === 2) len++;
    if (!s.on[start]) return 0;
    var t = (back + pos - step) / len, type = Math.floor(s.slope[start] * 4), c = s.curve[start];
    var shape = type <= 0 ? mfBend(t, c) : type === 2 ? mfBend(t < 0.5 ? 2 * t : 2 - 2 * t, c) : mfBend(1 - t, c);
    return s.amp[start] * shape;
  }
  function shaperSample(s, phase) {
    if (s.mode === 1) return mfShaperSample(s, phase);
    var n = clamp(s.steps, 1, SH_STEPS);
    var pos = phase * n, step = Math.floor(pos);
    if (step < 0) step = 0;
    if (step >= n) step = n - 1;
    var frac = pos - step;
    var lvl = function (i) { return s.on[i] ? s.amp[i] : 0; };
    var cur = lvl(step), slope = s.slope[step];
    if (slope <= 0.0005) return cur;
    var start = 1 - slope;
    if (frac <= start) return cur;
    var t = (frac - start) / slope;
    var next = lvl((step + 1) % n);
    return cur + (next - cur) * bend(t, s.curve[step]);
  }

  function readShaper(i) {
    var p = "lfo" + (i + 1) + "_shp_", s = shapers[i], jobs = [];
    jobs.push(R.getParam(k(p + "steps")).then(function (v) { var n = Math.round(Number(v)); if (n >= 1) s.steps = clamp(n, 1, SH_STEPS); }));
    jobs.push(R.getParam(k(p + "mode")).then(function (v) { s.mode = Number(v) > 0.5 ? 1 : 0; }));
    for (var j = 0; j < SH_STEPS; j++) (function (j) {
      ["amp", "curve", "slope", "on"].forEach(function (f) {
        jobs.push(R.getParam(k(p + f + (j + 1))).then(function (v) {
          var n = Number(v);
          if (isFinite(n)) s[f][j] = f === "on" ? (n >= 1.5 ? 2 : n > 0.5 ? 1 : 0) : n;
        }));
      });
    }(j));
    return Promise.all(jobs);
  }
  function writeShaperKey(i, field, step) {
    var s = shapers[i], p = "lfo" + (i + 1) + "_shp_";
    if (field === "steps") R.setParam(k(p + "steps"), String(s.steps));
    else R.setParam(k(p + field + (step + 1)), String(s[field][step]));
    /* The LFO cards draw from these shapers too; repaint them once per frame. */
    if (onShaper && !shaperFrame) shaperFrame = requestAnimationFrame(function () { shaperFrame = 0; onShaper(); });
  }

  /* ------------------------------------------------------------ LFO designer */

  function mountLfo(host, api) {
    var cur = 0, sel = 0, W = 760, H = 230, PAD = 14;
    var card = make("div", "card sfx-card");
    var head = make("div", "sfx-head");
    head.appendChild(make("h2", "", "LFO Designer — Shaper"));
    var which = make("div", "sfx-seg");
    var whichBtns = [0, 1].map(function (i) {
      var b = make("button", "", "LFO " + (i + 1)); b.type = "button";
      b.addEventListener("click", function () { choose(i); });
      which.appendChild(b); return b;
    });
    head.appendChild(which);
    var shapeNote = make("span", "sfx-note", "");
    head.appendChild(shapeNote);
    var useBtn = button("Use Shaper shape", function () {
      var key = "lfo" + (cur + 1) + "_shape";
      if (setPanel) setPanel(key, "Shaper"); else R.setParam(k(key), "Shaper");
      readShapeNote();
    }, "Set this LFO's shape to Shaper");
    head.appendChild(useBtn);
    card.appendChild(head);

    var stage = make("div", "sfx-stage");
    var canvas = make("canvas", "sfx-canvas");
    stage.appendChild(canvas);
    card.appendChild(stage);
    card.appendChild(make("p", "sfx-hint", "Click a step to select it · drag vertically to set its level · each step holds its level, then glides to the next step over the last Slope part of the step (Slope 0 = hard staircase). Curve bends the glide. Muted steps read as 0."));

    var insp = make("div", "sfx-row");
    var stepsSel = select(Array.apply(null, Array(16)).map(function (_, i) { return String(i + 1); }), function (v) {
      shapers[cur].steps = v + 1; sel = Math.min(sel, v); writeShaperKey(cur, "steps"); draw();
    });
    insp.appendChild(field("Steps", stepsSel));
    var selLabel = make("b", "sfx-sel", "");
    insp.appendChild(selLabel);
    var ampR = slider(-1, 1, 0.01, function (v) { shapers[cur].amp[sel] = v; writeShaperKey(cur, "amp", sel); draw(); });
    var slopeR = slider(0, 1, 0.01, function (v) { shapers[cur].slope[sel] = v; writeShaperKey(cur, "slope", sel); draw(); });
    var curveR = slider(0, 1, 0.01, function (v) { shapers[cur].curve[sel] = v; writeShaperKey(cur, "curve", sel); draw(); });
    var onB = make("input"); onB.type = "checkbox";
    onB.addEventListener("change", function () { shapers[cur].on[sel] = onB.checked ? 1 : 0; writeShaperKey(cur, "on", sel); draw(); });
    insp.appendChild(field("Level", ampR));
    insp.appendChild(field("Slope", slopeR));
    insp.appendChild(field("Curve", curveR));
    insp.appendChild(field("Active", onB));
    card.appendChild(insp);

    var tools = make("div", "sfx-row sfx-tools");
    var presets = {
      "Triangle": function (n, i) { return { amp: 1 - Math.abs(2 * (i + 0.5) / n * 2 - 2) , slope: 1 }; },
      "Ramp up": function (n, i) { return { amp: -1 + 2 * i / Math.max(1, n - 1), slope: 1 }; },
      "Ramp down": function (n, i) { return { amp: 1 - 2 * i / Math.max(1, n - 1), slope: 1 }; },
      "Staircase up": function (n, i) { return { amp: -1 + 2 * i / Math.max(1, n - 1), slope: 0 }; },
      "Staircase down": function (n, i) { return { amp: 1 - 2 * i / Math.max(1, n - 1), slope: 0 }; },
      "Square": function (n, i) { return { amp: i < n / 2 ? 1 : -1, slope: 0 }; },
      "Sine": function (n, i) { return { amp: Math.sin(2 * Math.PI * i / n), slope: 1 }; },
      "Bounce": function (n, i) { return { amp: i % 2 ? -1 : 1, slope: 0.5 }; },
      "Pulse": function (n, i) { return { amp: i === 0 ? 1 : -1, slope: 0.25 }; },
      "Random": function () { return { amp: Math.round((Math.random() * 2 - 1) * 100) / 100, slope: Math.random() < 0.5 ? 0 : 1 }; }
    };
    var presetSel = select(["Preset…"].concat(Object.keys(presets)), function (v) {
      if (!v) return;
      var name = Object.keys(presets)[v - 1], s = shapers[cur], n = s.steps;
      for (var i = 0; i < SH_STEPS; i++) {
        if (i < n) {
          var p = presets[name](n, i);
          s.amp[i] = Math.round(clamp(p.amp, -1, 1) * 100) / 100; s.slope[i] = p.slope; s.curve[i] = 0.5; s.on[i] = 1;
        }
      }
      writeAll(cur); presetSel.value = "0"; draw();
    });
    tools.appendChild(presetSel);
    function bulk(label, fn, title) { tools.appendChild(button(label, function () { fn(shapers[cur], shapers[cur].steps); writeAll(cur); draw(); }, title)); }
    bulk("Invert", function (s, n) { for (var i = 0; i < n; i++) s.amp[i] = -s.amp[i]; });
    bulk("Reverse", function (s, n) { ["amp", "curve", "slope", "on"].forEach(function (f) { var a = s[f].slice(0, n).reverse(); for (var i = 0; i < n; i++) s[f][i] = a[i]; }); });
    bulk("◀ Shift", function (s, n) { ["amp", "curve", "slope", "on"].forEach(function (f) { var a = s[f].slice(0, n); a.push(a.shift()); for (var i = 0; i < n; i++) s[f][i] = a[i]; }); }, "Rotate the pattern one step left");
    bulk("Shift ▶", function (s, n) { ["amp", "curve", "slope", "on"].forEach(function (f) { var a = s[f].slice(0, n); a.unshift(a.pop()); for (var i = 0; i < n; i++) s[f][i] = a[i]; }); }, "Rotate the pattern one step right");
    bulk("Smooth", function (s, n) { for (var i = 0; i < n; i++) { s.slope[i] = 1; s.curve[i] = 0.5; } }, "Slope 1 on every step: continuous glide");
    bulk("Stairs", function (s, n) { for (var i = 0; i < n; i++) s.slope[i] = 0; }, "Slope 0 on every step: hard steps");
    bulk("Double steps", function (s, n) {
      if (n > 8) return;
      for (var i = 0; i < n; i++) ["amp", "curve", "slope", "on"].forEach(function (f) { s[f][i + n] = s[f][i]; });
      s.steps = n * 2;
    }, "Repeat the pattern to fill twice the steps (max 16)");
    var shDoubleBtn = tools.lastChild;
    tools.appendChild(button("Copy → other LFO", function () {
      var o = 1 - cur, a = shapers[cur], b = shapers[o];
      b.steps = a.steps;
      ["amp", "curve", "slope", "on"].forEach(function (f) { b[f] = a[f].slice(); });
      writeAll(o);
    }));
    card.appendChild(tools);
    host.appendChild(card);

    function writeAll(i) {
      var s = shapers[i];
      writeShaperKey(i, "steps");
      for (var j = 0; j < SH_STEPS; j++) ["amp", "curve", "slope", "on"].forEach(function (f) { writeShaperKey(i, f, j); });
      stepsSel.value = String(s.steps - 1);
    }

    function stepX(i, n) { return PAD + (W - 2 * PAD) * i / n; }
    function yOf(v) { return PAD + (H - 2 * PAD) * (1 - (v + 1) / 2); }
    function draw() {
      var rect = stage.getBoundingClientRect();
      W = Math.max(320, Math.round(rect.width || 760));
      var c = prep(canvas, W, H);
      var s = shapers[cur], n = s.steps;
      var col = cur ? "#7b93ff" : "#4cd9ff";
      c.fillStyle = "#0e100d"; c.fillRect(0, 0, W, H);
      for (var i = 0; i < n; i++) {
        var x0 = stepX(i, n), x1 = stepX(i + 1, n);
        if (i === sel) { c.fillStyle = "rgba(214,255,56,.09)"; c.fillRect(x0, PAD, x1 - x0, H - 2 * PAD); }
        c.strokeStyle = i % 4 === 0 ? "#3a3f33" : "#252821"; c.lineWidth = 1;
        c.beginPath(); c.moveTo(x0, PAD); c.lineTo(x0, H - PAD); c.stroke();
        c.fillStyle = s.on[i] ? "#98a08c" : "#4a4f44"; c.font = "10px system-ui"; c.textAlign = "center";
        c.fillText(String(i + 1), (x0 + x1) / 2, H - 2);
      }
      c.strokeStyle = "#252821"; c.beginPath(); c.moveTo(stepX(n, n), PAD); c.lineTo(stepX(n, n), H - PAD); c.stroke();
      c.strokeStyle = "#3a3f33"; c.beginPath(); c.moveTo(PAD, yOf(0)); c.lineTo(W - PAD, yOf(0)); c.stroke();
      /* level bars */
      for (i = 0; i < n; i++) {
        var a = s.on[i] ? s.amp[i] : 0;
        c.fillStyle = s.on[i] ? "rgba(214,255,56,.16)" : "rgba(120,120,120,.10)";
        var top = Math.min(yOf(a), yOf(0)), hh = Math.abs(yOf(a) - yOf(0));
        c.fillRect(stepX(i, n) + 3, top, stepX(i + 1, n) - stepX(i, n) - 6, Math.max(1, hh));
      }
      /* resulting wave */
      c.beginPath();
      var N = 480;
      for (i = 0; i <= N; i++) {
        var ph = i / N;
        var v = shaperSample(s, Math.min(ph, 0.99999));
        var px = PAD + (W - 2 * PAD) * ph, py = yOf(v);
        if (i) c.lineTo(px, py); else c.moveTo(px, py);
      }
      c.lineWidth = 2; c.strokeStyle = col; c.shadowColor = col; c.shadowBlur = 6; c.stroke(); c.shadowBlur = 0;
      /* handles */
      for (i = 0; i < n; i++) {
        var hx = (stepX(i, n) + stepX(i + 1, n)) / 2, hy = yOf(s.on[i] ? s.amp[i] : 0);
        c.beginPath(); c.arc(hx, hy, i === sel ? 6 : 4.5, 0, Math.PI * 2);
        c.fillStyle = s.on[i] ? (i === sel ? "#d6ff38" : col) : "#626858"; c.fill();
        c.lineWidth = 1.5; c.strokeStyle = "#0e100d"; c.stroke();
      }
      syncInspector();
    }
    function syncInspector() {
      var s = shapers[cur];
      stepsSel.value = String(s.steps - 1);
      selLabel.textContent = "Step " + (sel + 1);
      ampR.value = s.amp[sel]; slopeR.value = s.slope[sel]; curveR.value = s.curve[sel]; onB.checked = !!s.on[sel];
      whichBtns.forEach(function (b, i) { b.className = i === cur ? "on" : ""; });
      shDoubleBtn.disabled = s.steps > 8;
    }
    function hit(x) {
      var s = shapers[cur];
      return clamp(Math.floor((x - PAD) / (W - 2 * PAD) * s.steps), 0, s.steps - 1);
    }
    var lastWrite = 0;
    function setLevel(y, fine) {
      var v = (1 - (y - PAD) / (H - 2 * PAD)) * 2 - 1;
      v = Math.round(clamp(v, -1, 1) * (fine ? 100 : 20)) / (fine ? 100 : 20);
      var s = shapers[cur];
      if (s.amp[sel] === v) return;
      s.amp[sel] = v; s.on[sel] = 1;
      writeShaperKey(cur, "amp", sel); draw();
    }
    /* Both clicks of a double-click also set the level, so remember the step
     * as it was before the first click and restore it before toggling. */
    var beforeClick = null, lastDown = 0, clicks = 0;
    pointerDrag(canvas, function (x, y, e) {
      sel = hit(x);
      var s = shapers[cur], now = Date.now();
      var sameSpot = beforeClick && beforeClick.lfo === cur && beforeClick.step === sel;
      clicks = now - lastDown < 500 && sameSpot ? clicks + 1 : 1;
      /* A double-click is clicks 1+2 (or 3+4...): snapshot on the odd click. */
      if (clicks % 2) beforeClick = { lfo: cur, step: sel, amp: s.amp[sel], on: s.on[sel] };
      lastDown = now;
      setLevel(y, e.shiftKey);
    }, function (x, y, e) { setLevel(y, e.shiftKey); });
    canvas.addEventListener("dblclick", function (e) {
      var r = canvas.getBoundingClientRect(), s = shapers[cur];
      sel = hit(e.clientX - r.left);
      if (beforeClick && beforeClick.lfo === cur && beforeClick.step === sel) {
        s.amp[sel] = beforeClick.amp; s.on[sel] = beforeClick.on;
        writeShaperKey(cur, "amp", sel);
      }
      beforeClick = null; clicks = 0;
      s.on[sel] = s.on[sel] ? 0 : 1; writeShaperKey(cur, "on", sel); draw();
    });
    window.addEventListener("resize", function () { if (host.offsetParent) draw(); });
    if (window.ResizeObserver) new ResizeObserver(function () { if (host.offsetParent && Math.abs(stage.clientWidth - W) > 2) draw(); }).observe(stage);

    function choose(i) { cur = i; sel = Math.min(sel, shapers[i].steps - 1); readShapeNote(); draw(); }
    function readShapeNote() {
      R.getParam(k("lfo" + (cur + 1) + "_shape")).then(function (v) {
        shapeNote.textContent = String(v) === "Shaper" ? "" : "LFO " + (cur + 1) + " currently uses the “" + v + "” wave";
        useBtn.hidden = String(v) === "Shaper";
      });
    }
    return {
      refresh: function () { return Promise.all([readShaper(0), readShaper(1)]).then(function () { readShapeNote(); draw(); }); },
      redraw: draw,
      choose: choose,
      shapeChanged: readShapeNote
    };
  }

  /* --------------------------------------------------------------- Sequencer */

  function mountSeq(host) {
    var st = { mode: 0, arp: 0, oct: 0, div: 10, gate: 0.5, swing: 0, length: 16, steps: [], lanes: [] };
    for (var i = 0; i < MAX_STEPS; i++) st.steps.push([]);
    for (i = 0; i < AUTO_LANES; i++) st.lanes.push(newLane());
    var curLane = 0;
    function newLane() { var v = []; for (var j = 0; j < MAX_STEPS; j++) v.push(0); return { dest: 0, smooth: 1, v: v }; }
    var sel = null;            /* {s: step, i: index in step} */
    var base = 48, ROWS = 25, CW = 30, RH = 15, LANE = 64;
    var defVel = 100, defLen = 0;

    function parse(data) {
      st.steps = [];
      for (var i = 0; i < MAX_STEPS; i++) st.steps.push([]);
      String(data || "").split(",").forEach(function (chunk, si) {
        if (si >= MAX_STEPS || !chunk) return;
        chunk.split("+").forEach(function (n) {
          var m = n.split(".");
          if (!m[0] || isNaN(Number(m[0]))) return;
          if (st.steps[si].length < MAX_LAYERS)
            st.steps[si].push({ p: clamp(+m[0] | 0, 0, 127), v: clamp(+m[1] || 100, 1, 127), l: clamp(+m[2] | 0, 0, 127) });
        });
      });
    }
    function serialize() {
      var last = st.length - 1;
      for (var i = 0; i < MAX_STEPS; i++) if (st.steps[i].length && i > last) last = i;
      var out = [];
      for (i = 0; i <= last; i++) out.push(st.steps[i].map(function (n) { return n.p + "." + n.v + "." + n.l; }).join("+"));
      return out.join(",");
    }
    var pushData = debounce(function () {
      var d = serialize();
      raw.value = d;
      R.setParam(k("seq_data"), d);
    }, 50);
    function changed() { pushData(); draw(); }
    function put(key, v) { R.setParam(k(key), String(v)); }

    var card = make("div", "card sfx-card");
    var head = make("div", "sfx-head");
    head.appendChild(make("h2", "", "Sequencer / Arpeggiator"));
    var modeSeg = make("div", "sfx-seg");
    var modeBtns = SEQ_MODES.map(function (m, i) {
      var b = make("button", "", m); b.type = "button";
      b.addEventListener("click", function () { st.mode = i; put("seq_mode", i); sync(); });
      modeSeg.appendChild(b); return b;
    });
    head.appendChild(modeSeg);
    var playing = false, playBtn = null;
    if (R.web) {
      /* The sequencer runs while a key is held: Play holds C4, which plays
       * the pattern untransposed (and gives the arpeggiator one note). */
      playBtn = button("▶ Play", function () {
        playing = !playing;
        if (playing) R.web.noteOn(60, 100); else R.web.noteOff(60);
        playBtn.textContent = playing ? "■ Stop" : "▶ Play";
        playBtn.classList.toggle("on", playing);
      }, "Hold C4 so the pattern plays as written (Arp: plays C4)");
      playBtn.classList.add("sfx-play");
      head.appendChild(playBtn);
    }
    head.appendChild(make("span", "sfx-note", (R.web ? "Press Play or hold keys to start. " : "Hold keys to start. ")
      + "Seq plays the pattern, transposed by the last key held vs. C4; Arp arpeggiates the held keys."));
    card.appendChild(head);

    var ctl = make("div", "sfx-row");
    var divSel = select(DIVISIONS, function (v) { st.div = v; put("seq_div", v); });
    var gateR = slider(0, 1, 0.01, function (v) { st.gate = v; put("seq_gate", v); });
    var swingR = slider(0, 1, 0.01, function (v) { st.swing = v; put("seq_swing", v); });
    var lenIn = make("input", "sfx-input sfx-num"); lenIn.type = "number"; lenIn.min = 1; lenIn.max = 64;
    lenIn.addEventListener("change", function () { st.length = clamp(Math.round(+lenIn.value || 16), 1, 64); lenIn.value = st.length; put("seq_length", st.length); changed(); });
    var arpSel = select(ARP_MODES, function (v) { st.arp = v; put("seq_arp_mode", v); });
    var octSel = select(["1", "2", "3", "4"], function (v) { st.oct = v; put("seq_arp_oct", v); });
    ctl.appendChild(field("Rate", divSel)); ctl.appendChild(field("Gate", gateR)); ctl.appendChild(field("Swing", swingR));
    ctl.appendChild(field("Length", lenIn)); ctl.appendChild(field("Arp mode", arpSel)); ctl.appendChild(field("Arp octaves", octSel));
    card.appendChild(ctl);

    /* piano roll */
    var rollHead = make("div", "sfx-row");
    rollHead.appendChild(make("b", "sfx-sel", "Pattern"));
    rollHead.appendChild(button("▲ Oct", function () { base = clamp(base + 12, 0, 103); draw(); }, "Scroll view up an octave"));
    rollHead.appendChild(button("▼ Oct", function () { base = clamp(base - 12, 0, 103); draw(); }, "Scroll view down an octave"));
    rollHead.appendChild(button("Fit", fit, "Scroll to the notes in the pattern"));
    var defVelR = slider(1, 127, 1, function (v) { defVel = v; });
    defVelR.value = defVel;
    rollHead.appendChild(field("New note vel", defVelR));
    var defLenIn = make("input", "sfx-input sfx-num"); defLenIn.type = "number"; defLenIn.min = 0; defLenIn.max = 127; defLenIn.value = 0;
    defLenIn.addEventListener("change", function () { defLen = clamp(Math.round(+defLenIn.value || 0), 0, 127); });
    rollHead.appendChild(field("New note len", defLenIn));
    var ghostInfo = make("span", "sfx-note sfx-ghost", "");
    ghostInfo.appendChild(document.createTextNode(""));
    var trimBtn = button("Trim", function () {
      for (var i = st.length; i < MAX_STEPS; i++) st.steps[i] = [];
      changed();
    }, "Delete the notes after the last step");
    ghostInfo.appendChild(trimBtn);
    rollHead.appendChild(ghostInfo);
    card.appendChild(rollHead);

    var scroller = make("div", "sfx-scroll");
    var roll = make("canvas", "sfx-canvas");
    var lane = make("canvas", "sfx-canvas sfx-lane");
    scroller.appendChild(roll); scroller.appendChild(lane);
    card.appendChild(scroller);
    card.appendChild(make("p", "sfx-hint", "Click an empty cell to add a note (up to 6 per step) and drag sideways to paint more · drag a note sideways to change its length · click the selected note again, or press Delete, to remove it · arrow keys move it · drag in the bottom lane to set a step's velocity. Note length is in extra steps (0 = just the gate). Steps after the pattern length are shown dimmed: their notes are kept but not played."));

    var insp = make("div", "sfx-row");
    var selLabel = make("b", "sfx-sel", "No note selected");
    var velR = slider(1, 127, 1, function (v) { var n = selNote(); if (n) { n.v = v; changed(); } });
    var lenN = make("input", "sfx-input sfx-num"); lenN.type = "number"; lenN.min = 0; lenN.max = 127;
    lenN.addEventListener("change", function () { var n = selNote(); if (n) { n.l = clamp(Math.round(+lenN.value || 0), 0, 127); changed(); } });
    insp.appendChild(selLabel); insp.appendChild(field("Velocity", velR)); insp.appendChild(field("Length", lenN));
    card.appendChild(insp);

    var tools = make("div", "sfx-row sfx-tools");
    function allNotes(fn) { st.steps.forEach(function (s, si) { s.forEach(function (n) { fn(n, si); }); }); }
    tools.appendChild(button("Clear", function () { st.steps.forEach(function (s) { s.length = 0; }); sel = null; changed(); }));
    function canTranspose(d) { var ok = true; allNotes(function (n) { if (n.p + d < 0 || n.p + d > 127) ok = false; }); return ok; }
    var transposeBtns = [["−1", -1], ["+1", 1], ["−Oct", -12], ["+Oct", 12]].map(function (t) {
      var b = button("Transpose " + t[0], function () {
        if (canTranspose(t[1])) { allNotes(function (n) { n.p += t[1]; }); changed(); }
      });
      b.dataset.d = t[1];
      tools.appendChild(b);
      return b;
    });
    tools.appendChild(button("◀ Rotate", function () { rot(-1); }));
    tools.appendChild(button("Rotate ▶", function () { rot(1); }));
    function rot(d) {
      var L = st.length, a = st.steps.slice(0, L);
      if (d < 0) a.push(a.shift()); else a.unshift(a.pop());
      for (var i = 0; i < L; i++) st.steps[i] = a[i];
      sel = null; changed();
    }
    tools.appendChild(button("Double", function () {
      var L = st.length; if (L * 2 > MAX_STEPS) return;
      for (var i = 0; i < L; i++) st.steps[i + L] = st.steps[i].map(function (n) { return { p: n.p, v: n.v, l: n.l }; });
      st.length = L * 2; lenIn.value = st.length; put("seq_length", st.length); changed();
    }, "Repeat the pattern to fill twice the length"));
    var seqDoubleBtn = tools.lastChild;
    tools.appendChild(button("Randomize", function () {
      var scale = [0, 3, 5, 7, 10];
      st.steps.forEach(function (s) { s.length = 0; });
      for (var i = 0; i < st.length; i++) if (Math.random() < 0.6) {
        var oct = Math.floor(Math.random() * 2) * 12;
        st.steps[i].push({ p: 60 + oct + scale[Math.floor(Math.random() * scale.length)], v: 70 + Math.floor(Math.random() * 57), l: 0 });
      }
      sel = null; fit(); changed();
    }, "Random minor-pentatonic pattern around C4"));
    card.appendChild(tools);

    var rawRow = make("div", "sfx-row");
    var raw = make("input", "sfx-input sfx-raw"); raw.type = "text"; raw.spellcheck = false;
    raw.title = "pitch.velocity.length per note, + between notes in a step, , between steps";
    raw.addEventListener("change", function () { parse(raw.value); sel = null; R.setParam(k("seq_data"), raw.value); draw(); });
    rawRow.appendChild(field("Pattern data", raw));
    card.appendChild(rawRow);

    /* Step automation: four lanes, each an offset per step on one
     * modulation destination, played with the pattern in Seq mode. */
    var autoHead = make("div", "sfx-row");
    autoHead.appendChild(make("b", "sfx-sel", "Automation"));
    var laneSeg = make("div", "sfx-seg");
    var laneBtns = [];
    for (i = 0; i < AUTO_LANES; i++) (function (li) {
      var b = make("button", "", "Lane " + (li + 1)); b.type = "button";
      b.addEventListener("click", function () { curLane = li; draw(); });
      laneSeg.appendChild(b); laneBtns.push(b);
    })(i);
    autoHead.appendChild(laneSeg);
    var destSel = select(DESTS, function (v) { st.lanes[curLane].dest = v; put("seq_lane" + (curLane + 1) + "_dest", v); draw(); });
    autoHead.appendChild(field("Destination", destSel));
    var smoothB = make("input"); smoothB.type = "checkbox";
    smoothB.addEventListener("change", function () { st.lanes[curLane].smooth = smoothB.checked ? 1 : 0; put("seq_lane" + (curLane + 1) + "_smooth", st.lanes[curLane].smooth); draw(); });
    autoHead.appendChild(field("Smooth", smoothB));
    autoHead.appendChild(button("Clear lane", function () {
      var ln = st.lanes[curLane]; for (var j = 0; j < MAX_STEPS; j++) ln.v[j] = 0; pushLane(curLane); draw();
    }, "Set every step of this lane back to no change"));
    card.appendChild(autoHead);
    var autoScroll = make("div", "sfx-scroll");
    var auto = make("canvas", "sfx-canvas");
    autoScroll.appendChild(auto);
    card.appendChild(autoScroll);
    card.appendChild(make("p", "sfx-hint", "Drag up or down in a step to set the offset it adds to the destination (−1..+1 of the knob range; Shift for fine) · double-click a step to reset it. Smooth glides from step to step; off holds each step's value. Lanes play with the pattern in Seq mode."));
    var AH = 90;
    function laneText(li) {
      var v = st.lanes[li].v, last = -1, out = [];
      for (var j = 0; j < MAX_STEPS; j++) if (v[j]) last = j;
      for (j = 0; j <= last; j++) out.push(v[j] ? String(Math.round(v[j] * 1000) / 1000) : "");
      return out.join(",");
    }
    var lanePushers = [];
    for (i = 0; i < AUTO_LANES; i++) (function (li) {
      lanePushers.push(debounce(function () { R.setParam(k("seq_lane" + (li + 1) + "_data"), laneText(li)); }, 50));
    })(i);
    function pushLane(li) { lanePushers[li](); }
    function parseLane(li, text) {
      var ln = st.lanes[li];
      for (var j = 0; j < MAX_STEPS; j++) ln.v[j] = 0;
      String(text || "").split(",").forEach(function (t, j) {
        if (j < MAX_STEPS && t !== "" && isFinite(+t)) ln.v[j] = clamp(+t, -1, 1);
      });
    }
    function drawAuto() {
      var L = st.length, ln = st.lanes[curLane], w = L * CW + 34, gutter = 34, mid = AH / 2;
      var c = prep(auto, w, AH);
      c.fillStyle = "#0e100d"; c.fillRect(0, 0, w, AH);
      c.fillStyle = "#2c3025"; c.fillRect(gutter, mid, w - gutter, 1);
      c.fillStyle = "#626858"; c.font = "9px system-ui"; c.textAlign = "left";
      c.fillText("+1", 5, 10); c.fillText("0", 5, mid + 3); c.fillText("−1", 5, AH - 3);
      var on = ln.dest > 0, rgb = on ? "214,255,56" : "130,135,120";
      for (var s = 0; s < L; s++) {
        var x0 = gutter + s * CW;
        c.fillStyle = s % 4 === 0 ? "#3a3f33" : "#20231c"; c.fillRect(x0, 0, 1, AH);
        if (s === playStep) { c.fillStyle = "rgba(214,255,56,.13)"; c.fillRect(x0, 0, CW, AH); }
        var v = ln.v[s], hh = (mid - 3) * Math.abs(v);
        c.fillStyle = "rgba(" + rgb + ",.85)";
        if (v > 0) c.fillRect(x0 + 4, mid - hh, CW - 8, hh); else if (v < 0) c.fillRect(x0 + 4, mid + 1, CW - 8, hh);
      }
      if (ln.smooth && L > 1) {
        c.strokeStyle = "rgba(" + rgb + ",.5)"; c.lineWidth = 1; c.beginPath();
        for (s = 0; s <= L; s++) {
          var xx = gutter + s * CW + CW / 2, yy = mid - (mid - 3) * ln.v[s % L];
          if (s === 0) c.moveTo(xx, yy); else c.lineTo(xx, yy);
        }
        c.stroke();
      }
      laneBtns.forEach(function (b, li) {
        b.className = li === curLane ? "on" : "";
        b.textContent = "Lane " + (li + 1) + (st.lanes[li].dest ? " · " + DESTS[st.lanes[li].dest] : "");
      });
      destSel.value = String(ln.dest); smoothB.checked = !!ln.smooth;
    }
    pointerDrag(auto, function (x, y, e) { autoSet(x, y, e.shiftKey); },
      function (x, y, e) { autoSet(x, y, e.shiftKey); });
    function autoSet(x, y, fine) {
      var s = Math.floor((x - 34) / CW), mid = AH / 2;
      if (s < 0 || s >= st.length) return;
      var v = clamp((mid - y) / (mid - 3), -1, 1);
      v = Math.round(v * (fine ? 1000 : 50)) / (fine ? 1000 : 50);
      st.lanes[curLane].v[s] = v; pushLane(curLane); drawAuto();
    }
    auto.addEventListener("dblclick", function (e) {
      var rr = auto.getBoundingClientRect(), s = Math.floor((e.clientX - rr.left - 34) / CW);
      if (s < 0 || s >= st.length) return;
      st.lanes[curLane].v[s] = 0; pushLane(curLane); drawAuto();
    });
    host.appendChild(card);

    function selNote() { return sel && st.steps[sel.s] ? st.steps[sel.s][sel.i] : null; }
    function sync() {
      modeBtns.forEach(function (b, i) { b.className = i === st.mode ? "on" : ""; });
      divSel.value = String(st.div); gateR.value = st.gate; swingR.value = st.swing; lenIn.value = st.length;
      arpSel.value = String(st.arp); octSel.value = String(st.oct);
      arpSel.parentNode.style.opacity = octSel.parentNode.style.opacity = st.mode === 1 ? 1 : 0.45;
      var n = selNote();
      selLabel.textContent = n ? "Step " + (sel.s + 1) + " · " + noteName(n.p) + " (" + n.p + ")" : "No note selected";
      if (n) { velR.value = n.v; lenN.value = n.l; }
      velR.disabled = lenN.disabled = !n;
      seqDoubleBtn.disabled = st.length * 2 > MAX_STEPS;
      transposeBtns.forEach(function (b) { b.disabled = !canTranspose(Number(b.dataset.d)); });
    }
    function fit() {
      var lo = 127, hi = 0, any = false;
      allNotes(function (n) { any = true; lo = Math.min(lo, n.p); hi = Math.max(hi, n.p); });
      if (!any) { base = 48; return; }
      base = clamp(Math.floor((lo + hi) / 2) - Math.floor(ROWS / 2), 0, 127 - ROWS + 1);
      draw();
    }
    var playStep = -1;
    function lastUsed() {
      for (var i = MAX_STEPS - 1; i >= 0; i--) if (st.steps[i].length) return i + 1;
      return 0;
    }
    function draw() { drawRoll(); drawAuto(); sync(); }
    function drawRoll() {
      var L = st.length, V = Math.max(L, lastUsed()), w = V * CW + 34;
      var gutter = 34;
      var c = prep(roll, w, ROWS * RH);
      c.fillStyle = "#0e100d"; c.fillRect(0, 0, w, ROWS * RH);
      for (var r = 0; r < ROWS; r++) {
        var pitch = base + ROWS - 1 - r, y = r * RH;
        c.fillStyle = isBlack(pitch) ? "#13150f" : "#171915"; c.fillRect(gutter, y, w - gutter, RH);
        c.fillStyle = pitch % 12 === 0 ? "#3a3f33" : "#1d201a"; c.fillRect(gutter, y + RH - 1, w - gutter, 1);
        c.fillStyle = isBlack(pitch) ? "#22251d" : "#2c3025"; c.fillRect(0, y, gutter - 1, RH - 1);
        if (pitch % 12 === 0 || r === 0) { c.fillStyle = "#98a08c"; c.font = "9px system-ui"; c.textAlign = "left"; c.fillText(noteName(pitch), 3, y + RH - 4); }
      }
      if (playStep >= 0 && playStep < L) { c.fillStyle = "rgba(214,255,56,.13)"; c.fillRect(gutter + playStep * CW, 0, CW, ROWS * RH); }
      for (var s = 0; s <= V; s++) {
        c.fillStyle = s === L ? "#d6ff38" : (s % 4 === 0 ? "#3a3f33" : "#20231c"); c.fillRect(gutter + s * CW, 0, 1, ROWS * RH);
      }
      st.steps.forEach(function (step, si) {
        if (si >= V) return;
        var past = si >= L;
        step.forEach(function (n, ni) {
          var row = base + ROWS - 1 - n.p;
          if (row < 0 || row >= ROWS) return;
          var x = gutter + si * CW + 1, y = row * RH + 1;
          var wlen = (n.l + st.gate) * CW - 2;
          var a = 0.35 + 0.65 * n.v / 127, rgb = past ? "130,135,120" : "214,255,56";
          c.fillStyle = "rgba(" + rgb + "," + (a * 0.3).toFixed(2) + ")"; c.fillRect(x, y, Math.max(CW - 2, wlen), RH - 2);
          c.fillStyle = "rgba(" + rgb + "," + a.toFixed(2) + ")"; c.fillRect(x, y, CW - 2, RH - 2);
          if (sel && sel.s === si && sel.i === ni) { c.strokeStyle = "#fff"; c.lineWidth = 1.5; c.strokeRect(x + 0.5, y + 0.5, CW - 3, RH - 3); }
        });
      });
      if (V > L) { c.fillStyle = "rgba(14,16,13,.55)"; c.fillRect(gutter + L * CW + 1, 0, (V - L) * CW, ROWS * RH); }
      var lc = prep(lane, w, LANE);
      lc.fillStyle = "#0e100d"; lc.fillRect(0, 0, w, LANE);
      lc.fillStyle = "#626858"; lc.font = "9px system-ui"; lc.fillText("VEL", 5, 12);
      for (s = 0; s < V; s++) {
        var step2 = st.steps[s], x0 = gutter + s * CW;
        lc.fillStyle = s % 4 === 0 ? "#3a3f33" : "#20231c"; lc.fillRect(x0, 0, 1, LANE);
        if (s === playStep && s < L) { lc.fillStyle = "rgba(214,255,56,.13)"; lc.fillRect(x0, 0, CW, LANE); }
        if (!step2.length) continue;
        var mx = 0; step2.forEach(function (n) { mx = Math.max(mx, n.v); });
        lc.fillStyle = s < L ? "#d6ff38" : "#6b7062"; var hh = (LANE - 6) * mx / 127;
        lc.fillRect(x0 + 4, LANE - 3 - hh, CW - 8, hh);
      }
      var hidden = 0;
      for (s = L; s < MAX_STEPS; s++) hidden += st.steps[s].length;
      ghostInfo.hidden = !hidden;
      ghostInfo.firstChild.nodeValue = hidden ? hidden + " note" + (hidden > 1 ? "s" : "") + " after step " + L + " (kept, not played) " : "";
    }
    function cellAt(x, y) {
      var s = Math.floor((x - 34) / CW), r = Math.floor(y / RH);
      if (s < 0 || s >= st.length || r < 0 || r >= ROWS) return null;
      return { s: s, p: base + ROWS - 1 - r };
    }
    /* Pointer editing: on an empty cell, add a note and drag sideways to paint
     * the same pitch into more steps; on a note, drag sideways to change its
     * length; a plain click on the selected note removes it. */
    var gesture = null;
    function rollPoint(e) { var rr = roll.getBoundingClientRect(); return { x: e.clientX - rr.left, y: e.clientY - rr.top }; }
    roll.addEventListener("pointerdown", function (e) {
      if (e.button) return;
      var pt = rollPoint(e), cell = cellAt(pt.x, pt.y);
      if (!cell) return;
      try { roll.setPointerCapture(e.pointerId); } catch (err) { /* synthetic pointer */ }
      var step = st.steps[cell.s], idx = -1;
      step.forEach(function (n, i) { if (n.p === cell.p) idx = i; });
      if (idx >= 0) {
        var wasSel = sel && sel.s === cell.s && sel.i === idx;
        sel = { s: cell.s, i: idx };
        gesture = { kind: "length", note: step[idx], s: cell.s, x0: pt.x, l0: step[idx].l, moved: false, wasSel: wasSel };
      } else if (step.length < MAX_LAYERS) {
        step.push({ p: cell.p, v: defVel, l: defLen });
        sel = { s: cell.s, i: step.length - 1 };
        previewNote(cell.p, defVel);
        gesture = { kind: "paint", p: cell.p };
      }
      changed();
      scroller.focus();
    });
    roll.addEventListener("pointermove", function (e) {
      if (!gesture) return;
      var pt = rollPoint(e);
      if (gesture.kind === "length") {
        var l = clamp(gesture.l0 + Math.round((pt.x - gesture.x0) / CW), 0, 127);
        if (Math.abs(pt.x - gesture.x0) > 4) gesture.moved = true;
        if (l !== gesture.note.l) { gesture.note.l = l; changed(); }
      } else {
        var s2 = Math.floor((pt.x - 34) / CW);
        if (s2 < 0 || s2 >= st.length) return;
        var step = st.steps[s2];
        if (step.length >= MAX_LAYERS || step.some(function (n) { return n.p === gesture.p; })) return;
        step.push({ p: gesture.p, v: defVel, l: defLen });
        changed();
      }
    });
    function endGesture() {
      if (gesture && gesture.kind === "length" && !gesture.moved && gesture.wasSel) {
        var at = st.steps[gesture.s].indexOf(gesture.note);
        if (at >= 0) st.steps[gesture.s].splice(at, 1);
        sel = null;
        changed();
      }
      gesture = null;
    }
    roll.addEventListener("pointerup", endGesture);
    roll.addEventListener("pointercancel", function () { gesture = null; });
    pointerDrag(lane, function (x, y) { laneSet(x, y); }, function (x, y) { laneSet(x, y); });
    function laneSet(x, y) {
      var s = Math.floor((x - 34) / CW);
      if (s < 0 || s >= st.length || !st.steps[s].length) return;
      var v = clamp(Math.round((1 - (y - 3) / (LANE - 6)) * 127), 1, 127);
      st.steps[s].forEach(function (n) { n.v = v; });
      changed();
    }
    scroller.tabIndex = 0;
    scroller.addEventListener("keydown", function (e) {
      var n = selNote(); if (!n) return;
      var d = { ArrowUp: [0, 1], ArrowDown: [0, -1], ArrowLeft: [-1, 0], ArrowRight: [1, 0] }[e.key];
      if (e.key === "Delete" || e.key === "Backspace") { st.steps[sel.s].splice(sel.i, 1); sel = null; changed(); e.preventDefault(); return; }
      if (!d) return;
      e.preventDefault();
      var np = clamp(n.p + d[1] * (e.shiftKey ? 12 : 1), 0, 127), ns = clamp(sel.s + d[0], 0, st.length - 1);
      if (ns === sel.s && np === n.p) return;
      if (st.steps[ns].some(function (o) { return o !== n && o.p === np; })) return;
      if (ns !== sel.s) {
        if (st.steps[ns].length >= MAX_LAYERS) return;
        st.steps[sel.s].splice(sel.i, 1); n.p = np; st.steps[ns].push(n); sel = { s: ns, i: st.steps[ns].length - 1 };
      } else n.p = np;
      changed();
    });
    function previewNote(p, v) {
      if (!R.web || st.mode !== 0) return;   /* a held key would start the arp or sequencer */
      R.web.noteOn(p, v);
      setTimeout(function () { R.web.noteOff(p); }, 220);
    }

    return {
      refresh: function () {
        var keys = ["seq_mode", "seq_arp_mode", "seq_arp_oct", "seq_div", "seq_gate", "seq_swing", "seq_length", "seq_data"];
        for (var li = 1; li <= AUTO_LANES; li++) keys.push("seq_lane" + li + "_dest", "seq_lane" + li + "_smooth", "seq_lane" + li + "_data");
        return Promise.all(keys.map(function (key) { return R.getParam(k(key)); })).then(function (v) {
          for (var li = 0; li < AUTO_LANES; li++) {
            var o = 8 + li * 3;
            st.lanes[li].dest = clamp(Math.round(+v[o] || 0), 0, DESTS.length - 1);
            st.lanes[li].smooth = v[o + 1] === null || v[o + 1] === undefined || v[o + 1] === "" ? 1 : (+v[o + 1] ? 1 : 0);
            parseLane(li, v[o + 2]);
          }
          st.mode = clamp(Math.round(+v[0] || 0), 0, 2); st.arp = clamp(Math.round(+v[1] || 0), 0, 7);
          st.oct = clamp(Math.round(+v[2] || 0), 0, 3); st.div = clamp(Math.round(+v[3] || 0), 0, 14);
          st.gate = isFinite(+v[4]) ? +v[4] : 0.5; st.swing = isFinite(+v[5]) ? +v[5] : 0;
          st.length = clamp(Math.round(+v[6] || 16), 1, 64);
          parse(v[7]); raw.value = String(v[7] || "");
          sel = null; fit(); draw();
        });
      },
      redraw: draw,
      setPlayhead: function (step) { if (step !== playStep) { playStep = step; drawRoll(); drawAuto(); } }
    };
  }

  /* -------------------------------------------------------------------- mount */

  function injectCss() {
    var s = make("style");
    s.textContent = [
      ".sfx-card{padding:12px 14px;display:grid;gap:10px}",
      ".sfx-head{display:flex;flex-wrap:wrap;gap:10px 14px;align-items:center}",
      ".sfx-head h2{margin:0;font-size:11px;letter-spacing:.14em;text-transform:uppercase}",
      ".sfx-note,.sfx-hint{color:var(--dim);font-size:11px;margin:0}",
      ".sfx-hint{color:var(--mute)}",
      ".sfx-seg{display:inline-flex;border:1px solid var(--line2);border-radius:8px;overflow:hidden}",
      ".sfx-seg button{border:0;background:transparent;color:var(--dim);padding:5px 12px;font-size:12px;font-weight:600}",
      ".sfx-seg button.on{background:var(--lime);color:#111}",
      ".sfx-row{display:flex;flex-wrap:wrap;gap:8px 14px;align-items:flex-end}",
      ".sfx-field{display:grid;gap:3px;font-size:10px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim)}",
      ".sfx-input{background:var(--bg);color:var(--ink);border:1px solid var(--line2);border-radius:7px;padding:5px 8px;font-size:12px}",
      ".sfx-num{width:64px}.sfx-raw{width:min(70vw,720px);font-family:ui-monospace,monospace;font-size:11px}",
      ".sfx-range{width:130px;accent-color:var(--lime)}",
      ".sfx-btn{background:var(--card2);color:var(--ink);border:1px solid var(--line2);border-radius:7px;padding:5px 10px;font-size:12px}",
      ".sfx-btn:hover{border-color:var(--lime);color:var(--lime)}",
      ".sfx-btn:disabled{opacity:.4;cursor:default;border-color:var(--line2);color:var(--ink)}",
      ".sfx-btn.on{background:var(--lime);color:#111}",
      ".sfx-ghost{display:inline-flex;gap:8px;align-items:center;color:var(--s-env)}",
      ".sfx-ghost[hidden]{display:none}",
      ".sfx-sel{font-size:12px;color:var(--lime);min-width:120px}",
      ".sfx-stage{border:1px solid var(--line);border-radius:10px;overflow:hidden}",
      ".sfx-canvas{display:block;touch-action:none}",
      ".sfx-scroll{overflow:auto;border:1px solid var(--line);border-radius:10px;max-height:520px;outline:none}",
      ".sfx-lane{border-top:1px solid var(--line)}",
      ".sfx-row select{width:auto;min-width:0}",
      "#pane-seq,#pane-lfo{padding:10px}"
    ].join("\n");
    document.head.appendChild(s);
  }

  window.SFExtras = {
    shaperSample: function (prefix, phase) {
      var i = prefix === "lfo2" ? 1 : 0;
      return shaperSample(shapers[i], phase);
    },
    mount: function (opts) {
      R = opts.remote; comp = opts.component || "synth";
      setPanel = opts.setParam || null; onShaper = opts.onShaperChange || null;
      injectCss();
      var seq = mountSeq(opts.seqHost);
      var lfo = mountLfo(opts.lfoHost);
      function refresh() {
        return Promise.all([seq.refresh(), lfo.refresh()]).then(function () { if (opts.onShaperChange) opts.onShaperChange(); });
      }
      R.onParamChange(function (changed) {
        var hit = false;
        Object.keys(changed || {}).forEach(function (key) {
          if (key === comp + ":preset" || key === comp + ":state") hit = true;
          if (/_shp_|:seq_/.test(key)) hit = true;
        });
        if (hit) setTimeout(refresh, 60);
      });
      refresh();
      return { refresh: refresh, redraw: function () { seq.redraw(); lfo.redraw(); }, chooseLfo: lfo.choose,
        shapeChanged: lfo.shapeChanged, setPlayhead: seq.setPlayhead };
    }
  };
}());
