#include "workspace/services/StatusBarModelService.h"

#include "workspace/git/GitSidebarCommandCenter.h"

#include <algorithm>
#include <string_view>

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microide::workspace {

void StatusBarModelService::Refresh(StatusBarService& status_bar_service,
                                    const Operations& operations,
                                    const ProjectWorkspaceState& project_state,
                                    const editor::TextViewport* active_viewport) {
  util::AddPerformanceCounter(util::PerfCounterId::FrameRefreshStatusBarCalls);

  StatusBarService::StatusBarSegmentUpdate project_segment;
  StatusBarService::StatusBarSegmentUpdate branch_segment;
  StatusBarService::StatusBarSegmentUpdate worktrees_segment;
  if (!project_state.root.empty()) {
    if (git_derived_cache_.raw_root != project_state.root) {
      git_derived_cache_.raw_root = project_state.root;
      git_derived_cache_.normalized_root = project_state.root.lexically_normal();
      git_derived_cache_.worktree_scan_valid = false;
    }
    const std::filesystem::path& project_root = git_derived_cache_.normalized_root;
    const auto& git_state = project_state.sidebar.git;
    const bool has_git_snapshot = git_state.repo_available || !git_state.branch_label.empty() ||
                                  !git_state.base_ref.empty() || !git_state.base_label.empty() ||
                                  !git_state.entries.empty();
    if (!git_derived_cache_.worktree_scan_valid ||
        git_derived_cache_.snapshot_generation != git_state.snapshot_generation ||
        git_derived_cache_.entry_count != git_state.entries.size()) {
      git_derived_cache_.snapshot_generation = git_state.snapshot_generation;
      git_derived_cache_.entry_count = git_state.entries.size();
      git_derived_cache_.has_worktree_changes = std::any_of(
          git_state.entries.begin(), git_state.entries.end(), [](const GitSidebarEntry& entry) {
            return IsGitWorkflowSection(entry.section);
          });
      git_derived_cache_.worktree_scan_valid = true;
    }
    const bool snapshot_has_worktree_changes = git_derived_cache_.has_worktree_changes;
    const bool tree_has_worktree_changes =
        !has_git_snapshot && project_state.directory_tree.has_dirty_files();
    const bool has_worktree_changes = snapshot_has_worktree_changes || tree_has_worktree_changes;
    bool repo_available = git_state.repo_available;
    project::GitAvailability availability = repo_available
                                                ? project::GitAvailability::Repository
                                                : project::GitAvailability::Unknown;
    const bool refresh_failed = !git_state.refresh_error.empty();
    if (refresh_failed) {
      availability = project::GitAvailability::Unknown;
      repo_available = false;
    } else if (!repo_available) {
      if (!marker_probe_cache_.valid ||
          marker_probe_cache_.project_root != project_root ||
          marker_probe_cache_.marker_generation != git_state.repository_marker_generation) {
        marker_probe_cache_.project_root = project_root;
        marker_probe_cache_.marker_generation = git_state.repository_marker_generation;
        marker_probe_cache_.availability = operations.git_availability
                                               ? operations.git_availability(project_root)
                                               : project::GitAvailability::Unknown;
        marker_probe_cache_.valid = true;
      }
      availability = marker_probe_cache_.availability;
      repo_available = availability == project::GitAvailability::Repository;
    }
    const std::string_view cleanliness =
        availability == project::GitAvailability::Unknown
            ? "scm-unknown"
            : (repo_available ? (has_worktree_changes ? "dirty" : "clean") : "no-scm");
    std::string_view branch_label =
        refresh_failed ? std::string_view() : std::string_view(project_state.sidebar.git.branch_label);
    if (branch_label.empty() && repo_available) {
      branch_label = git_state.base_label;
    }
    if (branch_label.empty() && repo_available) {
      branch_label = git_state.base_ref;
    }
    if (branch_label.empty() && repo_available && operations.read_head_branch) {
      if (!head_branch_cache_.valid || head_branch_cache_.project_root != project_root ||
          head_branch_cache_.marker_generation != git_state.repository_marker_generation) {
        head_branch_cache_.project_root = project_root;
        head_branch_cache_.marker_generation = git_state.repository_marker_generation;
        head_branch_cache_.branch = operations.read_head_branch(project_root).value_or(std::string{});
        head_branch_cache_.valid = true;
      }
      branch_label = head_branch_cache_.branch;
    }
    if (branch_label.empty()) {
      branch_label = repo_available ? "detached"
                     : availability == project::GitAvailability::Unknown ? "scm-unknown"
                                                                         : "no-scm";
    }

    const bool cache_hit = project_segment_cache_.valid &&
                           project_segment_cache_.branch_label == branch_label &&
                           project_segment_cache_.cleanliness == cleanliness;
    if (!cache_hit) {
      project_segment_cache_.branch_label.assign(branch_label);
      project_segment_cache_.cleanliness.assign(cleanliness);
      if (branch_label == cleanliness) {
        project_segment_cache_.text.assign(branch_label);
      } else {
        project_segment_cache_.text.clear();
        project_segment_cache_.text.reserve(branch_label.size() + cleanliness.size() + 3);
        project_segment_cache_.text.append(branch_label);
        project_segment_cache_.text.append(" [");
        project_segment_cache_.text.append(cleanliness);
        project_segment_cache_.text.append("]");
      }
      project_segment_cache_.tooltip.clear();
      project_segment_cache_.tooltip.reserve(cleanliness.size() + 22);
      project_segment_cache_.tooltip.append("Open Source Control (");
      project_segment_cache_.tooltip.append(cleanliness);
      project_segment_cache_.tooltip.append(")");
      project_segment_cache_.valid = true;
    }
    project_segment.text = project_segment_cache_.text;
    project_segment.tooltip = project_segment_cache_.tooltip;
    project_segment.visible = true;
    project_segment.command = "sidebar-show";
    project_segment.command_arg = "git";
    branch_segment = {};
    if (repo_available && !refresh_failed && git_state.worktree_count > 1) {
      if (worktree_segment_cache_.count != git_state.worktree_count) {
        worktree_segment_cache_.count = git_state.worktree_count;
        worktree_segment_cache_.text.clear();
        util::AppendUnsigned(worktree_segment_cache_.text, git_state.worktree_count);
        worktree_segment_cache_.text.append(" worktrees");
      }
      worktrees_segment.text = worktree_segment_cache_.text;
      worktrees_segment.tooltip = "Switch Worktree…";
      worktrees_segment.command = "git-worktrees";
      worktrees_segment.visible = true;
    }
  }
  if (project_state.root.empty()) {
    project_segment_cache_ = {};
    git_derived_cache_ = {};
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Project, project_segment);
  if (!operations.startup_mode_text.empty()) {
    branch_segment.text = operations.startup_mode_text;
    branch_segment.tooltip = operations.startup_mode_tooltip.empty()
                                 ? operations.startup_mode_text
                                 : operations.startup_mode_tooltip;
    branch_segment.visible = true;
    branch_segment.command = {};
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Branch, branch_segment);
  status_bar_service.SetSegment(StatusBarSegmentId::Worktrees, worktrees_segment);

  if (active_viewport != nullptr) {
    const editor::TextViewport* const viewport = active_viewport;
    if (editor_segments_cache_.viewport != viewport ||
        editor_segments_cache_.cursor_line != viewport->cursor_line() ||
        editor_segments_cache_.cursor_column != viewport->cursor_column() ||
        editor_segments_cache_.content_revision != viewport->content_revision() ||
        editor_segments_cache_.line_column_tab_size != viewport->tab_size()) {
      editor_segments_cache_.viewport = viewport;
      editor_segments_cache_.line_column_tab_size = viewport->tab_size();
      editor_segments_cache_.cursor_line = viewport->cursor_line();
      editor_segments_cache_.cursor_column = viewport->cursor_column();
      editor_segments_cache_.content_revision = viewport->content_revision();
      std::string& line_column_text = editor_segments_cache_.line_column_text;
      line_column_text.clear();
      line_column_text.append("Ln ");
      util::AppendUnsigned(line_column_text, viewport->cursor_line() + 1);
      line_column_text.append(", Col ");
      util::AppendUnsigned(line_column_text, viewport->cursor_visual_column() + 1);
    }
    StatusBarService::StatusBarSegmentUpdate line_col;
    line_col.text = editor_segments_cache_.line_column_text;
    line_col.tooltip = "Go to line/column";
    line_col.command = "goto";
    line_col.visible = true;
    status_bar_service.SetSegment(StatusBarSegmentId::LineColumn, line_col);

    if (editor_segments_cache_.viewport != viewport ||
        editor_segments_cache_.soft_tabs != viewport->soft_tabs() ||
        editor_segments_cache_.tab_size != viewport->tab_size()) {
      editor_segments_cache_.viewport = viewport;
      editor_segments_cache_.soft_tabs = viewport->soft_tabs();
      editor_segments_cache_.tab_size = viewport->tab_size();
      std::string& indent_text = editor_segments_cache_.indent_text;
      indent_text.clear();
      indent_text.append(viewport->soft_tabs() ? "Spaces: " : "Tabs: ");
      util::AppendUnsigned(indent_text, viewport->tab_size());
    }
    StatusBarService::StatusBarSegmentUpdate indent;
    indent.text = editor_segments_cache_.indent_text;
    indent.tooltip = "Select indentation";
    indent.command = "indentation";
    indent.visible = true;
    status_bar_service.SetSegment(StatusBarSegmentId::Indent, indent);

    StatusBarService::StatusBarSegmentUpdate language;
    const std::string& filetype = viewport->language_id();
    if (!filetype.empty()) {
      if (editor_segments_cache_.language_tooltip_source != filetype) {
        editor_segments_cache_.language_tooltip_source = filetype;
        editor_segments_cache_.language_tooltip.clear();
        editor_segments_cache_.language_tooltip.reserve(filetype.size() + 10);
        editor_segments_cache_.language_tooltip.append("Language: ");
        editor_segments_cache_.language_tooltip.append(filetype);
      }
      language.text = filetype == "unknown" ? std::string_view("Plain Text") : std::string_view(filetype);
      language.tooltip = editor_segments_cache_.language_tooltip;
      language.command = "language-mode";
      language.visible = true;
    }
    status_bar_service.SetSegment(StatusBarSegmentId::Language, language);

    const std::string encoding_label = viewport->EncodingLabel();
    const std::string line_ending_label = viewport->LineEndingLabel();
    if (editor_segments_cache_.encoding_label != encoding_label) {
      editor_segments_cache_.encoding_label = encoding_label;
    }
    if (editor_segments_cache_.line_ending_label != line_ending_label) {
      editor_segments_cache_.line_ending_label = line_ending_label;
    }
    StatusBarService::StatusBarSegmentUpdate encoding;
    if (!encoding_label.empty()) {
      encoding.text = editor_segments_cache_.encoding_label;
      encoding.tooltip = "File encoding";
      encoding.visible = true;
    }
    status_bar_service.SetSegment(StatusBarSegmentId::Encoding, encoding);
    StatusBarService::StatusBarSegmentUpdate line_ending;
    if (!line_ending_label.empty()) {
      line_ending.text = editor_segments_cache_.line_ending_label;
      line_ending.tooltip = "Select End of Line Sequence";
      line_ending.command = "line-ending";
      line_ending.visible = true;
    }
    status_bar_service.SetSegment(StatusBarSegmentId::LineEnding, line_ending);
  } else {
    editor_segments_cache_.viewport = nullptr;
    editor_segments_cache_.line_column_text.clear();
    editor_segments_cache_.indent_text.clear();
    status_bar_service.ClearSegment(StatusBarSegmentId::LineColumn);
    status_bar_service.ClearSegment(StatusBarSegmentId::Indent);
    status_bar_service.ClearSegment(StatusBarSegmentId::Language);
    status_bar_service.ClearSegment(StatusBarSegmentId::Encoding);
    status_bar_service.ClearSegment(StatusBarSegmentId::LineEnding);
  }

  StatusBarService::StatusBarSegmentUpdate problems;
  {
    const std::size_t errors = project_state.diagnostics_store.ErrorCount();
    const std::size_t warnings = project_state.diagnostics_store.WarningCount();
    if (errors > 0 || warnings > 0) {
      if (editor_segments_cache_.errors != errors || editor_segments_cache_.warnings != warnings) {
        editor_segments_cache_.errors = errors;
        editor_segments_cache_.warnings = warnings;
        editor_segments_cache_.problems_text = ProblemsSegmentText(errors, warnings);
      }
      problems.text = editor_segments_cache_.problems_text;
      problems.tooltip = "Open Problems";
      problems.command = "sidebar-show";
      problems.command_arg = "problems";
      problems.visible = true;
      problems.tone =
          errors > 0 ? StatusTone::Error : StatusTone::Warning;
    } else {
      editor_segments_cache_.errors = 0;
      editor_segments_cache_.warnings = 0;
      editor_segments_cache_.problems_text.clear();
    }
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Problems, problems);

  StatusBarService::StatusBarSegmentUpdate lsp;
  if (active_viewport != nullptr) {
    editor_segments_cache_.lsp_text.clear();
    editor_segments_cache_.lsp_tooltip.clear();
    StatusTone lsp_tone = StatusTone::Default;
    operations.active_lsp_status_strings(false, editor_segments_cache_.lsp_text,
                                         editor_segments_cache_.lsp_tooltip, lsp_tone);
    if (!editor_segments_cache_.lsp_text.empty()) {
      lsp.tone = lsp_tone;
      lsp.text = editor_segments_cache_.lsp_text;
      lsp.tooltip = editor_segments_cache_.lsp_tooltip;
      lsp.visible = true;
    }
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Lsp, lsp);
}

std::string ProblemsSegmentText(std::size_t errors, std::size_t warnings) {
  return std::to_string(errors) + (errors == 1 ? " error, " : " errors, ") + std::to_string(warnings) +
         (warnings == 1 ? " warning" : " warnings");
}

}
