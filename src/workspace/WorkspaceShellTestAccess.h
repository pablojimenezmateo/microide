#pragma once

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

#ifdef MICROIDE_TESTING

struct WorkspaceShell::TestAccess {
  static void SetProjectRoot(WorkspaceShell& shell, const std::filesystem::path& root) {
    shell.context_.current_project_state.root = root.lexically_normal();
    shell.context_.current_project_state.directory_tree.SetRoot(shell.context_.current_project_state.root);
    shell.context_.current_project_state.file_index.SetRoot(shell.context_.current_project_state.root);
    shell.context_.current_project_state.file_finder.SetIndex(&shell.context_.current_project_state.file_index);
    shell.context_.current_project_state.sidebar.visible = true;
    shell.context_.current_project_state.sidebar.view_id = "tree";
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Sidebar;
  }

  static void OpenSingleEditorTab(WorkspaceShell& shell, const std::filesystem::path& path) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      throw std::runtime_error("failed to open editor fixture: " + path.string());
    }
    shell.ApplyEditorPreferences(opened_view);
    shell.context_.current_project_state.welcome_surface.viewport = opened_view;
    shell.context_.current_project_state.open_tabs.push_back(WorkspaceShell::TabEntry{
        .kind = WorkspaceShell::TabEntry::Kind::Editor,
        .path = path.lexically_normal(),
        .title = path.filename().string(),
        .editor_state = WorkspaceShell::MakeEditorTabState(opened_view),
        .compare = std::nullopt,
        .merge = std::nullopt,
    });
    shell.context_.current_project_state.active_tab_index = 0;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Editor;
  }

  static editor::TextViewport& ActiveEditor(WorkspaceShell& shell) {
    return *shell.ActiveEditorViewport();
  }
  static bool ActiveEditorHasSelection(const WorkspaceShell& shell) {
    const editor::TextViewport* viewport = shell.ActiveEditorViewport();
    return viewport != nullptr && viewport->selection_range().has_value();
  }
  static std::string ActiveEditorSelectedText(WorkspaceShell& shell) {
    return shell.ActiveEditorViewport()->SelectedText();
  }
  static WorkspaceShell::CompareTabState& ActiveCompare(WorkspaceShell& shell) {
    return shell.context_.current_project_state.open_tabs[shell.context_.current_project_state.active_tab_index].compare.value();
  }
  static WorkspaceShell::MergeTabState& ActiveMerge(WorkspaceShell& shell) {
    return shell.context_.current_project_state.open_tabs[shell.context_.current_project_state.active_tab_index].merge.value();
  }
  static bool ActiveMergeHasSelection(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.open_tabs[shell.context_.current_project_state.active_tab_index]
        .merge->result_viewport.selection_range()
        .has_value();
  }
  static std::string ActiveMergeSelectedText(WorkspaceShell& shell) {
    return shell.context_.current_project_state.open_tabs[shell.context_.current_project_state.active_tab_index].merge->result_viewport.SelectedText();
  }
  static void ApplyMergeChoice(WorkspaceShell& shell, microide::compare::MergeChoice choice) {
    shell.ApplyMergeChoice(choice);
  }
  static void RefreshMergeTabDerivedState(WorkspaceShell& shell) {
    shell.RefreshMergeTabDerivedState(ActiveMerge(shell));
  }
  static const std::optional<WorkspaceShell::MergeHoverState>& ActiveMergeHoverState(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.open_tabs[shell.context_.current_project_state.active_tab_index].merge->hover_state;
  }
  static bool ActiveMergeHoverIsIncomingConflict(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.open_tabs[shell.context_.current_project_state.active_tab_index].merge->hover_state.has_value() &&
           shell.context_.current_project_state.open_tabs[shell.context_.current_project_state.active_tab_index].merge->hover_state->kind ==
               WorkspaceShell::MergeHoverState::Kind::IncomingConflict;
  }
  static microide::compare::MergeChoice ActiveMergeHoverPreviewChoice(const WorkspaceShell& shell) {
    const auto& hover = shell.context_.current_project_state.open_tabs[shell.context_.current_project_state.active_tab_index].merge->hover_state;
    return hover.has_value() ? hover->preview_choice : microide::compare::MergeChoice::Base;
  }
  static WorkspaceLayout CurrentLayout(WorkspaceShell& shell) {
    return shell.CurrentWorkspaceLayout().value();
  }
  static WorkspaceShell::MergeSurfaceLayout ActiveMergeSurfaceLayout(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeMergeSurfaceLayout(layout.editor_surface, ActiveMerge(shell));
  }
  static WorkspaceShell::MergeInteractionLayout ActiveMergeInteractionLayout(WorkspaceShell& shell) {
    auto& merge = ActiveMerge(shell);
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = shell.ComputeMergeSurfaceLayout(layout.editor_surface, merge);
    const auto scroll =
        shell.ComputeMergeScrollLayout(layout.editor_surface, surface, merge);
    merge.scroll_row = scroll.vertical_scroll;
    merge.horizontal_scroll = scroll.horizontal_scroll;
    merge.result_viewport.SetScrollLine(
        static_cast<std::size_t>(std::max(0, merge.scroll_row)));
    merge.result_viewport.SetHorizontalScroll(merge.horizontal_scroll);
    merge.scroll_row = static_cast<int>(merge.result_viewport.scroll_line());
    merge.horizontal_scroll = merge.result_viewport.horizontal_scroll();
    return shell.BuildMergeInteractionLayout(layout.editor_surface, surface, merge);
  }
  static WorkspaceShell::CompareSurfaceLayout ActiveCompareSurfaceLayout(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeCompareSurfaceLayout(layout.editor_surface, ActiveCompare(shell));
  }
  static SDL_FRect ActiveCompareEditableRect(WorkspaceShell& shell) {
    auto& compare = ActiveCompare(shell);
    const auto surface = ActiveCompareSurfaceLayout(shell);
    return shell.BuildCompareRightInteractionLayout(surface, compare).rect;
  }
  static SDL_FRect ActiveMergeResultRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    return ComputeMergeResultViewportRect(layout.editor_surface, surface.center_x, surface.rows_y,
                                          surface.gutter_width, surface.center_width,
                                          surface.show_horizontal);
  }
  static SDL_FRect MergeSourceAcceptRect(WorkspaceShell& shell,
                                         std::size_t conflict_index,
                                         bool incoming) {
    auto& merge = ActiveMerge(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto interaction = ActiveMergeInteractionLayout(shell);
    const auto& conflict = merge.conflicts[conflict_index];
    return shell.BuildMergeSourceActionButtonRect(surface, interaction, conflict, incoming);
  }
  static std::array<SDL_FRect, 4> MergeResultActionRects(WorkspaceShell& shell,
                                                         std::size_t conflict_index) {
    auto& merge = ActiveMerge(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto interaction = ActiveMergeInteractionLayout(shell);
    const auto& conflict = merge.conflicts[conflict_index];
    return shell.BuildMergeResultActionButtonRects(surface, interaction, conflict);
  }
  static std::array<SDL_FRect, 2> MergeToolbarNavigationRects(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto toolbar = shell.ComputeMergeToolbarLayout(layout.editor_surface, surface);
    return {toolbar.prev_rect, toolbar.next_rect};
  }
  static std::array<SDL_FRect, 2> MergeDividerRects(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    return {MakeRect(surface.center_x - surface.divider_width, layout.editor_surface.y,
                     surface.divider_width, layout.editor_surface.h),
            MakeRect(surface.right_x - surface.divider_width, layout.editor_surface.y,
                     surface.divider_width, layout.editor_surface.h)};
  }

  static void PrepareRenamePrompt(WorkspaceShell& shell,
                                  const std::filesystem::path& path,
                                  std::string input) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::RenamePath,
                            WorkspaceShell::PromptSurfaceState::Kind::TextInput, path,
                            std::move(input));
  }

  static void PrepareDeletePrompt(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::DeletePath,
                            WorkspaceShell::PromptSurfaceState::Kind::Confirm, path);
  }
  static void PrepareDiscardAllGitPrompt(WorkspaceShell& shell) {
    shell.OpenDiscardAllGitSidebarPrompt();
  }

  static void ConfirmPromptSurface(WorkspaceShell& shell) { shell.ConfirmPromptSurface(); }
  static void ConfirmPromptSurface(WorkspaceShell& shell, int selected_button) {
    shell.context_.prompts.surface.selected_button = selected_button;
    shell.ConfirmPromptSurface();
  }
  static bool SplitActiveEditor(WorkspaceShell& shell, bool vertical = true) {
    return shell.SplitActiveEditor(vertical ? WorkspaceShell::EditorSplitOrientation::Vertical
                                            : WorkspaceShell::EditorSplitOrientation::Horizontal);
  }
  static bool ActivateOrderedEditorSplit(WorkspaceShell& shell, std::size_t order_index) {
    return shell.ActivateOrderedEditorSplit(order_index);
  }
  static bool ReplaceActiveEditorWithFile(WorkspaceShell& shell, const std::filesystem::path& path) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      return false;
    }
    shell.ApplyEditorPreferences(opened_view);
    return shell.ReplaceActiveEditorView(opened_view);
  }
  static bool OpenWorkingTreeComparison(WorkspaceShell& shell,
                                        const std::filesystem::path& path,
                                        const std::string& left_ref,
                                        const std::string& left_label) {
    return shell.OpenWorkingTreeComparison(path, left_ref, left_label);
  }
  static bool OpenBranchHeadComparison(WorkspaceShell& shell,
                                       const std::filesystem::path& path,
                                       const std::string& left_ref,
                                       const std::string& left_label,
                                       const std::string& right_ref,
                                       const std::string& right_label) {
    return shell.OpenBranchHeadComparison(path, left_ref, left_label, right_ref, right_label);
  }
  static bool OpenMergeEditor(WorkspaceShell& shell,
                              const std::filesystem::path& base_path,
                              const std::filesystem::path& incoming_path,
                              const std::filesystem::path& current_path,
                              const std::filesystem::path& output_path) {
    return shell.OpenMergeEditor(base_path, incoming_path, current_path, output_path);
  }
  static bool OpenProjectTab(WorkspaceShell& shell,
                             const std::filesystem::path& project_root,
                             bool restore_persistence = false,
                             bool log_feedback = false) {
    return shell.OpenProjectTab(project_root, restore_persistence, log_feedback);
  }
  static void SetWindowSize(WorkspaceShell& shell, int width, int height) {
    shell.window_presentation_.logical_width = width;
    shell.window_presentation_.logical_height = height;
  }
  static void SetWindowChromeEnabled(WorkspaceShell& shell,
                                     bool enabled,
                                     bool maximized = false,
                                     bool fullscreen = false) {
    shell.window_presentation_.chrome = WorkspaceShell::WindowChromeState{
        .custom_enabled = enabled,
        .maximized = maximized,
        .fullscreen = fullscreen,
    };
  }
  static void RenderFrame(WorkspaceShell& shell) {
    shell.Render(nullptr, shell.window_presentation_.logical_width,
                 shell.window_presentation_.logical_height);
  }
  static bool ExecuteProjectOpenFromMenu(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::ProjectOpen, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteProjectOpenFromCommand(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::ProjectOpen, {}, WorkspaceShell::ActionSource::Command);
  }
  static void ResetProjectScopedState(WorkspaceShell& shell, bool show_welcome) {
    shell.ResetProjectScopedState(show_welcome);
  }
  static bool ExecuteFilesFromShortcut(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(WorkspaceShell::ActionId::Files, {},
                                            WorkspaceShell::ActionSource::Shortcut);
  }
  static void SetClipboardTextReader(
      WorkspaceShell& shell,
      std::function<std::optional<std::string>()> reader) {
    shell.clipboard_text_reader_ = std::move(reader);
  }
  static void SetClipboardTextWriter(WorkspaceShell& shell,
                                     std::function<bool(std::string_view)> writer) {
    shell.clipboard_text_writer_ = std::move(writer);
  }
  static void SetPrimarySelectionTextReader(
      WorkspaceShell& shell,
      std::function<std::optional<std::string>()> reader) {
    shell.primary_selection_text_reader_ = std::move(reader);
  }
  static void SetPrimarySelectionTextWriter(
      WorkspaceShell& shell,
      std::function<bool(std::string_view)> writer) {
    shell.primary_selection_text_writer_ = std::move(writer);
  }
  static void SetExternalUrlOpener(WorkspaceShell& shell,
                                   std::function<bool(std::string_view)> opener) {
    shell.external_url_opener_ = std::move(opener);
  }
  static void SetProjectOpenDialogLauncher(
      WorkspaceShell& shell,
      std::function<bool(WorkspaceShell&, const std::filesystem::path&)> launcher) {
    shell.project_dialog_state_.launcher = std::move(launcher);
  }
  static void QueueProjectOpenDialogSelection(WorkspaceShell& shell,
                                              const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(shell.project_dialog_state_.mutex);
    shell.project_dialog_state_.pending_result = microide::workspace::PendingProjectOpenDialogResult{
        .ready = true,
        .cancelled = false,
        .selected_path = path.lexically_normal(),
        .error_message = {},
    };
  }
  static void QueueProjectOpenDialogCancel(WorkspaceShell& shell) {
    std::lock_guard<std::mutex> lock(shell.project_dialog_state_.mutex);
    shell.project_dialog_state_.pending_result = microide::workspace::PendingProjectOpenDialogResult{
        .ready = true,
        .cancelled = true,
        .selected_path = {},
        .error_message = {},
    };
  }
  static void ConsumePendingProjectOpenDialogResult(WorkspaceShell& shell) {
    shell.ConsumePendingProjectOpenDialogResult();
  }
  static bool SwitchProject(WorkspaceShell& shell,
                            std::size_t index,
                            bool log_feedback = false) {
    return shell.SwitchProject(index, log_feedback);
  }
  static void RequestCloseProject(WorkspaceShell& shell, std::size_t index) {
    shell.RequestCloseProject(index);
  }
  static void CloseProject(WorkspaceShell& shell, std::size_t index) { shell.CloseProject(index); }
  static void OpenFile(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.OpenFile(path);
  }
  static bool OpenFileInNewTab(WorkspaceShell& shell, const std::filesystem::path& path) {
    return shell.OpenFileInNewTab(path);
  }
  static void RegisterVirtualDocument(WorkspaceShell& shell,
                                      const microide::workspace::VirtualDocumentSpec& spec) {
    shell.virtual_document_registry_.Register(spec);
  }
  static void UpdateVirtualDocumentContent(WorkspaceShell& shell,
                                           std::string_view uri,
                                           std::string content) {
    shell.virtual_document_registry_.UpdateContent(std::string(uri), content);
  }
  static bool OpenVirtualDocument(WorkspaceShell& shell, std::string_view uri) {
    return shell.OpenVirtualDocumentInNewTab(uri);
  }
  static bool ExecuteCommandLine(WorkspaceShell& shell, const std::string& command_line) {
    return shell.MakeCommandPromptCoordinator().ExecuteCommandLine(command_line);
  }
  static const std::vector<std::string>& PluginMessages(const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().Messages();
  }
  static const std::vector<std::string>& PluginErrors(const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().Errors();
  }
  static void ClearPluginMessages(WorkspaceShell& shell) {
    shell.plugin_runtime_.Host().ClearMessages();
  }
  static void SetPluginAssetPollInterval(WorkspaceShell& shell,
                                         std::chrono::milliseconds poll_interval) {
    shell.plugin_runtime_.SetPollInterval(poll_interval);
  }
  static void RegisterLifecycleWakeEvents(WorkspaceShell& shell) {
    shell.RegisterLifecycleWakeEvents();
  }
  static Uint32 ProjectFileEventType(const WorkspaceShell& shell) {
    return shell.project_file_event_type_;
  }
  static std::optional<Uint32> NextAnimationDelayMs(const WorkspaceShell& shell) {
    return shell.NextAnimationDelayMs();
  }
  static bool ReloadProjectIfFilesChanged(WorkspaceShell& shell, bool force_check) {
    return shell.ReloadProjectIfFilesChanged(force_check);
  }
  static WorkspaceShell::EventResult HandleScheduledWake(WorkspaceShell& shell) {
    return shell.HandleScheduledWake();
  }
  static const std::vector<std::string>* OutputChannelEntries(const WorkspaceShell& shell,
                                                              std::string_view id) {
    return shell.OutputChannelEntries(id);
  }
  static WorkspaceShell::PanelContentKind PanelContent(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.content;
  }
  static std::string ChatComposerInput(const WorkspaceShell& shell) {
    return shell.ChatComposerText();
  }
  static std::vector<std::string> ChatTranscriptRows(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ChatTranscriptDebugLines(layout.sidebar);
  }
  static std::optional<SDL_FPoint> ChatTranscriptLinkPoint(WorkspaceShell& shell,
                                                           std::string_view match) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto rect = shell.FindChatTranscriptLinkRect(layout.sidebar, match);
    if (!rect.has_value()) {
      return std::nullopt;
    }
    return SDL_FPoint{
        .x = rect->x + rect->w * 0.5f,
        .y = rect->y + rect->h * 0.5f,
    };
  }
  static WorkspaceShell::OverlayMode ActiveOverlayMode(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.mode;
  }
  static const WorkspaceShell::CompletionSessionState& CompletionSession(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.completion;
  }
  static const WorkspaceShell::CodeActionSessionState& CodeActionSession(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.code_actions;
  }
  static const WorkspaceShell::TaskPickerState& TaskPicker(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.task_picker;
  }
  static const WorkspaceShell::InlineCompletionState& InlineCompletion(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.inline_completion;
  }
  static bool AcceptInlineCompletion(WorkspaceShell& shell) {
    return shell.AcceptInlineCompletion();
  }
  static void DismissInlineCompletion(WorkspaceShell& shell) {
    shell.DismissInlineCompletion();
  }
  static std::vector<Message> ActiveConversationMessages(const WorkspaceShell& shell) {
    const auto* conversation = shell.context_.current_project_state.conversations.GetConversation(
        shell.context_.current_project_state.panel.chat.conversation_id);
    return conversation != nullptr ? conversation->messages : std::vector<Message>{};
  }
  static std::string ActiveConversationProviderId(const WorkspaceShell& shell) {
    const auto* conversation = shell.context_.current_project_state.conversations.GetConversation(
        shell.context_.current_project_state.panel.chat.conversation_id);
    return conversation != nullptr ? conversation->provider_id : std::string{};
  }
  static bool ActivateChatConversation(WorkspaceShell& shell, std::string_view conversation_id) {
    return shell.ActivateChatConversation(conversation_id);
  }
  static bool CreateChatConversation(WorkspaceShell& shell) {
    return shell.CreateChatConversation();
  }
  static bool DeleteActiveChatConversation(WorkspaceShell& shell) {
    return shell.DeleteActiveChatConversation();
  }
  static std::string ActiveConversationId(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.chat.conversation_id;
  }
  static bool CancelActiveChatRequest(WorkspaceShell& shell) {
    return shell.CancelActiveChatRequest();
  }
  static bool RetryActiveChatRequest(WorkspaceShell& shell, std::string* error_message = nullptr) {
    return shell.RetryActiveChatRequest(error_message);
  }
  static WorkspaceShell::ProjectWorkspaceState& CurrentProjectState(WorkspaceShell& shell) {
    return shell.context_.current_project_state;
  }
  static WorkspaceShell::ProjectWorkspaceState& ProjectState(WorkspaceShell& shell,
                                                             std::size_t index) {
    return *shell.context_.project_catalog.entries.at(index);
  }
  static const std::vector<AiProviderSpec>& AiProviders(const WorkspaceShell& shell) {
    return shell.ai_provider_registry_.Specs();
  }
  static bool SetProviderApiKey(WorkspaceShell& shell,
                                std::string_view provider_id,
                                std::string_view api_key,
                                std::string* error_message = nullptr) {
    return shell.SetProviderApiKey(provider_id, api_key, error_message);
  }
  static bool ClearProviderApiKey(WorkspaceShell& shell,
                                  std::string_view provider_id,
                                  std::string* error_message = nullptr) {
    return shell.ClearProviderApiKey(provider_id, error_message);
  }
  static ProviderAuthStatus GetProviderAuthStatus(const WorkspaceShell& shell,
                                                  std::string_view provider_id) {
    return shell.GetProviderAuthStatus(provider_id);
  }
  static void RequestProviderAuthCheck(WorkspaceShell& shell, std::string_view provider_id) {
    shell.provider_bridge_manager_.RequestAuthCheck(std::string(provider_id));
  }
  static void RequestProviderModelList(WorkspaceShell& shell, std::string_view provider_id) {
    shell.provider_bridge_manager_.RequestModelList(std::string(provider_id));
  }
  static std::vector<std::string> ProviderModels(const WorkspaceShell& shell,
                                                 std::string_view provider_id) {
    return shell.provider_bridge_manager_.GetModels(std::string(provider_id));
  }
  static ProviderCapabilities GetProviderCapabilities(const WorkspaceShell& shell,
                                                      std::string_view provider_id) {
    return shell.provider_bridge_manager_.GetCapabilities(std::string(provider_id));
  }
  static void ConsumeProviderBridgeUpdates(WorkspaceShell& shell) {
    shell.ConsumeProviderBridgeUpdates();
  }
  static void ConsumeLspCallbacks(WorkspaceShell& shell) { shell.ConsumeLspCallbacks(); }
  static void ConsumePluginAsyncProcessCallbacks(WorkspaceShell& shell) {
    shell.ConsumePluginAsyncProcessCallbacks();
  }
  static bool WaitForPluginAsyncProcessEventLoop(WorkspaceShell& shell, int timeout_ms = 5000) {
    const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(std::max(timeout_ms, 0));
    while (shell.plugin_runtime_.PendingAsyncProcessCount() > 0 &&
           SDL_GetTicks() <= deadline) {
      SDL_Event event;
      if (SDL_WaitEventTimeout(&event, 10)) {
        shell.HandleEvent(event);
        while (SDL_PollEvent(&event)) {
          shell.HandleEvent(event);
        }
      } else {
        shell.HandleScheduledWake();
      }
    }
    shell.ConsumePluginAsyncProcessCallbacks();
    return shell.plugin_runtime_.PendingAsyncProcessCount() == 0;
  }
  static bool WaitForPluginAsyncProcessCallbacks(WorkspaceShell& shell, int timeout_ms = 5000) {
    const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(std::max(timeout_ms, 0));
    while (shell.plugin_runtime_.Host().PendingAsyncProcessCount() > 0 &&
           SDL_GetTicks() <= deadline) {
      shell.ConsumePluginAsyncProcessCallbacks();
      SDL_Delay(5);
    }
    shell.ConsumePluginAsyncProcessCallbacks();
    return shell.plugin_runtime_.Host().PendingAsyncProcessCount() == 0;
  }
  static bool WaitForProviderBridgeIdle(WorkspaceShell& shell, int timeout_ms = 2000) {
    const auto any_in_flight = [&]() {
      const auto project_busy = [](const WorkspaceShell::ProjectWorkspaceState& project) {
        return project.panel.chat.request_in_flight || project.inline_completion.request_in_flight;
      };
      if (project_busy(shell.context_.current_project_state)) {
        return true;
      }
      return std::any_of(shell.context_.project_catalog.entries.begin(),
                         shell.context_.project_catalog.entries.end(),
                         [&](const auto& entry) {
                           return entry != nullptr && project_busy(*entry);
                         });
    };
    const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(std::max(timeout_ms, 0));
    while (any_in_flight() && SDL_GetTicks() <= deadline) {
      shell.ConsumeProviderBridgeUpdates();
      shell.HandleScheduledWake();
      SDL_Delay(5);
    }
    shell.ConsumeProviderBridgeUpdates();
    return !any_in_flight();
  }
  static void ConsumeTaskRuntimeUpdates(WorkspaceShell& shell) { shell.ConsumeTaskRuntimeUpdates(); }
  static bool WaitForTaskRuntimeIdle(WorkspaceShell& shell, int timeout_ms = 2000) {
    const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(std::max(timeout_ms, 0));
    while (shell.task_runtime_.active_run_id() != 0 && SDL_GetTicks() <= deadline) {
      shell.ConsumeTaskRuntimeUpdates();
      SDL_Delay(5);
    }
    shell.ConsumeTaskRuntimeUpdates();
    return shell.task_runtime_.active_run_id() == 0;
  }
  static const std::vector<editor::PublishedDiagnostic>* DiagnosticsForPath(
      const WorkspaceShell& shell,
      const std::filesystem::path& path) {
    return shell.context_.current_project_state.diagnostics_store.FindByPath(path);
  }
  static const std::vector<microide::workspace::ScmProviderSpec>& ScmProviders(
      const WorkspaceShell& shell) {
    return shell.scm_registry_.Specs();
  }
  static const std::vector<microide::workspace::AnnotationProviderSpec>& AnnotationProviders(
      const WorkspaceShell& shell) {
    return shell.annotation_registry_.Specs();
  }
  static std::vector<microide::workspace::AuthSession> AuthSessions(
      const WorkspaceShell& shell,
      std::string_view provider_id) {
    return shell.auth_provider_registry_.GetSessions(std::string(provider_id));
  }
  static const microide::workspace::AuthProviderSpec* AuthProvider(
      const WorkspaceShell& shell,
      std::string_view provider_id) {
    return shell.auth_provider_registry_.GetProvider(std::string(provider_id));
  }
  static bool PublishDiagnostics(WorkspaceShell& shell,
                                 std::string_view owner,
                                 const std::filesystem::path& path,
                                 std::vector<editor::Diagnostic> diagnostics) {
    return shell.context_.current_project_state.diagnostics_store.ReplaceForOwnerFile(owner, path, std::move(diagnostics));
  }
  static bool ExecuteCopySelectionWithContext(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopySelectionWithContext, {},
        WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopySelection(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopySelection, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteSelectAll(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::SelectAll, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopyRelativePath(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopyRelativePath, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopyAbsolutePath(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopyAbsolutePath, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecutePasteClipboard(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::PasteClipboard, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopyLastTerminalCommand(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CopyLastTerminalCommand, {},
        WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseAllTabs(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CloseAllTabs, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseOtherTabs(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CloseOtherTabs, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseTabsToRight(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CloseTabsToRight, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseTabsToLeft(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::CloseTabsToLeft, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteTreeRefresh(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::TreeRefresh, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteGitRefresh(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::GitRefresh, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteShowGitSidebar(WorkspaceShell& shell) {
    return ActionCoordinator(shell.MakeActionContext()).Execute(
        WorkspaceShell::ActionId::SidebarShow, {"git"}, WorkspaceShell::ActionSource::Menu);
  }
  static bool SaveTab(WorkspaceShell& shell, std::size_t index) { return shell.SaveTab(index); }
  static void ActivateTab(WorkspaceShell& shell, std::size_t index) { shell.ActivateTab(index); }
  static bool SelectTreePath(WorkspaceShell& shell, const std::filesystem::path& path) {
    return shell.context_.current_project_state.directory_tree.SelectPath(path);
  }
  static void CollapseTreeSelection(WorkspaceShell& shell) { shell.context_.current_project_state.directory_tree.CollapseSelection(); }
  static void ShowSearchSidebar(WorkspaceShell& shell,
                                std::string query,
                                bool temporary = false) {
    shell.ShowSearchSidebar(std::move(query), temporary);
  }
  static void ShowProblemsSidebar(WorkspaceShell& shell) { shell.ShowProblemsSidebar(); }
  static void ShowGitSidebar(WorkspaceShell& shell) { shell.ShowGitSidebar(); }
  static void ShowTestsSidebar(WorkspaceShell& shell) { shell.ShowTestsSidebar(); }
  static bool RefreshProblemsSidebar(WorkspaceShell& shell) {
    return shell.RefreshProblemsSidebar();
  }
  static void RefreshGitSidebar(WorkspaceShell& shell) { shell.RefreshGitSidebar(); }
  static std::vector<std::string> GitSidebarSummaryLines(const WorkspaceShell& shell) {
    return shell.GitSidebarSummaryLines();
  }
  static const std::vector<WorkspaceShell::ProblemsSidebarEntry>& ProblemsSidebarEntries(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.problems.entries;
  }
  static const std::vector<WorkspaceShell::TestsSidebarEntry>& TestsSidebarEntries(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.tests.entries;
  }
  static const std::vector<WorkspaceShell::GitSidebarEntry>& GitSidebarEntries(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.entries;
  }
  static void DismissOverlay(WorkspaceShell& shell, bool focus_editor = false) {
    shell.DismissOverlay(focus_editor);
  }
  static bool ApplySelectedCompletion(WorkspaceShell& shell) {
    return shell.ApplySelectedCompletion();
  }
  static bool ExecuteSelectedCodeAction(WorkspaceShell& shell) {
    return shell.ExecuteSelectedCodeAction();
  }
  static void ConsumeProjectSearchUpdates(WorkspaceShell& shell) {
    shell.ConsumeProjectSearchUpdates();
  }
  static void ToggleProjectSearchPatternMode(WorkspaceShell& shell) {
    shell.ToggleProjectSearchPatternMode();
  }
  static void CycleProjectSearchCaseMode(WorkspaceShell& shell) {
    shell.CycleProjectSearchCaseMode();
  }
  static void ToggleProjectSearchHiddenFiles(WorkspaceShell& shell) {
    shell.ToggleProjectSearchHiddenFiles();
  }
  static void EnsureTerminalTab(WorkspaceShell& shell) {
    if (shell.context_.current_project_state.terminal_tabs.empty()) {
      shell.context_.current_project_state.terminal_tabs.push_back(std::make_unique<WorkspaceShell::TerminalTabState>());
    }
    shell.context_.current_project_state.active_terminal_tab_index = shell.context_.current_project_state.terminal_tabs.size() - 1;
    shell.context_.current_project_state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void ShowChatPanel(WorkspaceShell& shell) { shell.ShowChatPanel(); }
  static void AddTerminalTab(WorkspaceShell& shell) {
    shell.context_.current_project_state.terminal_tabs.push_back(std::make_unique<WorkspaceShell::TerminalTabState>());
    shell.context_.current_project_state.active_terminal_tab_index = shell.context_.current_project_state.terminal_tabs.size() - 1;
    shell.context_.current_project_state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void ConsumeTerminalSessionUpdates(WorkspaceShell& shell) {
    shell.ConsumeTerminalSessionUpdates();
  }
  static microide::terminal::TerminalSession& ActiveTerminalSession(WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->session;
  }
  static void SetActiveTerminalFollowTail(WorkspaceShell& shell, bool follow_tail) {
    shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->follow_tail = follow_tail;
  }
  static bool ActiveTerminalFollowTail(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->follow_tail;
  }
  static void SetActiveTerminalScrollRow(WorkspaceShell& shell, int scroll_row) {
    shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->scroll_row = scroll_row;
  }
  static int ActiveTerminalScrollRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.terminal_tabs[shell.context_.current_project_state.active_terminal_tab_index]->scroll_row;
  }
  static bool HandleTerminalKeyDown(WorkspaceShell& shell,
                                    SDL_Keycode key,
                                    SDL_Keymod modifiers) {
    SDL_KeyboardEvent event{};
    event.key = key;
    return shell.MakeTextInputCoordinator().HandleTerminalKeyDown(event, modifiers);
  }
  static bool HandleTextInput(WorkspaceShell& shell, std::string_view text) {
    SDL_TextInputEvent event{};
    const std::string storage(text);
    event.text = storage.c_str();
    return shell.MakeTextInputCoordinator().HandleTextInput(event);
  }
  static bool HandleKeyEvent(WorkspaceShell& shell, SDL_Keycode key, SDL_Keymod modifiers) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.mod = modifiers;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleWindowMouseLeave(WorkspaceShell& shell) {
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleWindowFocusEvent(WorkspaceShell& shell, bool focused) {
    SDL_Event event{};
    event.type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseButtonDown(WorkspaceShell& shell, float x, float y, Uint8 button) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseButtonDown(WorkspaceShell& shell,
                                    float x,
                                    float y,
                                    Uint8 button,
                                    Uint8 clicks) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    event.button.clicks = clicks;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseButtonUp(WorkspaceShell& shell, float x, float y, Uint8 button) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseMotion(WorkspaceShell& shell, float x, float y, SDL_MouseButtonFlags state) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.state = state;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseWheel(WorkspaceShell& shell,
                               float x,
                               float y,
                               int vertical_ticks,
                               int horizontal_ticks = 0) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.mouse_x = x;
    event.wheel.mouse_y = y;
    event.wheel.integer_x = horizontal_ticks;
    event.wheel.integer_y = vertical_ticks;
    event.wheel.x = static_cast<float>(horizontal_ticks);
    event.wheel.y = static_cast<float>(vertical_ticks);
    return shell.HandleEvent(event).handled;
  }
  static int ProjectTabScrollIndex(const WorkspaceShell& shell) {
    return shell.context_.project_catalog.tab_scroll_index;
  }
  static int EditorTabScrollIndex(const WorkspaceShell& shell) { return shell.context_.current_project_state.tab_scroll_index; }
  static SDL_FRect BottomPanelContentRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return microide::workspace::BottomPanelContentRect(layout, shell.context_.current_project_state.panel.command_mode);
  }
  static SDL_FPoint TerminalCellPoint(WorkspaceShell& shell,
                                      std::size_t row,
                                      std::size_t column) {
    const auto* terminal_tab = shell.ActiveTerminalTab();
    const auto lines = terminal_tab != nullptr ? terminal_tab->session.SnapshotLines()
                                               : std::vector<microide::terminal::TerminalLine>{};
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto panel_layout = shell.ComputeBottomPanelLogLayout(layout, lines.size());
    return SDL_FPoint{
        .x = panel_layout.text_x +
             static_cast<float>(column) * std::max(1.0f, shell.text_renderer_.CharWidth()) + 1.0f,
        .y = panel_layout.text_y + static_cast<float>(row) * panel_layout.line_height +
             panel_layout.line_height * 0.5f,
    };
  }
  static SDL_FPoint BottomPanelTextOrigin(WorkspaceShell& shell) {
    const auto* terminal_tab = shell.ActiveTerminalTab();
    const std::size_t line_count =
        terminal_tab != nullptr ? terminal_tab->session.LineCount() : 0;
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto panel_layout = shell.ComputeBottomPanelLogLayout(layout, line_count);
    return SDL_FPoint{.x = panel_layout.text_x, .y = panel_layout.text_y};
  }
  static bool TerminalHasSelection(const WorkspaceShell& shell) {
    return shell.TerminalHasSelection();
  }
  static std::string ActiveTerminalSelectedText(WorkspaceShell& shell) {
    return shell.SelectedTerminalText();
  }
  static SDL_FRect ProjectSearchResultRect(WorkspaceShell& shell, std::size_t result_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto line_map = shell.BuildProjectSearchLineMap();
    const auto list_layout =
        shell.ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
    const int line_index = shell.ProjectSearchLineForResult(result_index);
    return ScrollableListRowRect(list_layout, line_index - list_layout.scroll_row);
  }
  static SDL_FRect ProjectTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
  static std::string ProjectTabTooltipLabel(WorkspaceShell& shell, std::size_t index) {
    return shell.ProjectTabTooltipLabel(index);
  }
  static WorkspaceShell::VisibleStripTab::ChatStatus ProjectTabChatStatus(WorkspaceShell& shell,
                                                                          std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.chat_status;
      }
    }
    return WorkspaceShell::VisibleStripTab::ChatStatus::None;
  }
  static std::string ProjectTabBadgeText(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.badge_text;
      }
    }
    return {};
  }
  static bool ProjectTabShowsBadge(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.show_badge;
      }
    }
    return false;
  }
  static SDL_FRect EditorTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab : shell.ComputeVisibleTabs(layout.tab_strip)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
  static std::string TabDisplayTitle(WorkspaceShell& shell, std::size_t index) {
    return shell.TabDisplayTitle(index);
  }
  static std::string TabTooltipLabel(WorkspaceShell& shell, std::size_t index) {
    return shell.TabTooltipLabel(index);
  }
  static std::string HoveredTabTooltipLabel(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredTabTooltipLabel(layout.tab_strip);
  }
  static std::optional<SDL_FRect> HoveredTabTooltipRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredTabTooltipRect(layout);
  }
  static std::vector<WorkspaceShell::VisibleStatusItem> VisibleStatusItems(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeVisibleStatusItems(layout.breadcrumb);
  }
  static std::string HoveredStatusTooltipLabel(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredStatusTooltip(layout.breadcrumb);
  }
  static std::string HoveredGitSidebarTooltipLabel(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredGitSidebarTooltipLabel(layout.sidebar);
  }
  static std::optional<SDL_FRect> HoveredGitSidebarTooltipRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const std::string label = shell.HoveredGitSidebarTooltipLabel(layout.sidebar);
    if (label.empty()) {
      return std::nullopt;
    }
    const auto tooltip = detail::BuildTooltipLayout(
        shell.text_renderer_, label, std::max(160.0f, layout.full.w - 24.0f));
    const float tooltip_x =
        std::clamp(shell.last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                   layout.full.x + layout.full.w - tooltip.rect.w - 8.0f);
    const float tooltip_y =
        shell.last_mouse_y_ - tooltip.rect.h - 10.0f >= layout.full.y + 8.0f
            ? shell.last_mouse_y_ - tooltip.rect.h - 10.0f
            : std::clamp(shell.last_mouse_y_ + 14.0f, layout.full.y + 8.0f,
                         layout.full.y + layout.full.h - tooltip.rect.h - 8.0f);
    return MakeRect(tooltip_x, tooltip_y, tooltip.rect.w, tooltip.rect.h);
  }
  static std::array<SDL_FRect, 3> GitSidebarTopActionRects(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return {shell.GitSidebarStageAllButtonRect(layout.sidebar),
            shell.GitSidebarDiscardAllButtonRect(layout.sidebar),
            shell.GitSidebarRefreshButtonRect(layout.sidebar)};
  }
  static std::array<SDL_FRect, 2> GitSidebarEntryActionRects(WorkspaceShell& shell,
                                                             std::size_t entry_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto lines = shell.BuildGitSidebarLines();
    const auto list_layout = shell.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].entry_index < 0 || static_cast<std::size_t>(lines[i].entry_index) != entry_index) {
        continue;
      }
      const SDL_FRect row_rect = ScrollableListRowRect(
          list_layout, static_cast<int>(i) - list_layout.scroll_row);
      const auto actions = shell.ComputeGitSidebarEntryActionLayout(
          row_rect, shell.context_.current_project_state.sidebar.git.entries[entry_index]);
      return {actions.primary_rect.value_or(SDL_FRect{}),
              actions.discard_rect.value_or(SDL_FRect{})};
    }
    return {};
  }
  static SDL_FRect GitSidebarEntryRowRect(WorkspaceShell& shell, std::size_t entry_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto lines = shell.BuildGitSidebarLines();
    const auto list_layout = shell.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].entry_index < 0 || static_cast<std::size_t>(lines[i].entry_index) != entry_index) {
        continue;
      }
      return ScrollableListRowRect(
          list_layout, static_cast<int>(i) - list_layout.scroll_row);
    }
    return {};
  }
  static SDL_FRect ActiveEditorPaneRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.ActiveTabIsEditor()) {
      return {};
    }
    shell.SyncActiveEditorTab();
    auto* editor_tab = shell.ActiveEditorTab();
    if (editor_tab == nullptr) {
      return {};
    }
    shell.NormalizeEditorSplitTree(*editor_tab);
    const auto panes = shell.ComputeEditorPaneLayouts(layout.editor_surface);
    for (const auto& pane : panes) {
      if (pane.active) {
        return pane.rect;
      }
    }
    return layout.editor_surface;
  }
  static SDL_FRect InactiveEditorPaneRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.ActiveTabIsEditor()) {
      return {};
    }
    shell.SyncActiveEditorTab();
    auto* editor_tab = shell.ActiveEditorTab();
    if (editor_tab == nullptr) {
      return {};
    }
    shell.NormalizeEditorSplitTree(*editor_tab);
    const auto panes = shell.ComputeEditorPaneLayouts(layout.editor_surface);
    for (const auto& pane : panes) {
      if (!pane.active) {
        return pane.rect;
      }
    }
    return {};
  }
  static microide::editor::EditorViewMetrics ActiveEditorMetrics(WorkspaceShell& shell) {
    const SDL_FRect pane = ActiveEditorPaneRect(shell);
    return microide::editor::EditorViewRenderer::ComputeMetrics(shell.text_renderer_,
                                                                *shell.ActiveEditorViewport(),
                                                                pane);
  }
  static std::optional<SDL_FRect> ActiveEditorLineRangeRect(WorkspaceShell& shell,
                                                            std::size_t start_line,
                                                            std::size_t end_line) {
    return shell.CurrentEditorLineRangeRect(start_line, end_line);
  }
  static std::optional<SDL_FRect> ActiveEditorLineToBottomRect(WorkspaceShell& shell,
                                                               std::size_t start_line) {
    return shell.CurrentEditorLineToBottomRect(start_line);
  }
  static std::optional<SDL_FRect> ActiveCompareRowRangeRect(WorkspaceShell& shell,
                                                            std::size_t start_row,
                                                            std::size_t end_row) {
    return shell.CurrentCompareRowRangeRect(start_row, end_row);
  }
  static std::optional<SDL_FRect> ActiveCompareRowToBottomRect(WorkspaceShell& shell,
                                                               std::size_t start_row) {
    return shell.CurrentCompareRowToBottomRect(start_row);
  }
  static std::optional<SDL_FRect> ActiveMergeConflictRect(WorkspaceShell& shell,
                                                          std::size_t conflict_index) {
    return shell.CurrentMergeConflictRect(conflict_index);
  }
  static std::optional<SDL_FRect> ActiveMergeResultLineToBottomRect(WorkspaceShell& shell,
                                                                    std::size_t start_line) {
    return shell.CurrentMergeResultLineToBottomRect(start_line);
  }
  static float TextCharWidth(WorkspaceShell& shell) { return shell.text_renderer_.CharWidth(); }
  static float TextLineHeight(WorkspaceShell& shell) { return shell.text_renderer_.LineHeight(); }
  static SDL_HitTestResult WindowHitTest(WorkspaceShell& shell, float x, float y) {
    return shell.WindowHitTest(x, y);
  }
  static bool WindowDragRegionContains(WorkspaceShell& shell, float x, float y) {
    return shell.WindowDragRegionContains(x, y);
  }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveEditorBlameOverlay(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.ActiveTabIsEditor()) {
      return std::nullopt;
    }
    auto* editor_tab = shell.ActiveEditorTab();
    if (editor_tab == nullptr) {
      return std::nullopt;
    }
    shell.NormalizeEditorSplitTree(*editor_tab);
    microide::editor::TextViewport* viewport = shell.ActiveEditorViewport();
    if (viewport == nullptr) {
      return std::nullopt;
    }
    const auto panes = shell.ComputeEditorPaneLayouts(layout.editor_surface);
    for (const auto& pane : panes) {
      if (pane.active) {
        return shell.BuildEditorBlameOverlay(*viewport, pane.rect);
      }
    }
    return viewport->is_placeholder()
               ? shell.BuildEditorBlameOverlay(*viewport, layout.editor_surface)
               : std::nullopt;
  }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveCompareBlameOverlay(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.ActiveTabIsCompare()) {
      return std::nullopt;
    }
    auto& compare = ActiveCompare(shell);
    return shell.BuildCompareBlameOverlay(compare, shell.ComputeCompareSurfaceLayout(layout.editor_surface, compare),
                                          layout.editor_surface);
  }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveMergeBlameOverlay(
      WorkspaceShell& shell) {
    if (!shell.ActiveTabIsMerge()) {
      return std::nullopt;
    }
    auto& merge = ActiveMerge(shell);
    return shell.BuildEditorBlameOverlay(merge.result_viewport, ActiveMergeResultRect(shell), 280.0f);
  }
  static void SetVisibleEditorBlameOverlay(
      WorkspaceShell& shell,
      std::optional<microide::editor::EditorBlameOverlay> overlay) {
    shell.visible_editor_blame_overlay_ = std::move(overlay);
  }
  static std::optional<SDL_FRect> ActiveEditorHoverPopupRect(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorHoverPopupLayout();
    return popup.has_value() ? std::make_optional(popup->rect) : std::nullopt;
  }
  static std::optional<std::string> ActiveEditorDiagnosticHoverMessage(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorHoverPopupLayout();
    return popup.has_value() && popup->diagnostic.has_value()
               ? std::make_optional(popup->diagnostic->message)
               : std::nullopt;
  }
  static std::optional<std::string> ActiveEditorPluginHoverContent(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorHoverPopupLayout();
    return popup.has_value() && popup->plugin_hover.has_value()
               ? std::make_optional(popup->plugin_hover->content)
               : std::nullopt;
  }
  static std::optional<SDL_FRect> ActiveEditorBlamePopupRect(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorBlamePopupLayout();
    return popup.has_value() ? std::make_optional(popup->rect) : std::nullopt;
  }
  static std::optional<SDL_FRect> ActiveEditorBlamePopupCopyShaRect(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorBlamePopupLayout();
    return popup.has_value() ? std::make_optional(popup->copy_sha_rect) : std::nullopt;
  }
  static SDL_FRect ActiveTerminalTabRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    for (const WorkspaceShell::VisibleStripTab& tab : shell.ComputeVisibleTerminalTabs(panel_header)) {
      if (tab.index == shell.context_.current_project_state.active_terminal_tab_index) {
        return tab.rect;
      }
    }
    return {};
  }
  static std::vector<std::string> BottomPanelTabDisplayTitles(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    std::vector<std::string> labels;
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleBottomPanelTabs(panel_header)) {
      labels.push_back(tab.display_title);
    }
    return labels;
  }
  static std::optional<SDL_FRect> BottomPanelTabRectByTitle(WorkspaceShell& shell,
                                                             std::string_view title) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleBottomPanelTabs(panel_header)) {
      if (tab.display_title == title) {
        return tab.rect;
      }
    }
    return std::nullopt;
  }
  static SDL_FRect TerminalTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    for (const WorkspaceShell::VisibleStripTab& tab : shell.ComputeVisibleTerminalTabs(panel_header)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
  static bool RestoreSessionState(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreSessionState();
  }
  static bool RestoreUserConfig(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreUserConfig();
  }
  static bool RestoreConfigState(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreConfigState();
  }
  static void SaveSessionState(WorkspaceShell& shell) {
    shell.MakePersistenceCoordinator().SaveSessionState();
  }
  static bool RestoreWorkspaceSession(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreWorkspaceSession();
  }
  static void SaveWorkspaceSession(WorkspaceShell& shell) {
    shell.MakePersistenceCoordinator().SaveWorkspaceSession();
  }
  static void RequestQuit(WorkspaceShell& shell) { shell.RequestQuit(); }
  static bool ConsumeQuitRequested(WorkspaceShell& shell) { return shell.ConsumeQuitRequested(); }
  static WorkspaceShell::WindowAction ConsumeWindowAction(WorkspaceShell& shell) {
    return shell.ConsumeWindowAction();
  }

  static void ConfirmDirtyPrompt(WorkspaceShell& shell, int selected_action) {
    shell.context_.prompts.dirty.selected_action = selected_action;
    shell.ConfirmDirtyPrompt();
  }
  static bool StageAllGitSidebarEntries(WorkspaceShell& shell) {
    return shell.StageAllGitSidebarEntries();
  }
  static bool DiscardAllGitSidebarEntries(WorkspaceShell& shell) {
    return shell.DiscardAllGitSidebarEntries();
  }

  static bool DirtyPromptVisible(const WorkspaceShell& shell) { return shell.context_.prompts.dirty_visible; }
  static std::string DirtyPromptMessage(const WorkspaceShell& shell) {
    return shell.DirtyPromptMessage();
  }
  static bool PromptSurfaceVisible(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface_visible;
  }
  static SDL_FRect PromptSurfaceInputRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return ComputePromptSurfaceInputRect(ComputePromptSurfaceRect(layout.full));
  }
  static const std::string& PromptSurfaceInput(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface.input.text();
  }
  static std::string PromptSurfaceTitle(const WorkspaceShell& shell) {
    return shell.PromptSurfaceTitle();
  }
  static std::string PromptSurfaceMessage(const WorkspaceShell& shell) {
    return shell.PromptSurfaceMessage();
  }
  static std::string PromptSurfaceDetail(const WorkspaceShell& shell) {
    return shell.PromptSurfaceDetail();
  }
  static int PromptSurfaceButtonCount(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface.button_count;
  }
  static const std::vector<WorkspaceShell::TabEntry>& OpenTabs(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.open_tabs;
  }
  static std::size_t ActiveTabIndex(const WorkspaceShell& shell) { return shell.context_.current_project_state.active_tab_index; }
  static std::size_t ActiveTerminalTabIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.active_terminal_tab_index;
  }
  static std::vector<std::filesystem::path> ProjectRoots(const WorkspaceShell& shell) {
    std::vector<std::filesystem::path> roots;
    roots.reserve(shell.context_.project_catalog.entries.size());
    for (std::size_t i = 0; i < shell.context_.project_catalog.entries.size(); ++i) {
      roots.push_back(shell.ProjectCatalogRoot(i));
    }
    return roots;
  }
  static std::vector<std::string> TerminalLaunchLabels(WorkspaceShell& shell) {
    std::vector<std::string> labels;
    labels.reserve(shell.context_.current_project_state.terminal_tabs.size());
    for (const auto& terminal_tab : shell.context_.current_project_state.terminal_tabs) {
      labels.push_back(terminal_tab == nullptr ? std::string{} : terminal_tab->session.LaunchLabel());
    }
    return labels;
  }
  static std::vector<std::string> VisibleMenuBarLabels(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    std::vector<std::string> labels;
    for (const auto& item : shell.ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(item.id); menu != nullptr) {
        labels.emplace_back(menu->label);
      }
    }
    return labels;
  }
  static std::optional<SDL_FRect> MenuBarItemRect(WorkspaceShell& shell, std::string_view label) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const auto& item : shell.ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(item.id);
          menu != nullptr && menu->label == label) {
        return item.rect;
      }
    }
    return std::nullopt;
  }
  static std::vector<std::string> VisiblePopupMenuLabels(WorkspaceShell& shell,
                                                         WorkspaceShell::MenuId id) {
    std::vector<std::string> labels;
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto popup_rect = shell.ComputePopupMenuRect(layout.menu_bar, id);
    if (!popup_rect.has_value()) {
      return labels;
    }
    const auto items = shell.MenuItems(id);
    for (const auto& visible_item : shell.ComputeVisiblePopupMenuItems(id, *popup_rect)) {
      if (visible_item.separator) {
        continue;
      }
      labels.push_back(shell.MenuItemLabel(items[visible_item.index]));
    }
    return labels;
  }
  static std::optional<SDL_FRect> PopupMenuItemRect(WorkspaceShell& shell,
                                                    WorkspaceShell::MenuId id,
                                                    std::string_view label) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto popup_rect = shell.ComputePopupMenuRect(layout.menu_bar, id);
    if (!popup_rect.has_value()) {
      return std::nullopt;
    }
    const auto items = shell.MenuItems(id);
    for (const auto& visible_item : shell.ComputeVisiblePopupMenuItems(id, *popup_rect)) {
      if (visible_item.separator) {
        continue;
      }
      if (shell.MenuItemLabel(items[visible_item.index]) == label) {
        return visible_item.rect;
      }
    }
    return std::nullopt;
  }
  static std::vector<std::string> SidebarModeMenuLabels(WorkspaceShell& shell) {
    return VisiblePopupMenuLabels(shell, WorkspaceShell::MenuId::SidebarMode);
  }
  static std::optional<SDL_FRect> SidebarModeMenuItemRect(WorkspaceShell& shell,
                                                          std::string_view label) {
    return PopupMenuItemRect(shell, WorkspaceShell::MenuId::SidebarMode, label);
  }
  static SDL_FRect SidebarModeButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.SidebarModeControlRect(layout.sidebar);
  }
  static SDL_FRect TreeSidebarCollapseButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.TreeSidebarCollapseButtonRect(layout.sidebar);
  }
  static std::string BreadcrumbLabel(WorkspaceShell& shell) { return shell.BreadcrumbLabel(); }
  static const std::vector<project::TreeEntry>& TreeEntries(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.directory_tree.entries();
  }
  static std::filesystem::path SelectedTreePath(const WorkspaceShell& shell) {
    return shell.SelectedTreePath();
  }
  static int SidebarScrollRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.scroll_row;
  }
  static std::size_t ProjectCount(const WorkspaceShell& shell) { return shell.context_.project_catalog.entries.size(); }
  static std::size_t ActiveProjectIndex(const WorkspaceShell& shell) {
    return shell.context_.project_catalog.active_index;
  }
  static const std::filesystem::path& ProjectRoot(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.root;
  }
  static const std::vector<project::ProjectSearchResult>& ProjectSearchResults(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.results;
  }
  static bool ProjectSearchRunning(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.running;
  }
  static bool ProjectSearchTruncated(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.truncated;
  }
  static const std::string& ProjectSearchError(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.error;
  }
  static bool ProjectOpenDialogActive(const WorkspaceShell& shell) {
    return shell.project_dialog_state_.active;
  }
  static bool CommandMode(const WorkspaceShell& shell) { return shell.context_.current_project_state.panel.command_mode; }
  static const std::string& CommandInput(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.command.input.text();
  }
  static std::string CommandPromptStatusText(const WorkspaceShell& shell) {
    return CommandPromptCoordinator::PromptStatusText(shell.context_.current_project_state.panel.command);
  }
  static const std::string& ProjectSearchQuery(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.query.text();
  }
  static SDL_FRect ProjectSearchQueryRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ProjectSearchQueryRect(layout.sidebar);
  }
  static SDL_FRect ProjectSearchReplaceRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ProjectSearchReplaceRect(layout.sidebar);
  }
  static bool OverlayVisible(const WorkspaceShell& shell) { return shell.context_.current_project_state.overlay.visible; }
  static bool OverlayModeIsFileFinder(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.mode == WorkspaceShell::OverlayMode::FileFinder;
  }
  static bool TextInputSurfaceIsEditor(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::Editor;
  }
  static bool TextInputSurfaceIsFileFinder(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::FileFinder;
  }
  static bool TextInputSurfaceIsPromptInput(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::PromptInput;
  }
  static bool SidebarVisible(const WorkspaceShell& shell) { return shell.context_.current_project_state.sidebar.visible; }
  static WorkspaceShell::SidebarMode SidebarMode(const WorkspaceShell& shell) {
    return shell.ActiveSidebarMode();
  }
  static const std::string& SidebarViewId(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.view_id;
  }
  static std::size_t ProblemsSidebarSelectedIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.problems.selected_index;
  }
  static const std::vector<plugin::PluginHost::SidebarItem>& PluginSidebarItems(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.plugin.items;
  }
  static const std::string& PluginSidebarError(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.plugin.error;
  }
  static float SidebarWidth(const WorkspaceShell& shell) { return shell.context_.current_project_state.sidebar.width; }
  static float UiScale(const WorkspaceShell& shell) { return shell.UiScale(); }
  static bool SoftTabsEnabled(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.soft_tabs;
  }
  static void SetTransientDragTargetSidebarDivider(WorkspaceShell& shell) {
    shell.context_.interaction_state.drag_target = WorkspaceShell::DragTarget::SidebarDivider;
  }
  static void SetTransientDragTargetBottomPanelScrollbar(WorkspaceShell& shell) {
    shell.context_.interaction_state.drag_target = WorkspaceShell::DragTarget::BottomPanelScrollbar;
  }
  static bool TransientDragTargetIsNone(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.drag_target == WorkspaceShell::DragTarget::None;
  }
  static void SetTransientMouseSelecting(WorkspaceShell& shell, bool selecting) {
    shell.context_.interaction_state.mouse_selecting = selecting;
  }
  static bool TransientMouseSelecting(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.mouse_selecting;
  }
  static bool FocusIsEditor(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Editor;
  }
  static bool FocusIsSidebar(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Sidebar;
  }
  static bool FocusIsPanel(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Panel;
  }
  static void ResetCaretBlink(WorkspaceShell& shell) { shell.ResetCaretBlink(); }
  static void SetCaretBlinkEpochMs(WorkspaceShell& shell, Uint64 epoch_ms) {
    shell.caret_blink_epoch_ms_ = epoch_ms;
  }
  static bool CaretVisibleNow(const WorkspaceShell& shell) { return shell.CaretVisibleNow(); }
  static std::optional<SDL_FRect> CurrentCaretDirtyRect(const WorkspaceShell& shell) {
    return shell.CurrentCaretDirtyRect();
  }
  static WorkspaceShell::RenderInvalidation ConsumePendingRenderInvalidation(WorkspaceShell& shell) {
    return shell.ConsumePendingRenderInvalidation();
  }
  static std::optional<std::chrono::milliseconds> ProjectFileMonitorNextPollDelay(
      const WorkspaceShell& shell) {
    return shell.project_file_monitor_.NextPollDelay();
  }
  static void MoveMergeSelection(WorkspaceShell& shell, int delta) { shell.MoveMergeSelection(delta); }
  static void MoveCompareSelection(WorkspaceShell& shell, int delta) { shell.MoveCompareSelection(delta); }
  static bool ShouldBlinkCaret(const WorkspaceShell& shell) { return shell.ShouldBlinkCaret(); }
  static bool FocusIsOverlay(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Overlay;
  }
  static bool MenuBarOpen(const WorkspaceShell& shell) { return shell.context_.menu_state.menu_bar_open; }
  static bool EditMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::Edit;
  }
  static bool EditorContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::EditorContext;
  }
  static bool FileMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::File;
  }
  static bool EditorTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::EditorTabContext;
  }
  static bool SidebarModeMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::SidebarMode;
  }
  static bool TerminalTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::TerminalTabContext;
  }
  static bool TerminalContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::TerminalContext;
  }
};

#endif

}  // namespace microide::workspace
