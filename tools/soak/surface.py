#!/usr/bin/env python3
"""The surface inventory and its coverage (safety-plan W1.7).

    tools/soak/surface.py [--binary PATH] [--coverage FILE] [--suite-coverage FILE]
                          [--out DIR] [--extra tools/soak/surface-extra.toml]

Reads the app's own inventory (the `surface` control query: commands,
keybindings, settings, menu and context-menu rows, sidebar views, overlay modes,
panel contents, tab kinds, focus targets, query verbs) from a headless launch,
and compares it with what was reached:

  * the soak runner's coverage file (`build/soak/coverage.json`: commands run,
    elements pressed, setting values set, and the state signatures seen, which
    name overlay modes, sidebar views, panel contents, tab kinds and focus);
  * optionally the QA suite's `command-coverage.txt` (commands a checking flow
    drove).

Writes `<out>/surface.json` (the inventory, kept per commit so the next run
lists what is NEW) and `<out>/surface-gaps.md`: every item nothing reached, by
category, new items first. The hand-kept `surface-extra.toml` lists what cannot
be enumerated (outside events, plugin APIs, remote operations) and what the
monkey cannot drive from inside, each with an owner or the exclusion's reason.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "tools" / "qa"))
import microide_qa as q  # noqa: E402


def read_surface(binary: Path) -> dict:
    scratch = Path(tempfile.mkdtemp(prefix="surface-", dir=REPO / "build"))
    project = q.make_fixture(scratch / "taskflow")
    with q.App.launch(project=project, binary=binary) as app:
        return app.query("surface")


def flatten(surface: dict) -> dict[str, set[str]]:
    """Category -> item names, the form coverage is compared in."""
    out: dict[str, set[str]] = {
        "commands": set(surface.get("commands", [])),
        "keybindings": {f"{b['id']} ({b['chord']})" for b in surface.get("keybindings", [])},
        "settings": set(surface.get("settings", [])),
        "sidebarViews": set(surface.get("sidebarViews", [])),
        "overlayModes": set(surface.get("overlayModes", [])),
        "panelContents": set(surface.get("panelContents", [])),
        "tabKinds": set(surface.get("tabKinds", [])),
        "focusTargets": set(surface.get("focusTargets", [])),
        "queries": set(surface.get("queries", [])),
    }
    out["menuRows"] = {f"{menu}/{row['label']}" for menu, rows in (surface.get("menus") or {}).items() for row in rows}
    out["contextMenuRows"] = {f"{target}/{row['label']}" for target, rows in (surface.get("contextMenus") or {}).items()
                              for row in rows}
    return out


def reached(coverage: dict, suite_commands: set[str]) -> dict[str, set[str]]:
    states = coverage.get("states", {})
    fields: dict[str, set[str]] = {k: set() for k in ("f", "o", "s", "p", "t")}
    for state in states:
        for part in state.split():
            key, _, value = part.partition("=")
            if key in fields:
                fields[key].add(value.split("+")[0])
    commands = set(coverage.get("commands", {})) | suite_commands
    elements = set(coverage.get("elements", {}))
    settings = {key.split("=", 1)[0] for key in coverage.get("settings", {})}
    # The bindings the monkey pressed where they are live, by binding id
    # (Monkey.chord_action). This was read off the state x action pairs, which
    # have been stored as digests since the coverage file was shrunk: the set
    # was always empty, and every binding whose id is not also a command's name
    # -- 49 of 135 -- read as never pressed however often it was.
    pressed = set(coverage.get("keys", {}))
    menu_rows = {name.split(":", 1)[1] for name in elements if name.startswith("menu-item:")}
    return {
        "commands": commands, "settings": settings, "sidebarViews": fields["s"], "overlayModes": fields["o"],
        "panelContents": fields["p"], "tabKinds": fields["t"], "focusTargets": fields["f"],
        "pressedBindings": pressed, "menuRows": menu_rows, "elements": elements,
    }


def suite_commands_from(path: Path | None) -> set[str]:
    if path is None or not path.exists():
        return set()
    out = set()
    for line in path.read_text().splitlines():
        match = re.match(r"^([a-z0-9-]+): ", line)
        if match:
            out.add(match.group(1))
    return out


def gaps(inventory: dict[str, set[str]], seen: dict[str, set[str]]) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for category, items in inventory.items():
        if category == "keybindings":
            # A binding is reached when it was pressed where it is live.
            have = {item for item in items if item.split(" (", 1)[0] in seen["pressedBindings"]}
        elif category == "menuRows":
            have = {item for item in items if item in seen["menuRows"]
                    or any(name.endswith("/" + item.split("/", 1)[1]) for name in seen["menuRows"])}
        elif category == "contextMenuRows":
            labels = {name.rsplit("/", 1)[-1] for name in seen["menuRows"]}
            have = {item for item in items if item.split("/", 1)[1] in labels}
        elif category == "queries":
            have = items  # the driver asks every verb it uses; the drift test asks all
        else:
            have = items & seen.get(category, set())
        out[category] = sorted(items - have)
    return out


def excluded_items(extra: dict) -> dict[str, dict[str, str]]:
    """surface-extra.toml's `[exclude.<category>]` tables: item -> why the
    monkey does not reach it on purpose, and what covers it instead."""
    return {category: dict(items) for category, items in (extra.get("exclude") or {}).items()}


def exclusion_for(category: str, item: str, excluded: dict[str, dict[str, str]]) -> str | None:
    """The reason `item` is excluded, or None. A keybinding is listed by its id
    (`quit`), an item of any other category by its own name.

    >>> exclusion_for("keybindings", "quit (Ctrl+Q)", {"keybindings": {"quit": "ends the run"}})
    'ends the run'
    >>> exclusion_for("commands", "quit", {"keybindings": {"quit": "ends the run"}}) is None
    True
    """
    names = excluded.get(category) or {}
    key = item.split(" (", 1)[0] if category == "keybindings" else item
    return names.get(key)


def summarize(inventory: dict[str, set[str]], missing: dict[str, list[str]],
              excluded: dict[str, dict[str, str]]) -> dict:
    """The surface's coverage in numbers: per category, how many items there
    are, how many nothing reached, and of those how many are excluded with a
    reason -- so `unreached` is what is left to do, and an exclusion that names
    an item the app no longer has is listed as stale."""
    categories = {}
    for category, items in inventory.items():
        gaps_here = missing.get(category, [])
        explained = [item for item in gaps_here if exclusion_for(category, item, excluded) is not None]
        categories[category] = {"total": len(items), "unreached": [i for i in gaps_here if i not in explained],
                                "excluded": explained}
    stale = sorted(f"{category}: {name}" for category, names in excluded.items() for name in names
                   if category in inventory and not any(
                       (item.split(" (", 1)[0] if category == "keybindings" else item) == name
                       for item in inventory[category]))
    total = sum(c["total"] for c in categories.values())
    unreached = sum(len(c["unreached"]) for c in categories.values())
    explained = sum(len(c["excluded"]) for c in categories.values())
    return {"total": total, "reached": total - unreached - explained, "excluded": explained,
            "unreached": unreached, "categories": categories, "staleExclusions": stale}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=REPO / "build/microide/microide")
    parser.add_argument("--coverage", type=Path, default=REPO / "build/soak/coverage.json")
    parser.add_argument("--suite-coverage", type=Path, help="a QA suite run's command-coverage.txt")
    parser.add_argument("--out", type=Path, default=REPO / "build/soak")
    parser.add_argument("--extra", type=Path, default=HERE / "surface-extra.toml")
    parser.add_argument("--inventory", type=Path,
                        help="a saved surface.json to compare against instead of launching the app "
                             "(the soak runner recounts between builds)")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    previous_path = args.out / "surface.json"
    previous = flatten(json.loads(previous_path.read_text())) if previous_path.exists() else {}
    surface = json.loads(args.inventory.read_text()) if args.inventory else read_surface(args.binary)
    inventory = flatten(surface)
    coverage = json.loads(args.coverage.read_text()) if args.coverage.exists() else {}
    seen = reached(coverage, suite_commands_from(args.suite_coverage))
    missing = gaps(inventory, seen)
    extra = tomllib.loads(args.extra.read_text()) if args.extra.exists() else {}
    excluded = excluded_items(extra)
    summary = summarize(inventory, missing, excluded)
    lines = ["# Surface gaps", "",
             f"{summary['reached']} of {summary['total']} inventoried items reached "
             f"({100 * summary['reached'] / max(1, summary['total']):.0f}%), {summary['excluded']} excluded with a "
             f"reason, **{summary['unreached']} not reached**. Inventory: the `surface` query; "
             f"reached: {args.coverage}" + (f" and {args.suite_coverage}" if args.suite_coverage else "") + ".", ""]
    new_items = {cat: sorted(items - previous.get(cat, set())) for cat, items in inventory.items()} if previous else {}
    if any(new_items.values()):
        lines += ["## New since the last inventory", ""]
        for category, items in new_items.items():
            for item in items:
                lines.append(f"- {category}: `{item}`" + (" -- not reached yet" if item in missing[category] else ""))
        lines.append("")
    for category, counts in summary["categories"].items():
        lines.append(f"## {category}: {len(counts['unreached'])} of {counts['total']} not reached"
                     + (f" ({len(counts['excluded'])} more excluded)" if counts["excluded"] else ""))
        lines.append("")
        lines += [f"- `{item}`" for item in counts["unreached"][:200]]
        if len(counts["unreached"]) > 200:
            lines.append(f"- ... and {len(counts['unreached']) - 200} more")
        lines += [f"- ~~`{item}`~~ excluded: {exclusion_for(category, item, excluded)}" for item in counts["excluded"]]
        lines.append("")
    if summary["staleExclusions"]:
        lines += ["## Stale exclusions (surface-extra.toml names an item the app no longer has)", ""]
        lines += [f"- {item}" for item in summary["staleExclusions"]] + [""]
    killed = coverage.get("crashpoints") or {}
    if killed:
        lines += ["## Durable-write boundaries the kill oracle has killed at", "",
                  "By `<site>@<file>` (the app's `crash-points` inventory; a record, its `.bak` and its staging "
                  "temporary are boundaries of their own).", ""]
        lines += [f"- `{name}`: {count}" for name, count in sorted(killed.items())] + [""]
    if extra:
        lines += ["## Not enumerable: the hand-kept list (surface-extra.toml)", ""]
        for name, entry in extra.get("item", {}).items():
            status = entry.get("covered_by") or entry.get("excluded") or "UNCOVERED"
            lines.append(f"- **{name}** ({entry.get('kind', '?')}): {status}")
        lines.append("")
    (args.out / "surface-gaps.md").write_text("\n".join(lines))
    (args.out / "surface-summary.json").write_text(json.dumps(summary, indent=1))
    if not args.inventory or args.inventory.resolve() != previous_path.resolve():
        previous_path.write_text(json.dumps(surface, indent=1))
    print(f"{summary['reached']}/{summary['total']} reached, {summary['excluded']} excluded, "
          f"{summary['unreached']} not reached; {args.out / 'surface-gaps.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
