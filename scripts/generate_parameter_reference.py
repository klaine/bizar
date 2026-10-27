#!/usr/bin/env python3
"""Generate PARAMETER_REFERENCE.md from the module's own UI contract.

The module serves `ui_hierarchy` and `chain_params` from get_param. `build.sh
--reference` compiles tools/dump_contract.c beside the DSP, runs it, and calls
this script with the two documents plus module.json. Nothing here is a second
copy of the parameter list: a key, range, or page that is not in the contract
cannot appear in the document.

Usage:
    generate_parameter_reference.py HIERARCHY_JSON CHAIN_PARAMS_JSON \
        MODULE_JSON OUTPUT_MD
"""

import json
import sys


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def cell(text):
    return str(text).replace("|", "\\|").replace("\n", " ").strip()


def range_text(meta):
    if meta.get("type") == "enum":
        options = meta.get("options") or []
        return "Enum: " + ", ".join(cell(option) for option in options)
    pieces = []
    low = meta.get("min")
    high = meta.get("max")
    if low is not None and high is not None:
        pieces.append(f"{low} ... {high}")
    elif low is not None:
        pieces.append(f">= {low}")
    elif high is not None:
        pieces.append(f"<= {high}")
    if meta.get("unit"):
        pieces.append(str(meta["unit"]))
    if meta.get("step"):
        pieces.append(f"step {meta['step']}")
    return " ".join(pieces) if pieces else "-"


def condition_text(condition, names):
    if not condition:
        return ""
    key = condition.get("param", "?")
    name = names.get(key, key)
    if "equals" in condition:
        return f"shown when {name} = {condition['equals']}"
    if "not_equals" in condition:
        return f"shown when {name} != {condition['not_equals']}"
    for field in ("gt", "greater_than", "greater"):
        if field in condition:
            return f"shown when {name} > {condition[field]}"
    for field in ("lt", "smaller_than", "smaller"):
        if field in condition:
            return f"shown when {name} < {condition[field]}"
    if condition.get("truthy"):
        return f"shown when {name} is on"
    if condition.get("falsey") or condition.get("falsy"):
        return f"shown when {name} is off"
    return f"shown when {key} matches its condition"


def entry_map(level):
    entries = {}
    for entry in level.get("params", []):
        if isinstance(entry, str):
            entries.setdefault(entry, {})
        elif isinstance(entry, dict) and "key" in entry:
            entries[entry["key"]] = entry
    return entries


def level_label(key, level):
    """A stable title. The modcol labels are formatted with the live matrix
    destination at runtime, so a generated document must not capture whichever
    preset happened to be loaded."""
    if key.startswith("modcol"):
        return "Mod {}".format(key[len("modcol"):])
    return level.get("label") or key


def page_table(level, params, names, defaults):
    keys = level.get("knobs")
    if not keys:
        return []
    entries = entry_map(level)
    lines = [
        "| Knob | Key | Name | Range | Default | Notes |",
        "|---|---|---|---|---|---|",
    ]
    for index, key in enumerate(keys, start=1):
        meta = params.get(key)
        entry = entries.get(key, {})
        if meta is None:
            continue
        name = meta.get("name", key)
        default = defaults.get(key, "-")
        notes = []
        if entry.get("visible_if"):
            notes.append(condition_text(entry["visible_if"], names))
        if meta.get("access") == "read":
            notes.append("read-only")
        if meta.get("access") == "write":
            notes.append("trigger")
        lines.append(
            "| {} | `{}` | {} | {} | {} | {} |".format(
                index,
                key,
                cell(name),
                cell(range_text(meta)),
                cell(default),
                cell("; ".join(part for part in notes if part)),
            )
        )
    return lines


def main(argv):
    if len(argv) != 5:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    hierarchy = load(argv[1])
    chain_params = load(argv[2])
    module = load(argv[3])
    output_path = argv[4]

    levels = hierarchy.get("levels", {})
    params = {entry["key"]: entry for entry in chain_params}
    names = {key: meta.get("name", key) for key, meta in params.items()}
    defaults = module.get("defaults", {})
    version = module.get("version", "unknown")

    lines = [
        "# Bizar parameter reference",
        "",
        f"Version {version}. Generated from the module's own `ui_hierarchy` and "
        "`chain_params` by `scripts/generate_parameter_reference.py`; do not edit "
        "by hand.",
        "",
        "The Move grid shows up to eight knobs per page. A row marked `shown when` "
        "is hidden until the named control reaches that value, so an effect slot "
        "shows its EQ controls only while it is running that effect.",
        "",
    ]

    root = levels.get("root", {})
    lines.append("## Pages")
    lines.append("")
    lines.append("### Main")
    lines.append("")
    lines.extend(page_table(root, params, names, defaults))
    lines.append("")

    emitted = set()
    for link in root.get("params", []):
        if not isinstance(link, dict) or "level" not in link:
            continue
        key = link["level"]
        level = levels.get(key)
        if level is None:
            continue
        emitted.add(key)
        label = link.get("label") or level_label(key, level)
        lines.append(f"### {label}")
        lines.append("")
        if level.get("visible_if"):
            lines.append(
                "Gate: {}.".format(condition_text(level["visible_if"], names))
            )
            lines.append("")
        if level.get("items_param"):
            lines.append(
                "Selector page: lists the items in `{}`, selects with `{}`, and "
                "navigates to `{}`.".format(
                    level.get("items_param"),
                    level.get("select_param", "-"),
                    level.get("navigate_to", "-"),
                )
            )
            lines.append("")
            continue
        if level.get("list_param"):
            lines.append(
                "List page: browses `{}` names from `{}`, with `{}` entries.".format(
                    level.get("list_param"),
                    level.get("name_param", "-"),
                    level.get("count_param", "-"),
                )
            )
            lines.append("")
            continue
        table = page_table(level, params, names, defaults)
        if table:
            lines.extend(table)
        else:
            lines.append("No knobs.")
        lines.append("")

    leftovers = [key for key in levels if key not in emitted and key != "root"]
    if leftovers:
        lines.append("## Additional levels")
        lines.append("")
        lines.append(
            "These levels stay in the document for the Remote UI and for state "
            "compatibility, but the Move walk does not visit them."
        )
        lines.append("")
        for key in leftovers:
            level = levels[key]
            label = level_label(key, level)
            lines.append(f"### {label}")
            lines.append("")
            if level.get("visible_if"):
                lines.append(
                    "Gate: {}.".format(
                        condition_text(level["visible_if"], names)
                    )
                )
                lines.append("")
            table = page_table(level, params, names, defaults)
            lines.extend(table if table else ["No knobs."])
            lines.append("")

    covered = set()
    for level in levels.values():
        covered.update(level.get("knobs") or [])
        if level.get("items_param"):
            covered.add(level["items_param"])
        if level.get("list_param"):
            covered.add(level["list_param"])
            if level.get("name_param"):
                covered.add(level["name_param"])

    remaining = [key for key in params if key not in covered]
    if remaining:
        lines.append("## Other parameters")
        lines.append("")
        lines.append(
            "Served by the module and used by presets or the Remote UI, but not "
            "on a knob page."
        )
        lines.append("")
        lines.append("| Key | Name | Range | Notes |")
        lines.append("|---|---|---|---|")
        for key in remaining:
            meta = params[key]
            notes = []
            if meta.get("access") == "read":
                notes.append("read-only")
            if meta.get("access") == "write":
                notes.append("trigger")
            lines.append(
                "| `{}` | {} | {} | {} |".format(
                    key,
                    cell(meta.get("name", key)),
                    cell(range_text(meta)),
                    cell("; ".join(notes)),
                )
            )
        lines.append("")

    with open(output_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines).rstrip() + "\n")

    print(f"Wrote {output_path} ({len(params)} parameters, {len(levels)} levels)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
