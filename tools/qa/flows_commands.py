"""Every command, run for real.

The menu and key sweeps run with a dry-run switch (a row or a chord records the
action it would run), so the actions themselves -- GlobalActionExecutor -- ran at
27 % in the coverage build (TD-2026-10-18-364). This runs each command of the
`commands` query once, bare (no arguments: the argument-taking ones answer with
their usage, which is a path too), in three places a person is: a source file,
a diff, and no project at all. After each the app must answer, its frames must
match a full repaint, and Escape must leave it unmodal.
"""

from __future__ import annotations

import os
from typing import Callable

import microide_qa as q
from flows_common import dismiss, step

# Not run: they leave the app (quit), launch another program (open-url), or open
# the desktop's native file dialog (no argument), which nothing here can answer.
SKIP = {"quit", "open-url", "open", "project-open", "save-as"}
# Run last: what follows runs with no project.
LAST = "project-close"


def _sweep(app: q.App, where: str, names: list[str], restore: Callable[[], None] | None = None) -> None:
    """Runs each of `names`, putting the place back first (`restore`): one
    command opening a diff must not turn the rest of a source-file sweep into a
    diff sweep."""
    ran = 0
    for name in names:
        if restore is not None:
            restore()
        try:
            reply = app.send_line(name, timeout=20.0)
        except q.QaFailure as failure:
            app.expect(False, f"{where}: `{name}` got no answer ({failure})")
            if not app.alive():
                return
            continue
        if not isinstance(reply, dict):
            app.expect(False, f"{where}: `{name}` answered {reply!r}")
        elif reply.get("ok"):
            ran += 1
        elif os.environ.get("MQA_COMMANDS_LOG"):
            # What each refusal said, for reading the sweep by eye.
            print(f"refused | {where} | {name} | {reply.get('error')}", flush=True)
        app.settle(0.15)
        if app.check(f"{where}: {name}") or not app.alive():
            app.expect(app.alive(), f"{where}: the app died running `{name}`")
            if not app.alive():
                return
        dismiss(app)
    # Not a sweep of usage errors: a third or more run bare even with no project.
    print(f"commands-run: {where}: {ran} of {len(names)} ran", flush=True)
    app.expect(ran > len(names) // 3, f"{where}: commands run bare ({ran} of {len(names)})")


def flow_commands_run(app: q.App) -> None:
    """Each command once, for real, from a source file, from a diff, and with no
    project open."""
    names = [c["command"] for c in app.query("commands")
             if not c.get("plugin") and c["command"] not in SKIP and c["command"] != LAST]
    app.expect(len(names) > 150, f"the commands query lists the commands ({len(names)})")

    def source() -> None:
        app.send("open", "src/scheduler.cpp")
        app.send("focus", "editor")

    source()
    app.wait_until(lambda: (app.active_tab() or {}).get("title") == "scheduler.cpp", 5.0, "scheduler.cpp")
    step(app, "a source file", 0.3)
    _sweep(app, "source file", names, source)
    if not app.alive():
        return

    app.send("review-commit", "HEAD")
    if app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 8.0, "a diff tab"):
        step(app, "a diff", 0.3)

        def diff() -> None:
            app.send("review-commit", "HEAD")
            app.send("focus", "editor")

        _sweep(app, "diff", names, diff)
    if not app.alive():
        return

    reply = app.send_line(LAST)
    app.expect(bool(reply.get("ok")), f"project-close runs ({reply})")
    step(app, "no project", 0.5)
    _sweep(app, "no project", names)
    if app.alive():
        app.wait_until(lambda: bool(app.query("status")), 30.0, "a status answer at the end")


def flow_argument_pickers(app: q.App) -> None:
    """Commands that need an argument, run bare from the palette, offer their own
    choices (TD-2026-10-18-357): Show Output Channel… a pick of the channels, Move
    Tab to Group a pick of the other panes, Compare Files… over an open file the
    file finder titled for it, whose pick is compared with it."""
    from flows_common import open_file
    from flows_coverage import palette
    open_file(app, "notes/left.txt", "one\ntwo\n")
    open_file(app, "notes/right.txt", "one\nTWO\n")
    app.key("ctrl+k ctrl+r")  # Revert Selected Ranges outside a diff: refused, logged to "Actions"
    step(app, "a refused key", 0.3)
    if palette(app, "Show Output Channel"):
        app.wait_until(lambda: app.ui()["overlay"].get("kind") == "command-choices", 3.0, "the channel pick")
        overlay = app.ui()["overlay"]
        app.expect(overlay.get("kind") == "command-choices" and overlay.get("title") == "Show Output Channel",
                   f"Show Output Channel… offers a pick of the channels ({overlay})")
        if (overlay.get("count") or 0) > 0:
            app.key("Return")
            app.wait_until(lambda: app.ui()["panel"]["visible"] and app.ui()["panel"]["content"] == "output", 3.0,
                           "Enter to show the channel")
            app.expect(app.ui()["panel"]["content"] == "output", f"Enter shows the channel ({app.ui()['panel']})")
        else:
            app.key("Escape")

    app.send("focus", "editor")
    app.key("ctrl+backslash")
    step(app, "split", 0.3)
    app.key("ctrl+1")
    if palette(app, "Move Tab to Group"):
        app.wait_until(lambda: app.ui()["overlay"].get("kind") == "command-choices", 3.0, "the group pick")
        app.expect(app.ui()["overlay"].get("count") == 1, f"Move Tab to Group offers the other pane ({app.ui()['overlay']})")
        app.key("Return")
        step(app, "move the tab", 0.3)
        groups = app.editor_state()["groups"]
        app.expect(len(groups) == 2 and len(groups[1]["tabs"]) == 2,
                   f"Enter moves the tab to group 2 ({[[t['title'] for t in g['tabs']] for g in groups]})")

    app.key("ctrl+2")
    open_file(app, "notes/left.txt")
    if palette(app, "Compare Files"):
        app.wait_until(lambda: app.ui()["overlay"].get("title") == "Compare left.txt With\u2026", 3.0,
                       "the file finder titled for left.txt")
        app.expect(app.ui()["overlay"].get("title") == "Compare left.txt With\u2026",
                   f"Compare Files… over an open file opens the finder for it ({app.ui()['overlay']})")
        app.type("right.txt")
        app.wait_until(lambda: (app.ui()["overlay"].get("count") or 0) > 0, 3.0, "the finder to find right.txt")
        app.key("Return")
        app.wait_until(lambda: (app.active_tab() or {}).get("kind") == "compare", 5.0, "the comparison")
        app.expect((app.active_tab() or {}).get("kind") == "compare", "Enter compares the pick with the open file")
    dismiss(app)


FLOWS: dict[str, Callable[[q.App], None]] = {"commands-run": flow_commands_run, "argument-pickers": flow_argument_pickers}
