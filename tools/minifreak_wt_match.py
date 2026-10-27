#!/usr/bin/env python3
"""Capture MiniFreak V wavetable harmonic profiles and rank CC0 banks against them.

Headless table selection is NOT possible (the plugin ignores patched state), so the
editor is opened and YOU click through the tables in its Wavetable browser (Osc 1 Type
= Wavetable). The script watches the plugin state, and whenever the selected table
changes it renders a 16-position sweep and stores harmonic profiles.

  capture:  tools/minifreak_wt_match.py capture OUT.npz CC0_DIR   (needs pedalboard, mido, numpy)
  match:    tools/minifreak_wt_match.py match OUT.npz CC0_DIR     (needs numpy)

CC0_DIR holds WaveEdit Online *.WAV banks (64 frames x 256 samples).
"""
import glob, os, re, sys, threading, time, wave
import numpy as np

ALPH = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"
PLUG = "/Library/Audio/Plug-Ins/VST3/MiniFreak V.vst3"
FLOOR = -45.0
EXCL = re.compile(r"^(PPG|ESQ|ENSONIQ|ENSHTU|KAWAI|PROPHE|FAIRLI|JUNOX|BRAIDS|PLAITS|MS2K|SQ8|NOMAD|RADIUS|"
                  r"ACCESS_V|VIRUS_SA|G2_ASTRA|KYMA|LSDJ|TRON_MAL|MK_DWG|TEZZALOG|SYNLP)", re.I)


def jdec(s):
    n, _, data = s.partition(".")
    bits = acc = 0
    out = bytearray()
    for ch in data:
        acc |= ALPH.index(ch) << bits
        bits += 6
        while bits >= 8:
            out.append(acc & 255)
            acc >>= 8
            bits -= 8
    return bytes(out[:int(n)])


def current_table(plugin):
    comp = re.search(rb"<IComponent>(.*?)</IComponent>", plugin.raw_state, re.S).group(1)
    hit = re.search(rb"Factory/WT/Factory/([^\0]*)\.raw", jdec(comp.decode().strip()))
    return hit.group(1).decode() if hit else None


def render_sweep(plugin, positions):
    import pedalboard  # noqa: F401
    from mido import Message
    rows = []
    for w in positions:
        plugin.osc1_wave = float(w)
        msgs = [Message("note_on", note=57, velocity=100, time=0.05), Message("note_off", note=57, velocity=0, time=0.9)]
        a = plugin.process(msgs, duration=1.3, sample_rate=44100, num_channels=2, buffer_size=512, reset=False)
        x = a[0, int(0.3 * 44100):int(0.8 * 44100)] * np.hanning(22050)
        spec = np.abs(np.fft.rfft(x, n=len(x) * 4))
        df = 44100 / (len(x) * 4)
        rows.append([spec[int(round(k * 220 / df)) - 12:int(round(k * 220 / df)) + 13].max() for k in range(1, 41)])
    return np.array(rows)


def capture(out, expected):
    import pedalboard
    p = pedalboard.load_plugin(PLUG)
    p.osc1_type = "Wavetable"
    for k, v in dict(osc2_volume=0.0, cutoff=1.0, reso=0.0, env_amt=0.0, attack=0.0, sustain=1.0, release=0.0,
                     vca=1.0, glide=0.0, uni_spread=0.0, osc1_volume=1.0, osc1_timbre=0.0, osc1_shape=0.5).items():
        setattr(p, k, v)
    done, stop, seen = {}, threading.Event(), None

    def watch():
        nonlocal seen
        idle = 0
        while not stop.is_set():
            time.sleep(0.7)
            name = current_table(p)
            if name and name != seen:
                seen = name
                if name not in done:
                    done[name] = render_sweep(p, np.linspace(0, 1, 16))
                    print(f"captured {name} ({len(done)})", flush=True)
                    np.savez(out, **done)
    th = threading.Thread(target=watch, daemon=True)
    th.start()
    print("Editor open: pick Osc 1 Type = Wavetable, then step through every table. Close the window when done.")
    p.show_editor(stop)
    stop.set()
    th.join()


def to_db(h):
    h = h / (np.sqrt((h ** 2).sum(-1, keepdims=True)) + 1e-9)
    return np.maximum(20 * np.log10(h + 1e-9), FLOOR)


def sim(a, b):
    return 1 - np.abs(a - b).mean() / -FLOOR


def match(npz, cc0_dir, top=4, extra=16):
    mf = {k: to_db(v) for k, v in np.load(npz).items()}
    cc = {}
    for f in sorted(glob.glob(os.path.join(cc0_dir, "*.WAV"))):
        n = os.path.basename(f)[:-4]
        if EXCL.match(n):
            continue
        with wave.open(f) as w:
            d = np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(float).reshape(64, 256)
        cc[n] = to_db(np.abs(np.fft.rfft(d - d.mean(1, keepdims=True), axis=1))[:, 1:41])
    pos = np.linspace(0, 63, 16)
    i0 = np.floor(pos).astype(int)
    i1 = np.minimum(i0 + 1, 63)
    fr = (pos - i0)[:, None]
    score = {}
    for t, m in mf.items():
        score[t] = {}
        for n, c in cc.items():
            ci = c[i0] * (1 - fr) + c[i1] * fr
            ordered = max(sim(m, ci), sim(m, ci[::-1]))
            x = 1 - np.abs(m[:, None, :] - c[None, :, :]).mean(2) / -FLOOR
            score[t][n] = 0.5 * ordered + 0.25 * (x.max(1).mean() + x.max(0).mean())
    for t in sorted(score):
        best = sorted(score[t].items(), key=lambda kv: -kv[1])[:top]
        print(f"{t:14s}", " ".join(f"{n}({s:.2f})" for n, s in best))
    sel = []
    cover = lambda s: np.mean([max(score[t][n] for n in s) for t in score]) if s else 0
    for _ in range(extra):
        sel.append(max((n for n in cc if n not in sel), key=lambda n: cover(sel + [n])))
        print(f"greedy {len(sel):2d}: {sel[-1]:10s} coverage {cover(sel):.3f}")


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("capture", "match"):
        raise SystemExit(__doc__)
    (capture if sys.argv[1] == "capture" else match)(sys.argv[2], sys.argv[3])
