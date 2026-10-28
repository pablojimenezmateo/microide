#include "workspace/assist/AssistHost.h"

#include <algorithm>
#include <string>
#include <utility>

#include "util/StringUtil.h"
#include "workspace/SettingFlags.h"

namespace microide::workspace {

namespace {

// "LSP startup failed for <id>[: <detail>]" when the language has a configured
// server that did not come up; empty when it has none.
std::string LspStartupFailure(const LspManager& manager, const std::string& language_id) {
  if (language_id.empty() || !manager.HasServer(language_id)) {
    return {};
  }
  const std::string detail = manager.LastServerError(language_id);
  return detail.empty() ? "LSP startup failed for " + language_id
                        : "LSP startup failed for " + language_id + ": " + detail;
}

}  // namespace

void AssistHost::Configure(WorkspaceContext& context,
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

bool AssistHost::IsStale(const std::filesystem::path& request_path) const {
  return assist::ResultIsStale(operations_.active_editable_viewport(), request_path);
}

bool AssistHost::SnippetsEnabled() const {
  return SettingFlagEnabled(operations_.get_setting_value("editor.snippets.enabled"),
                            /*default_value=*/true);
}

editor::TextViewport* AssistHost::RequireActiveEditableViewport(std::string* error_message) const {
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return nullptr;
  }
  return viewport;
}

LspClient* AssistHost::PrepareLspRequest(editor::TextViewport& viewport,
                                         std::string* error_message) const {
  std::string language_id;
  LspClient* client = operations_.lsp_client_for_viewport(viewport, &language_id);
  if (client == nullptr) {
    std::string failure = LspStartupFailure(operations_.current_lsp_manager(), language_id);
    if (failure.empty()) {
      failure = "No language server available";
    }
    output_channels_->AppendLine("lsp.log", "LSP Log", failure);
    if (error_message != nullptr) {
      *error_message = std::move(failure);
    }
    return nullptr;
  }
  operations_.ensure_lsp_document_open(viewport, *client, language_id);
  operations_.begin_tracked_lsp_request();
  return client;
}

void AssistHost::MaybeLogLspUnavailable(const std::string& language_id,
                                        bool lsp_authoritative) const {
  // A server served the buffer, or the language has none configured: not a
  // failure worth logging on every request.
  if (lsp_authoritative) {
    return;
  }
  const std::string failure = LspStartupFailure(operations_.current_lsp_manager(), language_id);
  if (!failure.empty()) {
    output_channels_->AppendLine("lsp.log", "LSP Log", failure);
  }
}

void AssistHost::Report(const char* channel_id, const char* channel_title,
                        const std::string& message) const {
  output_channels_->AppendLine(channel_id, channel_title, message);
  if (operations_.notify_info) {
    operations_.notify_info(message);
  }
}

AssistHost::EditSnapshot AssistHost::CaptureEditSnapshot(const editor::TextViewport& viewport) {
  return EditSnapshot{.was_dirty = viewport.dirty(), .cursor_before_line = viewport.cursor_line()};
}

void AssistHost::ApplyEditSideEffects(editor::TextViewport& viewport,
                                      const EditSnapshot& snapshot) const {
  if (auto* compare_tab = operations_.active_compare_tab();
      compare_tab != nullptr && &viewport == &compare_tab->right_viewport) {
    operations_.refresh_compare_tab_derived_state(*compare_tab);
    operations_.sync_compare_selection_from_viewport(*compare_tab, true);
  }
  if (auto* merge_tab = operations_.active_merge_tab();
      merge_tab != nullptr && &viewport == &merge_tab->result_viewport) {
    operations_.update_merge_tracking_after_viewport_edit(*merge_tab);
  }
  operations_.reset_caret_blink();
  operations_.request_active_editable_last_change_redraw();
  if (viewport.dirty() != snapshot.was_dirty) {
    operations_.request_active_editable_blame_neighborhood_redraw(snapshot.cursor_before_line,
                                                                  viewport.cursor_line());
  }
}

void AssistHost::RedrawAfterCaretChange() const {
  operations_.reset_caret_blink();
  operations_.request_active_editable_last_change_redraw();
  operations_.request_focused_editor_redraw();
}

namespace assist {

bool ResultIsStale(const editor::TextViewport* active_editable,
                   const std::filesystem::path& request_path) {
  return active_editable == nullptr || active_editable->path() != request_path;
}

std::string_view LineViewAt(const editor::TextViewport& viewport, std::size_t index) {
  return index < viewport.lines().size() ? viewport.lines().LineView(index) : std::string_view{};
}

editor::SelectionRange CompletionReplacementRange(const editor::TextViewport& viewport) {
  // Only walk back over the trailing identifier token. `.`/`/`/`-` are token
  // SEPARATORS, not word chars: including them made a member completion (obj.|)
  // or path completion (a/b|) replace the whole chain, dropping the qualifier.
  // The LSP path prefers the server's textEdit range anyway; this heuristic only
  // covers plugin completions and LSP items without a textEdit. Codepoint-aware:
  // a byte-wise ASCII walk stopped inside `größe|`, so accepting `größe_total`
  // produced `größgröße_total`.
  //
  // Quick suggestions ask this on every word character typed, and the whole line
  // is not needed -- only the run ending at the caret. Reading the line would
  // materialize it when it spans pieces (any edited line): on a minified file,
  // the file, per keystroke (editor_typing_minified_line, +14% against v2.12.0).
  // A window before the caret, widened while the run reaches its start (within a
  // codepoint of it, so a window opening mid-character cannot cut the walk short).
  const std::size_t line_index = viewport.cursor_line();
  if (line_index >= viewport.lines().size()) {
    const editor::TextPosition caret{line_index, viewport.cursor_column()};
    return editor::SelectionRange{.start = caret, .end = caret};
  }
  const std::size_t caret = std::min(viewport.cursor_column(), viewport.lines().LineLength(line_index));
  thread_local std::string scratch;
  std::size_t start_column = caret;
  for (std::size_t window = 256;; window *= 2) {
    const std::size_t window_start = caret > window ? caret - window : 0;
    const std::string_view text = viewport.lines().LineWindow(line_index, window_start, caret - window_start, scratch);
    const std::size_t run_start = util::Utf8IdentifierRunStart(text, text.size());
    start_column = window_start + run_start;
    if (window_start == 0 || run_start >= 4) {
      break;
    }
  }
  return editor::SelectionRange{
      .start = editor::TextPosition{viewport.cursor_line(), start_column},
      .end = editor::TextPosition{viewport.cursor_line(), viewport.cursor_column()},
  };
}

CodeActionEdit ToCodeActionEdit(const std::filesystem::path& path,
                                const LspClient::Range& range,
                                const std::string& new_text) {
  const auto clamp = [](int value) { return static_cast<std::size_t>(std::max(0, value)); };
  return CodeActionEdit{
      .path = path,
      .range = editor::SelectionRange{
          .start = editor::TextPosition{clamp(range.start.line), clamp(range.start.character)},
          .end = editor::TextPosition{clamp(range.end.line), clamp(range.end.character)},
      },
      .new_text = new_text,
  };
}

}  // namespace assist

}  // namespace microide::workspace
