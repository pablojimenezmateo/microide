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
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/WorkspaceLspManager.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

class WorkspaceEditService;

class AssistHost {
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
    std::function<void()> reset_caret_blink;
    std::function<void()> request_frame;
    std::function<void()> note_active_editable_edit;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void(MergeTabState&)> update_merge_tracking_after_viewport_edit;
    std::function<void(std::string)> notify_info;
  };

  void Configure(WorkspaceContext& context,
                 WorkspacePluginRuntime& plugin_runtime,
                 WorkspaceOutputChannels& output_channels,
                 WorkspaceLanguageContract& language_contract,
                 WorkspaceEditService& edits,
                 Operations operations);

  const Operations& ops() const { return operations_; }
  WorkspaceEditService& edits() const { return *edits_; }
  WorkspaceContext& context() const { return *context_; }
  WorkspacePluginRuntime& plugins() const { return *plugin_runtime_; }
  WorkspaceOutputChannels& output() const { return *output_channels_; }
  WorkspaceLanguageContract& languages() const { return *language_contract_; }

  bool IsStale(const std::filesystem::path& request_path) const;
  bool SnippetsEnabled() const;

  editor::TextViewport* RequireActiveEditableViewport(std::string* error_message) const;
  LspClient* PrepareLspRequest(editor::TextViewport& viewport, std::string* error_message) const;
  void MaybeLogLspUnavailable(const std::string& language_id, bool lsp_authoritative) const;
  void Report(const char* channel_id, const char* channel_title, const std::string& message) const;

  struct EditSnapshot {
    bool was_dirty = false;
    std::size_t cursor_before_line = 0;
  };
  static EditSnapshot CaptureEditSnapshot(const editor::TextViewport& viewport);
  void ApplyEditSideEffects(editor::TextViewport& viewport, const EditSnapshot& snapshot) const;
  void RedrawAfterCaretChange() const;

 private:
  WorkspaceContext* context_ = nullptr;
  WorkspacePluginRuntime* plugin_runtime_ = nullptr;
  WorkspaceOutputChannels* output_channels_ = nullptr;
  WorkspaceLanguageContract* language_contract_ = nullptr;
  WorkspaceEditService* edits_ = nullptr;
  Operations operations_{};
};

namespace assist {

bool ResultIsStale(const editor::TextViewport* active_editable,
                   const std::filesystem::path& request_path);

std::string_view LineViewAt(const editor::TextViewport& viewport, std::size_t index);

editor::SelectionRange CompletionReplacementRange(const editor::TextViewport& viewport);

CodeActionEdit ToCodeActionEdit(const std::filesystem::path& path,
                                const LspClient::Range& range,
                                const std::string& new_text);

}

}
