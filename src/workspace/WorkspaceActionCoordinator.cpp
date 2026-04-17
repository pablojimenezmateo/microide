#include "workspace/WorkspaceActionCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceSidebarRegistry.h"
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

struct ProjectOpenRequest {
  bool use_native_picker = false;
  std::filesystem::path path;
};

ProjectOpenRequest BuildProjectOpenRequest(const std::vector<std::string>& args) {
  if (args.empty()) {
    return ProjectOpenRequest{
        .use_native_picker = true,
        .path = {},
    };
  }
  return ProjectOpenRequest{
      .use_native_picker = false,
      .path = std::filesystem::path(args[0]),
  };
}

struct ProjectCycleRequest {
  int delta = 0;
};

ProjectCycleRequest BuildProjectCycleRequest(int delta) {
  return ProjectCycleRequest{.delta = delta};
}

std::filesystem::path NormalizeCommandPath(const std::filesystem::path& project_root,
                                           std::filesystem::path path) {
  if (path.empty()) {
    return {};
  }
  if (path.is_relative() && !project_root.empty()) {
    path = project_root / path;
  }
  return path.lexically_normal();
}

std::optional<float> ParseFloatArgument(const std::string& text) {
  try {
    return std::stof(text);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<int> ParseIntArgument(const std::string& text) {
  std::size_t parsed_length = 0;
  try {
    const int value = std::stoi(text, &parsed_length);
    if (parsed_length == text.size()) {
      return value;
    }
  } catch (...) {
  }
  return std::nullopt;
}

std::optional<std::size_t> ParseClampedSizeArgument(const std::string& text,
                                                    std::size_t minimum,
                                                    std::size_t maximum) {
  try {
    return std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(text)), minimum, maximum);
  } catch (...) {
    return std::nullopt;
  }
}

struct SidebarWidthRequest {
  float width = 0.0f;
};

std::optional<SidebarWidthRequest> BuildSidebarWidthRequest(const std::vector<std::string>& args) {
  if (args.empty()) {
    return std::nullopt;
  }
  const std::optional<float> width = ParseFloatArgument(args[0]);
  if (!width.has_value()) {
    return std::nullopt;
  }
  return SidebarWidthRequest{.width = *width};
}

struct TreeRootRequest {
  std::filesystem::path root;
};

TreeRootRequest BuildTreeRootRequest(const std::vector<std::string>& args) {
  return TreeRootRequest{
      .root = args.empty() ? std::filesystem::path{} : std::filesystem::path(args[0]),
  };
}

struct FilesRequest {
  std::filesystem::path project_root;
};

FilesRequest BuildFilesRequest(const std::vector<std::string>& args) {
  return FilesRequest{
      .project_root = args.empty() ? std::filesystem::path{} : std::filesystem::path(args[0]),
  };
}

struct CompareRequest {
  std::filesystem::path path;
  std::string commit_spec;
};

CompareRequest BuildCompareRequest(const std::vector<std::string>& args,
                                   const std::filesystem::path& project_root) {
  return CompareRequest{
      .path = args.empty() ? std::filesystem::path{}
                           : NormalizeCommandPath(project_root, std::filesystem::path(args[0])),
      .commit_spec = args.size() > 1 ? args[1] : std::string{},
  };
}

struct MergeRequest {
  std::filesystem::path base_path;
  std::filesystem::path incoming_path;
  std::filesystem::path current_path;
  std::filesystem::path output_path;
};

std::optional<MergeRequest> BuildMergeRequest(const std::vector<std::string>& args,
                                              const std::filesystem::path& project_root) {
  if (args.size() < 3 || args.size() > 4) {
    return std::nullopt;
  }
  const std::filesystem::path base_path =
      NormalizeCommandPath(project_root, std::filesystem::path(args[0]));
  const std::filesystem::path incoming_path =
      NormalizeCommandPath(project_root, std::filesystem::path(args[1]));
  const std::filesystem::path current_path =
      NormalizeCommandPath(project_root, std::filesystem::path(args[2]));
  return MergeRequest{
      .base_path = base_path,
      .incoming_path = incoming_path,
      .current_path = current_path,
      .output_path = args.size() > 3
                         ? NormalizeCommandPath(project_root, std::filesystem::path(args[3]))
                         : current_path,
  };
}

struct OpenPathRequest {
  std::filesystem::path path;
};

std::optional<OpenPathRequest> BuildOpenPathRequest(const std::vector<std::string>& args,
                                                    const std::filesystem::path& project_root) {
  if (args.empty()) {
    return std::nullopt;
  }
  return OpenPathRequest{
      .path = NormalizeCommandPath(project_root, std::filesystem::path(args[0])),
  };
}

struct TabPathsRequest {
  bool open_untitled = false;
  std::vector<std::filesystem::path> paths;
};

TabPathsRequest BuildTabPathsRequest(const std::vector<std::string>& args,
                                     const std::filesystem::path& project_root) {
  TabPathsRequest request;
  if (args.empty()) {
    request.open_untitled = true;
    return request;
  }
  request.paths.reserve(args.size());
  for (const std::string& arg : args) {
    request.paths.push_back(NormalizeCommandPath(project_root, std::filesystem::path(arg)));
  }
  return request;
}

struct TabSwitchRequest {
  std::string specifier;
};

TabSwitchRequest BuildTabSwitchRequest(const std::vector<std::string>& args) {
  return TabSwitchRequest{
      .specifier = JoinCommandArguments(args, 0),
  };
}

struct TabMoveRequest {
  int slot = 0;
  bool relative = false;
};

std::optional<TabMoveRequest> BuildTabMoveRequest(const std::vector<std::string>& args) {
  if (args.empty()) {
    return std::nullopt;
  }
  const std::optional<int> slot = ParseIntArgument(args[0]);
  if (!slot.has_value()) {
    return std::nullopt;
  }
  return TabMoveRequest{
      .slot = *slot,
      .relative = !args[0].empty() && (args[0].front() == '+' || args[0].front() == '-'),
  };
}

struct LineNavigationRequest {
  long long requested_line = 0;
  std::size_t column = 0;
};

std::optional<LineNavigationRequest> BuildLineNavigationRequest(const std::vector<std::string>& args,
                                                                bool allow_zero_line) {
  if (args.empty()) {
    return std::nullopt;
  }
  LineNavigationRequest request;
  if (!ParseLineColumnSpec(args[0], &request.requested_line, &request.column, allow_zero_line)) {
    return std::nullopt;
  }
  return request;
}

struct ColorschemeRequest {
  bool list = false;
  std::string name;
};

std::optional<ColorschemeRequest> BuildColorschemeRequest(const std::vector<std::string>& args) {
  if (args.empty()) {
    return std::nullopt;
  }
  return ColorschemeRequest{
      .list = args[0] == "list",
      .name = args[0],
  };
}

struct EditorPreferenceSizeRequest {
  std::size_t value = 0;
};

std::optional<EditorPreferenceSizeRequest> BuildEditorPreferenceSizeRequest(
    const std::vector<std::string>& args) {
  if (args.empty()) {
    return std::nullopt;
  }
  const std::optional<std::size_t> value = ParseClampedSizeArgument(args[0], 1, 16);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return EditorPreferenceSizeRequest{.value = *value};
}

struct UiScaleRequest {
  enum class Kind {
    Step,
    Reset,
    Direct,
  };

  Kind kind = Kind::Direct;
  int delta = 0;
  float scale = 1.0f;
};

std::optional<UiScaleRequest> BuildUiScaleRequest(const std::vector<std::string>& args) {
  if (args.empty()) {
    return std::nullopt;
  }
  if (args[0] == "up") {
    return UiScaleRequest{
        .kind = UiScaleRequest::Kind::Step,
        .delta = 1,
        .scale = 1.0f,
    };
  }
  if (args[0] == "down") {
    return UiScaleRequest{
        .kind = UiScaleRequest::Kind::Step,
        .delta = -1,
        .scale = 1.0f,
    };
  }
  if (args[0] == "reset") {
    return UiScaleRequest{
        .kind = UiScaleRequest::Kind::Reset,
        .delta = 0,
        .scale = 1.0f,
    };
  }
  const std::optional<float> scale = ParseUiScaleValue(args[0]);
  if (!scale.has_value()) {
    return std::nullopt;
  }
  return UiScaleRequest{
      .kind = UiScaleRequest::Kind::Direct,
      .delta = 0,
      .scale = *scale,
  };
}

struct SoftTabsRequest {
  bool enabled = false;
};

std::optional<SoftTabsRequest> BuildSoftTabsRequest(const std::vector<std::string>& args) {
  if (args.empty()) {
    return std::nullopt;
  }
  const std::string value = ToLower(args[0]);
  if (value != "on" && value != "off" && value != "true" && value != "false" && value != "1" &&
      value != "0") {
    return std::nullopt;
  }
  return SoftTabsRequest{
      .enabled = value == "on" || value == "true" || value == "1",
  };
}

enum class FocusRequestTarget {
  Sidebar,
  Editor,
  Panel,
  Unknown,
};

struct FocusRequest {
  FocusRequestTarget target = FocusRequestTarget::Unknown;
  std::string raw_target;
};

FocusRequest BuildFocusRequest(const std::vector<std::string>& args) {
  const std::string target = args.empty() ? std::string{} : args[0];
  if (target == "sidebar") {
    return FocusRequest{.target = FocusRequestTarget::Sidebar, .raw_target = target};
  }
  if (target == "editor") {
    return FocusRequest{.target = FocusRequestTarget::Editor, .raw_target = target};
  }
  if (target == "panel") {
    return FocusRequest{.target = FocusRequestTarget::Panel, .raw_target = target};
  }
  return FocusRequest{.target = FocusRequestTarget::Unknown, .raw_target = target};
}

}  // namespace

WorkspaceShell::ActionCoordinator::ActionCoordinator(WorkspaceShell& shell) : shell_(shell) {}

bool WorkspaceShell::ActionCoordinator::Execute(ActionId id,
                                   const std::vector<std::string>& args,
                                   ActionSource source) {
  if (source != ActionSource::ContextMenu) {
    MenuCoordinator(shell_).CloseTreeContextMenu();
  }

  const auto reject_command = [&](std::string feedback) {
    return CommandPromptCoordinator(shell_).RejectAction(source, std::move(feedback));
  };

  std::string rejection_feedback;
  const auto dispatch_result = [&](DispatchResult result) -> std::optional<bool> {
    switch (result) {
      case DispatchResult::Unhandled:
        return std::nullopt;
      case DispatchResult::Handled:
        return true;
      case DispatchResult::Rejected:
        return reject_command(std::move(rejection_feedback));
    }
    return std::nullopt;
  };

  if (const auto handled =
          dispatch_result(ExecuteProject(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteSidebar(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteSearch(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled = dispatch_result(ExecuteTab(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteEdit(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }
  if (const auto handled =
          dispatch_result(ExecuteGlobal(id, args, source, &rejection_feedback));
      handled.has_value()) {
    return *handled;
  }

  return true;
}

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteProject(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::ProjectOpen: {
      const ProjectOpenRequest request = BuildProjectOpenRequest(args);
      if (request.use_native_picker) {
        switch (shell_.OpenNativeProjectPicker(nullptr)) {
          case ProjectOpenDialogLaunchResult::Launched:
          case ProjectOpenDialogLaunchResult::AlreadyOpen:
            return DispatchResult::Handled;
          case ProjectOpenDialogLaunchResult::Unavailable:
            if (source == ActionSource::Menu) {
              const bool bottom_panel_was_visible = shell_.BottomPanelVisible();
              shell_.surface_.command_mode = true;
              shell_.surface_.focus = FocusTarget::Panel;
              shell_.command_.input = "project-open ";
              CommandPromptCoordinator(shell_).ResetSessionState();
              shell_.RequestCommandModeTransitionRedraw(bottom_panel_was_visible);
            }
            return DispatchResult::Handled;
        }
        return DispatchResult::Handled;
      }
      if (!shell_.OpenProjectTab(request.path, true, true)) {
        return reject("Failed to open project: " + request.path.string());
      }
      return DispatchResult::Handled;
    }
    case ActionId::ProjectClose:
      if (shell_.project_catalog_.entries.empty() || shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.RequestCloseProject(shell_.project_catalog_.active_index);
      return DispatchResult::Handled;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev: {
      if (shell_.project_catalog_.entries.empty() || shell_.project_root_.empty()) {
        return reject("No active project");
      }
      if (shell_.project_catalog_.entries.size() == 1) {
        return reject("Only one project tab is open");
      }
      const ProjectCycleRequest request =
          BuildProjectCycleRequest(id == ActionId::ProjectNext ? 1 : -1);
      const int project_count = static_cast<int>(shell_.project_catalog_.entries.size());
      const int next_index =
          (static_cast<int>(shell_.project_catalog_.active_index) + request.delta + project_count) %
          project_count;
      shell_.SwitchProject(static_cast<std::size_t>(next_index), true);
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteSidebar(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::SidebarToggle: {
      const SidebarToolRequest request = ParseBuiltinSidebarToolRequest(args);
      const std::string plugin_id = args.empty() ? std::string{} : args.front();
      if (request.tool != nullptr &&
          request.tool->mode == SidebarMode::Git) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == request.tool->mode) {
          shell_.CloseSidebar();
        } else {
          shell_.ShowGitSidebar();
        }
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr &&
          request.tool->mode == SidebarMode::Tree) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == request.tool->mode) {
          shell_.CloseSidebar();
        } else {
          shell_.ShowTreeSidebar(request.root);
        }
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr &&
          request.tool->mode == SidebarMode::Search) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == request.tool->mode &&
            !shell_.surface_.sidebar_temporary) {
          shell_.CloseSidebar();
        } else {
          shell_.ShowSearchSidebar(request.query, false);
        }
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr &&
          request.tool->mode == SidebarMode::Problems) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == request.tool->mode) {
          shell_.CloseSidebar();
        } else {
          shell_.ShowProblemsSidebar();
        }
        return DispatchResult::Handled;
      }
      if (!plugin_id.empty() && shell_.plugin_host_.FindSidebarProvider(plugin_id) != nullptr) {
        if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == SidebarMode::Plugin &&
            shell_.surface_.sidebar_plugin_id == plugin_id) {
          shell_.CloseSidebar();
        } else if (!shell_.ShowPluginSidebar(plugin_id, false)) {
          return reject("Failed to show plugin sidebar: " + plugin_id);
        }
        return DispatchResult::Handled;
      }
      shell_.ToggleSidebar();
      return DispatchResult::Handled;
    }
    case ActionId::SidebarShow: {
      const SidebarToolRequest request = ParseBuiltinSidebarToolRequest(args);
      const std::string plugin_id = args.empty() ? std::string{} : args.front();
      if (request.tool != nullptr &&
          request.tool->mode == SidebarMode::Git) {
        shell_.ShowGitSidebar();
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr &&
          request.tool->mode == SidebarMode::Tree) {
        shell_.ShowTreeSidebar(request.root);
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr &&
          request.tool->mode == SidebarMode::Search) {
        shell_.ShowSearchSidebar(request.query, false);
        return DispatchResult::Handled;
      }
      if (request.tool != nullptr &&
          request.tool->mode == SidebarMode::Problems) {
        shell_.ShowProblemsSidebar();
        return DispatchResult::Handled;
      }
      if (!plugin_id.empty() && shell_.plugin_host_.FindSidebarProvider(plugin_id) != nullptr) {
        if (!shell_.ShowPluginSidebar(plugin_id, false)) {
          return reject("Failed to show plugin sidebar: " + plugin_id);
        }
        return DispatchResult::Handled;
      }
      shell_.surface_.sidebar_visible = true;
      shell_.surface_.focus = FocusTarget::Sidebar;
      return DispatchResult::Handled;
    }
    case ActionId::SidebarHide:
    case ActionId::SidebarClose:
      shell_.CloseSidebar();
      return DispatchResult::Handled;
    case ActionId::SidebarWidth: {
      const std::optional<SidebarWidthRequest> request = BuildSidebarWidthRequest(args);
      if (!request.has_value()) {
        return reject("sidebar-width requires a numeric width");
      }
      const float current_width =
          shell_.CurrentWindowRect().has_value() ? shell_.CurrentWindowRect()->w : 1.0f;
      shell_.surface_.sidebar_width = ClampSidebarWidth(request->width,
                                                 std::max(1.0f, current_width));
      return DispatchResult::Handled;
    }
    case ActionId::TreeRefresh:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.RefreshProjectFiles();
      shell_.ReloadCleanOpenBuffersFromDisk();
      return DispatchResult::Handled;
    case ActionId::GitRefresh:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.RefreshProjectFiles();
      shell_.ReloadCleanOpenBuffersFromDisk();
      return DispatchResult::Handled;
    case ActionId::CreateFile:
    case ActionId::CreateDirectory: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path base_path = shell_.TreeMutationBasePath(source);
      if (base_path.empty()) {
        return reject("No target directory selected");
      }
      shell_.OpenPromptSurface(id == ActionId::CreateFile ? PromptSurfaceState::Action::CreateFile
                                                   : PromptSurfaceState::Action::CreateDirectory,
                        PromptSurfaceState::Kind::TextInput, base_path);
      return DispatchResult::Handled;
    }
    case ActionId::RenamePath: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      shell_.OpenPromptSurface(PromptSurfaceState::Action::RenamePath,
                        PromptSurfaceState::Kind::TextInput, path, path.filename().string());
      return DispatchResult::Handled;
    }
    case ActionId::DeletePath: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      shell_.OpenPromptSurface(PromptSurfaceState::Action::DeletePath,
                        PromptSurfaceState::Kind::Confirm, path);
      return DispatchResult::Handled;
    }
    case ActionId::CopyRelativePath:
    case ActionId::CopyAbsolutePath: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }

      std::string clipboard_text;
      if (id == ActionId::CopyRelativePath) {
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(path, shell_.project_root_, error);
        if (error || relative.empty()) {
          return reject("Unable to resolve a relative path for the selection");
        }
        clipboard_text = relative.generic_string();
      } else {
        clipboard_text = path.lexically_normal().string();
      }

      shell_.WriteClipboardText(clipboard_text);
      return DispatchResult::Handled;
    }
    case ActionId::Tree: {
      const TreeRootRequest request = BuildTreeRootRequest(args);
      if (request.root.empty() && shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.ShowTreeSidebar(request.root);
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteSearch(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  (void) source;
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::Term:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.OpenTerminal(JoinCommandArguments(args, 0));
      return DispatchResult::Handled;
    case ActionId::Find:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.file_index_.Refresh();
      shell_.file_finder_.SetIndex(&shell_.file_index_);
      shell_.file_finder_.SetQuery(JoinCommandArguments(args, 0));
      shell_.ShowOverlay(OverlayMode::FileFinder);
      return DispatchResult::Handled;
    case ActionId::Files: {
      const FilesRequest request = BuildFilesRequest(args);
      if (!request.project_root.empty() && !shell_.OpenProjectTab(request.project_root, true, true)) {
        return reject("Failed to open project: " + request.project_root.string());
      }
      if (source == ActionSource::Shortcut && shell_.surface_.overlay_visible) {
        shell_.DismissOverlay();
        return DispatchResult::Handled;
      }
      if (source != ActionSource::Shortcut && shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.ShowOverlay(OverlayMode::FileFinder);
      shell_.file_index_.Refresh();
      shell_.file_finder_.SetIndex(&shell_.file_index_);
      shell_.file_finder_.SetQuery("");
      return DispatchResult::Handled;
    }
    case ActionId::ProjectSearch:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.ShowSearchSidebar(JoinCommandArguments(args, 0), true);
      return DispatchResult::Handled;
    case ActionId::Search:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      if (shell_.ActiveTabIsCompare() || shell_.ActiveTabIsMerge()) {
        return reject("search is unavailable in compare and merge tabs");
      }
      shell_.OpenBufferSearch();
      shell_.overlay_workflow_.buffer_search.query = JoinCommandArguments(args, 0);
      shell_.RefreshBufferSearch();
      return DispatchResult::Handled;
    case ActionId::ReplaceInBuffer:
      shell_.OpenBufferReplace();
      return DispatchResult::Handled;
    case ActionId::Compare: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const CompareRequest request = BuildCompareRequest(args, shell_.project_root_);
      std::filesystem::path path = request.path;
      if (path.empty() && source == ActionSource::ContextMenu) {
        path = shell_.ResolveTreeActionPath(source);
      } else if (path.empty() && !shell_.text_viewport_.path().empty()) {
        path = shell_.text_viewport_.path().lexically_normal();
      } else if (path.empty() && shell_.surface_.sidebar_visible &&
                 shell_.surface_.sidebar_mode == SidebarMode::Tree) {
        const auto& entries = shell_.directory_tree_.entries();
        if (shell_.directory_tree_.selected_index() < entries.size() &&
            !entries[shell_.directory_tree_.selected_index()].is_directory) {
          path = entries[shell_.directory_tree_.selected_index()].path.lexically_normal();
        }
      }

      if (path.empty()) {
        return reject("No file selected for compare");
      }
      if (!std::filesystem::exists(path)) {
        return reject("Compare path does not exist: " + path.string());
      }

      shell_.OpenComparePickerForPath(path, request.commit_spec);
      return DispatchResult::Handled;
    }
    case ActionId::CompareHead: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No file selected for compare-head");
      }
      if (!std::filesystem::exists(path)) {
        return reject("Compare path does not exist: " + path.string());
      }
      shell_.overlay_workflow_.compare_picker.path = path.lexically_normal();
      shell_.OpenComparison(project::GitCommitEntry{
          .hash = "HEAD",
          .short_hash = "HEAD",
          .subject = "HEAD",
      });
      return DispatchResult::Handled;
    }
    case ActionId::Merge: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::optional<MergeRequest> request = BuildMergeRequest(args, shell_.project_root_);
      if (!request.has_value()) {
        return reject("merge requires base, incoming, current, and optional output paths");
      }
      if (!std::filesystem::exists(request->base_path) ||
          !std::filesystem::exists(request->incoming_path) ||
          !std::filesystem::exists(request->current_path)) {
        return reject("merge requires existing base, incoming, and current files");
      }
      shell_.OpenMergeEditor(request->base_path, request->incoming_path, request->current_path,
                      request->output_path);
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteTab(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::Open: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::optional<OpenPathRequest> request = BuildOpenPathRequest(args, shell_.project_root_);
      if (!request.has_value()) {
        return reject("open requires a path");
      }
      const std::filesystem::path& path = request->path;

      auto* editor_tab = shell_.ActiveEditorTab();
      if (editor_tab != nullptr && editor_tab->views.size() > 1) {
        editor::TextViewport opened_view;
        if (!opened_view.OpenFile(path)) {
          return reject("Failed to open file: " + path.string());
        }
        if (!shell_.ReplaceActiveEditorView(opened_view)) {
          return reject("Failed to replace the active split with: " + path.string());
        }
        return DispatchResult::Handled;
      }
      shell_.OpenFile(path);
      return DispatchResult::Handled;
    }
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      if (id == ActionId::OpenSelectedTreeItemInNewTab) {
        if (!shell_.OpenFileInNewTab(path)) {
          return reject("Failed to open file in a new tab: " + path.string());
        }
      } else {
        shell_.OpenFile(path);
      }
      return DispatchResult::Handled;
    }
    case ActionId::Tab:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      {
        const TabPathsRequest request = BuildTabPathsRequest(args, shell_.project_root_);
        if (request.open_untitled) {
          shell_.OpenUntitledTab();
          return DispatchResult::Handled;
        }

        for (const std::filesystem::path& path : request.paths) {
          if (!shell_.OpenFileInNewTab(path)) {
            return reject("Failed to open file in a new tab: " + path.string());
          }
        }

        return DispatchResult::Handled;
      }
    case ActionId::TabSwitch: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      std::string error_message;
      const TabSwitchRequest request = BuildTabSwitchRequest(args);
      const std::optional<std::size_t> tab_index =
          shell_.FindTabIndexBySpecifier(request.specifier, &error_message);
      if (!tab_index.has_value()) {
        return reject(error_message.empty() ? "No matching tab" : error_message);
      }
      shell_.ActivateTab(*tab_index);
      return DispatchResult::Handled;
    }
    case ActionId::TabMove:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      if (shell_.open_tabs_.empty()) {
        return reject("No open tabs");
      }
      {
        const std::optional<TabMoveRequest> request = BuildTabMoveRequest(args);
        if (!request.has_value()) {
          return reject("tabmove requires a tab slot or relative offset");
        }
        const int current_slot = static_cast<int>(shell_.active_tab_index_) + 1;
        const int requested_slot = request->relative ? current_slot + request->slot : request->slot;
        const int clamped_slot =
            std::clamp(requested_slot, 1, static_cast<int>(shell_.open_tabs_.size()));
        shell_.MoveActiveTabTo(static_cast<std::size_t>(clamped_slot - 1));
        return DispatchResult::Handled;
      }
    case ActionId::Reopen:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.ReopenActiveTab();
      return DispatchResult::Handled;
    case ActionId::Save:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      if (shell_.SaveTab(shell_.active_tab_index_)) {
        if (source == ActionSource::Shortcut) {
          shell_.ResetCaretBlink();
        }
      } else {
        return reject("Save failed");
      }
      return DispatchResult::Handled;
    case ActionId::Vsplit: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const EditorSplitOrientation orientation = EditorSplitOrientation::Vertical;
      const TabPathsRequest request = BuildTabPathsRequest(args, shell_.project_root_);

      if (request.open_untitled) {
        shell_.SplitActiveEditor(orientation);
        return DispatchResult::Handled;
      }

      for (const std::filesystem::path& path : request.paths) {
        editor::TextViewport opened_view;
        if (!opened_view.OpenFile(path)) {
          return reject("Failed to open file: " + path.string());
        }
        if (!shell_.SplitActiveEditor(orientation)) {
          return reject("Failed to split the active editor");
        }
        if (!shell_.ReplaceActiveEditorView(opened_view)) {
          return reject("Failed to replace the active split with: " + path.string());
        }
      }

      return DispatchResult::Handled;
    }
    case ActionId::Unsplit:
      shell_.UnsplitActiveEditor();
      return DispatchResult::Handled;
    case ActionId::SplitNext:
      shell_.CycleEditorSplit(1);
      return DispatchResult::Handled;
    case ActionId::SplitPrev:
      shell_.CycleEditorSplit(-1);
      return DispatchResult::Handled;
    case ActionId::SplitFirst:
      shell_.ActivateOrderedEditorSplit(0);
      return DispatchResult::Handled;
    case ActionId::SplitLast: {
      auto* editor_tab = shell_.ActiveEditorTab();
      const std::size_t last_index =
          editor_tab == nullptr || editor_tab->views.empty() ? 0 : editor_tab->views.size() - 1;
      shell_.ActivateOrderedEditorSplit(last_index);
      return DispatchResult::Handled;
    }
    case ActionId::CloseActiveTab:
      if (!shell_.open_tabs_.empty()) {
        shell_.RequestCloseTab(shell_.active_tab_index_);
      }
      return DispatchResult::Handled;
    case ActionId::CloseAllTabs:
      shell_.CloseAllTabs();
      return DispatchResult::Handled;
    case ActionId::CloseOtherTabs: {
      std::vector<std::size_t> indices;
      if (!shell_.open_tabs_.empty()) {
        indices.reserve(shell_.open_tabs_.size() - 1);
      }
      for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
        if (i != shell_.active_tab_index_) {
          indices.push_back(i);
        }
      }
      shell_.RequestCloseTabs(std::move(indices));
      return DispatchResult::Handled;
    }
    case ActionId::CloseTabsToRight: {
      std::vector<std::size_t> indices;
      for (std::size_t i = shell_.active_tab_index_ + 1; i < shell_.open_tabs_.size(); ++i) {
        indices.push_back(i);
      }
      shell_.RequestCloseTabs(std::move(indices));
      return DispatchResult::Handled;
    }
    case ActionId::CloseTabsToLeft: {
      std::vector<std::size_t> indices;
      indices.reserve(shell_.active_tab_index_);
      for (std::size_t i = 0; i < shell_.active_tab_index_; ++i) {
        indices.push_back(i);
      }
      shell_.RequestCloseTabs(std::move(indices));
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteEdit(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  (void) source;
  (void) rejection_feedback;

  switch (id) {
    case ActionId::Goto:
    case ActionId::Jump: {
      if (shell_.ActiveTabIsCompare() || shell_.ActiveTabIsMerge()) {
        return DispatchResult::Handled;
      }
      const std::optional<LineNavigationRequest> request =
          BuildLineNavigationRequest(args, id == ActionId::Jump);
      if (!request.has_value()) {
        return DispatchResult::Handled;
      }

      if (id == ActionId::Goto && request->requested_line == 0) {
        return DispatchResult::Handled;
      }

      const std::size_t line_count = std::max<std::size_t>(1, shell_.text_viewport_.line_count());
      std::size_t line = 0;
      if (id == ActionId::Jump) {
        const long long current_line = static_cast<long long>(shell_.text_viewport_.cursor_line()) + 1;
        const long long target_line = current_line + request->requested_line;
        line = static_cast<std::size_t>(
            std::clamp(target_line - 1, 0LL, static_cast<long long>(line_count - 1)));
      } else if (request->requested_line > 0) {
        line = static_cast<std::size_t>(request->requested_line - 1);
      } else {
        const std::size_t from_end = static_cast<std::size_t>(-request->requested_line);
        line = from_end >= line_count ? 0 : line_count - from_end;
      }

      shell_.text_viewport_.MoveCursorTo(line, request->column > 0 ? request->column - 1 : 0);
      shell_.surface_.focus = FocusTarget::Editor;
      shell_.RequestFocusedEditorRedraw();
      return DispatchResult::Handled;
    }
    case ActionId::SelectAll:
      if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
        viewport->SelectAll();
        shell_.ResetCaretBlink();
        shell_.RequestFocusedEditorRedraw();
      }
      shell_.surface_.focus = FocusTarget::Editor;
      return DispatchResult::Handled;
    case ActionId::Undo:
      if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
        const bool was_dirty = viewport->dirty();
        const std::vector<std::string> before_lines = viewport->lines();
        const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
        const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
        if (viewport->Undo()) {
          if (auto* compare_tab = shell_.ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            shell_.RefreshCompareTabDerivedState(*compare_tab);
            shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = shell_.ActiveMergeTab(); merge_tab != nullptr &&
                                                    viewport == &merge_tab->result_viewport) {
            shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                 cursor_before);
          }
          shell_.ResetCaretBlink();
          shell_.RequestActiveTabRedraw(false);
          shell_.RequestFocusedEditorRedraw();
          shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
          if (viewport->dirty() != was_dirty) {
            shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                        viewport->cursor_line());
            shell_.RequestTabStripRedraw();
          }
        }
      }
      return DispatchResult::Handled;
    case ActionId::Redo:
      if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
        const bool was_dirty = viewport->dirty();
        const std::vector<std::string> before_lines = viewport->lines();
        const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
        const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
        if (viewport->Redo()) {
          if (auto* compare_tab = shell_.ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            shell_.RefreshCompareTabDerivedState(*compare_tab);
            shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = shell_.ActiveMergeTab(); merge_tab != nullptr &&
                                                    viewport == &merge_tab->result_viewport) {
            shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                 cursor_before);
          }
          shell_.ResetCaretBlink();
          shell_.RequestActiveTabRedraw(false);
          shell_.RequestFocusedEditorRedraw();
          shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
          if (viewport->dirty() != was_dirty) {
            shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                        viewport->cursor_line());
            shell_.RequestTabStripRedraw();
          }
        }
      }
      return DispatchResult::Handled;
    case ActionId::CopySelection: {
      std::string text;
      if (shell_.surface_.focus == FocusTarget::Panel && shell_.TerminalHasSelection()) {
        text = shell_.SelectedTerminalText();
      } else if (shell_.ActiveEditableViewport() != nullptr) {
        text = shell_.ActiveEditableViewport()->SelectedText();
      }
      if (!text.empty()) {
        shell_.WriteClipboardText(text);
        shell_.WritePrimarySelectionText(text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopyLastTerminalCommand: {
      const std::optional<std::string> text = shell_.LastTerminalCommandText();
      if (text.has_value()) {
        shell_.WriteClipboardText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopySelectionWithContext: {
      const std::optional<std::string> text = shell_.SelectionTextWithContext();
      if (text.has_value()) {
        shell_.WriteClipboardText(*text);
        shell_.WritePrimarySelectionText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CutSelection: {
      if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
        const bool was_dirty = viewport->dirty();
        const std::string text = viewport->SelectedText();
        if (!text.empty() && shell_.WriteClipboardText(text)) {
          shell_.WritePrimarySelectionText(text);
          const std::vector<std::string> before_lines = viewport->lines();
          const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
          const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
          viewport->DeleteSelectedText();
          if (auto* compare_tab = shell_.ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            shell_.RefreshCompareTabDerivedState(*compare_tab);
            shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = shell_.ActiveMergeTab(); merge_tab != nullptr &&
                                                    viewport == &merge_tab->result_viewport) {
            shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                 cursor_before);
          }
          shell_.ResetCaretBlink();
          shell_.RequestActiveTabRedraw(false);
          shell_.RequestFocusedEditorRedraw();
          shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
          if (viewport->dirty() != was_dirty) {
            shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                        viewport->cursor_line());
            shell_.RequestTabStripRedraw();
          }
        }
      }
      return DispatchResult::Handled;
    }
    case ActionId::PasteClipboard: {
      if (const std::optional<std::string> clipboard_text = shell_.ReadClipboardText();
          clipboard_text.has_value()) {
        if (shell_.surface_.focus == FocusTarget::Panel && shell_.ActiveTerminalTab() != nullptr) {
          shell_.PasteClipboardIntoTerminal();
        } else if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
          const bool was_dirty = viewport->dirty();
          const std::vector<std::string> before_lines = viewport->lines();
          const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
          const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
          viewport->InsertText(*clipboard_text);
          if (auto* compare_tab = shell_.ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            shell_.RefreshCompareTabDerivedState(*compare_tab);
            shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = shell_.ActiveMergeTab(); merge_tab != nullptr &&
                                                    viewport == &merge_tab->result_viewport) {
            shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                 cursor_before);
          }
          shell_.ResetCaretBlink();
          shell_.RequestActiveTabRedraw(false);
          shell_.RequestFocusedEditorRedraw();
          shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
          if (viewport->dirty() != was_dirty) {
            shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                        viewport->cursor_line());
            shell_.RequestTabStripRedraw();
          }
        }
      }
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteGlobal(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  (void) source;
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };
  PersistenceCoordinator persistence(shell_);

  switch (id) {
    case ActionId::Colorscheme: {
      const std::optional<ColorschemeRequest> request = BuildColorschemeRequest(args);
      if (!request.has_value()) {
        return DispatchResult::Handled;
      }
      if (request->list) {
        persistence.RefreshAvailableColorschemeNames();
        return DispatchResult::Handled;
      }
      persistence.RefreshAvailableColorschemeNames();
      persistence.ApplyColorscheme(request->name, true, true);
      return DispatchResult::Handled;
    }
    case ActionId::TabSize: {
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("tab-size requires an integer from 1 to 16");
      }
      shell_.editor_preferences_.tab_size = request->value;
      shell_.ApplyEditorPreferencesToAllTabs();
      persistence.SaveConfigState();
      return DispatchResult::Handled;
    }
    case ActionId::IndentWidth: {
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("indent-width requires an integer from 1 to 16");
      }
      shell_.editor_preferences_.indent_width = request->value;
      shell_.ApplyEditorPreferencesToAllTabs();
      persistence.SaveConfigState();
      return DispatchResult::Handled;
    }
    case ActionId::UiScale: {
      const std::optional<UiScaleRequest> request = BuildUiScaleRequest(args);
      if (!request.has_value()) {
        return reject("ui-scale requires a preset or numeric value");
      }
      switch (request->kind) {
        case UiScaleRequest::Kind::Step:
          persistence.ApplyUiScale(StepUiScale(shell_.ui_scale_, request->delta), true, true);
          break;
        case UiScaleRequest::Kind::Reset:
          persistence.ApplyUiScale(1.0f, true, true);
          break;
        case UiScaleRequest::Kind::Direct:
          persistence.ApplyUiScale(request->scale, true, true);
          break;
      }
      return DispatchResult::Handled;
    }
    case ActionId::SoftTabs: {
      const std::optional<SoftTabsRequest> request = BuildSoftTabsRequest(args);
      if (!request.has_value()) {
        return reject("soft-tabs expects on or off");
      }
      shell_.editor_preferences_.soft_tabs = request->enabled;
      shell_.ApplyEditorPreferencesToAllTabs();
      persistence.SaveConfigState();
      return DispatchResult::Handled;
    }
    case ActionId::Focus: {
      const FocusRequest request = BuildFocusRequest(args);
      switch (request.target) {
        case FocusRequestTarget::Sidebar:
          if (shell_.surface_.sidebar_visible) {
            shell_.surface_.focus = FocusTarget::Sidebar;
            return DispatchResult::Handled;
          }
          break;
        case FocusRequestTarget::Editor:
          shell_.surface_.focus = FocusTarget::Editor;
          return DispatchResult::Handled;
        case FocusRequestTarget::Panel:
          if (shell_.surface_.command_mode || shell_.ActiveTerminalTab() != nullptr) {
            shell_.surface_.focus = FocusTarget::Panel;
            return DispatchResult::Handled;
          }
          break;
        case FocusRequestTarget::Unknown:
          break;
      }
      return reject("Cannot focus target: " +
                    (request.raw_target.empty() ? std::string("<empty>") : request.raw_target));
    }
    case ActionId::OpenCommandPrompt: {
        const bool bottom_panel_was_visible = shell_.BottomPanelVisible();
        shell_.surface_.command_mode = true;
        shell_.surface_.focus = FocusTarget::Panel;
        shell_.command_.input.clear();
        CommandPromptCoordinator(shell_).ResetSessionState();
        shell_.RequestCommandModeTransitionRedraw(bottom_panel_was_visible);
      }
      return DispatchResult::Handled;
    case ActionId::PluginsReload:
      if (!shell_.plugin_host_.enabled()) {
        return reject("Lua plugin runtime unavailable");
      }
      shell_.ReloadPluginsForCurrentProject();
      CommandPromptCoordinator(shell_).SetFeedback(shell_.PluginRuntimeReloadSummary());
      return DispatchResult::Handled;
    case ActionId::Quit:
      shell_.RequestQuit();
      return DispatchResult::Handled;
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
