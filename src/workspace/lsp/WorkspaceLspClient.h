#pragma once

#include "platform/AsyncSubprocess.h"
#include "platform/ProcessLauncher.h"
#include "util/JsonValue.h"
#include "workspace/lsp/LspFileWatchRegistry.h"

#include <SDL3/SDL.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

constexpr std::string_view LspReadinessWord(LspReadinessSnapshot::State state) {
  switch (state) {
    case LspReadinessSnapshot::State::Idle:
      return "Idle";
    case LspReadinessSnapshot::State::Starting:
      return "Starting…";
    case LspReadinessSnapshot::State::Indexing:
      return "Indexing…";
    case LspReadinessSnapshot::State::Ready:
      return "Ready";
    case LspReadinessSnapshot::State::Failed:
      return "Failed";
  }
  return "Idle";
}

inline std::string_view LspReadinessText(const LspReadinessSnapshot& snapshot,
                                         std::string& scratch) {
  if (snapshot.state != LspReadinessSnapshot::State::Indexing || snapshot.indexed_count <= 0) {
    return LspReadinessWord(snapshot.state);
  }
  scratch.assign("Indexing ");
  std::array<char, 16> digits{};
  const auto [end, ec] =
      std::to_chars(digits.data(), digits.data() + digits.size(), snapshot.indexed_count);
  if (ec != std::errc{}) {
    return LspReadinessWord(snapshot.state);
  }
  scratch.append(digits.data(), static_cast<std::size_t>(end - digits.data()));
  scratch.append("…");
  return scratch;
}

enum class LspRequestOutcome {
  kOk,
  kEmpty,
  kTimeout,
  kUnavailable,
  kProtocolError,
};

template <typename T>
struct LspResult {
  LspRequestOutcome outcome = LspRequestOutcome::kEmpty;
  std::optional<T> value;

  LspResult() = default;
  LspResult(std::nullopt_t) {}                                      // NOLINT: implicit
  LspResult(std::optional<T> v)                                     // NOLINT: implicit
      : outcome(v.has_value() ? LspRequestOutcome::kOk : LspRequestOutcome::kEmpty),
        value(std::move(v)) {}
  LspResult(T v)                                                    // NOLINT: implicit
      : outcome(LspRequestOutcome::kOk), value(std::move(v)) {}
  LspResult(LspRequestOutcome o, std::optional<T> v) : outcome(o), value(std::move(v)) {}

  bool has_value() const { return value.has_value(); }
  explicit operator bool() const { return value.has_value(); }
  const T& operator*() const { return *value; }
  T& operator*() { return *value; }
  const T* operator->() const { return &*value; }
  T* operator->() { return &*value; }

  bool answered() const {
    return outcome == LspRequestOutcome::kOk || outcome == LspRequestOutcome::kEmpty;
  }
  bool timed_out() const { return outcome == LspRequestOutcome::kTimeout; }
};

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
    int severity = 1;
    std::string code;
  };

  struct CompletionItem {
    std::string label;
    int kind = 1;
    std::string detail;
    std::string documentation;
    std::string insert_text;
    std::string sort_text;
    std::string filter_text;
    int insert_text_format = 1;
    std::optional<Range> replace_range;
    std::vector<std::pair<Range, std::string>> additional_text_edits;
  };

  struct CompletionList {
    std::vector<CompletionItem> items;
    bool is_incomplete = false;
  };

  enum class CompletionTrigger : std::uint8_t {
    kInvoked = 1,
    kTriggerCharacter = 2,
    kIncomplete = 3,
  };

  struct WorkspaceEdit {
    struct ResourceOp {
      enum class Kind : std::uint8_t { Create, Rename, Delete };
      Kind kind = Kind::Create;
      std::string uri;
      std::string new_uri;
      bool overwrite = false;
      bool ignore_if_exists = false;
      bool ignore_if_not_exists = false;
      bool recursive = false;
    };
    std::unordered_map<std::string, std::vector<std::pair<Range, std::string>>> changes;
    std::vector<ResourceOp> resource_ops;
    std::unordered_map<std::string, int> expected_versions;
  };

  struct CodeAction {
    std::string title;
    std::string command;
    std::vector<util::JsonValue> arguments;
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
    int active_parameter = -1;
  };

  struct SignatureHelp {
    std::vector<SignatureInformation> signatures;
    int active_signature = 0;
    int active_parameter = 0;
  };

  struct SemanticToken {
    int line = 0;
    int start_char = 0;
    int length = 0;
    int token_type = 0;
  };

  struct InlayHint {
    Position position;
    std::string label;
    int kind = 0;
    bool padding_left = false;
    bool padding_right = false;
  };

  struct DocumentHighlight {
    Range range;
    int kind = 1;
  };

  struct CodeLens {
    Range range;
    std::string title;
    std::string command;
    std::vector<util::JsonValue> arguments;
    util::JsonValue unresolved;

    bool needs_resolve() const { return title.empty() && !unresolved.IsNull(); }
  };

  struct CallHierarchyItem {
    std::string name;
    std::string detail;
    int kind = 1;
    std::string uri;
    Range range{};
    Range selection_range{};
    util::JsonValue raw;
  };

  struct CallHierarchyCall {
    CallHierarchyItem item;
    std::vector<Range> call_ranges;
  };

  using OnPublishDiagnostics = std::function<void(std::string uri, std::vector<Diagnostic>)>;

  using TextEdit = std::pair<Range, std::string>;

  using HoverCallback = std::function<void(LspResult<util::JsonValue>)>;
  using CompletionCallback = std::function<void(LspResult<CompletionList>)>;
  using CodeActionCallback = std::function<void(LspResult<std::vector<CodeAction>>)>;
  using FormattingCallback = std::function<void(LspResult<std::vector<TextEdit>>)>;
  using LocationCallback = std::function<void(LspResult<std::vector<Location>>)>;
  using RenameCallback = std::function<void(LspResult<WorkspaceEdit>)>;
  using DocumentSymbolCallback =
      std::function<void(LspResult<std::vector<DocumentSymbol>>)>;
  using SemanticTokensCallback =
      std::function<void(LspResult<std::vector<SemanticToken>>)>;
  using InlayHintCallback = std::function<void(LspResult<std::vector<InlayHint>>)>;
  using DocumentHighlightCallback =
      std::function<void(LspResult<std::vector<DocumentHighlight>>)>;
  using CodeLensCallback = std::function<void(LspResult<std::vector<CodeLens>>)>;
  using ResolveCodeLensCallback = std::function<void(LspResult<CodeLens>)>;
  using ExecuteCommandCallback = std::function<void(LspResult<util::JsonValue>)>;
  using PrepareCallHierarchyCallback =
      std::function<void(LspResult<std::vector<CallHierarchyItem>>)>;
  using CallHierarchyCallsCallback =
      std::function<void(LspResult<std::vector<CallHierarchyCall>>)>;
  using SignatureHelpCallback = std::function<void(LspResult<SignatureHelp>)>;
  using WorkspaceSymbolCallback =
      std::function<void(LspResult<std::vector<WorkspaceSymbol>>)>;

  LspClient();
  ~LspClient();
  LspClient(const LspClient&) = delete;
  LspClient& operator=(const LspClient&) = delete;

  void SetWakeChannel(Uint32 event_type);

  bool Start(const platform::ProcessLauncher& launcher, const std::vector<std::string>& command,
             const std::string& root_uri,
             const std::string& language_id, const std::string& cwd = {},
             const util::JsonValue& initialization_options = {},
             const util::JsonValue& settings = {},
             const platform::SubprocessSandbox& sandbox = {});

  bool IsRunning() const;

  bool IsInitialized() const;

  bool SupportsIncrementalSync() const;

  std::string ServerPositionEncoding() const;

  bool HasOpenDocument(const std::string& uri) const;

  std::string LastError() const;

  ReadinessSnapshot GetReadinessSnapshot() const;

  void SetDiagnosticsCallback(OnPublishDiagnostics callback);

  bool HasDiagnosticsCallback() const;

  void SetApplyEditHandler(std::function<bool(WorkspaceEdit)> handler);
  bool HasApplyEditHandler() const;

  std::optional<int> TrackedDocumentVersion(const std::string& uri) const;

  void DrainCallbacks();

  bool DidOpen(std::string uri, std::string language_id, std::string text);

  bool DidChange(const std::string& uri, const std::string& text);

  bool DidChangeIncremental(const std::string& uri,
                            Range changed_range,
                            const std::string& new_text);

  bool DidSave(const std::string& uri);

  bool DidClose(const std::string& uri);

  struct WatchedFileChange {
    std::string uri;
    LspFileChangeType type = LspFileChangeType::Changed;
  };

  bool WantsWatchedFiles() const;

  bool WantsWatchedFileChange(std::string_view relative_path,
                              std::string_view absolute_path,
                              LspFileChangeType type) const;

  bool DidChangeWatchedFiles(const std::vector<WatchedFileChange>& changes);

  void RegisterFileWatchersForTesting(const util::JsonValue& registration,
                                      const std::filesystem::path& project_root);

  void RequestHoverAsync(std::string uri, Position pos, HoverCallback callback);

  void RequestCompletionAsync(std::string uri, Position pos, CompletionCallback callback,
                              CompletionTrigger trigger = CompletionTrigger::kInvoked,
                              char trigger_character = '\0');

  static void SortCompletionItemsByServerRank(std::vector<CompletionItem>& items);

  void RequestSignatureHelpAsync(std::string uri, Position pos, SignatureHelpCallback callback);

  void RequestCodeActionAsync(std::string uri, Range range,
                              std::vector<Diagnostic> context_diagnostics,
                              CodeActionCallback callback);

  void RequestFormattingAsync(std::string uri, int tab_size, bool insert_spaces,
                               FormattingCallback callback);

  void RequestRangeFormattingAsync(std::string uri, Range range, int tab_size,
                                   bool insert_spaces, FormattingCallback callback);

  void RequestGoToDefinitionAsync(std::string uri, Position pos, LocationCallback callback);

  void RequestGoToTypeDefinitionAsync(std::string uri, Position pos, LocationCallback callback);
  void RequestGoToImplementationAsync(std::string uri, Position pos, LocationCallback callback);
  void RequestGoToDeclarationAsync(std::string uri, Position pos, LocationCallback callback);

  void RequestFindReferencesAsync(std::string uri, Position pos, bool include_declaration,
                                   LocationCallback callback);

  struct PrepareRename {
    bool can_rename = false;
    Range range{};
    std::string placeholder;
  };
  using PrepareRenameCallback = std::function<void(LspResult<PrepareRename>)>;

  void RequestPrepareRenameAsync(std::string uri, Position pos, PrepareRenameCallback callback);

  void RequestRenameAsync(std::string uri, Position pos, std::string new_name,
                           RenameCallback callback);

  void RequestDocumentSymbolAsync(std::string uri, DocumentSymbolCallback callback);

  void RequestWorkspaceSymbolAsync(std::string query, WorkspaceSymbolCallback callback);

  void RequestSemanticTokensAsync(std::string uri, SemanticTokensCallback callback);

  std::vector<std::string> SemanticTokenLegend() const;
  bool SupportsSemanticTokens() const;

  void RequestInlayHintsAsync(std::string uri, Range range, InlayHintCallback callback);
  bool SupportsInlayHints() const;
  bool IsCompletionTriggerCharacter(char c) const;
  void SetCompletionTriggerCharactersForTesting(std::string_view characters);
  bool IsSignatureHelpTriggerCharacter(char c) const;
  void SetSignatureHelpTriggerCharactersForTesting(std::string_view characters);

  void RequestDocumentHighlightAsync(std::string uri, Position pos,
                                     DocumentHighlightCallback callback);
  bool SupportsDocumentHighlight() const;

  void RequestCodeLensAsync(std::string uri, CodeLensCallback callback);
  void ResolveCodeLensAsync(util::JsonValue unresolved, ResolveCodeLensCallback callback);
  bool SupportsCodeLens() const;
  bool SupportsCodeLensResolve() const;

  void ExecuteServerCommandAsync(std::string command, std::vector<util::JsonValue> arguments,
                                 ExecuteCommandCallback callback);

  void RequestPrepareCallHierarchyAsync(std::string uri, Position pos,
                                        PrepareCallHierarchyCallback callback);
  void RequestIncomingCallsAsync(util::JsonValue item, CallHierarchyCallsCallback callback);
  void RequestOutgoingCallsAsync(util::JsonValue item, CallHierarchyCallsCallback callback);
  bool SupportsCallHierarchy() const;

  bool SupportsPrepareRename() const;

  void EnableTestStubMode();
  void SetMaxQueuedBytesForTesting(std::size_t bytes);
  void SetRequestTimeoutForTesting(std::chrono::milliseconds timeout);
  void SetTestDocumentSymbolHandler(
      std::function<void(std::string uri, DocumentSymbolCallback cb)> handler);
  void SetTestHoverHandler(std::function<void(std::string uri, HoverCallback cb)> handler);
  void SetTestFormattingHandler(
      std::function<void(std::string uri, bool insert_spaces, FormattingCallback cb)> handler);
  void SetTestCodeActionHandler(std::function<void(std::string uri, CodeActionCallback cb)> handler);
  void SetTestRenameHandler(
      std::function<void(std::string uri, std::string new_name, RenameCallback cb)> handler);
  void SetTestCompletionHandler(
      std::function<void(std::string uri, Position pos, CompletionCallback cb)> handler);
  void SetTestSignatureHelpHandler(
      std::function<void(std::string uri, Position pos, SignatureHelpCallback cb)> handler);
  void SetTestPrepareRenameHandler(
      std::function<void(std::string uri, Position pos, PrepareRenameCallback cb)> handler);
  void SetTestReferencesHandler(std::function<void(std::string uri, Position pos, LocationCallback cb)> handler);
  void SetTestWorkspaceSymbolHandler(
      std::function<void(std::string query, WorkspaceSymbolCallback cb)> handler);
  void SimulateServerRequestForTesting(const std::string& method, util::JsonValue params,
                                       util::JsonValue id);
  void SimulateServerNotificationForTesting(const std::string& method, util::JsonValue params);
  void SetTestSemanticTokensHandler(
      std::function<void(std::string uri, SemanticTokensCallback cb)> handler);
  void SetTestSemanticTokenLegend(std::vector<std::string> legend);
  void SetTestInlayHintHandler(
      std::function<void(std::string uri, Range range, InlayHintCallback cb)> handler);
  void ClearTestInlayHintHandler();
  void SetTestDocumentHighlightHandler(
      std::function<void(std::string uri, Position pos, DocumentHighlightCallback cb)> handler);
  void ClearTestDocumentHighlightHandler();
  void SetTestCodeLensHandler(std::function<void(std::string uri, CodeLensCallback cb)> handler);
  void ClearTestCodeLensHandler();
  void SetTestResolveCodeLensHandler(
      std::function<void(util::JsonValue unresolved, ResolveCodeLensCallback cb)> handler);
  void SetTestExecuteCommandHandler(
      std::function<void(std::string command, std::vector<util::JsonValue> arguments,
                         ExecuteCommandCallback cb)> handler);
  void SetTestPrepareCallHierarchyHandler(
      std::function<void(std::string uri, Position pos, PrepareCallHierarchyCallback cb)> handler);
  void SetTestCallHierarchyCallsHandler(
      std::function<void(bool incoming, util::JsonValue item, CallHierarchyCallsCallback cb)>
          handler);

  void BeginShutdown();
  void Shutdown();

  bool IsShutdownComplete() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
