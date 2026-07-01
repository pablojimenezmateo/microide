#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/WorkspaceLspManager.h"

namespace microide::render {
struct Theme;
}

namespace microide::workspace {

struct WorkspaceContext;
struct ProjectWorkspaceState;
class CompletionRegistry;
class CodeActionRegistry;

// Host-owned home for the LSP glue that used to live directly on WorkspaceShell:
// per-project server management, document synchronization, diagnostics publishing,
// provider-presence queries, readiness/status strings, and the in-flight request
// indicator. WorkspaceShell keeps thin forwarders; render/menu/plugin TUs are
// unchanged. The shell wiring is injected through the narrow Operations seam.
class LspService {
 public:
  struct Operations {
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<void()> refresh_problems_sidebar;
    std::function<void()> request_editor_surface_redraw;
    std::function<void()> request_chrome_redraw;
    std::function<void()> request_bottom_panel_redraw;
  };

  LspService() = default;

  void Configure(WorkspaceContext& context, CompletionRegistry& completion_registry,
                 CodeActionRegistry& code_action_registry, Operations operations);
  void SetWakeEventType(Uint32 event_type);
  // Live pointer to the host-owned active theme used to bake semantic-token colors
  // (the address is stable; a theme switch mutates it in place). Optional: when
  // null, semantic-token publishing is skipped.
  void SetTheme(const render::Theme* theme);

  // Provider-presence queries for the active editable viewport.
  std::string ActiveLanguageIdForProvider() const;
  bool HasActiveCompletionProvider() const;
  bool HasActiveCodeActionProvider() const;
  bool HasActiveDefinitionProvider() const;
  bool HasActiveReferencesProvider() const;

  // Per-project server manager access and main-thread callback pump.
  LspManager& CurrentLspManager();
  const LspManager& CurrentLspManager() const;
  LspManager& EnsureProjectLspManager(ProjectWorkspaceState& state);
  void ConsumeLspCallbacks();

  // Status-bar readiness for the active server.
  LspClient::ReadinessSnapshot ActiveLspReadinessSnapshot(bool ensure_started = true);
  void ActiveLspStatusStrings(bool ensure_started, std::string& text, std::string& tooltip);
  std::string ActiveLspStatusText(bool ensure_started = true);
  std::string ActiveLspStatusTooltip(bool ensure_started = true);

  // In-flight request indicator (drives the "LSP: working..." status segment).
  void BeginTrackedLspRequest();
  void FinishTrackedLspRequest();
  void ExpireTrackedLspRequestIfNeeded();

  // Document lifecycle / synchronization.
  LspClient* LspClientForViewport(const editor::TextViewport& viewport, std::string* language_id);
  // `precomputed_uri`, when non-empty, is used verbatim instead of re-deriving the
  // document URI from the viewport path (FileUriForPath does path normalization +
  // percent-encoding). The per-keystroke sync path already computes the URI once and
  // threads it in so the encode does not run twice per keystroke.
  void EnsureLspDocumentOpen(const editor::TextViewport& viewport, LspClient& client,
                             std::string_view language_id,
                             std::string_view precomputed_uri = {});
  void PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
                             std::vector<LspClient::Diagnostic> diagnostics);
  // Request textDocument/semanticTokens/full for `viewport` and publish the
  // recolor decorations under owner "lsp:semantic" when the response arrives.
  void RequestLspSemanticTokens(const editor::TextViewport& viewport, LspClient& client);
  void PublishLspSemanticTokens(ProjectWorkspaceState& state, std::string uri,
                                std::vector<std::string> legend,
                                std::vector<LspClient::SemanticToken> tokens);
  void SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                      const std::vector<std::string>& after_lines);
  void SyncLspForActiveEditableLastChange();

 private:
  ProjectWorkspaceState& CurrentProjectState();
  const ProjectWorkspaceState& CurrentProjectState() const;

  WorkspaceContext* context_ = nullptr;
  CompletionRegistry* completion_registry_ = nullptr;
  CodeActionRegistry* code_action_registry_ = nullptr;
  const render::Theme* theme_ = nullptr;
  Operations operations_{};
  Uint32 wake_event_type_ = 0;
};

}  // namespace microide::workspace
