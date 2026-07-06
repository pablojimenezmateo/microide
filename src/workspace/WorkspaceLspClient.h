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

  // One decoded semantic token: an absolute (line, start_char, length) range plus
  // the server-legend token-type index (the deltas in `data` are pre-resolved).
  struct SemanticToken {
    int line = 0;
    int start_char = 0;
    int length = 0;
    int token_type = 0;
  };

  using OnPublishDiagnostics = std::function<void(std::string uri, std::vector<Diagnostic>)>;

  // Async callback types — called on the main thread from DrainCallbacks().
  using HoverCallback = std::function<void(std::optional<util::JsonValue>)>;
  using CompletionCallback = std::function<void(std::optional<std::vector<CompletionItem>>)>;
  using CodeActionCallback = std::function<void(std::optional<std::vector<CodeAction>>)>;
  using FormattingCallback = std::function<void(std::optional<std::string>)>;
  using LocationCallback = std::function<void(std::optional<std::vector<Location>>)>;
  using RenameCallback = std::function<void(std::optional<WorkspaceEdit>)>;
  using DocumentSymbolCallback =
      std::function<void(std::optional<std::vector<DocumentSymbol>>)>;
  using SemanticTokensCallback =
      std::function<void(std::optional<std::vector<SemanticToken>>)>;

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

  // Async textDocument/codeAction. `context_diagnostics` populates the request
  // `context.diagnostics`; clangd only returns quickfixes for diagnostics passed
  // here (it matches them by range + message).
  void RequestCodeActionAsync(std::string uri, Range range,
                              std::vector<Diagnostic> context_diagnostics,
                              CodeActionCallback callback);

  // Async textDocument/formatting.
  void RequestFormattingAsync(std::string uri, int tab_size, bool insert_spaces,
                               FormattingCallback callback);

  // Async textDocument/definition.
  void RequestGoToDefinitionAsync(std::string uri, Position pos, LocationCallback callback);

  // Async textDocument/references.
  void RequestFindReferencesAsync(std::string uri, Position pos, bool include_declaration,
                                   LocationCallback callback);

  // Async textDocument/rename.
  void RequestRenameAsync(std::string uri, Position pos, std::string new_name,
                           RenameCallback callback);

  // Async textDocument/documentSymbol.
  void RequestDocumentSymbolAsync(std::string uri, DocumentSymbolCallback callback);

  // Async textDocument/semanticTokens/full. The callback receives tokens decoded
  // to absolute ranges; map the `token_type` index through SemanticTokenLegend().
  void RequestSemanticTokensAsync(std::string uri, SemanticTokensCallback callback);

  // The server's semantic-token type legend (index -> type name), captured from
  // the initialize handshake. Empty when the server advertises no semanticTokens
  // provider; in that case RequestSemanticTokensAsync reports nullopt.
  std::vector<std::string> SemanticTokenLegend() const;
  bool SupportsSemanticTokens() const;

  // Unit tests: pretend a connected server without starting a subprocess.
  void EnableTestStubMode();
  void DisableTestStubMode();
  void SetTestDocumentSymbolHandler(
      std::function<void(std::string uri, DocumentSymbolCallback cb)> handler);
  void ClearTestDocumentSymbolHandler();
  // Unit tests: feed a canned semantic-tokens response + legend.
  void SetTestSemanticTokensHandler(
      std::function<void(std::string uri, SemanticTokensCallback cb)> handler);
  void SetTestSemanticTokenLegend(std::vector<std::string> legend);

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
