#include "workspace/git/CompareTabReview.h"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>

#include "compare/BranchReviewStateTypes.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceUiText.h"

namespace microide::workspace {

namespace {

compare::ComparePresentationOptions PresentationOptionsFromTab(const CompareTabState& compare_tab) {
  return compare::ComparePresentationOptions{
      .show_whitespace = compare_tab.show_whitespace,
  };
}

std::string CompareReviewHeaderModeLabel(compare::CompareReviewMode mode) {
  switch (mode) {
    case compare::CompareReviewMode::WorkingTree:
      return "Working tree review";
    case compare::CompareReviewMode::Commit:
      return "Commit review";
    case compare::CompareReviewMode::Branch:
      return "Branch review";
    case compare::CompareReviewMode::Conflict:
      return "Conflict review";
    case compare::CompareReviewMode::Plain:
      return "Compare";
  }
  return "Working tree review";
}

std::string CompareReviewHeaderStagingLabel(compare::WorkingTreeStagingView view) {
  switch (view) {
    case compare::WorkingTreeStagingView::Combined:
      return "combined";
    case compare::WorkingTreeStagingView::Unstaged:
      return "unstaged";
    case compare::WorkingTreeStagingView::Staged:
      return "staged";
  }
  return "combined";
}

// Identity of the git entry the semantic classifier reads (rename source and
// target). It is the one classifier input the compare tab's content fingerprint
// does not cover, so the memo below has to notice when it moves.
std::size_t GitEntrySignature(const std::optional<project::GitRepositoryEntry>& entry) {
  if (!entry.has_value()) {
    return 0;
  }
  const auto mix = [](std::size_t seed, std::size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
  };
  std::size_t signature = 1;
  signature = mix(signature, static_cast<std::size_t>(entry->kind));
  const std::hash<std::string> hash_text;
  signature = mix(signature, hash_text(entry->path.relative_path));
  if (entry->old_path.has_value()) {
    signature = mix(signature, hash_text(entry->old_path->relative_path));
  }
  return signature;
}

}  // namespace

void ApplyCompareTabReviewMetadata(CompareTabState& compare_tab,
                                   const CompareTabReviewRefreshInput& input) {
  // The semantic classification reads both whole buffers — it serializes the right
  // viewport and scans both files for NUL bytes, a submodule pointer, and a
  // line-ending-only difference. That is the single most expensive thing this
  // refresh does, and the refresh fires from ~10 event sites (every keystroke,
  // mouse move, focus change, plugin refresh), almost all of which leave the
  // compared content untouched. Gate it on the caller's content-changed signal plus
  // the git entry, which is the only other input it reads.
  const std::size_t git_entry_signature = GitEntrySignature(input.git_entry);
  const bool semantic_inputs_changed = input.content_changed ||
                                       !compare_tab.semantic_inputs_valid ||
                                       compare_tab.semantic_git_entry_signature !=
                                           git_entry_signature;
  compare_tab.semantic_inputs_valid = true;
  compare_tab.semantic_git_entry_signature = git_entry_signature;

  // A plain (non-git) compare has no refs to infer from. The mode is sticky —
  // fixed at build time — so this refresh (fired from ~10 sites) must not
  // re-infer it back to a git mode and re-enable staging/branch review. Keep the
  // cheap semantic-file inference (binary/text) below; it is source-agnostic.
  if (compare_tab.plain_compare) {
    compare_tab.review_mode = compare::CompareReviewMode::Plain;
    compare_tab.staging_view = compare::WorkingTreeStagingView::Combined;
    if (!semantic_inputs_changed) {
      return;
    }
    // The serialized right buffer has to outlive the input: its content fields are
    // views now, so binding the temporary directly would dangle by the next line.
    const std::string plain_right_content =
        util::SerializeLinesStreaming(editor::LineSpan(compare_tab.right_viewport.lines()),
                                      compare_tab.right_viewport.line_ending());
    compare::CompareSemanticMetadataInput plain_semantic_input{
        .path = compare_tab.path,
        .left_content = compare_tab.left_content,
        .right_content = plain_right_content,
        .git_entry = std::nullopt,
        .old_path = {},
    };
    compare_tab.semantic_file = compare::InferCompareSemanticFileMetadata(plain_semantic_input);
    return;
  }
  compare_tab.review_mode = compare::InferCompareReviewMode(compare_tab.commit_hash,
                                                            compare_tab.right_ref,
                                                            input.opened_from_commit_picker);
  compare_tab.staging_view =
      compare::InferWorkingTreeStagingView(compare_tab.commit_hash, compare_tab.right_ref);
  if (compare_tab.review_mode == compare::CompareReviewMode::Branch) {
    compare_tab.branch_target = compare::MakeBranchReviewTargetIdentity(
        input.repository_root, compare_tab.commit_hash, compare_tab.right_ref,
        input.merge_base_commit, input.snapshot_generation);
  }
  if (!semantic_inputs_changed) {
    return;
  }
  // Same lifetime rule as the plain branch above: the input holds views.
  const std::string right_content =
      util::SerializeLinesStreaming(editor::LineSpan(compare_tab.right_viewport.lines()),
                                    compare_tab.right_viewport.line_ending());
  compare::CompareSemanticMetadataInput semantic_input{
      .path = compare_tab.path,
      .left_content = compare_tab.left_content,
      .right_content = right_content,
      .git_entry = input.git_entry,
      .old_path = {},
  };
  if (input.git_entry.has_value() && input.git_entry->old_path.has_value()) {
    semantic_input.old_path = std::filesystem::path(input.git_entry->old_path->relative_path);
  }
  compare_tab.semantic_file = compare::InferCompareSemanticFileMetadata(semantic_input);
}

void ApplyBranchReviewPresentationMarkers(
    CompareTabState& compare_tab,
    const compare::BranchReviewStateService& review_service) {
  if (compare_tab.review_mode != compare::CompareReviewMode::Branch) {
    compare_tab.review_markers_valid = false;
    return;
  }
  // This runs from the compare tab's derived-state refresh, which fires from ~10
  // event sites — every mouse move over the tab included — and it walks every
  // presentation row. Its inputs are exactly three: the row list (which only
  // RefreshCompareTabPresentation rebuilds, bumping presentation_revision), the
  // review state (whose every mutation bumps revision()), and the branch target
  // the rows were resolved against. If none moved, the rows already hold the
  // answer.
  const std::uint64_t review_revision = review_service.revision();
  if (compare_tab.review_markers_valid &&
      compare_tab.review_markers_built_presentation_revision ==
          compare_tab.presentation_revision &&
      compare_tab.review_markers_built_review_revision == review_revision &&
      compare_tab.review_markers_built_target == compare_tab.branch_target) {
    return;
  }
  compare_tab.review_markers_valid = true;
  compare_tab.review_markers_built_presentation_revision = compare_tab.presentation_revision;
  compare_tab.review_markers_built_review_revision = review_revision;
  compare_tab.review_markers_built_target = compare_tab.branch_target;

  // Every hunk's marker and note flag in one pass over the review state. Asking
  // per hunk (memoized per hunk index, which is what this used to do) still walked
  // the target's reviewed-hunk list once per hunk and fell back to a FileStatus
  // that walked it again against every model hunk — and copied the query's target
  // identity and path each time.
  const compare::BranchReviewStateQueryInput query{
      .target = compare_tab.branch_target,
      .path = compare_tab.path,
      .model = &compare_tab.model,
  };
  review_service.ResolveHunkMarkers(query, &compare_tab.review_hunk_markers);
  const std::vector<compare::BranchReviewHunkMarker>& markers = compare_tab.review_hunk_markers;

  for (compare::ComparePresentationRow& row : compare_tab.presentation.rows) {
    row.review_marker_label.clear();
    row.has_review_note = false;
    // Resolve the hunk from the underlying model row. `ComparePresentationRow::
    // hunk_index` is never populated by the builder (it stays -1), so gating on it
    // made this branch dead code — review markers/notes never rendered on any row.
    // The model row carries the real hunk id (matching the navigation path in
    // CompareTabPresentationRowForHunk).
    if (row.kind == compare::ComparePresentationRowKind::Model &&
        row.model_row_index < compare_tab.model.rows.size()) {
      const int hunk = compare_tab.model.rows[row.model_row_index].hunk;
      if (hunk >= 0 && static_cast<std::size_t>(hunk) < markers.size()) {
        // Assign into the row's existing capacity — the label is one of two short
        // literals, so after the first pass no row reallocates.
        const std::string_view label = compare::BranchReviewMarkerLabel(markers[hunk].status);
        row.review_marker_label.assign(label.begin(), label.end());
        row.has_review_note = markers[hunk].has_note;
      }
    }
    compare::ComposeComparePresentationDisplaySummary(row);
  }
}

void RefreshCompareTabPresentation(CompareTabState& compare_tab) {
  util::PerformanceTrace::Scope trace_scope("workspace::RefreshCompareTabPresentation");
  compare_tab.presentation = compare::BuildComparePresentationModel(
      compare_tab.model, compare_tab.semantic_file, PresentationOptionsFromTab(compare_tab),
      compare_tab.presentation.collapse_state, compare_tab.model_revision);
  ++compare_tab.presentation_revision;
  // Record what this build consumed, so the derived-state refresh can tell whether
  // a later event moved any of it. Collapse-state edits call straight in here, and
  // they are exactly the case the (model, options, semantic) triple cannot see —
  // revalidating here keeps them correct without a fourth revision counter.
  compare_tab.presentation_valid = true;
  compare_tab.presentation_built_model_revision = compare_tab.model_revision;
  compare_tab.presentation_built_show_whitespace = compare_tab.show_whitespace;
  NormalizeCompareSelectionToModelRow(compare_tab);
}

void NormalizeCompareSelectionToModelRow(CompareTabState& compare_tab) {
  if (compare_tab.presentation.rows.empty()) {
    return;
  }
  compare_tab.selected_row =
      std::min(compare_tab.selected_row, compare_tab.presentation.rows.size() - 1);
  const auto& selected = compare_tab.presentation.rows[compare_tab.selected_row];
  if (selected.kind == compare::ComparePresentationRowKind::Model) {
    return;
  }
  for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
    if (compare_tab.presentation.rows[i].kind == compare::ComparePresentationRowKind::Model) {
      compare_tab.selected_row = i;
      return;
    }
  }
}

void RefreshCompareReviewHeader(CompareTabState& compare_tab) {
  const bool plain = compare_tab.review_mode == compare::CompareReviewMode::Plain;
  // A plain compare has no git mode/staging metadata — lead with the two side
  // labels ("left ↔ right") so the header names what is actually being diffed.
  std::string summary = plain
                            ? compare_tab.left_label + "  ↔  " + compare_tab.right_label
                            : CompareReviewHeaderModeLabel(compare_tab.review_mode);
  if (compare_tab.review_mode == compare::CompareReviewMode::WorkingTree) {
    summary += "  ·  ";
    summary += CompareReviewHeaderStagingLabel(compare_tab.staging_view);
  }
  summary += "  ·  ";
  AppendUnsigned(summary, compare_tab.model.hunks.size());
  summary += compare_tab.model.hunks.size() == 1 ? " hunk" : " hunks";
  switch (compare_tab.semantic_file.file_kind) {
    case compare::CompareSemanticFileKind::Binary:
      summary += "  ·  binary";
      break;
    case compare::CompareSemanticFileKind::Submodule:
      summary += "  ·  submodule";
      break;
    case compare::CompareSemanticFileKind::Text:
      break;
  }
  if (compare_tab.semantic_file.renamed) {
    summary += "  ·  rename";
  }
  if (compare_tab.semantic_file.mode_changed) {
    summary += "  ·  mode";
  }
  if (compare_tab.semantic_file.line_ending_only) {
    summary += "  ·  line endings";
  }
  // Plain compares already lead with both side labels; the git path suffix is
  // redundant (and often empty for buffer/clipboard sides).
  if (!plain) {
    const std::string file_label = compare_tab.path.filename().string();
    if (!file_label.empty()) {
      summary += "  ·  ";
      summary += file_label;
    }
  }
  compare_tab.review_header.summary_line = std::move(summary);

  std::string actions;
  AppendHintSegment(actions, "[ / ] hunks");
  AppendHintSegment(actions, "Enter open");
  AppendHintSegment(actions, "o open");
  if (compare_tab.semantic_file.file_kind == compare::CompareSemanticFileKind::Text &&
      compare_tab.right_editable && !compare_tab.model.hunks.empty()) {
    if (compare_tab.staging_view == compare::WorkingTreeStagingView::Staged) {
      AppendHintSegment(actions, "c unstage hunk");
      AppendHintSegment(actions, "C unstage lines");
    } else {
      AppendHintSegment(actions, "a stage hunk");
      AppendHintSegment(actions, "A stage lines");
    }
    AppendHintSegment(actions, "d discard hunk");
    AppendHintSegment(actions, "D discard lines");
  }
  compare_tab.review_header.action_hint_line = std::move(actions);
}

std::size_t CompareTabPresentationRowCount(const CompareTabState& compare_tab) {
  if (!compare_tab.presentation.rows.empty()) {
    return compare_tab.presentation.rows.size();
  }
  return compare_tab.model.rows.size();
}

std::size_t CompareTabSelectedModelRow(const CompareTabState& compare_tab) {
  if (!compare_tab.presentation.rows.empty()) {
    return compare::ComparePresentationToModelRow(compare_tab.presentation, compare_tab.selected_row);
  }
  return compare_tab.selected_row;
}

const compare::CompareRow& CompareTabSelectedModelRowRef(const CompareTabState& compare_tab) {
  // Defensive empty-guard: `rows.size() - 1` underflows to SIZE_MAX on an empty
  // model, so indexing would be UB. Callers currently guard, but return a stable
  // empty row rather than trust that forever.
  static const compare::CompareRow kEmptyRow{};
  if (compare_tab.model.rows.empty()) {
    return kEmptyRow;
  }
  const std::size_t model_row = CompareTabSelectedModelRow(compare_tab);
  return compare_tab.model.rows[std::min(model_row, compare_tab.model.rows.size() - 1)];
}

int CompareTabSelectedHunkIndex(const CompareTabState& compare_tab) {
  if (compare_tab.model.rows.empty()) {
    return -1;
  }
  const std::size_t model_row = CompareTabSelectedModelRow(compare_tab);
  // Clamp like the sibling CompareTabSelectedModelRowRef: a selected_row derived
  // from a stale presentation (before RefreshCompareTabPresentation re-runs) can
  // exceed model.rows, and this must never index out of bounds.
  return compare_tab.model.rows[std::min(model_row, compare_tab.model.rows.size() - 1)].hunk;
}

const compare::ComparePresentationRow* CompareTabPresentationRowAt(
    const CompareTabState& compare_tab,
    std::size_t presentation_row) {
  if (presentation_row >= compare_tab.presentation.rows.size()) {
    return nullptr;
  }
  return &compare_tab.presentation.rows[presentation_row];
}

namespace {

// What the wrap table needs to know about presentation row `unit`: the two panes'
// text, or "this row is a full-width summary that never wraps".
DiffWrapLayout::UnitText CompareWrapUnitText(const CompareTabState& compare_tab,
                                            std::size_t unit) {
  DiffWrapLayout::UnitText text;
  std::size_t model_row = unit;
  if (!compare_tab.presentation.rows.empty()) {
    const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(compare_tab, unit);
    if (row == nullptr || row->kind != compare::ComparePresentationRowKind::Model) {
      // Metadata / hunk-header / collapsed-context rows span the whole surface and
      // are truncated, not wrapped — one row each, exactly as with wrap off.
      text.single_row = true;
      text.has_left = true;
      return text;
    }
    model_row = row->model_row_index;
  }
  if (model_row >= compare_tab.model.rows.size()) {
    text.single_row = true;
    return text;
  }
  const compare::CompareRow& model = compare_tab.model.rows[model_row];
  text.has_left = model.left_line > 0;
  text.has_right = model.right_line > 0;
  text.left = model.left_text;
  text.right = model.right_text;
  return text;
}

}  // namespace

void EnsureCompareWrapLayout(const CompareTabState& compare_tab,
                             bool soft_wrap,
                             std::size_t left_columns,
                             std::size_t right_columns) {
  // Both revisions in the key: the presentation is rebuilt whenever the model
  // changes today, but a table that silently kept a deleted row's text is the kind
  // of bug that only shows up under a later refactor.
  const std::uint64_t content_key =
      compare_tab.presentation_revision * 0x9E3779B97F4A7C15ull ^ compare_tab.model_revision;
  compare_tab.wrap_layout.Ensure(
      content_key, soft_wrap, left_columns,
      right_columns, compare_tab.right_viewport.tab_size(),
      CompareTabPresentationRowCount(compare_tab),
      [&compare_tab](std::size_t unit) { return CompareWrapUnitText(compare_tab, unit); });
}

void RefreshCompareWrapLayoutForContent(const CompareTabState& compare_tab) {
  if (!compare_tab.wrap_layout.active()) {
    return;
  }
  EnsureCompareWrapLayout(compare_tab, true, compare_tab.wrap_layout.built_left_columns(),
                          compare_tab.wrap_layout.built_right_columns());
}

std::size_t CompareTabVisualRowCount(const CompareTabState& compare_tab) {
  return compare_tab.wrap_layout.RowCount(CompareTabPresentationRowCount(compare_tab));
}

std::size_t CompareVisualRowToPresentationRow(const CompareTabState& compare_tab,
                                              std::size_t visual_row) {
  return compare_tab.wrap_layout.UnitForRow(visual_row);
}

std::size_t ComparePresentationRowToVisualRow(const CompareTabState& compare_tab,
                                              std::size_t presentation_row) {
  return compare_tab.wrap_layout.FirstRowForUnit(presentation_row);
}

std::size_t CompareTabRightLineForModelRow(const CompareTabState& compare_tab,
                                           std::size_t model_row) {
  if (compare_tab.right_viewport.line_count() == 0) {
    return 0;
  }
  if (model_row >= compare_tab.model.rows.size()) {
    return compare_tab.right_viewport.line_count() - 1;
  }
  if (const int line = compare_tab.model.rows[model_row].right_line; line > 0) {
    return static_cast<std::size_t>(line - 1);
  }
  for (std::size_t i = model_row + 1; i < compare_tab.model.rows.size(); ++i) {
    if (compare_tab.model.rows[i].right_line > 0) {
      return static_cast<std::size_t>(compare_tab.model.rows[i].right_line - 1);
    }
  }
  return compare_tab.right_viewport.line_count() - 1;
}

CompareRightCaretPlacement CompareRightCaretPlacementFor(const CompareTabState& compare_tab,
                                                         std::size_t right_line,
                                                         std::size_t visual_column) {
  const std::size_t presentation_row = compare::ComparePresentationRowForModelRow(
      compare_tab.presentation, CompareTabModelRowForRightLine(compare_tab, right_line));
  if (!compare_tab.wrap_layout.active()) {
    return CompareRightCaretPlacement{
        .visual_row = ComparePresentationRowToVisualRow(compare_tab, presentation_row),
        .column = visual_column,
    };
  }
  const std::size_t visual_row =
      compare_tab.wrap_layout.RowForUnitColumn(presentation_row, visual_column, /*right_side=*/true);
  const DiffWrapRow row = compare_tab.wrap_layout.RowAt(visual_row);
  const std::size_t offset = visual_column > row.right_start ? visual_column - row.right_start : 0;
  return CompareRightCaretPlacement{
      .visual_row = visual_row,
      .column = row.right_indent + offset,
  };
}

CompareRightPaneHit CompareRightPaneHitAt(const CompareTabState& compare_tab,
                                          std::size_t visual_row,
                                          std::size_t screen_column) {
  const std::size_t presentation_row = CompareVisualRowToPresentationRow(compare_tab, visual_row);
  const std::size_t model_row =
      compare_tab.presentation.rows.empty()
          ? presentation_row
          : compare::ComparePresentationToModelRow(compare_tab.presentation, presentation_row);
  CompareRightPaneHit hit{
      .presentation_row = presentation_row,
      .model_row = model_row,
      .line = CompareTabRightLineForModelRow(compare_tab, model_row),
      .visual_column = screen_column,
  };
  if (!compare_tab.wrap_layout.active()) {
    return hit;
  }
  // Clicking a padding row (this side has no segment here) resolves to the unit's
  // last row that does, which is where the caret can actually land.
  DiffWrapRow row = compare_tab.wrap_layout.RowAt(visual_row);
  if (!row.right_present) {
    row = compare_tab.wrap_layout.RowAt(compare_tab.wrap_layout.RowForUnitColumn(
        presentation_row, std::numeric_limits<std::size_t>::max(), /*right_side=*/true));
  }
  const std::size_t within =
      screen_column > row.right_indent ? screen_column - row.right_indent : 0;
  hit.visual_column = std::min<std::size_t>(row.right_start + within, row.right_end);
  return hit;
}

CompareRightLineAnchor CompareRightLineBlameAnchor(const CompareTabState& compare_tab,
                                                   std::size_t right_line,
                                                   std::size_t line_visual_columns) {
  const std::size_t model_row = CompareTabModelRowForRightLine(compare_tab, right_line);
  if (model_row >= compare_tab.model.rows.size() ||
      compare_tab.model.rows[model_row].right_line != static_cast<int>(right_line + 1)) {
    return CompareRightLineAnchor{};
  }
  // The end of the line is past every segment's start, so the caret placement
  // resolves to the last row carrying right-pane text -- which is where an inline
  // annotation belongs.
  const CompareRightCaretPlacement placement =
      CompareRightCaretPlacementFor(compare_tab, right_line, line_visual_columns);
  return CompareRightLineAnchor{
      .visual_row = placement.visual_row,
      .end_column = placement.column,
      .valid = true,
  };
}

std::size_t CompareTabModelRowForRightLine(const CompareTabState& compare_tab,
                                           std::size_t right_line_index) {
  if (compare_tab.model.rows.empty()) {
    return 0;
  }

  const int target_line = static_cast<int>(right_line_index + 1);
  for (std::size_t i = 0; i < compare_tab.model.rows.size(); ++i) {
    const auto& row = compare_tab.model.rows[i];
    if (row.right_line == target_line) {
      return i;
    }
    if (row.right_line > target_line) {
      return i;
    }
  }
  return compare_tab.model.rows.size() - 1;
}

std::optional<std::size_t> CompareTabPresentationRowForHunk(const CompareTabState& compare_tab,
                                                            int hunk_index) {
  if (hunk_index < 0) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
    const auto& row = compare_tab.presentation.rows[i];
    if (row.kind == compare::ComparePresentationRowKind::HunkHeader && row.hunk_index == hunk_index) {
      return i;
    }
    if (row.kind == compare::ComparePresentationRowKind::Model &&
        row.model_row_index < compare_tab.model.rows.size() &&
        compare_tab.model.rows[row.model_row_index].hunk == hunk_index) {
      return i;
    }
  }
  return std::nullopt;
}

bool ExpandCompareCollapsedContext(CompareTabState& compare_tab,
                                   std::size_t presentation_row,
                                   CompareCollapsedContextAction action,
                                   std::size_t reveal_lines) {
  const compare::ComparePresentationRow* row =
      CompareTabPresentationRowAt(compare_tab, presentation_row);
  if (row == nullptr || row->kind != compare::ComparePresentationRowKind::CollapsedContext ||
      row->collapsed_line_count <= 0) {
    return false;
  }

  const std::size_t collapsed_lines = static_cast<std::size_t>(row->collapsed_line_count);
  const std::size_t previous_selected_row = compare_tab.selected_row;
  const int previous_scroll_row = compare_tab.scroll_row;
  const std::size_t collapsed_run_start_model_row = row->collapsed_run_start_model_row;
  const std::size_t collapsed_run_length = row->collapsed_run_length;
  std::size_t revealed_before = 0;
  compare::ComparePresentationCollapsedRunState* collapsed_run_state =
      compare::FindCollapsedRunState(compare_tab.presentation.collapse_state.collapsed_runs,
                                     collapsed_run_start_model_row, collapsed_run_length);
  if (collapsed_run_state == nullptr) {
    compare_tab.presentation.collapse_state.collapsed_runs.push_back(
        compare::ComparePresentationCollapsedRunState{
            .run_start_model_row = collapsed_run_start_model_row,
            .run_length = collapsed_run_length,
        });
    collapsed_run_state =
        &compare_tab.presentation.collapse_state.collapsed_runs.back();
  }
  auto grow_reveal = [&](std::size_t& current,
                         std::size_t opposite,
                         std::size_t amount,
                         std::size_t* revealed_delta) {
    const std::size_t max_reveal =
        collapsed_run_length > opposite ? collapsed_run_length - opposite : 0;
    const std::size_t target = std::min(max_reveal, current + amount);
    if (current >= target) {
      return false;
    }
    if (revealed_delta != nullptr) {
      *revealed_delta += target - current;
    }
    current = target;
    return true;
  };

  bool changed = false;
  switch (action) {
    case CompareCollapsedContextAction::ShowPrevious:
      changed = grow_reveal(collapsed_run_state->expanded_above,
                            collapsed_run_state->expanded_below, reveal_lines,
                            &revealed_before);
      break;
    case CompareCollapsedContextAction::ShowAll:
      changed = grow_reveal(collapsed_run_state->expanded_above,
                            collapsed_run_state->expanded_below, collapsed_lines,
                            &revealed_before) ||
                changed;
      changed = grow_reveal(collapsed_run_state->expanded_below,
                            collapsed_run_state->expanded_above, collapsed_lines, nullptr) ||
                changed;
      break;
    case CompareCollapsedContextAction::ShowNext:
      changed = grow_reveal(collapsed_run_state->expanded_below,
                            collapsed_run_state->expanded_above, reveal_lines, nullptr);
      break;
  }
  if (!changed) {
    return false;
  }
  // Anchor the viewport top in PRESENTATION space across the rebuild: `scroll_row`
  // indexes on-screen rows, and expanding a run inserts presentation rows above it.
  const std::size_t previous_top_presentation_row =
      CompareVisualRowToPresentationRow(compare_tab, static_cast<std::size_t>(
                                                         std::max(0, previous_scroll_row)));
  RefreshCompareTabPresentation(compare_tab);
  RefreshCompareWrapLayoutForContent(compare_tab);
  if (revealed_before > 0) {
    compare_tab.scroll_row = static_cast<int>(ComparePresentationRowToVisualRow(
        compare_tab, previous_top_presentation_row + revealed_before));
  }
  const auto matches_collapsed_row = [&](const compare::ComparePresentationRow& candidate) {
    return candidate.kind == compare::ComparePresentationRowKind::CollapsedContext &&
           candidate.collapsed_run_start_model_row == collapsed_run_start_model_row &&
           candidate.collapsed_run_length == collapsed_run_length;
  };
  for (std::size_t i = 0; i < compare_tab.presentation.rows.size(); ++i) {
    if (matches_collapsed_row(compare_tab.presentation.rows[i])) {
      compare_tab.selected_row = i;
      return true;
    }
  }
  const std::size_t target_selected_row = previous_selected_row + revealed_before;
  compare_tab.selected_row =
      compare_tab.presentation.rows.empty()
          ? 0
          : std::min(target_selected_row, compare_tab.presentation.rows.size() - 1);
  return true;
}

std::optional<CompareCollapsedContextRowHit> CompareCollapsedContextRowAt(
    const CompareTabState& compare_tab,
    const SDL_FRect& editor_surface,
    float rows_y,
    float line_height,
    bool show_vertical_scrollbar,
    float y) {
  if (line_height <= 0.0f) {
    return std::nullopt;
  }
  const int visible_row = static_cast<int>((y - rows_y) / line_height);
  const int visual_row = compare_tab.scroll_row + visible_row;
  if (visible_row < 0 || visual_row < 0 ||
      static_cast<std::size_t>(visual_row) >= CompareTabVisualRowCount(compare_tab)) {
    return std::nullopt;
  }
  const std::size_t presentation_row =
      CompareVisualRowToPresentationRow(compare_tab, static_cast<std::size_t>(visual_row));
  const compare::ComparePresentationRow* row =
      CompareTabPresentationRowAt(compare_tab, presentation_row);
  if (row == nullptr || row->kind != compare::ComparePresentationRowKind::CollapsedContext) {
    return std::nullopt;
  }
  return CompareCollapsedContextRowHit{
      .row = row,
      .presentation_row = presentation_row,
      .visible_row = visible_row,
      .block_rect = CompareCollapsedContextBlockRect(editor_surface, rows_y, line_height,
                                                     show_vertical_scrollbar, visible_row),
  };
}

std::optional<CompareHoverKind> CompareCollapsedContextActionAt(
    const CollapsedContextActionRects& rects, float x, float y) {
  const auto contains = [&](const SDL_FRect& rect) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
  };
  if (rects.previous_rect.has_value() && contains(*rects.previous_rect)) {
    return CompareHoverKind::CollapsedContextPreviousAction;
  }
  if (contains(rects.all_rect)) {
    return CompareHoverKind::CollapsedContextAllAction;
  }
  if (rects.next_rect.has_value() && contains(*rects.next_rect)) {
    return CompareHoverKind::CollapsedContextNextAction;
  }
  return std::nullopt;
}

}  // namespace microide::workspace
