#include "workspace/lsp/LspService.h"

#include "workspace/lsp/LspWorkspaceEditOps.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/DiagnosticsStore.h"
#include "editor/EditBatchOrder.h"
#include "editor/PluginDecorationStore.h"
#include "editor/SyntaxHighlighter.h"
#include "render/Theme.h"
#include "util/PathMatch.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/FileUri.h"
#include "workspace/lsp/LspClientTrace.h"
#include "workspace/lsp/LspFeatureFlags.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "workspace/registries/WorkspaceCodeActionRegistry.h"
#include "workspace/registries/WorkspaceCompletionRegistry.h"
#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

namespace {

constexpr Uint64 kLspRequestTimeoutMs =
    static_cast<Uint64>(kLspRequestTimeout.count()) + 2000;
static_assert(kLspRequestTimeoutMs >= 30000,
              "the LSP busy-indicator backstop must not expire before the transport deadline");

const editor::TextViewport* FindOpenEditorViewport(const ProjectWorkspaceState& state,
                                                   const std::filesystem::path& path) {
  std::filesystem::path normalized_storage;
  const std::filesystem::path* normalized_ptr = &path;
  if (util::PathTextNeedsNormalizing(path.native())) {
    normalized_storage = path.lexically_normal();
    normalized_ptr = &normalized_storage;
  }
  return OpenEditorViewOfPath(state.editor_groups, *normalized_ptr);
}

editor::SyntaxTokenKind SyntaxKindForSemanticType(std::string_view type) {
  if (type == "keyword" || type == "modifier") return editor::SyntaxTokenKind::Keyword;
  if (type == "type" || type == "class" || type == "struct" || type == "interface" ||
      type == "enum" || type == "typeParameter" || type == "namespace" ||
      type == "concept") {
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

}

void LspService::Configure(WorkspaceContext& context, CompletionRegistry& completion_registry,
                           CodeActionRegistry& code_action_registry, Operations operations) {
  context_ = &context;
  completion_registry_ = &completion_registry;
  code_action_registry_ = &code_action_registry;
  operations_ = std::move(operations);
}

void LspService::SetWakeChannel(Uint32 event_type) { wake_event_type_ = event_type; }

void LspService::SetTheme(const render::Theme* theme) { theme_ = theme; }

void LspService::ReconcileFeatureSettings() {
  if (!operations_.get_setting_value) {
    return;
  }
  const auto& get = operations_.get_setting_value;
  const bool master = LspMasterEnabled(get);
  const bool diagnostics = master && SettingFlagEnabled(get("lsp.diagnostics.enabled"), true);
  const bool semantic = master && SettingFlagEnabled(get("lsp.semantic_tokens.enabled"), true);
  const bool inlay = master && SettingFlagEnabled(get("editor.inlay_hints.enabled"), true);
  const bool code_lens = master && SettingFlagEnabled(get("lsp.code_lens.enabled"), true);
  const bool document_highlight =
      master && SettingFlagEnabled(get("lsp.document_highlight.enabled"), true);

  const bool first = !last_feature_enablement_.has_value();
  const FeatureEnablement prev = last_feature_enablement_.value_or(
      FeatureEnablement{true, true, true, true, true, true});
  last_feature_enablement_ =
      FeatureEnablement{master, diagnostics, semantic, inlay, code_lens, document_highlight};

  ProjectWorkspaceState& state = CurrentProjectState();

  if (!master) {
    if (prev.master) {
      if (state.lsp_manager != nullptr) {
        RetireClients(state.lsp_manager->BeginShutdownAllAndTakeClients());
      }
      bool changed = state.diagnostics_store.ClearOwner("lsp");
      changed = ClearAllLspOverlays(state) || changed;
      if (changed) {
        operations_.refresh_problems_sidebar();
        operations_.request_frame();
      }
    }
    return;
  }

  editor::TextViewport* viewport = operations_.active_editable_viewport();
  const bool has_active = viewport != nullptr && !viewport->path().empty();

  const bool master_turned_on = !first && !prev.master;
  if (master_turned_on && has_active) {
    std::string language_id;
    if (LspClient* client = LspClientForViewport(*viewport, &language_id); client != nullptr) {
      EnsureLspDocumentOpen(*viewport, *client, language_id);
    }
  }

  if (!diagnostics && prev.diagnostics) {
    if (state.diagnostics_store.ClearOwner("lsp")) {
      operations_.refresh_problems_sidebar();
      operations_.request_frame();
    }
  }

  const auto apply_overlay_toggle = [&](bool enabled, bool was_enabled, std::string_view owner,
                                        const auto& forget, const auto& request) {
    if (!enabled && was_enabled) {
      if (state.plugin_presentation != nullptr &&
          state.plugin_presentation->decorations.ClearOwner(owner)) {
        state.MaybeReleasePluginPresentation();
        operations_.request_frame();
      }
      forget();
    } else if (enabled && !was_enabled && !master_turned_on && has_active) {
      std::string language_id;
      if (LspClient* client = LspClientForViewport(*viewport, &language_id); client != nullptr) {
        EnsureLspDocumentOpen(*viewport, *client, language_id);
        request(*client);
      }
    }
  };
  apply_overlay_toggle(
      semantic, prev.semantic, "lsp:semantic", [this]() { semantic_token_generation_.clear(); },
      [&](LspClient& client) { RequestLspSemanticTokens(*viewport, client); });
  apply_overlay_toggle(
      inlay, prev.inlay, "lsp:inlay", [this]() { inlay_hint_generation_.clear(); },
      [&](LspClient& client) { RequestLspInlayHints(*viewport, client); });
  apply_overlay_toggle(
      code_lens, prev.code_lens, "lsp:codelens",
      [this]() {
        code_lens_generation_.clear();
        code_lens_commands_.clear();
      },
      [&](LspClient& client) { RequestLspCodeLenses(*viewport, client); });

  if (!document_highlight && prev.document_highlight && !state.semantic_occurrences.empty()) {
    state.semantic_occurrences.Clear();
    operations_.request_frame();
  }
}

bool LspService::ClearAllLspOverlays(ProjectWorkspaceState& state) {
  semantic_token_generation_.clear();
  inlay_hint_generation_.clear();
  code_lens_generation_.clear();
  code_lens_commands_.clear();
  ++document_highlight_generation_;
  document_highlight_request_valid_ = false;

  bool changed = false;
  if (!state.semantic_occurrences.empty()) {
    state.semantic_occurrences.Clear();
    changed = true;
  }
  if (state.plugin_presentation != nullptr) {
    for (const char* owner : {"lsp:semantic", "lsp:inlay", "lsp:codelens"}) {
      changed = state.plugin_presentation->decorations.ClearOwner(owner) || changed;
    }
    if (changed) {
      state.MaybeReleasePluginPresentation();
    }
  }
  if (changed) {
    operations_.request_frame();
  }
  return changed;
}

ProjectWorkspaceState& LspService::CurrentProjectState() {
  return context_->current_project_state;
}

std::string LspService::ActiveLanguageIdForProvider() const {
  const editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return {};
  }
  return viewport->language_id();
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

bool LspService::HasActivePluginDefinitionProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() && operations_.plugin_has_definition_provider &&
         operations_.plugin_has_definition_provider(language_id);
}

bool LspService::HasActivePluginReferencesProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() && operations_.plugin_has_references_provider &&
         operations_.plugin_has_references_provider(language_id);
}

bool LspService::HasActiveLanguageServer() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() && CurrentLspManager().HasServer(language_id);
}

bool LspService::HasActiveDefinitionProvider() const {
  if (HasActivePluginDefinitionProvider()) {
    return true;
  }
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() && CurrentLspManager().HasServer(language_id);
}

bool LspService::HasActiveReferencesProvider() const {
  if (HasActivePluginReferencesProvider()) {
    return true;
  }
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() && CurrentLspManager().HasServer(language_id);
}

LspClient::ReadinessSnapshot LspService::ActiveLspReadinessSnapshot(bool ensure_started) {
  ExpireTrackedLspRequestIfNeeded();

  LspClient::ReadinessSnapshot snapshot;
  if (operations_.get_setting_value && !LspMasterEnabled(operations_.get_setting_value)) {
    snapshot.message = "Off";
    return snapshot;
  }
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return snapshot;
  }

  const std::string& language_id = viewport->language_id();
  if (language_id.empty()) {
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
    return snapshot;
  }

  return client->GetReadinessSnapshot();
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
    text = "LSP: Working…";
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
  std::string scratch;
  const std::string_view readiness = LspReadinessText(snapshot, scratch);
  text = "LSP: ";
  text += readiness;
  tooltip = "Language server: ";
  tooltip += readiness;
  if (!snapshot.message.empty() && snapshot.message != readiness) {
    tooltip += " — ";
    tooltip += snapshot.message;
  }
}

void LspService::BeginTrackedLspRequest() {
  auto& lsp = CurrentProjectState().lsp;
  ++lsp.request_in_flight_count;
  lsp.request_started_ticks = SDL_GetTicks();
  lsp.request_timeout_ticks = lsp.request_started_ticks + kLspRequestTimeoutMs;
  operations_.request_frame();
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
  operations_.request_frame();
}

void LspService::ExpireTrackedLspRequestIfNeeded() {
  auto& lsp = CurrentProjectState().lsp;
  if (lsp.request_in_flight_count == 0 || lsp.request_timeout_ticks == 0 ||
      SDL_GetTicks() < lsp.request_timeout_ticks) {
    return;
  }
  lsp.request_in_flight_count = 0;
  lsp.request_started_ticks = 0;
  lsp.request_timeout_ticks = 0;
  operations_.request_frame();
}

LspManager& LspService::EnsureProjectLspManager(ProjectWorkspaceState& state) {
  if (state.lsp_manager == nullptr) {
    state.lsp_manager = std::make_unique<LspManager>();
  }
  if (wake_event_type_ != 0) {
    state.lsp_manager->SetWakeChannel(wake_event_type_);
  }
  return *state.lsp_manager;
}

LspManager& LspService::CurrentLspManager() { return EnsureProjectLspManager(CurrentProjectState()); }

const LspManager& LspService::CurrentLspManager() const {
  return const_cast<LspService*>(this)->CurrentLspManager();
}

std::size_t LspService::NotifyWatchedFileChanges(
    const std::vector<project::ProjectFileChange>& changes) {
  if (changes.empty() || context_ == nullptr) {
    return 0;
  }
  if (operations_.get_setting_value && !LspMasterEnabled(operations_.get_setting_value)) {
    return 0;
  }
  ProjectWorkspaceState& state = CurrentProjectState();
  if (state.lsp_manager == nullptr || !state.lsp_manager->HasRegisteredServers()) {
    return 0;
  }

  util::PerformanceTrace::Scope perf_scope("LspService::NotifyWatchedFileChanges");

  std::vector<LspManager::WatchedFileChange> events;
  events.reserve(changes.size());
  for (const project::ProjectFileChange& change : changes) {
    LspManager::WatchedFileChange event;
    switch (change.kind) {
      case project::ProjectFileChangeKind::Created:
        event.type = LspFileChangeType::Created;
        break;
      case project::ProjectFileChangeKind::Deleted:
        event.type = LspFileChangeType::Deleted;
        break;
      case project::ProjectFileChangeKind::Modified:
        event.type = LspFileChangeType::Changed;
        break;
    }
    event.relative_path = change.relative_path.generic_string();
    event.absolute_path = change.absolute_path.generic_string();
    event.uri = FileUriForPath(change.absolute_path);
    events.push_back(std::move(event));
  }
  return state.lsp_manager->NotifyWatchedFileChanges(events);
}

LspClient* LspService::LspClientForViewport(const editor::TextViewport& viewport,
                                            std::string* language_id) {
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
  const std::string& detected_language = viewport.language_id();
  if (language_id != nullptr) {
    *language_id = detected_language;
  }
  if (detected_language.empty()) {
    return nullptr;
  }

  ProjectWorkspaceState* const project = &CurrentProjectState();
  LspClient* client = CurrentLspManager().GetServer(detected_language);
  if (client == nullptr) {
    return nullptr;
  }
  if (!client->HasDiagnosticsCallback()) {
    client->SetDiagnosticsCallback([this, project, client](
                                       std::string uri,
                                       std::vector<LspClient::Diagnostic> diagnostics) {
      PublishLspDiagnostics(*project, std::move(uri), LspEncodingForClient(*client),
                            std::move(diagnostics));
    });
  }
  if (!client->HasApplyEditHandler()) {
    client->SetApplyEditHandler([this, client](LspClient::WorkspaceEdit edit) {
      return operations_.apply_server_workspace_edit &&
             operations_.apply_server_workspace_edit(*client, std::move(edit));
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
  client.DidOpen(uri, std::string(language_id), viewport.SerializeText());
  synced_content_revision_[uri] = viewport.content_revision();
  RequestLspSemanticTokens(viewport, client);
  RequestLspInlayHints(viewport, client);
  RequestLspCodeLenses(viewport, client);
}

void LspService::ScheduleBufferOpen(const std::filesystem::path& path) {
  std::filesystem::path normalized = path.lexically_normal();
  if (normalized.empty()) {
    return;
  }
  pending_buffer_open_ = std::move(normalized);
}

bool LspService::ConsumeDeferredBufferOpen() {
  if (!pending_buffer_open_.has_value()) {
    return false;
  }
  const std::filesystem::path path = std::move(*pending_buffer_open_);
  pending_buffer_open_.reset();
  editor::TextViewport* viewport =
      operations_.active_editable_viewport ? operations_.active_editable_viewport() : nullptr;
  if (viewport == nullptr || !util::SameAsNormalizedPath(viewport->path(), path)) {
    return false;
  }
  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    return false;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);
  return true;
}

void LspService::PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
                                       lsp_encoding::PositionEncoding encoding,
                                       std::vector<LspClient::Diagnostic> diagnostics) {
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }

  if (operations_.get_setting_value &&
      !LspFeatureEnabled(operations_.get_setting_value, "lsp.diagnostics.enabled")) {
    if (state.diagnostics_store.ClearOwnerFile("lsp", *path)) {
      operations_.refresh_problems_sidebar();
      operations_.request_frame();
    }
    return;
  }

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
    operations_.request_frame();
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
               LspResult<std::vector<LspClient::SemanticToken>> tokens) mutable {
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
  if (!OverlayGenerationCurrent(generations, uri, request_generation)) {
    return;
  }
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }
  const editor::TextViewport* file_viewport = FindOpenEditorViewport(state, *path);
  editor::PluginDecorationData data = build(file_viewport, encoding, items);
  if (state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile(owner_key, *path,
                                                                       std::move(data))) {
    operations_.request_frame();
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
            continue;
          }
          editor::TextStyleDecoration style;
          style.line = static_cast<std::uint32_t>(token.line);
          const std::size_t line = static_cast<std::size_t>(token.line);
          const std::size_t start_byte =
              LspInboundColumn(file_viewport, line, token.start_char, encoding);
          const std::int64_t end_units =
              std::min<std::int64_t>(static_cast<std::int64_t>(token.start_char) + token.length,
                                     std::numeric_limits<int>::max());
          const std::size_t end_byte =
              LspInboundColumn(file_viewport, line, static_cast<int>(end_units), encoding);
          style.start_column = static_cast<std::uint32_t>(start_byte);
          style.end_column = static_cast<std::uint32_t>(std::max(start_byte, end_byte));
          style.foreground = editor::SyntaxTokenColor(*theme_, kind, plain);
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
    return;
  }
  if (presentation->decorations.ClearOwnerFile("lsp:semantic", viewport.path())) {
    state.MaybeReleasePluginPresentation();
    operations_.request_frame();
  }
}

void LspService::RequestLspInlayHints(const editor::TextViewport& viewport, LspClient& client) {
  if (viewport.path().empty() || !client.SupportsInlayHints()) {
    return;
  }
  if (operations_.get_setting_value &&
      (!LspMasterEnabled(operations_.get_setting_value) ||
       !SettingFlagEnabled(operations_.get_setting_value("editor.inlay_hints.enabled"),
                           true))) {
    return;
  }
  ProjectWorkspaceState* const project = &CurrentProjectState();
  std::string uri = FileUriForPath(viewport.path());
  const std::size_t line_count = viewport.lines().size();
  LspClient::Range range{
      .start = LspClient::Position{0, 0},
      .end = LspClient::Position{SaturateToLspInt(line_count), 0},
  };
  const std::uint64_t generation = NextOverlayGeneration(inlay_hint_generation_, uri);
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(client);
  client.RequestInlayHintsAsync(
      uri, range,
      [this, project, uri, generation, encoding](
          LspResult<std::vector<LspClient::InlayHint>> hints) mutable {
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

namespace {
constexpr std::size_t kMaxResolvedCodeLenses = 256;

struct CodeLensResolveBatch {
  std::vector<LspClient::CodeLens> lenses;
  std::size_t pending = 0;
};
}

void LspService::RequestLspCodeLenses(const editor::TextViewport& viewport, LspClient& client) {
  if (viewport.path().empty() || !client.SupportsCodeLens()) {
    return;
  }
  if (operations_.get_setting_value &&
      !LspFeatureEnabled(operations_.get_setting_value, "lsp.code_lens.enabled")) {
    return;
  }
  ProjectWorkspaceState* const project = &CurrentProjectState();
  std::string uri = FileUriForPath(viewport.path());
  std::string language_id = viewport.language_id();
  const std::uint64_t generation = NextOverlayGeneration(code_lens_generation_, uri);
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(client);
  const bool can_resolve = client.SupportsCodeLensResolve();
  LspClient* const client_ptr = &client;
  client.RequestCodeLensAsync(
      uri, [this, project, client_ptr, uri, language_id, generation, encoding, can_resolve](
               LspResult<std::vector<LspClient::CodeLens>> lenses) mutable {
        if (!lenses.has_value()) {
          return;
        }
        const bool any_unresolved =
            can_resolve && std::any_of(lenses->begin(), lenses->end(),
                                       [](const LspClient::CodeLens& lens) {
                                         return lens.needs_resolve();
                                       });
        if (!any_unresolved) {
          std::erase_if(*lenses, [](const LspClient::CodeLens& lens) { return lens.title.empty(); });
          PublishLspCodeLenses(*project, std::move(uri), generation, std::move(language_id),
                               encoding, std::move(*lenses));
          return;
        }

        auto batch = std::make_shared<CodeLensResolveBatch>();
        batch->lenses = std::move(*lenses);
        std::vector<std::size_t> to_resolve;
        for (std::size_t i = 0; i < batch->lenses.size(); ++i) {
          if (!batch->lenses[i].needs_resolve()) {
            continue;
          }
          if (to_resolve.size() >= kMaxResolvedCodeLenses) {
            batch->lenses[i].unresolved = util::JsonValue{};
            continue;
          }
          to_resolve.push_back(i);
        }
        batch->pending = to_resolve.size();
        for (const std::size_t index : to_resolve) {
          client_ptr->ResolveCodeLensAsync(
              batch->lenses[index].unresolved,
              [this, project, batch, index, uri, language_id, generation, encoding](
                  LspResult<LspClient::CodeLens> resolved) mutable {
                if (resolved.has_value() && !resolved->title.empty()) {
                  const LspClient::Range range = batch->lenses[index].range;
                  batch->lenses[index] = std::move(*resolved);
                  batch->lenses[index].range = range;
                }
                if (--batch->pending != 0) {
                  return;
                }
                std::erase_if(batch->lenses,
                              [](const LspClient::CodeLens& lens) { return lens.title.empty(); });
                PublishLspCodeLenses(*project, std::move(uri), generation, std::move(language_id),
                                     encoding, std::move(batch->lenses));
              });
        }
      });
}

void LspService::PublishLspCodeLenses(ProjectWorkspaceState& state, std::string uri,
                                      std::uint64_t request_generation, std::string language_id,
                                      lsp_encoding::PositionEncoding encoding,
                                      std::vector<LspClient::CodeLens> lenses) {
  PublishGuardedOverlay(
      state, "lsp:codelens", code_lens_generation_, uri, request_generation, encoding,
      std::move(lenses),
      [this, &uri, &language_id](const editor::TextViewport* ,
                                 lsp_encoding::PositionEncoding ,
                                 std::vector<LspClient::CodeLens>& lenses) {
        std::erase_if(code_lens_commands_,
                      [&uri](const auto& entry) { return entry.second.uri == uri; });
        editor::PluginDecorationData data;
        data.code_lenses.reserve(lenses.size());
        for (LspClient::CodeLens& lens : lenses) {
          if (lens.range.start.line < 0) {
            continue;
          }
          editor::CodeLensDecoration decoration;
          decoration.line = static_cast<std::uint32_t>(lens.range.start.line);
          decoration.text = std::move(lens.title);
          if (!lens.command.empty()) {
            decoration.payload = ++next_code_lens_payload_;
            code_lens_commands_.emplace(
                decoration.payload,
                CodeLensCommand{.uri = uri,
                                .language_id = language_id,
                                .command = std::move(lens.command),
                                .arguments = std::move(lens.arguments)});
          }
          data.code_lenses.push_back(std::move(decoration));
        }
        return data;
      });
}

void LspService::ActivateCodeLens(std::uint64_t payload) {
  const auto it = code_lens_commands_.find(payload);
  if (it == code_lens_commands_.end()) {
    return;
  }
  LspClient* const client = CurrentLspManager().FindStartedServer(it->second.language_id);
  if (client == nullptr) {
    return;
  }
  BeginTrackedLspRequest();
  client->ExecuteServerCommandAsync(
      it->second.command, it->second.arguments,
      [this](LspResult<util::JsonValue>) { FinishTrackedLspRequest(); });
}


void LspService::MaybeRequestDocumentHighlights() {
  if (!operations_.active_editable_viewport) {
    return;
  }
  editor::TextViewport* const viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty() || viewport->is_placeholder()) {
    return;
  }
  if (operations_.get_setting_value &&
      (!LspFeatureEnabled(operations_.get_setting_value, "lsp.document_highlight.enabled") ||
       !SettingFlagEnabled(operations_.get_setting_value("editor.occurrences.enabled"), true))) {
    return;
  }
  if (viewport->CaretIsFromActiveTextEdit() && !viewport->selection_range().has_value()) {
    return;
  }

  const std::uint64_t revision = viewport->content_revision();
  const std::size_t caret_line = viewport->cursor_line();
  const std::size_t caret_column = viewport->cursor_column();
  if (document_highlight_request_valid_ && document_highlight_request_revision_ == revision &&
      document_highlight_request_document_ == viewport->document_identity() &&
      document_highlight_request_line_ == caret_line &&
      document_highlight_request_column_ == caret_column &&
      document_highlight_request_path_ == viewport->path()) {
    return;
  }
  if (CurrentProjectState().semantic_occurrences.CoversCaret(viewport->path(), viewport->document_identity(),
                                                             revision,
                                                             caret_line, caret_column)) {
    return;
  }

  std::string language_id;
  LspClient* const client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr || !client->SupportsDocumentHighlight()) {
    return;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);

  document_highlight_request_valid_ = true;
  document_highlight_request_path_ = viewport->path();
  document_highlight_request_revision_ = revision;
  document_highlight_request_document_ = viewport->document_identity();
  document_highlight_request_line_ = caret_line;
  document_highlight_request_column_ = caret_column;

  ProjectWorkspaceState* const project = &CurrentProjectState();
  const std::uint64_t generation = ++document_highlight_generation_;
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  client->RequestDocumentHighlightAsync(
      FileUriForPath(viewport->path()),
      ByteColumnToLspPosition(*viewport, caret_line, caret_column, encoding),
      [this, project, path = viewport->path(), document = viewport->document_identity(), generation,
       revision, encoding, caret_line, caret_column](LspResult<std::vector<LspClient::DocumentHighlight>> highlights) {
        if (!highlights.has_value()) {
          return;
        }
        PublishLspDocumentHighlights(*project, path, document, generation, revision, encoding,
                                     caret_line, caret_column, std::move(*highlights));
      });
}

void LspService::PublishLspDocumentHighlights(
    ProjectWorkspaceState& state, const std::filesystem::path& path, const void* document,
    std::uint64_t request_generation, std::uint64_t content_revision,
    lsp_encoding::PositionEncoding encoding, std::size_t caret_line, std::size_t caret_column,
    std::vector<LspClient::DocumentHighlight> highlights) {
  if (request_generation != document_highlight_generation_) {
    return;
  }
  const editor::TextViewport* const file_viewport = FindOpenEditorViewport(state, path);
  if (file_viewport == nullptr || file_viewport->content_revision() != content_revision ||
      file_viewport->document_identity() != document) {
    return;
  }

  auto& store = state.semantic_occurrences;
  store.path = path;
  store.document = document;
  store.content_revision = content_revision;
  store.ranges.clear();
  store.ranges.reserve(highlights.size());
  for (const LspClient::DocumentHighlight& highlight : highlights) {
    if (highlight.range.start.line != highlight.range.end.line || highlight.range.start.line < 0) {
      continue;
    }
    const std::size_t line = static_cast<std::size_t>(highlight.range.start.line);
    const std::size_t start =
        LspInboundColumn(file_viewport, line, highlight.range.start.character, encoding);
    const std::size_t end =
        LspInboundColumn(file_viewport, line, highlight.range.end.character, encoding);
    if (start >= end) {
      continue;
    }
    store.ranges.push_back(editor::OccurrenceRange{
        .line_index = line,
        .start_column = start,
        .end_column = end,
        .is_primary_seed = line == caret_line && caret_column >= start && caret_column <= end,
        .kind = highlight.kind == 3   ? editor::OccurrenceKind::Write
                : highlight.kind == 2 ? editor::OccurrenceKind::Read
                                      : editor::OccurrenceKind::Text,
    });
  }
  std::sort(store.ranges.begin(), store.ranges.end(),
            [](const editor::OccurrenceRange& lhs, const editor::OccurrenceRange& rhs) {
              return lhs.line_index != rhs.line_index ? lhs.line_index < rhs.line_index
                                                      : lhs.start_column < rhs.start_column;
            });
  if (store.ranges.empty()) {
    store.Clear();
  }
  operations_.request_frame();
}

void LspService::ClearLspEditOverlaysForFile(const editor::TextViewport& viewport) {
  if (viewport.path().empty()) {
    return;
  }
  const bool any_inlay_request = !inlay_hint_generation_.empty();
  const bool any_lens_state = !code_lens_generation_.empty() || !code_lens_commands_.empty();
  if (any_inlay_request || any_lens_state) {
    const std::string uri = FileUriForPath(viewport.path());
    if (any_inlay_request) {
      NextOverlayGeneration(inlay_hint_generation_, uri);
    }
    if (any_lens_state) {
      NextOverlayGeneration(code_lens_generation_, uri);
      std::erase_if(code_lens_commands_,
                    [&uri](const auto& entry) { return entry.second.uri == uri; });
    }
  }
  ProjectWorkspaceState& state = CurrentProjectState();
  auto* presentation = state.plugin_presentation.get();
  if (presentation == nullptr) {
    return;
  }
  const bool cleared_inlay = presentation->decorations.ClearOwnerFile("lsp:inlay", viewport.path());
  const bool cleared_lens =
      presentation->decorations.ClearOwnerFile("lsp:codelens", viewport.path());
  if (cleared_inlay || cleared_lens) {
    state.MaybeReleasePluginPresentation();
    operations_.request_frame();
  }
}

void LspService::ShiftLspDiagnosticsForAppliedEdit(const editor::TextViewport& viewport,
                                                   std::optional<std::uint64_t> followed_revision) {
  if (viewport.path().empty()) {
    return;
  }
  const editor::AppliedEdit* applied =
      followed_revision.has_value() ? viewport.AppliedEditFromRevision(*followed_revision) : nullptr;
  if (applied == nullptr) {
    applied = viewport.LatestAppliedEdit();
  }
  if (applied == nullptr) {
    return;
  }
  const editor::SelectionRange before =
      editor::TextViewport::NormalizeRange(applied->range_before);
  if (before.start == before.end && applied->replacement_text.empty()) {
    return;
  }
  TransformLspDiagnostics(viewport, [&edit = *applied](editor::SelectionRange range) {
    range.start = editor::MapPositionThroughEdit(range.start, edit);
    range.end = editor::MapPositionThroughEdit(range.end, edit);
    return range;
  });
}

void LspService::ShiftLspDiagnosticsForBulkChange(const editor::TextViewport& viewport,
                                                  std::size_t before_line_count,
                                                  std::size_t first_changed_line) {
  const std::size_t after_line_count = viewport.line_count();
  if (viewport.path().empty() || before_line_count == after_line_count) {
    return;
  }
  const std::size_t first_changed = first_changed_line;
  const std::ptrdiff_t line_delta = static_cast<std::ptrdiff_t>(after_line_count) -
                                    static_cast<std::ptrdiff_t>(before_line_count);
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
    const editor::TextViewport& viewport, std::string_view uri) {
  BufferSyncTarget target;
  target.client = LspClientForViewport(viewport, &target.language_id);
  if (target.client == nullptr) {
    return std::nullopt;
  }
  target.uri = uri.empty() ? FileUriForPath(viewport.path()) : std::string(uri);
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
  editor::DiagnosticsStore& store = CurrentProjectState().diagnostics_store;
  if (store.FindByPathKey(viewport.path_key()) == nullptr) {
    return;
  }
  store.TransformOwnerFile("lsp", viewport.path(), std::forward<Transform>(transform));
}

void LspService::SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                                const std::vector<std::string>& after_lines) {
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr) {
    return;
  }
  BufferChangeDelta delta;
  delta.before_line_count = before_lines.size();
  const std::size_t common = std::min(before_lines.size(), after_lines.size());
  while (delta.first_changed_line < common &&
         before_lines[delta.first_changed_line] == after_lines[delta.first_changed_line]) {
    ++delta.first_changed_line;
  }
  SyncLspForBufferChange(*viewport, delta);
}

void LspService::SyncLspForBufferChange(const editor::TextViewport& viewport,
                                        BufferChangeDelta delta) {
  if (viewport.path().empty()) {
    return;
  }

  ClearLspSemanticTokensForFile(viewport);
  ClearLspEditOverlaysForFile(viewport);

  ShiftLspDiagnosticsForBulkChange(viewport, delta.before_line_count, delta.first_changed_line);

  const std::optional<BufferSyncTarget> target = ResolveOpenDocumentForSync(viewport);
  if (!target.has_value()) {
    return;
  }

  if (target->was_open) {
    target->client->DidChange(target->uri, viewport.SerializeText());
  }
  synced_content_revision_[target->uri] = viewport.content_revision();
  if (!viewport.dirty()) {
    RequestLspSemanticTokens(viewport, *target->client);
    RequestLspInlayHints(viewport, *target->client);
    RequestLspCodeLenses(viewport, *target->client);
  }
}

void LspService::ForgetDocument(const std::string& uri) {
  synced_content_revision_.erase(uri);
}

void LspService::SyncLspForActiveEditableLastChange() {
  util::PerformanceTrace::Scope perf_scope("LspService::SyncLspForActiveEditableLastChange");
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }
  if (sync_uri_.empty() || viewport->path() != sync_uri_path_) {
    sync_uri_path_ = viewport->path();
    sync_uri_ = FileUriForPath(sync_uri_path_);
  }
  std::optional<std::uint64_t> server_revision;
  if (const auto it = synced_content_revision_.find(sync_uri_); it == synced_content_revision_.end()) {
    synced_content_revision_.emplace(sync_uri_, viewport->content_revision());
  } else if (it->second == viewport->content_revision()) {
    return;
  } else {
    server_revision = it->second;
    it->second = viewport->content_revision();
  }

  ClearLspSemanticTokensForFile(*viewport);
  ClearLspEditOverlaysForFile(*viewport);

  ShiftLspDiagnosticsForAppliedEdit(*viewport, server_revision);

  std::optional<BufferSyncTarget> target;
  {
    util::PerformanceTrace::Scope scope(
        "LspService::SyncLspForActiveEditableLastChange::ResolveAndOpen");
    target = ResolveOpenDocumentForSync(*viewport, sync_uri_);
  }
  if (!target.has_value()) {
    return;
  }
  LspClient* const client = target->client;
  const std::string& uri = target->uri;
  const bool was_open = target->was_open;

  const editor::AppliedEdit* applied_edit =
      server_revision.has_value() ? viewport->AppliedEditFromRevision(*server_revision) : nullptr;

  if (was_open) {
    const bool utf8_positions =
        LspEncodingForClient(*client) == lsp_encoding::PositionEncoding::Utf8;
    if (applied_edit == nullptr || !client->SupportsIncrementalSync() || !utf8_positions) {
      util::PerformanceTrace::Scope scope(
          "LspService::SyncLspForActiveEditableLastChange::FullSync");
      client->DidChange(uri, viewport->SerializeText());
    } else {
      util::PerformanceTrace::Scope scope(
          "LspService::SyncLspForActiveEditableLastChange::IncrementalSync");
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
                  SaturateToLspInt(applied_edit->range_before.start.line),
                  SaturateToLspInt(applied_edit->range_before.start.column),
              },
              .end = LspClient::Position{
                  SaturateToLspInt(applied_edit->range_before.end.line),
                  SaturateToLspInt(applied_edit->range_before.end.column),
              },
          },
          replacement);
    }
  }

  if (!viewport->dirty()) {
    RequestLspSemanticTokens(*viewport, *client);
    RequestLspInlayHints(*viewport, *client);
    RequestLspCodeLenses(*viewport, *client);
  }
}

void LspService::ConsumeLspCallbacks() {
  CurrentLspManager().DrainCallbacks();
  DrainRetiringClients();
}

LspService::~LspService() {
  for (auto& client : retiring_clients_) {
    if (client != nullptr) {
      client->Shutdown();
    }
  }
  retiring_clients_.clear();
}

void LspService::RetireClients(std::vector<std::unique_ptr<LspClient>> clients) {
  for (auto& client : clients) {
    if (client != nullptr) {
      retiring_clients_.push_back(std::move(client));
    }
  }
  DrainRetiringClients();
}

void LspService::RetireCurrentProjectServers() {
  ProjectWorkspaceState& state = CurrentProjectState();
  if (state.lsp_manager != nullptr) {
    RetireClients(state.lsp_manager->BeginShutdownAllAndTakeClients());
  }
}

void LspService::DrainRetiringClients() {
  auto write_it = retiring_clients_.begin();
  for (auto read_it = retiring_clients_.begin(); read_it != retiring_clients_.end(); ++read_it) {
    if (*read_it == nullptr) {
      continue;
    }
    (*read_it)->DrainCallbacks();
    if ((*read_it)->IsShutdownComplete()) {
      (*read_it)->Shutdown();
      continue;
    }
    if (write_it != read_it) {
      *write_it = std::move(*read_it);
    }
    ++write_it;
  }
  retiring_clients_.erase(write_it, retiring_clients_.end());
}

}
