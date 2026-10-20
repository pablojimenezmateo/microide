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


FLOWS: dict[str, Callable[[q.App], None]] = {"commands-run": flow_commands_run}
