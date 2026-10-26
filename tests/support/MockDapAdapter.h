#pragma once

// A scriptable DAP adapter for tests: a Python program speaking the Debug
// Adapter Protocol over stdio, whose behaviour argv[1] selects (see the `mode`
// table at the top of the script). Shared by the DAP client/session tests and
// the shell-level tests that need a real stop, step and resume without gdb
// (tests/QaFlowRegressionTests.cpp). Write MockAdapterSource() to a file and
// register MockAdapterCommand(path, mode) as an adapter.

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {

// A minimal DAP adapter that walks the Phase 1 launch lifecycle:
//   initialize -> (initialized event) -> launch -> configurationDone ->
//   output event -> terminated event. Capabilities are toggled by argv[1] so a
//   single script covers both the configurationDone-supported and
//   not-supported code paths.
inline const char* MockAdapterSource() {
  return R"py(import json
import sys

mode = sys.argv[1] if len(sys.argv) > 1 else ""
# config_done / stop / pause / variables / evaluate / restart / threads / exception
# all advertise configurationDone support.
supports_config_done = mode in ("config_done", "stop", "pause", "pause_silent_threads", "silent_terminate",
                                "variables", "evaluate",
                                "restart", "threads", "exception", "die", "thread_event",
                                "function", "reverse", "reverse_late", "stale_stack",
                                "relocate", "partial_continue", "stop_no_thread", "reject_resume",
                                "gdb15_wedged")
# `reject_resume`: after the stop, reject the resume request (continue/next/...) so
# the host must undo its optimistic Running flip and restore the stopped view.
reject_resume = (mode == "reject_resume")
supports_set_variable = (mode == "variables")
supports_evaluate = (mode == "evaluate")
supports_restart = (mode == "restart")
supports_step_back = (mode == "reverse")  # advertise supportsStepBack (reverse execution)
supports_function = (mode == "function")  # advertise supportsFunctionBreakpoints
multi_thread = (mode == "threads")     # `threads` returns two threads + per-thread frames
exception_mode = (mode == "exception")  # advertise exceptionBreakpointFilters (+ filter options)
# `relocate`: setBreakpoints returns an EXTRA non-requested result + an async
# `breakpoint` event that relocates the first breakpoint (E3 consistency).
relocate_mode = (mode == "relocate")
# `partial_continue`: after the stop, emit a single-thread `continued`
# (allThreadsContinued=false) that must NOT resume the session (E4).
partial_continue = (mode == "partial_continue")
# `stop_no_thread`: emit `stopped` with NO threadId; the host must resolve a real
# thread rather than requesting threadId:0 (J9).
stop_no_thread = (mode == "stop_no_thread")
# `thread_event`: a worker thread spawns after the first `threads` fetch; the mock
# emits a `thread` started event so the host must refresh the list a second time.
thread_event_mode = (mode == "thread_event")
thread_started = False
# emit `stopped` after configurationDone
# `reverse_late`: does NOT advertise supportsStepBack at init; turns it on later via
# a DAP `capabilities` event when it receives a `record` evaluate (mirrors gdb under
# rr / gdb `record`, which announce reverse execution after initialize).
reverse_late = (mode == "reverse_late")
stop_on_config = mode in ("stop", "variables", "evaluate", "restart", "threads", "exception",
                          "thread_event", "function", "reverse", "reverse_late", "stale_stack",
                          "partial_continue", "stop_no_thread", "reject_resume")
# `stale_stack`: hold the first stackTrace response until a resume arrives, then
# flush it late so the host must drop the stale stopped view (session Running).
stale_stack = (mode == "stale_stack")
deferred_stack_req = None
running_no_stop = mode in ("pause", "pause_silent_threads", "silent_terminate", "die")  # stay running (die: then exit silently)
# `silent_terminate`: a wedged adapter -- `terminate` is never answered (gdb under
# load stopped answering everything after launch). The host's stop must still end it.
silent_terminate = (mode == "silent_terminate")
# `pause_silent_threads`: gdb's shape -- the thread is announced by a `thread`
# event once the program runs, and a `threads` request sent while it runs is never
# answered (gdb's DAP serves requests only between stops). A `pause` must not
# depend on that answer.
silent_threads = (mode == "pause_silent_threads")
running = False
# `gdb15_wedged`: gdb 15.1's DAP (Ubuntu 24.04). It starts the program inside
# `launch`, and under load that `run` may not return until the program stops, so
# `configurationDone` -- and every request queued behind it, `pause` included --
# goes unanswered. Here: configurationDone starts a real child (reported in a
# `process` event, as gdb does), is left unanswered, and a `pause` is answered only
# once that child has been stopped by SIGINT, which is what gdb's own Ctrl+C sends.
gdb15_wedged = (mode == "gdb15_wedged")
wedged_child = None
wedged_config_done = None
wedged_pauses = []
import threading
write_lock = threading.Lock()

def watch_wedged_child():
    # gdb notices its program's stop by itself: answer what was queued behind
    # configurationDone once the child is stopped by SIGINT.
    import signal
    if wedged_child.wait() != -signal.SIGINT:
        return
    respond(wedged_config_done, {})
    for pause in wedged_pauses:
        respond(pause, {})
    event("stopped", {"reason": "signal", "description": "SIGINT", "threadId": 1,
                      "allThreadsStopped": True})
# `die`: reach Running (answer configurationDone) then exit WITHOUT a terminated event, to
# exercise the host's dead-adapter reconciliation (no zombie session).
die_after_launch = (mode == "die")
seq = 0

def read_message():
    content_length = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
    if content_length is None:
        return None
    body = sys.stdin.buffer.read(content_length)
    if not body:
        return None
    return json.loads(body.decode("utf-8"))

def write_message(message):
    global seq
    with write_lock:
        seq += 1
        message["seq"] = seq
        data = json.dumps(message).encode("utf-8")
        sys.stdout.buffer.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

def respond(req, body=None, success=True, message=None):
    resp = {"type": "response", "request_seq": req["seq"], "success": success,
            "command": req.get("command", "")}
    if body is not None:
        resp["body"] = body
    if message is not None:
        resp["message"] = message
    write_message(resp)

def event(name, body=None):
    msg = {"type": "event", "event": name}
    if body is not None:
        msg["body"] = body
    write_message(msg)

received = []

def finish_launch():
    # Emit the command order so tests can assert setBreakpoints precedes
    # configurationDone on the wire.
    event("output", {"category": "stdout", "output": "commands:" + ",".join(received) + "\n"})
    event("output", {"category": "stdout", "output": "hello from adapter\n"})
    event("terminated")

def emit_stop():
    if stop_no_thread:
        # DAP allows omitting threadId on `stopped`; the host must resolve one.
        event("stopped", {"reason": "breakpoint", "allThreadsStopped": True})
    else:
        event("stopped", {"reason": "breakpoint", "threadId": 1, "allThreadsStopped": True})

def send_stack(req):
    tid = req.get("arguments", {}).get("threadId", 1)
    # Encode the threadId into the top frame name so a thread switch is
    # observable (the frames differ per thread).
    top_name = "main" if tid == 1 else "worker"
    respond(req, {"stackFrames": [
        {"id": tid * 10 + 1, "name": top_name, "line": 10, "column": 1,
         "source": {"name": "main.py", "path": "/proj/main.py"}},
        {"id": tid * 10 + 2, "name": "caller", "line": 3, "column": 1,
         "source": {"name": "main.py", "path": "/proj/main.py"}},
    ], "totalFrames": 2})

while True:
    msg = read_message()
    if msg is None:
        break
    if msg.get("type") != "request":
        continue
    command = msg.get("command")
    received.append(command)
    if command == "initialize":
        caps = {"supportsConfigurationDoneRequest": supports_config_done,
                "supportsSetVariable": supports_set_variable,
                "supportsEvaluateForHovers": supports_evaluate,
                "supportsRestartRequest": supports_restart,
                "supportsStepBack": supports_step_back,
                "supportsFunctionBreakpoints": supports_function,
                "supportsConditionalBreakpoints": supports_function,
                "supportsTerminateRequest": True}
        if exception_mode:
            caps["supportsExceptionFilterOptions"] = True
            caps["exceptionBreakpointFilters"] = [
                {"filter": "raised", "label": "Raised Exceptions", "default": False,
                 "supportsCondition": True},
                {"filter": "uncaught", "label": "Uncaught Exceptions", "default": True,
                 "supportsCondition": True},
            ]
        respond(msg, caps)
        event("initialized")
    elif command == "setBreakpoints":
        args = msg.get("arguments", {})
        src = args.get("source", {}).get("path", "")
        reqs = args.get("breakpoints", [])
        verified = []
        for i, bp in enumerate(reqs):
            verified.append({"id": i + 1, "verified": True, "line": bp.get("line", 0)})
        if relocate_mode and reqs:
            # A non-conformant adapter appends an EXTRA result with no matching
            # request, at the LAST requested line. The host must DROP it, not apply
            # it by line and clobber that breakpoint's adapter id.
            verified.append({"id": 99, "verified": True, "line": reqs[-1].get("line", 0)})
        respond(msg, {"breakpoints": verified})
        if relocate_mode and reqs:
            # Async relocation of the FIRST breakpoint to line+5: the marker must
            # stay at the user's requested line (reconciled by adapter id), not jump.
            event("breakpoint", {"reason": "changed", "breakpoint": {
                "id": 1, "verified": True, "line": reqs[0].get("line", 0) + 5,
                "source": {"path": src}}})
    elif command in ("launch", "attach"):
        respond(msg, {})
        if not supports_config_done:
            finish_launch()
    elif command == "configurationDone":
        # Spec-compliant adapters (gdb 17.2, lldb-dap, debugpy) defer the run to
        # configurationDone and reject it if no launch/attach is pending. Mirror
        # that so a regression to "launch last" fails loudly here.
        if "launch" not in received and "attach" not in received:
            respond(msg, success=False, message="configurationDone before launch")
            continue
        if gdb15_wedged:
            import subprocess
            # `sleep`, not a second Python: a child interpreter failed to start on
            # CI's runner ("Failed to import the site module"), leaving no program to
            # interrupt. SIGINT's default action ends `sleep`, which is all the
            # stand-in debuggee needs.
            wedged_child = subprocess.Popen(["sleep", "600"])
            event("process", {"name": "spin", "systemProcessId": wedged_child.pid,
                              "isLocalProcess": True, "startMethod": "launch"})
            event("thread", {"reason": "started", "threadId": 1})
            wedged_config_done = msg
            threading.Thread(target=watch_wedged_child, daemon=True).start()
            continue
        respond(msg, {})
        if stop_on_config:
            # Echo the received command order (as finish_launch does) so stop-based
            # tests can assert handshake ordering too.
            event("output", {"category": "stdout", "output": "commands:" + ",".join(received) + "\n"})
            emit_stop()
            if partial_continue:
                # A single-thread continue for a DIFFERENT thread. Because
                # allThreadsContinued is false, the host must leave the stopped view
                # intact (not fire on_resumed / flip to Running).
                event("continued", {"threadId": 2, "allThreadsContinued": False})
                event("output", {"category": "stdout", "output": "partial-continue-sent\n"})
        elif die_after_launch:
            # Now Running (the whole handshake answered); exit abruptly with NO
            # terminated event, simulating a crash / external kill / RLIMIT_AS cap.
            # Not at the launch response: the host pipelines setBreakpoints and
            # configurationDone behind launch, so exiting there left a request
            # pending some runs and not others, and a pending request failing on
            # exit reconciles the session by itself (TD-2026-10-08-330).
            sys.stdout.flush()
            sys.exit(0)
        elif running_no_stop:
            # stay running; the test will issue a pause
            if silent_threads:
                running = True
                event("thread", {"reason": "started", "threadId": 1})
        else:
            finish_launch()
    elif command == "stackTrace":
        if stale_stack and deferred_stack_req is None:
            # Hold the first stack request; it is flushed (stale) after the resume.
            deferred_stack_req = msg
        else:
            if stop_no_thread:
                # Surface the threadId the host targeted so the test can assert it
                # resolved a real thread (1) instead of sending threadId:0.
                tid = msg.get("arguments", {}).get("threadId", "none")
                event("output", {"category": "stdout", "output": "stack-thread:" + str(tid) + "\n"})
            send_stack(msg)
    elif command == "restart":
        event("output", {"category": "stdout", "output": "cmd:restart\n"})
        respond(msg, {})
        # Re-run the configuration handshake (some adapters re-emit initialized).
        event("initialized")
    elif command in ("continue", "next", "stepIn", "stepOut", "reverseContinue", "stepBack"):
        event("output", {"category": "stdout", "output": "cmd:" + command + "\n"})
        if reject_resume:
            # Reject the resume: the target stays stopped. The host must restore the
            # stopped view (re-request stackTrace) rather than showing Running.
            respond(msg, success=False, message="cannot resume")
            continue
        if stop_no_thread:
            # Surface the threadId the resume targeted (J9: must be the resolved
            # thread, never 0).
            rtid = msg.get("arguments", {}).get("threadId", "none")
            event("output", {"category": "stdout", "output": "resume-thread:" + str(rtid) + "\n"})
        respond(msg, {"allThreadsContinued": True} if command == "continue" else {})
        if stale_stack and deferred_stack_req is not None:
            # The host already flipped to Running (resume is optimistic). Flush the
            # STALE stackTrace now; a correct host drops it instead of re-projecting
            # a stopped view. The sentinel is written AFTER it on the same pipe, so
            # the test can wait deterministically for the stale stack to be processed
            # without any timing assumption. Do NOT re-stop: stay running.
            send_stack(deferred_stack_req)
            deferred_stack_req = None
            event("output", {"category": "stdout", "output": "stale-stack-flushed\n"})
        else:
            emit_stop()  # re-stop so the test can drive the next step
    elif command == "scopes":
        respond(msg, {"scopes": [
            {"name": "Locals", "variablesReference": 1000, "expensive": False},
            {"name": "Globals", "variablesReference": 2000, "expensive": False},
        ]})
    elif command == "variables":
        ref = msg.get("arguments", {}).get("variablesReference", 0)
        if ref == 1000:
            respond(msg, {"variables": [
                {"name": "x", "value": "1", "type": "int", "variablesReference": 0},
                {"name": "obj", "value": "{...}", "type": "Obj", "variablesReference": 1001},
            ]})
        elif ref == 1001:
            respond(msg, {"variables": [
                {"name": "field", "value": "7", "type": "int", "variablesReference": 0},
            ]})
        else:
            respond(msg, {"variables": []})
    elif command == "setVariable":
        args = msg.get("arguments", {})
        respond(msg, {"value": args.get("value", ""), "type": "int", "variablesReference": 0})
    elif command == "evaluate":
        args = msg.get("arguments", {})
        # Echo the expression + frame so the test can assert both reached the wire.
        expr = args.get("expression", "")
        # Surface gdb value-formatting limit commands (and whether a frameId rode
        # along) so a test can assert they were sent with the safe values and with
        # no frameId (a present frameId:0 pre-launch makes gdb error).
        if expr.startswith("set "):
            event("output", {"category": "stdout",
                             "output": "limit:" + expr + "|frameId=" +
                                       str(args.get("frameId", "none")) + "\n"})
        if reverse_late and expr == "record":
            # gdb turns reverse execution on once recording starts, announcing it
            # with a post-initialize `capabilities` event (a partial body).
            event("capabilities", {"capabilities": {"supportsStepBack": True}})
        value = expr + "@" + str(args.get("frameId", 0))
        # "obj" yields a structured result (variablesReference 1001) so the REPL can
        # expand one level via a follow-up variables request; everything else is a leaf.
        ref = 1001 if expr == "obj" else 0
        respond(msg, {"result": value, "type": "Obj" if ref else "int",
                      "variablesReference": ref})
    elif command == "threads" and silent_threads and running:
        pass  # never answered while the program runs, as gdb does
    elif command == "threads":
        if multi_thread:
            respond(msg, {"threads": [{"id": 1, "name": "main"}, {"id": 2, "name": "worker"}]})
        elif thread_event_mode and not thread_started:
            # First fetch (from the stop): only main exists. A worker then spawns,
            # announced via a `thread` event so the host refreshes the list again.
            respond(msg, {"threads": [{"id": 1, "name": "main"}]})
            thread_started = True
            event("thread", {"reason": "started", "threadId": 2})
        elif thread_event_mode:
            respond(msg, {"threads": [{"id": 1, "name": "main"}, {"id": 2, "name": "worker"}]})
        else:
            respond(msg, {"threads": [{"id": 1, "name": "main"}]})
    elif command == "setFunctionBreakpoints":
        args = msg.get("arguments", {})
        names = [bp.get("name", "") for bp in args.get("breakpoints", [])]
        conds = [bp.get("condition", "") for bp in args.get("breakpoints", [])]
        event("output", {"category": "stdout", "output": "fnbp:" + ",".join(names) + "\n"})
        event("output", {"category": "stdout", "output": "fncond:" + ",".join(conds) + "\n"})
        verified = [{"id": i + 1, "verified": True} for i, _ in enumerate(names)]
        respond(msg, {"breakpoints": verified})
    elif command == "setExceptionBreakpoints":
        args = msg.get("arguments", {})
        filters = args.get("filters", [])
        event("output", {"category": "stdout", "output": "exc:" + ",".join(filters) + "\n"})
        opts = args.get("filterOptions", [])
        if opts:
            rendered = ",".join(o.get("filterId", "") + "=" + o.get("condition", "") for o in opts)
            event("output", {"category": "stdout", "output": "excopt:" + rendered + "\n"})
        respond(msg, {})
    elif command == "pause" and gdb15_wedged:
        wedged_pauses.append(msg)  # queued behind configurationDone, as on gdb 15
    elif command == "pause":
        running = False
        event("output", {"category": "stdout", "output": "cmd:pause\n"})
        respond(msg, {})
        emit_stop()
    elif command == "terminate" and silent_terminate:
        pass  # wedged: never answered
    elif command == "terminate":
        if wedged_child is not None and wedged_child.poll() is None:
            wedged_child.kill()
        respond(msg, {})
        event("terminated")
    elif command == "disconnect":
        # MOCK_DAP_RECORD names a file the disconnect's arguments are appended to:
        # what the host asked for the debuggee at teardown (no event survives it).
        import os
        record = os.environ.get("MOCK_DAP_RECORD")
        if record:
            with open(record, "a") as handle:
                handle.write("disconnect " + json.dumps(msg.get("arguments", {}), sort_keys=True) + "\n")
        respond(msg, {})
        break
    else:
        respond(msg, success=False, message="unknown command")
)py";
}

inline std::vector<std::string> MockAdapterCommand(const std::filesystem::path& server_path,
                                            const std::string& mode) {
  return {"python3", server_path.string(), mode};
}


}  // namespace microide::tests
