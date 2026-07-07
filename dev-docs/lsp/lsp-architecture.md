# LSP Subsystem Architecture

First-stop map of the Language Server Protocol subsystem. There is no `src/lsp/`
directory — the code lives under `src/workspace/`. This doc reflects the state
after the `lsp-dedup-and-feature-wiring` consolidation (the 1332-line transport
header was split; framing and tracing were extracted).

## Layers

```
WorkspaceShell            thin forwarders (WorkspaceShellLsp.cpp)
   │
   ▼
LspService                per-project glue: doc sync, diagnostics, semantic tokens
   │                      (LspService.{h,cpp})
   ▼
LspManager                one server per language id, aliasing, retiring-clients
   │                      (WorkspaceLspManager.{h,cpp})
   ▼
LspClient                 one JSON-RPC 2.0 client per server subprocess
   │                      (WorkspaceLspClient.{h,cpp} + the Impl TUs below)
   ▼
platform::AsyncSubprocess stdio pipes to the language-server process
```

Language servers themselves are contributed by Lua plugins (`plugins/*-lsp/`) via
`ctx.lsp.add{...}`; the host owns lifecycle, transport, and rendering.

## File map (all under `src/workspace/`)

| File | Role |
|------|------|
| `WorkspaceLspClient.{h,cpp}` | Public `LspClient` interface + wire struct types; `Start`, `DidOpen/DidChange/DidChangeIncremental/DidSave/DidClose`. |
| `WorkspaceLspClientInternal.h` | `LspClient::Impl` declaration: fields, nested `QueuedMessage`/`PendingRequest`, small inline accessors, and the `DispatchResultRequest` template. **Declaration-heavy** — heavy bodies live in the three TUs below. |
| `WorkspaceLspClientTransport.cpp` | I/O thread body (`IoMain`), outbound send/drain (`DrainOutbound`, `SendMessageImmediate`, `SendMessageBuilderAfterInitialize`), `WaitStdoutReadable`. |
| `WorkspaceLspClientDispatch.cpp` | Inbound routing (`DispatchMessage`), server-request replies (`HandleServerRequest`, `SendResponse*`), timeout sweep (`FailPendingRequests`). |
| `WorkspaceLspClientLifecycle.cpp` | Blocking initialize handshake (`DoInitializeBlocking`), shutdown (`DoShutdown`/`BeginShutdown`/`WaitForShutdown`), readiness (`SetProgressReadiness`). |
| `WorkspaceLspClientRequests.cpp` | Interactive request methods (hover/completion/codeAction/formatting/definition/references/rename/documentSymbol/semanticTokens). |
| `LspMessageFraming.{h,cpp}` | `LspMessageFramer`: the `Content-Length` JSON-RPC codec as a pure, unit-tested value type (partial-frame + oversized-skip state). |
| `LspClientTrace.{h,cpp}` | `TraceLspLifecycle` (opt-in via `MICROIDE_TRACE_LSP_LIFECYCLE`) + transport tuning constants (`kLspRequestTimeout`, queue/message/read-buffer caps). |
| `WorkspaceLspManager.{h,cpp}` | `LspManager`: one subprocess per canonical language id (aliases share one), non-blocking retiring-clients drain. |
| `LspService.{h,cpp}` | Per-project doc sync, diagnostics convert+publish, semantic-token request/publish with generation guards, diagnostic shifting on dirty edits, readiness/status strings, the closed-file on-disk edit applier, and the server-initiated `applyEdit` handler. |
| `LspProtocol.{h,cpp}` | JSON ↔ LSP wire mapping (parse/encode helpers), incl. the shared `ParseWorkspaceEdit` (rename / code-action / applyEdit all route through it). |
| `AssistProviderMerge.h` | Pure, unit-tested ranking / de-dup / navigation-choice helpers for the LSP-primary concurrent provider merge. |
| `LspPositionEncoding.{h,cpp}` | Pure byte ↔ code-unit codec (utf-8/16/32). |
| `LspViewportPositions.h` | Viewport-aware wrappers over the codec (the single home for "resolve line in a viewport, then convert its column"). |
| `WorkspaceShellLsp.cpp` | Thin `WorkspaceShell` forwarders → `LspService`; document-symbol → outline adapter. |

## Load-bearing invariants

- **Position encoding is the #1 corruption risk.** The editor stores columns as
  UTF-8 byte offsets; LSP `character` offsets are in the server's negotiated
  encoding. Every position crossing a non-ASCII codepoint must convert through
  `LspViewportPositions.h` / `lsp_encoding::*`. Do **not** re-implement this
  conversion inline — a skew between copies corrupts non-ASCII lines. The
  per-keystroke incremental sync only sends ranged edits when the server
  negotiated utf-8; otherwise it falls back to a full-document replace (no
  per-column re-encoding needed).
- **didOpen carries current text.** `ResolveOpenDocumentForSync` captures
  `was_open` *before* opening, because a fresh `didOpen` sends the already-edited
  buffer; a following `didChange` would then double-apply and desync the server.
- **Diagnostics shift on dirty edits** (`ShiftLspDiagnostics*`) run *before* the
  client early-out, so stored diagnostics stay aligned to their text even when no
  server serves the buffer, until the server republishes authoritative ranges.
- **Semantic-token responses are generation-guarded.** Each request bumps a
  per-URI generation; a response whose captured generation is stale (the buffer
  changed) is dropped in `PublishLspSemanticTokens` rather than painting stale
  colors. The absolute-positioned overlay is cleared on any edit.
- **Non-blocking didOpen on tab activate.** `WorkspaceTabCoordinator::Activate`
  must not call `DidOpen/DidChange/EnsureLspDocumentOpen` synchronously
  (enforced by `CheckLspDidOpenIsNonBlocking`).
- **Do not rename** `SendMessageImmediate` (a `tsan.supp` entry targets it by
  mangled symbol) or the five didOpen-guard methods scanned by
  `CheckLspDidOpenIsNonBlocking`.
- **Server-initiated `workspace/applyEdit` is wired.** The client advertises
  `workspace.applyEdit=true`; `HandleServerRequest` parses the edit, posts it to the
  main thread (buffer/disk mutation must not run on the I/O thread), applies it via
  the bound `apply_edit_handler`, then replies with the real `applied` flag. The
  handler (`LspService::ApplyServerWorkspaceEdit`) edits open buffers in place and
  writes closed files silently on disk — the same split as client-initiated rename.
- **Provider precedence is LSP-primary concurrent merge** (`AssistProviderMerge.h`).
  Completion / code actions / go-to-definition / find-references fire the plugin
  worker and the language server *at the same time*, then merge: list overlays
  publish a ranked, de-duplicated union (LSP-first for served languages) as each
  source arrives; go-to-definition waits for the authoritative server and falls back
  to the plugin only when the server returns empty. Never serial plugin-first.
- **Rename across unopened files applies silently on disk** (VSCode-style). The
  closed-file applier (`LspService::ApplyLspEditsToClosedFilesOnDisk`) loads each
  file into a throwaway `TextViewport` (reusing line-ending / BOM / encoding
  detection + the atomic, permission-preserving save), applies the encoding-mapped
  edits, and saves — no tab is opened. Open buffers still edit in place via
  `WorkspaceShell::ApplyLspWorkspaceEdit`. A confirmation prompt gates the write.

## Transport model

One dedicated I/O thread per server (`IoMain`) blocks in `poll()` over stdout + a
self-pipe wakeup, so an idle server makes no fixed-cadence wakeups. Outbound work
writes a wake byte to break the poll. Responses are matched to a pending-request
id map and marshalled to the main thread via `MainThreadMailbox`; the main loop
drains them each frame (`DrainCallbacks`). Every request carries a deadline so a
silent server never strands a request (and its UI loading state). Hostile-input
backstops: bounded message/read-buffer sizes with oversized-frame skip-and-resync
(`LspMessageFramer`), and a bounded outbound queue that refuses on a wedged server.

## Testing

- `LspProtocolTests` / `LspPositionEncodingTests` — wire codec + encoding units.
- `WorkspaceLspClientTests` — lifecycle/shutdown races, stub-mode request
  round-trips, and direct `LspMessageFramer` framing units.
- `AssistServiceTests` — pure provider-merge units (`RankedUnion` ordering/de-dup,
  `ChooseNavigation` LSP-wins/wait/fallback) plus the stale-result guard.
- `WorkspaceShellPluginTests` — dual-source completion merge (LSP-first + de-dup),
  silent on-disk rename (closed file written, no tab), and server-initiated
  `workspace/applyEdit` (open buffer + closed file) via the simulate-request hook.
- `Phase5Tests` / `WorkspaceShellPluginTests` — end-to-end through a Python
  fake server (diagnostics, hover, outline, format, rename, semantic overlay).
- `LspRealServerE2ETests` — **opt-in** end-to-end against real clangd (skips when
  clangd is absent). Set `MICROIDE_TEST_LSP_CLANGD` to force a binary. This is the
  only coverage that exercises a production server's real negotiation/payloads.
