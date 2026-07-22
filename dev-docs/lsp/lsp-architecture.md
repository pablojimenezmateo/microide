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
| `WorkspaceLspClientRequests.cpp` | Interactive request methods (hover/completion/signatureHelp/codeAction/formatting/rangeFormatting/definition/typeDefinition/implementation/declaration/references/prepareRename/rename/documentSymbol/workspaceSymbol/semanticTokens/inlayHint). definition + the three sibling navigations share one templated `DispatchLocationRequest`. |
| `editor/InlayHintColumns.{h,cpp}` | Pure per-row inlay-hint grid displacement (**under `src/editor/`, not `src/workspace/`**): `InlayRowDisplacement` (cells-before / next-anchor / hit-test inverse), `BuildInlayRowSpans` (line decorations → row-local spans), and `RealVisualColumnForDisplayColumn` (the mouse inverse). |
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
- **WorkspaceEdit resource ops + versioned edits are supported (TD-2026-07-17-011).**
  The client advertises `workspace.workspaceEdit = {documentChanges,
  resourceOperations: [create, rename, delete], failureHandling:
  textOnlyTransactional}`. `ParseWorkspaceEdit` keeps the ops in array order
  (re-keying pre-rename text edits to their post-rename URI) and records versioned
  TextDocumentEdits in `expected_versions`. Apply order everywhere: version gate
  (a stale tracked-document version fails the whole edit —
  `LspService::WorkspaceEditVersionsCurrent` over
  `LspClient::TrackedDocumentVersion`) → resource ops
  (`LspService::ApplyWorkspaceResourceOps`: validate-first against a simulated
  overlay, project-root containment, staged deletes/overwrites with rollback on
  mid-flight failure, tab/diagnostic reconcile via
  `PathMutationCoordinator::ReconcileAfterExternal{Rename,Delete}`) → text edits.
  Ops-carrying code actions route through `LspService::ApplyFullWorkspaceEdit`
  (ops, then open buffers, then closed files on disk); ops-carrying renames go
  through the confirm prompt with the ops stashed in `PendingRenameSave`.
- **Provider precedence is LSP-primary concurrent merge** (`AssistProviderMerge.h`).
  Completion / code actions / go-to-definition / find-references fire the plugin
  worker and the language server *at the same time*, then merge: list overlays
  publish a ranked, de-duplicated union (LSP-first for served languages) as each
  source arrives; go-to-definition waits for the authoritative server and falls back
  to the plugin only when the server returns empty. Never serial plugin-first.
- **Signature help / navigation are LSP-primary too.** `textDocument/signatureHelp`
  is fired concurrently with the plugin provider and chosen LSP-first (via
  `ChooseNavigation`) into the caret-anchored popup. `typeDefinition` /
  `implementation` / `declaration` reuse definition's `ParseLocations` +
  `NavigateToLspLocation` (LSP-only, single-source). `workspace/symbol` is a
  query-driven command (`workspace-symbol <query>`) that renders navigable results
  into the `lsp.workspaceSymbols` output channel — no picker overlay.
- **Inlay hints render as mid-line virtual text.** `textDocument/inlayHint` is
  requested for the whole document on the same pull triggers as semantic tokens
  (didOpen, save, and clean-landing undo/redo — never per keystroke) and gated on
  `editor.inlay_hints.enabled` + the server's `inlayHintProvider`. Results publish
  as `lsp:inlay` `InlineTextDecoration`s anchored at the hint's byte column (LSP
  padding baked into the label text). The editor grid renders them mid-line via a
  pure per-row displacement primitive (`editor/InlayHintColumns.h`): every
  column→x consumer on the row (text runs — split + shifted at each anchor — plus
  selection/search/bracket fills, changed-span/diagnostic underlines, whitespace,
  both carets, and the end-of-line anchor) adds the phantom-cell shift, and the
  three editor click hit-tests invert it so clicks land on the intended glyph. The
  displacement is generation-guarded like semantic tokens and identity (≈zero
  cost) on a row with no hints. **v1 limitation:** hints are suppressed on
  soft-wrapped lines (cross-wrap-row displacement is out of scope).
- **prepareRename refines the rename prompt.** The prompt opens instantly with the
  heuristic identifier seed; `textDocument/prepareRename` then (async, best-effort,
  gated on the server's `renameProvider.prepareProvider`) prefills the server
  placeholder — only while the user has not typed over the seed — or dismisses the
  prompt for a non-renameable position. Range formatting (`textDocument/
  rangeFormatting`) formats the selection when there is one, else the whole document.
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

- **Serialization is deferred to the I/O thread.** `SendMessageAfterInitialize`
  moves the message into the outbound builder (`QueuedMessage::build_serialized`),
  so `SerializeMessage` (JSON + Content-Length production) runs in `DrainOutbound`
  on the I/O thread, not on the calling (usually UI) thread. `DidChange`/
  `DidChangeIncremental` likewise capture the buffer into the builder (version bumped
  eagerly under `mutex` to keep ordering monotonic), so the whole-document copy of a
  full-sync edit also lands off the UI thread. Order is fixed at enqueue time under
  `send_mutex`, so lazy serialization never reorders. The shutdown/initialize path
  (`SendMessageImmediate`) stays eager — it runs before the I/O thread exists or as
  teardown drains it, and its bounded `write_mutex` timeout is load-bearing. Measured
  win (`microide_lsp_serialize_bench` on the reference workstation): a whole-document
  full-sync `didChange` serialization is ≈245µs at 5k lines / ≈1.04ms at 20k lines —
  that per-keystroke cost is now off the UI thread for utf-16 servers (utf-8 servers
  use ranged incremental sync, so the payload is tiny either way).

## Testing

- `LspProtocolTests` / `LspPositionEncodingTests` — wire codec + encoding units.
- `WorkspaceLspClientTests` — lifecycle/shutdown races, stub-mode request
  round-trips, direct `LspMessageFramer` framing units, the deferred-serialization
  FIFO-order + full-payload guard (`DidChangePreservesOrderAndPayload`, against a
  real Python server), and the real (non-stub) completion-parse path
  (`CompletionParsesJsonResult` — the stub path bypasses the JSON parser, so this is
  the only unit coverage of the move-out completion parser).
- `JsonValueTests` — the additive `MutableAt`/`MutableArray`/`MutableString` accessors
  (presence/type guards + move-out) the completion/code-action parsers use to move
  result strings out instead of copying them on the main thread.
- `microide_lsp_serialize_bench` — standalone microbench (not baseline-gated) for the
  per-keystroke `didChange` serialization cost moved off the UI thread by Track A.
- `AssistServiceTests` — pure provider-merge units (`RankedUnion` ordering/de-dup,
  `ChooseNavigation` LSP-wins/wait/fallback) plus the stale-result guard.
- `LspProtocolTests` also covers the newer parsers: `ParseSignatureHelp` (string +
  `[start,end]` offset labels), `ParsePrepareRename` (all wire shapes incl. null),
  `ParseTextEdits`, and `ParseWorkspaceSymbols`.
- `InlayHintColumnsTests` — the pure per-row displacement primitive (cells-before,
  next-anchor, and the hit-test inverse incl. phantom-region snapping).
- `RowDecorationBuilderTests` — a mid-line inlay hint splits + shifts the real runs
  and draws its glyph in the reserved phantom cells (grid-exact geometry).
- `WorkspaceShellPluginTests` also covers the inlay end-to-end: a stub server's
  `textDocument/inlayHint` publishes an `lsp:inlay` mid-line decoration at the right
  byte column with padding baked into the label.
- Not yet implemented (deferred): automatic document highlight
  (`textDocument/documentHighlight` — the editor already ships automatic *lexical*
  occurrence highlighting), on-type formatting, and semantic tokens range/delta.
  **Semantic range/delta was profiled and rejected**, not merely deferred: the
  client-side parse+publish of a whole-document `full` response is sub-millisecond
  and one-shot (≈126µs at 30k tokens / ≈782µs at 100k tokens on the reference
  workstation) and fires only on didOpen/save/clean-undo — never during typing —
  so it is never a bottleneck. `delta`/`range` would only shrink the server
  recompute + wire round-trip (out of process) at that low frequency, while forcing
  partial-overlay reconciliation against today's atomic full-replace model — real
  new desync risk for a bounded, off-hot-path win. Revisit only if a concrete
  very-large-file profile shows the server round-trip dominating interactive
  latency.
- `WorkspaceShellPluginTests` — dual-source completion merge (LSP-first + de-dup),
  silent on-disk rename (closed file written, no tab), and server-initiated
  `workspace/applyEdit` (open buffer + closed file) via the simulate-request hook —
  incl. the resource-op batch (create+fill / rename-with-tab-retarget / delete /
  no staging residue), validate-first atomicity + root containment, and the
  versioned-edit gate (`ServerApplyEdit*` tests); the op/version parsing itself is
  covered by `LspProtocol/ParsesWorkspaceEditResourceOps`.
- `Phase5Tests` / `WorkspaceShellPluginTests` — end-to-end through a Python
  fake server (diagnostics, hover, outline, format, rename, semantic overlay).
- `LspRealServerE2ETests` — **opt-in** end-to-end against real clangd (skips when
  clangd is absent). Set `MICROIDE_TEST_LSP_CLANGD` to force a binary. This is the
  only coverage that exercises a production server's real negotiation/payloads.
