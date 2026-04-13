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

enum class SidebarToolKind {
  Default,
  Git,
  Tree,
  Search,
};

struct SidebarToolRequest {
  SidebarToolKind tool = SidebarToolKind::Default;
  std::filesystem::path root;
  std::string query;
};

SidebarToolRequest BuildSidebarToolRequest(const std::vector<std::string>& args) {
  SidebarToolRequest request;
  if (args.empty()) {
    return request;
  }
  if (args[0] == "git") {
    request.tool = SidebarToolKind::Git;
    return request;
  }
  if (args[0] == "tree") {
    request.tool = SidebarToolKind::Tree;
    request.root = args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::path{};
    return request;
  }
  if (args[0] == "search") {
    request.tool = SidebarToolKind::Search;
    request.query = JoinCommandArguments(args, 1);
  }
  return request;
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
    case ActionId::ProjectOpen: {
      const ProjectOpenRequest request = BuildProjectOpenRequest(args);
      if (request.use_native_picker) {
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
      if (!OpenProjectTab(request.path, true, true)) {
        return reject("Failed to open project: " + request.path.string());
      }
      return ActionDispatchResult::Handled;
    }
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
      const ProjectCycleRequest request =
          BuildProjectCycleRequest(id == ActionId::ProjectNext ? 1 : -1);
      const int project_count = static_cast<int>(project_catalog_.entries.size());
      const int next_index =
          (static_cast<int>(project_catalog_.active_index) + request.delta + project_count) %
          project_count;
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
      const SidebarToolRequest request = BuildSidebarToolRequest(args);
      if (request.tool == SidebarToolKind::Git) {
        if (surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Git) {
          CloseSidebar();
        } else {
          ShowGitSidebar();
        }
        return ActionDispatchResult::Handled;
      }
      if (request.tool == SidebarToolKind::Tree) {
        if (surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Tree) {
          CloseSidebar();
        } else {
          ShowTreeSidebar(request.root);
        }
        return ActionDispatchResult::Handled;
      }
      if (request.tool == SidebarToolKind::Search) {
        if (surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Search &&
            !surface_.sidebar_temporary) {
          CloseSidebar();
        } else {
          ShowSearchSidebar(request.query, false);
        }
        return ActionDispatchResult::Handled;
      }
      ToggleSidebar();
      return ActionDispatchResult::Handled;
    }
    case ActionId::SidebarShow: {
      const SidebarToolRequest request = BuildSidebarToolRequest(args);
      if (request.tool == SidebarToolKind::Git) {
        ShowGitSidebar();
        return ActionDispatchResult::Handled;
      }
      if (request.tool == SidebarToolKind::Tree) {
        ShowTreeSidebar(request.root);
        return ActionDispatchResult::Handled;
      }
      if (request.tool == SidebarToolKind::Search) {
        ShowSearchSidebar(request.query, false);
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
    case ActionId::SidebarWidth: {
      const std::optional<SidebarWidthRequest> request = BuildSidebarWidthRequest(args);
      if (!request.has_value()) {
        return reject("sidebar-width requires a numeric width");
      }
      surface_.sidebar_width = ClampSidebarWidth(request->width,
                                                 static_cast<float>(std::max(1, last_window_width_)));
      return ActionDispatchResult::Handled;
    }
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
      const TreeRootRequest request = BuildTreeRootRequest(args);
      if (request.root.empty() && project_root_.empty()) {
        return reject("No active project");
      }
      ShowTreeSidebar(request.root);
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
      const FilesRequest request = BuildFilesRequest(args);
      if (!request.project_root.empty() && !OpenProjectTab(request.project_root, true, true)) {
        return reject("Failed to open project: " + request.project_root.string());
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
      const CompareRequest request = BuildCompareRequest(args, project_root_);
      std::filesystem::path path = request.path;
      if (path.empty() && source == ActionSource::ContextMenu) {
        path = ResolveTreeActionPath(source);
      } else if (path.empty() && !text_viewport_.path().empty()) {
        path = text_viewport_.path().lexically_normal();
      } else if (path.empty() && surface_.sidebar_visible &&
                 surface_.sidebar_mode == SidebarMode::Tree) {
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

      OpenComparePickerForPath(path, request.commit_spec);
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
      const std::optional<MergeRequest> request = BuildMergeRequest(args, project_root_);
      if (!request.has_value()) {
        return reject("merge requires base, incoming, current, and optional output paths");
      }
      if (!std::filesystem::exists(request->base_path) ||
          !std::filesystem::exists(request->incoming_path) ||
          !std::filesystem::exists(request->current_path)) {
        return reject("merge requires existing base, incoming, and current files");
      }
      OpenMergeEditor(request->base_path, request->incoming_path, request->current_path,
                      request->output_path);
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
    case ActionId::Open: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      const std::optional<OpenPathRequest> request = BuildOpenPathRequest(args, project_root_);
      if (!request.has_value()) {
        return reject("open requires a path");
      }
      const std::filesystem::path& path = request->path;

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
      {
        const TabPathsRequest request = BuildTabPathsRequest(args, project_root_);
        if (request.open_untitled) {
          OpenUntitledTab();
          return ActionDispatchResult::Handled;
        }

        for (const std::filesystem::path& path : request.paths) {
          if (!OpenFileInNewTab(path)) {
            return reject("Failed to open file in a new tab: " + path.string());
          }
        }

        return ActionDispatchResult::Handled;
      }
    case ActionId::TabSwitch: {
      if (project_root_.empty()) {
        return reject("No active project");
      }
      std::string error_message;
      const TabSwitchRequest request = BuildTabSwitchRequest(args);
      const std::optional<std::size_t> tab_index =
          FindTabIndexBySpecifier(request.specifier, &error_message);
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
      if (open_tabs_.empty()) {
        return reject("No open tabs");
      }
      {
        const std::optional<TabMoveRequest> request = BuildTabMoveRequest(args);
        if (!request.has_value()) {
          return reject("tabmove requires a tab slot or relative offset");
        }
        const int current_slot = static_cast<int>(active_tab_index_) + 1;
        const int requested_slot = request->relative ? current_slot + request->slot : request->slot;
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
      const TabPathsRequest request = BuildTabPathsRequest(args, project_root_);

      if (request.open_untitled) {
        SplitActiveEditor(orientation);
        return ActionDispatchResult::Handled;
      }

      for (const std::filesystem::path& path : request.paths) {
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
      const std::optional<LineNavigationRequest> request =
          BuildLineNavigationRequest(args, id == ActionId::Jump);
      if (!request.has_value()) {
        return ActionDispatchResult::Handled;
      }

      if (id == ActionId::Goto && request->requested_line == 0) {
        return ActionDispatchResult::Handled;
      }

      const std::size_t line_count = std::max<std::size_t>(1, text_viewport_.line_count());
      std::size_t line = 0;
      if (id == ActionId::Jump) {
        const long long current_line = static_cast<long long>(text_viewport_.cursor_line()) + 1;
        const long long target_line = current_line + request->requested_line;
        line = static_cast<std::size_t>(
            std::clamp(target_line - 1, 0LL, static_cast<long long>(line_count - 1)));
      } else if (request->requested_line > 0) {
        line = static_cast<std::size_t>(request->requested_line - 1);
      } else {
        const std::size_t from_end = static_cast<std::size_t>(-request->requested_line);
        line = from_end >= line_count ? 0 : line_count - from_end;
      }

      text_viewport_.MoveCursorTo(line, request->column > 0 ? request->column - 1 : 0);
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
    case ActionId::Colorscheme: {
      const std::optional<ColorschemeRequest> request = BuildColorschemeRequest(args);
      if (!request.has_value()) {
        return ActionDispatchResult::Handled;
      }
      if (request->list) {
        RefreshAvailableColorschemeNames();
        return ActionDispatchResult::Handled;
      }
      RefreshAvailableColorschemeNames();
      ApplyColorscheme(request->name, true, true);
      return ActionDispatchResult::Handled;
    }
    case ActionId::TabSize: {
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("tab-size requires an integer from 1 to 16");
      }
      editor_preferences_.tab_size = request->value;
      ApplyEditorPreferencesToAllTabs();
      SaveConfigState();
      return ActionDispatchResult::Handled;
    }
    case ActionId::IndentWidth: {
      const std::optional<EditorPreferenceSizeRequest> request =
          BuildEditorPreferenceSizeRequest(args);
      if (!request.has_value()) {
        return reject("indent-width requires an integer from 1 to 16");
      }
      editor_preferences_.indent_width = request->value;
      ApplyEditorPreferencesToAllTabs();
      SaveConfigState();
      return ActionDispatchResult::Handled;
    }
    case ActionId::UiScale: {
      const std::optional<UiScaleRequest> request = BuildUiScaleRequest(args);
      if (!request.has_value()) {
        return reject("ui-scale requires a preset or numeric value");
      }
      switch (request->kind) {
        case UiScaleRequest::Kind::Step:
          ApplyUiScale(StepUiScale(ui_scale_, request->delta), true, true);
          break;
        case UiScaleRequest::Kind::Reset:
          ApplyUiScale(1.0f, true, true);
          break;
        case UiScaleRequest::Kind::Direct:
          ApplyUiScale(request->scale, true, true);
          break;
      }
      return ActionDispatchResult::Handled;
    }
    case ActionId::SoftTabs: {
      const std::optional<SoftTabsRequest> request = BuildSoftTabsRequest(args);
      if (!request.has_value()) {
        return reject("soft-tabs expects on or off");
      }
      editor_preferences_.soft_tabs = request->enabled;
      ApplyEditorPreferencesToAllTabs();
      SaveConfigState();
      return ActionDispatchResult::Handled;
    }
    case ActionId::Focus: {
      const FocusRequest request = BuildFocusRequest(args);
      switch (request.target) {
        case FocusRequestTarget::Sidebar:
          if (surface_.sidebar_visible) {
            surface_.focus = FocusTarget::Sidebar;
            return ActionDispatchResult::Handled;
          }
          break;
        case FocusRequestTarget::Editor:
          surface_.focus = FocusTarget::Editor;
          return ActionDispatchResult::Handled;
        case FocusRequestTarget::Panel:
          if (surface_.command_mode || ActiveTerminalTab() != nullptr) {
            surface_.focus = FocusTarget::Panel;
            return ActionDispatchResult::Handled;
          }
          break;
        case FocusRequestTarget::Unknown:
          break;
      }
      return reject("Cannot focus target: " +
                    (request.raw_target.empty() ? std::string("<empty>") : request.raw_target));
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
