#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "compare/CompareModel.h"
#include "compare/CompareReviewTypes.h"
#include "editor/TextViewport.h"
#include "platform/ProcessLauncher.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

struct CompareTabLoadRequest {
  std::filesystem::path path;
  std::filesystem::path root;
  const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
  std::string left_ref;
  std::string right_ref = "WORKTREE";
  compare::CompareBuildOptions build_options;

  [[nodiscard]] bool right_is_working_tree() const {
    return right_ref.empty() || right_ref == "WORKTREE";
  }
};

struct CompareTabLoadResult {
  bool left_ok = false;
  bool right_read = false;
  bool right_ok = false;
  compare::CompareTextBuffer left = compare::EmptyCompareText();
  editor::TextViewport right;
  compare::CompareModel model;
  compare::CompareBuildOptions built_options;
  std::size_t left_line_count = 0;
};

void RunCompareTabLoad(const CompareTabLoadRequest& request, std::string right_bytes,
                       CompareTabLoadResult& result);

struct CompareTabRefreshRequest {
  std::filesystem::path path;
  std::filesystem::path root;
  const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
  std::filesystem::path left_path;
  std::string left_ref;
  bool left_held = false;
  std::filesystem::path right_path;
  std::string right_ref;
};

CompareRevisionSides ReadCompareRevisionSides(const CompareTabRefreshRequest& request);

std::size_t CountCompareTextLines(std::string_view text);

void MarkCompareModelBuilt(CompareTabState& compare_tab, const compare::CompareBuildOptions& built,
                           std::size_t left_line_count);

}
