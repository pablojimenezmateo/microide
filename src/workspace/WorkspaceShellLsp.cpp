#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <optional>

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/FileUri.h"

namespace microide::workspace {

namespace {

constexpr Uint64 kLspRequestTimeoutMs = 10000;

std::string DetectActiveLanguageId(const editor::TextViewport& viewport) {
  return editor::runtime_syntax::DetectFiletype(viewport.path(), viewport.lines());
}

std::string SerializeViewportText(const editor::TextViewport& viewport) {
  return util::SerializeLines(viewport.lines(), viewport.line_ending());
}

editor::DiagnosticSeverity DiagnosticSeverityFromLsp(int severity) {
  switch (severity) {
    case 2:
      return editor::DiagnosticSeverity::Warning;
    case 3:
      return editor::DiagnosticSeverity::Info;
    case 4:
      return editor::DiagnosticSeverity::Hint;
    case 1:
    default:
      return editor::DiagnosticSeverity::Error;
  }
}

std::string LspReadinessMessage(const LspClient::ReadinessSnapshot& snapshot) {
  using State = LspClient::ReadinessSnapshot::State;
  switch (snapshot.state) {
    case State::Idle:
      return snapshot.message.empty() ? "Idle" : snapshot.message;
    case State::Starting:
      return snapshot.message.empty() ? "Starting..." : snapshot.message;
    case State::Indexing:
      if (!snapshot.message.empty()) {
        return snapshot.message;
      }
      return snapshot.indexed_count > 0 ? "Indexing " + std::to_string(snapshot.indexed_count) + "..."
                                        : "Indexing...";
    case State::Ready:
      return snapshot.message.empty() ? "Ready" : snapshot.message;
    case State::Failed:
      return snapshot.message.empty() ? "Failed" : snapshot.message;
  }
  return "Idle";
}

}  // namespace

std::string WorkspaceShell::ActiveLanguageIdForProvider() const {
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return {};
  }
  return DetectActiveLanguageId(*viewport);
}

bool WorkspaceShell::HasActiveCompletionProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() &&
         (completion_registry_.FindProvider(language_id) != nullptr ||
          CurrentLspManager().HasServer(language_id));
}

bool WorkspaceShell::HasActiveCodeActionProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() &&
         (code_action_registry_.FindProvider(language_id) != nullptr ||
          CurrentLspManager().HasServer(language_id));
}

bool WorkspaceShell::HasActiveDefinitionProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() && CurrentLspManager().HasServer(language_id);
}

bool WorkspaceShell::HasActiveReferencesProvider() const {
  return HasActiveDefinitionProvider();
}

LspClient::ReadinessSnapshot WorkspaceShell::ActiveLspReadinessSnapshot(bool ensure_started) {
  ExpireTrackedLspRequestIfNeeded();

  LspClient::ReadinessSnapshot snapshot;
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    snapshot.message = "Idle";
    return snapshot;
  }

  const std::string language_id = DetectActiveLanguageId(*viewport);
  if (language_id.empty()) {
    snapshot.message = "Idle";
    return snapshot;
  }

  LspManager& manager = CurrentLspManager();
  if (!manager.HasServer(language_id)) {
    snapshot.message = "No LSP server";
    return snapshot;
  }

  LspClient* client = ensure_started ? manager.GetServer(language_id)
                                     : manager.FindStartedServer(language_id);
  if (client == nullptr) {
    const std::string error = manager.LastServerError(language_id);
    if (!error.empty()) {
      snapshot.state = LspClient::ReadinessSnapshot::State::Failed;
      snapshot.message = error;
      return snapshot;
    }
    snapshot.state = LspClient::ReadinessSnapshot::State::Starting;
    snapshot.message = "Starting...";
    return snapshot;
  }

  return client->GetReadinessSnapshot();
}

std::string WorkspaceShell::ActiveLspStatusText(bool ensure_started) {
  std::string text;
  std::string tooltip;
  ActiveLspStatusStrings(ensure_started, text, tooltip);
  return text;
}

std::string WorkspaceShell::ActiveLspStatusTooltip(bool ensure_started) {
  std::string text;
  std::string tooltip;
  ActiveLspStatusStrings(ensure_started, text, tooltip);
  return tooltip;
}

void WorkspaceShell::ActiveLspStatusStrings(bool ensure_started,
                                            std::string& text,
                                            std::string& tooltip) {
  const LspClient::ReadinessSnapshot snapshot = ActiveLspReadinessSnapshot(ensure_started);
  if (context_.current_project_state.lsp.request_in_flight) {
    text = "LSP: working...";
    tooltip = "Language server request in progress";
    return;
  }
  const std::string readiness = LspReadinessMessage(snapshot);
  text = "LSP: " + readiness;
  tooltip = "Language server: " + readiness;
}

void WorkspaceShell::BeginTrackedLspRequest() {
  auto& lsp = context_.current_project_state.lsp;
  lsp.request_in_flight = true;
  lsp.request_started_ticks = SDL_GetTicks();
  lsp.request_timeout_ticks = lsp.request_started_ticks + kLspRequestTimeoutMs;
  RequestChromeRedraw();
  RequestBottomPanelRedraw();
}

void WorkspaceShell::FinishTrackedLspRequest() {
  auto& lsp = context_.current_project_state.lsp;
  if (!lsp.request_in_flight) {
    return;
  }
  lsp.request_in_flight = false;
  lsp.request_started_ticks = 0;
  lsp.request_timeout_ticks = 0;
  RequestChromeRedraw();
  RequestBottomPanelRedraw();
}

void WorkspaceShell::ExpireTrackedLspRequestIfNeeded() {
  auto& lsp = context_.current_project_state.lsp;
  if (!lsp.request_in_flight || lsp.request_timeout_ticks == 0 ||
      SDL_GetTicks() < lsp.request_timeout_ticks) {
    return;
  }
  FinishTrackedLspRequest();
}

LspManager& WorkspaceShell::EnsureProjectLspManager(ProjectWorkspaceState& state) {
  if (state.lsp_manager == nullptr) {
    state.lsp_manager = std::make_unique<LspManager>();
  }
  if (lsp_event_type_ != 0) {
    state.lsp_manager->SetWakeEventType(lsp_event_type_);
  }
  return *state.lsp_manager;
}

LspManager& WorkspaceShell::CurrentLspManager() {
  return EnsureProjectLspManager(context_.current_project_state);
}

const LspManager& WorkspaceShell::CurrentLspManager() const {
  return const_cast<WorkspaceShell*>(this)->CurrentLspManager();
}

LspClient* WorkspaceShell::LspClientForViewport(const editor::TextViewport& viewport,
                                                std::string* language_id) {
  if (!CurrentLspManager().HasRegisteredServers()) {
    if (language_id != nullptr) {
      language_id->clear();
    }
    return nullptr;
  }
  if (language_id != nullptr) {
    *language_id = DetectActiveLanguageId(viewport);
  }
  const std::string detected_language =
      language_id != nullptr ? *language_id : DetectActiveLanguageId(viewport);
  if (detected_language.empty()) {
    return nullptr;
  }

  ProjectWorkspaceState* const project = &context_.current_project_state;
  LspClient* client = CurrentLspManager().GetServer(detected_language);
  if (client == nullptr) {
    return nullptr;
  }
  client->SetDiagnosticsCallback([this, project](std::string uri,
                                        std::vector<LspClient::Diagnostic> diagnostics) {
    PublishLspDiagnostics(*project, std::move(uri), std::move(diagnostics));
  });
  return client;
}

void WorkspaceShell::EnsureLspDocumentOpen(const editor::TextViewport& viewport,
                                           LspClient& client,
                                           std::string_view language_id) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::EnsureLspDocumentOpen");
  if (viewport.path().empty() || language_id.empty()) {
    return;
  }
  const std::string uri = FileUriForPath(viewport.path());
  if (client.HasOpenDocument(uri)) {
    return;
  }
  client.DidOpen(uri, std::string(language_id), SerializeViewportText(viewport));
}

void WorkspaceShell::PublishLspDiagnostics(ProjectWorkspaceState& state,
                                           std::string uri,
                                           std::vector<LspClient::Diagnostic> diagnostics) {
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }

  std::vector<editor::Diagnostic> converted;
  converted.reserve(diagnostics.size());
  for (const auto& diagnostic : diagnostics) {
    converted.push_back(editor::Diagnostic{
        .range =
            editor::SelectionRange{
                .start =
                    editor::TextPosition{
                        static_cast<std::size_t>(std::max(diagnostic.range.start.line, 0)),
                        static_cast<std::size_t>(std::max(diagnostic.range.start.character, 0)),
                    },
                .end =
                    editor::TextPosition{
                        static_cast<std::size_t>(std::max(diagnostic.range.end.line, 0)),
                        static_cast<std::size_t>(std::max(diagnostic.range.end.character, 0)),
                    },
            },
        .severity = DiagnosticSeverityFromLsp(diagnostic.severity),
        .message = diagnostic.message,
    });
  }

  if (state.diagnostics_store.ReplaceForOwnerFile("lsp", *path, std::move(converted))) {
    RefreshProblemsSidebar();
    RequestEditorSurfaceRedraw();
  }
}

void WorkspaceShell::SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                                    const std::vector<std::string>& after_lines) {
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }

  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    return;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);

  const std::string uri = FileUriForPath(viewport->path());
  const std::string full_text = util::SerializeLines(after_lines, viewport->line_ending());
  if (!client->SupportsIncrementalSync()) {
    client->DidChange(uri, full_text);
    return;
  }

  const std::size_t end_line = before_lines.empty() ? 0 : before_lines.size() - 1;
  const std::size_t end_column = before_lines.empty() ? 0 : before_lines.back().size();
  client->DidChangeIncremental(
      uri,
      LspClient::Range{
          .start = LspClient::Position{0, 0},
          .end =
              LspClient::Position{static_cast<int>(end_line), static_cast<int>(end_column)},
      },
      full_text);
}

void WorkspaceShell::SyncLspForActiveEditableLastChange() {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::SyncLspForActiveEditableLastChange");
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }

  std::string language_id;
  LspClient* client = nullptr;
  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::SyncLspForActiveEditableLastChange::ResolveClient");
    client = LspClientForViewport(*viewport, &language_id);
  }
  if (client == nullptr) {
    return;
  }
  {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::SyncLspForActiveEditableLastChange::EnsureDocumentOpen");
    EnsureLspDocumentOpen(*viewport, *client, language_id);
  }

  const std::string uri = FileUriForPath(viewport->path());
  const auto& applied_edit = viewport->last_applied_edit();
  if (!applied_edit.has_value()) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::SyncLspForActiveEditableLastChange::FullSyncNoAppliedEdit");
    const std::string full_text =
        util::SerializeLines(viewport->lines(), viewport->line_ending());
    client->DidChange(uri, full_text);
    return;
  }

  if (!client->SupportsIncrementalSync()) {
    util::PerformanceTrace::Scope scope(
        "WorkspaceShell::SyncLspForActiveEditableLastChange::FullSyncNoIncrementalSupport");
    const std::string full_text =
        util::SerializeLines(viewport->lines(), viewport->line_ending());
    client->DidChange(uri, full_text);
    return;
  }

  util::PerformanceTrace::Scope scope(
      "WorkspaceShell::SyncLspForActiveEditableLastChange::IncrementalSync");
  client->DidChangeIncremental(
      uri,
      LspClient::Range{
          .start = LspClient::Position{
              static_cast<int>(applied_edit->range_before.start.line),
              static_cast<int>(applied_edit->range_before.start.column),
          },
          .end = LspClient::Position{
              static_cast<int>(applied_edit->range_before.end.line),
              static_cast<int>(applied_edit->range_before.end.column),
          },
      },
      applied_edit->replacement_text);
}

void WorkspaceShell::ConsumeLspCallbacks() { CurrentLspManager().DrainCallbacks(); }

}  // namespace microide::workspace
