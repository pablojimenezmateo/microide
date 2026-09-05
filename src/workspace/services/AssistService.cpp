#include "workspace/services/AssistService.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/SnippetEngine.h"
#include "util/JsonValue.h"
#include "util/PathMatch.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/FileUri.h"
#include "workspace/lsp/LspWorkspaceEditOps.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

namespace {

bool PositionBefore(const editor::TextPosition& a, const editor::TextPosition& b) {
  return a.line < b.line || (a.line == b.line && a.column < b.column);
}

// Where `position` (at or after `edit`'s end, in the pre-edit document) sits
// after `edit` replaced its range with `new_text`.
editor::TextPosition RemapAcrossEdit(editor::TextPosition position,
                                     const CompletionAdditionalEdit& edit) {
  const std::size_t newlines =
      static_cast<std::size_t>(std::count(edit.new_text.begin(), edit.new_text.end(), '\n'));
  const std::size_t last_segment =
      edit.new_text.size() - (newlines == 0 ? 0 : edit.new_text.rfind('\n') + 1);
  if (position.line == edit.range.end.line) {
    const std::size_t tail = position.column - edit.range.end.column;
    return editor::TextPosition{
        edit.range.start.line + newlines,
        (newlines == 0 ? edit.range.start.column + last_segment : last_segment) + tail};
  }
  const std::size_t removed_lines = edit.range.end.line - edit.range.start.line;
  return editor::TextPosition{position.line + newlines - removed_lines, position.column};
}

// Applies a completion item's additionalTextEdits, highest first so each stays
// valid against the still-unedited text below it, and returns `insertion`
// remapped across every edit that sits above it. An edit overlapping the
// insertion range is skipped: the protocol forbids it and applying it would
// double-edit the token being completed.
editor::SelectionRange ApplyCompletionAdditionalEdits(
    editor::TextViewport& viewport,
    const std::vector<CompletionAdditionalEdit>& edits,
    editor::SelectionRange insertion) {
  std::vector<const CompletionAdditionalEdit*> ordered;
  ordered.reserve(edits.size());
  for (const CompletionAdditionalEdit& edit : edits) {
    const bool above = !PositionBefore(insertion.start, edit.range.end);
    const bool below = !PositionBefore(edit.range.start, insertion.end);
    if (above || below) {
      ordered.push_back(&edit);
    }
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const CompletionAdditionalEdit* a, const CompletionAdditionalEdit* b) {
              return PositionBefore(b->range.start, a->range.start);
            });
  for (const CompletionAdditionalEdit* edit : ordered) {
    if (!viewport.ReplaceRange(edit->range, edit->new_text)) {
      continue;
    }
    if (!PositionBefore(insertion.start, edit->range.end)) {
      insertion.start = RemapAcrossEdit(insertion.start, *edit);
      insertion.end = RemapAcrossEdit(insertion.end, *edit);
    }
  }
  return insertion;
}

}  // namespace

bool AssistService::ResultIsStale(const editor::TextViewport* active_editable,
                                  const std::filesystem::path& request_path) {
  return active_editable == nullptr || active_editable->path() != request_path;
}

namespace {

// TD-2026-07-17-072: read one line zero-copy via TextBuffer::LineView instead of
// materializing the whole document with Snapshot() just to index a single line.
// Opening completion / attempting a snippet on a large file no longer pays an
// O(document) copy before any provider/LSP work.
std::string_view LineViewAt(const editor::TextViewport& viewport, std::size_t index) {
  return index < viewport.lines().size() ? viewport.lines().LineView(index) : std::string_view{};
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
  const std::string_view line = LineViewAt(viewport, viewport.cursor_line());
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

  const std::string language_id = viewport->language_id();
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
  session.error = "Loading\xE2\x80\xA6";
  operations_.show_overlay(OverlayMode::Completion);

  auto merge = std::make_shared<CompletionMerge>();
  merge->language_id = language_id;
  merge->generation = ++completion_request_generation_;

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
            LspResult<std::vector<LspClient::CompletionItem>> items) {
          operations_.finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          // finish_tracked above must run first so the in-flight counter is not
          // leaked when a stale result bails.
          if (ResultIsStale(operations_.active_editable_viewport(), request_path) ||
              merge->generation != completion_request_generation_) {
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
        if (ResultIsStale(operations_.active_editable_viewport(), request_path) ||
            merge->generation != completion_request_generation_) {
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
    const LspResult<std::vector<LspClient::CompletionItem>>& items,
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
    std::vector<CompletionAdditionalEdit> additional_edits;
    if (apply_viewport != nullptr) {
      if (item.replace_range.has_value()) {
        item_range = LspRangeToEditorRange(*apply_viewport, *item.replace_range, encoding);
      }
      additional_edits.reserve(item.additional_text_edits.size());
      for (const auto& [range, new_text] : item.additional_text_edits) {
        additional_edits.push_back(CompletionAdditionalEdit{
            .range = LspRangeToEditorRange(*apply_viewport, range, encoding),
            .new_text = new_text,
        });
      }
    }
    result.push_back(CompletionSessionItem{
        .label = item.label,
        .detail = item.detail,
        .documentation = item.documentation,
        .insert_text = item.insert_text,
        .is_snippet = snippets_on && item.insert_text_format == 2,
        .replacement_range = item_range,
        .additional_edits = std::move(additional_edits),
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
    session.error = session.items.empty() ? "Loading\xE2\x80\xA6" : std::string{};
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
    operations_.update_merge_tracking_after_viewport_edit(*merge_tab, snapshot.selection_before,
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
  editor::SelectionRange replacement_range =
      item.replacement_range.value_or(session.replacement_range);
  const EditSideEffectsSnapshot snapshot = CaptureEditSnapshot(*viewport);
  // The item's additionalTextEdits (an auto-import line, typically) are positioned
  // in the document BEFORE the completion, so they go in first, highest first,
  // and the insertion range is remapped across the ones above it. Doing them
  // first also keeps a snippet insertion last, so its session sees the final
  // document. One undo group covers the lot; a group nests inside the snippet
  // path's own, so undo stays one step either way.
  const bool has_additional_edits = !item.additional_edits.empty();
  if (has_additional_edits) {
    viewport->BeginUndoGroup();
    replacement_range = ApplyCompletionAdditionalEdits(*viewport, item.additional_edits,
                                                       replacement_range);
  }
  const bool want_snippet = EditorSnippetsSettingEnabled() && item.is_snippet;
  bool snippet_applied = false;
  const auto finish_group = [&] {
    if (has_additional_edits && viewport->UndoGroupActive()) {
      viewport->EndUndoGroup();
    }
  };
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
          finish_group();
          return has_additional_edits;  // the import line still landed
        }
      }
    } else if (!viewport->ReplaceRange(replacement_range, item.insert_text)) {
      finish_group();
      return has_additional_edits;
    }
  } else if (!viewport->ReplaceRange(replacement_range, item.insert_text)) {
    finish_group();
    return has_additional_edits;
  }
  finish_group();
  // A completion edit mutates the buffer; the fold model's content_revision fingerprint
  // alone does not force a rescan once a file is fully resolved (see the Undo/Redo path),
  // so mark it dirty here — like the sibling snippet/insert paths — otherwise a
  // same-line-count completion (a re-indenting textEdit, an item that inserts a bracket
  // while replacing a same-line range) leaves stale/phantom fold ranges.
  if (TabEntry::EditorTabState* tab = operations_.active_editor_tab(); tab != nullptr) {
    tab->folding_model->MarkDirty();
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
  const std::string language_id = viewport->language_id();
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
  const std::string language_id = viewport.language_id();
  const LanguageContract* contract = language_contract_->Find(language_id);
  if (contract == nullptr || contract->snippets.empty()) {
    return false;
  }
  const editor::SelectionRange range = CompletionReplacementRange(viewport);
  if (range.start.line != range.end.line || range.end.column <= range.start.column) {
    return false;
  }
  const std::string_view line = LineViewAt(viewport, range.start.line);
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

  const std::string language_id = viewport->language_id();
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
  session.error = "Loading\xE2\x80\xA6";
  operations_.show_overlay(OverlayMode::CodeActions);

  auto merge = std::make_shared<CodeActionMerge>();
  merge->language_id = language_id;
  merge->generation = ++code_action_request_generation_;

  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    operations_.ensure_lsp_document_open(*viewport, *client, language_id);
    const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
    std::vector<LspClient::Diagnostic> context_diagnostics;
    if (operations_.collect_lsp_context_diagnostics) {
      context_diagnostics =
          operations_.collect_lsp_context_diagnostics(*viewport, range, encoding);
    }
    operations_.begin_tracked_lsp_request();
    client->RequestCodeActionAsync(
        FileUriForPath(request_path),
        LspClient::Range{
            .start =
                ByteColumnToLspPosition(*viewport, range.start.line, range.start.column, encoding),
            .end = ByteColumnToLspPosition(*viewport, range.end.line, range.end.column, encoding),
        },
        std::move(context_diagnostics),
        [this, request_path, merge](LspResult<std::vector<LspClient::CodeAction>> actions) {
          operations_.finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          if (ResultIsStale(operations_.active_editable_viewport(), request_path) ||
              merge->generation != code_action_request_generation_) {
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
        if (ResultIsStale(operations_.active_editable_viewport(), request_path) ||
            merge->generation != code_action_request_generation_) {
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
    const LspResult<std::vector<LspClient::CodeAction>>& actions) {
  std::vector<CodeActionSessionItem> result;
  if (!actions.has_value()) {
    return result;
  }
  // Shared aggregate budget across ALL actions so a server returning many large
  // (but individually capped) inline WorkspaceEdits cannot force the overlay to hold
  // the sum of every action's edit payload before the user selects one. Past the
  // budget, an action's inline fix is not materialized (edits_truncated set).
  // TD-2026-07-17A-057.
  constexpr std::size_t kMaxAggregateEdits = 50000;
  constexpr std::size_t kMaxAggregateEditBytes = 16u * 1024 * 1024;  // 16 MiB
  std::size_t total_edits = 0;
  std::size_t total_edit_bytes = 0;
  result.reserve(actions->size());
  for (const auto& action : *actions) {
    std::vector<std::string> arguments;
    arguments.reserve(action.arguments.size());
    for (const auto& argument : action.arguments) {
      arguments.push_back(JsonValueToArgumentString(argument));
    }
    std::vector<CodeActionEdit> edits;
    std::vector<WorkspaceResourceOp> resource_ops;
    bool edits_truncated = false;
    if (action.has_edit) {
      // Flatten the action's file resource ops (e.g. rust-analyzer "extract
      // module" creates the target file). One undecodable op URI disables the
      // whole inline fix — applying the text edits without their ops would leave
      // the workspace inconsistent.
      if (std::optional<std::vector<WorkspaceResourceOp>> flattened =
              lsp_workspace_edit::FlattenResourceOps(action.edit.resource_ops);
          flattened.has_value()) {
        resource_ops = *std::move(flattened);
      } else {
        edits_truncated = true;
      }
      for (const auto& [uri, text_edits] : action.edit.changes) {
        if (edits_truncated) {
          break;
        }
        const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
        if (!path.has_value()) {
          // An undecodable / non-local / malformed URI must never fall back to
          // the empty path, which the apply layer interprets as "edit the active
          // buffer". Skip these edits rather than corrupt whatever file is focused.
          continue;
        }
        for (const auto& [lsp_range, new_text] : text_edits) {
          if (total_edits >= kMaxAggregateEdits ||
              total_edit_bytes + new_text.size() > kMaxAggregateEditBytes) {
            // Budget exhausted: drop this action's (partial) edits AND its
            // resource ops so the overlay never holds a half-applied fix, and
            // mark it disabled.
            edits.clear();
            resource_ops.clear();
            edits_truncated = true;
            break;
          }
          ++total_edits;
          total_edit_bytes += new_text.size();
          edits.push_back(CodeActionEdit{
              .path = *path,
              // Clamp negative positions to 0, matching the rename/formatting paths
              // (a malformed quick fix with line:-1 must not wrap to a huge size_t and
              // land at the end of the buffer).
              .range = editor::SelectionRange{
                  .start = editor::TextPosition{
                      static_cast<std::size_t>(std::max(0, lsp_range.start.line)),
                      static_cast<std::size_t>(std::max(0, lsp_range.start.character))},
                  .end = editor::TextPosition{
                      static_cast<std::size_t>(std::max(0, lsp_range.end.line)),
                      static_cast<std::size_t>(std::max(0, lsp_range.end.character))},
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
        .resource_ops = std::move(resource_ops),
        .edits_truncated = edits_truncated,
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
    session.error = session.items.empty() ? "Loading\xE2\x80\xA6" : std::string{};
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
  // the command registry. An action with neither is inert. File resource ops
  // (create/rename/delete) run FIRST — the edits are keyed by the post-op paths
  // (e.g. rust-analyzer "extract module" creates the file its edits then fill).
  if (!action.resource_ops.empty()) {
    // Ops-carrying fix: the full applier runs the ops (rollback-safe), then the
    // edits against open buffers AND closed files on disk (the edits typically
    // fill the file the ops just created).
    const bool applied = operations_.apply_full_lsp_workspace_edit
                             ? operations_.apply_full_lsp_workspace_edit(action.edits,
                                                                         action.resource_ops)
                             : false;
    if (applied) {
      operations_.dismiss_overlay(true);
    } else {
      session.error = "Could not apply fix";
      operations_.request_overlay_redraw();
    }
    return applied;
  }
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
  const std::string language_id = viewport->language_id();
  const std::filesystem::path request_path = viewport->path();
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();

  auto merge = std::make_shared<NavigationMerge>();
  merge->generation = ++navigation_request_generation_;

  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    operations_.ensure_lsp_document_open(*viewport, *client, language_id);
    operations_.begin_tracked_lsp_request();
    merge->lsp_encoding = LspEncodingForClient(*client);
    client->RequestGoToDefinitionAsync(
        FileUriForPath(request_path),
        ByteColumnToLspPosition(*viewport, request_line, request_column, merge->lsp_encoding),
        [this, request_path, merge](LspResult<std::vector<LspClient::Location>> locations) {
          operations_.finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          merge->sources.lsp_failed = !locations.answered();
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
  if (merge->generation != navigation_request_generation_ ||
      ResultIsStale(operations_.active_editable_viewport(), request_path)) {
    // The buffer switched, or a newer navigation superseded this one, since the
    // request; abandon without navigating a different file/caret or emitting a
    // spurious "no definition" message.
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
      // Both sources ended up empty. If the language server never actually answered
      // (timeout / gone / protocol error), don't claim "No definition found" for a
      // request it never resolved.
      output_channels_->AppendLine(
          "lsp.definition", "LSP Definition",
          merge->sources.lsp_failed ? "Language server did not respond (no definition resolved)"
                                    : "No definition found");
      return;
    case assist_merge::NavChoice::UsePlugin:
      merge->acted = true;
      NavigateToPluginLocation(merge->plugin_locations.front());
      return;
    case assist_merge::NavChoice::UseLsp:
      merge->acted = true;
      NavigateToLspLocation(merge->lsp_locations.front(), merge->lsp_encoding);
      return;
  }
}

void AssistService::NavigateToLspLocation(const LspClient::Location& location,
                                          lsp_encoding::PositionEncoding encoding) {
  const std::optional<std::filesystem::path> path = PathFromFileUri(location.uri);
  if (!path.has_value() || !operations_.open_file_in_new_tab(*path)) {
    return;
  }
  if (editor::TextViewport* active = operations_.active_editor_viewport(); active != nullptr) {
    const std::size_t target_line =
        static_cast<std::size_t>(std::max(location.range.start.line, 0));
    active->JumpCursorTo(
        target_line,
        LspPositionToByteColumn(*active, target_line, location.range.start.character, encoding));
    operations_.reset_caret_blink();
    operations_.request_focused_editor_redraw();
  }
}

bool AssistService::GoToLspNavigation(LspNavigationKind kind, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  // typeDefinition / implementation / declaration are LSP-only (no plugin provider
  // counterpart), so this is a single-source navigation: fire the server request and
  // jump to the first location. PrepareLspRequest records the unavailable message.
  LspClient* client = PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }
  const std::filesystem::path request_path = viewport->path();
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  const LspClient::Position position = ByteColumnToLspPosition(
      *viewport, viewport->cursor_line(), viewport->cursor_column(), encoding);
  const char* empty_message = kind == LspNavigationKind::TypeDefinition ? "No type definition found"
                              : kind == LspNavigationKind::Implementation ? "No implementation found"
                                                                          : "No declaration found";
  const std::uint64_t request_generation = ++navigation_request_generation_;
  auto on_result = [this, request_path, encoding, empty_message, request_generation](
                       LspResult<std::vector<LspClient::Location>> locations) {
    operations_.finish_tracked_lsp_request();
    if (request_generation != navigation_request_generation_ ||
        ResultIsStale(operations_.active_editable_viewport(), request_path)) {
      return;
    }
    if (!locations.answered()) {
      output_channels_->AppendLine("lsp.definition", "LSP Definition",
                                   "Language server did not respond");
      return;
    }
    if (!locations.has_value() || locations->empty()) {
      output_channels_->AppendLine("lsp.definition", "LSP Definition", empty_message);
      return;
    }
    NavigateToLspLocation(locations->front(), encoding);
  };
  const std::string uri = FileUriForPath(request_path);
  switch (kind) {
    case LspNavigationKind::TypeDefinition:
      client->RequestGoToTypeDefinitionAsync(uri, position, std::move(on_result));
      break;
    case LspNavigationKind::Implementation:
      client->RequestGoToImplementationAsync(uri, position, std::move(on_result));
      break;
    case LspNavigationKind::Declaration:
      client->RequestGoToDeclarationAsync(uri, position, std::move(on_result));
      break;
  }
  return true;
}

bool AssistService::GoToLspTypeDefinition(std::string* error_message) {
  return GoToLspNavigation(LspNavigationKind::TypeDefinition, error_message);
}

bool AssistService::GoToLspImplementation(std::string* error_message) {
  return GoToLspNavigation(LspNavigationKind::Implementation, error_message);
}

bool AssistService::GoToLspDeclaration(std::string* error_message) {
  return GoToLspNavigation(LspNavigationKind::Declaration, error_message);
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
  const std::string language_id = viewport->language_id();
  const std::filesystem::path request_path = viewport->path();
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();

  auto merge = std::make_shared<NavigationMerge>();
  merge->generation = ++references_request_generation_;

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
        [this, request_path, merge](LspResult<std::vector<LspClient::Location>> locations) {
          operations_.finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          merge->sources.lsp_failed = !locations.answered();
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
  if (merge->generation != references_request_generation_ ||
      ResultIsStale(operations_.active_editable_viewport(), request_path)) {
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
    // Distinguish an authoritative empty result from an LSP that never answered.
    output_channels_->AppendLine(
        "lsp.references", "LSP References",
        merge->sources.lsp_failed ? "Language server did not respond (no references resolved)"
                                  : "No references found");
    return;
  }
  std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
  for (std::size_t index = 0; index < merged.size(); ++index) {
    EmitReferenceEntry("lsp.references", "LSP References", merged[index].path, merged[index].line,
                       merged[index].column, index + 1 < merged.size(), file_line_cache);
  }
}

bool AssistService::ShowCallHierarchy(bool incoming, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  LspClient* client = PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }

  static constexpr const char* kChannelId = "lsp.callHierarchy";
  const char* const title = incoming ? "Incoming Calls" : "Outgoing Calls";
  if (!client->SupportsCallHierarchy()) {
    // Say so instead of letting the short-circuited request read as "no callers":
    // a missing provider is a property of the server, not of the code.
    operations_.finish_tracked_lsp_request();
    output_channels_->Clear(kChannelId);
    output_channels_->AppendLine(kChannelId, title,
                                 "This language server does not provide call hierarchy");
    return true;
  }
  const std::uint64_t generation = ++call_hierarchy_request_generation_;
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();
  output_channels_->Clear(kChannelId);
  output_channels_->AppendLine(kChannelId, title, "Resolving call hierarchy…");

  // Both hops report through here so "the server never answered" and "the server
  // says there are none" stay distinguishable at every step of the chain.
  const auto finish_empty = [this, title](const char* message) {
    output_channels_->Clear(kChannelId);
    output_channels_->AppendLine(kChannelId, title, message);
  };

  client->RequestPrepareCallHierarchyAsync(
      FileUriForPath(viewport->path()),
      ByteColumnToLspPosition(*viewport, request_line, request_column,
                              LspEncodingForClient(*client)),
      [this, client, incoming, title, generation, finish_empty](
          LspResult<std::vector<LspClient::CallHierarchyItem>> items) {
        if (generation != call_hierarchy_request_generation_) {
          operations_.finish_tracked_lsp_request();
          return;
        }
        if (!items.answered()) {
          operations_.finish_tracked_lsp_request();
          finish_empty("Language server did not respond");
          return;
        }
        if (!items.has_value() || items->empty()) {
          operations_.finish_tracked_lsp_request();
          finish_empty("No callable symbol at the cursor");
          return;
        }
        // prepareCallHierarchy may resolve to several items (overloads); the first
        // is the server's best match, which is what VS Code walks by default.
        const LspClient::CallHierarchyItem& root = items->front();
        std::string root_label = root.name;
        if (!root.detail.empty()) {
          root_label += "  ·  " + root.detail;
        }
        const auto render = [this, title, root_label, incoming, generation, finish_empty](
                                LspResult<std::vector<LspClient::CallHierarchyCall>> calls) {
          operations_.finish_tracked_lsp_request();
          if (generation != call_hierarchy_request_generation_) {
            return;
          }
          if (!calls.answered()) {
            finish_empty("Language server did not respond");
            return;
          }
          if (!calls.has_value() || calls->empty()) {
            finish_empty(incoming ? "No callers found" : "No calls found");
            return;
          }
          output_channels_->Clear(kChannelId);
          output_channels_->AppendLine(
              kChannelId, title,
              (incoming ? "Callers of " : "Called by ") + root_label);
          std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
          for (std::size_t i = 0; i < calls->size(); ++i) {
            const LspClient::CallHierarchyCall& call = (*calls)[i];
            const std::optional<std::filesystem::path> path = PathFromFileUri(call.item.uri);
            if (!path.has_value()) {
              continue;
            }
            std::string header = call.item.name;
            if (!call.item.detail.empty()) {
              header += "  ·  " + call.item.detail;
            }
            output_channels_->AppendLine(kChannelId, title, header);
            const bool last = i + 1 >= calls->size();
            if (incoming && !call.call_ranges.empty()) {
              // Incoming: the interesting positions are the call sites inside the
              // caller, so list each one rather than the caller's declaration.
              for (std::size_t r = 0; r < call.call_ranges.size(); ++r) {
                const LspClient::Range& range = call.call_ranges[r];
                EmitReferenceEntry(
                    kChannelId, title, *path,
                    static_cast<std::size_t>(std::max(range.start.line, 0)) + 1,
                    static_cast<std::size_t>(std::max(range.start.character, 0)) + 1,
                    last && r + 1 >= call.call_ranges.size(), file_line_cache);
              }
              continue;
            }
            // Outgoing (and callers whose call sites the server omitted): navigate
            // to the symbol's own name.
            EmitReferenceEntry(
                kChannelId, title, *path,
                static_cast<std::size_t>(std::max(call.item.selection_range.start.line, 0)) + 1,
                static_cast<std::size_t>(
                    std::max(call.item.selection_range.start.character, 0)) + 1,
                !last, file_line_cache);
          }
        };
        if (incoming) {
          client->RequestIncomingCallsAsync(root.raw, render);
        } else {
          client->RequestOutgoingCallsAsync(root.raw, render);
        }
      });
  return true;
}

bool AssistService::ShowWorkspaceSymbols(const std::string& query, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  // workspace/symbol is project-wide; query the active buffer's server (the project
  // it belongs to). PrepareLspRequest resolves the client + begins the tracked req.
  LspClient* client = PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }
  // Ensure the channel exists and show progress synchronously so the host can
  // surface it immediately; the response rebuilds it.
  const std::uint64_t request_generation = ++workspace_symbol_request_generation_;
  output_channels_->Clear("lsp.workspaceSymbols");
  output_channels_->AppendLine("lsp.workspaceSymbols", "Workspace Symbols",
                               "Searching for \"" + query + "\"…");
  client->RequestWorkspaceSymbolAsync(
      query,
      [this, query, request_generation](
          LspResult<std::vector<LspClient::WorkspaceSymbol>> symbols) {
        operations_.finish_tracked_lsp_request();
        // Drop a superseded query's response so it can't clear the channel and render
        // stale results over the newer query (TD-2026-07-17A-034).
        if (request_generation != workspace_symbol_request_generation_) {
          return;
        }
        output_channels_->Clear("lsp.workspaceSymbols");
        if (!symbols.answered()) {
          output_channels_->AppendLine("lsp.workspaceSymbols", "Workspace Symbols",
                                       "Language server did not respond");
          return;
        }
        if (!symbols.has_value() || symbols->empty()) {
          output_channels_->AppendLine("lsp.workspaceSymbols", "Workspace Symbols",
                                       "No symbols matching \"" + query + "\"");
          return;
        }
        // Render each symbol as a name/container header plus a navigable
        // file:line:col entry with context (reusing the references formatter).
        std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
        for (std::size_t i = 0; i < symbols->size(); ++i) {
          const LspClient::WorkspaceSymbol& symbol = (*symbols)[i];
          const std::optional<std::filesystem::path> path = PathFromFileUri(symbol.location.uri);
          if (!path.has_value()) {
            continue;
          }
          std::string header = symbol.name;
          if (!symbol.container_name.empty()) {
            header += "  ·  " + symbol.container_name;
          }
          output_channels_->AppendLine("lsp.workspaceSymbols", "Workspace Symbols", header);
          EmitReferenceEntry(
              "lsp.workspaceSymbols", "Workspace Symbols", *path,
              static_cast<std::size_t>(std::max(symbol.location.range.start.line, 0)) + 1,
              static_cast<std::size_t>(std::max(symbol.location.range.start.character, 0)) + 1,
              i + 1 < symbols->size(), file_line_cache);
        }
      });
  return true;
}

std::string AssistService::SymbolAtCursor() const {
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr) {
    return {};
  }
  const std::string_view line = LspLineView(*viewport, viewport->cursor_line());
  std::size_t begin = std::min(viewport->cursor_column(), line.size());
  std::size_t end = begin;
  while (begin > 0 && editor::IsIdentifierByte(line[begin - 1])) {
    --begin;
  }
  while (end < line.size() && editor::IsIdentifierByte(line[end])) {
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
      // `client` cannot dangle: the callback is stored on the client and invoked
      // from its own DrainCallbacks (live or retiring), so the client outlives it.
      [this, request_path, new_name, client](LspResult<LspClient::WorkspaceEdit> edit) {
        operations_.finish_tracked_lsp_request();
        if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
          return;
        }
        if (!edit.answered()) {
          output_channels_->AppendLine("lsp.log", "LSP Log",
                                       "Rename request did not complete (server timed out or errored)");
          return;
        }
        if (!edit.has_value() || (edit->changes.empty() && edit->resource_ops.empty())) {
          output_channels_->AppendLine("lsp.log", "LSP Log", "No rename edits returned");
          return;
        }
        // Version gate: a versioned TextDocumentEdit pinned to a document version
        // we have since advanced past is stale — applying it would corrupt the
        // buffer the user kept typing in. LSP requires failing the whole edit.
        if (!lsp_workspace_edit::VersionsCurrent(*client, *edit)) {
          output_channels_->AppendLine(
              "lsp.log", "LSP Log",
              "Rename aborted: the document changed while the server computed the rename");
          return;
        }
        // Flatten resource ops (file create/rename/delete — e.g. rust-analyzer
        // renames the file when a module is renamed). An undecodable target URI
        // refuses the whole rename, mirroring the text-edit rule below.
        std::optional<std::vector<WorkspaceResourceOp>> flattened_ops =
            lsp_workspace_edit::FlattenResourceOps(edit->resource_ops);
        if (!flattened_ops.has_value()) {
          output_channels_->AppendLine(
              "lsp.log", "LSP Log",
              "Rename aborted: server returned an unusable resource-operation URI");
          return;
        }
        std::vector<WorkspaceResourceOp> resource_ops = *std::move(flattened_ops);
        // Flatten the URI-keyed WorkspaceEdit into the shared edit records (0-based
        // LSP coordinates; the apply path maps columns through the position
        // encoding). The host decides whether to apply in place or open + save the
        // files that are not currently open.
        std::vector<CodeActionEdit> workspace_edits;
        for (const auto& [uri, text_edits] : edit->changes) {
          const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
          if (!path.has_value()) {
            // A rename that touches a target whose URI cannot be decoded (non-local
            // authority, malformed percent escape, unsupported scheme) must not be
            // silently applied as a partial rename — and must never fall back to the
            // empty "active buffer" path. Refuse the whole rename so the user is not
            // left with a half-renamed symbol.
            output_channels_->AppendLine(
                "lsp.log", "LSP Log", "Rename aborted: server returned an unusable edit target URI");
            return;
          }
          for (const auto& [range, new_text] : text_edits) {
            workspace_edits.push_back(CodeActionEdit{
                .path = *path,
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
          operations_.apply_rename_workspace_edit(new_name, workspace_edits, resource_ops);
        } else if (!resource_ops.empty() && operations_.apply_full_lsp_workspace_edit) {
          // Headless fallback: resource ops first (the edits are keyed by the
          // post-rename paths), then the text edits.
          operations_.apply_full_lsp_workspace_edit(workspace_edits, resource_ops);
        } else if (operations_.apply_lsp_workspace_edit) {
          operations_.apply_lsp_workspace_edit(workspace_edits);
        }
      });
  return true;
}

void AssistService::PrepareRenameForCursor(std::function<void(bool, std::string)> callback) {
  if (!callback) {
    return;
  }
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;  // no callback: the caller keeps its heuristic seed
  }
  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  if (client == nullptr || !client->SupportsPrepareRename()) {
    return;  // no server / no prepareRename provider — keep the heuristic seed
  }
  const std::filesystem::path request_path = viewport->path();
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  const LspClient::Position position = ByteColumnToLspPosition(
      *viewport, viewport->cursor_line(), viewport->cursor_column(), encoding);
  std::string language_id = viewport->language_id();
  operations_.ensure_lsp_document_open(*viewport, *client, language_id);
  client->RequestPrepareRenameAsync(
      FileUriForPath(request_path), position,
      [this, request_path, callback = std::move(callback)](
          LspResult<LspClient::PrepareRename> result) {
        // Only meaningful while the same buffer is active; a switch abandons refine.
        if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
          return;
        }
        if (!result.has_value()) {
          return;  // no provider / timeout / error — the caller keeps its heuristic seed
        }
        callback(result->can_rename, result->placeholder);
      });
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
  const auto on_result = [this, request_path](
                             LspResult<std::vector<LspClient::TextEdit>> edits) {
    ApplyFormattingResult(request_path, std::move(edits));
  };
  // Format the SELECTION only when there is one (VSCode "Format Selection"), else the
  // whole document. Both return the same TextEdit[] shape and apply identically.
  if (viewport->has_selection()) {
    if (const std::optional<editor::SelectionRange> selection = viewport->selection_range()) {
      const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
      const editor::SelectionRange normalized = editor::TextViewport::NormalizeRange(*selection);
      const LspClient::Range range{
          .start = ByteColumnToLspPosition(*viewport, normalized.start.line,
                                           normalized.start.column, encoding),
          .end = ByteColumnToLspPosition(*viewport, normalized.end.line, normalized.end.column,
                                         encoding),
      };
      client->RequestRangeFormattingAsync(FileUriForPath(request_path), range, tab_size,
                                          /*insert_spaces=*/true, on_result);
      return true;
    }
  }
  client->RequestFormattingAsync(FileUriForPath(request_path), tab_size, /*insert_spaces=*/true,
                                 on_result);
  return true;
}

void AssistService::ApplyFormattingResult(const std::filesystem::path& request_path,
                                          LspResult<std::vector<LspClient::TextEdit>> edits) {
  operations_.finish_tracked_lsp_request();
  // Drop superseded results (buffer switched/closed) before mutating.
  if (ResultIsStale(operations_.active_editable_viewport(), request_path)) {
    return;
  }
  if (!edits.answered()) {
    output_channels_->AppendLine("lsp.log", "LSP Log",
                                 "Formatting request did not complete (server timed out or errored)");
    return;
  }
  if (!edits.has_value() || edits->empty()) {
    output_channels_->AppendLine("lsp.log", "LSP Log", "No formatting changes");
    return;
  }
  // Convert to the shared workspace-edit records (0-based LSP coordinates; the apply
  // path maps their columns through the server's position encoding) and apply the
  // whole set together, highest-position-first.
  std::vector<CodeActionEdit> workspace_edits;
  workspace_edits.reserve(edits->size());
  for (const auto& [range, new_text] : *edits) {
    workspace_edits.push_back(CodeActionEdit{
        .path = request_path,
        .range = editor::SelectionRange{
            .start = editor::TextPosition{static_cast<std::size_t>(std::max(0, range.start.line)),
                                          static_cast<std::size_t>(std::max(0, range.start.character))},
            .end = editor::TextPosition{static_cast<std::size_t>(std::max(0, range.end.line)),
                                        static_cast<std::size_t>(std::max(0, range.end.character))},
        },
        .new_text = new_text,
    });
  }
  if (operations_.apply_lsp_workspace_edit &&
      !operations_.apply_lsp_workspace_edit(workspace_edits)) {
    output_channels_->AppendLine("lsp.log", "LSP Log", "Could not apply formatting");
  }
}

namespace {

// A reference/workspace-symbol result shows a 3-line snippet (the hit line +/- 1).
// Files at or below this size are read+split once and cached so many references
// into the same file amortize to one read; larger files use the bounded
// line-window reader instead so a hit inside a huge generated file never
// materializes (or retains) the whole file on the shell thread. 256 KiB covers
// essentially every hand-written source file.
constexpr std::uintmax_t kReferenceSnippetWholeReadBytes = 256ull * 1024;

// Byte budget for the bounded line-window reader used on large files: it streams
// at most this many bytes looking for the snippet's last line, then gives up
// (showing just the file:line:col header). Bounds the shell-thread scan for a
// deep hit without ever slurping a multi-hundred-MB blob.
constexpr std::uintmax_t kReferenceSnippetScanBytes = 2ull * 1024 * 1024;

// Lower a signature-help result to the display (signature, documentation) pair the
// caret-anchored popup shows: the active overload's label plus a block that leads
// with the active parameter (so the user sees which argument they are typing) then
// the signature documentation. Returns nullopt when there is nothing to show. The
// plugin and LSP variants share this exact shape so either source renders identically.
std::optional<std::pair<std::string, std::string>> LowerPluginSignature(
    const plugin::PluginHost::SignatureHelpResult& result) {
  if (result.signatures.empty()) {
    return std::nullopt;
  }
  const std::size_t active =
      result.active_signature >= 0 &&
              static_cast<std::size_t>(result.active_signature) < result.signatures.size()
          ? static_cast<std::size_t>(result.active_signature)
          : 0;
  const plugin::PluginHost::SignatureInfo& signature = result.signatures[active];
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
  return std::make_pair(signature.label, std::move(documentation));
}

std::optional<std::pair<std::string, std::string>> LowerLspSignature(
    const LspClient::SignatureHelp& help) {
  if (help.signatures.empty()) {
    return std::nullopt;
  }
  const std::size_t active =
      help.active_signature >= 0 &&
              static_cast<std::size_t>(help.active_signature) < help.signatures.size()
          ? static_cast<std::size_t>(help.active_signature)
          : 0;
  const LspClient::SignatureInformation& signature = help.signatures[active];
  // A per-signature activeParameter (LSP 3.16+) overrides the top-level one.
  const int active_parameter =
      signature.active_parameter >= 0 ? signature.active_parameter : help.active_parameter;
  std::string documentation;
  if (active_parameter >= 0 &&
      static_cast<std::size_t>(active_parameter) < signature.parameters.size()) {
    const LspClient::SignatureParameter& parameter =
        signature.parameters[static_cast<std::size_t>(active_parameter)];
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
  return std::make_pair(signature.label, std::move(documentation));
}

}  // namespace

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

  // Query the plugin provider and the language server CONCURRENTLY (mirrors
  // completion / definition). The server is authoritative for its language: the
  // popup waits for its answer and uses the plugin result only if the server
  // comes back empty. The lookups run on the worker / I/O thread so neither
  // blocks the UI; the popup is chosen and shown from the mailbox drain.
  const std::string language_id = viewport->language_id();
  const std::filesystem::path request_path = viewport->path();
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();

  auto merge = std::make_shared<SignatureHelpMerge>();
  merge->generation = ++signature_request_generation_;

  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    operations_.ensure_lsp_document_open(*viewport, *client, language_id);
    operations_.begin_tracked_lsp_request();
    const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
    client->RequestSignatureHelpAsync(
        FileUriForPath(request_path),
        ByteColumnToLspPosition(*viewport, request_line, request_column, encoding),
        [this, request_path, merge](LspResult<LspClient::SignatureHelp> help) {
          operations_.finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          if (help.has_value()) {
            if (auto lowered = LowerLspSignature(*help)) {
              merge->lsp_has = true;
              merge->lsp_signature = std::move(lowered->first);
              merge->lsp_documentation = std::move(lowered->second);
            }
          }
          ResolveSignatureHelp(merge, request_path);
        });
  } else {
    merge->sources.lsp_pending = false;
  }

  plugin_runtime_->Host().QuerySignatureHelpAsync(
      language_id, request_path, request_line + 1, request_column + 1,
      [this, request_path, merge](bool resolved, plugin::PluginHost::SignatureHelpResult result,
                                  std::string /*provider_error*/) {
        merge->sources.plugin_pending = false;
        if (resolved) {
          if (auto lowered = LowerPluginSignature(result)) {
            merge->plugin_has = true;
            merge->plugin_signature = std::move(lowered->first);
            merge->plugin_documentation = std::move(lowered->second);
          }
        }
        ResolveSignatureHelp(merge, request_path);
      });
  return true;
}

void AssistService::ResolveSignatureHelp(const std::shared_ptr<SignatureHelpMerge>& merge,
                                         const std::filesystem::path& request_path) {
  if (merge->acted) {
    return;
  }
  if (merge->generation != signature_request_generation_ ||
      ResultIsStale(operations_.active_editable_viewport(), request_path)) {
    return;  // buffer switched or a newer signature request superseded this one
  }
  const assist_merge::NavChoice choice = assist_merge::ChooseNavigation(
      merge->sources.lsp_authoritative, merge->sources.lsp_pending, merge->lsp_has,
      merge->sources.plugin_pending, merge->plugin_has);
  switch (choice) {
    case assist_merge::NavChoice::Pending:
      return;
    case assist_merge::NavChoice::None:
      // Nothing to show. Leave any existing popup untouched (the host lifecycle
      // dismisses it on Escape / edits), matching the prior plugin-only behavior.
      merge->acted = true;
      return;
    case assist_merge::NavChoice::UsePlugin:
      merge->acted = true;
      if (operations_.show_signature_help) {
        operations_.show_signature_help(merge->plugin_signature, merge->plugin_documentation);
      }
      return;
    case assist_merge::NavChoice::UseLsp:
      merge->acted = true;
      if (operations_.show_signature_help) {
        operations_.show_signature_help(merge->lsp_signature, merge->lsp_documentation);
      }
      return;
  }
}

void AssistService::NavigateToPluginLocation(const plugin::PluginHost::LocationResult& location) {
  if (location.path.empty() || !operations_.open_file_in_new_tab(location.path)) {
    return;
  }
  if (editor::TextViewport* active = operations_.active_editor_viewport(); active != nullptr) {
    // Provider line/column are 1-based; the viewport caret is 0-based.
    active->JumpCursorTo(location.line > 0 ? location.line - 1 : 0,
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

  const std::size_t target_line = line > 0 ? line : 1;
  const std::size_t first_line = target_line > 1 ? target_line - 1 : 1;
  const std::size_t last_line = target_line + 1;

  const auto append_snippet_line = [&](std::size_t line_number, std::string_view text) {
    output_channels_->AppendLine(
        channel_id, channel_title,
        std::string(line_number == target_line ? " > " : "   ") + std::to_string(line_number) +
            " | " + std::string(text));
  };

  // 1. When the reference lands in the file that is already open as the active
  //    editor buffer, read the snippet straight from the live document via a
  //    zero-copy LineSpan — no file I/O and no whole-file line vector.
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const editor::TextViewport* viewport = operations_.active_editable_viewport();
      viewport != nullptr && util::SameAsNormalizedPath(viewport->path(), normalized_path)) {
    const editor::LineSpan lines = viewport->lines();
    for (std::size_t line_number = first_line;
         line_number <= last_line && line_number <= lines.size(); ++line_number) {
      append_snippet_line(line_number, lines[line_number - 1]);
    }
    if (append_separator) {
      output_channels_->AppendLine(channel_id, channel_title, "");
    }
    return;
  }

  // 2. On disk. Small files are read+split once and cached, so many references
  //    into the same file share one read; large files use a bounded line-window
  //    reader that streams only up to the snippet's last line and never
  //    materializes (or caches) the whole file. This bounds both the shell-thread
  //    time and the retained memory for a reference set that spans huge generated
  //    files.
  const std::vector<std::string>* file_lines = nullptr;
  std::vector<std::string> window;  // owns the bounded-window fallback lines
  const auto cache_it = file_line_cache.find(path);
  if (cache_it != file_line_cache.end()) {
    file_lines = &cache_it->second;
  } else {
    const util::FileSignature signature = util::StatFileSignature(path);
    if (signature.exists && !signature.error &&
        signature.size <= kReferenceSnippetWholeReadBytes) {
      if (const auto text = util::ReadTextFile(path); text.has_value()) {
        file_lines = &file_line_cache.emplace(path, util::SplitLines(*text)).first->second;
      }
    } else {
      window = util::ReadFileLineWindow(path, first_line, last_line,
                                        kReferenceSnippetScanBytes);
    }
  }

  if (file_lines != nullptr) {
    if (file_lines->empty()) {
      return;
    }
    for (std::size_t line_number = first_line;
         line_number <= last_line && line_number <= file_lines->size(); ++line_number) {
      append_snippet_line(line_number, (*file_lines)[line_number - 1]);
    }
  } else {
    if (window.empty()) {
      return;
    }
    for (std::size_t i = 0; i < window.size(); ++i) {
      append_snippet_line(first_line + i, window[i]);
    }
  }
  if (append_separator) {
    output_channels_->AppendLine(channel_id, channel_title, "");
  }
}

}  // namespace microide::workspace
