-- QA-only: a scripted language server for `.qalsp` files, for tools/qa's
-- `lsp-workspace-edits` flow. clangd renames across files only with an index the
-- fixture does not build, so the host's workspace-edit paths -- a rename reaching
-- a file that is not open, a code action creating and renaming files, a server
-- command answered with workspace/applyEdit -- ran at a fraction in the coverage
-- build (TD-2026-10-18-364). The server is Python, inline: a plugin's directory
-- is not something its Lua can name.
local ide = require("microide")

local SERVER = [==[
import json, os, re, sys
from urllib.parse import quote, unquote

docs = {}
next_id = [1000]

def send(payload):
    body = json.dumps(payload).encode("utf-8")
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()

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

def path_of(uri):
    return unquote(uri[len("file://"):])

def uri_of(path):
    return "file://" + quote(path, safe="/-._~")

def text_of(uri):
    if uri in docs:
        return docs[uri]
    try:
        with open(path_of(uri), encoding="utf-8") as handle:
            return handle.read()
    except OSError:
        return ""

def word_at(text, line, character):
    rows = text.split("\n")
    if line >= len(rows):
        return ""
    for match in re.finditer(r"\w+", rows[line]):
        if match.start() <= character <= match.end():
            return match.group(0)
    return ""

def occurrences(text, word, new_text):
    edits = []
    for number, row in enumerate(text.split("\n")):
        for match in re.finditer(r"\b%s\b" % re.escape(word), row):
            edits.append({"range": {"start": {"line": number, "character": match.start()},
                                    "end": {"line": number, "character": match.end()}},
                          "newText": new_text})
    return edits

while True:
    message = read()
    if message is None:
        break
    method = message.get("method")
    params = message.get("params") or {}
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": message["id"], "result": {"capabilities": {
            "textDocumentSync": 1, "renameProvider": True, "codeActionProvider": True,
            "codeLensProvider": {"resolveProvider": False},
            "executeCommandProvider": {"commands": ["qa.lens"]}}}})
    elif method == "textDocument/didOpen":
        docs[params["textDocument"]["uri"]] = params["textDocument"]["text"]
    elif method == "textDocument/didChange":
        docs[params["textDocument"]["uri"]] = params["contentChanges"][-1]["text"]
    elif method == "textDocument/didClose":
        docs.pop(params["textDocument"]["uri"], None)
    elif method == "textDocument/rename":
        uri = params["textDocument"]["uri"]
        position = params["position"]
        word = word_at(text_of(uri), position["line"], position["character"])
        changes = []
        folder = os.path.dirname(path_of(uri))
        for name in sorted(os.listdir(folder)):
            if name.endswith(".qalsp"):
                target = uri_of(os.path.join(folder, name))
                edits = occurrences(text_of(target), word, params["newName"]) if word else []
                if edits:
                    changes.append({"textDocument": {"uri": target, "version": None}, "edits": edits})
        send({"jsonrpc": "2.0", "id": message["id"], "result": {"documentChanges": changes}})
    elif method == "textDocument/codeAction":
        folder = os.path.dirname(path_of(params["textDocument"]["uri"]))
        created = uri_of(os.path.join(folder, "created.qalsp"))
        send({"jsonrpc": "2.0", "id": message["id"], "result": [{
            "title": "QA: create and rename files", "kind": "refactor",
            "edit": {"documentChanges": [
                {"kind": "create", "uri": created},
                {"textDocument": {"uri": created, "version": None}, "edits": [
                    {"range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}},
                     "newText": "made by the code action\n"}]},
                {"kind": "rename", "oldUri": uri_of(os.path.join(folder, "old.qalsp")),
                 "newUri": uri_of(os.path.join(folder, "renamed.qalsp"))}]}}]})
    elif method == "textDocument/codeLens":
        uri = params["textDocument"]["uri"]
        send({"jsonrpc": "2.0", "id": message["id"], "result": [{
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 1}},
            "command": {"title": "QA lens", "command": "qa.lens", "arguments": [uri]}}]})
    elif method == "workspace/executeCommand":
        uri = (params.get("arguments") or [""])[0]
        next_id[0] += 1
        send({"jsonrpc": "2.0", "id": next_id[0], "method": "workspace/applyEdit", "params": {
            "label": "QA lens", "edit": {"changes": {uri: [
                {"range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}},
                 "newText": "lens ran\n"}]}}}})
        send({"jsonrpc": "2.0", "id": message["id"], "result": None})
    elif method == "shutdown":
        send({"jsonrpc": "2.0", "id": message["id"], "result": None})
    elif method == "exit":
        break
    elif "id" in message and method is not None:
        send({"jsonrpc": "2.0", "id": message["id"], "result": None})
]==]

return ide.plugin({
  id = "qa-lsp",
  capabilities = { process = { exec = true } },

  setup = function(ctx)
    ctx.lsp.add({
      id = "qa-lsp",
      language_ids = { "qalsp" },
      command = { "python3", "-c", SERVER },
    })
  end,
})
