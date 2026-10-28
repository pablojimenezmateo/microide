#include "workspace/assist/CompletionAssist.h"

#include <algorithm>
#include <utility>

#include "editor/CompletionFilter.h"
#include "editor/EditTypes.h"
#include "editor/SnippetEngine.h"
#include "util/StringUtil.h"
#include "workspace/FileUri.h"
#include "workspace/SettingFlags.h"
#include "workspace/lsp/LspFeatureFlags.h"
#include "workspace/lsp/LspViewportPositions.h"

namespace microide::workspace {

namespace {

using editor::PositionBefore;

constexpr const char* kLoading = "Loading\xE2\x80\xA6";

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

bool CompletionListUp(const OverlayState& overlay) {
  return overlay.visible && overlay.mode == OverlayMode::Completion;
}

}  // namespace

CompletionSessionState& CompletionAssist::ResetSession(const editor::TextViewport& viewport,
                                                       bool automatic, const char* source) {
  auto& session = host_.context().current_project_state.overlay.workflow.completion;
  session.items.clear();
  session.visible.clear();
  session.selected_index = 0;
  session.replacement_range = assist::CompletionReplacementRange(viewport);
  session.request_column = viewport.cursor_column();
  session.is_incomplete = false;
  session.automatic = automatic;
  session.source = source;
  session.error.clear();
  return session;
}

bool CompletionAssist::ShowCompletionOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = host_.ops().active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  // Open the overlay in a loading state immediately, then query the plugin worker
  // and the language server CONCURRENTLY (never serially). Neither blocks the UI;
  // each fills the overlay on its own mailbox drain via PublishMerge, which ranks
  // LSP-first for served languages and de-dupes overlapping labels.
  ResetSession(*viewport, /*automatic=*/false, "lsp").error = kLoading;
  host_.ops().show_overlay(OverlayMode::Completion);
  RequestCompletions(*viewport, /*refresh=*/false);
  return true;
}

void CompletionAssist::StartAutomaticCompletion(editor::TextViewport& viewport,
                                                char trigger_character) {
  auto_anchor_ = ResetSession(viewport, /*automatic=*/true, "lsp").replacement_range.start;
  RequestCompletions(viewport, /*refresh=*/false,
                     trigger_character != '\0' ? LspClient::CompletionTrigger::kTriggerCharacter
                                                : LspClient::CompletionTrigger::kInvoked,
                     trigger_character);
}

void CompletionAssist::AfterTyping(std::string_view typed) {
  const OverlayState& overlay = host_.context().current_project_state.overlay;
  const bool list_up = CompletionListUp(overlay);
  if (overlay.visible && !list_up) {
    return;  // another overlay owns the screen
  }
  const AssistHost::Operations& ops = host_.ops();
  editor::TextViewport* viewport = ops.active_editable_viewport();
  if (typed.size() != 1 || viewport == nullptr || viewport->path().empty() ||
      viewport->has_multiple_carets()) {
    if (list_up) {
      FollowCompletionCaret();
    }
    return;
  }
  // Nothing serves completions for this language (no server, no plugin provider):
  // nothing below can trigger or suggest. Asked first because this runs on every
  // keystroke, and the questions below (feature settings, the server, the word at
  // the caret) cost a keystroke in a split ~25% over v2.12.0 for files nothing
  // completes (editor_split_grid_workout's typing phase).
  if (ops.has_completion_provider && !ops.has_completion_provider(viewport->language_id())) {
    if (list_up) {
      FollowCompletionCaret();
    }
    return;
  }
  const char c = typed.front();
  // A trigger character -- a server's, or a plugin provider's -- starts a new list
  // at the caret, replacing any list the character just ended (`obj` + `.` ->
  // members of obj).
  bool server_trigger = false;
  if (LspFeatureEnabled(ops.get_setting_value, "lsp.completion.enabled")) {
    LspClient* client = ops.lsp_client_for_viewport(*viewport, nullptr);
    server_trigger = client != nullptr && client->IsCompletionTriggerCharacter(c);
  }
  if (server_trigger || (ops.plugin_completion_trigger_characters &&
                         ops.plugin_completion_trigger_characters(viewport->language_id())
                                 .find(c) != std::string::npos)) {
    if (list_up) {
      ops.dismiss_overlay(true);
    }
    StartAutomaticCompletion(*viewport, server_trigger ? c : '\0');
    return;
  }
  if (list_up) {
    FollowCompletionCaret();
    return;
  }
  // A word character (the whole-word byte class: ASCII alnum, `_`, any non-ASCII byte).
  if (!util::IsSearchWordByte(c) ||
      !SettingFlagEnabled(ops.get_setting_value("editor.quickSuggestions"),
                          /*default_value=*/true)) {
    return;
  }
  // Once per word: a word whose request is out (or came back empty) is not asked
  // about again on every character typed into it.
  const editor::SelectionRange word = assist::CompletionReplacementRange(*viewport);
  if (auto_anchor_.has_value() && auto_anchor_->line == word.start.line &&
      auto_anchor_->column == word.start.column) {
    return;
  }
  StartAutomaticCompletion(*viewport, '\0');
}

void CompletionAssist::RequestCompletions(editor::TextViewport& viewport, bool refresh,
                                          LspClient::CompletionTrigger trigger,
                                          char trigger_character) {
  const std::string language_id = viewport.language_id();
  const std::filesystem::path request_path = viewport.path();

  auto merge = std::make_shared<Merge>();
  merge->language_id = language_id;
  merge->generation = ++request_generation_;
  merge->request_column = viewport.cursor_column();

  // Language-server source. A present server is authoritative for its language.
  const AssistHost::Operations& ops = host_.ops();
  LspClient* client = ops.lsp_client_for_viewport(viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    ops.ensure_lsp_document_open(viewport, *client, language_id);
    ops.begin_tracked_lsp_request();
    const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
    const std::size_t request_line = viewport.cursor_line();
    client->RequestCompletionAsync(
        FileUriForPath(request_path),
        ByteColumnToLspPosition(viewport, request_line, viewport.cursor_column(), encoding),
        [this, request_path, encoding, merge](LspResult<LspClient::CompletionList> list) {
          host_.ops().finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          // finish_tracked above must run first so the in-flight counter is not
          // leaked when a stale result bails.
          if (host_.IsStale(request_path) || merge->generation != request_generation_) {
            return;
          }
          if (list.has_value()) {
            merge->lsp_items = TransformLspCompletions(list->items, encoding);
            merge->lsp_incomplete = list->is_incomplete;
          }
          PublishMerge(merge, request_path);
        },
        refresh ? LspClient::CompletionTrigger::kIncomplete : trigger,
        refresh ? '\0' : trigger_character);
  } else {
    merge->sources.lsp_pending = false;
  }

  // Plugin source, dispatched at the same time.
  host_.plugins().Host().QueryCompletionsAsync(
      language_id, request_path, viewport.cursor_line() + 1, viewport.cursor_column() + 1, {},
      [this, request_path, merge](std::vector<plugin::PluginHost::CompletionCandidate> items,
                                  std::string /*provider_error*/) {
        merge->sources.plugin_pending = false;
        if (host_.IsStale(request_path) || merge->generation != request_generation_) {
          return;
        }
        merge->plugin_items = TransformPluginCompletions(items);
        PublishMerge(merge, request_path);
      });
}

bool CompletionAssist::RefilterCompletion(CompletionSessionState& session,
                                          const editor::TextViewport& viewport,
                                          bool keep_selection) {
  const editor::TextPosition anchor = session.replacement_range.start;
  if (viewport.has_multiple_carets() || viewport.has_selection() ||
      viewport.cursor_line() != anchor.line) {
    return false;
  }
  const std::string_view line = assist::LineViewAt(viewport, anchor.line);
  const std::size_t caret = std::min(viewport.cursor_column(), line.size());
  // The anchor is where the word began when the list opened. The caret is still
  // in that word while the identifier run ending at the caret starts at or before
  // it: a typed space or `(` starts a new run after it, and a backspace past the
  // anchor puts the caret before it. (At or before, not at: the insert-snippet
  // list may anchor at the caret inside a word.)
  if (caret < anchor.column || util::Utf8IdentifierRunStart(line, caret) > anchor.column) {
    return false;
  }
  session.replacement_range.end = editor::TextPosition{anchor.line, caret};
  const std::optional<std::uint32_t> selected_item =
      keep_selection && session.selected_index < session.visible.size()
          ? std::optional<std::uint32_t>(session.visible[session.selected_index])
          : std::nullopt;
  const std::string_view word = line.substr(anchor.column, caret - anchor.column);
  editor::RankCompletionCandidates(
      session.items.size(), word,
      [&](std::size_t i) { return session.items[i].FilterText(); }, rank_scratch_,
      session.visible);
  session.selected_index = 0;
  if (selected_item.has_value()) {
    const auto it = std::find(session.visible.begin(), session.visible.end(), *selected_item);
    if (it != session.visible.end()) {
      session.selected_index = static_cast<std::size_t>(it - session.visible.begin());
    }
  }
  return true;
}

void CompletionAssist::FollowCompletionCaret() {
  if (!CompletionListUp(host_.context().current_project_state.overlay)) {
    return;
  }
  auto& session = host_.context().current_project_state.overlay.workflow.completion;
  editor::TextViewport* viewport = host_.ops().active_editable_viewport();
  const std::size_t previous_end = session.replacement_range.end.column;
  if (viewport == nullptr || !RefilterCompletion(session, *viewport)) {
    host_.ops().dismiss_overlay(true);
    return;
  }
  const bool word_changed = session.replacement_range.end.column != previous_end;
  if (word_changed && session.is_incomplete && session.source != "snippet") {
    // The server's list does not cover the new word: ask again, keeping the
    // narrowed old list on screen until the answer replaces it.
    RequestCompletions(*viewport, /*refresh=*/true);
  } else if (session.visible.empty() && session.error.empty()) {
    // Nothing the sources returned matches the word, and they were not still
    // loading: VS Code hides the widget rather than show an empty one.
    host_.ops().dismiss_overlay(true);
    return;
  }
  host_.ops().request_overlay_redraw();
}

std::vector<CompletionSessionItem> CompletionAssist::TransformPluginCompletions(
    const std::vector<plugin::PluginHost::CompletionCandidate>& items) const {
  const bool snippets_on = host_.SnippetsEnabled();
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

std::vector<CompletionSessionItem> CompletionAssist::TransformLspCompletions(
    const std::vector<LspClient::CompletionItem>& items,
    lsp_encoding::PositionEncoding encoding) const {
  std::vector<CompletionSessionItem> result;
  const bool snippets_on = host_.SnippetsEnabled();
  editor::TextViewport* apply_viewport = host_.ops().active_editable_viewport();
  result.reserve(items.size());
  for (const auto& item : items) {
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
        .filter_text = item.filter_text,
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

void CompletionAssist::PublishMerge(const std::shared_ptr<Merge>& merge,
                                    const std::filesystem::path& request_path) {
  if (host_.IsStale(request_path)) {
    return;
  }
  const AssistHost::Operations& ops = host_.ops();
  auto& session = host_.context().current_project_state.overlay.workflow.completion;
  const auto& primary = merge->sources.lsp_authoritative ? merge->lsp_items : merge->plugin_items;
  const auto& secondary =
      merge->sources.lsp_authoritative ? merge->plugin_items : merge->lsp_items;
  session.items = assist_merge::RankedUnion(
      primary, secondary, [](const CompletionSessionItem& item) { return item.label; });
  session.is_incomplete = merge->lsp_incomplete;
  session.request_column = merge->request_column;
  session.source = merge->lsp_items.empty() ? "plugin" : "lsp";
  const bool list_up = CompletionListUp(host_.context().current_project_state.overlay);
  // A list the user closed stays closed; only an automatic one opens itself.
  if (!list_up && !session.automatic) {
    return;
  }
  // The answer may arrive after more typing; it is filtered by the word as it is
  // NOW, which is the point of keeping the items rather than the rows.
  editor::TextViewport* viewport = ops.active_editable_viewport();
  if (viewport == nullptr || !RefilterCompletion(session, *viewport)) {
    if (list_up) {
      ops.dismiss_overlay(true);
    }
    return;
  }
  if (session.automatic) {
    // Never "Loading…" or "No completions available" for a list nobody asked
    // for: it opens when it has rows and otherwise stays out of the way.
    session.error.clear();
    if (session.visible.empty()) {
      if (list_up && !merge->sources.AnyPending()) {
        ops.dismiss_overlay(true);
      }
      return;
    }
    if (!list_up) {
      ops.show_overlay(OverlayMode::Completion);
    }
    ops.request_overlay_redraw();
    return;
  }
  if (merge->sources.AnyPending()) {
    session.error = session.visible.empty() ? kLoading : std::string{};
  } else if (session.visible.empty()) {
    session.error = "No completions available";
    host_.MaybeLogLspUnavailable(merge->language_id, merge->sources.lsp_authoritative);
  } else {
    session.error.clear();
  }
  ops.request_overlay_redraw();
}

bool CompletionAssist::ApplySelectedCompletion() {
  auto& session = host_.context().current_project_state.overlay.workflow.completion;
  if (session.items.empty()) {
    return false;
  }
  const AssistHost::Operations& ops = host_.ops();
  editor::TextViewport* viewport = ops.active_editable_viewport();
  if (viewport == nullptr) {
    return false;
  }

  // Re-filter by the word as it is now, keeping the highlighted row: a click or
  // Enter accepts what the user picked. The re-filter used to reset the
  // selection to the top, so every accept took the first row (QA flow
  // lsp-completion: clicking `empty()` inserted `emplace(...)`).
  if (!RefilterCompletion(session, *viewport, /*keep_selection=*/true) || session.visible.empty()) {
    return false;
  }
  const CompletionSessionItem& item =
      session.VisibleItem(std::min(session.selected_index, session.visible.size() - 1));
  // Prefer the item's server-supplied textEdit range; fall back to the session's
  // heuristic word range for plugin completions / items without a textEdit. A
  // server range ends where the caret was when the list was requested, so the
  // characters typed since (or erased) move its end with them -- otherwise
  // accepting after typing `ve` into a list requested at `v` would leave the `e`.
  editor::SelectionRange replacement_range = session.replacement_range;
  if (item.replacement_range.has_value()) {
    replacement_range = *item.replacement_range;
    if (replacement_range.end.line == session.replacement_range.end.line) {
      const std::size_t caret = session.replacement_range.end.column;
      const std::size_t shifted =
          caret >= session.request_column
              ? replacement_range.end.column + (caret - session.request_column)
              : replacement_range.end.column - std::min(replacement_range.end.column,
                                                        session.request_column - caret);
      replacement_range.end.column = std::max(shifted, replacement_range.start.column);
    }
  }
  const AssistHost::EditSnapshot snapshot = AssistHost::CaptureEditSnapshot(*viewport);
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
  const auto finish_group = [&] {
    if (has_additional_edits && viewport->UndoGroupActive()) {
      viewport->EndUndoGroup();
    }
  };
  // A snippet item expands into the active tab's session; without a tab, or when
  // the body does not parse, it is inserted as plain text.
  bool inserted = false;
  if (host_.SnippetsEnabled() && item.is_snippet) {
    if (TabEntry::EditorTabState* editor_tab = ops.active_editor_tab()) {
      viewport->BeginUndoGroup();
      inserted = editor::ExpandSnippetAtSelection(*viewport, editor_tab->snippet_session,
                                                  replacement_range, item.insert_text);
      if (!inserted && viewport->UndoGroupActive()) {
        viewport->EndUndoGroup();
      }
    }
  }
  if (!inserted && !viewport->ReplaceRange(replacement_range, item.insert_text)) {
    finish_group();
    return has_additional_edits;  // the import line still landed
  }
  finish_group();
  // A completion edit mutates the buffer; the fold model's content_revision fingerprint
  // alone does not force a rescan once a file is fully resolved (see the Undo/Redo path),
  // so mark it dirty here — like the sibling snippet/insert paths — otherwise a
  // same-line-count completion (a re-indenting textEdit, an item that inserts a bracket
  // while replacing a same-line range) leaves stale/phantom fold ranges.
  if (TabEntry::EditorTabState* tab = ops.active_editor_tab(); tab != nullptr) {
    tab->folding_model->MarkDirty();
  }
  host_.ApplyEditSideEffects(*viewport, snapshot);
  ops.dismiss_overlay(true);
  return true;
}

bool CompletionAssist::ShowInsertSnippetOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (!host_.SnippetsEnabled()) {
    if (error_message != nullptr) {
      *error_message = "Snippets are disabled";
    }
    return false;
  }
  editor::TextViewport* viewport = host_.RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  const LanguageContract* contract = host_.languages().Find(viewport->language_id());
  if (contract == nullptr || contract->snippets.empty()) {
    if (error_message != nullptr) {
      *error_message = "No snippets for this language";
    }
    return false;
  }
  CompletionSessionState& session = ResetSession(*viewport, /*automatic=*/false, "snippet");
  session.items.reserve(contract->snippets.size());
  for (const auto& sn : contract->snippets) {
    session.items.push_back(CompletionSessionItem{
        .label = sn.label.empty() ? sn.prefix : sn.label,
        .filter_text = sn.prefix,
        .detail = sn.prefix,
        .documentation = {},
        .insert_text = sn.body,
        .is_snippet = true,
    });
  }
  // The word at the caret narrows the snippets like any completion. When it
  // matches none, the caret is not on a snippet prefix: offer them all and insert
  // at the caret instead of replacing the word.
  if (!RefilterCompletion(session, *viewport) || session.visible.empty()) {
    session.replacement_range.start = session.replacement_range.end =
        editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
    (void)RefilterCompletion(session, *viewport);  // an empty word keeps every item
  }
  host_.ops().show_overlay(OverlayMode::Completion);
  return true;
}

}  // namespace microide::workspace
