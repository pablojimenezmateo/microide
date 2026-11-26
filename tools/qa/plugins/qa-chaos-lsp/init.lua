-- QA-only: a language server for `.qachaos` files that answers the way a real
-- one does on a bad day, for the soak's `tooling` profile (TD-2026-11-08-435
-- item 5). clangd answers in order, once, with well-formed results, so the host's
-- request bookkeeping had only been driven off that path by unit tests.
--
-- Each request is answered on time, late (so later ones overtake it), twice,
-- with an error, with a result of the wrong shape, after a reply to an id that
-- was never sent, under a string id, or with neither result nor error first.
-- Results point past the end of the file, at files that do not exist, or are
-- very large. Every opened or changed document gets diagnostics, some for a
-- version it no longer has or for a file that is not open, and now and then a
-- request or notification from the server. What a request gets is a function of
-- its method and id, so a replay that sends the same requests sees the same
-- answers. Every request is answered within two seconds: one the host still
-- counts in flight after that has been lost by the host (monkey_oracles.RequestsSettle).
local ide = require("microide")

local SERVER = [==[
import json, random, sys, threading

lock = threading.Lock()
server_ids = [5000]

def send(payload):
    body = json.dumps(payload).encode("utf-8")
    with lock:
        sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        sys.stdout.buffer.flush()

def later(delay, payload):
    timer = threading.Timer(delay, send, [payload])
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

def span(l0, c0, l1, c1):
    return {"start": {"line": l0, "character": c0}, "end": {"line": l1, "character": c1}}

GOTO = ("textDocument/definition", "textDocument/declaration", "textDocument/typeDefinition",
        "textDocument/implementation")

def answer(method, uri, line, rng):
    row = line + (100000 if rng.random() < 0.3 else 0)
    here = {"uri": uri, "range": span(row, 0, row, 3)}
    elsewhere = {"uri": uri.rsplit("/", 1)[0] + "/missing.qachaos", "range": span(0, 0, 0, 1)}
    if method in GOTO:
        link = {"targetUri": uri, "targetRange": span(row, 0, row, 4), "targetSelectionRange": span(row, 0, row, 1)}
        return rng.choice([here, [here, elsewhere], [], None, [link]])
    if method == "textDocument/references":
        return [here, elsewhere] * rng.randint(0, 40)
    if method == "textDocument/hover":
        size = rng.choice([1, 80, 200000])
        return rng.choice([{"contents": "x" * size},
                           {"contents": {"kind": "markdown", "value": "# chaos\n" + "word " * size}},
                           {"contents": ["plain", {"language": "qachaos", "value": "def chaos"}]}, None])
    if method == "textDocument/completion":
        items = [{"label": "item%d" % n, "textEdit": {"range": span(row, 0, row, 2), "newText": "item%d" % n}}
                 for n in range(rng.choice([0, 3, 2000]))]
        return rng.choice([items, {"isIncomplete": True, "items": items}])
    if method == "textDocument/signatureHelp":
        return {"signatures": [{"label": "f(a, b)", "parameters": [{"label": [2, 3]}, {"label": [99, 120]}]}],
                "activeSignature": rng.choice([0, 7]), "activeParameter": rng.choice([0, 1, 9])}
    if method == "textDocument/codeAction":
        return [{"title": "chaos fix", "edit": {"changes": {uri: [{"range": span(row, 0, row, 1), "newText": "fixed"}]}}}]
    if method == "textDocument/formatting":
        edits = [{"range": span(0, 0, 0, 2), "newText": "def"}]
        if rng.random() < 0.5:
            edits.append({"range": span(0, 1, 1, 0), "newText": "overlap"})
        edits.append({"range": span(row, 0, row + 1, 0), "newText": ""})
        return edits
    if method == "textDocument/documentSymbol":
        return [{"name": "sym", "kind": 12, "range": span(row, 0, row, 3), "selectionRange": span(row, 0, row, 1)}]
    if method == "workspace/symbol":
        return [{"name": "sym%d" % n, "kind": 12, "location": here} for n in range(rng.choice([0, 5, 500]))]
    if method == "textDocument/rename":
        return {"changes": {uri: [{"range": span(row, 0, row, 3), "newText": "renamed"}]}}
    return None

def respond(message):
    method = message.get("method")
    rid = message["id"]
    rng = random.Random("%s|%s" % (method, rid))
    params = message.get("params") or {}
    uri = (params.get("textDocument") or {}).get("uri", "file:///nowhere.qachaos")
    line = (params.get("position") or {}).get("line", 0)
    reply = {"jsonrpc": "2.0", "id": rid, "result": answer(method, uri, line, rng)}
    roll = rng.random()
    if roll < 0.3:
        send(reply)
    elif roll < 0.5:
        later(rng.uniform(0.05, 2.0), reply)
    elif roll < 0.6:
        send(reply)
        later(rng.uniform(0.0, 0.5), reply)
    elif roll < 0.7:
        send({"jsonrpc": "2.0", "id": rid,
              "error": {"code": rng.choice([-32603, -32800, -32801, 1]), "message": "chaos"}})
    elif roll < 0.8:
        send({"jsonrpc": "2.0", "id": rid, "result": rng.choice([42, "text", {"bogus": True}, [1, None, "x"], True])})
    elif roll < 0.85:
        stray = rid + 100000 if isinstance(rid, int) else "%sx" % rid
        send({"jsonrpc": "2.0", "id": stray, "result": reply["result"]})
        send(reply)
    elif roll < 0.9:
        send({"jsonrpc": "2.0", "id": str(rid), "result": reply["result"]})
        later(0.2, reply)
    elif roll < 0.95:
        send({"jsonrpc": "2.0", "id": rid, "result": reply["result"],
              "error": {"code": -32603, "message": "both"}})
    else:
        send({"jsonrpc": "2.0", "id": rid})
        later(0.1, reply)

SERVER_REQUESTS = (
    ("workspace/configuration", {"items": [{"section": "chaos"}]}),
    ("window/workDoneProgress/create", {"token": "chaos"}),
    ("client/registerCapability", {"registrations": [{"id": "chaos", "method": "workspace/didChangeWatchedFiles",
        "registerOptions": {"watchers": [{"globPattern": "**/*.qachaos"}]}}]}),
    ("workspace/workspaceFolders", None),
    ("chaos/unknown", {"any": [1, 2]}),
)

def on_document(uri, version, text):
    rng = random.Random("%s|%s" % (uri, version))
    stale = version - 1 if rng.random() < 0.3 else version
    rows = text.count("\n") + 3
    diagnostics = [{"range": span(rng.randrange(rows), 0, rng.randrange(rows), 400),
                    "severity": rng.choice([1, 2, 3, 4, 7]),
                    "message": rng.choice(["chaos", "", "x" * 5000, "wide 漢字"])}
                   for _ in range(rng.choice([0, 1, 5, 300]))]
    later(rng.uniform(0.0, 1.0), {"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
                                  "params": {"uri": uri, "version": stale, "diagnostics": diagnostics}})
    if rng.random() < 0.2:
        send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
              "params": {"uri": uri.rsplit("/", 1)[0] + "/closed.qachaos", "diagnostics": diagnostics[:3]}})
    if rng.random() < 0.3:
        server_ids[0] += 1
        method, params = rng.choice(SERVER_REQUESTS)
        request = {"jsonrpc": "2.0", "id": server_ids[0], "method": method}
        if params is not None:
            request["params"] = params
        send(request)
    if rng.random() < 0.1:
        send({"jsonrpc": "2.0", "method": rng.choice(["window/logMessage", "$/progress", "chaos/notice"]),
              "params": {"type": 3, "message": "chaos", "token": "chaos", "value": {"kind": "end"}}})

while True:
    message = read()
    if message is None:
        break
    method = message.get("method")
    params = message.get("params") or {}
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": message["id"], "result": {"capabilities": {
            "textDocumentSync": 1, "hoverProvider": True, "definitionProvider": True,
            "declarationProvider": True, "typeDefinitionProvider": True, "implementationProvider": True,
            "referencesProvider": True, "completionProvider": {"triggerCharacters": ["."]},
            "signatureHelpProvider": {"triggerCharacters": ["("]}, "codeActionProvider": True,
            "documentFormattingProvider": True, "documentSymbolProvider": True,
            "workspaceSymbolProvider": True, "renameProvider": True}}})
    elif method in ("shutdown",):
        send({"jsonrpc": "2.0", "id": message["id"], "result": None})
    elif method == "exit":
        break
    elif method == "textDocument/didOpen":
        document = params["textDocument"]
        on_document(document["uri"], document.get("version", 0), document.get("text", ""))
    elif method == "textDocument/didChange":
        document = params["textDocument"]
        on_document(document["uri"], document.get("version", 0), params["contentChanges"][-1].get("text", ""))
    elif "id" in message and method is not None:
        respond(message)
]==]

return ide.plugin({
  id = "qa-chaos-lsp",
  capabilities = { process = { exec = true } },

  setup = function(ctx)
    ctx.lsp.add({
      id = "qa-chaos-lsp",
      language_ids = { "qachaos" },
      command = { "python3", "-c", SERVER },
    })
  end,
})
