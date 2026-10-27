#!/usr/bin/env python3
"""Assemble dist/bizar-web.html: one self-contained file (UI, host, wasm).

Usage: python3 web/build.py [--no-compile]
Needs zig for the wasm step (zig cc, wasm32-wasi) and the generated headers in
build/generated (scripts/build.sh creates them).
"""
import base64, gzip, os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GEN = os.path.join(ROOT, "build", "generated")
WASM = os.path.join(ROOT, "build", "bizar.wasm")
OUT = os.path.join(ROOT, "dist", "bizar-web.html")
# A build that imported a MiniFreak bank is for personal use only, so it never
# takes the release file name.
PERSONAL_OUT = os.path.join(ROOT, "dist", "bizar-web-personal.html")


def has_minifreak_bank():
    path = os.path.join(GEN, "minifreak_bank.h")
    return os.path.exists(path) and "#define SF_MF_PRESET_COUNT 0\n" not in read(path)

def compile_wasm():
    if not os.path.exists(os.path.join(GEN, "wavetable_bank.h")):
        sys.exit("missing build/generated headers; run scripts/build.sh once first")
    src = [os.path.join(ROOT, "src", f) for f in
           ("bizar.c", "engines.c", "effects.c", "filter.c", "modulation.c", "voice.c", "sequencer.c")]
    subprocess.check_call(
        [os.environ.get("ZIG", "zig"), "cc", "-target", "wasm32-wasi", "-mexec-model=reactor", "-O3", "-std=c11", "-D_GNU_SOURCE",
         "-ffile-prefix-map=" + ROOT + "=.",  # with --strip-all: no absolute build paths (user name) in the output
         "-I" + os.path.join(ROOT, "web", "shim"), "-I" + os.path.join(ROOT, "include"), "-I" + GEN,
         *src, os.path.join(ROOT, "web", "glue.c"),
         "-Wl,--export-table", "-Wl,--export=malloc", "-Wl,--export=free", "-Wl,--strip-all", "-lm", "-o", WASM])

def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()

def main():
    if "--no-compile" not in sys.argv:
        compile_wasm()
    wasm = open(WASM, "rb").read()
    b64 = base64.b64encode(gzip.compress(wasm, 9)).decode()
    host = read(os.path.join(ROOT, "web", "host.js")).replace("__WASM_B64_GZ__", b64)
    preview = read(os.path.join(GEN, "wavetable-preview.js"))
    subprocess.check_call([sys.executable, os.path.join(ROOT, "scripts", "inline_extras.py")])
    html = read(os.path.join(ROOT, "web_ui.html"))
    a = '<script src="/static/schwung-remote-api.js"></script>'
    b = '<script src="./wavetable-preview.js"></script>'
    if a not in html or b not in html:
        sys.exit("web_ui.html script tags changed; update web/build.py")
    esc = lambda s: s.replace("</script", "<\\/script")
    html = html.replace(a, "<script>" + esc(host) + "</script>")
    html = html.replace(b, "<script>" + esc(preview) + "</script>")
    out = PERSONAL_OUT if has_minifreak_bank() else OUT
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    print("Built %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))

main()
