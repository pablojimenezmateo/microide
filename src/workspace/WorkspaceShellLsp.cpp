#include "workspace/WorkspaceShell.h"

// The LSP glue lives in LspService; these are thin forwarders kept so existing
// render/menu/plugin call sites stay unchanged. See src/workspace/LspService.*.
namespace microide::workspace {

bool WorkspaceShell::HasActiveCompletionProvider() const {
  return lsp_service_.HasActiveCompletionProvider();
}

bool WorkspaceShell::HasActiveCodeActionProvider() const {
  return lsp_service_.HasActiveCodeActionProvider();
}

bool WorkspaceShell::HasActiveDefinitionProvider() const {
  return lsp_service_.HasActiveDefinitionProvider();
}

bool WorkspaceShell::HasActiveReferencesProvider() const {
  return lsp_service_.HasActiveReferencesProvider();
}

LspManager& WorkspaceShell::CurrentLspManager() { return lsp_service_.CurrentLspManager(); }

const LspManager& WorkspaceShell::CurrentLspManager() const {
  return lsp_service_.CurrentLspManager();
}

LspManager& WorkspaceShell::EnsureProjectLspManager(ProjectWorkspaceState& state) {
  return lsp_service_.EnsureProjectLspManager(state);
}

void WorkspaceShell::ConsumeLspCallbacks() { lsp_service_.ConsumeLspCallbacks(); }

LspClient::ReadinessSnapshot WorkspaceShell::ActiveLspReadinessSnapshot(bool ensure_started) {
  return lsp_service_.ActiveLspReadinessSnapshot(ensure_started);
}

void WorkspaceShell::ActiveLspStatusStrings(bool ensure_started, std::string& text,
                                            std::string& tooltip) {
  lsp_service_.ActiveLspStatusStrings(ensure_started, text, tooltip);
}

std::string WorkspaceShell::ActiveLspStatusText(bool ensure_started) {
  return lsp_service_.ActiveLspStatusText(ensure_started);
}

void WorkspaceShell::BeginTrackedLspRequest() { lsp_service_.BeginTrackedLspRequest(); }

void WorkspaceShell::FinishTrackedLspRequest() { lsp_service_.FinishTrackedLspRequest(); }

LspClient* WorkspaceShell::LspClientForViewport(const editor::TextViewport& viewport,
                                                std::string* language_id) {
  return lsp_service_.LspClientForViewport(viewport, language_id);
}

void WorkspaceShell::EnsureLspDocumentOpen(const editor::TextViewport& viewport, LspClient& client,
                                           std::string_view language_id) {
  lsp_service_.EnsureLspDocumentOpen(viewport, client, language_id);
}

void WorkspaceShell::PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
                                           std::vector<LspClient::Diagnostic> diagnostics) {
  lsp_service_.PublishLspDiagnostics(state, std::move(uri), std::move(diagnostics));
}

void WorkspaceShell::SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                                    const std::vector<std::string>& after_lines) {
  lsp_service_.SyncLspForActiveEditableChange(before_lines, after_lines);
}

void WorkspaceShell::SyncLspForActiveEditableLastChange() {
  lsp_service_.SyncLspForActiveEditableLastChange();
}

}  // namespace microide::workspace
