#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/SnippetEngine.h"
#include "util/JsonValue.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::workspace {

namespace {

std::string_view LineAtOrEmpty(const std::vector<std::string>& lines, std::size_t index) {
  return index < lines.size() ? std::string_view(lines[index]) : std::string_view{};
}

std::string DetectActiveLanguageId(const editor::TextViewport& viewport) {
  return editor::runtime_syntax::DetectFiletype(viewport.path(), viewport.lines());
}

bool IsUnreservedUriByte(unsigned char ch) {
  return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

std::string FileUriForPath(const std::filesystem::path& path) {
  const std::string raw = path.lexically_normal().generic_string();
  std::ostringstream encoded;
  encoded << "file://";
  for (unsigned char ch : raw) {
    if (IsUnreservedUriByte(ch)) {
      encoded << static_cast<char>(ch);
      continue;
    }
    encoded << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(ch) << std::nouppercase << std::dec;
  }
  return encoded.str();
}

std::optional<std::filesystem::path> PathFromFileUri(std::string_view uri) {
  static constexpr std::string_view kFileScheme = "file://";
  if (!uri.starts_with(kFileScheme)) {
    return std::nullopt;
  }

  std::string_view encoded = uri.substr(kFileScheme.size());
  if (encoded.starts_with("localhost/")) {
    encoded.remove_prefix(std::string_view("localhost").size());
  }

  std::string decoded;
  decoded.reserve(encoded.size());
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      const auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
      };
      const int hi = hex_value(encoded[i + 1]);
      const int lo = hex_value(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    decoded.push_back(encoded[i]);
  }
  if (decoded.empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(decoded).lexically_normal();
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

bool WorkspaceShell::EditorSnippetsSettingEnabled() const {
  const auto value = GetSettingValue("editor.snippets.enabled");
  if (!value.has_value()) {
    return true;
  }
  return *value != "false" && *value != "0" && *value != "off";
}

bool WorkspaceShell::ShowCompletionOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectActiveLanguageId(*viewport);
  std::string provider_error;
  const auto items =
      plugin_runtime_.Host().QueryCompletions(language_id, viewport->path(),
                                              viewport->cursor_line() + 1,
                                              viewport->cursor_column() + 1, {}, &provider_error);
  context_.current_project_state.overlay.workflow.completion.items.clear();
  context_.current_project_state.overlay.workflow.completion.selected_index = 0;
  context_.current_project_state.overlay.workflow.completion.replacement_range =
      CompletionReplacementRange(*viewport);
  context_.current_project_state.overlay.workflow.completion.source = "plugin";
  context_.current_project_state.overlay.workflow.completion.error = provider_error;
  for (const auto& item : items) {
    const bool snippets_on = EditorSnippetsSettingEnabled();
    context_.current_project_state.overlay.workflow.completion.items.push_back(
        CompletionSessionItem{
            .label = item.label,
            .detail = item.detail,
            .documentation = item.documentation,
            .insert_text = item.insert_text,
            .is_snippet = snippets_on && item.is_snippet,
        });
  }
  if (!context_.current_project_state.overlay.workflow.completion.items.empty()) {
    ShowOverlay(OverlayMode::Completion);
    return true;
  }

  LspClient* client = LspClientForViewport(*viewport, nullptr);
  if (client == nullptr) {
    const std::string failure = LspUnavailableMessage(CurrentLspManager(), language_id, provider_error);
    output_channels_.AppendLine("lsp.log", "LSP Log", failure);
    ShowOutputChannel("lsp.log");
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }

  EnsureLspDocumentOpen(*viewport, *client, language_id);
  auto& session = context_.current_project_state.overlay.workflow.completion;
  session.items.clear();
  session.selected_index = 0;
  session.replacement_range = CompletionReplacementRange(*viewport);
  session.source = "lsp";
  session.error = "Loading...";
  ShowOverlay(OverlayMode::Completion);
  BeginTrackedLspRequest();
  client->RequestCompletionAsync(
      FileUriForPath(viewport->path()),
      LspClient::Position{static_cast<int>(viewport->cursor_line()),
                          static_cast<int>(viewport->cursor_column())},
      [this](std::optional<std::vector<LspClient::CompletionItem>> items) {
        FinishTrackedLspRequest();
        auto& current_session = context_.current_project_state.overlay.workflow.completion;
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
        RequestOverlayRedraw();
      });
  return true;
}

bool WorkspaceShell::ApplySelectedCompletion() {
  auto& session = context_.current_project_state.overlay.workflow.completion;
  if (session.items.empty()) {
    return false;
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return false;
  }

  const CompletionSessionItem& item =
      session.items[std::min(session.selected_index, session.items.size() - 1)];
  const bool was_dirty = viewport->dirty();
  const std::size_t cursor_before_line = viewport->cursor_line();
  std::vector<std::string> before_lines;
  std::optional<editor::SelectionRange> selection_before;
  std::optional<editor::TextPosition> cursor_before;
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
    before_lines = viewport->lines();
    selection_before = viewport->selection_range();
    cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
  }
  const bool want_snippet = EditorSnippetsSettingEnabled() && item.is_snippet;
  bool snippet_applied = false;
  if (want_snippet) {
    if (TabEntry::EditorTabState* editor_tab = ActiveEditorTab()) {
      viewport->BeginUndoGroup();
      snippet_applied = editor::ExpandSnippetAtSelection(*viewport, editor_tab->snippet_session,
                                                         session.replacement_range, item.insert_text);
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
  if (auto* compare_tab = ActiveCompareTab();
      compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
    RefreshCompareTabDerivedState(*compare_tab);
    SyncCompareSelectionFromViewport(*compare_tab, true);
  }
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
    UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, *cursor_before);
  }
  ResetCaretBlink();
  RequestActiveEditableLastChangeRedraw();
  RequestFocusedEditorRedraw();
  if (viewport->dirty() != was_dirty) {
    RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line, viewport->cursor_line());
    RequestTabStripRedraw();
  }
  DismissOverlay(true);
  return true;
}

bool WorkspaceShell::ShowInsertSnippetOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (!EditorSnippetsSettingEnabled()) {
    if (error_message != nullptr) {
      *error_message = "Snippets are disabled";
    }
    return false;
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return false;
  }
  const std::string language_id = DetectActiveLanguageId(*viewport);
  const LanguageContract* contract = language_contract_.Find(language_id);
  if (contract == nullptr || contract->snippets.empty()) {
    if (error_message != nullptr) {
      *error_message = "No snippets for this language";
    }
    return false;
  }
  auto& session = context_.current_project_state.overlay.workflow.completion;
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
  ShowOverlay(OverlayMode::Completion);
  return true;
}

bool WorkspaceShell::TrySnippetTabInEditor(bool shift_tab) {
  if (!EditorSnippetsSettingEnabled()) {
    return false;
  }
  TabEntry::EditorTabState* tab = ActiveEditorTab();
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (tab == nullptr || viewport == nullptr || !tab->snippet_session.active) {
    return false;
  }
  if (!editor::SnippetNavigateTab(*viewport, tab->snippet_session, shift_tab)) {
    return false;
  }
  tab->folding_model.MarkDirty();
  ResetCaretBlink();
  RequestActiveEditableLastChangeRedraw();
  RequestFocusedEditorRedraw();
  return true;
}

bool WorkspaceShell::TrySnippetEscapeInEditor() {
  if (!EditorSnippetsSettingEnabled()) {
    return false;
  }
  TabEntry::EditorTabState* tab = ActiveEditorTab();
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (tab == nullptr || viewport == nullptr || !tab->snippet_session.active) {
    return false;
  }
  if (!editor::SnippetHandleEscape(*viewport, tab->snippet_session)) {
    return false;
  }
  tab->folding_model.MarkDirty();
  ResetCaretBlink();
  RequestActiveEditableLastChangeRedraw();
  RequestFocusedEditorRedraw();
  return true;
}

void WorkspaceShell::NotifySnippetSessionCaretMoved() {
  TabEntry::EditorTabState* tab = ActiveEditorTab();
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (tab == nullptr || viewport == nullptr || !tab->snippet_session.active) {
    return;
  }
  editor::SnippetOnCaretMoved(*viewport, tab->snippet_session);
  if (!tab->snippet_session.active) {
    tab->folding_model.MarkDirty();
    ResetCaretBlink();
    RequestActiveEditableLastChangeRedraw();
    RequestFocusedEditorRedraw();
  }
}

void WorkspaceShell::ClearActiveSnippetSessionAfterUndo() {
  TabEntry::EditorTabState* tab = ActiveEditorTab();
  if (tab == nullptr || !tab->snippet_session.active) {
    return;
  }
  tab->snippet_session.Reset(nullptr);
}

bool WorkspaceShell::TrySnippetInsertTextInEditor(editor::TextViewport* viewport, std::string_view text) {
  if (!EditorSnippetsSettingEnabled() || viewport == nullptr || text.empty()) {
    return false;
  }
  TabEntry::EditorTabState* tab = ActiveEditorTab();
  if (tab == nullptr || !tab->snippet_session.active) {
    return false;
  }
  const bool was_dirty = viewport->dirty();
  const std::size_t cursor_before_line = viewport->cursor_line();
  std::vector<std::string> before_lines;
  std::optional<editor::SelectionRange> selection_before;
  std::optional<editor::TextPosition> cursor_before;
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
    before_lines = viewport->lines();
    selection_before = viewport->selection_range();
    cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
  }
  if (!editor::SnippetTryInsertText(*viewport, tab->snippet_session, text)) {
    return false;
  }
  tab->folding_model.MarkDirty();
  if (auto* compare_tab = ActiveCompareTab();
      compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
    RefreshCompareTabDerivedState(*compare_tab);
    SyncCompareSelectionFromViewport(*compare_tab, true);
  }
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport && cursor_before.has_value()) {
    UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, *cursor_before);
  }
  ResetCaretBlink();
  RequestActiveEditableLastChangeRedraw();
  if (viewport->dirty() != was_dirty) {
    RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line, viewport->cursor_line());
    RequestTabStripRedraw();
  }
  return true;
}

bool WorkspaceShell::TrySnippetBackspaceInEditor(editor::TextViewport* viewport) {
  if (!EditorSnippetsSettingEnabled() || viewport == nullptr) {
    return false;
  }
  TabEntry::EditorTabState* tab = ActiveEditorTab();
  if (tab == nullptr || !tab->snippet_session.active) {
    return false;
  }
  const bool was_dirty = viewport->dirty();
  const std::size_t cursor_before_line = viewport->cursor_line();
  std::vector<std::string> before_lines;
  std::optional<editor::SelectionRange> selection_before;
  std::optional<editor::TextPosition> cursor_before;
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
    before_lines = viewport->lines();
    selection_before = viewport->selection_range();
    cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
  }
  if (!editor::SnippetTryBackspace(*viewport, tab->snippet_session)) {
    return false;
  }
  tab->folding_model.MarkDirty();
  if (auto* compare_tab = ActiveCompareTab();
      compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
    RefreshCompareTabDerivedState(*compare_tab);
    SyncCompareSelectionFromViewport(*compare_tab, true);
  }
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport && cursor_before.has_value()) {
    UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, *cursor_before);
  }
  ResetCaretBlink();
  RequestActiveEditableLastChangeRedraw();
  if (viewport->dirty() != was_dirty) {
    RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line, viewport->cursor_line());
    RequestTabStripRedraw();
  }
  return true;
}

bool WorkspaceShell::TrySnippetDeleteForwardInEditor(editor::TextViewport* viewport) {
  if (!EditorSnippetsSettingEnabled() || viewport == nullptr) {
    return false;
  }
  TabEntry::EditorTabState* tab = ActiveEditorTab();
  if (tab == nullptr || !tab->snippet_session.active) {
    return false;
  }
  const bool was_dirty = viewport->dirty();
  const std::size_t cursor_before_line = viewport->cursor_line();
  std::vector<std::string> before_lines;
  std::optional<editor::SelectionRange> selection_before;
  std::optional<editor::TextPosition> cursor_before;
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
    before_lines = viewport->lines();
    selection_before = viewport->selection_range();
    cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
  }
  if (!editor::SnippetTryDeleteForward(*viewport, tab->snippet_session)) {
    return false;
  }
  tab->folding_model.MarkDirty();
  if (auto* compare_tab = ActiveCompareTab();
      compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
    RefreshCompareTabDerivedState(*compare_tab);
    SyncCompareSelectionFromViewport(*compare_tab, true);
  }
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport && cursor_before.has_value()) {
    UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, *cursor_before);
  }
  ResetCaretBlink();
  RequestActiveEditableLastChangeRedraw();
  if (viewport->dirty() != was_dirty) {
    RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line, viewport->cursor_line());
    RequestTabStripRedraw();
  }
  return true;
}

bool WorkspaceShell::ShowCodeActionsOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectActiveLanguageId(*viewport);
  const std::optional<editor::SelectionRange> selection = viewport->selection_range();
  const editor::SelectionRange range = selection.value_or(editor::SelectionRange{
      .start = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
      .end = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
  });
  std::string provider_error;
  const auto items = plugin_runtime_.Host().QueryCodeActions(
      language_id, viewport->path(), range.start.line + 1, range.start.column + 1,
      range.end.line + 1, range.end.column + 1, &provider_error);
  auto& session = context_.current_project_state.overlay.workflow.code_actions;
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
    ShowOverlay(OverlayMode::CodeActions);
    return true;
  }

  LspClient* client = LspClientForViewport(*viewport, nullptr);
  if (client == nullptr) {
    const std::string failure = LspUnavailableMessage(CurrentLspManager(), language_id, provider_error);
    output_channels_.AppendLine("lsp.log", "LSP Log", failure);
    ShowOutputChannel("lsp.log");
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }

  EnsureLspDocumentOpen(*viewport, *client, language_id);
  session.items.clear();
  session.selected_index = 0;
  session.source = "lsp";
  session.error = "Loading...";
  ShowOverlay(OverlayMode::CodeActions);
  BeginTrackedLspRequest();
  client->RequestCodeActionAsync(
      FileUriForPath(viewport->path()),
      LspClient::Range{
          .start = LspClient::Position{static_cast<int>(range.start.line),
                                       static_cast<int>(range.start.column)},
          .end = LspClient::Position{static_cast<int>(range.end.line),
                                     static_cast<int>(range.end.column)},
      },
      [this](std::optional<std::vector<LspClient::CodeAction>> actions) {
        FinishTrackedLspRequest();
        auto& current_session = context_.current_project_state.overlay.workflow.code_actions;
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
        RequestOverlayRedraw();
      });
  return true;
}

bool WorkspaceShell::ExecuteSelectedCodeAction() {
  auto& session = context_.current_project_state.overlay.workflow.code_actions;
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
      ExecuteCommandName(action.command, action.arguments, ActionSource::Command, &error_message);
  if (executed) {
    DismissOverlay(true);
  } else {
    session.error = error_message;
    RequestOverlayRedraw();
  }
  return executed;
}

bool WorkspaceShell::GoToLspDefinition(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return false;
  }

  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    const std::string failure = LspUnavailableMessage(CurrentLspManager(), language_id, {});
    output_channels_.AppendLine("lsp.log", "LSP Log", failure);
    ShowOutputChannel("lsp.log");
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);
  BeginTrackedLspRequest();
  client->RequestGoToDefinitionAsync(
      FileUriForPath(viewport->path()),
      LspClient::Position{static_cast<int>(viewport->cursor_line()),
                          static_cast<int>(viewport->cursor_column())},
      [this](std::optional<std::vector<LspClient::Location>> locations) {
        FinishTrackedLspRequest();
        if (!locations.has_value() || locations->empty()) {
          output_channels_.AppendLine("lsp.definition", "LSP Definition", "No definition found");
          ShowOutputChannel("lsp.definition");
          return;
        }
        const std::optional<std::filesystem::path> path = PathFromFileUri(locations->front().uri);
        if (!path.has_value()) {
          return;
        }
        if (!OpenFileInNewTab(*path)) {
          return;
        }
        if (editor::TextViewport* active = ActiveEditorViewport(); active != nullptr) {
          active->MoveCursorTo(
              static_cast<std::size_t>(std::max(locations->front().range.start.line, 0)),
              static_cast<std::size_t>(std::max(locations->front().range.start.character, 0)));
          ResetCaretBlink();
          RequestFocusedEditorRedraw();
        }
      });
  return true;
}

bool WorkspaceShell::FindLspReferences(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return false;
  }

  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    const std::string failure = LspUnavailableMessage(CurrentLspManager(), language_id, {});
    output_channels_.AppendLine("lsp.log", "LSP Log", failure);
    ShowOutputChannel("lsp.log");
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);
  BeginTrackedLspRequest();
  client->RequestFindReferencesAsync(
      FileUriForPath(viewport->path()),
      LspClient::Position{static_cast<int>(viewport->cursor_line()),
                          static_cast<int>(viewport->cursor_column())},
      true,
      [this](std::optional<std::vector<LspClient::Location>> locations) {
        FinishTrackedLspRequest();
        output_channels_.Clear("lsp.references");
        if (!locations.has_value() || locations->empty()) {
          output_channels_.AppendLine("lsp.references", "LSP References", "No references found");
          ShowOutputChannel("lsp.references");
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
              context_.current_project_state.root.empty()
                  ? path->generic_string()
                  : std::filesystem::relative(*path, context_.current_project_state.root)
                        .generic_string();
          output_channels_.AppendLine(
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
            output_channels_.AppendLine(
                "lsp.references", "LSP References",
                std::string(line_number == target_line ? " > " : "   ") +
                    std::to_string(line_number) + " | " + (*file_lines)[line_number - 1]);
          }
          if (location_index + 1 < locations->size()) {
            output_channels_.AppendLine("lsp.references", "LSP References", "");
          }
        }
        ShowOutputChannel("lsp.references");
      });
  return true;
}

}  // namespace microide::workspace
