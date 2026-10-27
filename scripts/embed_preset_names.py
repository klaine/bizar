#!/usr/bin/env python3
"""Embed the factory preset names in the built web_ui.html.

Move's Remote UI only forwards preset, preset_count and preset_name to a module
page, so the page cannot ask the module for the whole list. The names are read
from the generated banks (the same order the module uses: Bizar's own bank, then
any imported MiniFreak bank) and spliced in at the sf:preset-names marker.
Usage: embed_preset_names.py <generated dir> <web_ui.html>"""
import json, os, re, sys

gen, html_path = sys.argv[1], sys.argv[2]


def names(header, array):
    path = os.path.join(gen, header)
    if not os.path.exists(path):
        return []
    text = open(path, encoding="utf-8").read()
    m = re.search(array + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        return []
    return [json.loads(q) for q in re.findall(r'"(?:[^"\\]|\\.)*"', m.group(1))]


listing = names("factory_bank.h", "g_factory_names") + names("minifreak_bank.h", "sf_mf_preset_names")
html = open(html_path, encoding="utf-8").read()
marker = "null; /*sf:preset-names*/"
if marker not in html:
    sys.exit("sf:preset-names marker not found in " + html_path)
html = html.replace(marker, json.dumps(listing, ensure_ascii=False).replace("</", "<\\/") + "; /*sf:preset-names*/", 1)
open(html_path, "w", encoding="utf-8").write(html)
print("embedded %d preset names" % len(listing))
