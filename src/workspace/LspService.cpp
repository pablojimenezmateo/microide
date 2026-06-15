#include "workspace/LspService.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/FileUri.h"
#include "workspace/LanguageDetection.h"
#include "workspace/WorkspaceCodeActionRegistry.h"
#include "workspace/WorkspaceCompletionRegistry.h"
#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

namespace {

constexpr Uint64 kLspRequestTimeoutMs = 10000;

// Memoized language detection for the active viewport. The result is stable while
// the active file is unchanged, so this collapses the repeated per-frame filetype
// detection (status bar + provider checks + sync) into a single regex pass.
std::string DetectActiveLanguageIdCached(const editor::TextViewport& viewport,
                                         const LspUiState& lsp) {
  if (!lsp.language_cache_path.empty() && viewport.path() == lsp.language_cache_path) {
    return lsp.language_cache_id;
  }
  lsp.language_cache_path = viewport.path();
  lsp.language_cache_id = DetectViewportLanguageId(viewport);
  return lsp.language_cache_id;
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
      return snapshot.indexed_count > 0
                 ? "Indexing " + std::to_string(snapshot.indexed_count) + "..."
                 : "Indexing...";
    case State::Ready:
      return snapshot.message.empty() ? "Ready" : snapshot.message;
    case State::Failed:
      return snapshot.message.empty() ? "Failed" : snapshot.message;
  }
  return "Idle";
}

}  // namespace

void LspService::Configure(WorkspaceContext& context, CompletionRegistry& completion_registry,
                           CodeActionRegistry& code_action_registry, Operations operations) {
  context_ = &context;
  completion_registry_ = &completion_registry;
  code_action_registry_ = &code_action_registry;
  operations_ = std::move(operations);
}

void LspService::SetWakeEventType(Uint32 event_type) { wake_event_type_ = event_type; }

ProjectWorkspaceState& LspService::CurrentProjectState() {
  return context_->current_project_state;
}

const ProjectWorkspaceState& LspService::CurrentProjectState() const {
  return context_->current_project_state;
}

std::string LspService::ActiveLanguageIdForProvider() const {
  const editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return {};
  }
  return DetectActiveLanguageIdCached(*viewport, CurrentProjectState().lsp);
}

bool LspService::HasActiveCompletionProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() &&
         (completion_registry_->FindProvider(language_id) != nullptr ||
          CurrentLspManager().HasServer(language_id));
}

bool LspService::HasActiveCodeActionProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() &&
         (code_action_registry_->FindProvider(language_id) != nullptr ||
          CurrentLspManager().HasServer(language_id));
}

bool LspService::HasActiveDefinitionProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() && CurrentLspManager().HasServer(language_id);
}

bool LspService::HasActiveReferencesProvider() const { return HasActiveDefinitionProvider(); }

LspClient::ReadinessSnapshot LspService::ActiveLspReadinessSnapshot(bool ensure_started) {
  ExpireTrackedLspRequestIfNeeded();

  LspClient::ReadinessSnapshot snapshot;
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    snapshot.message = "Idle";
    return snapshot;
  }

  const std::string language_id =
      DetectActiveLanguageIdCached(*viewport, CurrentProjectState().lsp);
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

std::string LspService::ActiveLspStatusText(bool ensure_started) {
  std::string text;
  std::string tooltip;
  ActiveLspStatusStrings(ensure_started, text, tooltip);
  return text;
}

std::string LspService::ActiveLspStatusTooltip(bool ensure_started) {
  std::string text;
  std::string tooltip;
  ActiveLspStatusStrings(ensure_started, text, tooltip);
  return tooltip;
}

void LspService::ActiveLspStatusStrings(bool ensure_started, std::string& text,
                                        std::string& tooltip) {
  const LspClient::ReadinessSnapshot snapshot = ActiveLspReadinessSnapshot(ensure_started);
  if (CurrentProjectState().lsp.request_in_flight) {
    text = "LSP: working...";
    tooltip = "Language server request in progress";
    return;
  }
  const std::string readiness = LspReadinessMessage(snapshot);
  text = "LSP: " + readiness;
  tooltip = "Language server: " + readiness;
}

void LspService::BeginTrackedLspRequest() {
  auto& lsp = CurrentProjectState().lsp;
  lsp.request_in_flight = true;
  lsp.request_started_ticks = SDL_GetTicks();
  lsp.request_timeout_ticks = lsp.request_started_ticks + kLspRequestTimeoutMs;
  operations_.request_chrome_redraw();
  operations_.request_bottom_panel_redraw();
}

void LspService::FinishTrackedLspRequest() {
  auto& lsp = CurrentProjectState().lsp;
  if (!lsp.request_in_flight) {
    return;
  }
  lsp.request_in_flight = false;
  lsp.request_started_ticks = 0;
  lsp.request_timeout_ticks = 0;
  operations_.request_chrome_redraw();
  operations_.request_bottom_panel_redraw();
}

void LspService::ExpireTrackedLspRequestIfNeeded() {
  auto& lsp = CurrentProjectState().lsp;
  if (!lsp.request_in_flight || lsp.request_timeout_ticks == 0 ||
      SDL_GetTicks() < lsp.request_timeout_ticks) {
    return;
  }
  FinishTrackedLspRequest();
}

LspManager& LspService::EnsureProjectLspManager(ProjectWorkspaceState& state) {
  if (state.lsp_manager == nullptr) {
    state.lsp_manager = std::make_unique<LspManager>();
  }
  if (wake_event_type_ != 0) {
    state.lsp_manager->SetWakeEventType(wake_event_type_);
  }
  return *state.lsp_manager;
}

LspManager& LspService::CurrentLspManager() { return EnsureProjectLspManager(CurrentProjectState()); }

const LspManager& LspService::CurrentLspManager() const {
  return const_cast<LspService*>(this)->CurrentLspManager();
}

LspClient* LspService::LspClientForViewport(const editor::TextViewport& viewport,
                                            std::string* language_id) {
  if (!CurrentLspManager().HasRegisteredServers()) {
    if (language_id != nullptr) {
      language_id->clear();
    }
    return nullptr;
  }
  const LspUiState& lsp_ui = CurrentProjectState().lsp;
  if (language_id != nullptr) {
    *language_id = DetectActiveLanguageIdCached(viewport, lsp_ui);
  }
  const std::string detected_language =
      language_id != nullptr ? *language_id : DetectActiveLanguageIdCached(viewport, lsp_ui);
  if (detected_language.empty()) {
    return nullptr;
  }

  ProjectWorkspaceState* const project = &CurrentProjectState();
  LspClient* client = CurrentLspManager().GetServer(detected_language);
  if (client == nullptr) {
    return nullptr;
  }
  // Bind the diagnostics sink once per client lifetime; this funnel runs on the
  // hot edit path, so avoid re-allocating the std::function on every change.
  if (!client->HasDiagnosticsCallback()) {
    client->SetDiagnosticsCallback([this, project](std::string uri,
                                                   std::vector<LspClient::Diagnostic> diagnostics) {
      PublishLspDiagnostics(*project, std::move(uri), std::move(diagnostics));
    });
  }
  return client;
}

void LspService::EnsureLspDocumentOpen(const editor::TextViewport& viewport, LspClient& client,
                                       std::string_view language_id) {
  util::PerformanceTrace::Scope perf_scope("LspService::EnsureLspDocumentOpen");
  if (viewport.path().empty() || language_id.empty()) {
    return;
  }
  const std::string uri = FileUriForPath(viewport.path());
  if (client.HasOpenDocument(uri)) {
    return;
  }
  client.DidOpen(uri, std::string(language_id), SerializeViewportText(viewport));
}

void LspService::PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
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
    operations_.refresh_problems_sidebar();
    operations_.request_editor_surface_redraw();
  }
}

void LspService::SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                                const std::vector<std::string>& after_lines) {
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }

  (void)before_lines;
  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    return;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);

  // Full-document sync for the bulk-change path. The per-keystroke path
  // (SyncLspForActiveEditableLastChange) sends true ranged incremental edits via
  // the viewport's last applied edit; here we only have before/after snapshots,
  // so a clean full replace is the correct, desync-proof choice.
  const std::string uri = FileUriForPath(viewport->path());
  client->DidChange(uri, util::SerializeLines(after_lines, viewport->line_ending()));
}

void LspService::SyncLspForActiveEditableLastChange() {
  util::PerformanceTrace::Scope perf_scope("LspService::SyncLspForActiveEditableLastChange");
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }

  std::string language_id;
  LspClient* client = nullptr;
  {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::ResolveClient");
    client = LspClientForViewport(*viewport, &language_id);
  }
  if (client == nullptr) {
    return;
  }
  {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::EnsureDocumentOpen");
    EnsureLspDocumentOpen(*viewport, *client, language_id);
  }

  const std::string uri = FileUriForPath(viewport->path());
  const auto& applied_edit = viewport->last_applied_edit();
  if (!applied_edit.has_value()) {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::FullSyncNoAppliedEdit");
    client->DidChange(uri, util::SerializeLines(viewport->lines(), viewport->line_ending()));
    return;
  }

  if (!client->SupportsIncrementalSync()) {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::FullSyncNoIncrementalSupport");
    client->DidChange(uri, util::SerializeLines(viewport->lines(), viewport->line_ending()));
    return;
  }

  util::PerformanceTrace::Scope scope(
      "LspService::SyncLspForActiveEditableLastChange::IncrementalSync");
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

void LspService::ConsumeLspCallbacks() { CurrentLspManager().DrainCallbacks(); }

}  // namespace microide::workspace
