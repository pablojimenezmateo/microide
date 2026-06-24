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
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {
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
  const std::string_view line = LineAtOrEmpty(viewport.lines(), viewport.cursor_line());
  std::size_t start_column = std::min(viewport.cursor_column(), line.size());
  while (start_column > 0) {
    const char ch = line[start_column - 1];
    const bool identifier_char =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '_' || ch == '.' || ch == '/' || ch == '-';
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
  const auto value = operations_.get_setting_value("editor.snippets.enabled");
  if (!value.has_value()) {
    return true;
  }
  return *value != "false" && *value != "0" && *value != "off";
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
  std::string provider_error;
  const auto items = plugin_runtime_->Host().QueryCompletions(language_id, viewport->path(),
                                                              viewport->cursor_line() + 1,
                                                              viewport->cursor_column() + 1, {},
                                                              &provider_error);
  context_->current_project_state.overlay.workflow.completion.items.clear();
  context_->current_project_state.overlay.workflow.completion.selected_index = 0;
  context_->current_project_state.overlay.workflow.completion.replacement_range =
      CompletionReplacementRange(*viewport);
  context_->current_project_state.overlay.workflow.completion.source = "plugin";
  context_->current_project_state.overlay.workflow.completion.error = provider_error;
  for (const auto& item : items) {
    const bool snippets_on = EditorSnippetsSettingEnabled();
    context_->current_project_state.overlay.workflow.completion.items.push_back(
        CompletionSessionItem{
            .label = item.label,
            .detail = item.detail,
            .documentation = item.documentation,
            .insert_text = item.insert_text,
            .is_snippet = snippets_on && item.is_snippet,
        });
  }
  if (!context_->current_project_state.overlay.workflow.completion.items.empty()) {
    operations_.show_overlay(OverlayMode::Completion);
    return true;
  }

  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  if (client == nullptr) {
    const std::string failure =
        LspUnavailableMessage(operations_.current_lsp_manager(), language_id, provider_error);
    output_channels_->AppendLine("lsp.log", "LSP Log", failure);
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }

  operations_.ensure_lsp_document_open(*viewport, *client, language_id);
  auto& session = context_->current_project_state.overlay.workflow.completion;
  session.items.clear();
  session.selected_index = 0;
  session.replacement_range = CompletionReplacementRange(*viewport);
  session.source = "lsp";
  session.error = "Loading...";
  operations_.show_overlay(OverlayMode::Completion);
  operations_.begin_tracked_lsp_request();
  client->RequestCompletionAsync(
      FileUriForPath(viewport->path()),
      LspClient::Position{static_cast<int>(viewport->cursor_line()),
                          static_cast<int>(viewport->cursor_column())},
      [this](std::optional<std::vector<LspClient::CompletionItem>> items) {
        operations_.finish_tracked_lsp_request();
        auto& current_session = context_->current_project_state.overlay.workflow.completion;
        current_session.items.clear();
        current_session.selected_index = 0;
        current_session.source = "lsp";
        if (!items.has_value() || items->empty()) {
          current_session.error = "No completions available";
        } else {
          current_session.error.clear();
          for (const auto& item : *items) {
            const bool snippets_on = EditorSnippetsSettingEnabled();
            current_session.items.push_back(CompletionSessionItem{
                .label = item.label,
                .detail = item.detail,
                .documentation = item.documentation,
                .insert_text = item.insert_text,
                .is_snippet = snippets_on && item.insert_text_format == 2,
            });
          }
        }
        operations_.request_overlay_redraw();
      });
  return true;
}

AssistService::EditSideEffectsSnapshot AssistService::CaptureEditSnapshot(
    editor::TextViewport& viewport) const {
  EditSideEffectsSnapshot snapshot;
  snapshot.was_dirty = viewport.dirty();
  snapshot.cursor_before_line = viewport.cursor_line();
  if (auto* merge_tab = operations_.active_merge_tab();
      merge_tab != nullptr && &viewport == &merge_tab->result_viewport) {
    snapshot.before_lines = viewport.lines();
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
  const EditSideEffectsSnapshot snapshot = CaptureEditSnapshot(*viewport);
  const bool want_snippet = EditorSnippetsSettingEnabled() && item.is_snippet;
  bool snippet_applied = false;
  if (want_snippet) {
    if (TabEntry::EditorTabState* editor_tab = operations_.active_editor_tab()) {
      viewport->BeginUndoGroup();
      snippet_applied = editor::ExpandSnippetAtSelection(*viewport, editor_tab->snippet_session,
                                                         session.replacement_range,
                                                         item.insert_text);
      if (!snippet_applied) {
        if (viewport->UndoGroupActive()) {
          viewport->EndUndoGroup();
        }
        if (!viewport->ReplaceRange(session.replacement_range, item.insert_text)) {
          return false;
        }
      }
    } else if (!viewport->ReplaceRange(session.replacement_range, item.insert_text)) {
      return false;
    }
  } else if (!viewport->ReplaceRange(session.replacement_range, item.insert_text)) {
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
  if (tab == nullptr || viewport == nullptr || !tab->snippet_session.active) {
    return false;
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

bool AssistService::ShowCodeActionsOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectViewportLanguageId(*viewport);
  const std::optional<editor::SelectionRange> selection = viewport->selection_range();
  const editor::SelectionRange range = selection.value_or(editor::SelectionRange{
      .start = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
      .end = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
  });
  std::string provider_error;
  const auto items = plugin_runtime_->Host().QueryCodeActions(
      language_id, viewport->path(), range.start.line + 1, range.start.column + 1,
      range.end.line + 1, range.end.column + 1, &provider_error);
  auto& session = context_->current_project_state.overlay.workflow.code_actions;
  session.items.clear();
  session.selected_index = 0;
  session.source = "plugin";
  session.error = provider_error;
  for (const auto& item : items) {
    session.items.push_back(CodeActionSessionItem{
        .title = item.title,
        .command = item.command,
        .arguments = item.arguments,
    });
  }
  if (!session.items.empty()) {
    operations_.show_overlay(OverlayMode::CodeActions);
    return true;
  }

  LspClient* client = operations_.lsp_client_for_viewport(*viewport, nullptr);
  if (client == nullptr) {
    const std::string failure =
        LspUnavailableMessage(operations_.current_lsp_manager(), language_id, provider_error);
    output_channels_->AppendLine("lsp.log", "LSP Log", failure);
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }

  operations_.ensure_lsp_document_open(*viewport, *client, language_id);
  session.items.clear();
  session.selected_index = 0;
  session.source = "lsp";
  session.error = "Loading...";
  operations_.show_overlay(OverlayMode::CodeActions);
  operations_.begin_tracked_lsp_request();
  client->RequestCodeActionAsync(
      FileUriForPath(viewport->path()),
      LspClient::Range{
          .start = LspClient::Position{static_cast<int>(range.start.line),
                                       static_cast<int>(range.start.column)},
          .end = LspClient::Position{static_cast<int>(range.end.line),
                                     static_cast<int>(range.end.column)},
      },
      [this](std::optional<std::vector<LspClient::CodeAction>> actions) {
        operations_.finish_tracked_lsp_request();
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
            current_session.items.push_back(CodeActionSessionItem{
                .title = action.title,
                .command = action.command,
                .arguments = std::move(arguments),
            });
          }
        }
        operations_.request_overlay_redraw();
      });
  return true;
}

bool AssistService::ExecuteSelectedCodeAction() {
  auto& session = context_->current_project_state.overlay.workflow.code_actions;
  if (session.items.empty()) {
    return false;
  }
  const CodeActionSessionItem& action =
      session.items[std::min(session.selected_index, session.items.size() - 1)];
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
  editor::TextViewport* viewport = operations_.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return false;
  }

  // Plugin-native definition providers run first (mirrors completion / code
  // action orchestration); a non-empty result short-circuits the LSP path.
  {
    const std::string plugin_language_id = DetectViewportLanguageId(*viewport);
    std::string provider_error;
    const auto locations = plugin_runtime_->Host().QueryDefinition(
        plugin_language_id, viewport->path(), viewport->cursor_line() + 1,
        viewport->cursor_column() + 1, &provider_error);
    if (!locations.empty()) {
      NavigateToPluginLocation(locations.front());
      return true;
    }
  }

  std::string language_id;
  LspClient* client = operations_.lsp_client_for_viewport(*viewport, &language_id);
  if (client == nullptr) {
    const std::string failure =
        LspUnavailableMessage(operations_.current_lsp_manager(), language_id, {});
    output_channels_->AppendLine("lsp.log", "LSP Log", failure);
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }
  operations_.ensure_lsp_document_open(*viewport, *client, language_id);
  operations_.begin_tracked_lsp_request();
  client->RequestGoToDefinitionAsync(
      FileUriForPath(viewport->path()),
      LspClient::Position{static_cast<int>(viewport->cursor_line()),
                          static_cast<int>(viewport->cursor_column())},
      [this](std::optional<std::vector<LspClient::Location>> locations) {
        operations_.finish_tracked_lsp_request();
        if (!locations.has_value() || locations->empty()) {
          output_channels_->AppendLine("lsp.definition", "LSP Definition", "No definition found");
          return;
        }
        const std::optional<std::filesystem::path> path = PathFromFileUri(locations->front().uri);
        if (!path.has_value()) {
          return;
        }
        if (!operations_.open_file_in_new_tab(*path)) {
          return;
        }
        if (editor::TextViewport* active = operations_.active_editor_viewport(); active != nullptr) {
          active->MoveCursorTo(
              static_cast<std::size_t>(std::max(locations->front().range.start.line, 0)),
              static_cast<std::size_t>(std::max(locations->front().range.start.character, 0)));
          operations_.reset_caret_blink();
          operations_.request_focused_editor_redraw();
        }
      });
  return true;
}

bool AssistService::FindLspReferences(std::string* error_message) {
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

  // Plugin-native reference providers run first; a non-empty result is rendered
  // into the same References output channel the LSP path uses and short-circuits.
  {
    const std::string plugin_language_id = DetectViewportLanguageId(*viewport);
    std::string provider_error;
    const auto locations = plugin_runtime_->Host().QueryReferences(
        plugin_language_id, viewport->path(), viewport->cursor_line() + 1,
        viewport->cursor_column() + 1, true, &provider_error);
    if (!locations.empty()) {
      EmitPluginReferences(locations);
      return true;
    }
  }

  std::string language_id;
  LspClient* client = operations_.lsp_client_for_viewport(*viewport, &language_id);
  if (client == nullptr) {
    const std::string failure =
        LspUnavailableMessage(operations_.current_lsp_manager(), language_id, {});
    output_channels_->AppendLine("lsp.log", "LSP Log", failure);
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }
  operations_.ensure_lsp_document_open(*viewport, *client, language_id);
  operations_.begin_tracked_lsp_request();
  client->RequestFindReferencesAsync(
      FileUriForPath(viewport->path()),
      LspClient::Position{static_cast<int>(viewport->cursor_line()),
                          static_cast<int>(viewport->cursor_column())},
      true,
      [this](std::optional<std::vector<LspClient::Location>> locations) {
        operations_.finish_tracked_lsp_request();
        output_channels_->Clear("lsp.references");
        if (!locations.has_value() || locations->empty()) {
          output_channels_->AppendLine("lsp.references", "LSP References", "No references found");
          return;
        }
        std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
        for (std::size_t location_index = 0; location_index < locations->size(); ++location_index) {
          const auto& location = (*locations)[location_index];
          const std::optional<std::filesystem::path> path = PathFromFileUri(location.uri);
          if (!path.has_value()) {
            continue;
          }
          const std::string label =
              context_->current_project_state.root.empty()
                  ? path->generic_string()
                  : RelativePathLabel(context_->current_project_state.root, *path);
          output_channels_->AppendLine(
              "lsp.references", "LSP References",
              label + ":" + std::to_string(location.range.start.line + 1) + ":" +
                  std::to_string(location.range.start.character + 1));

          const auto lines_it = file_line_cache.find(*path);
          const std::vector<std::string>* file_lines = nullptr;
          if (lines_it != file_line_cache.end()) {
            file_lines = &lines_it->second;
          } else if (const auto text = util::ReadTextFile(*path); text.has_value()) {
            file_lines = &file_line_cache.emplace(*path, util::SplitLines(*text)).first->second;
          }
          if (file_lines == nullptr || file_lines->empty()) {
            continue;
          }

          const std::size_t target_line =
              static_cast<std::size_t>(std::max(location.range.start.line + 1, 1));
          const std::size_t first_line = target_line > 1 ? target_line - 1 : 1;
          const std::size_t last_line = target_line + 1;
          for (std::size_t line_number = first_line; line_number <= last_line; ++line_number) {
            if (line_number == 0 || line_number > file_lines->size()) {
              continue;
            }
            output_channels_->AppendLine(
                "lsp.references", "LSP References",
                std::string(line_number == target_line ? " > " : "   ") +
                    std::to_string(line_number) + " | " + (*file_lines)[line_number - 1]);
          }
          if (location_index + 1 < locations->size()) {
            output_channels_->AppendLine("lsp.references", "LSP References", "");
          }
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
    const std::string label =
        context_->current_project_state.root.empty()
            ? location.path.generic_string()
            : RelativePathLabel(context_->current_project_state.root, location.path);
    output_channels_->AppendLine(
        "lsp.references", "References",
        label + ":" + std::to_string(location.line) + ":" + std::to_string(location.column));

    const auto lines_it = file_line_cache.find(location.path);
    const std::vector<std::string>* file_lines = nullptr;
    if (lines_it != file_line_cache.end()) {
      file_lines = &lines_it->second;
    } else if (const auto text = util::ReadTextFile(location.path); text.has_value()) {
      file_lines =
          &file_line_cache.emplace(location.path, util::SplitLines(*text)).first->second;
    }
    if (file_lines == nullptr || file_lines->empty()) {
      continue;
    }

    const std::size_t target_line = location.line > 0 ? location.line : 1;
    const std::size_t first_line = target_line > 1 ? target_line - 1 : 1;
    const std::size_t last_line = target_line + 1;
    for (std::size_t line_number = first_line; line_number <= last_line; ++line_number) {
      if (line_number == 0 || line_number > file_lines->size()) {
        continue;
      }
      output_channels_->AppendLine(
          "lsp.references", "References",
          std::string(line_number == target_line ? " > " : "   ") +
              std::to_string(line_number) + " | " + (*file_lines)[line_number - 1]);
    }
    if (index + 1 < locations.size()) {
      output_channels_->AppendLine("lsp.references", "References", "");
    }
  }
}

}  // namespace microide::workspace
