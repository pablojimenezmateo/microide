#include "compare/ComparePresentationModel.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microide::compare {

namespace {

// Appends into a caller-owned buffer rather than returning a string, and uses no
// `std::ostringstream`: the old spelling constructed a stream (and called
// `.str()` — a whole copy of the accumulated text — once per separator test) on
// every presentation rebuild, which is every keystroke in an editable diff. It
// is empty for the overwhelmingly common Text/unrenamed/unchanged-mode case, so
// almost all of that work produced nothing.
void AppendMetadataSummary(const CompareSemanticFileMetadata& semantic, std::string& out) {
  const auto separate = [&out] {
    if (!out.empty()) {
      out += " · ";
    }
  };
  switch (semantic.file_kind) {
    case CompareSemanticFileKind::Binary:
      out += "Binary file";
      break;
    case CompareSemanticFileKind::Submodule:
      out += "Submodule";
      if (semantic.submodule_pointer_changed) {
        out += " (";
        out += semantic.old_submodule_oid;
        out += " -> ";
        out += semantic.new_submodule_oid;
        out += ')';
      }
      break;
    case CompareSemanticFileKind::Text:
    default:
      break;
  }
  if (semantic.renamed) {
    separate();
    out += "Renamed ";
    out += semantic.old_path.generic_string();
    out += " -> ";
    out += semantic.new_path.generic_string();
  }
  if (semantic.mode_changed) {
    separate();
    out += "Mode ";
    out += semantic.old_executable ? "executable" : "non-executable";
    out += " -> ";
    out += semantic.new_executable ? "executable" : "non-executable";
  }
  if (semantic.line_ending_only) {
    separate();
    out += "Line endings only";
  }
}

bool RowIsChanged(const CompareRow& row) {
  return row.kind != CompareRowKind::Unchanged;
}

// Claim the next row of the slab, recycling the one already there.
//
// `rows` is written through a cursor rather than cleared and re-pushed, because
// a rebuild fires on every keystroke in an editable diff and almost never
// changes the row COUNT. Clearing freed three `std::string`s per row and the
// rebuild allocated them straight back with, in the collapsed-run case, a count
// that had not moved (TD-2026-08-17-261). Every field is reset here so a
// recycled row carries nothing from its previous occupant.
ComparePresentationRow& ClaimRow(ComparePresentationModel& presentation, std::size_t& cursor) {
  if (cursor == presentation.rows.size()) {
    presentation.rows.emplace_back();
  }
  ComparePresentationRow& row = presentation.rows[cursor];
  ++cursor;
  row.kind = ComparePresentationRowKind::Model;
  row.model_row_index = 0;
  row.summary_text.clear();
  row.display_summary_text.clear();
  row.hunk_index = -1;
  row.collapsed_line_count = 0;
  row.collapsed_run_start_model_row = 0;
  row.collapsed_run_length = 0;
  row.context_above = false;
  row.previous_hunk_index = -1;
  row.next_hunk_index = -1;
  row.review_marker_label.clear();
  row.has_review_note = false;
  return row;
}

// Finish a rebuild: drop any rows the new model did not need, then compose the
// render-ready summaries.
void FinishPresentationRows(ComparePresentationModel& presentation, std::size_t cursor) {
  presentation.rows.resize(cursor);
  for (ComparePresentationRow& row : presentation.rows) {
    ComposeComparePresentationDisplaySummary(row);
  }
}

}  // namespace

ComparePresentationCollapsedRunState* FindCollapsedRunState(
    std::vector<ComparePresentationCollapsedRunState>& collapsed_runs,
    std::size_t run_start_model_row,
    std::size_t run_length) {
  for (ComparePresentationCollapsedRunState& state : collapsed_runs) {
    if (state.run_start_model_row == run_start_model_row && state.run_length == run_length) {
      return &state;
    }
  }
  return nullptr;
}

const ComparePresentationCollapsedRunState* FindCollapsedRunState(
    const std::vector<ComparePresentationCollapsedRunState>& collapsed_runs,
    std::size_t run_start_model_row,
    std::size_t run_length) {
  for (const ComparePresentationCollapsedRunState& state : collapsed_runs) {
    if (state.run_start_model_row == run_start_model_row && state.run_length == run_length) {
      return &state;
    }
  }
  return nullptr;
}

ComparePresentationModel BuildComparePresentationModel(
    const CompareModel& model,
    const CompareSemanticFileMetadata& semantic,
    const ComparePresentationOptions& options,
    ComparePresentationCollapseState collapse_state) {
  ComparePresentationModel presentation;
  presentation.collapse_state = std::move(collapse_state);
  BuildComparePresentationModelInto(presentation, model, semantic, options);
  return presentation;
}

void BuildComparePresentationModelInto(ComparePresentationModel& presentation,
                                       const CompareModel& model,
                                       const CompareSemanticFileMetadata& semantic,
                                       const ComparePresentationOptions& options) {
  // Double-buffer the collapsed-run list: the previous build's entries have to
  // stay readable (they carry the per-run expand state) while the new ones are
  // written. A swap recycles both buffers; the old spelling COPIED the vector to
  // keep it, on every rebuild, plus a second copy in the by-value parameter.
  presentation.previous_collapsed_runs.swap(presentation.collapse_state.collapsed_runs);
  presentation.collapse_state.collapsed_runs.clear();
  const std::vector<ComparePresentationCollapsedRunState>& previous_collapsed_runs =
      presentation.previous_collapsed_runs;

  std::size_t cursor = 0;
  {
    ComparePresentationRow& metadata_row = ClaimRow(presentation, cursor);
    AppendMetadataSummary(semantic, metadata_row.summary_text);
    if (metadata_row.summary_text.empty()) {
      --cursor;  // No metadata line for this file; give the slot straight back.
    } else {
      metadata_row.kind = ComparePresentationRowKind::Metadata;
    }
  }

  if (semantic.file_kind == CompareSemanticFileKind::Binary) {
    FinishPresentationRows(presentation, cursor);
    return;
  }

  std::size_t row_index = 0;
  while (row_index < model.rows.size()) {
    const CompareRow& row = model.rows[row_index];
    if (!RowIsChanged(row)) {
      std::size_t run_start = row_index;
      while (row_index < model.rows.size() && !RowIsChanged(model.rows[row_index])) {
        ++row_index;
      }
      const std::size_t run_length = row_index - run_start;
      const int previous_hunk_index = run_start > 0 ? model.rows[run_start - 1].hunk : -1;
      const int next_hunk_index =
          row_index < model.rows.size() ? model.rows[row_index].hunk : -1;
      ComparePresentationCollapsedRunState collapsed_run_state;
      collapsed_run_state.run_start_model_row = run_start;
      collapsed_run_state.run_length = run_length;
      if (const ComparePresentationCollapsedRunState* previous_state =
              FindCollapsedRunState(previous_collapsed_runs, run_start, run_length);
          previous_state != nullptr) {
        collapsed_run_state.expanded_above = previous_state->expanded_above;
        collapsed_run_state.expanded_below = previous_state->expanded_below;
      }
      const std::size_t expanded_above = collapsed_run_state.expanded_above;
      const std::size_t expanded_below = collapsed_run_state.expanded_below;
      const std::size_t default_prefix =
          previous_hunk_index >= 0 ? std::min(options.context_lines, run_length) : 0;
      const std::size_t default_suffix =
          next_hunk_index >= 0 ? std::min(options.context_lines, run_length) : 0;
      const std::size_t max_prefix = run_length > default_suffix ? run_length - default_suffix : 0;
      const std::size_t visible_prefix =
          std::min(max_prefix, default_prefix + expanded_above);
      const std::size_t max_suffix = run_length - visible_prefix;
      const std::size_t visible_suffix =
          std::min(max_suffix, default_suffix + expanded_below);
      const std::size_t collapsed_count =
          run_length > visible_prefix + visible_suffix
              ? run_length - visible_prefix - visible_suffix
              : 0;

      for (std::size_t i = 0; i < visible_prefix; ++i) {
        ClaimRow(presentation, cursor).model_row_index = run_start + i;
      }
      if (collapsed_count >= options.collapse_threshold) {
        ComparePresentationRow& summary = ClaimRow(presentation, cursor);
        summary.kind = ComparePresentationRowKind::CollapsedContext;
        util::AppendUnsigned(summary.summary_text, collapsed_count);
        summary.summary_text += " unchanged lines hidden";
        summary.collapsed_line_count = static_cast<int>(collapsed_count);
        summary.collapsed_run_start_model_row = run_start;
        summary.collapsed_run_length = run_length;
        summary.context_above = visible_prefix > 0;
        summary.previous_hunk_index = previous_hunk_index;
        summary.next_hunk_index = next_hunk_index;
        if (expanded_above > 0 || expanded_below > 0) {
          presentation.collapse_state.collapsed_runs.push_back(collapsed_run_state);
        }
      } else {
        for (std::size_t i = visible_prefix; i < run_length - visible_suffix; ++i) {
          ClaimRow(presentation, cursor).model_row_index = run_start + i;
        }
        if (run_length >= options.collapse_threshold &&
            (expanded_above > 0 || expanded_below > 0)) {
          presentation.collapse_state.collapsed_runs.push_back(collapsed_run_state);
        }
      }
      for (std::size_t i = run_length - visible_suffix; i < run_length; ++i) {
        ClaimRow(presentation, cursor).model_row_index = run_start + i;
      }
      continue;
    }

    ClaimRow(presentation, cursor).model_row_index = row_index;
    ++row_index;
  }

  FinishPresentationRows(presentation, cursor);
}

void ComposeComparePresentationDisplaySummary(ComparePresentationRow& row) {
  row.display_summary_text.clear();
  if (!row.review_marker_label.empty()) {
    row.display_summary_text.reserve(row.review_marker_label.size() +
                                     row.summary_text.size() + 4);
    row.display_summary_text += '[';
    row.display_summary_text += row.review_marker_label;
    row.display_summary_text += ']';
    if (row.has_review_note) {
      row.display_summary_text += '*';
    }
    row.display_summary_text += ' ';
    row.display_summary_text += row.summary_text;
  } else if (row.has_review_note) {
    row.display_summary_text = "* ";
    row.display_summary_text += row.summary_text;
  } else {
    row.display_summary_text = row.summary_text;
  }
}

std::optional<std::size_t> ComparePresentationModelRowIndex(
    const ComparePresentationModel& presentation,
    std::size_t model_row_index) {
  for (std::size_t i = 0; i < presentation.rows.size(); ++i) {
    const auto& row = presentation.rows[i];
    if (row.kind == ComparePresentationRowKind::Model && row.model_row_index == model_row_index) {
      return i;
    }
  }
  return std::nullopt;
}

std::size_t ComparePresentationRowForModelRow(const ComparePresentationModel& presentation,
                                              std::size_t model_row_index) {
  // An exact Model row always wins. Otherwise fall to the nearest anchored row
  // whose representative model start is <= the target: presentation rows are in
  // ascending model order, and a collapsed run emits its visible prefix rows,
  // then the CollapsedContext placeholder, then its visible suffix rows — so for
  // a genuinely hidden interior row (which has no Model row of its own) the last
  // anchor at-or-below it is exactly that run's placeholder. Note collapsed_run_
  // length is the *full* run length including the visible context, so a
  // containment test on it would wrongly capture the visible rows — the
  // ascending nearest-anchor rule avoids that entirely.
  std::size_t best = 0;
  bool found = false;
  for (std::size_t i = 0; i < presentation.rows.size(); ++i) {
    const ComparePresentationRow& row = presentation.rows[i];
    std::size_t representative_start = 0;
    if (row.kind == ComparePresentationRowKind::Model) {
      if (row.model_row_index == model_row_index) {
        return i;  // exact visible row
      }
      representative_start = row.model_row_index;
    } else if (row.kind == ComparePresentationRowKind::CollapsedContext) {
      representative_start = row.collapsed_run_start_model_row;
    } else {
      continue;  // Metadata / HunkHeader rows have no model anchor.
    }
    if (representative_start <= model_row_index) {
      best = i;
      found = true;
    }
  }
  return found ? best : 0;
}

std::size_t ComparePresentationToModelRow(const ComparePresentationModel& presentation,
                                          std::size_t presentation_row_index) {
  if (presentation_row_index >= presentation.rows.size()) {
    return 0;
  }
  const auto& row = presentation.rows[presentation_row_index];
  if (row.kind == ComparePresentationRowKind::Model) {
    return row.model_row_index;
  }
  for (std::size_t i = presentation_row_index + 1; i < presentation.rows.size(); ++i) {
    if (presentation.rows[i].kind == ComparePresentationRowKind::Model) {
      return presentation.rows[i].model_row_index;
    }
  }
  for (std::size_t i = presentation_row_index; i-- > 0;) {
    if (presentation.rows[i].kind == ComparePresentationRowKind::Model) {
      return presentation.rows[i].model_row_index;
    }
  }
  return 0;
}

}  // namespace microide::compare
