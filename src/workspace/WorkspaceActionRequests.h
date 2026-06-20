#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::workspace {

struct ProjectOpenRequest {
  bool use_native_picker = false;
  std::filesystem::path path;
};

ProjectOpenRequest BuildProjectOpenRequest(const std::vector<std::string>& args);

struct ProjectCycleRequest {
  int delta = 0;
};

ProjectCycleRequest BuildProjectCycleRequest(int delta);

struct SidebarWidthRequest {
  float width = 0.0f;
};

std::optional<SidebarWidthRequest> BuildSidebarWidthRequest(
    const std::vector<std::string>& args);

struct TreeRootRequest {
  std::filesystem::path root;
};

TreeRootRequest BuildTreeRootRequest(const std::vector<std::string>& args);

struct FilesRequest {
  std::filesystem::path project_root;
};

FilesRequest BuildFilesRequest(const std::vector<std::string>& args);

struct CompareRequest {
  std::filesystem::path path;
  std::string commit_spec;
};

CompareRequest BuildCompareRequest(const std::vector<std::string>& args,
                                   const std::filesystem::path& project_root);

struct MergeRequest {
  std::filesystem::path base_path;
  std::filesystem::path incoming_path;
  std::filesystem::path current_path;
  std::filesystem::path output_path;
};

std::optional<MergeRequest> BuildMergeRequest(const std::vector<std::string>& args,
                                              const std::filesystem::path& project_root);

// Result of a batch review verb (review-conflicts / review-branch / review-commit).
// `message` is the human/agent-facing summary (file list on success, reason on
// failure); it is surfaced as the control-channel feedback/error.
struct ReviewOpenOutcome {
  bool ok = false;
  std::string message;
};

struct OpenPathRequest {
  std::filesystem::path path;
};

std::optional<OpenPathRequest> BuildOpenPathRequest(const std::vector<std::string>& args,
                                                    const std::filesystem::path& project_root);

struct TabPathsRequest {
  bool open_untitled = false;
  std::vector<std::filesystem::path> paths;
};

TabPathsRequest BuildTabPathsRequest(const std::vector<std::string>& args,
                                     const std::filesystem::path& project_root);

struct TabSwitchRequest {
  std::string specifier;
};

TabSwitchRequest BuildTabSwitchRequest(const std::vector<std::string>& args);

struct TabMoveRequest {
  int slot = 0;
  bool relative = false;
};

std::optional<TabMoveRequest> BuildTabMoveRequest(const std::vector<std::string>& args);

struct LineNavigationRequest {
  long long requested_line = 0;
  std::size_t column = 0;
};

std::optional<LineNavigationRequest> BuildLineNavigationRequest(
    const std::vector<std::string>& args,
    bool allow_zero_line);

struct ColorschemeRequest {
  bool list = false;
  std::string name;
};

std::optional<ColorschemeRequest> BuildColorschemeRequest(const std::vector<std::string>& args);

struct EditorPreferenceSizeRequest {
  std::size_t value = 0;
};

std::optional<EditorPreferenceSizeRequest> BuildEditorPreferenceSizeRequest(
    const std::vector<std::string>& args);

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

std::optional<UiScaleRequest> BuildUiScaleRequest(const std::vector<std::string>& args);

struct SoftTabsRequest {
  bool enabled = false;
};

std::optional<SoftTabsRequest> BuildSoftTabsRequest(const std::vector<std::string>& args);

struct WrapRequest {
  bool enabled = false;
};

std::optional<WrapRequest> BuildWrapRequest(const std::vector<std::string>& args);

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

FocusRequest BuildFocusRequest(const std::vector<std::string>& args);

}  // namespace microide::workspace
