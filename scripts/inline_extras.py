#!/usr/bin/env python3
"""Splice web_ui_extras.js into web_ui.html between the sfx markers (the Remote
UI is shipped as one file, so the extras are inlined rather than referenced)."""
import os, re, sys
root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
html_path = os.path.join(root, "web_ui.html")
html = orig = open(html_path, encoding="utf-8").read()
js = open(os.path.join(root, "web_ui_extras.js"), encoding="utf-8").read().replace("</script", "<\\/script")
block = "<!-- sfx:begin -->\n<script>\n" + js + "</script>\n<!-- sfx:end -->"
if "<!-- sfx:begin -->" in html:
    html = re.sub(r"<!-- sfx:begin -->.*?<!-- sfx:end -->", lambda m: block, html, flags=re.S)
else:
    tag = '<script src="./wavetable-preview.js"></script>'
    if tag not in html: sys.exit("wavetable-preview script tag not found")
    html = html.replace(tag, tag + "\n  " + block, 1)
# Unchanged content is not rewritten, so the file keeps its mtime.
if html != orig:
    open(html_path, "w", encoding="utf-8").write(html)
