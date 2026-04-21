#pragma once

#include "platform/AsyncSubprocess.h"
#include "util/JsonValue.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace microide::workspace {

// Single LSP server connection with JSON-RPC 2.0.
// Synchronous request-response model: each request blocks until response arrives.
class LspClient {
 public:
  struct InitializeResult {
    bool success = false;
    std::string server_capabilities_json;  // raw JSON for optional parsing later
  };

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

  using OnPublishDiagnostics = std::function<void(std::string uri, std::vector<Diagnostic>)>;

  LspClient();
  ~LspClient();
  LspClient(const LspClient&) = delete;
  LspClient& operator=(const LspClient&) = delete;

  // Start the server and send initialize request.
  // Returns false if startup failed.
  bool Start(const std::vector<std::string>& command, const std::string& root_uri,
             const std::string& language_id);

  // True while the server is running.
  bool IsRunning() const;

  // Set callback for publishDiagnostics notifications.
  void SetDiagnosticsCallback(OnPublishDiagnostics callback) {
    on_diagnostics_ = std::move(callback);
  }

  // Poll for incoming notifications (non-blocking).
  void PollNotifications();

  // Send didOpen.
  bool DidOpen(const std::string& uri, const std::string& language_id,
               const std::string& text);

  // Send didChange (full document sync only).
  bool DidChange(const std::string& uri, const std::string& text);

  // Send didSave.
  bool DidSave(const std::string& uri);

  // Send didClose.
  bool DidClose(const std::string& uri);

  // Request: textDocument/hover
  // Returns JSON or nullopt if request failed/timed out.
  std::optional<util::JsonValue> RequestHover(const std::string& uri, Position pos);

  // Request: textDocument/completion
  std::optional<std::vector<CompletionItem>> RequestCompletion(const std::string& uri,
                                                               Position pos);

  // Request: textDocument/codeAction
  std::optional<std::vector<CodeAction>> RequestCodeAction(const std::string& uri, Range range);

  // Request: textDocument/formatting
  std::optional<std::string> RequestFormatting(const std::string& uri,
                                               int tab_size = 4,
                                               bool insert_spaces = true);

  // Shutdown and close connection.
  void Shutdown();

 private:
  struct Impl;
  Impl* impl_;

  OnPublishDiagnostics on_diagnostics_;
};

}  // namespace microide::workspace
