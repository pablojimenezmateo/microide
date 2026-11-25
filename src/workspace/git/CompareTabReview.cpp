#include "workspace/git/CompareTabReview.h"

#include "util/Fnv1a.h"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

#include "compare/BranchReviewStateTypes.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceUiText.h"

namespace microide::workspace {

namespace {

compare::ComparePresentationOptions PresentationOptionsFromTab(const CompareTabState& compare_tab) {
  return compare::ComparePresentationOptions{
      .show_whitespace = compare_tab.show_whitespace,
      .hunk_markers = &compare_tab.review_hunk_markers,
      .file_note = compare_tab.review_file_note,
      .detached_notes = &compare_tab.review_detached_notes,
  };
}

void RebuildPresentationForNotes(CompareTabState& compare_tab) {
  const bool on_model_row = [&] {
    const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(compare_tab, compare_tab.selected_row);
    return row != nullptr && row->kind == compare::ComparePresentationRowKind::Model;
  }();
  const std::size_t selected_model_row = CompareTabSelectedModelRow(compare_tab);
  RefreshCompareTabPresentation(compare_tab);
  if (on_model_row && !compare_tab.model.rows.empty()) {
    compare_tab.selected_row =
        compare::ComparePresentationRowForModelRow(compare_tab.presentation, selected_model_row);
  }
  RefreshCompareWrapLayoutForContent(compare_tab);
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

}

void ApplyCompareTabReviewMetadata(CompareTabState& compare_tab,
                                   const CompareTabReviewRefreshInput& input) {
  const std::size_t git_entry_signature = GitEntrySignature(input.git_entry);
  const bool semantic_inputs_changed = input.content_changed ||
                                       !compare_tab.semantic_inputs_valid ||
                                       compare_tab.semantic_git_entry_signature !=
                                           git_entry_signature;
  compare_tab.semantic_inputs_valid = true;
  compare_tab.semantic_git_entry_signature = git_entry_signature;

  if (compare_tab.plain_compare) {
    compare_tab.review_mode = compare::CompareReviewMode::Plain;
    compare_tab.staging_view = compare::WorkingTreeStagingView::Combined;
    if (!semantic_inputs_changed) {
      return;
    }
    const std::string plain_right_content = compare_tab.right_viewport.SerializeDocumentText();
    compare::CompareSemanticMetadataInput plain_semantic_input{
        .path = compare_tab.path,
        .left_content = *compare_tab.left_content,
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
  if (CompareTabTakesReviewMarks(compare_tab)) {
    compare_tab.branch_target = compare::MakeBranchReviewTargetIdentity(
        input.repository_root, compare_tab.commit_hash, compare_tab.right_ref,
        input.merge_base_commit, input.snapshot_generation);
  }
  if (!semantic_inputs_changed) {
    return;
  }
  const std::string right_content = compare_tab.right_viewport.SerializeDocumentText();
  compare::CompareSemanticMetadataInput semantic_input{
      .path = compare_tab.path,
      .left_content = *compare_tab.left_content,
      .right_content = right_content,
      .git_entry = input.git_entry,
      .old_path = {},
  };
  if (input.git_entry.has_value() && input.git_entry->old_path.has_value()) {
    semantic_input.old_path = std::filesystem::path(input.git_entry->old_path->relative_path);
  }
  compare_tab.semantic_file = compare::InferCompareSemanticFileMetadata(semantic_input);
}

bool CompareTabTakesReviewMarks(const CompareTabState& compare_tab) {
  if (compare_tab.load_pending) {
    return false;
  }
  if (compare_tab.review_mode == compare::CompareReviewMode::Branch) {
    return true;
  }
  const std::string_view base = compare_tab.commit_hash;
  return compare_tab.review_mode == compare::CompareReviewMode::WorkingTree && !base.empty() && base != "HEAD" &&
         base != ":0" && base != "INDEX";
}

void ApplyBranchReviewPresentationMarkers(
    CompareTabState& compare_tab,
    const compare::BranchReviewStateService& review_service) {
  if (!CompareTabTakesReviewMarks(compare_tab)) {
    compare_tab.review_markers_valid = false;
    compare_tab.review_reviewed_hunk_count = 0;
    compare_tab.review_hunk_markers.clear();
    compare_tab.review_file_note.clear();
    compare_tab.review_detached_notes.clear();
    if (std::exchange(compare_tab.review_notes_fingerprint, 0) != 0) {
      RebuildPresentationForNotes(compare_tab);
    }
    return;
  }
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

  const compare::BranchReviewStateQueryInput query{
      .target = compare_tab.branch_target,
      .path = compare_tab.path,
      .model = &compare_tab.model,
  };
  review_service.ResolveHunkMarkers(query, &compare_tab.review_hunk_markers, &compare_tab.review_detached_notes);
  const std::vector<compare::BranchReviewHunkMarker>& markers = compare_tab.review_hunk_markers;
  compare_tab.review_file_note =
      review_service.NoteText(query, compare::BranchReviewNoteScope::File).value_or(std::string());

  std::size_t reviewed = 0;
  std::uint64_t fingerprint = 0;
  const auto mix_note = [&fingerprint](std::uint64_t place, std::string_view text) {
    fingerprint = util::Fnv1aBytes(util::Fnv1aValue(fingerprint == 0 ? util::kFnv1aOffsetBasis : fingerprint, place),
                                   text) | 1u;
  };
  for (std::size_t hunk = 0; hunk < markers.size(); ++hunk) {
    reviewed += markers[hunk].status == compare::BranchReviewMarkerStatus::Reviewed ? 1 : 0;
    if (markers[hunk].has_note) {
      mix_note((hunk << 1) | (markers[hunk].note_outdated ? 1u : 0u), markers[hunk].note_text);
    }
  }
  compare_tab.review_reviewed_hunk_count = reviewed;
  if (!compare_tab.review_file_note.empty()) {
    mix_note(~std::uint64_t{0}, compare_tab.review_file_note);
  }
  for (const compare::BranchReviewNote& detached : compare_tab.review_detached_notes) {
    mix_note(~std::uint64_t{1} - static_cast<std::uint64_t>(detached.hunk_identity.has_value()
                                                                ? detached.hunk_identity->new_start
                                                                : 0),
             detached.text);
  }
  if (std::exchange(compare_tab.review_notes_fingerprint, fingerprint) != fingerprint) {
    RebuildPresentationForNotes(compare_tab);
    compare_tab.review_markers_built_presentation_revision = compare_tab.presentation_revision;
  }

  for (compare::ComparePresentationRow& row : compare_tab.presentation.rows) {
    row.review_marker_label.clear();
    row.has_review_note = false;
    if (row.kind == compare::ComparePresentationRowKind::Model &&
        row.model_row_index < compare_tab.model.rows.size()) {
      const int hunk = compare_tab.model.rows[row.model_row_index].hunk;
      if (hunk >= 0 && static_cast<std::size_t>(hunk) < markers.size()) {
        const std::string_view label = compare::BranchReviewMarkerLabel(markers[hunk].status);
        row.review_marker_label.assign(label.begin(), label.end());
        row.has_review_note = markers[hunk].has_note;
      }
    }
    compare::ComposeComparePresentationDisplaySummary(row);
  }
  RefreshCompareReviewHeader(compare_tab);
}

void RefreshCompareTabPresentation(CompareTabState& compare_tab) {
  util::PerformanceTrace::Scope trace_scope("workspace::RefreshCompareTabPresentation");
  compare::BuildComparePresentationModelInto(
      compare_tab.presentation, compare_tab.model, compare_tab.semantic_file,
      PresentationOptionsFromTab(compare_tab));
  ++compare_tab.presentation_revision;
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
  std::string summary = plain
                            ? compare_tab.left_label + "  ↔  " + compare_tab.right_label
                            : CompareReviewHeaderModeLabel(compare_tab.review_mode);
  if (compare_tab.review_mode == compare::CompareReviewMode::WorkingTree) {
    summary += "  ·  ";
    summary += CompareReviewHeaderStagingLabel(compare_tab.staging_view);
  }
  summary += "  ·  ";
  AppendUnsigned(summary, compare_tab.model.hunks.size());
  summary += compare_tab.model.hunks.size() == 1 ? " change" : " changes";
  if (CompareTabTakesReviewMarks(compare_tab) && !compare_tab.model.hunks.empty()) {
    summary += ", ";
    AppendUnsigned(summary, compare_tab.review_reviewed_hunk_count);
    summary += " reviewed";
  }
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
  if (!plain) {
    const std::string file_label = compare_tab.path.filename().string();
    if (!file_label.empty()) {
      summary += "  ·  ";
      summary += file_label;
    }
  }
  compare_tab.review_header.summary_line = std::move(summary);
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

DiffWrapLayout::UnitText CompareWrapUnitText(const CompareTabState& compare_tab,
                                            std::size_t unit) {
  DiffWrapLayout::UnitText text;
  std::size_t model_row = unit;
  if (!compare_tab.presentation.rows.empty()) {
    const compare::ComparePresentationRow* row = CompareTabPresentationRowAt(compare_tab, unit);
    if (row == nullptr || row->kind != compare::ComparePresentationRowKind::Model) {
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

}

void EnsureCompareWrapLayout(const CompareTabState& compare_tab,
                             bool soft_wrap,
                             std::size_t left_columns,
                             std::size_t right_columns) {
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

std::size_t CompareFirstChangePresentationRow(const CompareTabState& compare_tab) {
  if (compare_tab.model.hunks.empty()) {
    return 0;
  }
  const compare::CompareHunk& first = compare_tab.model.hunks.front();
  if (const auto presentation_row = CompareTabPresentationRowForHunk(compare_tab, first.index);
      presentation_row.has_value()) {
    return *presentation_row;
  }
  return compare::ComparePresentationRowForModelRow(compare_tab.presentation,
                                                    static_cast<std::size_t>(first.start_row));
}

void SyncCompareCaretToSelectedRow(CompareTabState& compare_tab) {
  if (!compare_tab.right_editable || compare_tab.right_viewport.line_count() == 0) {
    return;
  }
  const std::size_t right_line =
      CompareTabRightLineForModelRow(compare_tab, CompareTabSelectedModelRow(compare_tab));
  if (compare_tab.right_viewport.cursor_line() == right_line) {
    return;
  }
  compare_tab.right_viewport.MoveCursorTo(right_line, 0, false);
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
      compare_tab.wrap_layout.RowForUnitColumn(presentation_row, visual_column, true);
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
  DiffWrapRow row = compare_tab.wrap_layout.RowAt(visual_row);
  if (!row.right_present) {
    row = compare_tab.wrap_layout.RowAt(compare_tab.wrap_layout.RowForUnitColumn(
        presentation_row, std::numeric_limits<std::size_t>::max(), true));
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

}
