#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor/TextViewport.h"
#include "project/ProjectChangeTypes.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/registries/WorkspaceCodeActionRegistry.h"
#include "workspace/registries/WorkspaceCompletionRegistry.h"
#include "workspace/lsp/WorkspaceLspManager.h"

namespace microide::render {
struct Theme;
}

namespace microide::workspace {

struct WorkspaceContext;
struct ProjectWorkspaceState;
struct CodeActionEdit;
struct WorkspaceResourceOp;

class LspService {
 public:
  struct Operations {
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<void()> refresh_problems_sidebar;
    std::function<void()> request_frame;
    std::function<bool(const LspClient&, LspClient::WorkspaceEdit)> apply_server_workspace_edit;
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    std::function<bool(std::string_view)> plugin_has_definition_provider;
    std::function<bool(std::string_view)> plugin_has_references_provider;
  };

  LspService() = default;
  ~LspService();

  void Configure(WorkspaceContext& context, CompletionRegistry& completion_registry,
                 CodeActionRegistry& code_action_registry, Operations operations);
  void SetWakeChannel(Uint32 event_type);
  void SetTheme(const render::Theme* theme);

  void ReconcileFeatureSettings();

  std::string ActiveLanguageIdForProvider() const;
  bool HasActivePluginDefinitionProvider() const;
  bool HasActivePluginReferencesProvider() const;
  bool HasActiveLanguageServer() const;
  bool HasActiveCompletionProvider() const;
  bool HasActiveCodeActionProvider() const;
  bool HasActiveDefinitionProvider() const;
  bool HasActiveReferencesProvider() const;

  LspManager& CurrentLspManager();
  const LspManager& CurrentLspManager() const;
  LspManager& EnsureProjectLspManager(ProjectWorkspaceState& state);
  void ConsumeLspCallbacks();

  void RetireCurrentProjectServers();

  std::size_t RetiringClientCountForTesting() const { return retiring_clients_.size(); }

  enum class LspStatusSeverity : std::uint8_t { Idle, Busy, Error };

  LspClient::ReadinessSnapshot ActiveLspReadinessSnapshot(bool ensure_started = true);
  void ActiveLspStatusStrings(bool ensure_started, std::string& text, std::string& tooltip,
                              LspStatusSeverity* severity = nullptr);

  void BeginTrackedLspRequest();
  void FinishTrackedLspRequest();
  void ExpireTrackedLspRequestIfNeeded();

  void ScheduleBufferOpen(const std::filesystem::path& path);
  bool ConsumeDeferredBufferOpen();
  bool HasPendingBufferOpen() const { return pending_buffer_open_.has_value(); }

  std::size_t NotifyWatchedFileChanges(const std::vector<project::ProjectFileChange>& changes);

  void ForgetDocument(const std::string& uri);
  std::size_t synced_document_count() const { return synced_content_revision_.size(); }
  LspClient* LspClientForViewport(const editor::TextViewport& viewport, std::string* language_id);
  void EnsureLspDocumentOpen(const editor::TextViewport& viewport, LspClient& client,
                             std::string_view language_id,
                             std::string_view precomputed_uri = {});
  void PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
                             lsp_encoding::PositionEncoding encoding,
                             std::vector<LspClient::Diagnostic> diagnostics);
  void RequestLspSemanticTokens(const editor::TextViewport& viewport, LspClient& client);
  void PublishLspSemanticTokens(ProjectWorkspaceState& state, std::string uri,
                                std::uint64_t request_generation,
                                lsp_encoding::PositionEncoding encoding,
                                std::vector<std::string> legend,
                                std::vector<LspClient::SemanticToken> tokens);
  void RequestLspInlayHints(const editor::TextViewport& viewport, LspClient& client);
  void PublishLspInlayHints(ProjectWorkspaceState& state, std::string uri,
                            std::uint64_t request_generation,
                            lsp_encoding::PositionEncoding encoding,
                            std::vector<LspClient::InlayHint> hints);

  void RequestLspCodeLenses(const editor::TextViewport& viewport, LspClient& client);
  void PublishLspCodeLenses(ProjectWorkspaceState& state, std::string uri,
                            std::uint64_t request_generation, std::string language_id,
                            lsp_encoding::PositionEncoding encoding,
                            std::vector<LspClient::CodeLens> lenses);
  void ActivateCodeLens(std::uint64_t payload);

  void MaybeRequestDocumentHighlights();
  void PublishLspDocumentHighlights(ProjectWorkspaceState& state,
                                    const std::filesystem::path& path,
                                    const void* document,
                                    std::uint64_t request_generation,
                                    std::uint64_t content_revision,
                                    lsp_encoding::PositionEncoding encoding,
                                    std::size_t caret_line, std::size_t caret_column,
                                    std::vector<LspClient::DocumentHighlight> highlights);
  void SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                      const std::vector<std::string>& after_lines);
  struct BufferChangeDelta {
    std::size_t before_line_count = 0;
    std::size_t first_changed_line = 0;
  };
  void SyncLspForBufferChange(const editor::TextViewport& viewport, BufferChangeDelta delta);
  void SyncLspForActiveEditableLastChange();

 private:
  ProjectWorkspaceState& CurrentProjectState();

  struct BufferSyncTarget {
    LspClient* client = nullptr;
    std::string uri;
    std::string language_id;
    bool was_open = false;
  };
  std::optional<BufferSyncTarget> ResolveOpenDocumentForSync(const editor::TextViewport& viewport,
                                                             std::string_view uri = {});

  template <typename Item, typename Build>
  void PublishGuardedOverlay(ProjectWorkspaceState& state, std::string_view owner_key,
                             std::unordered_map<std::string, std::uint64_t>& generations,
                             const std::string& uri, std::uint64_t request_generation,
                             lsp_encoding::PositionEncoding encoding, std::vector<Item> items,
                             Build build);

  static std::uint64_t NextOverlayGeneration(
      std::unordered_map<std::string, std::uint64_t>& generations, const std::string& uri);
  static bool OverlayGenerationCurrent(
      const std::unordered_map<std::string, std::uint64_t>& generations, const std::string& uri,
      std::uint64_t generation);

  template <typename Transform>
  void TransformLspDiagnostics(const editor::TextViewport& viewport, Transform&& transform);

  void ShiftLspDiagnosticsForAppliedEdit(const editor::TextViewport& viewport,
                                         std::optional<std::uint64_t> followed_revision);
  void ShiftLspDiagnosticsForBulkChange(const editor::TextViewport& viewport,
                                        std::size_t before_line_count,
                                        std::size_t first_changed_line);

  void ClearLspSemanticTokensForFile(const editor::TextViewport& viewport);

  void ClearLspEditOverlaysForFile(const editor::TextViewport& viewport);

  WorkspaceContext* context_ = nullptr;
  CompletionRegistry* completion_registry_ = nullptr;
  CodeActionRegistry* code_action_registry_ = nullptr;
  const render::Theme* theme_ = nullptr;
  Operations operations_{};
  Uint32 wake_event_type_ = 0;

  std::optional<std::filesystem::path> pending_buffer_open_;

  std::unordered_map<std::string, std::uint64_t> synced_content_revision_;
  std::filesystem::path sync_uri_path_;
  std::string sync_uri_;

  std::unordered_map<std::string, std::uint64_t> semantic_token_generation_;
  std::unordered_map<std::string, std::uint64_t> inlay_hint_generation_;

  std::unordered_map<std::string, std::uint64_t> code_lens_generation_;
  struct CodeLensCommand {
    std::string uri;
    std::string language_id;
    std::string command;
    std::vector<util::JsonValue> arguments;
  };
  std::unordered_map<std::uint64_t, CodeLensCommand> code_lens_commands_;
  std::uint64_t next_code_lens_payload_ = 0;

  std::uint64_t document_highlight_generation_ = 0;
  std::filesystem::path document_highlight_request_path_;
  std::uint64_t document_highlight_request_revision_ = 0;
  const void* document_highlight_request_document_ = nullptr;
  std::size_t document_highlight_request_line_ = 0;
  std::size_t document_highlight_request_column_ = 0;
  bool document_highlight_request_valid_ = false;

  struct FeatureEnablement {
    bool master = false;
    bool diagnostics = false;
    bool semantic = false;
    bool inlay = false;
    bool code_lens = false;
    bool document_highlight = false;
  };
  std::optional<FeatureEnablement> last_feature_enablement_;

  bool ClearAllLspOverlays(ProjectWorkspaceState& state);

  std::vector<std::unique_ptr<LspClient>> retiring_clients_;
  void RetireClients(std::vector<std::unique_ptr<LspClient>> clients);
  void DrainRetiringClients();
};

}
