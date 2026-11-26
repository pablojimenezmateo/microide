-- QA-only: a debug adapter of type `qachaos` that answers the way a real one
-- does on a bad day, for the soak's `tooling` profile (TD-2026-11-08-435 item 5).
-- gdb answers in order and once, so the host's request bookkeeping had only been
-- driven off that path by WorkspaceDapClient's sequence tests and DapSessionFuzz.
--
-- It debugs no program: `debug-run --type qachaos <file>` starts a pretend one
-- whose frames point into <file>. Each request is answered on time, late (so
-- later ones overtake it), twice, unsuccessfully, with a body of the wrong shape,
-- after a response to a request never sent, under the wrong command name, or
-- with a string request_seq first. Stops name threads that do not exist, frames
-- point past the end of the file, variables nest and run long, and events come
-- unasked: output floods, capability changes, module and invalidated events, an
-- event nobody defined, a reverse request nobody defined. What a request gets is
-- a function of its command and seq, so a replay that sends the same requests
-- sees the same answers. Every request is answered within two seconds: one the
-- host still counts in flight after that has been lost by the host
-- (monkey_oracles.RequestsSettle).
local ide = require("microide")

local ADAPTER = [==[
import json, os, random, sys, threading

lock = threading.Lock()
out_seq = [0]
program = ["/nowhere.qachaos"]

def send(message):
    with lock:
        out_seq[0] += 1
        message = dict(message, seq=out_seq[0])
        body = json.dumps(message).encode("utf-8")
        sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        sys.stdout.buffer.flush()

def later(delay, message):
    timer = threading.Timer(delay, send, [message])
    timer.daemon = True
    timer.start()

def read():
    length = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1])
    return json.loads(sys.stdin.buffer.read(length)) if length else None

def event(name, body=None):
    message = {"type": "event", "event": name}
    if body is not None:
        message["body"] = body
    return message

def response(request, body=None, success=True, message=None):
    reply = {"type": "response", "request_seq": request["seq"], "command": request.get("command", ""),
             "success": success}
    if body is not None:
        reply["body"] = body
    if message is not None:
        reply["message"] = message
    return reply

def stopped(rng):
    body = {"reason": rng.choice(["entry", "step", "breakpoint", "exception", "pause", "goto", "chaos"]),
            "allThreadsStopped": rng.random() < 0.5}
    thread = rng.choice([1, 1, 2, 99, None])
    if thread is not None:
        body["threadId"] = thread
    if body["reason"] == "exception":
        body["text"] = "chaos " * rng.choice([1, 2000])
    return event("stopped", body)

def source():
    return {"name": os.path.basename(program[0]), "path": program[0]}

def body_for(command, arguments, rng):
    if command == "threads":
        count = rng.choice([1, 2, 200])
        return {"threads": [{"id": n + 1, "name": "main" if n == 0 else "worker 漢字 %d" % n}
                            for n in range(count)]}
    if command == "stackTrace":
        count = rng.choice([0, 3, 500])
        return {"stackFrames": [{"id": 1000 * (arguments.get("threadId") or 1) + n,
                                 "name": "frame_%d" % n, "source": source(),
                                 "line": rng.choice([0, 1, 7, 24, 99999]), "column": rng.choice([0, 1, 400])}
                                for n in range(count)], "totalFrames": count + rng.choice([0, 10])}
    if command == "scopes":
        frame = arguments.get("frameId", 0)
        return {"scopes": [{"name": "Locals", "variablesReference": 10 + frame % 1000, "expensive": False},
                           {"name": "Registers", "variablesReference": 0, "expensive": True}]}
    if command == "variables":
        ref = arguments.get("variablesReference", 0)
        count = 0 if ref > 10 ** 6 else rng.choice([0, 5, 1000])
        return {"variables": [{"name": "v%d" % n, "value": rng.choice(["1", "x" * 100000, "éé", ""]),
                               "type": "chaos", "variablesReference": ref * 10 + n if n < 3 else 0}
                              for n in range(count)]}
    if command == "evaluate":
        return {"result": rng.choice(["42", "y" * 50000, ""]), "variablesReference": rng.choice([0, 77])}
    if command == "setVariable":
        return {"value": str(arguments.get("value", ""))}
    if command == "setBreakpoints":
        wanted = arguments.get("breakpoints") or []
        kept = wanted[:len(wanted) - 1] if wanted and rng.random() < 0.2 else wanted
        return {"breakpoints": [{"id": n + 1, "verified": rng.random() < 0.7,
                                 "line": b.get("line", 1) + rng.choice([0, 0, 1, 5000]), "source": source()}
                                for n, b in enumerate(kept)]}
    if command in ("setFunctionBreakpoints", "setExceptionBreakpoints"):
        wanted = arguments.get("breakpoints") or arguments.get("filters") or []
        return {"breakpoints": [{"id": 500 + n, "verified": rng.random() < 0.5} for n, _ in enumerate(wanted)]}
    if command in ("continue", "reverseContinue"):
        return {"allThreadsContinued": rng.random() < 0.5}
    if command == "source":
        return {"content": "chaos source\n" * rng.choice([1, 5000])}
    return None

RESUMES = ("continue", "next", "stepIn", "stepOut", "stepBack", "reverseContinue")

def after(command, rng):
    """What the pretend program does once a request is answered."""
    if command == "launch":
        later(0.05, event("process", {"name": program[0], "systemProcessId": 0, "startMethod": "launch"}))
        later(0.1, event("thread", {"reason": "started", "threadId": 1}))
        later(rng.uniform(0.1, 1.0), stopped(rng))
    elif command in RESUMES:
        later(0.02, event("continued", {"threadId": 1, "allThreadsContinued": True}))
        roll = rng.random()
        if roll < 0.05:
            later(0.3, event("exited", {"exitCode": rng.choice([0, 1, -9])}))
            later(0.35, event("terminated"))
        elif roll < 0.2:
            later(0.2, event("output", {"category": "stdout", "output": "flood \x1b[31mred\x1b[0m\n" * 3000}))
            later(1.0, stopped(rng))
        else:
            later(rng.uniform(0.05, 1.5), stopped(rng))
    elif command == "pause":
        later(rng.uniform(0.0, 0.5), stopped(rng))
    elif command == "restart":
        later(0.2, stopped(rng))
    if rng.random() < 0.15:
        extra = rng.choice([
            event("output", {"category": rng.choice(["console", "stderr", "important", "telemetry"]),
                             "output": "漢字 chaos\n" * rng.choice([1, 500])}),
            event("capabilities", {"capabilities": {"supportsStepBack": rng.random() < 0.5}}),
            event("module", {"reason": "new", "module": {"id": 1, "name": "chaos.so"}}),
            event("invalidated", {"areas": ["all"]}),
            event("breakpoint", {"reason": "changed", "breakpoint": {"id": 1, "verified": True, "line": 3}}),
            event("thread", {"reason": rng.choice(["started", "exited"]), "threadId": rng.choice([2, 99])}),
            event("chaosEvent", {"any": [1, 2]}),
            {"type": "request", "command": "chaosReverse", "arguments": {}},
        ])
        later(rng.uniform(0.0, 1.0), extra)

def chaos_reply(request, reply, rng):
    roll = rng.random()
    if roll < 0.35:
        send(reply)
    elif roll < 0.55:
        later(rng.uniform(0.05, 2.0), reply)
    elif roll < 0.63:
        send(reply)
        later(rng.uniform(0.0, 0.5), reply)
    elif roll < 0.71:
        send(response(request, success=False, message=rng.choice(["chaos", "", "e" * 3000])))
    elif roll < 0.79:
        send(dict(reply, body=rng.choice([42, "text", [1, None], {"bogus": True}])))
    elif roll < 0.85:
        send(dict(reply, request_seq=request["seq"] + 100000))
        send(reply)
    elif roll < 0.9:
        send(dict(reply, command="chaos"))
        later(0.1, reply)
    elif roll < 0.95:
        send(dict(reply, request_seq=str(request["seq"])))
        later(0.2, reply)
    else:
        send(dict(reply, success=True, body=None))

def handle(request):
    command = request.get("command", "")
    arguments = request.get("arguments") or {}
    rng = random.Random("%s|%s" % (command, request.get("seq")))
    if command == "initialize":
        send(response(request, {"supportsConfigurationDoneRequest": True, "supportsFunctionBreakpoints": True,
                                "supportsConditionalBreakpoints": True, "supportsHitConditionalBreakpoints": True,
                                "supportsLogPoints": True, "supportsSetVariable": True,
                                "supportsEvaluateForHovers": True, "supportsRestartRequest": rng.random() < 0.5,
                                "supportsTerminateRequest": True, "supportsStepBack": rng.random() < 0.5,
                                "exceptionBreakpointFilters": [{"filter": "throw", "label": "Throw"},
                                                               {"filter": "chaos", "label": "漢字"}]}))
        later(0.05, event("initialized"))
        return
    if command in ("disconnect", "terminate"):
        send(response(request))
        send(event("terminated"))
        if command == "disconnect":
            threading.Timer(0.1, os._exit, [0]).start()
        return
    if command == "launch":
        program[0] = str(arguments.get("program") or program[0])
    known = command in ("launch", "attach", "configurationDone", "pause", "restart", "source", "setVariable",
                        "evaluate", "threads", "stackTrace", "scopes", "variables", "setBreakpoints",
                        "setFunctionBreakpoints", "setExceptionBreakpoints") or command in RESUMES
    reply = response(request, body_for(command, arguments, rng)) if known else \
        response(request, success=False, message="unknown request " + command)
    chaos_reply(request, reply, rng)
    if known:
        after(command, rng)

while True:
    message = read()
    if message is None:
        break
    if message.get("type") == "request":
        handle(message)
]==]

return ide.plugin({
  id = "qa-chaos-dap",
  capabilities = { process = { exec = true } },

  setup = function(ctx)
    ctx.debug.add({
      id = "qa-chaos-dap",
      type = "qachaos",
      command = { "python3", "-c", ADAPTER },
    })
  end,
})
