/* Browser host for the Bizar WASM build.
 * Provides window.schwungRemote (what web_ui.html expects from Schwung), runs
 * the DSP in an AudioWorklet, and feeds it Web MIDI. Build-time placeholder
 * is the gzipped wasm, base64, in WASM_B64_GZ below. */
(function () {
  "use strict";

  var WORKLET_SRC = "(" + function () {
    class SF extends AudioWorkletProcessor {
      constructor(options) {
        super();
        this.ready = false;
        this.port.onmessage = (e) => { try { this.onMsg(e.data); } catch (err) { this.port.postMessage({ type: "error", id: e.data && e.data.id, message: String(err) }); } };
      }
      str(p) { var m = new Uint8Array(this.ex.memory.buffer), n = p; while (m[n]) n++; var t = ""; for (var i = p; i < n; i += 8192) t += String.fromCharCode.apply(null, m.subarray(i, Math.min(n, i + 8192))); return decodeURIComponent(escape(t)); }
      put(s) {
        var u = unescape(encodeURIComponent(s)), b = new Uint8Array(u.length + 1), p;
        for (var i = 0; i < u.length; i++) b[i] = u.charCodeAt(i);
        p = this.ex.malloc(b.length);
        new Uint8Array(this.ex.memory.buffer, p, b.length).set(b);
        return p;
      }
      get(key) {
        var k = this.put(key), n = this.cap;
        var r = this.f.get_param(this.inst, k, this.buf, n);
        this.ex.free(k);
        return r < 0 ? "" : this.str(this.buf);
      }
      set(key, val) {
        var k = this.put(key), v = this.put(val);
        this.f.set_param(this.inst, k, v);
        this.ex.free(k); this.ex.free(v);
      }
      onMsg(m) {
        if (m.type === "init") {
          var inst = new WebAssembly.Instance(new WebAssembly.Module(m.bytes), {});
          this.ex = inst.exports;
          this.ex._initialize();
          var api = this.ex.sf_web_api();
          var T = this.ex.__indirect_function_table;
          var dv = new DataView(this.ex.memory.buffer);
          var fn = (i) => T.get(dv.getUint32(api + 4 + 4 * i, true));
          this.f = { create: fn(0), destroy: fn(1), midi: fn(2), set_param: fn(3), get_param: fn(4), render: fn(6) };
          this.inst = this.f.create(0, 0);
          this.cap = 1 << 20;
          this.buf = this.ex.malloc(this.cap);
          this.out = this.ex.malloc(128 * 2 * 2);
          this.midiBuf = this.ex.malloc(4);
          this.notesKey = this.put("sounding_notes");
          this.notesBuf = this.ex.malloc(1024);
          this.notesLast = ""; this.blocks = 0;
          this.ready = true;
          this.port.postMessage({ type: "ready" });
        } else if (!this.ready) {
          return;
        } else if (m.type === "midi") {
          var mem = new Uint8Array(this.ex.memory.buffer, this.midiBuf, 4);
          mem.set(m.data.slice(0, 3));
          this.f.midi(this.inst, this.midiBuf, Math.min(m.data.length, 3), 0);
        } else if (m.type === "set") {
          this.set(m.key, m.value);
        } else if (m.type === "tempo") {
          this.ex.sf_web_set_tempo(m.bpm);
        } else if (m.type === "get") {
          this.port.postMessage({ type: "got", id: m.id, values: m.keys.map((k) => this.get(k)) });
        }
      }
      process(inputs, outputs) {
        var o = outputs[0];
        if (!this.ready) return true;
        var n = o[0].length;
        this.f.render(this.inst, this.out, n);
        var s = new Int16Array(this.ex.memory.buffer, this.out, n * 2);
        for (var i = 0; i < n; i++) { o[0][i] = s[2 * i] / 32768; o[1][i] = s[2 * i + 1] / 32768; }
        /* Sounding keys (played, received, sequenced or arpeggiated) for the on-screen keyboard. */
        if ((this.blocks++ & 7) === 0) {
          var r = this.f.get_param(this.inst, this.notesKey, this.notesBuf, 1024);
          var list = r < 0 ? "" : this.str(this.notesBuf);
          if (list !== this.notesLast) { this.notesLast = list; this.port.postMessage({ type: "notes", list: list }); }
        }
        return true;
      }
    }
    registerProcessor("sf-synth", SF);
  } + ")()";

  var WASM_B64_GZ = "__WASM_B64_GZ__";

  var ctx = null, node = null, ready = null, pending = {}, nextId = 1;
  var listeners = [], known = {}, knownKeys = [], written = {}, sideTimer = 0;
  var tempo = 120, startError = "", gain = null, analyser = null, volume = 0.8;

  function b64ToBytes(b64) {
    var bin = atob(b64), out = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  /* The audio graph needs a user gesture; parameter access waits on it so
   * the UI can load normally and the first click starts sound. */
  function start() {
    if (ready) return ready;
    ready = (async function () {
      /* Fail with a reason instead of hanging: AudioWorklet needs a secure
       * context (https, localhost or file://), and the wasm is gzipped. */
      if (!window.isSecureContext)
        throw new Error("audio needs https:// or localhost (this page is " + location.protocol + "//" + location.host + ")");
      if (!window.AudioWorkletNode) throw new Error("this browser has no AudioWorklet");
      if (!window.DecompressionStream) throw new Error("this browser has no DecompressionStream; update it");
      ctx = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
      var blob = new Blob([WORKLET_SRC], { type: "text/javascript" });
      var bytes = new Response(new Blob([b64ToBytes(WASM_B64_GZ)]).stream().pipeThrough(new DecompressionStream("gzip")));
      var wasm = await bytes.arrayBuffer();
      /* A file:// page has an opaque origin, where Chrome rejects blob: modules. */
      try { await ctx.audioWorklet.addModule(URL.createObjectURL(blob)); }
      catch (err) { await ctx.audioWorklet.addModule("data:text/javascript;base64," + btoa(unescape(encodeURIComponent(WORKLET_SRC)))); }
      node = new AudioWorkletNode(ctx, "sf-synth", { numberOfInputs: 0, outputChannelCount: [2] });
      gain = ctx.createGain();
      gain.gain.value = volume;
      analyser = ctx.createAnalyser();
      analyser.fftSize = 2048;
      node.connect(gain); gain.connect(analyser); analyser.connect(ctx.destination);
      await new Promise(function (resolve) {
        node.port.onmessage = function (e) {
          var p = pending[e.data.id];
          if (e.data.type === "ready") resolve();
          else if (e.data.type === "error") {
            console.error("Bizar worklet:", e.data.message);
            if (p) { p.reject(new Error(e.data.message)); delete pending[e.data.id]; }
          } else if (e.data.type === "notes") {
            sounding = {};
            if (e.data.list) e.data.list.split(",").forEach(function (n) { sounding[+n] = 1; });
            paintKeys();
          } else if (e.data.type === "got" && p) { p.resolve(e.data.values); delete pending[e.data.id]; }
        };
        node.port.postMessage({ type: "init", bytes: wasm }, [wasm]);
      });
      node.port.postMessage({ type: "tempo", bpm: tempo });
    })();
    ready.catch(function (err) {
      console.error("Bizar start failed:", err);
      startError = "Audio unavailable: " + (err && err.message || err);
      if (midiStatus) midiStatus.textContent = startError;
      if (startBtn) startBtn.textContent = "↻ Retry audio";
      ready = null;   /* let the button try again */
    });
    return ready;
  }

  function getMany(keys) {
    return start().then(function () {
      return new Promise(function (resolve, reject) {
        var id = nextId++;
        pending[id] = { resolve: resolve, reject: reject };
        node.port.postMessage({ type: "get", id: id, keys: keys });
      });
    });
  }

  function bare(k) { return k.slice(k.indexOf(":") + 1); }

  /* Preset and state loads change many parameters at once; re-read them all
   * and report the ones that moved, as Schwung's remote API does. */
  function refresh(skip) {
    if (!knownKeys.length) return;
    getMany(knownKeys).then(function (vals) {
      var changed = {};
      knownKeys.forEach(function (k, i) {
        var was = known[k];
        known[k] = vals[i];
        if (skip && skip[k]) return;
        if (was === vals[i] || (was !== undefined && was !== "" && vals[i] !== ""
            && isFinite(was) && isFinite(vals[i]) && Math.abs(was - vals[i]) < 1e-4)) return;
        changed["synth:" + k] = vals[i];
      });
      if (Object.keys(changed).length) listeners.forEach(function (fn) { fn(changed); });
    }).catch(function () { /* start failure is reported by start() */ });
  }

  /* Some parameters move others (the Shaper amplitudes are also the LFO
   * point knobs, cycle_depth aliases cycle_amount). After a burst of writes,
   * re-read everything and report only what the UI did not write itself, so
   * an echo never fights a control that is being dragged. */
  function sideEffects() {
    var skip = written;
    written = {};
    refresh(skip);
  }

  /* Single reads issued in the same tick (the UI asks for ~250 values at
   * startup, then polls the live keys) travel to the worklet as one batch. */
  var batch = null;
  function getOne(key) {
    return new Promise(function (resolve, reject) {
      if (!batch) {
        batch = [];
        Promise.resolve().then(function () {
          var jobs = batch;
          batch = null;
          getMany(jobs.map(function (j) { return j.key; })).then(function (vals) {
            jobs.forEach(function (j, i) { j.resolve(vals[i]); });
          }, function (err) { jobs.forEach(function (j) { j.reject(err); }); });
        });
      }
      batch.push({ key: key, resolve: resolve, reject: reject });
    });
  }

  window.schwungRemote = {
    component: "synth",
    getParam: function (k) {
      var key = bare(k);
      return getOne(key).then(function (v) { known[key] = v; return v; });
    },
    setParam: function (k, v) {
      var key = bare(k);
      known[key] = String(v);
      start().then(function () { node.port.postMessage({ type: "set", key: key, value: String(v) }); }, function () {});
      clearTimeout(sideTimer);
      if (key === "preset" || key === "state") { written = {}; sideTimer = setTimeout(refresh, 30); }
      else { written[key] = 1; sideTimer = setTimeout(sideEffects, 200); }
      return Promise.resolve();
    },
    audio: function () { return { ctx: ctx, node: node, gain: gain, analyser: analyser }; },
    onParamChange: function (fn) { listeners.push(fn); },
    getHierarchy: function () { return getMany(["ui_hierarchy"]).then(function (v) { return JSON.parse(v[0]); }); },
    getChainParams: function () {
      return getMany(["chain_params_full"]).then(function (v) {
        var list = JSON.parse(v[0]);
        knownKeys = list.map(function (p) { return p.key; }).concat(["preset"],
          ["seq_mode", "seq_arp_mode", "seq_arp_oct", "seq_div", "seq_gate", "seq_swing", "seq_length", "seq_data"]);
        [1, 2].forEach(function (n) {
          knownKeys.push("lfo" + n + "_shp_steps");
          for (var i = 1; i <= 16; i++) ["amp", "curve", "slope", "on"].forEach(function (f) { knownKeys.push("lfo" + n + "_shp_" + f + i); });
        });
        return list;
      });
    },
    /* Browser-only extras the Remote UI uses when present: playing notes,
     * panic, raw MIDI for MIDI learn, and a flag for browser-only features
     * such as autosave and share links. */
    web: {
      noteOn: function (note, vel) { press(note, vel == null ? velocity : vel); },
      noteOff: function (note) { release(note); },
      panic: panic,
      onMidi: function (fn) { midiListeners.push(fn); }
    }
  };

  function store(key, value) { try { localStorage.setItem(key, JSON.stringify(value)); } catch (err) { /* blocked */ } }
  function load(key, fallback) {
    try { var v = localStorage.getItem(key); return v == null ? fallback : JSON.parse(v); } catch (err) { return fallback; }
  }
  volume = load("sf-volume", 0.8);

  /* ---------- notes: computer keys, on-screen keys, Play, MIDI ---------- */
  function sendMidi(data) {
    if (node) node.port.postMessage({ type: "midi", data: Array.prototype.slice.call(data) });
  }
  /* Every local note goes through press/release, so Hold latches the keyboard,
   * the on-screen keys and the sequencer's Play button alike. */
  var sounding = {}, down = {}, latched = {}, hold = false, velocity = 100;
  function anyDown() { return Object.keys(down).some(function (n) { return down[n] > 0; }); }
  function press(note, vel) {
    if (note < 0 || note > 127) return;
    if (hold && !anyDown()) {
      Object.keys(latched).forEach(function (n) { if (!down[n]) sendMidi([0x80, +n, 0]); });
      latched = {};
    }
    down[note] = (down[note] || 0) + 1;
    if (down[note] === 1 && !latched[note]) sendMidi([0x90, note, Math.max(1, Math.min(127, vel | 0))]);
    if (hold) latched[note] = 1;
    paintKeys();
  }
  function release(note) {
    if (!down[note]) return;
    if (--down[note] > 0) return;
    delete down[note];
    if (!hold) sendMidi([0x80, note, 0]);
    paintKeys();
  }
  function setHold(on) {
    hold = on;
    if (!on) {
      Object.keys(latched).forEach(function (n) { if (!down[n]) sendMidi([0x80, +n, 0]); });
      latched = {};
    }
    if (holdBtn) holdBtn.setAttribute("aria-pressed", String(on));
    paintKeys();
  }
  function panic() {
    down = {}; latched = {}; held = {};
    for (var ch = 0; ch < 16; ch++) { sendMidi([0xb0 | ch, 123, 0]); sendMidi([0xb0 | ch, 120, 0]); }
    paintKeys();
  }

  /* ---------- the bar (top bar slot when the page has one) ---------- */
  var bar, midiStatus, startBtn, gate, gateErr, keysInfo, bpmIn, holdBtn, kbdPanel, kbdKeys = [], scopeCanvas, scopeMode = "wave", clipUntil = 0;
  var CSS = [
    "#sf-host{display:flex;flex-wrap:wrap;gap:6px 8px;align-items:center;justify-content:flex-end;font-size:11px;color:var(--dim,#aaa)}",
    "#sf-host.floating{position:fixed;right:10px;bottom:10px;z-index:40;padding:6px 10px;border-radius:10px;background:var(--card,#171915);border:1px solid var(--line2,#333)}",
    "#sf-host button,#sf-host select,#sf-host input{font:inherit;color:var(--ink,#eee);background:var(--card2,#1d201a);border:1px solid var(--line2,#333);border-radius:7px;padding:3px 8px}",
    "#sf-host button:hover{border-color:var(--lime,#d6ff38)}",
    "#sf-host button[aria-pressed=true]{background:var(--lime,#d6ff38);color:#111}",
    "#sf-host .go{background:var(--lime,#d6ff38);color:#111;font-weight:700;border-color:transparent}",
    "#sf-host .grp{display:inline-flex;gap:4px;align-items:center}",
    "#sf-host input[type=range]{padding:0;width:70px;accent-color:var(--lime,#d6ff38);border:0;background:none}",
    "#sf-host input[type=number]{width:54px}",
    "#sf-host canvas{display:block;width:96px;height:26px;border-radius:5px;background:var(--bg,#0e100d);border:1px solid var(--line,#262a22);cursor:pointer}",
    "#sf-host .st{max-width:260px;white-space:normal}",
    "#sf-gate{position:fixed;inset:0;z-index:80;display:grid;place-items:center;padding:20px;background:rgba(8,9,7,.82);backdrop-filter:blur(6px);-webkit-backdrop-filter:blur(6px)}",
    "#sf-gate .card{max-width:460px;display:flex;flex-direction:column;align-items:center;gap:12px;text-align:center;padding:34px 30px;border-radius:14px;background:var(--card,#171915);border:1px solid var(--line2,#333);box-shadow:0 30px 80px #000a;color:var(--ink,#eee)}",
    "#sf-gate .logo{font-size:30px;font-weight:700;letter-spacing:.2em;margin-bottom:4px}",
    "#sf-gate p{margin:0;color:var(--dim,#aaa);font-size:13px;line-height:1.5}",
    "#sf-gate .go{margin-top:8px;padding:11px 30px;border-radius:9px;border:1px solid transparent;background:var(--lime,#d6ff38);color:#111;font:inherit;font-weight:700;letter-spacing:.08em;cursor:pointer}",
    "#sf-gate .go:disabled{opacity:.6;cursor:default}",
    "#sf-gate .err{min-height:1em;color:#ff9a8a}",
    "#sf-midi{position:fixed;z-index:61;width:260px;padding:12px;display:grid;gap:8px;background:var(--card,#171915);border:1px solid var(--line2,#333);border-radius:12px;box-shadow:0 20px 60px #000c;font-size:12px;color:var(--ink,#eee)}",
    "#sf-midi label{display:grid;gap:3px;color:var(--dim,#aaa);font-size:10px;letter-spacing:.08em;text-transform:uppercase}",
    "#sf-midi label.row{display:flex;gap:6px;align-items:center;text-transform:none;letter-spacing:0;font-size:12px;color:var(--ink,#eee)}",
    "#sf-midi select,#sf-midi button{font:inherit;color:var(--ink,#eee);background:var(--card2,#1d201a);border:1px solid var(--line2,#333);border-radius:7px;padding:5px 8px}",
    "#sf-midi p{margin:0;color:var(--mute,#777);font-size:11px}",
    "#sf-kbd{position:fixed;left:0;right:0;bottom:0;z-index:45;display:grid;grid-template-rows:auto 1fr;gap:6px;padding:8px 10px 10px;background:var(--card,#171915);border-top:1px solid var(--line2,#333);box-shadow:0 -10px 30px #0008}",
    "#sf-kbd[hidden]{display:none}",
    "#sf-kbd .bar{display:flex;flex-wrap:wrap;gap:6px 10px;align-items:center;font-size:11px;color:var(--dim,#aaa)}",
    "#sf-kbd .bar button{font:inherit;color:var(--ink,#eee);background:var(--card2,#1d201a);border:1px solid var(--line2,#333);border-radius:7px;padding:4px 10px}",
    "#sf-kbd .bar button[aria-pressed=true]{background:var(--lime,#d6ff38);color:#111}",
    "#sf-kbd .bar input{accent-color:var(--lime,#d6ff38);width:110px}",
    "#sf-kbd .keys{position:relative;height:96px;touch-action:none;user-select:none;-webkit-user-select:none}",
    "#sf-kbd .k{position:absolute;top:0;border:1px solid #0008;border-radius:0 0 6px 6px}",
    "#sf-kbd .k.w{height:100%;background:#e9ebe3;z-index:1}",
    "#sf-kbd .k.b{height:60%;background:#22251d;z-index:2}",
    "#sf-kbd .k.on{background:var(--lime,#d6ff38)}",
    "#sf-kbd .k.w span{position:absolute;bottom:4px;left:0;right:0;text-align:center;font-size:9px;color:#555}",
    "body.sf-kbd-open{padding-bottom:150px}"
  ].join("\n");

  function el(tag, text, attrs) {
    var e = document.createElement(tag);
    if (text != null) e.textContent = text;
    Object.keys(attrs || {}).forEach(function (k) { e.setAttribute(k, attrs[k]); });
    return e;
  }

  function buildBar() {
    document.head.appendChild(el("style", CSS));
    bar = el("div", null, { id: "sf-host" });
    var slot = document.getElementById("host-slot");
    if (!slot) bar.className = "floating";

    startBtn = el("button", "Start Bizar", { type: "button", class: "go" });
    startBtn.addEventListener("click", function () { startBtn.disabled = true; startBtn.textContent = "Starting…"; wake(); });
    gateErr = el("p", null, { class: "err", role: "alert" });
    gate = el("div", null, { id: "sf-gate" });
    var card = el("div", null, { class: "card" });
    card.appendChild(el("span", "BIZAR", { class: "logo" }));
    card.appendChild(el("p", "A two-engine hybrid synthesizer for Ableton Move. This is the same engine, compiled to WebAssembly and running in your browser."));
    card.appendChild(el("p", "Play it with the Keys panel, your computer keyboard (A–K) or a MIDI keyboard."));
    card.appendChild(startBtn);
    card.appendChild(gateErr);
    gate.appendChild(card);
    document.body.appendChild(gate);
    midiStatus = el("span", "Audio not started", { class: "st" });

    scopeCanvas = el("canvas", null, { title: "Output: waveform and level (click for spectrum). Red = clipping." });
    scopeCanvas.addEventListener("click", function () { scopeMode = scopeMode === "wave" ? "spectrum" : "wave"; });

    var vol = el("input", null, { type: "range", min: "0", max: "1", step: "0.01", title: "Volume", "aria-label": "Volume" });
    vol.value = volume;
    vol.addEventListener("input", function () {
      volume = +vol.value; store("sf-volume", volume);
      if (gain) gain.gain.setTargetAtTime(volume, ctx.currentTime, 0.02);
    });

    var panicBtn = el("button", "Panic", { type: "button", title: "All notes and sounds off" });
    panicBtn.addEventListener("click", panic);

    var midiBtn = el("button", "MIDI ▾", { type: "button", "aria-haspopup": "dialog" });
    midiBtn.addEventListener("click", function () { toggleMidiPop(midiBtn); });

    bpmIn = el("input", null, { type: "number", min: "20", max: "300", title: "Tempo (BPM) for synced LFOs and sequencer", "aria-label": "Tempo" });
    bpmIn.value = tempo;
    bpmIn.disabled = !!midiPrefs.clock;
    bpmIn.addEventListener("change", function () { setTempo(+bpmIn.value || 120); });

    var kbdBtn = el("button", "⌨ Keys", { type: "button", "aria-pressed": "false", title: "On-screen keyboard" });
    kbdBtn.addEventListener("click", function () {
      var open = kbdPanel.hidden;
      kbdPanel.hidden = !open;
      document.body.classList.toggle("sf-kbd-open", open);
      kbdBtn.setAttribute("aria-pressed", String(open));
      if (open) layoutKeys();
      store("sf-kbd", open);
    });
    keysInfo = el("span", null, { title: "Computer keyboard: A–K white keys, W E T Y U O P black keys, Z / X octave, C / V velocity" });
    showOctave();

    if (startError) { setStatus(startError); gateErr.textContent = startError; startBtn.textContent = "↻ Retry audio"; }
    var g1 = el("span", null, { class: "grp" }); g1.appendChild(midiStatus);
    var g2 = el("span", null, { class: "grp" }); g2.appendChild(scopeCanvas); g2.appendChild(vol); g2.appendChild(panicBtn);
    var g3 = el("span", null, { class: "grp" }); g3.appendChild(midiBtn); g3.appendChild(document.createTextNode("BPM")); g3.appendChild(bpmIn);
    var g4 = el("span", null, { class: "grp" }); g4.appendChild(kbdBtn); g4.appendChild(keysInfo);
    [g1, g2, g3, g4].forEach(function (g) { bar.appendChild(g); });
    (slot || document.body).appendChild(bar);
    buildKeyboard();
    if (load("sf-kbd", false)) kbdBtn.click();
  }
  /* Any gesture that plays (Start, a key, the on-screen keyboard) starts audio. */
  function wake() {
    return start().then(function () {
      if (ctx.state !== "running") ctx.resume();
      if (gate && gate.isConnected) {
        gate.remove(); setStatus("MIDI: not connected"); initMidi(); requestAnimationFrame(drawScope);
      }
    }, function () {
      if (gate && gate.isConnected) {
        gateErr.textContent = startError;
        startBtn.disabled = false; startBtn.textContent = "↻ Retry audio";
      }
    });
  }
  function setStatus(text) { if (midiStatus) midiStatus.textContent = text; }
  function setTempo(bpm) {
    tempo = Math.max(20, Math.min(300, Math.round(bpm * 10) / 10));
    if (bpmIn && document.activeElement !== bpmIn) bpmIn.value = tempo;
    if (node) node.port.postMessage({ type: "tempo", bpm: tempo });
  }

  /* Output scope: waveform or spectrum, with a level bar and a clip light. */
  var scopeBuf = null;
  function drawScope() {
    requestAnimationFrame(drawScope);
    if (!analyser || document.hidden || !scopeCanvas) return;
    var c = scopeCanvas, r = window.devicePixelRatio || 1, w = 96, h = 26;
    if (c.width !== w * r) { c.width = w * r; c.height = h * r; }
    var g = c.getContext("2d");
    g.setTransform(r, 0, 0, r, 0, 0);
    g.clearRect(0, 0, w, h);
    if (!scopeBuf) scopeBuf = new Float32Array(analyser.fftSize);
    analyser.getFloatTimeDomainData(scopeBuf);
    var peak = 0;
    for (var i = 0; i < scopeBuf.length; i++) peak = Math.max(peak, Math.abs(scopeBuf[i]));
    if (peak >= 0.99) clipUntil = performance.now() + 1500;
    var lime = getComputedStyle(document.documentElement).getPropertyValue("--lime").trim() || "#d6ff38";
    g.strokeStyle = lime; g.lineWidth = 1.2; g.beginPath();
    if (scopeMode === "wave") {
      for (var x = 0; x < w - 6; x++) {
        var v = scopeBuf[Math.floor(x / (w - 6) * 512)] || 0;
        var y = h / 2 - v * (h / 2 - 2);
        if (x) g.lineTo(x, y); else g.moveTo(x, y);
      }
    } else {
      var bins = new Float32Array(analyser.frequencyBinCount);
      analyser.getFloatFrequencyData(bins);
      for (x = 0; x < w - 6; x++) {
        var bin = Math.min(bins.length - 1, Math.floor(Math.pow(bins.length, x / (w - 6))));
        var db = Math.max(-100, Math.min(0, bins[bin]));
        y = h - 2 - (db + 100) / 100 * (h - 4);
        if (x) g.lineTo(x, y); else g.moveTo(x, y);
      }
    }
    g.stroke();
    var level = Math.min(1, peak);
    g.fillStyle = performance.now() < clipUntil ? "#ff4d4d" : (level > 0.7 ? "#ffd23f" : lime);
    g.fillRect(w - 4, h - level * h, 4, level * h);
  }

  /* ---------- on-screen keyboard ---------- */
  var kbdBase = 48, kbdPointers = {};
  function buildKeyboard() {
    kbdPanel = el("div", null, { id: "sf-kbd", role: "group", "aria-label": "On-screen keyboard" });
    kbdPanel.hidden = true;
    var top = el("div", null, { class: "bar" });
    var down8 = el("button", "◀ Oct", { type: "button" }), up8 = el("button", "Oct ▶", { type: "button" });
    var octLabel = el("span");
    function showKbdOct() { octLabel.textContent = "C" + (kbdBase / 12 - 1); }
    down8.addEventListener("click", function () { kbdBase = Math.max(0, kbdBase - 12); showKbdOct(); layoutKeys(); });
    up8.addEventListener("click", function () { kbdBase = Math.min(96, kbdBase + 12); showKbdOct(); layoutKeys(); });
    var velIn = el("input", null, { type: "range", min: "1", max: "127", "aria-label": "Velocity" });
    velIn.value = velocity;
    velIn.addEventListener("input", function () { velocity = +velIn.value; showOctave(); });
    holdBtn = el("button", "Hold", { type: "button", "aria-pressed": "false", title: "Latch notes: they keep sounding after release (the arpeggiator and sequencer keep running)" });
    holdBtn.addEventListener("click", function () { setHold(!hold); });
    var panicBtn = el("button", "Panic", { type: "button" });
    panicBtn.addEventListener("click", panic);
    showKbdOct();
    [down8, octLabel, up8, document.createTextNode("Velocity"), velIn, holdBtn, panicBtn].forEach(function (n) { top.appendChild(n); });
    var keys = el("div", null, { class: "keys" });
    kbdPanel.appendChild(top); kbdPanel.appendChild(keys);
    document.body.appendChild(kbdPanel);

    function noteAt(x, y) {
      var hit = document.elementFromPoint(x, y);
      return hit && hit.dataset && hit.dataset.note != null ? +hit.dataset.note : (hit && hit.parentNode && hit.parentNode.dataset && hit.parentNode.dataset.note != null ? +hit.parentNode.dataset.note : null);
    }
    keys.addEventListener("pointerdown", function (e) {
      var n = noteAt(e.clientX, e.clientY);
      if (n == null) return;
      e.preventDefault();
      try { keys.setPointerCapture(e.pointerId); } catch (err) { /* synthetic */ }
      kbdPointers[e.pointerId] = n;
      wake();
      press(n, velIn.value);
    });
    keys.addEventListener("pointermove", function (e) {
      if (kbdPointers[e.pointerId] == null) return;
      var n = noteAt(e.clientX, e.clientY);
      if (n == null || n === kbdPointers[e.pointerId]) return;
      release(kbdPointers[e.pointerId]);
      kbdPointers[e.pointerId] = n;
      press(n, velIn.value);
    });
    function up(e) {
      if (kbdPointers[e.pointerId] == null) return;
      release(kbdPointers[e.pointerId]);
      delete kbdPointers[e.pointerId];
    }
    keys.addEventListener("pointerup", up);
    keys.addEventListener("pointercancel", up);
    window.addEventListener("resize", function () { if (!kbdPanel.hidden) layoutKeys(); });
  }
  function layoutKeys() {
    var keys = kbdPanel.querySelector(".keys");
    keys.textContent = "";
    kbdKeys = [];
    var width = keys.clientWidth || window.innerWidth - 20;
    var whites = Math.max(7, Math.min(36, Math.floor(width / 34)));
    var ww = width / whites, n = kbdBase, wi = 0;
    while (wi < whites && n <= 127) {
      var black = [1, 3, 6, 8, 10].indexOf(n % 12) >= 0;
      var k = el("div", null, { class: "k " + (black ? "b" : "w") });
      k.dataset.note = n;
      if (black) { k.style.left = (wi * ww - ww * 0.32) + "px"; k.style.width = (ww * 0.64) + "px"; }
      else {
        k.style.left = (wi * ww) + "px"; k.style.width = ww + "px";
        if (n % 12 === 0) k.appendChild(el("span", "C" + (n / 12 - 1)));
        wi++;
      }
      keys.appendChild(k);
      kbdKeys.push(k);
      n++;
    }
    paintKeys();
  }
  function paintKeys() {
    kbdKeys.forEach(function (k) { var n = +k.dataset.note; k.classList.toggle("on", !!(down[n] || latched[n] || sounding[n])); });
  }

  /* ---------- MIDI: input, channel, clock, learn ---------- */
  var midiAccess = null, midiListeners = [];
  var midiPrefs = load("sf-midi", { input: "all", channel: 0, clock: false });
  var clockTicks = [], lastClockSet = 0;
  function initMidi() {
    if (!navigator.requestMIDIAccess) {
      setStatus("WebMIDI unsupported — use Chrome/Edge; play with A–K or ⌨ Keys");
      return;
    }
    navigator.requestMIDIAccess({ sysex: false }).then(function (access) {
      midiAccess = access;
      access.onstatechange = wire;
      wire();
    }, function () { setStatus("MIDI: access denied (keys still play)"); });
  }
  function wire() {
    var names = [];
    midiAccess.inputs.forEach(function (inp) {
      var use = midiPrefs.input === "all" || midiPrefs.input === inp.id;
      inp.onmidimessage = use ? onMidiMessage : null;
      if (use) names.push(inp.name);
    });
    setStatus(names.length ? "MIDI: " + names.join(", ") : "MIDI: no input — plug one in");
    if (midiPop) fillMidiPop();
  }
  function onMidiMessage(e) {
    var d = e.data, status = d[0];
    if (status === 0xf8) { clock(e.timeStamp); return; }
    if (status >= 0xf0) return;
    if (midiPrefs.channel && (status & 0x0f) !== midiPrefs.channel - 1) return;
    for (var i = 0; i < midiListeners.length; i++) if (midiListeners[i](d)) return;   /* learned CC */
    sendMidi(d);
  }
  /* 24 clocks per quarter note; average the last bar of ticks. */
  function clock(t) {
    if (!midiPrefs.clock) return;
    clockTicks.push(t);
    if (clockTicks.length > 96) clockTicks.shift();
    if (clockTicks.length < 24) return;
    var span = (clockTicks[clockTicks.length - 1] - clockTicks[0]) / (clockTicks.length - 1);
    if (span <= 0) return;
    var bpm = 60000 / (span * 24);
    if (t - lastClockSet > 250 && Math.abs(bpm - tempo) > 0.3) { lastClockSet = t; setTempo(bpm); }
  }

  var midiPop = null;
  function toggleMidiPop(anchor) {
    if (midiPop) { closeMidiPop(); return; }
    midiPop = el("div", null, { id: "sf-midi", role: "dialog", "aria-label": "MIDI settings" });
    document.body.appendChild(midiPop);
    fillMidiPop();
    var r = anchor.getBoundingClientRect();
    midiPop.style.top = Math.min(innerHeight - midiPop.offsetHeight - 8, r.bottom + 6) + "px";
    midiPop.style.left = Math.max(8, Math.min(innerWidth - 268, r.right - 260)) + "px";
    setTimeout(function () { document.addEventListener("pointerdown", outside, true); }, 0);
  }
  function outside(e) { if (midiPop && !midiPop.contains(e.target)) closeMidiPop(); }
  function closeMidiPop() {
    if (midiPop) midiPop.remove();
    midiPop = null;
    document.removeEventListener("pointerdown", outside, true);
  }
  function fillMidiPop() {
    midiPop.textContent = "";
    var inSel = el("select");
    inSel.appendChild(new Option("All inputs", "all"));
    if (midiAccess) midiAccess.inputs.forEach(function (inp) { inSel.appendChild(new Option(inp.name, inp.id)); });
    inSel.value = midiPrefs.input;
    if (inSel.value !== midiPrefs.input) inSel.value = "all";
    inSel.addEventListener("change", function () { midiPrefs.input = inSel.value; store("sf-midi", midiPrefs); if (midiAccess) wire(); });
    var chSel = el("select");
    chSel.appendChild(new Option("Omni (all channels)", "0"));
    for (var c = 1; c <= 16; c++) chSel.appendChild(new Option("Channel " + c, String(c)));
    chSel.value = String(midiPrefs.channel);
    chSel.addEventListener("change", function () { midiPrefs.channel = +chSel.value; store("sf-midi", midiPrefs); });
    var clk = el("input", null, { type: "checkbox" });
    clk.checked = !!midiPrefs.clock;
    clk.addEventListener("change", function () { midiPrefs.clock = clk.checked; clockTicks = []; store("sf-midi", midiPrefs); bpmIn.disabled = clk.checked; });
    var l1 = el("label", "Input"); l1.appendChild(inSel);
    var l2 = el("label", "Channel"); l2.appendChild(chSel);
    var l3 = el("label", null, { class: "row" }); l3.appendChild(clk); l3.appendChild(document.createTextNode("Follow MIDI clock for BPM"));
    midiPop.appendChild(l1); midiPop.appendChild(l2); midiPop.appendChild(l3);
    if (!midiAccess) midiPop.appendChild(el("p", "Start audio to see MIDI devices."));
    midiPop.appendChild(el("p", "MIDI learn: Alt-click (or press L on) a knob, then move a controller. Alt-click again to unlearn."));
    if (window.SFMidiLearn) {
      var clear = el("button", "Clear learned controls (" + window.SFMidiLearn.count() + ")", { type: "button" });
      clear.addEventListener("click", function () { window.SFMidiLearn.clear(); fillMidiPop(); });
      midiPop.appendChild(clear);
    }
  }

  /* ---------- computer keyboard ---------- */
  /* Live's layout: A-K are the white keys, W E T Y U O P the black ones,
   * Z / X shift the octave and C / V the velocity. Digits and M are left
   * alone; the Remote UI uses them for mod assignment. */
  var KEYS = "awsedftgyhujkolp", held = {}, octave = 60;
  function showOctave() {
    if (keysInfo) keysInfo.textContent = "Keys C" + (octave / 12 - 1) + " · vel " + velocity;
  }
  function releaseKeys() {
    Object.keys(held).forEach(function (code) { release(held[code]); });
    held = {};
  }
  document.addEventListener("keydown", function (e) {
    if (e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
    if (/^(INPUT|SELECT|TEXTAREA)$/.test((e.target || {}).tagName || "") || (e.target && e.target.isContentEditable)) return;
    var key = e.key.toLowerCase();
    if (key === "z" || key === "x") {
      octave = Math.max(12, Math.min(108, octave + (key === "z" ? -12 : 12)));
      showOctave();
      return;
    }
    if (key === "c" || key === "v") {
      velocity = Math.max(1, Math.min(127, velocity + (key === "c" ? -20 : 20)));
      showOctave();
      return;
    }
    var i = KEYS.indexOf(key);
    if (i < 0 || held[e.code] != null) return;
    if (!node || ctx.state !== "running") wake();
    held[e.code] = octave + i;
    press(octave + i, velocity);
  });
  document.addEventListener("keyup", function (e) {
    if (held[e.code] == null) return;
    release(held[e.code]); delete held[e.code];
  });
  /* A key released while the page is in the background never sends keyup. */
  window.addEventListener("blur", releaseKeys);
  document.addEventListener("visibilitychange", function () { if (document.hidden) releaseKeys(); });

  document.addEventListener("DOMContentLoaded", buildBar);
}());
