#include "workspace/StatusBarModelService.h"

#include "workspace/GitSidebarCommandCenter.h"

#include <algorithm>
#include <string_view>

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/PerformanceCounters.h"

namespace microide::workspace {

void StatusBarModelService::Refresh(StatusBarService& status_bar_service,
                                    const Operations& operations,
                                    const ProjectWorkspaceState& project_state,
                                    const editor::TextViewport* active_viewport) {
  util::AddPerformanceCounter(util::PerfCounterId::FrameRefreshStatusBarCalls);

  StatusBarSegmentValue project_segment;
  StatusBarSegmentValue branch_segment;
  if (!project_state.root.empty()) {
    const std::filesystem::path project_root = project_state.root.lexically_normal();
    const auto& git_state = project_state.sidebar.git;
    const bool has_git_snapshot = git_state.repo_available || !git_state.branch_label.empty() ||
                                  !git_state.base_ref.empty() || !git_state.base_label.empty() ||
                                  !git_state.entries.empty();
    const bool snapshot_has_worktree_changes = std::any_of(
        git_state.entries.begin(), git_state.entries.end(), [](const GitSidebarEntry& entry) {
          return IsGitWorkflowSection(entry.section);
        });
    const bool tree_has_worktree_changes =
        !has_git_snapshot && project_state.directory_tree.has_dirty_files();
    const bool has_worktree_changes = snapshot_has_worktree_changes || tree_has_worktree_changes;
    bool repo_available = git_state.repo_available;
    if (!repo_available) {
      // is_git_repo_valid is a single cheap `.git` stat (not a subprocess), so it
      // runs directly rather than being cached by project_root — caching saved one
      // stat but went stale after an in-session `git init` / `.git` removal until a
      // real git refresh superseded it. Only reached when there is no git snapshot.
      repo_available = operations.is_git_repo_valid(project_root);
    }
    const std::string_view cleanliness =
        repo_available ? (has_worktree_changes ? "dirty" : "clean") : "no-scm";
    std::string_view branch_label = project_state.sidebar.git.branch_label;
    if (branch_label.empty() && repo_available) {
      branch_label = git_state.base_label;
    }
    if (branch_label.empty() && repo_available) {
      branch_label = git_state.base_ref;
    }
    if (branch_label.empty() && repo_available && operations.read_head_branch) {
      // Nothing has produced a `git status` snapshot yet — which is the normal
      // state for the first seconds after opening a project, and stays the state
      // indefinitely if the user never opens the Source Control view. Falling
      // through to "no-scm" here made a perfectly ordinary git checkout label
      // itself "no-scm [clean]": a contradiction, since only source control can
      // know it is clean. `<gitdir>/HEAD` answers this with one file read.
      if (!head_branch_cache_.valid || head_branch_cache_.project_root != project_root) {
        head_branch_cache_.project_root = project_root;
        head_branch_cache_.branch = operations.read_head_branch(project_root).value_or(std::string{});
        head_branch_cache_.valid = true;
      }
      branch_label = head_branch_cache_.branch;
    }
    if (branch_label.empty()) {
      // A detached HEAD in a real repository still is not "no source control";
      // say what it actually is.
      branch_label = repo_available ? "detached" : "no-scm";
    }

    const bool cache_hit = project_segment_cache_.valid &&
                           project_segment_cache_.branch_label == branch_label &&
                           project_segment_cache_.cleanliness == cleanliness;
    if (!cache_hit) {
      project_segment_cache_.branch_label.assign(branch_label);
      project_segment_cache_.cleanliness.assign(cleanliness);
      if (branch_label == "no-scm" && cleanliness == "no-scm") {
        project_segment_cache_.text = "no-scm";
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
    // Its tooltip has always read "Open Source Control (...)"; now it does.
    project_segment.command = "sidebar-show";
    project_segment.command_arg = "git";
    branch_segment = {};
  }
  if (project_state.root.empty()) {
    project_segment_cache_ = {};
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Project, std::move(project_segment));
  if (!operations.startup_mode_text.empty()) {
    branch_segment.text = std::string(operations.startup_mode_text);
    branch_segment.tooltip = operations.startup_mode_tooltip.empty()
                                 ? std::string(operations.startup_mode_text)
                                 : std::string(operations.startup_mode_tooltip);
    branch_segment.visible = true;
    branch_segment.command.clear();  // startup-mode text is a readout, not a control
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Branch, std::move(branch_segment));

  if (active_viewport != nullptr) {
    const editor::TextViewport* const viewport = active_viewport;
    if (editor_segments_cache_.viewport != viewport ||
        editor_segments_cache_.cursor_line != viewport->cursor_line() ||
        editor_segments_cache_.cursor_column != viewport->cursor_column()) {
      editor_segments_cache_.viewport = viewport;
      editor_segments_cache_.cursor_line = viewport->cursor_line();
      editor_segments_cache_.cursor_column = viewport->cursor_column();
      editor_segments_cache_.line_column_text =
          "Ln " + std::to_string(viewport->cursor_line() + 1) + ", Col " +
          std::to_string(viewport->cursor_column() + 1);
    }
    StatusBarSegmentValue line_col;
    line_col.text = editor_segments_cache_.line_column_text;
    line_col.tooltip = "Go to line/column";
    line_col.command = "goto";
    line_col.visible = true;
    status_bar_service.SetSegment(StatusBarSegmentId::LineColumn, std::move(line_col));

    if (editor_segments_cache_.viewport != viewport ||
        editor_segments_cache_.soft_tabs != viewport->soft_tabs() ||
        editor_segments_cache_.tab_size != viewport->tab_size()) {
      editor_segments_cache_.viewport = viewport;
      editor_segments_cache_.soft_tabs = viewport->soft_tabs();
      editor_segments_cache_.tab_size = viewport->tab_size();
      editor_segments_cache_.indent_text =
          (viewport->soft_tabs() ? "Spaces: " : "Tabs: ") + std::to_string(viewport->tab_size());
    }
    StatusBarSegmentValue indent;
    indent.text = editor_segments_cache_.indent_text;
    indent.tooltip = "Change indent settings";
    indent.command = "settings";
    indent.visible = true;
    status_bar_service.SetSegment(StatusBarSegmentId::Indent, std::move(indent));

    StatusBarSegmentValue language;
    const std::filesystem::path viewport_path = viewport->path().lexically_normal();
    const std::string& filetype = language_memo_.Resolve(
        viewport, viewport_path, viewport->content_revision(), viewport->lines());
    if (!filetype.empty()) {
      language.text = filetype;
      language.tooltip = "Language: " + filetype;
      language.visible = true;
    }
    status_bar_service.SetSegment(StatusBarSegmentId::Language, std::move(language));

    StatusBarSegmentValue encoding;
    const std::string encoding_label = viewport->EncodingLabel();
    const std::string line_ending_label = viewport->LineEndingLabel();
    if (!encoding_label.empty() || !line_ending_label.empty()) {
      if (!encoding_label.empty() && !line_ending_label.empty()) {
        encoding.text = encoding_label + " · " + line_ending_label;
      } else if (!encoding_label.empty()) {
        encoding.text = encoding_label;
      } else {
        encoding.text = line_ending_label;
      }
      encoding.tooltip = "File encoding and line endings";
      encoding.visible = true;
    }
    status_bar_service.SetSegment(StatusBarSegmentId::Encoding, std::move(encoding));
  } else {
    language_memo_.Invalidate();
    editor_segments_cache_.viewport = nullptr;
    editor_segments_cache_.line_column_text.clear();
    editor_segments_cache_.indent_text.clear();
    status_bar_service.SetSegment(StatusBarSegmentId::LineColumn, StatusBarSegmentValue{});
    status_bar_service.SetSegment(StatusBarSegmentId::Indent, StatusBarSegmentValue{});
    status_bar_service.SetSegment(StatusBarSegmentId::Language, StatusBarSegmentValue{});
    status_bar_service.SetSegment(StatusBarSegmentId::Encoding, StatusBarSegmentValue{});
  }

  StatusBarSegmentValue problems;
  {
    const std::size_t errors = project_state.diagnostics_store.ErrorCount();
    const std::size_t warnings = project_state.diagnostics_store.WarningCount();
    if (errors > 0 || warnings > 0) {
      if (editor_segments_cache_.errors != errors || editor_segments_cache_.warnings != warnings) {
        editor_segments_cache_.errors = errors;
        editor_segments_cache_.warnings = warnings;
        editor_segments_cache_.problems_text =
            std::to_string(errors) + " errors, " + std::to_string(warnings) + " warnings";
      }
      problems.text = editor_segments_cache_.problems_text;
      problems.tooltip = "Open Problems";
      problems.command = "sidebar-show";
      problems.command_arg = "problems";
      problems.visible = true;
      problems.tone =
          errors > 0 ? StatusBarSegmentTone::Error : StatusBarSegmentTone::Warning;
    } else {
      editor_segments_cache_.errors = 0;
      editor_segments_cache_.warnings = 0;
      editor_segments_cache_.problems_text.clear();
    }
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Problems, std::move(problems));

  StatusBarSegmentValue lsp;
  if (active_viewport != nullptr) {
    std::string lsp_text;
    std::string lsp_tooltip;
    StatusBarSegmentTone lsp_tone = StatusBarSegmentTone::Default;
    operations.active_lsp_status_strings(false, lsp_text, lsp_tooltip, lsp_tone);
    if (!lsp_text.empty()) {
      // The tone comes from typed LSP readiness state (idle/busy/failed), so a
      // server named "Ready…" or a "Not Ready" message no longer mis-colors the
      // segment, and a failed server is flagged Error rather than mere Info.
      lsp.tone = lsp_tone;
      lsp.text = std::move(lsp_text);
      lsp.tooltip = std::move(lsp_tooltip);
      lsp.visible = true;
    }
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Lsp, std::move(lsp));
}

}  // namespace microide::workspace
