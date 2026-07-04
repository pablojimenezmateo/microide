#include "workspace/WorkspaceActionRequests.h"

#include <algorithm>
#include <string_view>

#include "util/Parse.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceTextSearch.h"

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
  const std::optional<std::int64_t> parsed_line = util::ParseInt64(location.substr(0, colon));
  if (!parsed_line.has_value()) {
    return false;
  }
  std::size_t parsed_column = 0;
  if (colon != std::string_view::npos) {
    const std::optional<std::size_t> column_value = util::ParseSize(location.substr(colon + 1));
    if (!column_value.has_value()) {
      return false;
    }
    parsed_column = *column_value;
  }

  if (!allow_zero_line && *parsed_line == 0) {
    return false;
  }

  *line = static_cast<long long>(*parsed_line);
  *column = parsed_column;
  return true;
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
  return util::ParseFloat(text);
}

std::optional<int> ParseIntArgument(const std::string& text) {
  return util::ParseInt(text);
}

std::optional<std::size_t> ParseClampedSizeArgument(const std::string& text,
                                                    std::size_t minimum,
                                                    std::size_t maximum) {
  const std::optional<std::size_t> parsed = util::ParseSize(text);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  return std::clamp(*parsed, minimum, maximum);
}

}  // namespace

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

ProjectCycleRequest BuildProjectCycleRequest(int delta) {
  return ProjectCycleRequest{.delta = delta};
}

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

TreeRootRequest BuildTreeRootRequest(const std::vector<std::string>& args) {
  return TreeRootRequest{
      .root = args.empty() ? std::filesystem::path{} : std::filesystem::path(args[0]),
  };
}

FilesRequest BuildFilesRequest(const std::vector<std::string>& args) {
  return FilesRequest{
      .project_root = args.empty() ? std::filesystem::path{} : std::filesystem::path(args[0]),
  };
}

CompareRequest BuildCompareRequest(const std::vector<std::string>& args,
                                   const std::filesystem::path& project_root) {
  return CompareRequest{
      .path = args.empty() ? std::filesystem::path{}
                           : NormalizeCommandPath(project_root, std::filesystem::path(args[0])),
      .commit_spec = args.size() > 1 ? args[1] : std::string{},
  };
}

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

std::optional<OpenPathRequest> BuildOpenPathRequest(const std::vector<std::string>& args,
                                                    const std::filesystem::path& project_root) {
  if (args.empty()) {
    return std::nullopt;
  }
  return OpenPathRequest{
      .path = NormalizeCommandPath(project_root, std::filesystem::path(args[0])),
  };
}

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

TabSwitchRequest BuildTabSwitchRequest(const std::vector<std::string>& args) {
  return TabSwitchRequest{
      .specifier = JoinCommandArguments(args, 0),
  };
}

std::optional<TabMoveRequest> BuildTabMoveRequest(const std::vector<std::string>& args) {
  if (args.empty() || args[0].empty()) {
    return std::nullopt;
  }
  const std::string& raw = args[0];
  const bool relative = raw.front() == '+' || raw.front() == '-';
  // ParseInt (std::from_chars) rejects a leading '+', so the relative-forward form
  // "+N" must have its sign stripped before parsing; "-N" parses directly as a
  // negative offset and "N" as an absolute slot.
  const std::optional<int> slot =
      raw.front() == '+' ? util::ParseInt(std::string_view(raw).substr(1)) : ParseIntArgument(raw);
  if (!slot.has_value()) {
    return std::nullopt;
  }
  return TabMoveRequest{
      .slot = *slot,
      .relative = relative,
  };
}

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

std::optional<ColorschemeRequest> BuildColorschemeRequest(const std::vector<std::string>& args) {
  if (args.empty()) {
    return std::nullopt;
  }
  return ColorschemeRequest{
      .list = args[0] == "list",
      .name = args[0],
  };
}

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

std::optional<SoftTabsRequest> BuildSoftTabsRequest(const std::vector<std::string>& args) {
  if (args.empty()) {
    return std::nullopt;
  }
  const std::string value = ToLower(args[0]);
  if (value != "on" && value != "off" && value != "true" && value != "false" &&
      value != "1" && value != "0") {
    return std::nullopt;
  }
  return SoftTabsRequest{
      .enabled = value == "on" || value == "true" || value == "1",
  };
}

std::optional<WrapRequest> BuildWrapRequest(const std::vector<std::string>& args) {
  if (args.empty()) {
    return std::nullopt;
  }
  const std::string value = ToLower(args[0]);
  if (value != "on" && value != "off" && value != "true" && value != "false" &&
      value != "1" && value != "0") {
    return std::nullopt;
  }
  return WrapRequest{
      .enabled = value == "on" || value == "true" || value == "1",
  };
}

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

}  // namespace microide::workspace
