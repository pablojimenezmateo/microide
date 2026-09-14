#!/usr/bin/env python3
"""Find `constexpr` constants of the SAME NAME defined in more than one file.

The failure shape this session kept finding: two files answer one question with
their own copy of a number, one file's comment claims it mirrors the other, and
nothing enforces it. The control wire's request ceiling had drifted 16x that way
(client framed 16 MiB, server shed anything past 1 MiB); the merge toolbar and
the sidebar list origin had not drifted YET, but a change to either copy would
have painted buttons where clicks do not land.

A shared name is not automatically a bug -- `kInset` means different things in
the project-search panel and the commit workflow -- so this reports groups and
exits 1 only for UNACCEPTED ones, like tools/clone-scan.py. Entries in
ACCEPTED_DUPLICATES carry the reason they are separate; a stale entry (its name
no longer duplicated) also fails, so the list cannot rot into a rubber stamp.

Usage:  python3 tools/duplicate-constant-scan.py [root=src]
"""

from __future__ import annotations

import collections
import os
import re
import sys

# name -> why these are legitimately separate values, not one question answered twice.
ACCEPTED_DUPLICATES = {
    "kButtonGap": "different buttons: project-search panel, workspace chrome, commit workflow",
    "kButtonHeight": "same -- three unrelated button families with their own metrics",
    "kInset": "panel-specific padding; the sidebar render TU static_asserts its own against the commit field",
    "kMargin": "hover tooltip vs presentation chrome; unrelated surfaces",
    "kPrefix": "unrelated literal prefixes (theme link, gitdir:, ref:)",
    "kHeaderBytes": "the persisted reader derives it from the magic; the writer states it -- same value, two derivations, pinned by the round-trip tests",
    "kMaxCodeActions": "plugin providers and the LSP server have separate budgets",
    "kMaxParameters": "same -- plugin vs LSP signature-help ceilings",
    "kMaxDocumentSymbolDepth": "the protocol's 64 binds; the sidebar's 256 is a looser walk guard",
    "kMaxPooledBuffers": "terminal line pool vs surface token window; unrelated pools",
    "kHighBits": "SWAR masks, independently obvious",
    "kLowOnes": "same",
    "kFileScheme": "the literal \"file://\" in two unrelated parsers",
    "kNoFootprint": "size_t sentinel, independently obvious",
    "kMaxHoverBytes": "plugin vs LSP hover ceilings, deliberately equal but separately owned",
    "kMaxSignatures": "same",
    "kMaxQueuedBytes": "DAP and LSP client queues, separately tuned",
    "kMaxQueuedMessages": "same",
    "kMaxTerminalPasteBytes": "the input coordinator's guard and the session's own, deliberately equal",
    "kCommitButtonHeight": "the render TU shadows the shared header's value; harmless but worth collapsing",
    "kScrollbarInset": "both alias the one kWorkspaceScrollbarInset",
    "kScrollbarThickness": "both alias the one kWorkspaceScrollbarThickness",
    "kOverlayInset": "overlay chrome padding, view-model builder and render TU",
    "kSidebarInset": "render TU static_asserts it against kCommitWorkflowFieldInset",
    "kMaxRequestLineBytes": "both alias the one kMaxControlRequestLineBytes",
}

DEF = re.compile(
    r"\b(?:inline\s+)?constexpr\s+(?:std::)?\w[\w:]*\s+(k[A-Z]\w+)\s*=\s*([^;]+);"
)


def scan(root: str) -> dict[str, list[tuple[str, int, str]]]:
    found: dict[str, list[tuple[str, int, str]]] = collections.defaultdict(list)
    for base, _, files in os.walk(root):
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(base, name)
            with open(path, errors="replace") as handle:
                for number, line in enumerate(handle, 1):
                    match = DEF.search(line)
                    if match:
                        found[match.group(1)].append(
                            (path, number, re.sub(r"\s+", " ", match.group(2).strip()))
                        )
    return found


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else "src"
    found = scan(root)
    duplicated = {
        name: sites
        for name, sites in found.items()
        if len({site[0] for site in sites}) > 1
    }

    failures = 0
    for name, sites in sorted(duplicated.items()):
        values = {site[2] for site in sites}
        accepted = name in ACCEPTED_DUPLICATES
        label = "accepted" if accepted else "UNACCEPTED"
        if not accepted:
            failures += 1
        print(f"--- {label}: {name}" + ("  <-- DIFFERENT VALUES" if len(values) > 1 else ""))
        if accepted:
            print(f"      reason: {ACCEPTED_DUPLICATES[name]}")
        for path, number, value in sites:
            print(f"      {path}:{number} = {value}")

    stale = sorted(set(ACCEPTED_DUPLICATES) - set(duplicated))
    for name in stale:
        print(f"--- STALE acceptance (no longer duplicated): {name}")
        failures += 1

    if failures:
        print(f"\n{failures} unaccepted or stale duplicate constant group(s).")
        print("Either collapse them to one definition, or add the reason to ACCEPTED_DUPLICATES.")
    else:
        print("\nNo unaccepted duplicate constants.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
