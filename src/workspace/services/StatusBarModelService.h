#pragma once

#include <cstdint>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewport.h"
#include "workspace/services/LayoutModeService.h"
#include "workspace/services/StatusBarService.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

class StatusBarModelService {
 public:
  struct Operations {
    std::function<bool(const std::filesystem::path&)> is_git_repo_valid;
    // Branch name straight out of `<gitdir>/HEAD` (no subprocess), used only while
    // no `git status` snapshot has arrived yet. Result is cached per project root.
    std::function<std::optional<std::string>(const std::filesystem::path&)> read_head_branch;
    // Fills text + tooltip and, in the fourth argument, the semantic tone derived
    // from typed LSP readiness state (so the model never re-scans the label text).
    std::function<void(bool, std::string&, std::string&, StatusBarSegmentTone&)>
        active_lsp_status_strings;
    std::string_view startup_mode_text;
    std::string_view startup_mode_tooltip;
  };

  void Refresh(StatusBarService& status_bar_service,
               const Operations& operations,
               const ProjectWorkspaceState& project_state,
               const editor::TextViewport* active_viewport);

 private:
  // `<gitdir>/HEAD` is only consulted while there is no git snapshot, but the
  // status bar rebuilds every frame — so remember the answer per project root
  // instead of re-reading the file on each one. Any HEAD movement lands a real
  // snapshot through GitRepositoryMetadataTracker, which supersedes this.
  struct HeadBranchCache {
    std::filesystem::path project_root;
    std::string branch;
    bool valid = false;
  };

  struct ProjectSegmentCache {
    std::string branch_label;
    std::string cleanliness;
    std::string text;
    std::string tooltip;
    bool valid = false;
  };

  // The `.git` availability probe, keyed on the project root AND the repository
  // marker generation. The generation is what makes caching this safe: it moves
  // on every repository change, including `.git` appearing or disappearing, so
  // an in-session `git init` is still reflected — without a stat per painted
  // frame (TD-2026-08-06-158).
  struct MarkerProbeCache {
    std::filesystem::path project_root;
    std::uint64_t marker_generation = 0;
    bool present = false;
    bool valid = false;
  };

  HeadBranchCache head_branch_cache_;
  MarkerProbeCache marker_probe_cache_;

  // Two per-frame costs this refresh used to pay unconditionally, both keyed on
  // things that move far less often than once a frame:
  //
  //  - `root.lexically_normal()`, which allocates a whole path every frame for a
  //    project root that changes only when the user switches projects
  //  - a scan of every git sidebar entry looking for a worktree change, which on
  //    a 1000-changed-file repository is 1000 iterations per frame for an answer
  //    that only moves when a new git snapshot lands
  struct GitSnapshotDerivedCache {
    std::filesystem::path raw_root;
    std::filesystem::path normalized_root;
    std::uint64_t snapshot_generation = 0;
    std::size_t entry_count = 0;
    bool has_worktree_changes = false;
    bool worktree_scan_valid = false;
  };
  GitSnapshotDerivedCache git_derived_cache_;

  struct EditorSegmentsCache {
    const editor::TextViewport* viewport = nullptr;
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
    // The visual column depends on the bytes before the caret, so an edit that
    // leaves the caret where it is (undo, a multi-caret edit) still re-composes.
    std::uint64_t content_revision = 0;
    bool soft_tabs = false;
    std::size_t tab_size = 0;
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::string line_column_text;
    std::string indent_text;
    std::string problems_text;
    // Composed segment strings and the inputs they were composed from. All three
    // used to be rebuilt on every painted frame from values that move only when
    // the file's language, encoding or line ending does.
    std::string language_tooltip_source;
    std::string language_tooltip;
    std::string encoding_source_left;
    std::string encoding_source_right;
    std::string encoding_text;
    // The LSP readout, filled by the operations callback. Held so the callback
    // assigns into buffers that already have capacity instead of two fresh
    // strings per frame.
    std::string lsp_text;
    std::string lsp_tooltip;
  };

  ProjectSegmentCache project_segment_cache_;
  EditorSegmentsCache editor_segments_cache_;
};

}  // namespace microide::workspace
