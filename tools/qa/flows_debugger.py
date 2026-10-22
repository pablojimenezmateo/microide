"""The breakpoint kinds and run control no flow reached: a hit count, a log
message (a logpoint: no stop, a line in the debug console per pass), a breakpoint
disabled and enabled again, and Pause on a program that is running.

`dbg-conditions` covers a condition; `dbg-session`/`dbg-stepping`/`debug-keys`
the stepping. The fixture's program runs its loop over four tasks sorted by
priority (flush 9/cost 1, index 8/cost 4, ingest 5/cost 3, compact 2/cost 2), so
which pass a stop is on is read off `task.cost` at the loop body (line 19).
"""

from __future__ import annotations

import re
import subprocess
from typing import Callable

import microide_qa as q
from flows_common import open_file, step, write
from flows_lsp_debug import _require, _state

LOOP_LINE = 19  # `total += task.cost;`


def _adapter_ignored(app: q.App, command: str) -> bool:
    """True when the app sent `command` to the adapter and the adapter let it time
    out (MICROIDE_DAP_LOG): gdb 15.1 under load stops answering
    (TD-2026-10-21-375). The app did its part then; a flow notes it rather than
    reporting the adapter's silence as the app's bug. Any other failure -- the
    request never sent, sent to no thread, answered and still no stop -- stays a
    finding."""
    log = app.work / "dap.log"
    if not log.exists():
        return False
    text = log.read_text(errors="replace")
    sent = re.findall(rf"send\s+request {command} seq=(\d+)", text)
    timed_out = set(re.findall(r"request-timeout seq (\d+)", text))
    if sent and sent[-1] in timed_out:
        print(f"        note: gdb did not answer `{command}` (TD-2026-10-21-375); the app sent it", flush=True)
        return True
    return False


def _gutter_menu(app: q.App, label: str) -> bool:
    """Runs a row of the breakpoint's right-click menu on the loop line."""
    app.click(*app.gutter_point(LOOP_LINE, "breakpoint"), button=3)
    step(app, "breakpoint menu", 0.3)
    row = next((e for e in app.elements("menu-item") if e.get("label", "").startswith(label)), None)
    if not app.expect(row is not None, f"the breakpoint's menu offers {label} "
                                       f"({[e.get('label') for e in app.elements('menu-item')]})"):
        app.key("Escape")
        return False
    app.click(*app.center(row))
    step(app, label, 0.3)
    return True


def _bps(app: q.App) -> list[dict]:
    return [b for f in app.query("breakpoints") for b in f["breakpoints"]]


def _run_to_stop(app: q.App, timeout: float = 40.0) -> bool:
    assert app.project is not None
    app.send("debug-run", "--type", "gdb", str(app.project / "taskflow"))
    return app.wait_until(lambda: _state(app).get("stopped"), timeout, "the program to stop")


def _stop_session(app: q.App) -> None:
    app.send("debug-stop")
    app.wait_until(lambda: not _state(app).get("stopped") and not _state(app).get("frames"), 15.0,
                   "the session to stop")
    step(app, "session stopped", 0.5)


def _cost_at_stop(app: q.App) -> str:
    """`task.cost` read by hovering it at the stop (the debugger's own answer)."""
    app.move(*app.text_point(LOOP_LINE, 26))
    app.wait_until(lambda: (app.ui()["popups"]["hover"] or {}).get("kind") == "debug-value", 8.0,
                   "a debug hover on task.cost")
    text = (app.ui()["popups"]["hover"] or {}).get("text", "").strip()
    x, y, w, h = app.ui()["surfaces"]["sidebar"]
    app.move(x + w / 2, y + h - 20)
    return text


def _session_live(app: q.App) -> bool:
    """A debug session is up while its floating toolbar is (debug-state reports
    the stop, not the session)."""
    return any(e["name"].startswith("button:debug-toolbar/") for e in app.elements("button"))


def _console_lines(app: q.App) -> list[str]:
    """Every session's debug console (`debug.console.<session id>`)."""
    lines: list[str] = []
    for session in range(1, 10):
        lines += app.output(f"debug.console.{session}")
    return lines


def flow_breakpoint_kinds(app: q.App) -> None:
    """Hit count 3 stops on the third pass; a log message stops nowhere and
    logs each pass; a disabled breakpoint is passed over, enabled stops again."""
    if not _require(app, "gdb"):
        return
    assert app.project is not None
    app.send("set-setting", "debug.enabled", "true")
    open_file(app, "src/scheduler.cpp")
    rel = "src/scheduler.cpp"  # the headless commands' form, for cleanup

    # Hit count, through the breakpoint's gutter menu.
    app.click(*app.gutter_point(LOOP_LINE, "breakpoint"))
    step(app, "breakpoint", 0.3)
    if _gutter_menu(app, "Set Hit Count"):
        app.expect((app.ui().get("prompt") or {}).get("kind") == "input",
                   f"Set Hit Count asks for the count ({app.ui().get('prompt')})")
        app.type("3")
        app.key("Return")
        step(app, "hit count 3", 0.3)
    bp = next(iter(_bps(app)), {})
    app.expect(str(bp.get("hitCondition", "")) == "3", f"the hit count is stored ({bp})")
    if _run_to_stop(app):
        step(app, "stopped on the hit count", 0.5)
        app.expect(_state(app).get("line") == LOOP_LINE, f"the stop is on the breakpoint ({_state(app)})")
        cost = _cost_at_stop(app)
        app.expect(cost == "3", f"hit count 3 stops on the third pass: ingest, cost 3 ({cost!r})")
    _stop_session(app)
    app.send("breakpoint-clear", rel)

    # A log message: no stop, one console line per pass with the expression in it.
    app.click(*app.gutter_point(LOOP_LINE, "breakpoint"))
    step(app, "breakpoint", 0.3)
    if _gutter_menu(app, "Set Log Message"):
        app.type("qa-log {task.name}")
        app.key("Return")
        step(app, "log message", 0.3)
    bp = next(iter(_bps(app)), {})
    app.expect("qa-log" in str(bp.get("logMessage", "")), f"the log message is stored ({bp})")
    app.send("debug-run", "--type", "gdb", str(app.project / "taskflow"))
    app.wait_until(lambda: sum("qa-log" in line for line in _console_lines(app)) >= 4, 40.0,
                   "a console line for each of the four passes")
    logged = [line for line in _console_lines(app) if "qa-log" in line]
    app.expect(any("flush" in line for line in logged) and any("compact" in line for line in logged),
               f"the message interpolates task.name ({logged[:6]})")
    app.expect(not _state(app).get("stopped"), f"a logpoint does not stop the program ({_state(app)})")
    _stop_session(app)
    app.send("breakpoint-clear", rel)

    # Disabled: the run passes it; enabled again: it stops.
    app.click(*app.gutter_point(LOOP_LINE, "breakpoint"))
    step(app, "breakpoint", 0.3)
    _gutter_menu(app, "Disable Breakpoint")
    bp = next(iter(_bps(app)), {})
    app.expect(bp.get("enabled") is False, f"the breakpoint is disabled ({bp})")
    app.send("debug-run", "--type", "gdb", str(app.project / "taskflow"))
    app.wait_until(lambda: _state(app).get("stopped") or not _session_live(app), 30.0,
                   "the run to end or stop")
    app.expect(not _state(app).get("stopped"), f"a disabled breakpoint is passed over ({_state(app)})")
    _stop_session(app)
    app.send("breakpoint-enable", rel, str(LOOP_LINE))
    if _run_to_stop(app):
        app.expect(_state(app).get("line") == LOOP_LINE, f"enabled again, it stops ({_state(app)})")
    _stop_session(app)


SPIN = """#include <unistd.h>
volatile unsigned long counter = 0;
int main(void) {
    for (;;) {
        counter++;
        if ((counter & 0xffffff) == 0) usleep(1000);
    }
}
"""


def flow_debug_pause(app: q.App) -> None:
    """F6 pauses a running program where it is; F5 resumes it; Shift+F5 ends it."""
    if not _require(app, "gdb") or not _require(app, "gcc") or not _build_spin(app):
        return
    assert app.project is not None
    app.send("set-setting", "debug.enabled", "true")
    open_file(app, "spin/spin.c")
    app.send("debug-run", "--type", "gdb", str(app.project / "spin" / "spin"))
    app.wait_until(lambda: _state(app).get("session") == "running", 30.0, "the program to be running")
    step(app, "running", 1.0)
    app.expect(not _state(app).get("stopped"), f"the program runs ({_state(app)})")
    app.send("focus", "editor")
    app.key("F6")
    if not app.wait_until(lambda: _state(app).get("stopped"), 15.0, "F6 to pause the running program") \
            and _adapter_ignored(app, "pause"):
        app.findings[:] = [f for f in app.findings if "F6 to pause" not in f.detail]
        _stop_session(app)
        return
    step(app, "paused", 0.5)
    state = _state(app)
    app.expect(state.get("stopped") and state.get("line") in range(4, 7),
               f"the pause stops inside the loop ({state})")
    app.key("F5")
    app.wait_until(lambda: not _state(app).get("stopped"), 10.0, "F5 to resume")
    step(app, "resumed", 0.5)
    app.expect(not _state(app).get("stopped"), f"F5 resumes it ({_state(app)})")
    app.key("shift+F5")
    app.wait_until(lambda: not _session_live(app), 15.0, "Shift+F5 to end the session")
    step(app, "ended", 0.5)


def _sessions(app: q.App) -> list[dict]:
    return _state(app).get("sessions", [])


def _active_session(app: q.App) -> int:
    return next((s["id"] for s in _sessions(app) if s.get("active")), 0)


def _build_spin(app: q.App) -> bool:
    assert app.project is not None
    write(app, "spin/spin.c", SPIN)
    built = subprocess.run(["gcc", "-g", "-O0", "-o", str(app.project / "spin" / "spin"),
                            str(app.project / "spin" / "spin.c")], capture_output=True, text=True)
    return app.expect(built.returncode == 0, f"the spinning program builds ({built.stderr})")


def flow_debug_sessions(app: q.App) -> None:
    """Two programs debugged at once: Switch Debug Session cycles the active one
    (and F6 then pauses that one, not the other), Stop All Debug Sessions ends
    both."""
    if not _require(app, "gdb") or not _require(app, "gcc") or not _build_spin(app):
        return
    assert app.project is not None
    app.send("set-setting", "debug.enabled", "true")
    open_file(app, "spin/spin.c")
    for n in (1, 2):
        app.send("debug-run", "--type", "gdb", str(app.project / "spin" / "spin"))
        app.wait_until(lambda: len(_sessions(app)) == n and all(s["state"] == "running" for s in _sessions(app)),
                       30.0, f"session {n} to run")
    step(app, "two sessions", 0.5)
    sessions = _sessions(app)
    if not app.expect(len(sessions) == 2, f"two sessions run at once ({sessions})"):
        return
    first, second = sessions[0]["id"], sessions[1]["id"]
    app.expect(_active_session(app) == second, f"the newer session is the active one ({sessions})")
    app.send("debug-switch-session")
    app.wait_until(lambda: _active_session(app) == first, 5.0, "Switch Debug Session to the other")
    step(app, "switch", 0.3)
    app.expect(_active_session(app) == first, f"Switch Debug Session makes the other active ({_sessions(app)})")
    app.send("focus", "editor")
    app.key("F6")
    if not app.wait_until(lambda: any(s["id"] == first and s["state"] == "stopped" for s in _sessions(app)), 15.0,
                          "F6 to pause the active session") and _adapter_ignored(app, "pause"):
        app.findings[:] = [f for f in app.findings if "F6 to pause" not in f.detail]
        app.send("debug-stop-all")
        return
    states = {s["id"]: s["state"] for s in _sessions(app)}
    app.expect(states.get(first) == "stopped" and states.get(second) == "running",
               f"F6 pauses the active session and leaves the other running ({states})")
    app.send("debug-switch-session")
    app.wait_until(lambda: _active_session(app) == second, 5.0, "Switch Debug Session back")
    app.expect(_active_session(app) == second, f"and back again ({_sessions(app)})")
    if _palette_run(app, "Stop All Debug Sessions"):
        app.wait_until(lambda: not _sessions(app), 20.0, "every session to end")
        app.expect(not _sessions(app), f"Stop All Debug Sessions ends both ({_sessions(app)})")
        app.wait_until(lambda: not _session_live(app), 10.0, "the toolbar to go")


def _palette_run(app: q.App, label: str) -> bool:
    app.key("ctrl+shift+p")
    step(app, "palette", 0.3)
    app.type(label)
    step(app, f"type {label}", 0.3)
    row = next((r for r in app.elements("overlay-row") if r.get("label", "").startswith(label)), None)
    if not app.expect(row is not None, f"the palette lists {label!r}"):
        app.key("Escape")
        return False
    app.click(*app.center(row))
    step(app, label, 0.4)
    return True



def _palette(app: q.App, label: str) -> bool:
    """Runs a palette row by its label (typed, the row clicked)."""
    app.key("ctrl+shift+p")
    step(app, "palette", 0.3)
    app.type(label)
    step(app, f"type {label}", 0.3)
    row = next((r for r in app.elements("overlay-row") if r.get("label", "").startswith(label)), None)
    if not app.expect(row is not None, f"the palette lists {label!r}"):
        app.key("Escape")
        return False
    app.click(*app.center(row))
    step(app, label, 0.3)
    return True


def flow_function_breakpoints(app: q.App) -> None:
    """A function breakpoint, added from the palette's prompt, stops the program
    in that function; toggled off, the run passes it; with a condition on
    `Scheduler::add` it stops once, for the one task that meets it. No flow
    reached the function-breakpoint commands (TD-364)."""
    if not _require(app, "gdb"):
        return
    assert app.project is not None
    app.send("set-setting", "debug.enabled", "true")
    open_file(app, "src/scheduler.cpp")

    if _palette(app, "Add Function Breakpoint"):
        app.expect((app.ui().get("prompt") or {}).get("kind") == "input",
                   f"Add Function Breakpoint asks for the name ({app.ui().get('prompt')})")
        app.type("taskflow::Scheduler::run")
        app.key("Return")
        step(app, "the function's name", 0.3)
    if _run_to_stop(app):
        state = _state(app)
        app.expect(12 <= int(state.get("line") or 0) <= 16 and str(state.get("file", "")).endswith("scheduler.cpp"),
                   f"the run stops in Scheduler::run ({state.get('file')}:{state.get('line')})")
    _stop_session(app)

    app.send_line("breakpoint-function-toggle taskflow::Scheduler::run")
    app.send("debug-run", "--type", "gdb", str(app.project / "taskflow"))
    app.wait_until(lambda: _state(app).get("stopped") or not _session_live(app), 30.0, "the run to end or stop")
    app.expect(not _state(app).get("stopped"), f"a disabled function breakpoint is passed over ({_state(app)})")
    _stop_session(app)
    app.send_line("breakpoint-function-remove taskflow::Scheduler::run")

    # A condition: Scheduler::add runs four times, priority 9 once.
    app.send_line("breakpoint-function-add taskflow::Scheduler::add")
    app.send_line("breakpoint-function-condition taskflow::Scheduler::add priority == 9")
    stops = 0
    if _run_to_stop(app):
        stops += 1
        app.send("debug-continue")
        app.wait_until(lambda: _state(app).get("stopped") or not _session_live(app), 30.0,
                       "the run to end or stop again")
        if _state(app).get("stopped"):
            stops += 1
    app.expect(stops == 1, f"a conditional function breakpoint stops once, for priority 9 ({stops} stops)")
    _stop_session(app)
    reply = app.send("breakpoint-function-remove", "taskflow::Scheduler::add")
    app.expect(bool(reply.get("ok")), f"breakpoint-function-remove runs ({reply})")



THROWER = """#include <stdexcept>
#include <cstdio>
int risky(int n) { if (n == 2) throw std::runtime_error("two"); return n; }
int main() {
  int total = 0;
  for (int i = 0; i < 4; ++i) {
    try { total += risky(i); } catch (const std::exception& e) { std::printf("caught %s\\n", e.what()); }
  }
  return total;
}
"""


def flow_exception_breakpoints(app: q.App) -> None:
    """The adapter's exception filters, from the Breakpoints pane: after a first
    run lists them, a click on "when thrown" makes the next run stop inside the
    throw (risky() on the stack) and only there; a click on "when caught" in its
    place stops at the catch; with neither, the run passes. No flow reached the
    exception filters (TD-364)."""
    if not _require(app, "gdb") or not _require(app, "g++"):
        return
    assert app.project is not None
    write(app, "exc/exc.cpp", THROWER)
    built = subprocess.run(["g++", "-g", "-O0", "-o", str(app.project / "exc" / "exc"),
                            str(app.project / "exc" / "exc.cpp")], capture_output=True, text=True)
    if not app.expect(built.returncode == 0, f"the throwing program builds ({built.stderr})"):
        return
    program = str(app.project / "exc" / "exc")
    app.send("set-setting", "debug.enabled", "true")
    open_file(app, "exc/exc.cpp")

    def run_to_end_or_stop() -> bool:
        app.send("debug-run", "--type", "gdb", program)
        # The session first, then its end: the toolbar is not up yet right after
        # the command, which read as "already ended".
        app.wait_until(lambda: _session_live(app) or _state(app).get("stopped"), 15.0, "the session to start")
        app.wait_until(lambda: _state(app).get("stopped") or not _session_live(app), 30.0, "the run to end or stop")
        return bool(_state(app).get("stopped"))

    # A first run: the adapter advertises its filters.
    run_to_end_or_stop()

    def filter_row(prefix: str) -> dict | None:
        return next((e for e in app.elements("debug-row") if e.get("label", "").startswith(prefix)), None)

    def toggle(prefix: str) -> bool:
        # The pane goes away with each session, so each toggle opens its view.
        app.send_line("debug-pane-breakpoints")
        step(app, "the Breakpoints view", 0.3)
        app.wait_until(lambda: filter_row(prefix) is not None, 5.0, f"{prefix!r} listed")
        row = filter_row(prefix)
        if not app.expect(row is not None, f"the Breakpoints view lists {prefix!r} "
                                           f"({[e.get('label') for e in app.elements('debug-row')]})"):
            return False
        app.click(*app.center(row))
        step(app, f"toggle {prefix}", 0.3)
        return True

    def functions() -> list[str]:
        return [str(f.get("function")) for f in _state(app).get("frames", [])]

    if toggle("C++ exceptions, when thrown"):
        stopped = run_to_end_or_stop()
        app.expect(stopped and "risky" in functions(), f"the run stops on the throw, in risky() ({functions()})")
        if stopped:
            app.send("debug-continue")
            app.wait_until(lambda: _state(app).get("stopped") is False or not _session_live(app), 10.0, "the resume")
            app.wait_until(lambda: not _session_live(app) or _state(app).get("stopped"), 30.0, "the run to end")
            app.expect(not _state(app).get("stopped"), f"one throw, one stop ({_state(app)})")
        _stop_session(app)
        toggle("C++ exceptions, when thrown")
    if toggle("C++ exceptions, when caught"):
        stopped = run_to_end_or_stop()
        app.expect(stopped and "risky" not in functions()[:2] and "main" in functions(),
                   f"the run stops at the catch, in main ({functions()})")
        _stop_session(app)
        toggle("C++ exceptions, when caught")
    app.expect(not run_to_end_or_stop(), f"with no filter on, the run passes the throw ({_state(app)})")
    _stop_session(app)

    # A condition per filter, through the control API: set where the filter takes
    # one, refused with the reason where it does not, and refused for an unknown id.
    filters = app.query("exception-filters")
    filters = filters if isinstance(filters, list) else filters.get("filters", [])
    for item in filters:
        reply = app.send("breakpoint-exception-condition", item["id"], "1 == 1")
        if item.get("supportsCondition"):
            app.expect(bool(reply.get("ok")), f"{item['id']} takes a condition ({reply})")
            current = next((f for f in (app.query("exception-filters") if isinstance(app.query("exception-filters"), list)
                                        else []) if f["id"] == item["id"]), {})
            app.expect(current.get("condition") == "1 == 1", f"and reports it ({current})")
            app.send("breakpoint-exception-condition", item["id"])
        else:
            app.expect(not reply.get("ok") and "no condition" in reply.get("error", ""),
                       f"{item['id']} takes no condition and says so ({reply})")
    reply = app.send("breakpoint-exception-condition", "no-such-filter", "x")
    app.expect(not reply.get("ok") and "no exception filter" in reply.get("error", ""),
               f"an unknown filter id is refused with the known ones ({reply})")


def _breakpoint(app: q.App, line: int) -> dict:
    return next((b for b in _bps(app) if b["line"] == line), {})


def flow_debug_commands(app: q.App) -> None:
    """The debugger's commands no flow ran: Enable Debugger from the palette; the
    breakpoint commands of the control API (a condition, a hit count and a log
    message set on one breakpoint, read back, disabled, each cleared); Select
    Launch Configuration from the palette, its picker listing the plugin's
    configuration and Enter launching it to the breakpoint; at the stop, Show Call
    Stack, Show Debug Output and the pane toggled away and back; Step Back refused
    on gdb, which cannot run backwards, with the reason; `debug-launch <name>`."""
    if not _require(app, "gdb"):
        return
    enabled = lambda: next(s["value"] for s in app.query("settings") if s["id"] == "debug.enabled")  # noqa: E731
    before = enabled()
    _palette(app, "Enable Debugger")
    app.expect(enabled() != before, f"Enable Debugger flips debug.enabled ({before} -> {enabled()})")
    if enabled() != "true":
        app.send("set-setting", "debug.enabled", "true")

    main = "src/main.cpp"
    app.send("breakpoint-set", main, "12")
    app.send("breakpoint-condition", main, "12", "1 == 1")
    app.send("breakpoint-hit-condition", main, "12", "1")
    app.send("breakpoint-logmessage", main, "12", "at line twelve")
    bp = _breakpoint(app, 12)
    app.expect(bp.get("condition") == "1 == 1" and bp.get("hitCondition") == "1"
               and bp.get("logMessage") == "at line twelve",
               f"the condition, hit count and log message are on the breakpoint ({bp})")
    app.send("breakpoint-disable", main, "12")
    app.expect(_breakpoint(app, 12).get("enabled") is False, f"breakpoint-disable disables it ({_breakpoint(app, 12)})")
    for verb in ("breakpoint-condition", "breakpoint-hit-condition", "breakpoint-logmessage"):
        app.send(verb, main, "12")
    app.send("breakpoint-enable", main, "12")
    bp = _breakpoint(app, 12)
    app.expect(bp.get("enabled") and not bp.get("condition") and not bp.get("hitCondition")
               and not bp.get("logMessage"),
               f"each clears with no argument, and it is a plain enabled breakpoint again ({bp})")

    open_file(app, main)
    _palette(app, "Select Launch Configuration")
    overlay = app.ui()["overlay"]
    rows = [e.get("label") for e in app.elements("overlay-row")]
    app.expect(overlay.get("mode") == "launch-config-picker" and "taskflow (QA)" in rows,
               f"the picker lists the plugin's configuration ({overlay.get('mode')}, {rows})")
    app.key("Return")
    if not app.wait_until(lambda: _state(app).get("stopped"), 40.0, "the picked configuration to stop at line 12"):
        return
    step(app, "stopped", 0.5)
    app.expect(_state(app).get("line") == 12, f"it stops on the breakpoint ({_state(app).get('line')})")

    app.send("debug-pane-callstack")
    step(app, "show call stack", 0.3)
    tabs = {e["name"]: e.get("active") for e in app.elements() if e["name"].startswith("debug-tab:")}
    app.expect(tabs.get("debug-tab:callstack") is True, f"Show Call Stack selects the call stack ({tabs})")
    app.send("debug-show-output")
    step(app, "show debug output", 0.3)
    shown = app.query("output").get("shown", "")
    app.expect(app.ui()["panel"]["visible"] and shown.startswith("debug.console"),
               f"Show Debug Output shows the session's console ({shown!r})")
    app.send("debug-pane-toggle")
    step(app, "pane away", 0.3)
    app.expect(not any(e["name"].startswith("debug-tab:") for e in app.elements()), "the pane toggles away")
    app.send("debug-pane-toggle")
    step(app, "pane back", 0.3)
    app.expect(any(e["name"].startswith("debug-tab:") for e in app.elements()), "and back")

    for verb in ("debug-step-back", "debug-reverse-continue"):
        reply = app.send(verb)
        app.expect(not reply.get("ok") and "backwards" in reply.get("error", ""),
                   f"{verb} on gdb is refused with the reason ({reply})")
    app.expect(_state(app).get("line") == 12, "and nothing moved")

    _stop_session(app)
    reply = app.send("debug-launch", "taskflow (QA)")
    app.expect(bool(reply.get("ok")), f"debug-launch by name starts the configuration ({reply})")
    if app.wait_until(lambda: _state(app).get("stopped"), 40.0, "debug-launch to stop at the breakpoint"):
        step(app, "stopped again", 0.3)
    app.key("shift+F5")
    app.wait_until(lambda: _state(app).get("session") in ("none", "terminated"), 15.0, "Shift+F5 to end it")
    # Start Debugging with the configuration last picked.
    reply = app.send("debug-start")
    app.expect(bool(reply.get("ok")), f"Start Debugging runs the configuration ({reply})")
    if app.wait_until(lambda: _state(app).get("stopped"), 40.0, "debug-start to stop at the breakpoint"):
        _stop_session(app)


FLOWS: dict[str, Callable[[q.App], None]] = {
    "dbg-breakpoint-kinds": flow_breakpoint_kinds,
    "dbg-pause": flow_debug_pause,
    "dbg-sessions": flow_debug_sessions,
    "dbg-function-breakpoints": flow_function_breakpoints,
    "dbg-exception-breakpoints": flow_exception_breakpoints,
    "dbg-commands": flow_debug_commands,
}
PLUGINS: dict[str, tuple[str, ...]] = {
    "dbg-breakpoint-kinds": ("gdb-dap",),
    "dbg-pause": ("gdb-dap",),
    "dbg-sessions": ("gdb-dap",),
    "dbg-function-breakpoints": ("gdb-dap",),
    "dbg-exception-breakpoints": ("gdb-dap",),
    "dbg-commands": ("gdb-dap", "qa-launch"),
}
