#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

bool ParseLineColumnSpec(std::string_view location,
                         long long* line,
                         std::size_t* column,
                         bool allow_zero_line) {
  if (line == nullptr || column == nullptr || location.empty()) {
    return false;
  }

  const std::size_t colon = location.find(':');
  const std::string line_text(location.substr(0, colon));
  const std::string column_text =
      colon == std::string_view::npos ? std::string{} : std::string(location.substr(colon + 1));

  long long parsed_line = 0;
  std::size_t parsed_column = 0;
  try {
    parsed_line = std::stoll(line_text);
    if (!column_text.empty()) {
      parsed_column = static_cast<std::size_t>(std::stoull(column_text));
    }
  } catch (...) {
    return false;
  }

  if (!allow_zero_line && parsed_line == 0) {
    return false;
  }

  *line = parsed_line;
  *column = parsed_column;
  return true;
}

}  // namespace

bool WorkspaceShell::ExecuteAction(ActionId id,
                                   const std::vector<std::string>& args,
                                   ActionSource source) {
  if (source != ActionSource::ContextMenu) {
    CloseTreeContextMenu();
  }

  const auto reject_command = [&](std::string feedback) {
    return RejectCommandAction(source, std::move(feedback));
  };

  std::string rejection_feedback;
  const auto dispatch_result = [&](ActionDispatchResult result) -> std::optional<bool> {
    switch (result) {
      case ActionDispatchResult::Unhandled:
        return std::nullopt;
      case ActionDispatchResult::Handled:
        return true;
      case ActionDispatchResult::Rejected:
        return reject_command(std::move(rejection_feedback));
    }
    return std::nullopt;
  };

  if (const auto handled =
          dispatch_result(ExecuteProjectAction(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteSidebarAction(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteSearchAction(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled = dispatch_result(ExecuteTabAction(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteEditAction(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteGlobalAction(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }

  return true;
}

WorkspaceShell::ActionDispatchResult WorkspaceShell::ExecuteProjectAction(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return ActionDispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::ProjectOpen:
      if (args.empty()) {
        switch (OpenNativeProjectPicker(nullptr)) {
          case ProjectOpenDialogLaunchResult::Launched:
          case ProjectOpenDialogLaunchResult::AlreadyOpen:
            return ActionDispatchResult::Handled;
          case ProjectOpenDialogLaunchResult::Unavailable:
            if (source == ActionSource::Menu) {
              surface_.command_mode = true;
              surface_.focus = FocusTarget::Panel;
              command_.input = "project-open ";
              ResetCommandSessionState();
            }
            return ActionDispatchResult::Handled;
        }
        return ActionDispatchResult::Handled;
      }
      if (!OpenProjectTab(std::filesystem::path(args[0]), true, true)) {
        return reject("Failed to open project: " + args[0]);
      }
      return ActionDispatchResult::Handled;
    case ActionId::ProjectClose:
      if (project_catalog_.entries.empty() || project_root_.empty()) {
        return reject("No active project");
      }
      RequestCloseProject(project_catalog_.active_index);
      return ActionDispatchResult::Handled;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev: {
      if (project_catalog_.entries.empty() || project_root_.empty()) {
        return reject("No active project");
      }
      if (project_catalog_.entries.size() == 1) {
        return reject("Only one project tab is open");
      }
      const int delta = id == ActionId::ProjectNext ? 1 : -1;
      const int project_count = static_cast<int>(project_catalog_.entries.size());
      const int next_index =
          (static_cast<int>(project_catalog_.active_index) + delta + project_count) % project_count;
      SwitchProject(static_cast<std::size_t>(next_index), true);
      return ActionDispatchResult::Handled;
    }
    default:
      return ActionDispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionDispatchResult WorkspaceShell::ExecuteSidebarAction(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return ActionDispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::SidebarToggle: {
      const std::string tool = args.empty() ? std::string{} : args[0];
      if (tool == "git") {
        if (surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Git) {
          CloseSidebar();
        } else {
          ShowGitSidebar();
        }
        return ActionDispatchResult::Handled;
      }
      if (tool == "tree") {
        if (surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Tree) {
          CloseSidebar();
        } else {
          const std::filesystem::path root_arg =
              args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::path{};
          ShowTreeSidebar(root_arg);
        }
        return ActionDispatchResult::Handled;
      }
      if (tool == "search") {
        const std::string query = JoinCommandArguments(args, 1);
        if (surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Search &&
            !surface_.sidebar_temporary) {
          CloseSidebar();
        } else {
          ShowSearchSidebar(query, false);
        }
        return ActionDispatchResult::Handled;
      }
      ToggleSidebar();
      return ActionDispatchResult::Handled;
    }
    case ActionId::SidebarShow: {
      const std::string tool = args.empty() ? std::string{} : args[0];
      if (tool == "git") {
        ShowGitSidebar();
        return ActionDispatchResult::Handled;
      }
      if (tool == "tree") {
        const std::filesystem::path root_arg =
            args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::path{};
        ShowTreeSidebar(root_arg);
        return ActionDispatchResult::Handled;
      }
      if (tool == "search") {
        ShowSearchSidebar(JoinCommandArguments(args, 1), false);
        return ActionDispatchResult::Handled;
      }
      surface_.sidebar_visible = true;
      surface_.focus = FocusTarget::Sidebar;
      return ActionDispatchResult::Handled;
    }
    case ActionId::SidebarHide:
    case ActionId::SidebarClose:
      CloseSidebar();
      return ActionDispatchResult::Handled;
    case ActionId::SidebarWidth:
      if (args.empty()) {
        return reject("sidebar-width requires a numeric width");
      }
      try {
        const float width = std::stof(args[0]);
        surface_.sidebar_width =
            ClampSidebarWidth(width, static_cast<float>(std::max(1, last_window_width_)));
      } catch (...) {
        return reject("sidebar-width requires a numeric width");
      }
      return ActionDispatchResult::Handled;
    case ActionId::TreeRefresh:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      RefreshProjectFiles();
      return ActionDispatchResult::Handled;
    case ActionId::GitRefresh:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      RefreshProjectFiles();
      return ActionDispatchResult::Handled;
    case ActionId::CreateFile:
    case ActionId::CreateDirectory: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path base_path = TreeMutationBasePath(source);
      if (base_path.empty()) {
        return reject("No target directory selected");
      }
      OpenPromptSurface(id == ActionId::CreateFile ? PromptSurfaceState::Action::CreateFile
                                                   : PromptSurfaceState::Action::CreateDirectory,
                        PromptSurfaceState::Kind::TextInput, base_path);
      return ActionDispatchResult::Handled;
    }
    case ActionId::RenamePath: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      OpenPromptSurface(PromptSurfaceState::Action::RenamePath,
                        PromptSurfaceState::Kind::TextInput, path, path.filename().string());
      return ActionDispatchResult::Handled;
    }
    case ActionId::DeletePath: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      OpenPromptSurface(PromptSurfaceState::Action::DeletePath,
                        PromptSurfaceState::Kind::Confirm, path);
      return ActionDispatchResult::Handled;
    }
    case ActionId::CopyRelativePath:
    case ActionId::CopyAbsolutePath: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }

      std::string clipboard_text;
      if (id == ActionId::CopyRelativePath) {
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(path, project_root_, error);
        if (error || relative.empty()) {
          return reject("Unable to resolve a relative path for the selection");
        }
        clipboard_text = relative.generic_string();
      } else {
        clipboard_text = path.lexically_normal().string();
      }

      WriteClipboardText(clipboard_text);
      return ActionDispatchResult::Handled;
    }
    case ActionId::Tree: {
      const std::filesystem::path root_arg =
          args.empty() ? std::filesystem::path{} : std::filesystem::path(args[0]);
      if (root_arg.empty() && project_root_.empty()) {
        return reject("No active project");
      }
      ShowTreeSidebar(root_arg);
      return ActionDispatchResult::Handled;
    }
    default:
      return ActionDispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionDispatchResult WorkspaceShell::ExecuteSearchAction(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  (void) source;
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return ActionDispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::Term:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      OpenTerminal(JoinCommandArguments(args, 0));
      return ActionDispatchResult::Handled;
    case ActionId::Find:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      file_index_.Refresh();
      file_finder_.SetIndex(&file_index_);
      file_finder_.SetQuery(JoinCommandArguments(args, 0));
      ShowOverlay(OverlayMode::FileFinder);
      return ActionDispatchResult::Handled;
    case ActionId::Files: {
      const std::string root_arg = args.empty() ? std::string{} : args[0];
      if (!root_arg.empty() && !OpenProjectTab(root_arg, true, true)) {
        return reject("Failed to open project: " + root_arg);
      }
      if (source == ActionSource::Shortcut && surface_.overlay_visible) {
        DismissOverlay();
        return ActionDispatchResult::Handled;
      }
      if (source != ActionSource::Shortcut && project_root_.empty()) {
        return reject("No active project");
      }
      ShowOverlay(OverlayMode::FileFinder);
      file_index_.Refresh();
      file_finder_.SetIndex(&file_index_);
      file_finder_.SetQuery("");
      return ActionDispatchResult::Handled;
    }
    case ActionId::ProjectSearch:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      ShowSearchSidebar(JoinCommandArguments(args, 0), true);
      return ActionDispatchResult::Handled;
    case ActionId::Search:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      if (ActiveTabIsCompare() || ActiveTabIsMerge()) {
        return reject("search is unavailable in compare and merge tabs");
      }
      OpenBufferSearch();
      overlay_workflow_.buffer_search.query = JoinCommandArguments(args, 0);
      RefreshBufferSearch();
      return ActionDispatchResult::Handled;
    case ActionId::ReplaceInBuffer:
      OpenBufferReplace();
      return ActionDispatchResult::Handled;
    case ActionId::Compare: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      std::filesystem::path path;
      if (!args.empty()) {
        path = std::filesystem::path(args[0]);
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();
      } else if (source == ActionSource::ContextMenu) {
        path = ResolveTreeActionPath(source);
      } else if (!text_viewport_.path().empty()) {
        path = text_viewport_.path().lexically_normal();
      } else if (surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Tree) {
        const auto& entries = directory_tree_.entries();
        if (directory_tree_.selected_index() < entries.size() &&
            !entries[directory_tree_.selected_index()].is_directory) {
          path = entries[directory_tree_.selected_index()].path.lexically_normal();
        }
      }

      if (path.empty()) {
        return reject("No file selected for compare");
      }
      if (!std::filesystem::exists(path)) {
        return reject("Compare path does not exist: " + path.string());
      }

      const std::string commit_spec = args.size() > 1 ? args[1] : "";
      OpenComparePickerForPath(path, commit_spec);
      return ActionDispatchResult::Handled;
    }
    case ActionId::CompareHead: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No file selected for compare-head");
      }
      if (!std::filesystem::exists(path)) {
        return reject("Compare path does not exist: " + path.string());
      }
      overlay_workflow_.compare_picker.path = path.lexically_normal();
      OpenComparison(project::GitCommitEntry{
          .hash = "HEAD",
          .short_hash = "HEAD",
          .subject = "HEAD",
      });
      return ActionDispatchResult::Handled;
    }
    case ActionId::Merge: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      if (args.size() < 3 || args.size() > 4) {
        return reject("merge requires base, incoming, current, and optional output paths");
      }

      auto resolve_path = [&](const std::string& text) {
        std::filesystem::path path = text;
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        return path.lexically_normal();
      };

      const std::filesystem::path base_path = resolve_path(args[0]);
      const std::filesystem::path incoming_path = resolve_path(args[1]);
      const std::filesystem::path current_path = resolve_path(args[2]);
      const std::filesystem::path output_path =
          args.size() > 3 ? resolve_path(args[3]) : current_path;
      if (!std::filesystem::exists(base_path) || !std::filesystem::exists(incoming_path) ||
          !std::filesystem::exists(current_path)) {
        return reject("merge requires existing base, incoming, and current files");
      }

      OpenMergeEditor(base_path, incoming_path, current_path, output_path);
      return ActionDispatchResult::Handled;
    }
    default:
      return ActionDispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionDispatchResult WorkspaceShell::ExecuteTabAction(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return ActionDispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::Open:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      if (args.empty()) {
        return reject("open requires a path");
      }
      {
        std::filesystem::path path = args[0];
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();

        auto* editor_tab = ActiveEditorTab();
        if (editor_tab != nullptr && editor_tab->views.size() > 1) {
          editor::TextViewport opened_view;
          if (!opened_view.OpenFile(path)) {
            return reject("Failed to open file: " + path.string());
          }
          if (!ReplaceActiveEditorView(opened_view)) {
            return reject("Failed to replace the active split with: " + path.string());
          }
          return ActionDispatchResult::Handled;
        }

        OpenFile(path);
        return ActionDispatchResult::Handled;
      }
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      if (id == ActionId::OpenSelectedTreeItemInNewTab) {
        if (!OpenFileInNewTab(path)) {
          return reject("Failed to open file in a new tab: " + path.string());
        }
      } else {
        OpenFile(path);
      }
      return ActionDispatchResult::Handled;
    }
    case ActionId::Tab:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      if (args.empty()) {
        OpenUntitledTab();
        return ActionDispatchResult::Handled;
      }

      for (const std::string& arg : args) {
        std::filesystem::path path = arg;
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();

        if (!OpenFileInNewTab(path)) {
          return reject("Failed to open file in a new tab: " + path.string());
        }
      }

      return ActionDispatchResult::Handled;
    case ActionId::TabSwitch: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      std::string error_message;
      const std::optional<std::size_t> tab_index =
          FindTabIndexBySpecifier(JoinCommandArguments(args, 0), &error_message);
      if (!tab_index.has_value()) {
        return reject(error_message.empty() ? "No matching tab" : error_message);
      }
      ActivateTab(*tab_index);
      return ActionDispatchResult::Handled;
    }
    case ActionId::TabMove:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      if (args.empty()) {
        return reject("tabmove requires a tab slot or relative offset");
      }
      if (open_tabs_.empty()) {
        return reject("No open tabs");
      }
      {
        std::size_t parsed_length = 0;
        int slot = 0;
        try {
          slot = std::stoi(args[0], &parsed_length);
        } catch (...) {
          return reject("tabmove requires a tab slot or relative offset");
        }
        if (parsed_length != args[0].size()) {
          return reject("tabmove requires a tab slot or relative offset");
        }

        const bool relative =
            !args[0].empty() && (args[0].front() == '+' || args[0].front() == '-');
        const int current_slot = static_cast<int>(active_tab_index_) + 1;
        const int requested_slot = relative ? current_slot + slot : slot;
        const int clamped_slot =
            std::clamp(requested_slot, 1, static_cast<int>(open_tabs_.size()));
        MoveActiveTabTo(static_cast<std::size_t>(clamped_slot - 1));
        return ActionDispatchResult::Handled;
      }
    case ActionId::Reopen:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      ReopenActiveTab();
      return ActionDispatchResult::Handled;
    case ActionId::Save:
      if (project_root_.empty()) {
        return reject("No active project");
      }
      if (SaveTab(active_tab_index_)) {
        if (source == ActionSource::Shortcut) {
          ResetCaretBlink();
        }
      } else {
        return reject("Save failed");
      }
      return ActionDispatchResult::Handled;
    case ActionId::Vsplit: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      const EditorSplitOrientation orientation = EditorSplitOrientation::Vertical;

      if (args.empty()) {
        SplitActiveEditor(orientation);
        return ActionDispatchResult::Handled;
      }

      for (const std::string& arg : args) {
        std::filesystem::path path = arg;
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();

        editor::TextViewport opened_view;
        if (!opened_view.OpenFile(path)) {
          return reject("Failed to open file: " + path.string());
        }
        if (!SplitActiveEditor(orientation)) {
          return reject("Failed to split the active editor");
        }
        if (!ReplaceActiveEditorView(opened_view)) {
          return reject("Failed to replace the active split with: " + path.string());
        }
      }

      return ActionDispatchResult::Handled;
    }
    case ActionId::Unsplit:
      UnsplitActiveEditor();
      return ActionDispatchResult::Handled;
    case ActionId::SplitNext:
      CycleEditorSplit(1);
      return ActionDispatchResult::Handled;
    case ActionId::SplitPrev:
      CycleEditorSplit(-1);
      return ActionDispatchResult::Handled;
    case ActionId::SplitFirst:
      ActivateOrderedEditorSplit(0);
      return ActionDispatchResult::Handled;
    case ActionId::SplitLast: {
      auto* editor_tab = ActiveEditorTab();
      const std::size_t last_index =
          editor_tab == nullptr || editor_tab->views.empty() ? 0 : editor_tab->views.size() - 1;
      ActivateOrderedEditorSplit(last_index);
      return ActionDispatchResult::Handled;
    }
    case ActionId::CloseActiveTab:
      if (!open_tabs_.empty()) {
        RequestCloseTab(active_tab_index_);
      }
      return ActionDispatchResult::Handled;
    default:
      return ActionDispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionDispatchResult WorkspaceShell::ExecuteEditAction(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  (void) source;
  (void) rejection_feedback;

  switch (id) {
    case ActionId::Goto:
    case ActionId::Jump: {
      if (ActiveTabIsCompare() || ActiveTabIsMerge()) {
        return ActionDispatchResult::Handled;
      }
      if (args.empty()) {
        return ActionDispatchResult::Handled;
      }

      long long requested_line = 0;
      std::size_t column = 0;
      if (!ParseLineColumnSpec(args[0], &requested_line, &column, id == ActionId::Jump)) {
        return ActionDispatchResult::Handled;
      }

      if (id == ActionId::Goto && requested_line == 0) {
        return ActionDispatchResult::Handled;
      }

      const std::size_t line_count = std::max<std::size_t>(1, text_viewport_.line_count());
      std::size_t line = 0;
      if (id == ActionId::Jump) {
        const long long current_line = static_cast<long long>(text_viewport_.cursor_line()) + 1;
        const long long target_line = current_line + requested_line;
        line = static_cast<std::size_t>(
            std::clamp(target_line - 1, 0LL, static_cast<long long>(line_count - 1)));
      } else if (requested_line > 0) {
        line = static_cast<std::size_t>(requested_line - 1);
      } else {
        const std::size_t from_end = static_cast<std::size_t>(-requested_line);
        line = from_end >= line_count ? 0 : line_count - from_end;
      }

      text_viewport_.MoveCursorTo(line, column > 0 ? column - 1 : 0);
      surface_.focus = FocusTarget::Editor;
      return ActionDispatchResult::Handled;
    }
    case ActionId::SelectAll:
      if (auto* viewport = ActiveEditableViewport(); viewport != nullptr) {
        viewport->SelectAll();
        ResetCaretBlink();
      }
      surface_.focus = FocusTarget::Editor;
      return ActionDispatchResult::Handled;
    case ActionId::Undo:
      if (auto* viewport = ActiveEditableViewport(); viewport != nullptr) {
        const std::vector<std::string> before_lines = viewport->lines();
        const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
        const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
        if (viewport->Undo()) {
          if (auto* compare_tab = ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            RefreshCompareTabDerivedState(*compare_tab);
            SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = ActiveMergeTab(); merge_tab != nullptr &&
                                                    viewport == &merge_tab->result_viewport) {
            UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                 cursor_before);
          }
          ResetCaretBlink();
        }
      }
      return ActionDispatchResult::Handled;
    case ActionId::Redo:
      if (auto* viewport = ActiveEditableViewport(); viewport != nullptr) {
        const std::vector<std::string> before_lines = viewport->lines();
        const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
        const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
        if (viewport->Redo()) {
          if (auto* compare_tab = ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            RefreshCompareTabDerivedState(*compare_tab);
            SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = ActiveMergeTab(); merge_tab != nullptr &&
                                                    viewport == &merge_tab->result_viewport) {
            UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                 cursor_before);
          }
          ResetCaretBlink();
        }
      }
      return ActionDispatchResult::Handled;
    case ActionId::CopySelection: {
      const std::string text =
          ActiveEditableViewport() != nullptr ? ActiveEditableViewport()->SelectedText()
                                              : std::string{};
      if (!text.empty()) {
        WriteClipboardText(text);
      }
      return ActionDispatchResult::Handled;
    }
    case ActionId::CopyLastTerminalCommand: {
      const std::optional<std::string> text = LastTerminalCommandText();
      if (text.has_value()) {
        WriteClipboardText(*text);
      }
      return ActionDispatchResult::Handled;
    }
    case ActionId::CopySelectionWithContext: {
      const std::optional<std::string> text = SelectionTextWithContext();
      if (text.has_value()) {
        WriteClipboardText(*text);
      }
      return ActionDispatchResult::Handled;
    }
    case ActionId::CutSelection: {
      if (auto* viewport = ActiveEditableViewport(); viewport != nullptr) {
        const std::string text = viewport->SelectedText();
        if (!text.empty() && WriteClipboardText(text)) {
          const std::vector<std::string> before_lines = viewport->lines();
          const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
          const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
          viewport->DeleteSelectedText();
          if (auto* compare_tab = ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            RefreshCompareTabDerivedState(*compare_tab);
            SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = ActiveMergeTab(); merge_tab != nullptr &&
                                                    viewport == &merge_tab->result_viewport) {
            UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                 cursor_before);
          }
          ResetCaretBlink();
        }
      }
      return ActionDispatchResult::Handled;
    }
    case ActionId::PasteClipboard: {
      if (const std::optional<std::string> clipboard_text = ReadClipboardText();
          clipboard_text.has_value()) {
        if (auto* viewport = ActiveEditableViewport(); viewport != nullptr) {
          const std::vector<std::string> before_lines = viewport->lines();
          const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
          const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
          viewport->InsertText(*clipboard_text);
          if (auto* compare_tab = ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            RefreshCompareTabDerivedState(*compare_tab);
            SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = ActiveMergeTab(); merge_tab != nullptr &&
                                                    viewport == &merge_tab->result_viewport) {
            UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                 cursor_before);
          }
          ResetCaretBlink();
        }
      }
      return ActionDispatchResult::Handled;
    }
    default:
      return ActionDispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionDispatchResult WorkspaceShell::ExecuteGlobalAction(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  (void) source;
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return ActionDispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::Colorscheme:
      if (args.empty()) {
        return ActionDispatchResult::Handled;
      }
      if (args[0] == "list") {
        RefreshAvailableColorschemeNames();
        return ActionDispatchResult::Handled;
      }
      RefreshAvailableColorschemeNames();
      ApplyColorscheme(args[0], true, true);
      return ActionDispatchResult::Handled;
    case ActionId::TabSize:
      if (args.empty()) {
        return reject("tab-size requires an integer from 1 to 16");
      }
      try {
        editor_preferences_.tab_size =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(args[0])), 1, 16);
        ApplyEditorPreferencesToAllTabs();
        SaveConfigState();
      } catch (...) {
        return reject("tab-size requires an integer from 1 to 16");
      }
      return ActionDispatchResult::Handled;
    case ActionId::IndentWidth:
      if (args.empty()) {
        return reject("indent-width requires an integer from 1 to 16");
      }
      try {
        editor_preferences_.indent_width =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(args[0])), 1, 16);
        ApplyEditorPreferencesToAllTabs();
        SaveConfigState();
      } catch (...) {
        return reject("indent-width requires an integer from 1 to 16");
      }
      return ActionDispatchResult::Handled;
    case ActionId::UiScale:
      if (args.empty()) {
        return reject("ui-scale requires a preset or numeric value");
      }
      if (args[0] == "up") {
        ApplyUiScale(StepUiScale(ui_scale_, 1), true, true);
        return ActionDispatchResult::Handled;
      }
      if (args[0] == "down") {
        ApplyUiScale(StepUiScale(ui_scale_, -1), true, true);
        return ActionDispatchResult::Handled;
      }
      if (args[0] == "reset") {
        ApplyUiScale(1.0f, true, true);
        return ActionDispatchResult::Handled;
      }
      if (const auto scale = ParseUiScaleValue(args[0]); scale.has_value()) {
        ApplyUiScale(*scale, true, true);
      } else {
        return reject("ui-scale requires a preset or numeric value");
      }
      return ActionDispatchResult::Handled;
    case ActionId::SoftTabs:
      if (args.empty()) {
        return reject("soft-tabs expects on or off");
      }
      if (const std::string value = ToLower(args[0]);
          value != "on" && value != "off" && value != "true" && value != "false" &&
          value != "1" && value != "0") {
        return reject("soft-tabs expects on or off");
      } else {
        editor_preferences_.soft_tabs = value == "on" || value == "true" || value == "1";
      }
      ApplyEditorPreferencesToAllTabs();
      SaveConfigState();
      return ActionDispatchResult::Handled;
    case ActionId::Focus: {
      const std::string target = args.empty() ? std::string{} : args[0];
      if (target == "sidebar" && surface_.sidebar_visible) {
        surface_.focus = FocusTarget::Sidebar;
        return ActionDispatchResult::Handled;
      }
      if (target == "editor") {
        surface_.focus = FocusTarget::Editor;
        return ActionDispatchResult::Handled;
      }
      if (target == "panel" && (surface_.command_mode || ActiveTerminalTab() != nullptr)) {
        surface_.focus = FocusTarget::Panel;
        return ActionDispatchResult::Handled;
      }
      return reject("Cannot focus target: " +
                    (target.empty() ? std::string("<empty>") : target));
    }
    case ActionId::OpenCommandPrompt:
      surface_.command_mode = true;
      surface_.focus = FocusTarget::Panel;
      command_.input.clear();
      ResetCommandSessionState();
      return ActionDispatchResult::Handled;
    case ActionId::Quit:
      RequestQuit();
      return ActionDispatchResult::Handled;
    default:
      return ActionDispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
