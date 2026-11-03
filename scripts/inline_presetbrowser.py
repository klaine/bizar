#!/usr/bin/env python3
"""Splice the shared patch browser into web_ui.html (the Remote UI is shipped as
one file, so it is inlined, like web_ui_extras.js).

presetbrowser/ holds byte-identical copies of Amino's src/web/presetbrowser.js,
presetdrawer.js and presetbrowser.css, which are ES modules. Here they become one
classic script that sets window.PresetBrowser, between the pb:js markers; the
sheet goes between the pb:css markers; and the factory bank's categories (from
scripts/generate_factory_bank.py, the order of Move's Patch Category page) go
between the pb:library markers."""
import json, os, re, sys
root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(root, "scripts"))
import generate_factory_bank as bank  # noqa: E402

src = os.path.join(root, "presetbrowser")
html_path = os.path.join(root, "web_ui.html")
html = orig = open(html_path, encoding="utf-8").read()


def read(name):
    return open(os.path.join(src, name), encoding="utf-8").read()


def sub(text, old, new, name):
    """Replace exactly one occurrence, so a changed upstream file fails here, not in the page."""
    if text.count(old) != 1:
        sys.exit("%s: expected one %r; update scripts/inline_presetbrowser.py" % (name, old))
    return text.replace(old, new)


browser = read("presetbrowser.js")
browser = sub(browser, "import.meta.url", "document.baseURI", "presetbrowser.js")  # the sheet is inlined: never fetched
browser = sub(browser, 'import("./presetdrawer.js")', "Promise.resolve({ presetDrawer })", "presetbrowser.js")
drawer = read("presetdrawer.js")
drawer = sub(drawer, 'import { el } from "./presetbrowser.js";\n', "", "presetdrawer.js")
names = re.findall(r"^export (?:function|class|const) (\w+)", browser + drawer, flags=re.M)
code = re.sub(r"^export ", "", browser + drawer, flags=re.M)
if re.search(r"\bimport\b|\bexport\b", code.replace("document.baseURI", "")):
    sys.exit("presetbrowser: an import or export is left; update scripts/inline_presetbrowser.py")
js = ("(function () {\n" + code + "\nwindow.PresetBrowser = { " + ", ".join(names) + " };\n}());\n").replace("</script", "<\\/script")
css = read("presetbrowser.css")
library = {"categories": [[tag, bank.BROWSE_LABELS[tag]] for tag, _ in bank.CATEGORIES], "factory": len(bank.BANK)}

for marker, body in (("pb:js", js), ("pb:css", css), ("pb:library", json.dumps(library))):
    pattern = r"(/\*%s\*/).*?(/\*%s-end\*/)" % (re.escape(marker), re.escape(marker))
    if not re.search(pattern, html, flags=re.S):
        sys.exit("web_ui.html: the %s markers are missing" % marker)
    html = re.sub(pattern, lambda m: m.group(1) + ("\n" + body if marker != "pb:library" else body) + m.group(2), html, count=1, flags=re.S)
# Unchanged content is not rewritten, so the file keeps its mtime.
if html != orig:
    open(html_path, "w", encoding="utf-8").write(html)
