#include "workspace/LspService.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/DiagnosticsStore.h"
#include "editor/PluginDecorationStore.h"
#include "editor/SyntaxHighlighter.h"
#include "render/Theme.h"
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

// Map a document position through a single replacement of [before_start, before_end)
// whose inserted text ends at `new_end`. Positions at or before the edit start are
// unchanged; positions at or after the edit end shift onto the replacement's new
// geometry; positions strictly inside the replaced span collapse to the new end (the
// server republishes an authoritative range shortly after).
editor::TextPosition AdjustPositionForReplace(editor::TextPosition p,
                                              editor::TextPosition before_start,
                                              editor::TextPosition before_end,
                                              editor::TextPosition new_end) {
  if (p.line < before_start.line ||
      (p.line == before_start.line && p.column <= before_start.column)) {
    return p;
  }
  if (p.line > before_end.line ||
      (p.line == before_end.line && p.column >= before_end.column)) {
    if (p.line == before_end.line) {
      return editor::TextPosition{new_end.line, new_end.column + (p.column - before_end.column)};
    }
    const std::ptrdiff_t line_delta =
        static_cast<std::ptrdiff_t>(new_end.line) - static_cast<std::ptrdiff_t>(before_end.line);
    const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(p.line) + line_delta;
    return editor::TextPosition{static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, shifted)),
                                p.column};
  }
  return new_end;
}

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
  return util::SerializeLines(viewport.lines().Snapshot(), viewport.line_ending());
}

// Map an LSP standard semantic-token type name to the editor's lexical token
// kind. Returns Plain for kinds we have no distinct theme color for (variable,
// function, parameter, property, ...); the caller skips those so semantic tokens
// only refine coloring rather than flatten it.
editor::SyntaxTokenKind SyntaxKindForSemanticType(std::string_view type) {
  if (type == "keyword" || type == "modifier") return editor::SyntaxTokenKind::Keyword;
  if (type == "type" || type == "class" || type == "struct" || type == "interface" ||
      type == "enum" || type == "typeParameter" || type == "namespace") {
    return editor::SyntaxTokenKind::Type;
  }
  if (type == "string") return editor::SyntaxTokenKind::String;
  if (type == "comment") return editor::SyntaxTokenKind::Comment;
  if (type == "number") return editor::SyntaxTokenKind::Number;
  if (type == "macro") return editor::SyntaxTokenKind::Preprocessor;
  if (type == "operator") return editor::SyntaxTokenKind::Operator;
  if (type == "enumMember") return editor::SyntaxTokenKind::Constant;
  return editor::SyntaxTokenKind::Plain;
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

void LspService::SetTheme(const render::Theme* theme) { theme_ = theme; }

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
         (FindProvider(*completion_registry_, language_id) != nullptr ||
          CurrentLspManager().HasServer(language_id));
}

bool LspService::HasActiveCodeActionProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() &&
         (FindProvider(*code_action_registry_, language_id) != nullptr ||
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
                                       std::string_view language_id,
                                       std::string_view precomputed_uri) {
  util::PerformanceTrace::Scope perf_scope("LspService::EnsureLspDocumentOpen");
  if (viewport.path().empty() || language_id.empty()) {
    return;
  }
  const std::string uri = precomputed_uri.empty() ? FileUriForPath(viewport.path())
                                                   : std::string(precomputed_uri);
  if (client.HasOpenDocument(uri)) {
    return;
  }
  client.DidOpen(uri, std::string(language_id), SerializeViewportText(viewport));
  // Pull semantic tokens for the freshly-opened document so identifiers recolor.
  RequestLspSemanticTokens(viewport, client);
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

void LspService::RequestLspSemanticTokens(const editor::TextViewport& viewport, LspClient& client) {
  if (theme_ == nullptr || viewport.path().empty() || !client.SupportsSemanticTokens()) {
    return;
  }
  ProjectWorkspaceState* const project = &CurrentProjectState();
  std::string uri = FileUriForPath(viewport.path());
  std::vector<std::string> legend = client.SemanticTokenLegend();
  if (legend.empty()) {
    return;
  }
  client.RequestSemanticTokensAsync(
      uri, [this, project, uri, legend = std::move(legend)](
               std::optional<std::vector<LspClient::SemanticToken>> tokens) mutable {
        if (!tokens.has_value()) {
          return;
        }
        PublishLspSemanticTokens(*project, std::move(uri), std::move(legend), std::move(*tokens));
      });
}

void LspService::PublishLspSemanticTokens(ProjectWorkspaceState& state, std::string uri,
                                          std::vector<std::string> legend,
                                          std::vector<LspClient::SemanticToken> tokens) {
  if (theme_ == nullptr) {
    return;
  }
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }
  const SDL_Color plain = theme_->text_primary;
  editor::PluginDecorationData data;
  data.text_styles.reserve(tokens.size());
  for (const LspClient::SemanticToken& token : tokens) {
    if (token.token_type < 0 ||
        static_cast<std::size_t>(token.token_type) >= legend.size() || token.length <= 0) {
      continue;
    }
    const editor::SyntaxTokenKind kind = SyntaxKindForSemanticType(legend[token.token_type]);
    if (kind == editor::SyntaxTokenKind::Plain) {
      continue;  // no distinct color -> leave the lexical highlighting in place
    }
    editor::TextStyleDecoration style;
    style.line = static_cast<std::uint32_t>(token.line);
    style.start_column = static_cast<std::uint32_t>(token.start_char);
    // ParseSemanticTokensData bounds line/start_char/length to [0, INT_MAX]
    // individually, but their sum can still overflow a signed int here. Compute
    // in int64 and clamp so `end_column` never wraps to a garbage value (UB).
    const std::int64_t end_column =
        std::min<std::int64_t>(static_cast<std::int64_t>(token.start_char) + token.length,
                               std::numeric_limits<std::uint32_t>::max());
    style.end_column = static_cast<std::uint32_t>(end_column);
    style.foreground = editor::SyntaxTokenColor(*theme_, kind, plain);  // a!=0 => recolor
    data.text_styles.push_back(style);
  }
  if (state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("lsp:semantic", *path,
                                                                       std::move(data))) {
    operations_.request_editor_surface_redraw();
  }
}

void LspService::ShiftLspDiagnosticsForAppliedEdit(const editor::TextViewport& viewport) {
  if (viewport.path().empty()) {
    return;
  }
  const std::optional<editor::AppliedEdit>& applied = viewport.last_applied_edit();
  if (!applied.has_value()) {
    return;
  }
  const editor::SelectionRange before =
      editor::TextViewport::NormalizeRange(applied->range_before);
  const editor::TextPosition before_start = before.start;
  const editor::TextPosition before_end = before.end;
  const std::string& replacement = applied->replacement_text;
  const std::size_t newline_count =
      static_cast<std::size_t>(std::count(replacement.begin(), replacement.end(), '\n'));
  const std::size_t last_line_length =
      newline_count == 0 ? replacement.size()
                         : replacement.size() - (replacement.find_last_of('\n') + 1);
  const editor::TextPosition new_end =
      newline_count == 0
          ? editor::TextPosition{before_start.line, before_start.column + last_line_length}
          : editor::TextPosition{before_start.line + newline_count, last_line_length};
  if (before_start == before_end && new_end == before_start) {
    return;  // Empty edit -> nothing moved.
  }
  CurrentProjectState().diagnostics_store.TransformOwnerFile(
      "lsp", viewport.path(),
      [before_start, before_end, new_end](editor::SelectionRange range) {
        range.start = AdjustPositionForReplace(range.start, before_start, before_end, new_end);
        range.end = AdjustPositionForReplace(range.end, before_start, before_end, new_end);
        return range;
      });
}

void LspService::ShiftLspDiagnosticsForBulkChange(const editor::TextViewport& viewport,
                                                  const std::vector<std::string>& before_lines,
                                                  const std::vector<std::string>& after_lines) {
  if (viewport.path().empty() || before_lines.size() == after_lines.size()) {
    return;  // No net line change: in-place edits keep diagnostics roughly aligned.
  }
  std::size_t first_changed = 0;
  const std::size_t common = std::min(before_lines.size(), after_lines.size());
  while (first_changed < common && before_lines[first_changed] == after_lines[first_changed]) {
    ++first_changed;
  }
  const std::ptrdiff_t line_delta = static_cast<std::ptrdiff_t>(after_lines.size()) -
                                    static_cast<std::ptrdiff_t>(before_lines.size());
  CurrentProjectState().diagnostics_store.TransformOwnerFile(
      "lsp", viewport.path(), [first_changed, line_delta](editor::SelectionRange range) {
        const auto shift = [&](editor::TextPosition p) {
          if (p.line < first_changed) {
            return p;
          }
          std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(p.line) + line_delta;
          if (shifted < static_cast<std::ptrdiff_t>(first_changed)) {
            shifted = static_cast<std::ptrdiff_t>(first_changed);
          }
          p.line = static_cast<std::size_t>(shifted);
          return p;
        };
        range.start = shift(range.start);
        range.end = shift(range.end);
        return range;
      });
}

void LspService::SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                                const std::vector<std::string>& after_lines) {
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }

  // Keep diagnostics positioned for the dirty buffer until the server republishes.
  ShiftLspDiagnosticsForBulkChange(*viewport, before_lines, after_lines);
  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    return;
  }
  const std::string uri = FileUriForPath(viewport->path());
  EnsureLspDocumentOpen(*viewport, *client, language_id, uri);

  // Full-document sync for the bulk-change path. The per-keystroke path
  // (SyncLspForActiveEditableLastChange) sends true ranged incremental edits via
  // the viewport's last applied edit; here we only have before/after snapshots,
  // so a clean full replace is the correct, desync-proof choice.
  client->DidChange(uri, util::SerializeLines(after_lines, viewport->line_ending()));
  // Refresh semantic tokens on bulk edits (paste/undo/format/multi-edit). The
  // per-keystroke path stays request-free to keep typing fast; live incremental
  // semantic refresh (a debounced re-request) is a documented follow-up.
  RequestLspSemanticTokens(*viewport, *client);
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
  // Compute the document URI once and reuse it for both the ensure-open check and
  // the DidChange below, so FileUriForPath's normalize+percent-encode runs once
  // per keystroke instead of twice.
  const std::string uri = FileUriForPath(viewport->path());
  {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::EnsureDocumentOpen");
    EnsureLspDocumentOpen(*viewport, *client, language_id, uri);
  }

  const auto& applied_edit = viewport->last_applied_edit();
  if (!applied_edit.has_value()) {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::FullSyncNoAppliedEdit");
    client->DidChange(uri, util::SerializeLines(viewport->lines().Snapshot(), viewport->line_ending()));
    return;
  }

  // Slide stored diagnostics through this keystroke so they stay on their text
  // while the buffer is dirty, until the server republishes authoritative ranges.
  ShiftLspDiagnosticsForAppliedEdit(*viewport);

  if (!client->SupportsIncrementalSync()) {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::FullSyncNoIncrementalSupport");
    client->DidChange(uri, util::SerializeLines(viewport->lines().Snapshot(), viewport->line_ending()));
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
