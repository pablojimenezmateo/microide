#include "workspace/AssistService.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "editor/SnippetEngine.h"
#include "util/JsonValue.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/FileUri.h"
#include "workspace/LanguageDetection.h"
#include "workspace/LspPositionEncoding.h"
#include "workspace/LspViewportPositions.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

bool AssistService::ResultIsStale(const editor::TextViewport* active_editable,
                                  const std::filesystem::path& request_path) {
  return active_editable == nullptr || active_editable->path() != request_path;
}

namespace {

std::string_view LineAtOrEmpty(const std::vector<std::string>& lines, std::size_t index) {
  return index < lines.size() ? std::string_view(lines[index]) : std::string_view{};
}

std::string LspUnavailableMessage(const LspManager& manager,
                                  std::string_view language_id,
                                  std::string_view fallback) {
  if (!language_id.empty()) {
    const std::string id(language_id);
    if (manager.HasServer(id)) {
      const std::string detail = manager.LastServerError(id);
      if (!detail.empty()) {
        return "LSP startup failed for " + id + ": " + detail;
      }
      return "LSP startup failed for " + id;
    }
  }
  if (!fallback.empty()) {
    return std::string(fallback);
  }
  return "No language server available";
}

std::string JsonValueToArgumentString(const util::JsonValue& value) {
  return value.IsString() ? value.AsString() : util::SerializeJson(value);
}

editor::SelectionRange CompletionReplacementRange(const editor::TextViewport& viewport) {
  const std::string_view line = LineAtOrEmpty(viewport.lines().Snapshot(), viewport.cursor_line());
  std::size_t start_column = std::min(viewport.cursor_column(), line.size());
  while (start_column > 0) {
    const char ch = line[start_column - 1];
    // Only walk back over the trailing identifier token. `.`/`/`/`-` are token
    // SEPARATORS, not word chars: including them made a member completion (obj.|)
    // or path completion (a/b|) replace the whole chain, dropping the qualifier.
    // The LSP path prefers the server's textEdit range anyway; this heuristic only
    // covers plugin completions and LSP items without a textEdit.
    const bool identifier_char =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '_';
    if (!identifier_char) {
      break;
    }
    --start_column;
  }
  return editor::SelectionRange{
      .start = editor::TextPosition{viewport.cursor_line(), start_column},
      .end = editor::TextPosition{viewport.cursor_line(), viewport.cursor_column()},
  };
}

}  // namespace

void AssistService::Configure(WorkspaceContext& context,
                              WorkspacePluginRuntime& plugin_runtime,
                              WorkspaceOutputChannels& output_channels,
                              WorkspaceLanguageContract& language_contract,
                              Operations operations) {
  context_ = &context;
  plugin_runtime_ = &plugin_runtime;
  output_channels_ = &output_channels;
  language_contract_ = &language_contract;
  operations_ = std::move(operations);
}

bool AssistService::EditorSnippetsSettingEnabled() const {
  return SettingFlagEnabled(operations_.get_setting_value("editor.snippets.enabled"),
                            /*default_value=*/true);
}

bool AssistService::ShowCompletionOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectViewportLanguageId(*viewport);
  const std::filesystem::path request_path = viewport->path();

  // Open the overlay in a loading state immediately, then query the plugin worker
  // and the language server CONCURRENTLY (never serially). Neither blocks the UI;
  // each fills the overlay on its own mailbox drain via PublishCompletionMerge,
  // which ranks LSP-first for served languages and de-dupes overlapping labels.
  auto& session = context_->current_project_state.overlay.workflow.completion;
  session.items.clear();
  session.selected_index = 0;
  session.replacement_range = CompletionReplacementRange(*viewport);
  session.source = "lsp";
  session.error = "Loading...";
  operations_.show_overlay(OverlayMode::Completion);

  auto merge = std::make_shared<CompletionMerge>();
  merge->language_id = language_id;

  // Language-server source. A present server is authoritative for its language.
  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    operations_.ensure_lsp_document_open(*viewport, *client, language_id);
    operations_.begin_tracked_lsp_request();
    const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
    const std::size_t request_line = viewport->cursor_line();
    client->RequestCompletionAsync(
        FileUriForPath(request_path),
        ByteColumnToLspPosition(*viewport, request_line, viewport->cursor_column(), encoding),
        [this, request_path, encoding, merge](
            std::optional<std::vector<LspClient::CompletionItem>> items) {
          operations_.finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          // finish_tracked above must run first so the in-flight counter is not
          // leaked when a stale result bails.
          if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
            return;
          }
          merge->lsp_items = TransformLspCompletions(items, encoding);
          PublishCompletionMerge(merge, request_path);
        });
  } else {
    merge->sources.lsp_pending = false;
  }

  // Plugin source, dispatched at the same time.
  plugin_runtime_->Host().QueryCompletionsAsync(
      language_id, request_path, viewport->cursor_line() + 1, viewport->cursor_column() + 1, {},
      [this, request_path, merge](std::vector<plugin::PluginHost::CompletionCandidate> items,
                                  std::string /*provider_error*/) {
        merge->sources.plugin_pending = false;
        if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
          return;
        }
        merge->plugin_items = TransformPluginCompletions(items);
        PublishCompletionMerge(merge, request_path);
      });
  return true;
}

std::vector<CompletionSessionItem> AssistService::TransformPluginCompletions(
    const std::vector<plugin::PluginHost::CompletionCandidate>& items) const {
  const bool snippets_on = EditorSnippetsSettingEnabled();
  std::vector<CompletionSessionItem> result;
  result.reserve(items.size());
  for (const auto& item : items) {
    result.push_back(CompletionSessionItem{
        .label = item.label,
        .detail = item.detail,
        .documentation = item.documentation,
        .insert_text = item.insert_text,
        .is_snippet = snippets_on && item.is_snippet,
    });
  }
  return result;
}

std::vector<CompletionSessionItem> AssistService::TransformLspCompletions(
    const std::optional<std::vector<LspClient::CompletionItem>>& items,
    lsp_encoding::PositionEncoding encoding) const {
  std::vector<CompletionSessionItem> result;
  if (!items.has_value()) {
    return result;
  }
  const bool snippets_on = EditorSnippetsSettingEnabled();
  editor::TextViewport* apply_viewport = operations_.active_editable_viewport();
  result.reserve(items->size());
  for (const auto& item : *items) {
    std::optional<editor::SelectionRange> item_range;
    if (item.replace_range.has_value() && apply_viewport != nullptr) {
      item_range = LspRangeToEditorRange(*apply_viewport, *item.replace_range, encoding);
    }
    result.push_back(CompletionSessionItem{
        .label = item.label,
        .detail = item.detail,
        .documentation = item.documentation,
        .insert_text = item.insert_text,
        .is_snippet = snippets_on && item.insert_text_format == 2,
        .replacement_range = item_range,
    });
  }
  return result;
}

void AssistService::PublishCompletionMerge(const std::shared_ptr<CompletionMerge>& merge,
                                           const std::filesystem::path& request_path) {
  if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
    return;
  }
  auto& session = context_->current_project_state.overlay.workflow.completion;
  const auto& primary = merge->sources.lsp_authoritative ? merge->lsp_items : merge->plugin_items;
  const auto& secondary =
      merge->sources.lsp_authoritative ? merge->plugin_items : merge->lsp_items;
  session.items = assist_merge::RankedUnion(
      primary, secondary, [](const CompletionSessionItem& item) { return item.label; });
  if (session.selected_index >= session.items.size()) {
    session.selected_index = 0;
  }
  session.source = merge->lsp_items.empty() ? "plugin" : "lsp";
  if (merge->sources.AnyPending()) {
    session.error = session.items.empty() ? "Loading..." : std::string{};
  } else if (session.items.empty()) {
    session.error = "No completions available";
    MaybeLogLspUnavailable(merge->language_id, merge->sources.lsp_authoritative);
  } else {
    session.error.clear();
  }
  operations_.request_overlay_redraw();
}

void AssistService::MaybeLogLspUnavailable(const std::string& language_id, bool lsp_authoritative) {
  // A server served the buffer, or the language has none configured: not a
  // failure worth logging on every request.
  if (lsp_authoritative || language_id.empty()) {
    return;
  }
  LspManager& manager = operations_.current_lsp_manager();
  if (!manager.HasServer(language_id)) {
    return;
  }
  const std::string detail = manager.LastServerError(language_id);
  const std::string message = detail.empty() ? ("LSP startup failed for " + language_id)
                                             : ("LSP startup failed for " + language_id + ": " + detail);
  output_channels_->AppendLine("lsp.log", "LSP Log", message);
}

AssistService::EditSideEffectsSnapshot AssistService::CaptureEditSnapshot(
    editor::TextViewport& viewport) const {
  EditSideEffectsSnapshot snapshot;
  snapshot.was_dirty = viewport.dirty();
  snapshot.cursor_before_line = viewport.cursor_line();
  if (auto* merge_tab = operations_.active_merge_tab();
      merge_tab != nullptr && &viewport == &merge_tab->result_viewport) {
    snapshot.before_lines = viewport.lines().Snapshot();
    snapshot.selection_before = viewport.selection_range();
    snapshot.cursor_before = editor::TextPosition{viewport.cursor_line(), viewport.cursor_column()};
  }
  return snapshot;
}

void AssistService::ApplyEditSideEffects(editor::TextViewport& viewport,
                                         const EditSideEffectsSnapshot& snapshot) const {
  if (auto* compare_tab = operations_.active_compare_tab();
      compare_tab != nullptr && &viewport == &compare_tab->right_viewport) {
    operations_.refresh_compare_tab_derived_state(*compare_tab);
    operations_.sync_compare_selection_from_viewport(*compare_tab, true);
  }
  if (auto* merge_tab = operations_.active_merge_tab();
      merge_tab != nullptr && &viewport == &merge_tab->result_viewport &&
      snapshot.cursor_before.has_value()) {
    operations_.update_merge_tracking_after_viewport_edit(*merge_tab, snapshot.before_lines,
                                                          snapshot.selection_before,
                                                          *snapshot.cursor_before);
  }
  operations_.reset_caret_blink();
  operations_.request_active_editable_last_change_redraw();
  if (viewport.dirty() != snapshot.was_dirty) {
    operations_.request_active_editable_blame_neighborhood_redraw(snapshot.cursor_before_line,
                                                                  viewport.cursor_line());
    operations_.request_tab_strip_redraw();
  }
}

bool AssistService::ApplySelectedCompletion() {
  auto& session = context_->current_project_state.overlay.workflow.completion;
  if (session.items.empty()) {
    return false;
  }
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr) {
    return false;
  }

  const CompletionSessionItem& item =
      session.items[std::min(session.selected_index, session.items.size() - 1)];
  // Prefer the item's server-supplied textEdit range; fall back to the session's
  // heuristic word range for plugin completions / items without a textEdit.
  const editor::SelectionRange replacement_range =
      item.replacement_range.value_or(session.replacement_range);
  const EditSideEffectsSnapshot snapshot = CaptureEditSnapshot(*viewport);
  const bool want_snippet = EditorSnippetsSettingEnabled() && item.is_snippet;
  bool snippet_applied = false;
  if (want_snippet) {
    if (TabEntry::EditorTabState* editor_tab = operations_.active_editor_tab()) {
      viewport->BeginUndoGroup();
      snippet_applied = editor::ExpandSnippetAtSelection(*viewport, editor_tab->snippet_session,
                                                         replacement_range, item.insert_text);
      if (!snippet_applied) {
        if (viewport->UndoGroupActive()) {
          viewport->EndUndoGroup();
        }
        if (!viewport->ReplaceRange(replacement_range, item.insert_text)) {
          return false;
        }
      }
    } else if (!viewport->ReplaceRange(replacement_range, item.insert_text)) {
      return false;
    }
  } else if (!viewport->ReplaceRange(replacement_range, item.insert_text)) {
    return false;
  }
  ApplyEditSideEffects(*viewport, snapshot);
  operations_.dismiss_overlay(true);
  return true;
}

bool AssistService::ShowInsertSnippetOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (!EditorSnippetsSettingEnabled()) {
    if (error_message != nullptr) {
      *error_message = "Snippets are disabled";
    }
    return false;
  }
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return false;
  }
  const std::string language_id = DetectViewportLanguageId(*viewport);
  const LanguageContract* contract = language_contract_->Find(language_id);
  if (contract == nullptr || contract->snippets.empty()) {
    if (error_message != nullptr) {
      *error_message = "No snippets for this language";
    }
    return false;
  }
  auto& session = context_->current_project_state.overlay.workflow.completion;
  session.items.clear();
  session.selected_index = 0;
  session.replacement_range = CompletionReplacementRange(*viewport);
  session.source = "snippet";
  session.error.clear();
  for (const auto& sn : contract->snippets) {
    session.items.push_back(CompletionSessionItem{
        .label = sn.label.empty() ? sn.prefix : sn.label,
        .detail = sn.prefix,
        .documentation = {},
        .insert_text = sn.body,
        .is_snippet = true,
    });
  }
  operations_.show_overlay(OverlayMode::Completion);
  return true;
}

bool AssistService::TrySnippetTabInEditor(bool shift_tab) {
  if (!EditorSnippetsSettingEnabled()) {
    return false;
  }
  TabEntry::EditorTabState* tab = operations_.active_editor_tab();
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (tab == nullptr || viewport == nullptr) {
    return false;
  }
  // With no active session a forward Tab tries prefix-triggered snippet expansion
  // before the caller falls back to inserting a literal tab.
  if (!tab->snippet_session.active) {
    return !shift_tab && TrySnippetPrefixExpansion(*tab, *viewport);
  }
  if (!editor::SnippetNavigateTab(*viewport, tab->snippet_session, shift_tab)) {
    return false;
  }
  tab->folding_model->MarkDirty();
  operations_.reset_caret_blink();
  operations_.request_active_editable_last_change_redraw();
  operations_.request_focused_editor_redraw();
  return true;
}

bool AssistService::TrySnippetPrefixExpansion(TabEntry::EditorTabState& tab,
                                              editor::TextViewport& viewport) {
  const std::string language_id = DetectViewportLanguageId(viewport);
  const LanguageContract* contract = language_contract_->Find(language_id);
  if (contract == nullptr || contract->snippets.empty()) {
    return false;
  }
  const editor::SelectionRange range = CompletionReplacementRange(viewport);
  if (range.start.line != range.end.line || range.end.column <= range.start.column) {
    return false;
  }
  const std::string_view line = LineAtOrEmpty(viewport.lines().Snapshot(), range.start.line);
  if (range.end.column > line.size()) {
    return false;
  }
  const std::string_view prefix =
      line.substr(range.start.column, range.end.column - range.start.column);
  if (prefix.empty()) {
    return false;
  }
  // Require a unique exact prefix match; ambiguity falls through to a literal tab.
  const LanguageSnippet* match = nullptr;
  for (const auto& snippet : contract->snippets) {
    if (snippet.prefix == prefix) {
      if (match != nullptr) {
        return false;
      }
      match = &snippet;
    }
  }
  if (match == nullptr) {
    return false;
  }

  const EditSideEffectsSnapshot snapshot = CaptureEditSnapshot(viewport);
  viewport.BeginUndoGroup();
  if (!editor::ExpandSnippetAtSelection(viewport, tab.snippet_session, range, match->body)) {
    if (viewport.UndoGroupActive()) {
      viewport.EndUndoGroup();
    }
    return false;
  }
  tab.folding_model->MarkDirty();
  ApplyEditSideEffects(viewport, snapshot);
  return true;
}

bool AssistService::TrySnippetEscapeInEditor() {
  if (!EditorSnippetsSettingEnabled()) {
    return false;
  }
  TabEntry::EditorTabState* tab = operations_.active_editor_tab();
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (tab == nullptr || viewport == nullptr || !tab->snippet_session.active) {
    return false;
  }
  if (!editor::SnippetHandleEscape(*viewport, tab->snippet_session)) {
    return false;
  }
  tab->folding_model->MarkDirty();
  operations_.reset_caret_blink();
  operations_.request_active_editable_last_change_redraw();
  operations_.request_focused_editor_redraw();
  return true;
}

void AssistService::NotifySnippetSessionCaretMoved() {
  TabEntry::EditorTabState* tab = operations_.active_editor_tab();
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (tab == nullptr || viewport == nullptr || !tab->snippet_session.active) {
    return;
  }
  editor::SnippetOnCaretMoved(*viewport, tab->snippet_session);
  if (!tab->snippet_session.active) {
    tab->folding_model->MarkDirty();
    operations_.reset_caret_blink();
    operations_.request_active_editable_last_change_redraw();
    operations_.request_focused_editor_redraw();
  }
}

void AssistService::ClearActiveSnippetSessionAfterUndo() {
  TabEntry::EditorTabState* tab = operations_.active_editor_tab();
  if (tab == nullptr || !tab->snippet_session.active) {
    return;
  }
  tab->snippet_session.Reset(nullptr);
}

bool AssistService::TrySnippetInsertTextInEditor(editor::TextViewport* viewport,
                                                 std::string_view text) {
  if (!EditorSnippetsSettingEnabled() || viewport == nullptr || text.empty()) {
    return false;
  }
  TabEntry::EditorTabState* tab = operations_.active_editor_tab();
  if (tab == nullptr || !tab->snippet_session.active) {
    return false;
  }
  const EditSideEffectsSnapshot snapshot = CaptureEditSnapshot(*viewport);
  if (!editor::SnippetTryInsertText(*viewport, tab->snippet_session, text)) {
    return false;
  }
  tab->folding_model->MarkDirty();
  ApplyEditSideEffects(*viewport, snapshot);
  return true;
}

bool AssistService::TrySnippetBackspaceInEditor(editor::TextViewport* viewport) {
  if (!EditorSnippetsSettingEnabled() || viewport == nullptr) {
    return false;
  }
  TabEntry::EditorTabState* tab = operations_.active_editor_tab();
  if (tab == nullptr || !tab->snippet_session.active) {
    return false;
  }
  const EditSideEffectsSnapshot snapshot = CaptureEditSnapshot(*viewport);
  if (!editor::SnippetTryBackspace(*viewport, tab->snippet_session)) {
    return false;
  }
  tab->folding_model->MarkDirty();
  ApplyEditSideEffects(*viewport, snapshot);
  return true;
}

bool AssistService::TrySnippetDeleteForwardInEditor(editor::TextViewport* viewport) {
  if (!EditorSnippetsSettingEnabled() || viewport == nullptr) {
    return false;
  }
  TabEntry::EditorTabState* tab = operations_.active_editor_tab();
  if (tab == nullptr || !tab->snippet_session.active) {
    return false;
  }
  const EditSideEffectsSnapshot snapshot = CaptureEditSnapshot(*viewport);
  if (!editor::SnippetTryDeleteForward(*viewport, tab->snippet_session)) {
    return false;
  }
  tab->folding_model->MarkDirty();
  ApplyEditSideEffects(*viewport, snapshot);
  return true;
}

bool AssistService::ShowCodeActionsOverlay(std::string* error_message,
                                           const editor::SelectionRange* explicit_range) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectViewportLanguageId(*viewport);
  const std::filesystem::path request_path = viewport->path();
  const std::optional<editor::SelectionRange> selection = viewport->selection_range();
  const editor::SelectionRange range =
      explicit_range != nullptr
          ? *explicit_range
          : selection.value_or(editor::SelectionRange{
                .start = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
                .end = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
            });

  // Open the overlay in a loading state immediately, then query the plugin worker
  // and the language server CONCURRENTLY. Neither blocks the UI; each fills the
  // overlay on its own drain via PublishCodeActionMerge (LSP-first, de-duped).
  auto& session = context_->current_project_state.overlay.workflow.code_actions;
  session.items.clear();
  session.selected_index = 0;
  session.source = "lsp";
  session.error = "Loading...";
  operations_.show_overlay(OverlayMode::CodeActions);

  auto merge = std::make_shared<CodeActionMerge>();
  merge->language_id = language_id;

  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    operations_.ensure_lsp_document_open(*viewport, *client, language_id);
    std::vector<LspClient::Diagnostic> context_diagnostics;
    if (operations_.collect_lsp_context_diagnostics) {
      context_diagnostics = operations_.collect_lsp_context_diagnostics(*viewport, range);
    }
    operations_.begin_tracked_lsp_request();
    const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
    client->RequestCodeActionAsync(
        FileUriForPath(request_path),
        LspClient::Range{
            .start =
                ByteColumnToLspPosition(*viewport, range.start.line, range.start.column, encoding),
            .end = ByteColumnToLspPosition(*viewport, range.end.line, range.end.column, encoding),
        },
        std::move(context_diagnostics),
        [this, request_path, merge](std::optional<std::vector<LspClient::CodeAction>> actions) {
          operations_.finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
            return;
          }
          merge->lsp_items = TransformLspCodeActions(actions);
          PublishCodeActionMerge(merge, request_path);
        });
  } else {
    merge->sources.lsp_pending = false;
  }

  plugin_runtime_->Host().QueryCodeActionsAsync(
      language_id, request_path, range.start.line + 1, range.start.column + 1, range.end.line + 1,
      range.end.column + 1,
      [this, request_path, merge](std::vector<plugin::PluginHost::CodeActionCandidate> items,
                                  std::string /*provider_error*/) {
        merge->sources.plugin_pending = false;
        if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
          return;
        }
        merge->plugin_items = TransformPluginCodeActions(items);
        PublishCodeActionMerge(merge, request_path);
      });
  return true;
}

std::vector<CodeActionSessionItem> AssistService::TransformPluginCodeActions(
    const std::vector<plugin::PluginHost::CodeActionCandidate>& items) const {
  std::vector<CodeActionSessionItem> result;
  result.reserve(items.size());
  for (const auto& item : items) {
    result.push_back(CodeActionSessionItem{
        .title = item.title,
        .command = item.command,
        .arguments = item.arguments,
    });
  }
  return result;
}

std::vector<CodeActionSessionItem> AssistService::TransformLspCodeActions(
    const std::optional<std::vector<LspClient::CodeAction>>& actions) const {
  std::vector<CodeActionSessionItem> result;
  if (!actions.has_value()) {
    return result;
  }
  result.reserve(actions->size());
  for (const auto& action : *actions) {
    std::vector<std::string> arguments;
    arguments.reserve(action.arguments.size());
    for (const auto& argument : action.arguments) {
      arguments.push_back(JsonValueToArgumentString(argument));
    }
    std::vector<CodeActionEdit> edits;
    if (action.has_edit) {
      for (const auto& [uri, text_edits] : action.edit.changes) {
        const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
        for (const auto& [lsp_range, new_text] : text_edits) {
          edits.push_back(CodeActionEdit{
              .path = path.value_or(std::filesystem::path{}),
              .range = editor::SelectionRange{
                  .start = editor::TextPosition{static_cast<std::size_t>(lsp_range.start.line),
                                                static_cast<std::size_t>(lsp_range.start.character)},
                  .end = editor::TextPosition{static_cast<std::size_t>(lsp_range.end.line),
                                              static_cast<std::size_t>(lsp_range.end.character)},
              },
              .new_text = new_text,
          });
        }
      }
    }
    result.push_back(CodeActionSessionItem{
        .title = action.title,
        .command = action.command,
        .arguments = std::move(arguments),
        .edits = std::move(edits),
    });
  }
  return result;
}

void AssistService::PublishCodeActionMerge(const std::shared_ptr<CodeActionMerge>& merge,
                                           const std::filesystem::path& request_path) {
  if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
    return;
  }
  auto& session = context_->current_project_state.overlay.workflow.code_actions;
  const auto& primary = merge->sources.lsp_authoritative ? merge->lsp_items : merge->plugin_items;
  const auto& secondary =
      merge->sources.lsp_authoritative ? merge->plugin_items : merge->lsp_items;
  // De-dupe by title + command so an identical action offered by both sources
  // appears once (an empty command still distinguishes edit-only quickfixes).
  session.items = assist_merge::RankedUnion(
      primary, secondary,
      [](const CodeActionSessionItem& item) { return item.title + "\x1f" + item.command; });
  if (session.selected_index >= session.items.size()) {
    session.selected_index = 0;
  }
  session.source = merge->lsp_items.empty() ? "plugin" : "lsp";
  if (merge->sources.AnyPending()) {
    session.error = session.items.empty() ? "Loading..." : std::string{};
  } else if (session.items.empty()) {
    session.error = "No code actions available";
    MaybeLogLspUnavailable(merge->language_id, merge->sources.lsp_authoritative);
  } else {
    session.error.clear();
  }
  operations_.request_overlay_redraw();
}

bool AssistService::ExecuteSelectedCodeAction() {
  auto& session = context_->current_project_state.overlay.workflow.code_actions;
  if (session.items.empty()) {
    return false;
  }
  const CodeActionSessionItem& action =
      session.items[std::min(session.selected_index, session.items.size() - 1)];

  // Inline WorkspaceEdit actions (clangd quickfixes like "remove #include X")
  // apply directly to the open buffers; command-style actions dispatch through
  // the command registry. An action with neither is inert.
  if (!action.edits.empty()) {
    const bool applied = operations_.apply_lsp_workspace_edit
                             ? operations_.apply_lsp_workspace_edit(action.edits)
                             : false;
    if (applied) {
      operations_.dismiss_overlay(true);
    } else {
      session.error = "Could not apply fix";
      operations_.request_overlay_redraw();
    }
    return applied;
  }

  if (action.command.empty()) {
    return false;
  }
  std::string error_message;
  const bool executed =
      operations_.execute_command_name(action.command, action.arguments, &error_message);
  if (executed) {
    operations_.dismiss_overlay(true);
  } else {
    session.error = error_message;
    operations_.request_overlay_redraw();
  }
  return executed;
}

bool AssistService::GoToLspDefinition(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }

  // Query the plugin provider and the language server CONCURRENTLY (mirrors
  // completion / code actions). The server is authoritative for its language:
  // navigation waits for its answer and uses the plugin result only if the
  // server comes back empty, so a slower LSP reply can't be pre-empted by a
  // stale plugin hit.
  const std::string language_id = DetectViewportLanguageId(*viewport);
  const std::filesystem::path request_path = viewport->path();
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();

  auto merge = std::make_shared<NavigationMerge>();

  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    operations_.ensure_lsp_document_open(*viewport, *client, language_id);
    operations_.begin_tracked_lsp_request();
    merge->lsp_encoding = LspEncodingForClient(*client);
    client->RequestGoToDefinitionAsync(
        FileUriForPath(request_path),
        ByteColumnToLspPosition(*viewport, request_line, request_column, merge->lsp_encoding),
        [this, request_path, merge](std::optional<std::vector<LspClient::Location>> locations) {
          operations_.finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          if (locations.has_value()) {
            merge->lsp_locations = std::move(*locations);
          }
          ResolveDefinitionNavigation(merge, request_path);
        });
  } else {
    merge->sources.lsp_pending = false;
  }

  plugin_runtime_->Host().QueryDefinitionAsync(
      language_id, request_path, request_line + 1, request_column + 1,
      [this, request_path, merge](std::vector<plugin::PluginHost::LocationResult> locations,
                                  std::string /*provider_error*/) {
        merge->sources.plugin_pending = false;
        merge->plugin_locations = std::move(locations);
        ResolveDefinitionNavigation(merge, request_path);
      });
  return true;
}

void AssistService::ResolveDefinitionNavigation(const std::shared_ptr<NavigationMerge>& merge,
                                                const std::filesystem::path& request_path) {
  if (merge->acted) {
    return;
  }
  if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
    // The buffer switched since the request; abandon without navigating a
    // different file or emitting a spurious "no definition" message.
    return;
  }
  const assist_merge::NavChoice choice = assist_merge::ChooseNavigation(
      merge->sources.lsp_authoritative, merge->sources.lsp_pending,
      !merge->lsp_locations.empty(), merge->sources.plugin_pending,
      !merge->plugin_locations.empty());
  switch (choice) {
    case assist_merge::NavChoice::Pending:
      return;
    case assist_merge::NavChoice::None:
      merge->acted = true;
      output_channels_->AppendLine("lsp.definition", "LSP Definition", "No definition found");
      return;
    case assist_merge::NavChoice::UsePlugin:
      merge->acted = true;
      NavigateToPluginLocation(merge->plugin_locations.front());
      return;
    case assist_merge::NavChoice::UseLsp: {
      merge->acted = true;
      const LspClient::Location& location = merge->lsp_locations.front();
      const std::optional<std::filesystem::path> path = PathFromFileUri(location.uri);
      if (!path.has_value() || !operations_.open_file_in_new_tab(*path)) {
        return;
      }
      if (editor::TextViewport* active = operations_.active_editor_viewport(); active != nullptr) {
        const std::size_t target_line =
            static_cast<std::size_t>(std::max(location.range.start.line, 0));
        active->MoveCursorTo(target_line,
                             LspPositionToByteColumn(*active, target_line,
                                                     location.range.start.character,
                                                     merge->lsp_encoding));
        operations_.reset_caret_blink();
        operations_.request_focused_editor_redraw();
      }
      return;
    }
  }
}

bool AssistService::FindLspReferences(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }

  // Query the plugin provider and the language server CONCURRENTLY, then render
  // the de-duplicated union once both have resolved (LSP-first for served
  // languages). A single output-channel rebuild avoids mid-flight flicker.
  const std::string language_id = DetectViewportLanguageId(*viewport);
  const std::filesystem::path request_path = viewport->path();
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();

  auto merge = std::make_shared<NavigationMerge>();

  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    operations_.ensure_lsp_document_open(*viewport, *client, language_id);
    operations_.begin_tracked_lsp_request();
    client->RequestFindReferencesAsync(
        FileUriForPath(request_path),
        ByteColumnToLspPosition(*viewport, request_line, request_column,
                                LspEncodingForClient(*client)),
        true,
        [this, request_path, merge](std::optional<std::vector<LspClient::Location>> locations) {
          operations_.finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          if (locations.has_value()) {
            merge->lsp_locations = std::move(*locations);
          }
          PublishReferenceMerge(merge, request_path);
        });
  } else {
    merge->sources.lsp_pending = false;
  }

  plugin_runtime_->Host().QueryReferencesAsync(
      language_id, request_path, request_line + 1, request_column + 1, true,
      [this, request_path, merge](std::vector<plugin::PluginHost::LocationResult> locations,
                                  std::string /*provider_error*/) {
        merge->sources.plugin_pending = false;
        merge->plugin_locations = std::move(locations);
        PublishReferenceMerge(merge, request_path);
      });
  return true;
}

void AssistService::PublishReferenceMerge(const std::shared_ptr<NavigationMerge>& merge,
                                          const std::filesystem::path& request_path) {
  if (merge->acted || merge->sources.AnyPending()) {
    return;  // render exactly once, after both sources resolve
  }
  merge->acted = true;
  if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
    return;
  }

  // Normalize both sources to (path, 1-based line, 1-based column). LSP columns
  // use the raw code-unit offset: references only show line:col, so a slightly
  // off non-ASCII column is not worth a per-entry encoding round-trip.
  struct RefTarget {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
  };
  std::vector<RefTarget> lsp_targets;
  lsp_targets.reserve(merge->lsp_locations.size());
  for (const auto& location : merge->lsp_locations) {
    const std::optional<std::filesystem::path> path = PathFromFileUri(location.uri);
    if (!path.has_value()) {
      continue;
    }
    lsp_targets.push_back(
        RefTarget{*path, static_cast<std::size_t>(std::max(location.range.start.line, 0)) + 1,
                  static_cast<std::size_t>(std::max(location.range.start.character, 0)) + 1});
  }
  std::vector<RefTarget> plugin_targets;
  plugin_targets.reserve(merge->plugin_locations.size());
  for (const auto& location : merge->plugin_locations) {
    if (location.path.empty()) {
      continue;
    }
    plugin_targets.push_back(RefTarget{location.path, location.line, location.column});
  }

  const auto& primary = merge->sources.lsp_authoritative ? lsp_targets : plugin_targets;
  const auto& secondary = merge->sources.lsp_authoritative ? plugin_targets : lsp_targets;
  const std::vector<RefTarget> merged =
      assist_merge::RankedUnion(primary, secondary, [](const RefTarget& target) {
        return target.path.generic_string() + ":" + std::to_string(target.line) + ":" +
               std::to_string(target.column);
      });

  output_channels_->Clear("lsp.references");
  if (merged.empty()) {
    output_channels_->AppendLine("lsp.references", "LSP References", "No references found");
    return;
  }
  std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
  for (std::size_t index = 0; index < merged.size(); ++index) {
    EmitReferenceEntry("lsp.references", "LSP References", merged[index].path, merged[index].line,
                       merged[index].column, index + 1 < merged.size(), file_line_cache);
  }
}

namespace {
bool IsIdentifierByte(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
         ch == '_';
}
}  // namespace

std::string AssistService::SymbolAtCursor() const {
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr) {
    return {};
  }
  const std::string_view line = LspLineView(*viewport, viewport->cursor_line());
  std::size_t begin = std::min(viewport->cursor_column(), line.size());
  std::size_t end = begin;
  while (begin > 0 && IsIdentifierByte(line[begin - 1])) {
    --begin;
  }
  while (end < line.size() && IsIdentifierByte(line[end])) {
    ++end;
  }
  return std::string(line.substr(begin, end - begin));
}

bool AssistService::RenameSymbol(const std::string& new_name, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (new_name.empty()) {
    return false;
  }
  editor::TextViewport* viewport = RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  LspClient* client = PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }
  const std::filesystem::path request_path = viewport->path();
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  const LspClient::Position position =
      ByteColumnToLspPosition(*viewport, viewport->cursor_line(), viewport->cursor_column(), encoding);
  client->RequestRenameAsync(
      FileUriForPath(request_path), position, new_name,
      [this, request_path, new_name](std::optional<LspClient::WorkspaceEdit> edit) {
        operations_.finish_tracked_lsp_request();
        if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
          return;
        }
        if (!edit.has_value() || edit->changes.empty()) {
          output_channels_->AppendLine("lsp.log", "LSP Log", "No rename edits returned");
          return;
        }
        // Flatten the URI-keyed WorkspaceEdit into the shared edit records (0-based
        // LSP coordinates; the apply path maps columns through the position
        // encoding). The host decides whether to apply in place or open + save the
        // files that are not currently open.
        std::vector<CodeActionEdit> workspace_edits;
        for (const auto& [uri, text_edits] : edit->changes) {
          const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
          for (const auto& [range, new_text] : text_edits) {
            workspace_edits.push_back(CodeActionEdit{
                .path = path.value_or(std::filesystem::path{}),
                .range = editor::SelectionRange{
                    .start = editor::TextPosition{
                        static_cast<std::size_t>(std::max(0, range.start.line)),
                        static_cast<std::size_t>(std::max(0, range.start.character))},
                    .end = editor::TextPosition{
                        static_cast<std::size_t>(std::max(0, range.end.line)),
                        static_cast<std::size_t>(std::max(0, range.end.character))},
                },
                .new_text = new_text,
            });
          }
        }
        if (operations_.apply_rename_workspace_edit) {
          operations_.apply_rename_workspace_edit(new_name, workspace_edits);
        } else if (operations_.apply_lsp_workspace_edit) {
          operations_.apply_lsp_workspace_edit(workspace_edits);
        }
      });
  return true;
}

bool AssistService::FormatActiveDocument(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  // Formatting is LSP-only (no plugin provider path). PrepareLspRequest resolves the
  // client, opens the document, begins the tracked request, and records the
  // unavailable-server message when there is none.
  LspClient* client = PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }
  const std::filesystem::path request_path = viewport->path();
  const int tab_size = static_cast<int>(viewport->indent_width() > 0 ? viewport->indent_width() : 4);
  client->RequestFormattingAsync(
      FileUriForPath(request_path), tab_size, /*insert_spaces=*/true,
      [this, request_path](std::optional<std::vector<LspClient::TextEdit>> edits) {
        operations_.finish_tracked_lsp_request();
        // Drop superseded results (buffer switched/closed) before mutating.
        if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
          return;
        }
        if (!edits.has_value() || edits->empty()) {
          output_channels_->AppendLine("lsp.log", "LSP Log", "No formatting changes");
          return;
        }
        // Convert to the shared workspace-edit records (0-based LSP coordinates; the
        // apply path maps their columns through the server's position encoding) and
        // apply the whole set together, highest-position-first.
        std::vector<CodeActionEdit> workspace_edits;
        workspace_edits.reserve(edits->size());
        for (const auto& [range, new_text] : *edits) {
          workspace_edits.push_back(CodeActionEdit{
              .path = request_path,
              .range = editor::SelectionRange{
                  .start = editor::TextPosition{
                      static_cast<std::size_t>(std::max(0, range.start.line)),
                      static_cast<std::size_t>(std::max(0, range.start.character))},
                  .end = editor::TextPosition{
                      static_cast<std::size_t>(std::max(0, range.end.line)),
                      static_cast<std::size_t>(std::max(0, range.end.character))},
              },
              .new_text = new_text,
          });
        }
        if (operations_.apply_lsp_workspace_edit &&
            !operations_.apply_lsp_workspace_edit(workspace_edits)) {
          output_channels_->AppendLine("lsp.log", "LSP Log", "Could not apply formatting");
        }
      });
  return true;
}

bool AssistService::ShowSignatureHelp(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return false;
  }

  // Plugin language providers own signature help (no LSP fallback yet), dispatched
  // to the worker so the lookup never blocks the UI. An empty result is simply
  // "nothing to show"; the popup is built and shown from the callback on the drain.
  const std::string language_id = DetectViewportLanguageId(*viewport);
  const std::filesystem::path request_path = viewport->path();
  plugin_runtime_->Host().QuerySignatureHelpAsync(
      language_id, request_path, viewport->cursor_line() + 1, viewport->cursor_column() + 1,
      [this, request_path](bool resolved, plugin::PluginHost::SignatureHelpResult result,
                           std::string /*provider_error*/) {
        editor::TextViewport* current = operations_.active_editable_viewport();
        if (current == nullptr || current->path() != request_path) {
          return;  // superseded
        }
        if (!resolved || result.signatures.empty()) {
          return;
        }
        const std::size_t active =
            result.active_signature >= 0 &&
                    static_cast<std::size_t>(result.active_signature) < result.signatures.size()
                ? static_cast<std::size_t>(result.active_signature)
                : 0;
        const plugin::PluginHost::SignatureInfo& signature = result.signatures[active];

        // Build the supporting block off the render path: lead with the active
        // parameter (so the user sees which argument they are typing) then the
        // signature documentation.
        std::string documentation;
        if (signature.active_parameter >= 0 &&
            static_cast<std::size_t>(signature.active_parameter) < signature.parameters.size()) {
          const plugin::PluginHost::SignatureParameter& parameter =
              signature.parameters[static_cast<std::size_t>(signature.active_parameter)];
          if (!parameter.label.empty()) {
            documentation = parameter.label;
          }
          if (!parameter.documentation.empty()) {
            if (!documentation.empty()) {
              documentation += " — ";
            }
            documentation += parameter.documentation;
          }
        }
        if (!signature.documentation.empty()) {
          if (!documentation.empty()) {
            documentation += "\n";
          }
          documentation += signature.documentation;
        }

        if (operations_.show_signature_help) {
          operations_.show_signature_help(signature.label, std::move(documentation));
        }
      });
  return true;
}

void AssistService::NavigateToPluginLocation(const plugin::PluginHost::LocationResult& location) {
  if (location.path.empty() || !operations_.open_file_in_new_tab(location.path)) {
    return;
  }
  if (editor::TextViewport* active = operations_.active_editor_viewport(); active != nullptr) {
    // Provider line/column are 1-based; the viewport caret is 0-based.
    active->MoveCursorTo(location.line > 0 ? location.line - 1 : 0,
                         location.column > 0 ? location.column - 1 : 0);
    operations_.reset_caret_blink();
    operations_.request_focused_editor_redraw();
  }
}

editor::TextViewport* AssistService::RequireActiveEditableViewport(
    std::string* error_message) const {
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return nullptr;
  }
  return viewport;
}

LspClient* AssistService::PrepareLspRequest(editor::TextViewport& viewport,
                                            std::string* error_message) {
  std::string language_id;
  LspClient* client = operations_.lsp_client_for_viewport(viewport, &language_id);
  if (client == nullptr) {
    const std::string failure =
        LspUnavailableMessage(operations_.current_lsp_manager(), language_id, {});
    output_channels_->AppendLine("lsp.log", "LSP Log", failure);
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return nullptr;
  }
  operations_.ensure_lsp_document_open(viewport, *client, language_id);
  operations_.begin_tracked_lsp_request();
  return client;
}

void AssistService::EmitReferenceEntry(
    const char* channel_id, const char* channel_title, const std::filesystem::path& path,
    std::size_t line, std::size_t column, bool append_separator,
    std::map<std::filesystem::path, std::vector<std::string>>& file_line_cache) const {
  const std::string label =
      context_->current_project_state.root.empty()
          ? path.generic_string()
          : RelativePathLabel(context_->current_project_state.root, path);
  output_channels_->AppendLine(
      channel_id, channel_title,
      label + ":" + std::to_string(line) + ":" + std::to_string(column));

  const auto lines_it = file_line_cache.find(path);
  const std::vector<std::string>* file_lines = nullptr;
  if (lines_it != file_line_cache.end()) {
    file_lines = &lines_it->second;
  } else if (const auto text = util::ReadTextFile(path); text.has_value()) {
    file_lines = &file_line_cache.emplace(path, util::SplitLines(*text)).first->second;
  }
  if (file_lines == nullptr || file_lines->empty()) {
    return;
  }

  const std::size_t target_line = line > 0 ? line : 1;
  const std::size_t first_line = target_line > 1 ? target_line - 1 : 1;
  const std::size_t last_line = target_line + 1;
  for (std::size_t line_number = first_line; line_number <= last_line; ++line_number) {
    if (line_number == 0 || line_number > file_lines->size()) {
      continue;
    }
    output_channels_->AppendLine(
        channel_id, channel_title,
        std::string(line_number == target_line ? " > " : "   ") + std::to_string(line_number) +
            " | " + (*file_lines)[line_number - 1]);
  }
  if (append_separator) {
    output_channels_->AppendLine(channel_id, channel_title, "");
  }
}

}  // namespace microide::workspace
