#include "workspace/AssistService.h"

#include <algorithm>
#include <filesystem>
#include <map>
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

// Convert an LSP range (0-based, server position encoding) to an editor byte-column
// SelectionRange, clamped to the document. The `character` field is mapped through
// the negotiated position encoding so non-ASCII lines land on the right byte.
editor::SelectionRange LspRangeToEditorRange(const editor::TextViewport& viewport,
                                             const LspClient::Range& range,
                                             lsp_encoding::PositionEncoding encoding) {
  const auto& lines = viewport.lines();
  const auto to_position = [&](const LspClient::Position& p) {
    const std::size_t line = static_cast<std::size_t>(std::max(0, p.line));
    const std::size_t character = static_cast<std::size_t>(std::max(0, p.character));
    const std::string_view text = LineAtOrEmpty(lines.Snapshot(), line);
    return editor::TextPosition{line,
                                lsp_encoding::LspCharacterToByteColumn(text, character, encoding)};
  };
  return editor::SelectionRange{.start = to_position(range.start), .end = to_position(range.end)};
}

lsp_encoding::PositionEncoding EncodingForClient(const LspClient& client) {
  return lsp_encoding::ParsePositionEncoding(client.ServerPositionEncoding());
}

// Convert an editor byte column (on `line` of `viewport`) to an outbound LSP
// position in the server's negotiated encoding. Requests must be phrased in the
// server's units or they resolve at the wrong token on non-ASCII lines.
LspClient::Position ByteColumnToLspPosition(const editor::TextViewport& viewport, std::size_t line,
                                            std::size_t byte_column,
                                            lsp_encoding::PositionEncoding encoding) {
  const std::string_view text = LineAtOrEmpty(viewport.lines().Snapshot(), line);
  return LspClient::Position{
      static_cast<int>(line),
      static_cast<int>(lsp_encoding::ByteColumnToLspCharacter(text, byte_column, encoding))};
}

// Convert an inbound LSP position's `character` (server encoding) to an editor
// byte column on `line` of `viewport`.
std::size_t LspPositionToByteColumn(const editor::TextViewport& viewport, std::size_t line,
                                    int character, lsp_encoding::PositionEncoding encoding) {
  const std::string_view text = LineAtOrEmpty(viewport.lines().Snapshot(), line);
  return lsp_encoding::LspCharacterToByteColumn(
      text, static_cast<std::size_t>(std::max(0, character)), encoding);
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

  // Open the overlay in a loading state immediately; the plugin completion query
  // runs on the worker and never blocks the UI. Results (or the LSP fallback) fill
  // in on the next mailbox drain. With no worker wired the callback runs inline.
  auto& session = context_->current_project_state.overlay.workflow.completion;
  session.items.clear();
  session.selected_index = 0;
  session.replacement_range = CompletionReplacementRange(*viewport);
  session.source = "plugin";
  session.error = "Loading...";
  operations_.show_overlay(OverlayMode::Completion);

  plugin_runtime_->Host().QueryCompletionsAsync(
      language_id, request_path, viewport->cursor_line() + 1, viewport->cursor_column() + 1, {},
      [this, language_id, request_path](
          std::vector<plugin::PluginHost::CompletionCandidate> items, std::string provider_error) {
        // Drop superseded results: the active editable buffer changed since the
        // request was issued.
        editor::TextViewport* current = operations_.active_editable_viewport();
        if (ResultIsStale(current, request_path)) {
          return;
        }
        auto& current_session = context_->current_project_state.overlay.workflow.completion;
        if (!items.empty()) {
          current_session.items.clear();
          current_session.selected_index = 0;
          current_session.source = "plugin";
          current_session.error = std::move(provider_error);
          const bool snippets_on = EditorSnippetsSettingEnabled();
          for (const auto& item : items) {
            current_session.items.push_back(CompletionSessionItem{
                .label = item.label,
                .detail = item.detail,
                .documentation = item.documentation,
                .insert_text = item.insert_text,
                .is_snippet = snippets_on && item.is_snippet,
            });
          }
          operations_.request_overlay_redraw();
          return;
        }
        // No plugin completions: fall back to the language server in the same overlay.
        BeginLspCompletionFallback(*current, language_id, provider_error);
      });
  return true;
}

void AssistService::BeginLspCompletionFallback(editor::TextViewport& viewport,
                                               const std::string& language_id,
                                               const std::string& provider_error) {
  auto& session = context_->current_project_state.overlay.workflow.completion;
  LspClient* client = operations_.lsp_client_for_viewport(viewport, nullptr);
  if (client == nullptr) {
    const std::string failure =
        LspUnavailableMessage(operations_.current_lsp_manager(), language_id, provider_error);
    output_channels_->AppendLine("lsp.log", "LSP Log", failure);
    session.items.clear();
    session.selected_index = 0;
    session.source = "plugin";
    session.error = failure;
    operations_.request_overlay_redraw();
    return;
  }

  operations_.ensure_lsp_document_open(viewport, *client, language_id);
  const std::filesystem::path request_path = viewport.path();
  session.items.clear();
  session.selected_index = 0;
  session.replacement_range = CompletionReplacementRange(viewport);
  session.source = "lsp";
  session.error = "Loading...";
  operations_.request_overlay_redraw();
  operations_.begin_tracked_lsp_request();
  const lsp_encoding::PositionEncoding encoding =
      lsp_encoding::ParsePositionEncoding(client->ServerPositionEncoding());
  const std::size_t request_line = viewport.cursor_line();
  const std::string_view cursor_line_text =
      LineAtOrEmpty(viewport.lines().Snapshot(), request_line);
  client->RequestCompletionAsync(
      FileUriForPath(viewport.path()),
      LspClient::Position{static_cast<int>(request_line),
                          static_cast<int>(lsp_encoding::ByteColumnToLspCharacter(
                              cursor_line_text, viewport.cursor_column(), encoding))},
      [this, request_path, encoding](std::optional<std::vector<LspClient::CompletionItem>> items) {
        operations_.finish_tracked_lsp_request();
        // Drop superseded results: the active editable buffer changed since the
        // request was issued (mirrors the plugin completion guard above). Without
        // this a slow LSP response clobbers a newer completion session or lands
        // across a file/project switch. finish_tracked above must run first so the
        // in-flight counter is not leaked when we bail.
        if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
          return;
        }
        auto& current_session = context_->current_project_state.overlay.workflow.completion;
        current_session.items.clear();
        current_session.selected_index = 0;
        current_session.source = "lsp";
        if (!items.has_value() || items->empty()) {
          current_session.error = "No completions available";
        } else {
          current_session.error.clear();
          editor::TextViewport* apply_viewport = operations_.active_editable_viewport();
          for (const auto& item : *items) {
            const bool snippets_on = EditorSnippetsSettingEnabled();
            std::optional<editor::SelectionRange> item_range;
            if (item.replace_range.has_value() && apply_viewport != nullptr) {
              item_range = LspRangeToEditorRange(*apply_viewport, *item.replace_range, encoding);
            }
            current_session.items.push_back(CompletionSessionItem{
                .label = item.label,
                .detail = item.detail,
                .documentation = item.documentation,
                .insert_text = item.insert_text,
                .is_snippet = snippets_on && item.insert_text_format == 2,
                .replacement_range = item_range,
            });
          }
        }
        operations_.request_overlay_redraw();
      });
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

  // Open the overlay in a loading state immediately; the plugin code-action query
  // runs on the worker without blocking the UI. Results (or the LSP fallback) fill
  // in on the drain. With no worker wired the callback runs inline.
  auto& session = context_->current_project_state.overlay.workflow.code_actions;
  session.items.clear();
  session.selected_index = 0;
  session.source = "plugin";
  session.error = "Loading...";
  operations_.show_overlay(OverlayMode::CodeActions);

  plugin_runtime_->Host().QueryCodeActionsAsync(
      language_id, request_path, range.start.line + 1, range.start.column + 1, range.end.line + 1,
      range.end.column + 1,
      [this, language_id, request_path, range](
          std::vector<plugin::PluginHost::CodeActionCandidate> items, std::string provider_error) {
        editor::TextViewport* current = operations_.active_editable_viewport();
        if (current == nullptr || current->path() != request_path) {
          return;  // superseded
        }
        auto& current_session = context_->current_project_state.overlay.workflow.code_actions;
        if (!items.empty()) {
          current_session.items.clear();
          current_session.selected_index = 0;
          current_session.source = "plugin";
          current_session.error = std::move(provider_error);
          for (const auto& item : items) {
            current_session.items.push_back(CodeActionSessionItem{
                .title = item.title,
                .command = item.command,
                .arguments = item.arguments,
            });
          }
          operations_.request_overlay_redraw();
          return;
        }
        BeginLspCodeActionFallback(*current, language_id, range, provider_error);
      });
  return true;
}

void AssistService::BeginLspCodeActionFallback(editor::TextViewport& viewport,
                                               const std::string& language_id,
                                               const editor::SelectionRange& range,
                                               const std::string& provider_error) {
  auto& session = context_->current_project_state.overlay.workflow.code_actions;
  LspClient* client = operations_.lsp_client_for_viewport(viewport, nullptr);
  if (client == nullptr) {
    const std::string failure =
        LspUnavailableMessage(operations_.current_lsp_manager(), language_id, provider_error);
    output_channels_->AppendLine("lsp.log", "LSP Log", failure);
    session.items.clear();
    session.selected_index = 0;
    session.source = "plugin";
    session.error = failure;
    operations_.request_overlay_redraw();
    return;
  }

  operations_.ensure_lsp_document_open(viewport, *client, language_id);
  const std::filesystem::path request_path = viewport.path();
  std::vector<LspClient::Diagnostic> context_diagnostics;
  if (operations_.collect_lsp_context_diagnostics) {
    context_diagnostics = operations_.collect_lsp_context_diagnostics(viewport, range);
  }
  session.items.clear();
  session.selected_index = 0;
  session.source = "lsp";
  session.error = "Loading...";
  operations_.request_overlay_redraw();
  operations_.begin_tracked_lsp_request();
  const lsp_encoding::PositionEncoding code_action_encoding = EncodingForClient(*client);
  client->RequestCodeActionAsync(
      FileUriForPath(viewport.path()),
      LspClient::Range{
          .start = ByteColumnToLspPosition(viewport, range.start.line, range.start.column,
                                           code_action_encoding),
          .end = ByteColumnToLspPosition(viewport, range.end.line, range.end.column,
                                         code_action_encoding),
      },
      std::move(context_diagnostics),
      [this, request_path](std::optional<std::vector<LspClient::CodeAction>> actions) {
        operations_.finish_tracked_lsp_request();
        // Drop superseded results if the active editable buffer changed since the
        // request was issued (mirrors the plugin guard); finish_tracked above must
        // run first so the in-flight counter is not leaked when we bail.
        if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
          return;
        }
        auto& current_session = context_->current_project_state.overlay.workflow.code_actions;
        current_session.items.clear();
        current_session.selected_index = 0;
        current_session.source = "lsp";
        if (!actions.has_value() || actions->empty()) {
          current_session.error = "No code actions available";
        } else {
          current_session.error.clear();
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
            current_session.items.push_back(CodeActionSessionItem{
                .title = action.title,
                .command = action.command,
                .arguments = std::move(arguments),
                .edits = std::move(edits),
            });
          }
        }
        operations_.request_overlay_redraw();
      });
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

  // Plugin-native definition providers run first (mirrors completion / code action
  // orchestration), dispatched to the worker so the lookup never blocks the UI. A
  // non-empty result navigates; otherwise the callback hands off to the LSP path.
  const std::string language_id = DetectViewportLanguageId(*viewport);
  const std::filesystem::path request_path = viewport->path();
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();
  plugin_runtime_->Host().QueryDefinitionAsync(
      language_id, request_path, request_line + 1, request_column + 1,
      [this, request_path, request_line, request_column](
          std::vector<plugin::PluginHost::LocationResult> locations, std::string /*provider_error*/) {
        editor::TextViewport* current = operations_.active_editable_viewport();
        if (current == nullptr || current->path() != request_path) {
          return;  // superseded
        }
        if (!locations.empty()) {
          NavigateToPluginLocation(locations.front());
          return;
        }
        LspClient* client = PrepareLspRequest(*current, nullptr);
        if (client == nullptr) {
          return;
        }
        const lsp_encoding::PositionEncoding encoding = EncodingForClient(*client);
        client->RequestGoToDefinitionAsync(
            FileUriForPath(request_path),
            ByteColumnToLspPosition(*current, request_line, request_column, encoding),
            [this, encoding](std::optional<std::vector<LspClient::Location>> locations) {
              operations_.finish_tracked_lsp_request();
              if (!locations.has_value() || locations->empty()) {
                output_channels_->AppendLine("lsp.definition", "LSP Definition",
                                             "No definition found");
                return;
              }
              const std::optional<std::filesystem::path> path =
                  PathFromFileUri(locations->front().uri);
              if (!path.has_value()) {
                return;
              }
              if (!operations_.open_file_in_new_tab(*path)) {
                return;
              }
              if (editor::TextViewport* active = operations_.active_editor_viewport();
                  active != nullptr) {
                const std::size_t target_line = static_cast<std::size_t>(
                    std::max(locations->front().range.start.line, 0));
                active->MoveCursorTo(target_line,
                                     LspPositionToByteColumn(*active, target_line,
                                                             locations->front().range.start.character,
                                                             encoding));
                operations_.reset_caret_blink();
                operations_.request_focused_editor_redraw();
              }
            });
      });
  return true;
}

bool AssistService::FindLspReferences(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }

  // Plugin-native reference providers run first, dispatched to the worker so the
  // lookup never blocks the UI. A non-empty result renders into the References
  // output channel; otherwise the callback hands off to the LSP path.
  const std::string language_id = DetectViewportLanguageId(*viewport);
  const std::filesystem::path request_path = viewport->path();
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();
  plugin_runtime_->Host().QueryReferencesAsync(
      language_id, request_path, request_line + 1, request_column + 1, true,
      [this, request_path, request_line, request_column](
          std::vector<plugin::PluginHost::LocationResult> locations, std::string /*provider_error*/) {
        editor::TextViewport* current = operations_.active_editable_viewport();
        if (current == nullptr || current->path() != request_path) {
          return;  // superseded
        }
        if (!locations.empty()) {
          EmitPluginReferences(locations);
          return;
        }
        LspClient* client = PrepareLspRequest(*current, nullptr);
        if (client == nullptr) {
          return;
        }
        client->RequestFindReferencesAsync(
            FileUriForPath(request_path),
            ByteColumnToLspPosition(*current, request_line, request_column,
                                    EncodingForClient(*client)),
            true,
            [this](std::optional<std::vector<LspClient::Location>> locations) {
              operations_.finish_tracked_lsp_request();
              output_channels_->Clear("lsp.references");
              if (!locations.has_value() || locations->empty()) {
                output_channels_->AppendLine("lsp.references", "LSP References",
                                             "No references found");
                return;
              }
              std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
              for (std::size_t index = 0; index < locations->size(); ++index) {
                const auto& location = (*locations)[index];
                const std::optional<std::filesystem::path> path = PathFromFileUri(location.uri);
                if (!path.has_value()) {
                  continue;
                }
                // LSP positions are 0-based; the shared formatter expects 1-based.
                EmitReferenceEntry(
                    "lsp.references", "LSP References", *path,
                    static_cast<std::size_t>(std::max(location.range.start.line, 0)) + 1,
                    static_cast<std::size_t>(std::max(location.range.start.character, 0)) + 1,
                    index + 1 < locations->size(), file_line_cache);
              }
            });
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

void AssistService::EmitPluginReferences(
    const std::vector<plugin::PluginHost::LocationResult>& locations) {
  output_channels_->Clear("lsp.references");
  std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
  for (std::size_t index = 0; index < locations.size(); ++index) {
    const auto& location = locations[index];
    if (location.path.empty()) {
      continue;
    }
    // Provider line/column are already 1-based.
    EmitReferenceEntry("lsp.references", "References", location.path, location.line,
                       location.column, index + 1 < locations.size(), file_line_cache);
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
