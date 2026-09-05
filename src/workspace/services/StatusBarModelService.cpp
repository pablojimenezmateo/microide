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

  // Views into memoized strings, published through the assigning SetSegment: a
  // steady frame must not materialize a segment's text again just to hand it over.
  StatusBarService::StatusBarSegmentUpdate project_segment;
  StatusBarService::StatusBarSegmentUpdate branch_segment;
  if (!project_state.root.empty()) {
    // See GitSnapshotDerivedCache: both of these are per-frame costs keyed on
    // state that moves on a project switch / git snapshot, not on a frame.
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
    if (!repo_available) {
      // is_git_repo_valid is a `.git` stat (not a subprocess), but this refresh
      // runs from PrepareFrameOnce, so "cheap" was still one syscall per painted
      // frame — 120 a second, forever, for any project git has not answered for
      // yet (TD-2026-08-06-158). It ran uncached because a per-root cache went
      // stale after an in-session `git init` until an unrelated refresh
      // superseded it. `repository_marker_generation` is the missing
      // invalidation key: `.git` cannot appear or disappear under a live project
      // without producing a repository change, and one now fires for exactly
      // that transition (GitRepositoryMetadataTracker).
      if (!marker_probe_cache_.valid ||
          marker_probe_cache_.project_root != project_root ||
          marker_probe_cache_.marker_generation != git_state.repository_marker_generation) {
        marker_probe_cache_.project_root = project_root;
        marker_probe_cache_.marker_generation = git_state.repository_marker_generation;
        marker_probe_cache_.present = operations.is_git_repo_valid(project_root);
        marker_probe_cache_.valid = true;
      }
      repo_available = marker_probe_cache_.present;
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
    git_derived_cache_ = {};
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Project, project_segment);
  if (!operations.startup_mode_text.empty()) {
    branch_segment.text = operations.startup_mode_text;
    branch_segment.tooltip = operations.startup_mode_tooltip.empty()
                                 ? operations.startup_mode_text
                                 : operations.startup_mode_tooltip;
    branch_segment.visible = true;
    branch_segment.command = {};  // startup-mode text is a readout, not a control
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Branch, branch_segment);

  if (active_viewport != nullptr) {
    const editor::TextViewport* const viewport = active_viewport;
    if (editor_segments_cache_.viewport != viewport ||
        editor_segments_cache_.cursor_line != viewport->cursor_line() ||
        editor_segments_cache_.cursor_column != viewport->cursor_column() ||
        editor_segments_cache_.content_revision != viewport->content_revision()) {
      editor_segments_cache_.viewport = viewport;
      editor_segments_cache_.cursor_line = viewport->cursor_line();
      editor_segments_cache_.cursor_column = viewport->cursor_column();
      editor_segments_cache_.content_revision = viewport->content_revision();
      // Composed in place, like the tooltip below: the `+` chain built three
      // temporaries and assigned the last one, so a caret move past column 15
      // (where SSO stops covering "Ln N, Col M") allocated twice per keystroke on
      // the shell thread. Appending into the memo's own buffer reuses a capacity
      // the previous caret position already paid for.
      std::string& line_column_text = editor_segments_cache_.line_column_text;
      line_column_text.clear();
      line_column_text.append("Ln ");
      util::AppendUnsigned(line_column_text, viewport->cursor_line() + 1);
      line_column_text.append(", Col ");
      // The VISIBLE column (VS Code's "Col"): a tab counts as its width and a
      // multibyte character as one, where the raw byte offset put the caret at
      // "Col 3" after a single accented letter and "Col 2" after a tab.
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
    indent.tooltip = "Change indent settings";
    indent.command = "settings";
    indent.visible = true;
    status_bar_service.SetSegment(StatusBarSegmentId::Indent, indent);

    StatusBarService::StatusBarSegmentUpdate language;
    // The viewport owns the memo, so a settled buffer costs a field compare here.
    // The normalized-path cache this used to keep existed only to key the status
    // bar's own filetype memo; both went away with it.
    const std::string& filetype = viewport->language_id();
    if (!filetype.empty()) {
      // The tooltip is the only composed string here, so it gets its own memo
      // rather than being rebuilt per frame from a filetype that rarely changes.
      if (editor_segments_cache_.language_tooltip_source != filetype) {
        editor_segments_cache_.language_tooltip_source = filetype;
        editor_segments_cache_.language_tooltip.clear();
        editor_segments_cache_.language_tooltip.reserve(filetype.size() + 10);
        editor_segments_cache_.language_tooltip.append("Language: ");
        editor_segments_cache_.language_tooltip.append(filetype);
      }
      language.text = filetype;
      language.tooltip = editor_segments_cache_.language_tooltip;
      language.visible = true;
    }
    status_bar_service.SetSegment(StatusBarSegmentId::Language, language);

    StatusBarService::StatusBarSegmentUpdate encoding;
    // `EncodingLabel`/`LineEndingLabel` return owned strings and the joined form
    // is a third; all three were rebuilt per painted frame for a value that only
    // moves when the file's encoding or line ending does. Memoized on the pair.
    const std::string encoding_label = viewport->EncodingLabel();
    const std::string line_ending_label = viewport->LineEndingLabel();
    if (!encoding_label.empty() || !line_ending_label.empty()) {
      if (editor_segments_cache_.encoding_source_left != encoding_label ||
          editor_segments_cache_.encoding_source_right != line_ending_label) {
        editor_segments_cache_.encoding_source_left = encoding_label;
        editor_segments_cache_.encoding_source_right = line_ending_label;
        editor_segments_cache_.encoding_text.clear();
        editor_segments_cache_.encoding_text.reserve(encoding_label.size() +
                                                     line_ending_label.size() + 4);
        editor_segments_cache_.encoding_text.append(encoding_label);
        if (!encoding_label.empty() && !line_ending_label.empty()) {
          editor_segments_cache_.encoding_text.append(" · ");
        }
        editor_segments_cache_.encoding_text.append(line_ending_label);
      }
      encoding.text = editor_segments_cache_.encoding_text;
      encoding.tooltip = "File encoding and line endings";
      encoding.visible = true;
    }
    status_bar_service.SetSegment(StatusBarSegmentId::Encoding, encoding);
  } else {
    editor_segments_cache_.viewport = nullptr;
    editor_segments_cache_.line_column_text.clear();
    editor_segments_cache_.indent_text.clear();
    status_bar_service.ClearSegment(StatusBarSegmentId::LineColumn);
    status_bar_service.ClearSegment(StatusBarSegmentId::Indent);
    status_bar_service.ClearSegment(StatusBarSegmentId::Language);
    status_bar_service.ClearSegment(StatusBarSegmentId::Encoding);
  }

  StatusBarService::StatusBarSegmentUpdate problems;
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
  status_bar_service.SetSegment(StatusBarSegmentId::Problems, problems);

  StatusBarService::StatusBarSegmentUpdate lsp;
  if (active_viewport != nullptr) {
    // Reused buffers, not locals: the callback assigns into them, so a steady
    // frame reuses the capacity rather than allocating both strings again.
    editor_segments_cache_.lsp_text.clear();
    editor_segments_cache_.lsp_tooltip.clear();
    StatusBarSegmentTone lsp_tone = StatusBarSegmentTone::Default;
    operations.active_lsp_status_strings(false, editor_segments_cache_.lsp_text,
                                         editor_segments_cache_.lsp_tooltip, lsp_tone);
    if (!editor_segments_cache_.lsp_text.empty()) {
      // The tone comes from typed LSP readiness state (idle/busy/failed), so a
      // server named "Ready…" or a "Not Ready" message no longer mis-colors the
      // segment, and a failed server is flagged Error rather than mere Info.
      lsp.tone = lsp_tone;
      lsp.text = editor_segments_cache_.lsp_text;
      lsp.tooltip = editor_segments_cache_.lsp_tooltip;
      lsp.visible = true;
    }
  }
  status_bar_service.SetSegment(StatusBarSegmentId::Lsp, lsp);
}

}  // namespace microide::workspace
