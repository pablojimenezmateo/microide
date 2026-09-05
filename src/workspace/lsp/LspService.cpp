#include "workspace/lsp/LspService.h"

#include "workspace/lsp/LspWorkspaceEditOps.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
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

std::string SerializeViewportText(const editor::TextViewport& viewport) {
  // TD-2026-07-17-095: stream directly from the TextBuffer via a zero-copy LineSpan
  // rather than materializing a whole-document vector of strings with Snapshot().
  return util::SerializeLinesStreaming(editor::LineSpan(viewport.lines()),
                                       viewport.line_ending());
}

// Resolve the open editor viewport backing `path`, or nullptr if the file is not
// open in an (already-hydrated) editor tab. Used to obtain line text for inbound
// position-encoding conversion of diagnostics / semantic tokens.
const editor::TextViewport* FindOpenEditorViewport(const ProjectWorkspaceState& state,
                                                   const std::filesystem::path& path) {
  // Normalize the QUERY once, then compare each tab's path against it with the
  // scan-shaped helper: a mismatch between two already-normal paths costs a string
  // compare, where re-normalizing per tab cost ~12 allocations per tab per inbound
  // diagnostics/semantic-tokens message (TD-2026-08-10-174).
  std::filesystem::path normalized_storage;
  const std::filesystem::path* normalized_ptr = &path;
  if (util::PathTextNeedsNormalizing(path.native())) {
    normalized_storage = path.lexically_normal();
    normalized_ptr = &normalized_storage;
  }
  const std::filesystem::path& normalized = *normalized_ptr;
  for (const auto& group : state.editor_groups) {
    for (const auto& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      const auto& editor_state = *tab.editor_state;
      if (!editor_state.needs_restore &&
          util::SameAsNormalizedPath(editor_state.viewport.path(), normalized)) {
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
//
// The type-like set is checked against what clangd 18 actually advertises and
// emits, not just the LSP standard list: its legend is {bracket, class, comment,
// concept, enum, enumMember, function, interface, label, macro, method, modifier,
// namespace, operator, parameter, property, type, typeParameter, unknown,
// variable}. `concept` is C++20-specific, absent from the LSP standard set, and
// really is a type-like entity — clangd emits it for the constraint name in
// `template <Addable T>` — so it belongs here rather than falling through to
// Plain and being left to the lexical highlighter, which cannot know the name is
// a type.
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
  const bool inlay = master && SettingFlagEnabled(get("editor.inlay_hints.enabled"), true);
  const bool code_lens = master && SettingFlagEnabled(get("lsp.code_lens.enabled"), true);
  const bool document_highlight =
      master && SettingFlagEnabled(get("lsp.document_highlight.enabled"), true);

  // Treat the first reconcile as a transition from "all on", so any feature that
  // starts disabled is cleared once, and only actual flips do work thereafter.
  const bool first = !last_feature_enablement_.has_value();
  const FeatureEnablement prev = last_feature_enablement_.value_or(
      FeatureEnablement{true, true, true, true, true, true});
  last_feature_enablement_ =
      FeatureEnablement{master, diagnostics, semantic, inlay, code_lens, document_highlight};

  ProjectWorkspaceState& state = CurrentProjectState();

  if (!master) {
    // Master flipped off: stop the project's servers and drop every LSP-owned
    // decoration so nothing stale lingers. Lazy start resumes through
    // LspClientForViewport once the master is turned back on. Act only on entry.
    if (prev.master) {
      if (state.lsp_manager != nullptr) {
        // Retire into the host pool instead of a blocking ShutdownAll — flipping the
        // master switch off must not stall the shell thread (TD-2026-07-17-091).
        RetireClients(state.lsp_manager->BeginShutdownAllAndTakeClients());
      }
      bool changed = state.diagnostics_store.ClearOwner("lsp");
      // EVERY LSP-owned overlay, not just semantic tokens: inlay hints, code
      // lenses and semantic occurrence highlights are equally server-owned, and
      // leaving any of them painted after the subsystem is switched off shows the
      // user output from a server that is no longer running.
      changed = ClearAllLspOverlays(state) || changed;
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

  // Inlay hints and code lenses mirror semantic tokens: their own toggle clears
  // the overlay on disable and re-pulls it on enable. Both are pull-based, so
  // without the re-pull they would stay blank until an unrelated didOpen/save.
  if (!inlay && prev.inlay) {
    if (state.plugin_presentation != nullptr &&
        state.plugin_presentation->decorations.ClearOwner("lsp:inlay")) {
      state.MaybeReleasePluginPresentation();
      operations_.request_editor_surface_redraw();
    }
    inlay_hint_generation_.clear();
  } else if (inlay && !prev.inlay && !master_turned_on && has_active) {
    std::string language_id;
    if (LspClient* client = LspClientForViewport(*viewport, &language_id); client != nullptr) {
      EnsureLspDocumentOpen(*viewport, *client, language_id);
      RequestLspInlayHints(*viewport, *client);
    }
  }

  if (!code_lens && prev.code_lens) {
    if (state.plugin_presentation != nullptr &&
        state.plugin_presentation->decorations.ClearOwner("lsp:codelens")) {
      state.MaybeReleasePluginPresentation();
      operations_.request_editor_surface_redraw();
    }
    code_lens_generation_.clear();
    code_lens_commands_.clear();
  } else if (code_lens && !prev.code_lens && !master_turned_on && has_active) {
    std::string language_id;
    if (LspClient* client = LspClientForViewport(*viewport, &language_id); client != nullptr) {
      EnsureLspDocumentOpen(*viewport, *client, language_id);
      RequestLspCodeLenses(*viewport, *client);
    }
  }

  // Semantic occurrence highlights need no re-request on enable: the next presented
  // frame re-asks for the caret's symbol on its own. Turning them off just drops the
  // stored set so the textual word scan takes back over immediately.
  if (!document_highlight && prev.document_highlight && !state.semantic_occurrences.empty()) {
    state.semantic_occurrences.Clear();
    operations_.request_editor_surface_redraw();
  }
}

bool LspService::ClearAllLspOverlays(ProjectWorkspaceState& state) {
  // Invalidate the generations first, unconditionally: a response already in flight
  // captured the previous value, and dropping it there is the only thing that stops
  // it repainting an overlay this call just cleared.
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
    operations_.request_editor_surface_redraw();
  }
  return changed;
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

bool LspService::HasActiveDefinitionProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() &&
         ((operations_.plugin_has_definition_provider &&
           operations_.plugin_has_definition_provider(language_id)) ||
          CurrentLspManager().HasServer(language_id));
}

bool LspService::HasActiveReferencesProvider() const {
  const std::string language_id = ActiveLanguageIdForProvider();
  return !language_id.empty() &&
         ((operations_.plugin_has_references_provider &&
           operations_.plugin_has_references_provider(language_id)) ||
          CurrentLspManager().HasServer(language_id));
}

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
  // The status bar shows only the readiness word; whatever extra the server said (a
  // progress title, a startup error) goes to the tooltip. It used to go into the bar
  // itself, where a multi-line clangd startup error was truncated to noise.
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

std::size_t LspService::NotifyWatchedFileChanges(
    const std::vector<project::ProjectFileChange>& changes) {
  if (changes.empty() || context_ == nullptr) {
    return 0;
  }
  // Ordered cheapest-first: a project with the master switch off, or with no
  // language server registered at all, must not pay for path or URI work. Note
  // this deliberately does NOT call EnsureProjectLspManager — allocating a manager
  // for a project that has none would be pure cost on every change batch.
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
    // The normalizer already produced a project-relative path; use it rather than
    // recomputing a relative() per file (which stats the filesystem).
    event.relative_path = change.relative_path.generic_string();
    event.absolute_path = change.absolute_path.generic_string();
    event.uri = FileUriForPath(change.absolute_path);
    events.push_back(std::move(event));
  }
  return state.lsp_manager->NotifyWatchedFileChanges(events);
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
  // The viewport owns the memo, so this is a hash-free field compare on a
  // settled buffer -- no per-frame re-detection, and (unlike the path-keyed
  // cache this replaced) it re-detects when the buffer's content actually
  // changes, so an edited shebang no longer pins the stale language.
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
  // the main thread (buffer/disk mutation) when the server pushes an edit. The
  // captured raw `client` is the client invoking its own handler (via its
  // main-thread mailbox, which drops tasks unrun on destruction), so it cannot
  // dangle; it is needed for the version gate against tracked document versions.
  if (!client->HasApplyEditHandler()) {
    client->SetApplyEditHandler([this, client](LspClient::WorkspaceEdit edit) {
      return ApplyServerWorkspaceEdit(*client, std::move(edit));
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
  // Pull inlay hints for the freshly-opened document (mid-line type/param hints).
  RequestLspInlayHints(viewport, client);
  // Pull code lenses for the freshly-opened document (line-level actions).
  RequestLspCodeLenses(viewport, client);
}

void LspService::ScheduleBufferOpen(const std::filesystem::path& path) {
  std::filesystem::path normalized = path.lexically_normal();
  if (normalized.empty()) {
    return;
  }
  // Latest activation wins: a rapid A->B->C switch only hydrates C.
  pending_buffer_open_ = std::move(normalized);
}

bool LspService::ConsumeDeferredBufferOpen() {
  if (!pending_buffer_open_.has_value()) {
    return false;
  }
  const std::filesystem::path path = std::move(*pending_buffer_open_);
  pending_buffer_open_.reset();
  // Resolve against the CURRENT active buffer: if the user switched away again
  // before this drained, the scheduled hydration is obsolete and dropped (the new
  // tab scheduled its own). Mirrors WorkspaceShell::NotifyLspBufferOpen exactly,
  // just deferred past the tab-switch frame.
  editor::TextViewport* viewport =
      operations_.active_editable_viewport ? operations_.active_editable_viewport() : nullptr;
  // `path` came out of ScheduleBufferOpen already normalized.
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
               LspResult<std::vector<LspClient::SemanticToken>> tokens) mutable {
        // A transport failure (timeout / server-gone) leaves any prior tokens in
        // place; only an authoritative response (possibly empty) publishes.
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
       !SettingFlagEnabled(operations_.get_setting_value("editor.inlay_hints.enabled"),
                           /*default_value=*/true))) {
    return;
  }
  ProjectWorkspaceState* const project = &CurrentProjectState();
  std::string uri = FileUriForPath(viewport.path());
  // Whole-document range: start of file to one past the last line. Servers clamp
  // the end; line boundaries need no per-column position-encoding conversion.
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

namespace {
// Resolve fan-out bound. Every unresolved lens costs one codeLens/resolve round
// trip, so a pathological document must not turn one pull into thousands of
// in-flight requests; unresolved lenses past the bound are dropped rather than
// published blank.
constexpr std::size_t kMaxResolvedCodeLenses = 256;

// Shared state for the resolve fan-out: the lens array being filled in, plus how
// many resolves are still outstanding. Held by shared_ptr so each response can
// decrement and the last one publishes.
struct CodeLensResolveBatch {
  std::vector<LspClient::CodeLens> lenses;
  std::size_t pending = 0;
};
}  // namespace

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
          return;  // transport failure leaves the previous lenses alone
        }
        // Nothing to fill in: publish straight through.
        const bool any_unresolved =
            can_resolve && std::any_of(lenses->begin(), lenses->end(),
                                       [](const LspClient::CodeLens& lens) {
                                         return lens.needs_resolve();
                                       });
        if (!any_unresolved) {
          // Without a resolve provider a title-less lens can never be painted.
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
            batch->lenses[i].unresolved = util::JsonValue{};  // give up; dropped below
            continue;
          }
          to_resolve.push_back(i);
        }
        batch->pending = to_resolve.size();
        // Bound the batch BEFORE issuing anything, so `pending` can never be
        // decremented past zero by a response arriving mid-loop.
        for (const std::size_t index : to_resolve) {
          client_ptr->ResolveCodeLensAsync(
              batch->lenses[index].unresolved,
              [this, project, batch, index, uri, language_id, generation, encoding](
                  LspResult<LspClient::CodeLens> resolved) mutable {
                if (resolved.has_value() && !resolved->title.empty()) {
                  // Keep the original range: resolve is only required to fill in
                  // `command`, and some servers echo a default-constructed range.
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
      [this, &uri, &language_id](const editor::TextViewport* /*file_viewport*/,
                                 lsp_encoding::PositionEncoding /*encoding*/,
                                 std::vector<LspClient::CodeLens>& lenses) {
        // Retire the previous publish's payloads for this URI. This runs inside the
        // build callback, i.e. only once the generation guard has ALREADY accepted
        // the response: retiring earlier would strip the handles off lenses that a
        // superseded response then fails to replace, leaving the ones on screen
        // inert. Rebuilding here is also what keeps the table proportional to what
        // is currently published rather than growing for the session.
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
    return;  // a handle from a superseded publish; the clicked lens is gone
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

void LspService::ClearLspCodeLensesForFile(const editor::TextViewport& viewport) {
  if (viewport.path().empty()) {
    return;
  }
  // Bump the generation first for the same reason inlay hints do: lenses paint in
  // every buffer state, so an in-flight response captured before this edit would
  // otherwise re-add lenses at pre-edit line numbers.
  std::string uri = FileUriForPath(viewport.path());
  NextOverlayGeneration(code_lens_generation_, uri);
  std::erase_if(code_lens_commands_,
                [&uri](const auto& entry) { return entry.second.uri == uri; });
  ProjectWorkspaceState& state = CurrentProjectState();
  auto* presentation = state.plugin_presentation.get();
  if (presentation == nullptr) {
    return;
  }
  if (presentation->decorations.ClearOwnerFile("lsp:codelens", viewport.path())) {
    state.MaybeReleasePluginPresentation();
    operations_.request_editor_surface_redraw();
  }
}

void LspService::MaybeRequestDocumentHighlights() {
  if (!operations_.active_editable_viewport) {
    return;
  }
  editor::TextViewport* const viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty() || viewport->is_placeholder()) {
    return;
  }
  // Same two gates the render path applies to occurrence highlighting: the feature
  // must be on, and a caret being driven by active typing does not highlight (the
  // seed would be a growing prefix). Checking them here keeps a keystroke from
  // costing a server round-trip whose answer would be discarded anyway.
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
      document_highlight_request_line_ == caret_line &&
      document_highlight_request_column_ == caret_column &&
      document_highlight_request_path_ == viewport->path()) {
    return;  // steady state: nothing about the request's identity changed
  }
  // Moving WITHIN the current highlight set asks the same question again — the
  // symbol and the buffer are both unchanged — so the answer is already on screen.
  // Arrowing through a word therefore costs nothing instead of one round-trip per
  // character. (The render path applies the same rule to decide the set is still
  // valid, so the two can never disagree about what is painted.)
  if (CurrentProjectState().semantic_occurrences.CoversCaret(viewport->path(), revision,
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
  document_highlight_request_line_ = caret_line;
  document_highlight_request_column_ = caret_column;

  ProjectWorkspaceState* const project = &CurrentProjectState();
  const std::uint64_t generation = ++document_highlight_generation_;
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  client->RequestDocumentHighlightAsync(
      FileUriForPath(viewport->path()),
      ByteColumnToLspPosition(*viewport, caret_line, caret_column, encoding),
      [this, project, path = viewport->path(), generation, revision, encoding, caret_line,
       caret_column](LspResult<std::vector<LspClient::DocumentHighlight>> highlights) {
        // A transport failure leaves the previous set (and the textual fallback)
        // alone; only an authoritative answer replaces what is painted.
        if (!highlights.has_value()) {
          return;
        }
        PublishLspDocumentHighlights(*project, path, generation, revision, encoding, caret_line,
                                     caret_column, std::move(*highlights));
      });
}

void LspService::PublishLspDocumentHighlights(
    ProjectWorkspaceState& state, const std::filesystem::path& path,
    std::uint64_t request_generation, std::uint64_t content_revision,
    lsp_encoding::PositionEncoding encoding, std::size_t caret_line, std::size_t caret_column,
    std::vector<LspClient::DocumentHighlight> highlights) {
  if (request_generation != document_highlight_generation_) {
    return;  // a newer caret position already went out
  }
  const editor::TextViewport* const file_viewport = FindOpenEditorViewport(state, path);
  if (file_viewport == nullptr || file_viewport->content_revision() != content_revision) {
    return;  // the buffer moved on; these absolute positions no longer describe it
  }

  auto& store = state.semantic_occurrences;
  store.path = path;
  store.content_revision = content_revision;
  store.ranges.clear();
  store.ranges.reserve(highlights.size());
  for (const LspClient::DocumentHighlight& highlight : highlights) {
    // Single-line ranges only. A symbol highlight spanning lines is not something
    // the row-sliced fill path can paint, and no server produces one for an
    // identifier; dropping it is honest, clamping it would paint a wrong extent.
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
  // The renderer resolves a row's fills with a binary search over this vector, and
  // the protocol does not promise any order.
  std::sort(store.ranges.begin(), store.ranges.end(),
            [](const editor::OccurrenceRange& lhs, const editor::OccurrenceRange& rhs) {
              return lhs.line_index != rhs.line_index ? lhs.line_index < rhs.line_index
                                                      : lhs.start_column < rhs.start_column;
            });
  if (store.ranges.empty()) {
    // An authoritative "no symbol here" — drop the identity too so a later caret
    // move back onto a real symbol is not mistaken for a still-valid empty set.
    store.Clear();
  }
  operations_.request_editor_surface_redraw();
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
                                                  std::size_t before_line_count,
                                                  std::size_t first_changed_line) {
  const std::size_t after_line_count = viewport.line_count();
  if (viewport.path().empty() || before_line_count == after_line_count) {
    return;  // No net line change: in-place edits keep diagnostics roughly aligned.
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
  // The keystroke/action redraw path still hands us full before/after vectors; the
  // viewport already holds the after-content, so derive the compact delta (first
  // differing line) here and reuse the streaming sync below.
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

  // A bulk edit shifts the buffer geometry, so the absolute-positioned semantic
  // overlay is now stale -> drop it (the lexical layer keeps painting). Done
  // before the client early-out so a stale overlay is cleared even with no server.
  ClearLspSemanticTokensForFile(viewport);
  ClearLspInlayHintsForFile(viewport);
  ClearLspCodeLensesForFile(viewport);

  // Keep diagnostics positioned for the dirty buffer until the server republishes.
  // Runs before the client early-out so a dead/absent server never strands them.
  ShiftLspDiagnosticsForBulkChange(viewport, delta.before_line_count, delta.first_changed_line);

  const std::optional<BufferSyncTarget> target = ResolveOpenDocumentForSync(viewport);
  if (!target.has_value()) {
    return;
  }

  // Full-document sync for the bulk-change path. The per-keystroke path
  // (SyncLspForActiveEditableLastChange) sends true ranged incremental edits via
  // the viewport's last applied edit; here we only have a before/after delta, so a
  // clean full replace is the correct, desync-proof choice. The payload streams
  // straight from the live buffer (no whole-document vector materialized), and full
  // text needs no per-column position-encoding conversion, so this stays correct
  // for utf-16 servers too. Only when the doc was already open (see
  // ResolveOpenDocumentForSync).
  if (target->was_open) {
    target->client->DidChange(target->uri, SerializeViewportText(viewport));
  }
  // Re-request semantic tokens only when the edit left the buffer clean (e.g. an
  // undo landing on the saved point). The overlay is render-suppressed while
  // dirty, so requesting for a dirty buffer would paint nothing yet be superseded
  // by the next clean transition anyway.
  if (!viewport.dirty()) {
    RequestLspSemanticTokens(viewport, *target->client);
    RequestLspInlayHints(viewport, *target->client);
    RequestLspCodeLenses(viewport, *target->client);
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
  ClearLspCodeLensesForFile(*viewport);

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
      client->DidChange(uri, util::SerializeLinesStreaming(editor::LineSpan(viewport->lines()),
                                                           viewport->line_ending()));
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

  // Repopulate the semantic overlay when the edit leaves the buffer clean (an
  // undo/redo landing on the saved point). Typing leaves the buffer dirty and so
  // stays request-free; the overlay is render-suppressed while dirty regardless.
  if (!viewport->dirty()) {
    RequestLspSemanticTokens(*viewport, *client);
    RequestLspInlayHints(*viewport, *client);
    RequestLspCodeLenses(*viewport, *client);
  }
}

std::vector<LspClosedFileEditBucket> LspService::PrepareClosedFileBuckets(
    const std::vector<CodeActionEdit>& edits,
    const std::function<bool(const std::filesystem::path&)>& is_open) {
  std::vector<LspClosedFileEditBucket> buckets;
  // Map normalized path -> slot so grouping is O(1) per edit (O(edits * files)
  // otherwise for a large multi-file rename).
  std::unordered_map<std::string, std::size_t> bucket_index;
  const auto bucket_for = [&](const std::filesystem::path& normalized) -> LspClosedFileEditBucket& {
    const auto it = bucket_index.find(normalized.generic_string());
    if (it != bucket_index.end()) {
      return buckets[it->second];
    }
    bucket_index.emplace(normalized.generic_string(), buckets.size());
    buckets.push_back(LspClosedFileEditBucket{.path = normalized, .edits = {}, .encoding = {}});
    return buckets.back();
  };
  for (const CodeActionEdit& edit : edits) {
    if (edit.path.empty()) {
      continue;  // empty path targets the (always-open) active buffer
    }
    const std::filesystem::path normalized = edit.path.lexically_normal();
    if (is_open && is_open(normalized)) {
      continue;  // edited in place by the open-buffer applier; keep undo coherent
    }
    bucket_for(normalized).edits.emplace_back(edit.range, edit.new_text);
  }
  // Resolve each file's server position encoding HERE, on the main thread: the
  // background applier must not touch the shared runtime syntax registry
  // (lock-free-main-reader invariant) or the LSP manager. Path-only filetype
  // detection is a slight behaviour change from the old content-based detection for
  // extensionless files, which now default to UTF-8 unless a server matches the
  // path — negligible for real source files. FindStartedServer never spawns a server.
  for (LspClosedFileEditBucket& bucket : buckets) {
    const std::string language_id = editor::runtime_syntax::DetectFiletype(bucket.path);
    LspClient* client = CurrentLspManager().FindStartedServer(language_id);
    bucket.encoding = client != nullptr ? LspEncodingForClient(*client)
                                        : lsp_encoding::PositionEncoding::Utf8;
  }
  return buckets;
}

LspService::DiskEditResult LspService::RunClosedFileEdits(
    const std::vector<LspClosedFileEditBucket>& buckets) {
  DiskEditResult result;
  for (const LspClosedFileEditBucket& bucket : buckets) {
    if (bucket.edits.empty()) {
      continue;
    }
    // Load into a scratch viewport so line-ending / BOM / encoding detection and the
    // atomic, permission-preserving save path are reused verbatim. Private to this
    // call, so safe on a background thread; never registered as a tab.
    editor::TextViewport scratch;
    if (!scratch.OpenFile(bucket.path)) {
      result.any_failed = true;
      continue;
    }
    const lsp_encoding::PositionEncoding encoding = bucket.encoding;

    // Map 0-based LSP coordinates to editor byte columns. A line beyond EOF is a
    // hard reject (a buggy/hostile server must not silently mutate the last line
    // instead of the line it named), except the LSP end-of-document sentinel
    // `{line == line_count, character == 0}`, which maps to the end of the last
    // line. A `character` past the line end stays a soft clamp. `*ok` is set false
    // on reject.
    const auto map_position = [&](editor::TextPosition pos, bool* ok) -> editor::TextPosition {
      *ok = true;
      const std::size_t line_count = scratch.line_count();
      if (line_count == 0) {
        *ok = false;
        return editor::TextPosition{0, 0};
      }
      if (pos.line == line_count && pos.column == 0) {
        const std::size_t last = line_count - 1;
        return editor::TextPosition{last, scratch.lines().LineLength(last)};
      }
      if (pos.line >= line_count) {
        *ok = false;
        return pos;
      }
      pos.column = lsp_encoding::LspCharacterToByteColumn(
          scratch.lines().LineView(pos.line), pos.column, encoding);
      return pos;
    };

    // Copy the bucket's edits so their ranges can be mapped in place (bucket is const).
    std::vector<std::pair<editor::SelectionRange, std::string>> file_edits = bucket.edits;
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
    // Highest position first (see editor/EditBatchOrder.h for the tie rules) so
    // earlier ranges stay valid as later ones apply. No undo is recorded: scratch
    // is discarded. Overlapping edits are rejected whole: applying two
    // intersecting ranges double-edits shared bytes in an order-dependent way.
    std::vector<editor::SelectionRange> file_ranges;
    file_ranges.reserve(file_edits.size());
    for (const auto& [range, text] : file_edits) {
      file_ranges.push_back(range);
    }
    std::vector<std::size_t> apply_order;
    editor::OrderEditsForApplication(file_ranges, apply_order);
    if (editor::EditsOverlap(file_ranges, apply_order)) {
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

LspService::DiskEditResult LspService::ApplyLspEditsToClosedFilesOnDisk(
    const std::vector<CodeActionEdit>& edits,
    const std::function<bool(const std::filesystem::path&)>& is_open) {
  // Synchronous wrapper kept for the user-initiated rename path (which reports a
  // file count in its feedback). The server-initiated path dispatches
  // RunClosedFileEdits off-thread instead (see ApplyServerWorkspaceEdit).
  return RunClosedFileEdits(PrepareClosedFileBuckets(edits, is_open));
}

bool LspService::IsPathOpenInProject(const std::filesystem::path& normalized) const {
  // Same scan as FindOpenEditorViewport (which re-normalizes idempotently).
  return FindOpenEditorViewport(CurrentProjectState(), normalized) != nullptr;
}

bool LspService::ApplyFullWorkspaceEdit(const std::vector<CodeActionEdit>& edits,
                                        const std::vector<WorkspaceResourceOp>& resource_ops) {
  bool any_applied = false;
  if (!resource_ops.empty()) {
    const ResourceOpsResult ops_result = ApplyWorkspaceResourceOps(resource_ops);
    if (!ops_result.ok) {
      return false;
    }
    any_applied = ops_result.any_applied;
  }
  if (edits.empty()) {
    return any_applied;
  }
  OpenBufferEditResult open_result;
  if (operations_.apply_workspace_edit_to_open_buffers) {
    open_result = operations_.apply_workspace_edit_to_open_buffers(edits);
  }
  // Synchronous closed-file apply (code-action scale: a handful of files, often
  // just the one the ops created); the server-initiated bulk path stays async.
  const DiskEditResult disk = ApplyLspEditsToClosedFilesOnDisk(
      edits, [this](const std::filesystem::path& normalized) {
        return IsPathOpenInProject(normalized);
      });
  return any_applied || open_result.applied_any || disk.files_written > 0;
}

bool LspService::WorkspaceEditVersionsCurrent(const LspClient& client,
                                              const LspClient::WorkspaceEdit& edit) {
  return lsp_workspace_edit::VersionsCurrent(client, edit);
}

LspService::ResourceOpsResult LspService::ApplyWorkspaceResourceOps(
    const std::vector<WorkspaceResourceOp>& ops) {
  ResourceOpsResult result;
  if (ops.empty()) {
    return result;
  }
  namespace fs = std::filesystem;
  using Kind = WorkspaceResourceOp::Kind;
  const fs::path project_root = CurrentProjectState().root;

  // --- Validate every op against a simulated existence overlay BEFORE mutating
  // anything: the overlay tracks per-path existence as the earlier ops would
  // leave it, so a create-then-rename batch validates the rename against the
  // yet-to-be-created file. Catching every logical failure here is what makes
  // the batch effectively atomic — the apply phase below can only fail on raw
  // I/O errors, which the rollback journal covers.
  std::unordered_map<std::string, bool> overlay;
  const auto target_exists = [&](const fs::path& p) {
    const auto it = overlay.find(p.generic_string());
    if (it != overlay.end()) {
      return it->second;
    }
    std::error_code ec;
    return fs::exists(p, ec);
  };
  const auto set_exists = [&](const fs::path& p, bool exists) {
    overlay[p.generic_string()] = exists;
  };
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.any_applied = false;
    result.error = std::move(message);
    return result;
  };
  std::vector<char> skip(ops.size(), 0);  // ignore-option ops that resolve to no-ops
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const WorkspaceResourceOp& op = ops[i];
    const fs::path target = op.path.lexically_normal();
    // A server may only touch files inside the active project root (mirrors the
    // text-edit containment check; a compromised server must not create/rename/
    // delete arbitrary user files). One bad op fails the whole batch — skipping
    // it would desync every subsequent edit keyed to the op's outcome.
    if (target.empty() || !util::PathEqualsOrWithin(target, project_root)) {
      return fail("resource op target escapes the project root");
    }
    switch (op.kind) {
      case Kind::Create:
        if (target_exists(target)) {
          if (!op.overwrite) {  // LSP: overwrite wins over ignoreIfExists
            if (op.ignore_if_exists) {
              skip[i] = 1;
              break;
            }
            return fail("create target already exists: " + target.generic_string());
          }
        }
        set_exists(target, true);
        break;
      case Kind::Rename: {
        const fs::path dest = op.new_path.lexically_normal();
        if (dest.empty() || !util::PathEqualsOrWithin(dest, project_root)) {
          return fail("rename destination escapes the project root");
        }
        if (!target_exists(target)) {
          return fail("rename source does not exist: " + target.generic_string());
        }
        if (target_exists(dest)) {
          if (!op.overwrite) {
            if (op.ignore_if_exists) {
              skip[i] = 1;
              break;
            }
            return fail("rename destination already exists: " + dest.generic_string());
          }
        }
        set_exists(target, false);
        set_exists(dest, true);
        break;
      }
      case Kind::Delete: {
        if (!target_exists(target)) {
          if (op.ignore_if_not_exists) {
            skip[i] = 1;
            break;
          }
          return fail("delete target does not exist: " + target.generic_string());
        }
        std::error_code ec;
        if (!op.recursive && fs::is_directory(target, ec) && !fs::is_empty(target, ec)) {
          return fail("delete target is a non-empty directory (recursive not set): " +
                      target.generic_string());
        }
        set_exists(target, false);
        break;
      }
    }
  }

  // --- Apply in order, journaling the inverse of each mutation. A delete (or an
  // overwrite's displaced target) is STAGED — renamed to a hidden sibling in the
  // same directory (same filesystem, pure rename) — so it stays restorable until
  // the whole batch lands; only then are the staged backups disposed.
  struct JournalEntry {
    enum class Undo : std::uint8_t {
      RemoveCreatedFile,  // a: created file
      RemoveCreatedDirs,  // a: topmost directory this batch created
      RenameBack,         // a: current (new) path, b: original path
      RestoreStaged,      // a: original path, b: staged path
    };
    Undo undo;
    fs::path a;
    fs::path b;
  };
  std::vector<JournalEntry> journal;
  std::vector<fs::path> staged_disposals;
  std::size_t stage_seq = 0;
  const auto stage_aside = [&](const fs::path& victim) -> std::optional<fs::path> {
    while (true) {
      const fs::path staged =
          victim.parent_path() / (".microide-lsp-staged-" + std::to_string(stage_seq++) + "-" +
                                  victim.filename().string());
      std::error_code ec;
      if (fs::exists(staged, ec)) {
        continue;  // seq collision with leftover debris; try the next name
      }
      fs::rename(victim, staged, ec);
      if (ec) {
        return std::nullopt;
      }
      return staged;
    }
  };
  const auto ensure_parent_dirs = [&](const fs::path& target) -> bool {
    const fs::path parent = target.parent_path();
    std::error_code ec;
    if (parent.empty() || fs::exists(parent, ec)) {
      return true;
    }
    // Journal the TOPMOST directory this call creates so rollback removes the
    // whole new chain, not just the leaf.
    fs::path topmost = parent;
    while (!topmost.parent_path().empty() && topmost.parent_path() != topmost &&
           !fs::exists(topmost.parent_path(), ec)) {
      topmost = topmost.parent_path();
    }
    fs::create_directories(parent, ec);
    if (ec) {
      return false;
    }
    journal.push_back({JournalEntry::Undo::RemoveCreatedDirs, topmost, {}});
    return true;
  };
  const auto rollback = [&]() {
    for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
      std::error_code ec;
      switch (it->undo) {
        case JournalEntry::Undo::RemoveCreatedFile:
          fs::remove(it->a, ec);
          break;
        case JournalEntry::Undo::RemoveCreatedDirs:
          fs::remove_all(it->a, ec);
          break;
        case JournalEntry::Undo::RenameBack:
          fs::rename(it->a, it->b, ec);
          break;
        case JournalEntry::Undo::RestoreStaged:
          fs::rename(it->b, it->a, ec);
          break;
      }
    }
  };

  // Reconcile actions are recorded in APPLY ORDER, not grouped by kind: a batch
  // like [delete B, rename A->B] must close B's tabs before A's tabs are
  // retargeted onto B, or the retargeted tab (and its unsaved contents) is closed
  // by the reconcile of an op that ran earlier.
  struct AppliedReconcile {
    bool is_rename = false;
    fs::path from;  // rename source / delete target
    fs::path to;    // rename destination; empty for delete
  };
  std::vector<AppliedReconcile> applied_reconciles;
  fs::path last_mutated;
  bool any_applied = false;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    if (skip[i] != 0) {
      continue;
    }
    const WorkspaceResourceOp& op = ops[i];
    const fs::path target = op.path.lexically_normal();
    std::string failure;
    switch (op.kind) {
      case Kind::Create: {
        std::error_code ec;
        if (!ensure_parent_dirs(target)) {
          failure = "could not create parent directory for: " + target.generic_string();
          break;
        }
        if (fs::exists(target, ec)) {
          // Validated as an overwrite: stage the displaced content aside first so
          // a later failure restores it byte-identically.
          const std::optional<fs::path> staged = stage_aside(target);
          if (!staged.has_value()) {
            failure = "could not stage existing file for overwrite: " + target.generic_string();
            break;
          }
          journal.push_back({JournalEntry::Undo::RestoreStaged, target, *staged});
          staged_disposals.push_back(*staged);
        }
        std::ofstream created(target, std::ios::binary | std::ios::trunc);
        if (!created) {
          failure = "could not create file: " + target.generic_string();
          break;
        }
        created.close();
        journal.push_back({JournalEntry::Undo::RemoveCreatedFile, target, {}});
        break;
      }
      case Kind::Rename: {
        const fs::path dest = op.new_path.lexically_normal();
        std::error_code ec;
        if (!ensure_parent_dirs(dest)) {
          failure = "could not create parent directory for: " + dest.generic_string();
          break;
        }
        if (fs::exists(dest, ec)) {
          const std::optional<fs::path> staged = stage_aside(dest);
          if (!staged.has_value()) {
            failure = "could not stage existing file for overwrite: " + dest.generic_string();
            break;
          }
          journal.push_back({JournalEntry::Undo::RestoreStaged, dest, *staged});
          staged_disposals.push_back(*staged);
        }
        fs::rename(target, dest, ec);
        if (ec) {
          failure = "could not rename " + target.generic_string() + " -> " +
                    dest.generic_string() + ": " + ec.message();
          break;
        }
        journal.push_back({JournalEntry::Undo::RenameBack, dest, target});
        applied_reconciles.push_back({/*is_rename=*/true, target, dest});
        last_mutated = dest;
        break;
      }
      case Kind::Delete: {
        const std::optional<fs::path> staged = stage_aside(target);
        if (!staged.has_value()) {
          failure = "could not delete: " + target.generic_string();
          break;
        }
        journal.push_back({JournalEntry::Undo::RestoreStaged, target, *staged});
        staged_disposals.push_back(*staged);
        applied_reconciles.push_back({/*is_rename=*/false, target, {}});
        last_mutated = target.parent_path();
        break;
      }
    }
    if (op.kind == Kind::Create && failure.empty()) {
      last_mutated = target;
    }
    if (!failure.empty()) {
      rollback();
      return fail(std::move(failure));
    }
    any_applied = true;
  }
  result.any_applied = any_applied;
  if (!any_applied) {
    return result;  // every op was an ignore-option no-op
  }

  // --- The batch landed: reconcile shell state per op, dispose the staged
  // backups (off the shell thread when possible — a staged directory removal can
  // be slow), and refresh the project views once.
  for (const AppliedReconcile& action : applied_reconciles) {
    if (action.is_rename) {
      if (operations_.reconcile_tabs_after_resource_rename) {
        operations_.reconcile_tabs_after_resource_rename(action.from, action.to);
      }
    } else if (operations_.reconcile_tabs_after_resource_delete) {
      operations_.reconcile_tabs_after_resource_delete(action.from);
    }
  }
  // A staged FILE is one unlink — do it here, before the view refresh below, so
  // the hidden staging entry can never be walked into the file index and shown in
  // the tree/finder. Only a staged DIRECTORY (arbitrarily deep remove_all) is
  // worth an off-thread hop, and directory ops are the rare case.
  std::vector<fs::path> slow_disposals;
  for (fs::path& staged : staged_disposals) {
    std::error_code ec;
    if (fs::is_directory(staged, ec)) {
      slow_disposals.push_back(std::move(staged));
      continue;
    }
    fs::remove(staged, ec);
  }
  if (!slow_disposals.empty()) {
    if (operations_.dispose_staged_paths_async) {
      operations_.dispose_staged_paths_async(std::move(slow_disposals));
    } else {
      for (const fs::path& staged : slow_disposals) {
        std::error_code ec;
        fs::remove_all(staged, ec);
      }
    }
  }
  if (operations_.refresh_views_after_resource_ops) {
    operations_.refresh_views_after_resource_ops(last_mutated);
  }
  return result;
}

bool LspService::ApplyServerWorkspaceEdit(LspClient& client, LspClient::WorkspaceEdit edit) {
  // Version gate FIRST (before anything mutates): a versioned TextDocumentEdit
  // whose expected version differs from the tracked open document is stale — the
  // server computed it against text the user has since changed. LSP requires the
  // client to fail the whole request on a mismatch.
  if (!WorkspaceEditVersionsCurrent(client, edit)) {
    return false;
  }
  // Resource ops (create/rename/delete) run before any text edit; the parser
  // re-keyed pre-rename text edits to their post-rename URI to match. One
  // undecodable target URI fails the whole edit — partially applying an edit
  // whose op list we cannot fully honor would leave the workspace inconsistent.
  bool any_resource_applied = false;
  if (!edit.resource_ops.empty()) {
    const std::optional<std::vector<WorkspaceResourceOp>> ops =
        lsp_workspace_edit::FlattenResourceOps(edit.resource_ops);
    if (!ops.has_value()) {
      return false;
    }
    const ResourceOpsResult ops_result = ApplyWorkspaceResourceOps(*ops);
    if (!ops_result.ok) {
      return false;
    }
    any_resource_applied = ops_result.any_applied;
  }
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
    // A resource-op-only edit (e.g. a bare file rename) is a legitimate success.
    return any_resource_applied;
  }
  OpenBufferEditResult open_result;
  // Open buffers edit in place (the shell resolves + re-syncs them); closed files
  // are written silently on disk. Passing the full set to both is safe: the
  // open-buffer applier skips paths that are not open, and the disk applier skips
  // paths that are open.
  if (operations_.apply_workspace_edit_to_open_buffers) {
    open_result = operations_.apply_workspace_edit_to_open_buffers(flat);
  }
  // A server rename can push edits to thousands of CLOSED files. Load/apply/save
  // them off the shell thread (TD-2026-07-17-011 / TD-2026-07-16-18): prepare the
  // per-file buckets + encodings on the main thread, then dispatch the heavy
  // load/edit/save to the background executor. The file-index watcher reflects the
  // on-disk changes; a write failure self-heals (atomic writes leave the file
  // intact and a later request re-syncs) — so unlike the synchronous open-buffer
  // path we cannot gate the applyEdit response on the disk result.
  std::vector<LspClosedFileEditBucket> buckets = PrepareClosedFileBuckets(
      flat, [this](const std::filesystem::path& normalized) {
        return IsPathOpenInProject(normalized);
      });
  const bool had_closed_edits = !buckets.empty();
  if (had_closed_edits) {
    if (operations_.run_closed_file_edits_async) {
      operations_.run_closed_file_edits_async(std::move(buckets));
    } else {
      RunClosedFileEdits(buckets);  // headless fallback: synchronous
    }
  }
  // Acceptance response: report `applied: false` only when an OPEN-buffer group the
  // user can see was rejected (beyond-EOF / overlap). The closed-file writes were
  // accepted for background application (VSCode-style — it does not block the
  // applyEdit response on disk completion).
  if (open_result.any_rejected) {
    return false;
  }
  return open_result.applied_any || had_closed_edits || any_resource_applied;
}

void LspService::ConsumeLspCallbacks() {
  CurrentLspManager().DrainCallbacks();
  DrainRetiringClients();
}

LspService::~LspService() {
  // App teardown: block (bounded per client, ~3s worst case) on any client still
  // finishing its shutdown handshake. This is the only place the block remains —
  // project switches route retiring clients here and drain them asynchronously.
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
  // Reap any client that already finished shutting down so the pool stays small.
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
    // Pump the client's mailbox so its shutdown handshake makes progress, then reap
    // it once complete (Shutdown() returns immediately for an already-complete client).
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

}  // namespace microide::workspace
