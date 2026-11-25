#pragma once

#include <cstdint>

#include <filesystem>
#include <functional>
#include <optional>
#include <cstddef>
#include <string>

#include "editor/RuntimeSyntaxRegistry.h"
#include "project/GitCommandUtil.h"
#include "editor/TextViewport.h"
#include "workspace/services/LayoutModeService.h"
#include "workspace/services/StatusBarService.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

class StatusBarModelService {
 public:
  struct Operations {
    std::function<project::GitAvailability(const std::filesystem::path&)> git_availability;
    std::function<std::optional<std::string>(const std::filesystem::path&)> read_head_branch;
    std::function<void(bool, std::string&, std::string&, StatusTone&)>
        active_lsp_status_strings;
    std::string_view startup_mode_text;
    std::string_view startup_mode_tooltip;
  };

  void Refresh(StatusBarService& status_bar_service,
               const Operations& operations,
               const ProjectWorkspaceState& project_state,
               const editor::TextViewport* active_viewport);

 private:
  struct HeadBranchCache {
    std::filesystem::path project_root;
    std::uint64_t marker_generation = 0;
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

  struct MarkerProbeCache {
    std::filesystem::path project_root;
    std::uint64_t marker_generation = 0;
    project::GitAvailability availability = project::GitAvailability::Unknown;
    bool valid = false;
  };

  HeadBranchCache head_branch_cache_;
  MarkerProbeCache marker_probe_cache_;

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
    std::uint64_t content_revision = 0;
    std::size_t line_column_tab_size = 0;
    bool soft_tabs = false;
    std::size_t tab_size = 0;
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::string line_column_text;
    std::string indent_text;
    std::string problems_text;
    std::string language_tooltip_source;
    std::string language_tooltip;
    std::string encoding_label;
    std::string line_ending_label;
    std::string lsp_text;
    std::string lsp_tooltip;
  };

  ProjectSegmentCache project_segment_cache_;
  struct WorktreeSegmentCache {
    std::size_t count = 0;
    std::string text;
  } worktree_segment_cache_;
  EditorSegmentsCache editor_segments_cache_;
};

std::string ProblemsSegmentText(std::size_t errors, std::size_t warnings);

}
