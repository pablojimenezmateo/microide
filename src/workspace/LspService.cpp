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
#include "workspace/LspPositionEncoding.h"
#include "workspace/LspViewportPositions.h"
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

// Resolve the open editor viewport backing `path`, or nullptr if the file is not
// open in an (already-hydrated) editor tab. Used to obtain line text for inbound
// position-encoding conversion of diagnostics / semantic tokens.
const editor::TextViewport* FindOpenEditorViewport(const ProjectWorkspaceState& state,
                                                   const std::filesystem::path& path) {
  const std::filesystem::path normalized = path.lexically_normal();
  for (const auto& group : state.editor_groups) {
    for (const auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      const auto& editor_state = *tab.editor_state;
      if (!editor_state.needs_restore &&
          editor_state.viewport.path().lexically_normal() == normalized) {
        return &editor_state.viewport;
      }
    }
  }
  return nullptr;
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
    client->SetDiagnosticsCallback([this, project, client](
                                       std::string uri,
                                       std::vector<LspClient::Diagnostic> diagnostics) {
      PublishLspDiagnostics(*project, std::move(uri), LspEncodingForClient(*client),
                            std::move(diagnostics));
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
                                       lsp_encoding::PositionEncoding encoding,
                                       std::vector<LspClient::Diagnostic> diagnostics) {
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }

  // Resolve the file's buffer for position-encoding conversion (server `character`
  // units -> editor byte columns). Null (file not open / utf-8) passes through.
  const editor::TextViewport* file_viewport = FindOpenEditorViewport(state, *path);
  const auto to_position = [&](const LspClient::Position& p) {
    const std::size_t line = static_cast<std::size_t>(std::max(p.line, 0));
    return editor::TextPosition{line, LspInboundColumn(file_viewport, line, p.character, encoding)};
  };

  std::vector<editor::Diagnostic> converted;
  converted.reserve(diagnostics.size());
  for (const auto& diagnostic : diagnostics) {
    converted.push_back(editor::Diagnostic{
        .range =
            editor::SelectionRange{
                .start = to_position(diagnostic.range.start),
                .end = to_position(diagnostic.range.end),
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
  const std::uint64_t generation = ++semantic_token_generation_[uri];
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(client);
  client.RequestSemanticTokensAsync(
      uri, [this, project, uri, generation, encoding, legend = std::move(legend)](
               std::optional<std::vector<LspClient::SemanticToken>> tokens) mutable {
        if (!tokens.has_value()) {
          return;
        }
        PublishLspSemanticTokens(*project, std::move(uri), generation, encoding, std::move(legend),
                                 std::move(*tokens));
      });
}

void LspService::PublishLspSemanticTokens(ProjectWorkspaceState& state, std::string uri,
                                          std::uint64_t request_generation,
                                          lsp_encoding::PositionEncoding encoding,
                                          std::vector<std::string> legend,
                                          std::vector<LspClient::SemanticToken> tokens) {
  if (theme_ == nullptr) {
    return;
  }
  // Drop a response the buffer has already moved past: any edit since this request
  // bumped the URI's generation (or cleared it), so painting these absolute
  // positions would corrupt the current text. Mirrors the revision guard in
  // TextViewport::InstallPrefetchedHighlights.
  if (const auto it = semantic_token_generation_.find(uri);
      it == semantic_token_generation_.end() || it->second != request_generation) {
    return;
  }
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }
  // Resolve the file's buffer once for position-encoding conversion (token
  // start_char/length are in the server's encoding units; the overlay stores byte
  // columns). Null (file not open / utf-8) => LspInboundColumn passes through.
  const editor::TextViewport* file_viewport = FindOpenEditorViewport(state, *path);
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
    const std::size_t line = static_cast<std::size_t>(token.line);
    const std::size_t start_byte = LspInboundColumn(file_viewport, line, token.start_char, encoding);
    // ParseSemanticTokensData bounds start_char/length to [0, INT_MAX] each, but
    // their sum can overflow a signed int; compute the end unit in int64 and clamp.
    const std::int64_t end_units =
        std::min<std::int64_t>(static_cast<std::int64_t>(token.start_char) + token.length,
                               std::numeric_limits<int>::max());
    const std::size_t end_byte =
        LspInboundColumn(file_viewport, line, static_cast<int>(end_units), encoding);
    style.start_column = static_cast<std::uint32_t>(start_byte);
    style.end_column = static_cast<std::uint32_t>(std::max(start_byte, end_byte));
    style.foreground = editor::SyntaxTokenColor(*theme_, kind, plain);  // a!=0 => recolor
    data.text_styles.push_back(style);
  }
  if (state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("lsp:semantic", *path,
                                                                       std::move(data))) {
    operations_.request_editor_surface_redraw();
  }
}

void LspService::ClearLspSemanticTokensForFile(const editor::TextViewport& viewport) {
  if (viewport.path().empty()) {
    return;
  }
  ProjectWorkspaceState& state = CurrentProjectState();
  auto* presentation = state.plugin_presentation.get();
  if (presentation == nullptr) {
    return;  // Nothing published yet -> nothing to invalidate.
  }
  if (presentation->decorations.ClearOwnerFile("lsp:semantic", viewport.path())) {
    state.MaybeReleasePluginPresentation();
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
  if (viewport == nullptr) {
    return;
  }
  SyncLspForBufferChange(*viewport, before_lines, after_lines);
}

void LspService::SyncLspForBufferChange(const editor::TextViewport& viewport,
                                        const std::vector<std::string>& before_lines,
                                        const std::vector<std::string>& after_lines) {
  if (viewport.path().empty()) {
    return;
  }

  // A bulk edit shifts the buffer geometry, so the absolute-positioned semantic
  // overlay is now stale -> drop it (the lexical layer keeps painting). Done
  // before the client early-out so a stale overlay is cleared even with no server.
  ClearLspSemanticTokensForFile(viewport);

  // Keep diagnostics positioned for the dirty buffer until the server republishes.
  ShiftLspDiagnosticsForBulkChange(viewport, before_lines, after_lines);
  std::string language_id;
  LspClient* client = LspClientForViewport(viewport, &language_id);
  if (client == nullptr) {
    return;
  }
  const std::string uri = FileUriForPath(viewport.path());
  // If the document is not open yet, EnsureLspDocumentOpen's didOpen carries the
  // CURRENT (already-edited) buffer text, so a following didChange would apply the
  // edit a second time and desync the server. Only send didChange when it was
  // already open at entry.
  const bool was_open = client->HasOpenDocument(uri);
  EnsureLspDocumentOpen(viewport, *client, language_id, uri);

  // Full-document sync for the bulk-change path. The per-keystroke path
  // (SyncLspForActiveEditableLastChange) sends true ranged incremental edits via
  // the viewport's last applied edit; here we only have before/after snapshots,
  // so a clean full replace is the correct, desync-proof choice. Full text needs
  // no per-column position-encoding conversion, so this stays correct for utf-16
  // servers too.
  if (was_open) {
    client->DidChange(uri, util::SerializeLines(after_lines, viewport.line_ending()));
  }
  // Re-request semantic tokens only when the edit left the buffer clean (e.g. an
  // undo landing on the saved point). The overlay is render-suppressed while
  // dirty, so requesting for a dirty buffer would paint nothing yet be superseded
  // by the next clean transition anyway.
  if (!viewport.dirty()) {
    RequestLspSemanticTokens(viewport, *client);
  }
}

void LspService::SyncLspForActiveEditableLastChange() {
  util::PerformanceTrace::Scope perf_scope("LspService::SyncLspForActiveEditableLastChange");
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }

  // This keystroke/undo shifted the buffer, so the absolute-positioned semantic
  // overlay is stale -> drop it (the lexical layer keeps painting). Cleared before
  // the client early-out so a stale overlay is dropped even with no server. While
  // the buffer is dirty the overlay is render-suppressed anyway; the clean-branch
  // re-request below repopulates it when an undo/redo lands on the saved point.
  ClearLspSemanticTokensForFile(*viewport);

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
  // See SyncLspForActiveEditableChange: if the doc was not open, the didOpen just
  // sent the current (already-edited) text, so a didChange would double-apply.
  const bool was_open = client->HasOpenDocument(uri);
  {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::EnsureDocumentOpen");
    EnsureLspDocumentOpen(*viewport, *client, language_id, uri);
  }

  const auto& applied_edit = viewport->last_applied_edit();
  if (applied_edit.has_value()) {
    // Slide stored diagnostics through this keystroke so they stay on their text
    // while the buffer is dirty, until the server republishes authoritative ranges.
    ShiftLspDiagnosticsForAppliedEdit(*viewport);
  }

  if (was_open) {
    // The incremental range carries PRE-edit byte columns; we no longer have the
    // pre-edit line text to re-encode them, so ranged incremental sync is only
    // safe when the server counts UTF-8 bytes (clangd's negotiated case — the hot
    // path). For utf-16/utf-32 servers, fall back to a full-document replace, which
    // needs no per-column encoding and stays desync-proof.
    const bool utf8_positions =
        LspEncodingForClient(*client) == lsp_encoding::PositionEncoding::Utf8;
    if (!applied_edit.has_value() || !client->SupportsIncrementalSync() || !utf8_positions) {
      util::PerformanceTrace::Scope scope(
          "LspService::SyncLspForActiveEditableLastChange::FullSync");
      client->DidChange(uri,
                        util::SerializeLines(viewport->lines().Snapshot(), viewport->line_ending()));
    } else {
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
  }

  // Repopulate the semantic overlay when the edit leaves the buffer clean (an
  // undo/redo landing on the saved point). Typing leaves the buffer dirty and so
  // stays request-free; the overlay is render-suppressed while dirty regardless.
  if (!viewport->dirty()) {
    RequestLspSemanticTokens(*viewport, *client);
  }
}

void LspService::ConsumeLspCallbacks() { CurrentLspManager().DrainCallbacks(); }

}  // namespace microide::workspace
