#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/WorkspaceLspManager.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

class AssistService {
 public:
  struct Operations {
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<TabEntry::EditorTabState*()> active_editor_tab;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<LspClient*(const editor::TextViewport&, std::string*)> lsp_client_for_viewport;
    std::function<LspManager&()> current_lsp_manager;
    std::function<void(const editor::TextViewport&, LspClient&, std::string_view)>
        ensure_lsp_document_open;
    std::function<void()> begin_tracked_lsp_request;
    std::function<void()> finish_tracked_lsp_request;
    std::function<void(OverlayMode)> show_overlay;
    std::function<void(bool)> dismiss_overlay;
    std::function<void()> request_overlay_redraw;
    std::function<bool(std::string_view, const std::vector<std::string>&, std::string*)>
        execute_command_name;
    std::function<bool(const std::filesystem::path&)> open_file_in_new_tab;
    std::function<void()> reset_caret_blink;
    std::function<void()> request_focused_editor_redraw;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void(MergeTabState&, const std::vector<std::string>&,
                       const std::optional<editor::SelectionRange>&, const editor::TextPosition&)>
        update_merge_tracking_after_viewport_edit;
  };

  AssistService() = default;

  void Configure(WorkspaceContext& context,
                 WorkspacePluginRuntime& plugin_runtime,
                 WorkspaceOutputChannels& output_channels,
                 WorkspaceLanguageContract& language_contract,
                 Operations operations);

  bool EditorSnippetsSettingEnabled() const;
  bool ShowCompletionOverlay(std::string* error_message = nullptr);
  bool ApplySelectedCompletion();
  bool ShowInsertSnippetOverlay(std::string* error_message = nullptr);
  bool TrySnippetTabInEditor(bool shift_tab);
  bool TrySnippetEscapeInEditor();
  void NotifySnippetSessionCaretMoved();
  void ClearActiveSnippetSessionAfterUndo();
  bool TrySnippetInsertTextInEditor(editor::TextViewport* viewport, std::string_view text);
  bool TrySnippetBackspaceInEditor(editor::TextViewport* viewport);
  bool TrySnippetDeleteForwardInEditor(editor::TextViewport* viewport);
  bool ShowCodeActionsOverlay(std::string* error_message = nullptr);
  bool ExecuteSelectedCodeAction();
  bool GoToLspDefinition(std::string* error_message = nullptr);
  bool FindLspReferences(std::string* error_message = nullptr);

 private:
  struct EditSideEffectsSnapshot {
    bool was_dirty = false;
    std::size_t cursor_before_line = 0;
    std::vector<std::string> before_lines;
    std::optional<editor::SelectionRange> selection_before;
    std::optional<editor::TextPosition> cursor_before;
  };

  EditSideEffectsSnapshot CaptureEditSnapshot(editor::TextViewport& viewport) const;
  void ApplyEditSideEffects(editor::TextViewport& viewport,
                            const EditSideEffectsSnapshot& snapshot) const;

  WorkspaceContext* context_ = nullptr;
  WorkspacePluginRuntime* plugin_runtime_ = nullptr;
  WorkspaceOutputChannels* output_channels_ = nullptr;
  WorkspaceLanguageContract* language_contract_ = nullptr;
  Operations operations_{};
};

}  // namespace microide::workspace
