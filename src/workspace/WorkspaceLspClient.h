#pragma once

#include "platform/AsyncSubprocess.h"
#include "util/JsonValue.h"

#include <SDL3/SDL.h>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace microide::workspace {

struct LspReadinessSnapshot {
  enum class State {
    Idle,
    Starting,
    Indexing,
    Ready,
    Failed,
  };

  State state = State::Idle;
  std::string message;
  int indexed_count = 0;
};

// Single LSP server connection with JSON-RPC 2.0.
// All request methods are asynchronous: they return immediately and deliver
// results via callbacks dispatched on the main thread through DrainCallbacks().
// Call SetWakeEventType() once before use so the reader thread can wake the
// main event loop when responses are ready.
class LspClient {
 public:
  using ReadinessSnapshot = LspReadinessSnapshot;

  struct Position {
    int line = 0;
    int character = 0;
  };

  struct Range {
    Position start;
    Position end;
  };

  struct Location {
    std::string uri;
    Range range;
  };

  struct Diagnostic {
    Range range;
    std::string message;
    int severity = 1;  // 1=Error, 2=Warning, 3=Info, 4=Hint
    std::string code;
  };

  struct CompletionItem {
    std::string label;
    int kind = 1;  // Text=1, Method=2, Function=3, ...
    std::string detail;
    std::string documentation;
    std::string insert_text;
    int sort_text_priority = 0;
    // LSP InsertTextFormat: 1=PlainText, 2=Snippet
    int insert_text_format = 1;
    // Server-provided replacement range (0-based, in the server's negotiated
    // position encoding), parsed from `textEdit.range` or `insertReplaceEdit`.
    // When present it is authoritative: the client replaces exactly this range
    // instead of a heuristic word range, so member/path completions (obj.| , a/b|)
    // extend the qualifier rather than overwriting it. Absent for servers/items
    // that only return `insertText`.
    std::optional<Range> replace_range;
  };

  struct WorkspaceEdit {
    // Map from URI to list of text edits.
    std::unordered_map<std::string, std::vector<std::pair<Range, std::string>>> changes;
  };

  struct CodeAction {
    std::string title;
    std::string command;
    std::vector<util::JsonValue> arguments;
    // Inline WorkspaceEdit carried by the action (clangd delivers quickfixes,
    // e.g. "remove unused #include", this way). Applied directly to open buffers
    // rather than via a server command. `has_edit` distinguishes an empty edit
    // from an absent one.
    WorkspaceEdit edit;
    bool has_edit = false;
  };

  struct DocumentSymbol {
    std::string name;
    std::string detail;
    int kind = 1;
    Range range{};
    Range selection_range{};
    std::vector<DocumentSymbol> children;
  };

  // One `workspace/symbol` result: a project-wide symbol with its location.
  struct WorkspaceSymbol {
    std::string name;
    std::string container_name;
    int kind = 1;
    Location location;
  };

  struct SignatureParameter {
    std::string label;
    std::string documentation;
  };

  struct SignatureInformation {
    std::string label;
    std::string documentation;
    std::vector<SignatureParameter> parameters;
    // Per-signature active parameter (LSP 3.16+ signatureInformation.activeParameter);
    // -1 when the signature does not override the top-level activeParameter.
    int active_parameter = -1;
  };

  struct SignatureHelp {
    std::vector<SignatureInformation> signatures;
    int active_signature = 0;
    int active_parameter = 0;
  };

  // One decoded semantic token: an absolute (line, start_char, length) range plus
  // the server-legend token-type index (the deltas in `data` are pre-resolved).
  struct SemanticToken {
    int line = 0;
    int start_char = 0;
    int length = 0;
    int token_type = 0;
  };

  // One textDocument/inlayHint result: inline virtual text at `position`. `label`
  // is flattened from the string-or-parts wire shape; `kind` is 1=Type, 2=Parameter,
  // 0=unspecified; padding_left/right request a space of separation on that side.
  struct InlayHint {
    Position position;
    std::string label;
    int kind = 0;
    bool padding_left = false;
    bool padding_right = false;
  };

  using OnPublishDiagnostics = std::function<void(std::string uri, std::vector<Diagnostic>)>;

  // One text edit: a document range plus its replacement text (server encoding).
  using TextEdit = std::pair<Range, std::string>;

  // Async callback types — called on the main thread from DrainCallbacks().
  using HoverCallback = std::function<void(std::optional<util::JsonValue>)>;
  using CompletionCallback = std::function<void(std::optional<std::vector<CompletionItem>>)>;
  using CodeActionCallback = std::function<void(std::optional<std::vector<CodeAction>>)>;
  // Formatting returns the full TextEdit[] (whole-document reformats commonly come
  // back as many edits); the caller applies them together.
  using FormattingCallback = std::function<void(std::optional<std::vector<TextEdit>>)>;
  using LocationCallback = std::function<void(std::optional<std::vector<Location>>)>;
  using RenameCallback = std::function<void(std::optional<WorkspaceEdit>)>;
  using DocumentSymbolCallback =
      std::function<void(std::optional<std::vector<DocumentSymbol>>)>;
  using SemanticTokensCallback =
      std::function<void(std::optional<std::vector<SemanticToken>>)>;
  using InlayHintCallback = std::function<void(std::optional<std::vector<InlayHint>>)>;
  using SignatureHelpCallback = std::function<void(std::optional<SignatureHelp>)>;
  using WorkspaceSymbolCallback =
      std::function<void(std::optional<std::vector<WorkspaceSymbol>>)>;

  LspClient();
  ~LspClient();
  LspClient(const LspClient&) = delete;
  LspClient& operator=(const LspClient&) = delete;

  // Set the SDL custom event type used to wake the main event loop when
  // responses are ready. Call before Start().
  void SetWakeEventType(Uint32 event_type);

  // Start the server and begin asynchronous initialization.
  // initialization_options is forwarded verbatim as the LSP `initializationOptions`;
  // settings answers server-initiated `workspace/configuration` requests and is
  // pushed once via `workspace/didChangeConfiguration` after initialize. Both
  // should be JSON objects (or Null to omit).
  bool Start(const std::vector<std::string>& command, const std::string& root_uri,
             const std::string& language_id, const std::string& cwd = {},
             const util::JsonValue& initialization_options = {},
             const util::JsonValue& settings = {},
             const platform::SubprocessSandbox& sandbox = {});

  // True while the server process is running.
  bool IsRunning() const;

  // True when the server is in the process of initializing.
  bool IsInitializing() const;

  // True when the server has been fully initialized.
  bool IsInitialized() const;

  // True when the server negotiated incremental document sync.
  bool SupportsIncrementalSync() const;

  // The LSP position encoding the server negotiated from our advertised
  // [utf-8, utf-16] list ("utf-8" when honored; "utf-16" by spec default when the
  // server reports none). "utf-8" means the editor's byte-offset columns are exact
  // LSP positions with no conversion required.
  std::string ServerPositionEncoding() const;

  // True when the client currently tracks an open document for uri.
  bool HasOpenDocument(const std::string& uri) const;

  // Last startup/runtime error message captured by the LSP client.
  const std::string& LastError() const;

  // Current startup/readiness state for the server.
  ReadinessSnapshot GetReadinessSnapshot() const;

  // Set callback for publishDiagnostics notifications (called on main thread).
  void SetDiagnosticsCallback(OnPublishDiagnostics callback);

  // True once a diagnostics callback has been installed.
  bool HasDiagnosticsCallback() const;

  // Install the handler for server-initiated `workspace/applyEdit`. Set from the
  // main thread; invoked on the main thread (it mutates buffers / writes files)
  // and returns whether the edit was applied. Advertising the capability without a
  // handler still replies applied:false. HasApplyEditHandler binds it once.
  void SetApplyEditHandler(std::function<bool(WorkspaceEdit)> handler);
  bool HasApplyEditHandler() const;

  // Call from the main thread each frame to dispatch pending callbacks.
  void DrainCallbacks();

  // Send textDocument/didOpen.
  bool DidOpen(std::string uri, std::string language_id, std::string text);

  // Send textDocument/didChange (incremental or full depending on server capability).
  bool DidChange(const std::string& uri, const std::string& text);

  // Send textDocument/didChange with explicit incremental edits.
  bool DidChangeIncremental(const std::string& uri,
                            Range changed_range,
                            const std::string& new_text);

  // Send textDocument/didSave.
  bool DidSave(const std::string& uri);

  // Send textDocument/didClose.
  bool DidClose(const std::string& uri);

  // Async textDocument/hover.
  void RequestHoverAsync(std::string uri, Position pos, HoverCallback callback);

  // Async textDocument/completion.
  void RequestCompletionAsync(std::string uri, Position pos, CompletionCallback callback);

  // Async textDocument/signatureHelp. nullopt when the server has no result.
  void RequestSignatureHelpAsync(std::string uri, Position pos, SignatureHelpCallback callback);

  // Async textDocument/codeAction. `context_diagnostics` populates the request
  // `context.diagnostics`; clangd only returns quickfixes for diagnostics passed
  // here (it matches them by range + message).
  void RequestCodeActionAsync(std::string uri, Range range,
                              std::vector<Diagnostic> context_diagnostics,
                              CodeActionCallback callback);

  // Async textDocument/formatting.
  void RequestFormattingAsync(std::string uri, int tab_size, bool insert_spaces,
                               FormattingCallback callback);

  // Async textDocument/rangeFormatting — format only `range` (a selection). Same
  // TextEdit[] result shape as whole-document formatting.
  void RequestRangeFormattingAsync(std::string uri, Range range, int tab_size,
                                   bool insert_spaces, FormattingCallback callback);

  // Async textDocument/definition.
  void RequestGoToDefinitionAsync(std::string uri, Position pos, LocationCallback callback);

  // Async textDocument/typeDefinition, /implementation, /declaration. Same
  // TextDocumentPositionParams request and Location[] result shape as definition.
  void RequestGoToTypeDefinitionAsync(std::string uri, Position pos, LocationCallback callback);
  void RequestGoToImplementationAsync(std::string uri, Position pos, LocationCallback callback);
  void RequestGoToDeclarationAsync(std::string uri, Position pos, LocationCallback callback);

  // Async textDocument/references.
  void RequestFindReferencesAsync(std::string uri, Position pos, bool include_declaration,
                                   LocationCallback callback);

  // Result of a textDocument/prepareRename probe: whether the position is
  // renameable, the identifier range, and a suggested placeholder (the current
  // symbol text) to seed the rename prompt.
  struct PrepareRename {
    bool can_rename = false;
    Range range{};
    std::string placeholder;
  };
  using PrepareRenameCallback = std::function<void(std::optional<PrepareRename>)>;

  // Async textDocument/prepareRename — validate the cursor position and fetch the
  // server's suggested placeholder before opening the rename prompt. nullopt when
  // the server has no prepareRename provider (caller falls back to its heuristic).
  void RequestPrepareRenameAsync(std::string uri, Position pos, PrepareRenameCallback callback);

  // Async textDocument/rename.
  void RequestRenameAsync(std::string uri, Position pos, std::string new_name,
                           RenameCallback callback);

  // Async textDocument/documentSymbol.
  void RequestDocumentSymbolAsync(std::string uri, DocumentSymbolCallback callback);

  // Async workspace/symbol — project-wide symbol search for `query`.
  void RequestWorkspaceSymbolAsync(std::string query, WorkspaceSymbolCallback callback);

  // Async textDocument/semanticTokens/full. The callback receives tokens decoded
  // to absolute ranges; map the `token_type` index through SemanticTokenLegend().
  void RequestSemanticTokensAsync(std::string uri, SemanticTokensCallback callback);

  // The server's semantic-token type legend (index -> type name), captured from
  // the initialize handshake. Empty when the server advertises no semanticTokens
  // provider; in that case RequestSemanticTokensAsync reports nullopt.
  std::vector<std::string> SemanticTokenLegend() const;
  bool SupportsSemanticTokens() const;

  // Async textDocument/inlayHint for `range` (a whole-document or visible range).
  // Reports nullopt when the server advertises no inlayHint provider.
  void RequestInlayHintsAsync(std::string uri, Range range, InlayHintCallback callback);
  bool SupportsInlayHints() const;

  // True when the server advertised renameProvider.prepareProvider — i.e. a
  // textDocument/prepareRename request is worth sending.
  bool SupportsPrepareRename() const;

  // Unit tests: pretend a connected server without starting a subprocess.
  void EnableTestStubMode();
  void DisableTestStubMode();
  void SetTestDocumentSymbolHandler(
      std::function<void(std::string uri, DocumentSymbolCallback cb)> handler);
  void ClearTestDocumentSymbolHandler();
  // Unit tests: feed a canned hover response (raw LSP hover result JSON).
  void SetTestHoverHandler(std::function<void(std::string uri, HoverCallback cb)> handler);
  void ClearTestHoverHandler();
  // Unit tests: feed a canned formatting response (a TextEdit[]).
  void SetTestFormattingHandler(std::function<void(std::string uri, FormattingCallback cb)> handler);
  void ClearTestFormattingHandler();
  // Unit tests: feed a canned rename response (a WorkspaceEdit).
  void SetTestRenameHandler(
      std::function<void(std::string uri, std::string new_name, RenameCallback cb)> handler);
  void ClearTestRenameHandler();
  // Unit tests: feed a canned completion response (a CompletionItem[]).
  void SetTestCompletionHandler(
      std::function<void(std::string uri, Position pos, CompletionCallback cb)> handler);
  void ClearTestCompletionHandler();
  // Unit tests: feed a canned signature-help response.
  void SetTestSignatureHelpHandler(
      std::function<void(std::string uri, Position pos, SignatureHelpCallback cb)> handler);
  void ClearTestSignatureHelpHandler();
  // Unit tests: feed a canned prepareRename response (also marks the capability
  // supported so the request is not short-circuited).
  void SetTestPrepareRenameHandler(
      std::function<void(std::string uri, Position pos, PrepareRenameCallback cb)> handler);
  void ClearTestPrepareRenameHandler();
  // Unit tests: feed a canned workspace/symbol response.
  void SetTestWorkspaceSymbolHandler(
      std::function<void(std::string query, WorkspaceSymbolCallback cb)> handler);
  void ClearTestWorkspaceSymbolHandler();
  // Unit tests: drive the server-request path (as the I/O thread would) so a
  // simulated workspace/applyEdit exercises the real dispatch → main-thread apply.
  // The reply is enqueued to the outbound queue (swallowed in stub mode).
  void SimulateServerRequestForTesting(const std::string& method, util::JsonValue params,
                                       util::JsonValue id);
  // Unit tests: feed a canned semantic-tokens response + legend.
  void SetTestSemanticTokensHandler(
      std::function<void(std::string uri, SemanticTokensCallback cb)> handler);
  void SetTestSemanticTokenLegend(std::vector<std::string> legend);
  // Unit tests: feed a canned inlayHint response (also marks the capability
  // supported so the request is not short-circuited).
  void SetTestInlayHintHandler(
      std::function<void(std::string uri, Range range, InlayHintCallback cb)> handler);
  void ClearTestInlayHintHandler();

  // Shutdown and close connection (blocks until complete).
  void BeginShutdown();
  void Shutdown();

  // True while a background shutdown is in progress.
  bool IsShuttingDown() const;

  // True once shutdown has fully completed.
  bool IsShutdownComplete() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace microide::workspace
