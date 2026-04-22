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

// Single LSP server connection with JSON-RPC 2.0.
// All request methods are asynchronous: they return immediately and deliver
// results via callbacks dispatched on the main thread through DrainCallbacks().
// Call SetWakeEventType() once before use so the reader thread can wake the
// main event loop when responses are ready.
class LspClient {
 public:
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
  };

  struct CodeAction {
    std::string title;
    std::string command;
    std::vector<util::JsonValue> arguments;
  };

  struct WorkspaceEdit {
    // Map from URI to list of text edits.
    std::unordered_map<std::string, std::vector<std::pair<Range, std::string>>> changes;
  };

  using OnPublishDiagnostics = std::function<void(std::string uri, std::vector<Diagnostic>)>;

  // Async callback types — called on the main thread from DrainCallbacks().
  using HoverCallback = std::function<void(std::optional<util::JsonValue>)>;
  using CompletionCallback = std::function<void(std::optional<std::vector<CompletionItem>>)>;
  using CodeActionCallback = std::function<void(std::optional<std::vector<CodeAction>>)>;
  using FormattingCallback = std::function<void(std::optional<std::string>)>;
  using LocationCallback = std::function<void(std::optional<std::vector<Location>>)>;
  using RenameCallback = std::function<void(std::optional<WorkspaceEdit>)>;

  LspClient();
  ~LspClient();
  LspClient(const LspClient&) = delete;
  LspClient& operator=(const LspClient&) = delete;

  // Set the SDL custom event type used to wake the main event loop when
  // responses are ready. Call before Start().
  void SetWakeEventType(Uint32 event_type);

  // Start the server and send initialize request (blocks until initialized).
  bool Start(const std::vector<std::string>& command, const std::string& root_uri,
             const std::string& language_id);

  // True while the server process is running.
  bool IsRunning() const;

  // True when the server is in the process of initializing.
  bool IsInitializing() const;

  // True when the server has been fully initialized.
  bool IsInitialized() const;

  // True when the server negotiated incremental document sync.
  bool SupportsIncrementalSync() const;

  // True when the client currently tracks an open document for uri.
  bool HasOpenDocument(const std::string& uri) const;

  // Last startup/runtime error message captured by the LSP client.
  const std::string& LastError() const;

  // Set callback for publishDiagnostics notifications (called on main thread).
  void SetDiagnosticsCallback(OnPublishDiagnostics callback);

  // Call from the main thread each frame to dispatch pending callbacks.
  void DrainCallbacks();

  // Send textDocument/didOpen.
  bool DidOpen(const std::string& uri, const std::string& language_id,
               const std::string& text);

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

  // Async textDocument/codeAction.
  void RequestCodeActionAsync(std::string uri, Range range, CodeActionCallback callback);

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

  // Shutdown and close connection.
  void Shutdown();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace microide::workspace
