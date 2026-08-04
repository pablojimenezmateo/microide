# LSP Subsystem Architecture

First-stop map of the Language Server Protocol subsystem. The code lives under
`src/workspace/lsp/` (there is no top-level `src/lsp/`); the four entries that
live elsewhere are marked in the file map below. This doc reflects the state after the
`lsp-dedup-and-feature-wiring` consolidation (the 1332-line transport header was
split; framing and tracing were extracted) and the 2026-08-03 `src/workspace`
subsystem split, which moved these files from `src/workspace/` into
`src/workspace/lsp/`.

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

## File map (all under `src/workspace/lsp/` unless marked otherwise)

| File | Role |
|------|------|
| `WorkspaceLspClient.{h,cpp}` | Public `LspClient` interface + wire struct types; `Start`, `DidOpen/DidChange/DidChangeIncremental/DidSave/DidClose`. |
| `WorkspaceLspClientInternal.h` | `LspClient::Impl` declaration: fields, nested `QueuedMessage`/`PendingRequest`, small inline accessors, and the `DispatchResultRequest` template. **Declaration-heavy** — heavy bodies live in the three TUs below. |
| `WorkspaceLspClientTransport.cpp` | I/O thread body (`IoMain`), outbound send/drain (`DrainOutbound`, `SendMessageImmediate`, `SendMessageBuilderAfterInitialize`), `WaitStdoutReadable`. |
| `WorkspaceLspClientDispatch.cpp` | Inbound routing (`DispatchMessage`), server-request replies (`HandleServerRequest`, `SendResponse*`), timeout sweep (`FailPendingRequests`). |
| `WorkspaceLspClientLifecycle.cpp` | Blocking initialize handshake (`DoInitializeBlocking`), shutdown (`DoShutdown`/`BeginShutdown`/`WaitForShutdown`), readiness (`SetProgressReadiness`). |
| `WorkspaceLspClientRequests.cpp` | Interactive request methods (hover/completion/signatureHelp/codeAction/formatting/rangeFormatting/definition/typeDefinition/implementation/declaration/references/prepareRename/rename/documentSymbol/workspaceSymbol/semanticTokens/inlayHint/documentHighlight/codeLens/executeCommand/callHierarchy). definition + the three sibling navigations share one templated `DispatchLocationRequest`. |
| `editor/InlayHintColumns.{h,cpp}` | Pure per-row inlay-hint grid displacement (**under `src/editor/`, not `src/workspace/`**): `InlayRowDisplacement` (cells-before / next-anchor / hit-test inverse), `BuildInlayRowSpans` (line decorations → row-local spans), and `RealVisualColumnForDisplayColumn` (the mouse inverse). |
| `JsonRpcMessageFraming.{h,cpp}` | **Under `src/workspace/`** (shared with DAP, which speaks the same stdio JSON-RPC transport). `JsonRpcMessageFramer`: the `Content-Length` codec as a pure, unit-tested value type (partial-frame + oversized-skip state). Fuzzed by `JsonRpcMessageFramingFuzz`. |
| `LspClientTrace.{h,cpp}` | `TraceLspLifecycle` (opt-in via `MICROIDE_TRACE_LSP_LIFECYCLE`) + transport tuning constants (`kLspRequestTimeout`, queue/message/read-buffer caps). |
| `WorkspaceLspManager.{h,cpp}` | `LspManager`: one subprocess per canonical language id (aliases share one), non-blocking retiring-clients drain. |
| `LspService.{h,cpp}` | Per-project doc sync, diagnostics convert+publish, semantic-token request/publish with generation guards, diagnostic shifting on dirty edits, readiness/status strings, the closed-file on-disk edit applier, and the server-initiated `applyEdit` handler. |
| `LspFileWatchRegistry.{h,cpp}` | `workspace/didChangeWatchedFiles`: the set of glob patterns servers have registered interest in, plus the per-file match. Patterns are split into relative-to-base and absolute buckets **at registration time**, so the per-changed-file loop does one comparison per pattern instead of re-deciding the pattern's kind every time. |
| `LspProtocol.{h,cpp}` | JSON ↔ LSP wire mapping (parse/encode helpers), incl. the shared `ParseWorkspaceEdit` (rename / code-action / applyEdit all route through it). |
| `AssistProviderMerge.h` | **Under `src/workspace/`.** Pure, unit-tested ranking / de-dup / navigation-choice helpers for the LSP-primary concurrent provider merge. |
| `LspPositionEncoding.{h,cpp}` | Pure byte ↔ code-unit codec (utf-8/16/32). |
| `LspViewportPositions.h` | Viewport-aware wrappers over the codec (the single home for "resolve line in a viewport, then convert its column"). |
| `WorkspaceShellLsp.cpp` | **Under `src/workspace/shell/`.** Thin `WorkspaceShell` forwarders → `LspService`; document-symbol → outline adapter. |

## Load-bearing invariants

- **Position encoding is the #1 corruption risk.** The editor stores columns as
  UTF-8 byte offsets; LSP `character` offsets are in the server's negotiated
  encoding. Every position crossing a non-ASCII codepoint must convert through
  `LspViewportPositions.h` / `lsp_encoding::*`. Do **not** re-implement this
  conversion inline — a skew between copies corrupts non-ASCII lines. The
  per-keystroke incremental sync only sends ranged edits when the server
  negotiated utf-8; otherwise it falls back to a full-document replace (no
  per-column re-encoding needed).
- **Watched-file notifications are registration-driven and capped.** Servers
  subscribe through `client/registerCapability` for
  `workspace/didChangeWatchedFiles` (and drop it through
  `unregisterCapability`); `LspFileWatchRegistry` answers "does this changed path
  interest this server". Registrations are bounded
  (`kMaxLspFileWatchRegistrations` = 64, `kMaxLspFileWatchPatternsPerRegistration`
  = 128) because the list is server-controlled input and the match runs on the
  shell thread for every file in a change batch. A server that registers past the
  cap is served its first N patterns rather than being allowed to make project
  changes O(server's appetite). `RelativePattern` bases and the `WatchKind`
  bitmask (1=Create, 2=Change, 4=Delete; absent means all three) are honoured, so
  a server that asked only about deletions is not woken for edits.
- **Callbacks carry an outcome, not just an optional.** Every async `LspClient`
  request callback takes `LspResult<T>` = `{ LspRequestOutcome outcome;
  std::optional<T> value; }`, computed once in `Impl::DispatchResultRequest`.
  Outcomes: `kOk`, `kEmpty` (server answered null/absent), `kTimeout` (deadline
  sweep), `kUnavailable` (server gone / send failed), `kProtocolError`. Before
  this, a timed-out go-to-definition was shaped identically to "no definition
  here", so the UI said **"No definition found"** for a transport failure. Any
  caller emitting a "No X found" message MUST gate it on `answered()`
  (`kOk || kEmpty`) and surface "Language server did not respond" otherwise —
  this applies across definition / references / type-def / impl / declaration /
  workspace-symbol / rename / formatting in `AssistService`. Test seam:
  `LspClient::SetRequestTimeoutForTesting(ms)`.
- **The transport is SHARED with DAP — fix bugs once.** Both clients speak the
  same stdio JSON-RPC wire protocol and used to carry independent copies that
  drifted, which shipped a bug: the LSP framer parsed `Content-Length`
  tolerantly (case-insensitive, optional whitespace) with a test pinning it,
  while the DAP copy required a byte-exact `Content-Length: ` prefix — so an
  adapter writing `content-length:` had its header dropped, the parser resynced
  by reading the JSON body as headers, and the session tore down. Now shared:
  `JsonRpcMessageFraming.{h,cpp}` (the codec, with a per-client
  `max_message_bytes` ceiling) and `util::WakePipe::PollReadableOrWake`, which
  carries the load-bearing "re-fetch the stdout fd every poll, never trust the
  cached number" rule. **Still parallel, so check both when touching either:**
  shutdown sequencing, `SendMessageImmediate`'s bounded `write_mutex`
  acquisition, the outbound queue + byte budget, `FailPendingRequests`, the
  initialize handshake deadline, and `ResetProtocolState`.
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
- **Call hierarchy is a command, not a tree view.** `call-hierarchy
  [incoming|outgoing]` (default incoming — "who calls this?" is the question that
  gets asked) chains `textDocument/prepareCallHierarchy` →
  `callHierarchy/{incoming,outgoing}Calls`, one level deep, and renders into the
  `lsp.callHierarchy` output channel through the same `EmitReferenceEntry`
  formatter as find-references, so every row is a navigable `file:line:col` with
  context. One level rather than an expandable tree: the shell has no tree surface
  for it, and a flat list is what a channel can express honestly. The item object
  round-trips verbatim between the two hops (servers correlate through its `data`).
  Incoming calls navigate to the **call sites inside each caller** (`fromRanges`) —
  that is what the user is looking for — while outgoing calls navigate to each
  callee's own name (`selectionRange`). Both hops carry one generation token, so a
  second invocation mid-chain cannot let the older chain render last. A server
  without `callHierarchyProvider` says so instead of reporting "no callers".
- **Code lenses reuse the plugin decoration surface.** `textDocument/codeLens` is
  pulled on the same triggers as inlay hints (didOpen, save, clean-landing
  undo/redo) and gated on `lsp.code_lens.enabled` + the server's `codeLensProvider`.
  Lenses the first response leaves title-less are filled in through
  `codeLens/resolve` — the only way rust-analyzer and typescript-language-server
  deliver theirs — with the ORIGINAL lens object round-tripped verbatim (servers
  correlate through its private `data` field) and the original range kept (a
  resolve reply may echo a default one). The document publishes once, after the last
  resolve lands; the fan-out is bounded at 256 lenses per pull. Results become
  `lsp:codelens` `CodeLensDecoration`s, so end-of-line rendering, above-line strips
  (`plugins.code_lens_above`), sorting, per-row slicing and hit-testing are all the
  existing plugin machinery. A server command plus JSON arguments cannot be spelled
  as a host command name, so the decoration carries an opaque `payload` handle into
  an `LspService`-owned table instead; a click resolves it and runs
  `workspace/executeCommand`. The table is rebuilt per publish (each publish drops
  the URI's previous entries), and a handle from a superseded publish is inert.
- **Occurrence highlighting is semantic when a server can answer.**
  `textDocument/documentHighlight` is requested for the caret's position from
  `WorkspaceShell::OnFramePresented` — the one point every caret-moving path (mouse,
  keyboard, search, go-to-definition, folding) converges on — and only when the
  request's identity (path + content revision + caret line/column) actually changed,
  so a resting caret costs four integer compares per frame. It is suppressed while
  the caret is being driven by active typing, exactly where the render path already
  suppresses occurrence highlighting. Gated on `lsp.document_highlight.enabled` plus
  the existing `editor.occurrences.enabled`, and short-circuited without a
  round-trip when the server advertises no `documentHighlightProvider`.
  Responses land in `ProjectWorkspaceState::semantic_occurrences` as
  `editor::OccurrenceRange`s (byte columns, sorted by line), and
  `RenderViewModelBuilder` **replaces** its own textual word scan with them while
  the set is valid — same file, same content revision, caret still inside one of the
  ranges (VS Code's rule, so arrowing through a word does not blink the highlight
  off between round-trips). Everything else falls back to the word scan: unserved
  languages, servers without the provider, and the window between a caret move and
  its answer. `DocumentHighlightKind::Write` paints with the strong tint so an
  assignment stands out from the reads around it. A content edit invalidates the set
  by revision alone — nothing has to clear it.
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

### Stub-mode responses go through the main-thread mailbox

`LspClient::EnableTestStubMode()` + `SetTest*Handler` do **not** invoke the
handler inline. `DispatchTestStub` marshals through the same main-thread mailbox
a real response uses, so the handler only runs on `DrainCallbacks()` /
`WorkspaceShellTestAccess::ConsumeLspCallbacks(shell)`.

So a **chained** request — one issued from inside another's callback — needs one
drain per hop (`codeLens` → `codeLens/resolve`; `prepareCallHierarchy` →
`callHierarchy/{incoming,outgoing}Calls`). A single `ConsumeLspCallbacks` after a
two-hop chain leaves the second response in the mailbox, and the symptom is
indistinguishable from "the second request was never sent": the stub handler
simply never ran.

To get two responses **in flight at once**, park the callbacks (a handler that
pushes `cb` into a vector instead of calling it) and answer them out of order.
Driving a second pull through the save path does not work from a test, which is
why `WorkspaceShellTestAccess::RequestCodeLensesForActiveBuffer` exists.

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
- `LspResourceOpsTests` — `LspService::ApplyWorkspaceResourceOps` in isolation
  (no shell, no server): apply order, the simulated-existence overlay
  (create-then-rename, delete-then-create), ignore-option no-ops, `overwrite`
  beating `ignoreIfExists`, project-root containment (incl. `..` traversal),
  non-recursive directory-delete refusal, staged-backup disposal, and the
  rollback journal — a mid-batch I/O failure restores a staged delete
  byte-identically. Also pins that the tab/diagnostic reconcile hooks fire in
  APPLY order rather than grouped by op kind.
- `Phase5Tests` / `WorkspaceShellPluginTests` — end-to-end through a Python
  fake server (diagnostics, hover, outline, format, rename, semantic overlay).
- `LspRealServerE2ETests` — **opt-in** end-to-end against real clangd (skips when
  clangd is absent). Set `MICROIDE_TEST_LSP_CLANGD` to force a binary. This is the
  only coverage that exercises a production server's real negotiation/payloads.
