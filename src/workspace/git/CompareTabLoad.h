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

// Loading a git-backed working-tree compare OFF the shell thread
// (TD-2026-09-29-312).
//
// The synchronous open did three unbounded things with the window frozen: a git
// spawn piping the whole left blob, a read of the whole working file, and the
// diff. The diff is the one a size threshold cannot dodge: it is O(file) at
// best. This is the worker half: everything it touches is passed in or owned by
// the result, so it is safe on any thread. The shell half — finding the tab,
// installing the result, the presentation and review refresh — stays on the
// shell thread (DiffTabCoordinator::ApplyAsyncCompareLoad).
struct CompareTabLoadRequest {
  std::filesystem::path path;
  std::filesystem::path root;
  // The project's. Must outlive the load; see ProjectWorkspaceState::launcher().
  const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
  std::string left_ref;
  compare::CompareBuildOptions build_options;
};

struct CompareTabLoadResult {
  // False when the left revision could not be read, or came back truncated at
  // the subprocess capture ceiling — the same refusals the synchronous open
  // makes, because a partial blob diffed as the whole file is a wrong answer.
  bool left_ok = false;
  // False when the working file's bytes could not be installed (binary,
  // undecodable). Never the case for a merely ABSENT file: the load is only
  // posted for a file whose size was just read.
  bool right_ok = false;
  compare::CompareTextBuffer left = compare::EmptyCompareText();
  editor::TextViewport right;
  compare::CompareModel model;
  // What the model was built FROM — the options at posting time. A toggle made
  // while the load ran must still read as a change, so the fingerprint records
  // these, not whatever the tab holds when the result lands.
  compare::CompareBuildOptions built_options;
  std::size_t left_line_count = 0;
};

// Worker: read the left revision, install the right side's bytes into a fresh
// viewport, and build the diff. `right_bytes` is the working file, already read.
void RunCompareTabLoad(const CompareTabLoadRequest& request, std::string right_bytes,
                       CompareTabLoadResult& result);

// The left side's line count as the compare gutter sizes it.
std::size_t CountCompareTextLines(std::string_view text);

// Record that `compare_tab.model` was just built from the tab's current two
// sides with `built_ignore_whitespace`: bump the model revision, drop the
// visible-layout cache, and write the fingerprint the derived-state refresh
// checks. Shared by the synchronous rebuild and the asynchronous load so the
// two cannot disagree about what "built" means.
void MarkCompareModelBuilt(CompareTabState& compare_tab, bool built_ignore_whitespace,
                           std::size_t left_line_count);

}  // namespace microide::workspace
