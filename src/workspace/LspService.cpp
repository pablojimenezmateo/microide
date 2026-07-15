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
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/FileUri.h"
#include "workspace/LanguageDetection.h"
#include "workspace/LspClientTrace.h"
#include "workspace/LspFeatureFlags.h"
#include "workspace/LspPositionEncoding.h"
#include "workspace/LspViewportPositions.h"
#include "workspace/WorkspaceCodeActionRegistry.h"
#include "workspace/WorkspaceCompletionRegistry.h"
#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

namespace {

// Derive the UI busy-indicator backstop from the single source of truth for the
// transport deadline (LspClientTrace.h) so the two can never drift. The indicator
// must not expire before the transport itself would fail the request — otherwise
// a slow-but-alive server makes the UI falsely report idle. A small margin lets
// the transport's own failure sweep normally resolve the callback first.
constexpr Uint64 kLspRequestTimeoutMs =
    static_cast<Uint64>(kLspRequestTimeout.count()) + 2000;
static_assert(kLspRequestTimeoutMs >= 30000,
              "the LSP busy-indicator backstop must not expire before the transport deadline");

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

void LspService::ReconcileFeatureSettings() {
  if (!operations_.get_setting_value) {
    return;
  }
  const auto& get = operations_.get_setting_value;
  const bool master = LspMasterEnabled(get);
  const bool diagnostics = master && SettingFlagEnabled(get("lsp.diagnostics.enabled"), true);
  const bool semantic = master && SettingFlagEnabled(get("lsp.semantic_tokens.enabled"), true);

  // Treat the first reconcile as a transition from "all on", so any feature that
  // starts disabled is cleared once, and only actual flips do work thereafter.
  const bool first = !last_feature_enablement_.has_value();
  const FeatureEnablement prev =
      last_feature_enablement_.value_or(FeatureEnablement{true, true, true});
  last_feature_enablement_ = FeatureEnablement{master, diagnostics, semantic};

  ProjectWorkspaceState& state = CurrentProjectState();

  if (!master) {
    // Master flipped off: stop the project's servers and drop every LSP-owned
    // decoration so nothing stale lingers. Lazy start resumes through
    // LspClientForViewport once the master is turned back on. Act only on entry.
    if (prev.master) {
      if (state.lsp_manager != nullptr) {
        state.lsp_manager->ShutdownAll();
      }
      semantic_token_generation_.clear();
      bool changed = state.diagnostics_store.ClearOwner("lsp");
      if (state.plugin_presentation != nullptr &&
          state.plugin_presentation->decorations.ClearOwner("lsp:semantic")) {
        state.MaybeReleasePluginPresentation();
        changed = true;
      }
      if (changed) {
        operations_.refresh_problems_sidebar();
        operations_.request_editor_surface_redraw();
      }
    }
    return;
  }

  editor::TextViewport* viewport = operations_.active_editable_viewport();
  const bool has_active = viewport != nullptr && !viewport->path().empty();

  // Master just turned back on: re-open the active document so the server starts and
  // republishes diagnostics + semantic tokens (EnsureLspDocumentOpen requests them).
  const bool master_turned_on = !first && !prev.master;
  if (master_turned_on && has_active) {
    std::string language_id;
    if (LspClient* client = LspClientForViewport(*viewport, &language_id); client != nullptr) {
      EnsureLspDocumentOpen(*viewport, *client, language_id);
    }
  }

  // Diagnostics turned off: clear stored "lsp" diagnostics (re-enable repopulates on
  // the next server publish / reopen).
  if (!diagnostics && prev.diagnostics) {
    if (state.diagnostics_store.ClearOwner("lsp")) {
      operations_.refresh_problems_sidebar();
      operations_.request_editor_surface_redraw();
    }
  }

  // Semantic highlighting: clear on disable; re-request on enable (unless the master
  // off->on reopen above already covered it).
  if (!semantic && prev.semantic) {
    semantic_token_generation_.clear();
    if (state.plugin_presentation != nullptr &&
        state.plugin_presentation->decorations.ClearOwner("lsp:semantic")) {
      state.MaybeReleasePluginPresentation();
      operations_.request_editor_surface_redraw();
    }
  } else if (semantic && !prev.semantic && !master_turned_on && has_active) {
    std::string language_id;
    if (LspClient* client = LspClientForViewport(*viewport, &language_id); client != nullptr) {
      EnsureLspDocumentOpen(*viewport, *client, language_id);
      RequestLspSemanticTokens(*viewport, *client);
    }
  }
}

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
  // Master off: report "Off" without starting a server. State stays at its default
  // (never Ready), so LSP-driven menu items that key off readiness stay inert.
  if (operations_.get_setting_value && !LspMasterEnabled(operations_.get_setting_value)) {
    snapshot.message = "Off";
    return snapshot;
  }
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
                                        std::string& tooltip, LspStatusSeverity* severity) {
  const auto set_severity = [severity](LspStatusSeverity value) {
    if (severity != nullptr) *severity = value;
  };
  if (operations_.get_setting_value && !LspMasterEnabled(operations_.get_setting_value)) {
    text = "LSP: Off";
    tooltip = "Language Server Protocol disabled in settings";
    set_severity(LspStatusSeverity::Idle);
    return;
  }
  const LspClient::ReadinessSnapshot snapshot = ActiveLspReadinessSnapshot(ensure_started);
  if (CurrentProjectState().lsp.request_in_flight_count > 0) {
    text = "LSP: working...";
    tooltip = "Language server request in progress";
    set_severity(LspStatusSeverity::Busy);
    return;
  }
  using State = LspClient::ReadinessSnapshot::State;
  switch (snapshot.state) {
    case State::Failed:
      set_severity(LspStatusSeverity::Error);
      break;
    case State::Starting:
    case State::Indexing:
      set_severity(LspStatusSeverity::Busy);
      break;
    case State::Idle:
    case State::Ready:
      set_severity(LspStatusSeverity::Idle);
      break;
  }
  const std::string readiness = LspReadinessMessage(snapshot);
  text = "LSP: " + readiness;
  tooltip = "Language server: " + readiness;
}

void LspService::BeginTrackedLspRequest() {
  auto& lsp = CurrentProjectState().lsp;
  ++lsp.request_in_flight_count;
  // Each new request extends the backstop (the conservative choice): the indicator
  // stays lit until the last outstanding request resolves or the transport deadline
  // passes for the most recent one.
  lsp.request_started_ticks = SDL_GetTicks();
  lsp.request_timeout_ticks = lsp.request_started_ticks + kLspRequestTimeoutMs;
  operations_.request_chrome_redraw();
  operations_.request_bottom_panel_redraw();
}

void LspService::FinishTrackedLspRequest() {
  auto& lsp = CurrentProjectState().lsp;
  if (lsp.request_in_flight_count == 0) {
    return;
  }
  --lsp.request_in_flight_count;
  if (lsp.request_in_flight_count == 0) {
    lsp.request_started_ticks = 0;
    lsp.request_timeout_ticks = 0;
  }
  operations_.request_chrome_redraw();
  operations_.request_bottom_panel_redraw();
}

void LspService::ExpireTrackedLspRequestIfNeeded() {
  auto& lsp = CurrentProjectState().lsp;
  if (lsp.request_in_flight_count == 0 || lsp.request_timeout_ticks == 0 ||
      SDL_GetTicks() < lsp.request_timeout_ticks) {
    return;
  }
  // The backstop only trips past the transport deadline, by which point the
  // transport sweep has already failed every outstanding request — so clear the
  // whole count, not just one.
  lsp.request_in_flight_count = 0;
  lsp.request_started_ticks = 0;
  lsp.request_timeout_ticks = 0;
  operations_.request_chrome_redraw();
  operations_.request_bottom_panel_redraw();
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
  // Master switch off: no client, so no DidOpen, no requests, and no lazy server
  // start. This is the single choke point every feature/hover/outline/semantic
  // request routes through, so gating here disables the whole subsystem.
  if (operations_.get_setting_value && !LspMasterEnabled(operations_.get_setting_value)) {
    if (language_id != nullptr) {
      language_id->clear();
    }
    return nullptr;
  }
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
  // Bind the server-initiated workspace/applyEdit handler once per client. Runs on
  // the main thread (buffer/disk mutation) when the server pushes an edit.
  if (!client->HasApplyEditHandler()) {
    client->SetApplyEditHandler(
        [this](LspClient::WorkspaceEdit edit) { return ApplyServerWorkspaceEdit(std::move(edit)); });
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
  // Pull inlay hints for the freshly-opened document (mid-line type/param hints).
  RequestLspInlayHints(viewport, client);
}

void LspService::PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
                                       lsp_encoding::PositionEncoding encoding,
                                       std::vector<LspClient::Diagnostic> diagnostics) {
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }

  // Diagnostics disabled (or master off): drop the push and clear any stale
  // "lsp" diagnostics already painted for this file.
  if (operations_.get_setting_value &&
      !LspFeatureEnabled(operations_.get_setting_value, "lsp.diagnostics.enabled")) {
    if (state.diagnostics_store.ClearOwnerFile("lsp", *path)) {
      operations_.refresh_problems_sidebar();
      operations_.request_editor_surface_redraw();
    }
    return;
  }

  // Resolve the file's buffer for position-encoding conversion (server `character`
  // units -> editor byte columns). Null (file not open / utf-8) passes through.
  const editor::TextViewport* file_viewport = FindOpenEditorViewport(state, *path);

  std::vector<editor::Diagnostic> converted;
  converted.reserve(diagnostics.size());
  for (const auto& diagnostic : diagnostics) {
    converted.push_back(editor::Diagnostic{
        .range = LspRangeToEditorRange(file_viewport, diagnostic.range, encoding),
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
  if (operations_.get_setting_value &&
      !LspFeatureEnabled(operations_.get_setting_value, "lsp.semantic_tokens.enabled")) {
    return;
  }
  ProjectWorkspaceState* const project = &CurrentProjectState();
  std::string uri = FileUriForPath(viewport.path());
  std::vector<std::string> legend = client.SemanticTokenLegend();
  if (legend.empty()) {
    return;
  }
  const std::uint64_t generation = NextOverlayGeneration(semantic_token_generation_, uri);
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

template <typename Item, typename Build>
void LspService::PublishGuardedOverlay(ProjectWorkspaceState& state, std::string_view owner_key,
                                       std::unordered_map<std::string, std::uint64_t>& generations,
                                       const std::string& uri, std::uint64_t request_generation,
                                       lsp_encoding::PositionEncoding encoding,
                                       std::vector<Item> items, Build build) {
  // Drop a response the buffer has already moved past: any edit since this request
  // bumped the URI's generation (or cleared it), so painting these absolute
  // positions would corrupt the current text. Mirrors the revision guard in
  // TextViewport::InstallPrefetchedHighlights.
  if (!OverlayGenerationCurrent(generations, uri, request_generation)) {
    return;
  }
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }
  // Resolve the file's buffer once for position-encoding conversion (item positions
  // are in the server's encoding units; overlays store byte columns). Null (file not
  // open / utf-8) => LspInboundColumn passes through.
  const editor::TextViewport* file_viewport = FindOpenEditorViewport(state, *path);
  editor::PluginDecorationData data = build(file_viewport, encoding, items);
  if (state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile(owner_key, *path,
                                                                       std::move(data))) {
    operations_.request_editor_surface_redraw();
  }
}

void LspService::PublishLspSemanticTokens(ProjectWorkspaceState& state, std::string uri,
                                          std::uint64_t request_generation,
                                          lsp_encoding::PositionEncoding encoding,
                                          std::vector<std::string> legend,
                                          std::vector<LspClient::SemanticToken> tokens) {
  if (theme_ == nullptr) {
    return;
  }
  PublishGuardedOverlay(
      state, "lsp:semantic", semantic_token_generation_, uri, request_generation, encoding,
      std::move(tokens),
      [this, legend = std::move(legend)](const editor::TextViewport* file_viewport,
                                         lsp_encoding::PositionEncoding encoding,
                                         std::vector<LspClient::SemanticToken>& tokens) {
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
          const std::size_t start_byte =
              LspInboundColumn(file_viewport, line, token.start_char, encoding);
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
        return data;
      });
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

void LspService::RequestLspInlayHints(const editor::TextViewport& viewport, LspClient& client) {
  if (viewport.path().empty() || !client.SupportsInlayHints()) {
    return;
  }
  // User gate: skip the round-trip entirely when inlay hints are disabled or the
  // LSP master switch is off.
  if (operations_.get_setting_value &&
      (!LspMasterEnabled(operations_.get_setting_value) ||
       !SettingFlagEnabled(operations_.get_setting_value("editor.inlay_hints.enabled")))) {
    return;
  }
  ProjectWorkspaceState* const project = &CurrentProjectState();
  std::string uri = FileUriForPath(viewport.path());
  // Whole-document range: start of file to one past the last line. Servers clamp
  // the end; line boundaries need no per-column position-encoding conversion.
  const std::size_t line_count = viewport.lines().size();
  LspClient::Range range{
      .start = LspClient::Position{0, 0},
      .end = LspClient::Position{static_cast<int>(line_count), 0},
  };
  const std::uint64_t generation = NextOverlayGeneration(inlay_hint_generation_, uri);
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(client);
  client.RequestInlayHintsAsync(
      uri, range,
      [this, project, uri, generation, encoding](
          std::optional<std::vector<LspClient::InlayHint>> hints) mutable {
        if (!hints.has_value()) {
          return;
        }
        PublishLspInlayHints(*project, std::move(uri), generation, encoding, std::move(*hints));
      });
}

void LspService::PublishLspInlayHints(ProjectWorkspaceState& state, std::string uri,
                                      std::uint64_t request_generation,
                                      lsp_encoding::PositionEncoding encoding,
                                      std::vector<LspClient::InlayHint> hints) {
  PublishGuardedOverlay(
      state, "lsp:inlay", inlay_hint_generation_, uri, request_generation, encoding,
      std::move(hints),
      [this](const editor::TextViewport* file_viewport, lsp_encoding::PositionEncoding encoding,
             std::vector<LspClient::InlayHint>& hints) {
        editor::PluginDecorationData data;
        data.inline_texts.reserve(hints.size());
        for (const LspClient::InlayHint& hint : hints) {
          if (hint.position.line < 0 || hint.position.character < 0) {
            continue;
          }
          const std::size_t line = static_cast<std::size_t>(hint.position.line);
          const std::size_t byte_column =
              LspInboundColumn(file_viewport, line, hint.position.character, encoding);
          editor::InlineTextDecoration inl;
          inl.line = static_cast<std::uint32_t>(line);
          inl.anchor_column = static_cast<std::uint32_t>(byte_column);
          // Bake padding into the drawn text so the grid cell-width measurement (and the
          // hit-test) account for it uniformly. Labels like "name:" / ": i32" already
          // read naturally; padding just adds a space of separation from the code.
          inl.text.reserve(hint.label.size() + 2);
          if (hint.padding_left) inl.text.push_back(' ');
          inl.text += hint.label;
          if (hint.padding_right) inl.text.push_back(' ');
          inl.color = theme_ != nullptr ? theme_->text_disabled : SDL_Color{128, 128, 128, 255};
          data.inline_texts.push_back(std::move(inl));
        }
        return data;
      });
}

void LspService::ClearLspInlayHintsForFile(const editor::TextViewport& viewport) {
  if (viewport.path().empty()) {
    return;
  }
  // Bump the generation FIRST, unconditionally: unlike the semantic overlay (which
  // the renderer suppresses while the buffer is dirty), inlay-hint InlineText
  // decorations paint in every state, so an in-flight response captured before this
  // edit would otherwise re-add hints at pre-edit positions on the now-shifted
  // buffer. Invalidating the generation makes PublishLspInlayHints drop it even if
  // there is currently no overlay to clear.
  NextOverlayGeneration(inlay_hint_generation_, FileUriForPath(viewport.path()));
  ProjectWorkspaceState& state = CurrentProjectState();
  auto* presentation = state.plugin_presentation.get();
  if (presentation == nullptr) {
    return;
  }
  if (presentation->decorations.ClearOwnerFile("lsp:inlay", viewport.path())) {
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
  TransformLspDiagnostics(
      viewport, [before_start, before_end, new_end](editor::SelectionRange range) {
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
  TransformLspDiagnostics(
      viewport, [first_changed, line_delta](editor::SelectionRange range) {
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

std::optional<LspService::BufferSyncTarget> LspService::ResolveOpenDocumentForSync(
    const editor::TextViewport& viewport) {
  BufferSyncTarget target;
  target.client = LspClientForViewport(viewport, &target.language_id);
  if (target.client == nullptr) {
    return std::nullopt;
  }
  // Compute the URI once (FileUriForPath normalizes + percent-encodes) and reuse it
  // for both the was-open check and the caller's DidChange.
  target.uri = FileUriForPath(viewport.path());
  // Capture BEFORE EnsureLspDocumentOpen: if the doc was not open, the didOpen it
  // sends carries the current (already-edited) text, so a following didChange would
  // double-apply. The caller gates its DidChange on was_open for exactly this reason.
  target.was_open = target.client->HasOpenDocument(target.uri);
  EnsureLspDocumentOpen(viewport, *target.client, target.language_id, target.uri);
  return target;
}

std::uint64_t LspService::NextOverlayGeneration(
    std::unordered_map<std::string, std::uint64_t>& generations, const std::string& uri) {
  return ++generations[uri];
}

bool LspService::OverlayGenerationCurrent(
    const std::unordered_map<std::string, std::uint64_t>& generations, const std::string& uri,
    std::uint64_t generation) {
  const auto it = generations.find(uri);
  return it != generations.end() && it->second == generation;
}

template <typename Transform>
void LspService::TransformLspDiagnostics(const editor::TextViewport& viewport,
                                         Transform&& transform) {
  CurrentProjectState().diagnostics_store.TransformOwnerFile("lsp", viewport.path(),
                                                             std::forward<Transform>(transform));
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
  ClearLspInlayHintsForFile(viewport);

  // Keep diagnostics positioned for the dirty buffer until the server republishes.
  // Runs before the client early-out so a dead/absent server never strands them.
  ShiftLspDiagnosticsForBulkChange(viewport, before_lines, after_lines);

  const std::optional<BufferSyncTarget> target = ResolveOpenDocumentForSync(viewport);
  if (!target.has_value()) {
    return;
  }

  // Full-document sync for the bulk-change path. The per-keystroke path
  // (SyncLspForActiveEditableLastChange) sends true ranged incremental edits via
  // the viewport's last applied edit; here we only have before/after snapshots,
  // so a clean full replace is the correct, desync-proof choice. Full text needs
  // no per-column position-encoding conversion, so this stays correct for utf-16
  // servers too. Only when the doc was already open (see ResolveOpenDocumentForSync).
  if (target->was_open) {
    target->client->DidChange(target->uri, util::SerializeLines(after_lines, viewport.line_ending()));
  }
  // Re-request semantic tokens only when the edit left the buffer clean (e.g. an
  // undo landing on the saved point). The overlay is render-suppressed while
  // dirty, so requesting for a dirty buffer would paint nothing yet be superseded
  // by the next clean transition anyway.
  if (!viewport.dirty()) {
    RequestLspSemanticTokens(viewport, *target->client);
    RequestLspInlayHints(viewport, *target->client);
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
  ClearLspInlayHintsForFile(*viewport);

  // Slide stored diagnostics through this keystroke so they stay on their text while
  // the buffer is dirty, until the server republishes authoritative ranges. Runs
  // before the client early-out (and self-guards on last_applied_edit) so a
  // dead/absent server never strands diagnostics on the pre-edit position.
  ShiftLspDiagnosticsForAppliedEdit(*viewport);

  std::optional<BufferSyncTarget> target;
  {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::ResolveAndOpen");
    target = ResolveOpenDocumentForSync(*viewport);
  }
  if (!target.has_value()) {
    return;
  }
  LspClient* const client = target->client;
  const std::string& uri = target->uri;
  const bool was_open = target->was_open;

  const auto& applied_edit = viewport->last_applied_edit();

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
      // The document was opened on the server with its own line ending
      // (SerializeLines(..., line_ending())), but replacement_text is built from
      // editor edit deltas that use bare-LF line breaks. For a CRLF document, send
      // the replacement re-encoded to CRLF so the server mirror does not drift toward
      // LF inside every edited range until the next full sync. LF documents (the
      // common case) skip this entirely.
      std::string replacement = applied_edit->replacement_text;
      if (viewport->line_ending() == util::LineEnding::CRLF &&
          replacement.find('\n') != std::string::npos) {
        std::string crlf;
        crlf.reserve(replacement.size() + 8);
        for (const char c : replacement) {
          if (c == '\n') {
            crlf.push_back('\r');
          }
          crlf.push_back(c);
        }
        replacement = std::move(crlf);
      }
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
          replacement);
    }
  }

  // Repopulate the semantic overlay when the edit leaves the buffer clean (an
  // undo/redo landing on the saved point). Typing leaves the buffer dirty and so
  // stays request-free; the overlay is render-suppressed while dirty regardless.
  if (!viewport->dirty()) {
    RequestLspSemanticTokens(*viewport, *client);
    RequestLspInlayHints(*viewport, *client);
  }
}

LspService::DiskEditResult LspService::ApplyLspEditsToClosedFilesOnDisk(
    const std::vector<CodeActionEdit>& edits,
    const std::function<bool(const std::filesystem::path&)>& is_open) {
  DiskEditResult result;

  // Group edits by normalized target path. An empty path targets the active
  // buffer (always open); paths the caller edits in place are skipped so undo
  // stays coherent for the buffers the user has on screen.
  std::vector<std::pair<std::filesystem::path,
                        std::vector<std::pair<editor::SelectionRange, std::string>>>>
      by_path;
  const auto bucket_for = [&](const std::filesystem::path& normalized)
      -> std::vector<std::pair<editor::SelectionRange, std::string>>& {
    for (auto& entry : by_path) {
      if (entry.first == normalized) {
        return entry.second;
      }
    }
    by_path.emplace_back(normalized,
                         std::vector<std::pair<editor::SelectionRange, std::string>>{});
    return by_path.back().second;
  };
  for (const CodeActionEdit& edit : edits) {
    if (edit.path.empty()) {
      continue;
    }
    const std::filesystem::path normalized = edit.path.lexically_normal();
    if (is_open && is_open(normalized)) {
      continue;
    }
    bucket_for(normalized).emplace_back(edit.range, edit.new_text);
  }

  for (auto& [path, file_edits] : by_path) {
    if (file_edits.empty()) {
      continue;
    }
    // Load into a scratch viewport so line-ending / BOM / encoding detection and
    // the atomic, permission-preserving save path are reused verbatim. This
    // viewport is never registered as a tab.
    editor::TextViewport scratch;
    if (!scratch.OpenFile(path)) {
      result.any_failed = true;
      continue;
    }
    // Resolve the server's position encoding for this file's language (the same
    // running server produced the WorkspaceEdit, so its encoding governs the
    // `character` offsets). FindStartedServer avoids spawning a server just to
    // rename a closed file; default to UTF-8 when none is running.
    const std::string language_id = DetectViewportLanguageId(scratch);
    LspClient* client = CurrentLspManager().FindStartedServer(language_id);
    const lsp_encoding::PositionEncoding encoding =
        client != nullptr ? LspEncodingForClient(*client) : lsp_encoding::PositionEncoding::Utf8;

    // Map 0-based LSP coordinates to editor byte columns. A line beyond EOF is a
    // hard reject (a buggy/hostile server must not silently mutate the last line
    // instead of the line it named), except the LSP end-of-document sentinel
    // `{line == line_count, character == 0}`, which maps to the end of the last
    // line. A `character` past the line end stays a soft clamp (LSP servers
    // routinely address one past the last column). `*ok` is set false on reject.
    const auto map_position = [&](editor::TextPosition pos, bool* ok) -> editor::TextPosition {
      *ok = true;
      const std::size_t line_count = scratch.line_count();
      if (line_count == 0) {
        *ok = false;
        return editor::TextPosition{0, 0};
      }
      if (pos.line == line_count && pos.column == 0) {
        const std::size_t last = line_count - 1;
        return editor::TextPosition{last, scratch.lines()[last].size()};
      }
      if (pos.line >= line_count) {
        *ok = false;
        return pos;
      }
      pos.column = lsp_encoding::LspCharacterToByteColumn(
          std::string_view(scratch.lines()[pos.line]), pos.column, encoding);
      return pos;
    };
    bool file_out_of_range = false;
    for (auto& [range, text] : file_edits) {
      bool start_ok = true;
      bool end_ok = true;
      range.start = map_position(range.start, &start_ok);
      range.end = map_position(range.end, &end_ok);
      if (!start_ok || !end_ok) {
        file_out_of_range = true;
        break;
      }
    }
    if (file_out_of_range) {
      // Reject the whole file's edit group rather than applying a partial,
      // position-corrupted set; leave the file on disk untouched.
      result.any_failed = true;
      continue;
    }
    // Apply highest-position-first (later array entry first on ties) so earlier
    // ranges stay valid as later ones apply — identical ordering to the open-buffer
    // path. No undo is recorded: the scratch buffer is written and discarded.
    std::vector<std::size_t> apply_order(file_edits.size());
    for (std::size_t i = 0; i < apply_order.size(); ++i) {
      apply_order[i] = i;
    }
    std::sort(apply_order.begin(), apply_order.end(), [&](std::size_t lhs, std::size_t rhs) {
      const editor::SelectionRange a = editor::TextViewport::NormalizeRange(file_edits[lhs].first);
      const editor::SelectionRange b = editor::TextViewport::NormalizeRange(file_edits[rhs].first);
      if (a.start.line != b.start.line) {
        return a.start.line > b.start.line;
      }
      if (a.start.column != b.start.column) {
        return a.start.column > b.start.column;
      }
      return lhs > rhs;
    });
    // Reject overlapping edits: applying two intersecting ranges (even
    // highest-first) double-edits shared bytes in an order-dependent way. In this
    // descending order consecutive entries run higher-start -> lower-start; the
    // lower-start edit overlaps the higher one when its end passes the higher
    // edit's start. Touching endpoints (adjacent edits) are allowed.
    bool overlapping = false;
    for (std::size_t i = 1; i < apply_order.size() && !overlapping; ++i) {
      const editor::SelectionRange hi =
          editor::TextViewport::NormalizeRange(file_edits[apply_order[i - 1]].first);
      const editor::SelectionRange lo =
          editor::TextViewport::NormalizeRange(file_edits[apply_order[i]].first);
      overlapping = lo.end.line > hi.start.line ||
                    (lo.end.line == hi.start.line && lo.end.column > hi.start.column);
    }
    if (overlapping) {
      result.any_failed = true;
      continue;
    }
    for (const std::size_t idx : apply_order) {
      scratch.ReplaceRange(file_edits[idx].first, file_edits[idx].second, /*record_undo=*/false);
    }
    if (!scratch.Save()) {
      result.any_failed = true;
      continue;
    }
    ++result.files_written;
    result.edits_applied += file_edits.size();
  }
  return result;
}

bool LspService::IsPathOpenInProject(const std::filesystem::path& normalized) const {
  // Same scan as FindOpenEditorViewport (which re-normalizes idempotently).
  return FindOpenEditorViewport(CurrentProjectState(), normalized) != nullptr;
}

bool LspService::ApplyServerWorkspaceEdit(LspClient::WorkspaceEdit edit) {
  // Flatten the URI-keyed WorkspaceEdit into the shared 0-based edit records; both
  // appliers map the LSP `character` offsets through the position encoding.
  std::vector<CodeActionEdit> flat;
  const std::filesystem::path project_root = CurrentProjectState().root;
  for (const auto& [uri, text_edits] : edit.changes) {
    const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
    if (!path.has_value()) {
      continue;
    }
    // A server-initiated workspace edit may only target files inside the active
    // project root (or a buffer already open in this project). Without this a buggy
    // or compromised language server could request writes to any file the user
    // account can reach (e.g. file:///etc/passwd on a writable fixture). The disk
    // applier below writes closed targets silently, so the containment check must
    // happen here rather than trusting the URI.
    const std::filesystem::path normalized = path->lexically_normal();
    if (!util::PathEqualsOrWithin(normalized, project_root) &&
        !IsPathOpenInProject(normalized)) {
      continue;
    }
    for (const auto& [range, new_text] : text_edits) {
      flat.push_back(CodeActionEdit{
          .path = *path,
          .range = editor::SelectionRange{
              .start = editor::TextPosition{static_cast<std::size_t>(std::max(0, range.start.line)),
                                            static_cast<std::size_t>(std::max(0, range.start.character))},
              .end = editor::TextPosition{static_cast<std::size_t>(std::max(0, range.end.line)),
                                          static_cast<std::size_t>(std::max(0, range.end.character))},
          },
          .new_text = new_text,
      });
    }
  }
  if (flat.empty()) {
    return false;
  }
  bool applied = false;
  // Open buffers edit in place (the shell resolves + re-syncs them); closed files
  // are written silently on disk. Passing the full set to both is safe: the
  // open-buffer applier skips paths that are not open, and the disk applier skips
  // paths that are open.
  if (operations_.apply_workspace_edit_to_open_buffers) {
    applied = operations_.apply_workspace_edit_to_open_buffers(flat);
  }
  const DiskEditResult disk = ApplyLspEditsToClosedFilesOnDisk(
      flat, [this](const std::filesystem::path& normalized) {
        return IsPathOpenInProject(normalized);
      });
  return applied || disk.files_written > 0;
}

void LspService::ConsumeLspCallbacks() { CurrentLspManager().DrainCallbacks(); }

}  // namespace microide::workspace
