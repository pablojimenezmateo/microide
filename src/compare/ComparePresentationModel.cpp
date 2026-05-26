#include "compare/ComparePresentationModel.h"

#include <algorithm>
#include <sstream>

namespace microide::compare {

namespace {

std::string BuildMetadataSummary(const CompareSemanticFileMetadata& semantic) {
  std::ostringstream stream;
  switch (semantic.file_kind) {
    case CompareSemanticFileKind::Binary:
      stream << "Binary file";
      break;
    case CompareSemanticFileKind::Submodule:
      stream << "Submodule";
      if (semantic.submodule_pointer_changed) {
        stream << " (" << semantic.old_submodule_oid << " -> " << semantic.new_submodule_oid << ')';
      }
      break;
    case CompareSemanticFileKind::Text:
    default:
      break;
  }
  if (semantic.renamed) {
    if (!stream.str().empty()) {
      stream << " · ";
    }
    stream << "Renamed " << semantic.old_path.generic_string() << " -> "
           << semantic.new_path.generic_string();
  }
  if (semantic.mode_changed) {
    if (!stream.str().empty()) {
      stream << " · ";
    }
    stream << "Mode " << (semantic.old_executable ? "executable" : "non-executable") << " -> "
           << (semantic.new_executable ? "executable" : "non-executable");
  }
  if (semantic.line_ending_only) {
    if (!stream.str().empty()) {
      stream << " · ";
    }
    stream << "Line endings only";
  }
  return stream.str();
}

bool RowIsChanged(const CompareRow& row) {
  return row.kind != CompareRowKind::Unchanged;
}

}  // namespace

ComparePresentationModel BuildComparePresentationModel(
    const CompareModel& model,
    const CompareSemanticFileMetadata& semantic,
    const ComparePresentationOptions& options,
    ComparePresentationCollapseState collapse_state,
    std::uint64_t model_generation) {
  ComparePresentationModel presentation;
  presentation.collapse_state = std::move(collapse_state);
  if (presentation.collapse_state.expanded_above.size() != model.hunks.size()) {
    presentation.collapse_state.expanded_above.assign(model.hunks.size(), false);
  }
  if (presentation.collapse_state.expanded_below.size() != model.hunks.size()) {
    presentation.collapse_state.expanded_below.assign(model.hunks.size(), false);
  }

  const std::string metadata_summary = BuildMetadataSummary(semantic);
  if (!metadata_summary.empty()) {
    presentation.rows.push_back(ComparePresentationRow{
        .kind = ComparePresentationRowKind::Metadata,
        .summary_text = metadata_summary,
        .review_marker_label = {},
    });
  }

  if (semantic.file_kind == CompareSemanticFileKind::Binary) {
    RebuildCompareInlineDiffCache(&presentation, model, model_generation, true);
    return presentation;
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
      const int owning_hunk = run_start > 0 ? model.rows[run_start - 1].hunk : -1;
      const bool expand_above =
          owning_hunk >= 0 && owning_hunk < static_cast<int>(presentation.collapse_state.expanded_above.size()) &&
          presentation.collapse_state.expanded_above[static_cast<std::size_t>(owning_hunk)];
      const bool expand_below =
          row_index < model.rows.size() && model.rows[row_index].hunk >= 0 &&
          static_cast<std::size_t>(model.rows[row_index].hunk) <
              presentation.collapse_state.expanded_below.size() &&
          presentation.collapse_state.expanded_below[static_cast<std::size_t>(model.rows[row_index].hunk)];

      const std::size_t visible_prefix =
          expand_above ? std::min(options.context_lines, run_length) : 0;
      const std::size_t visible_suffix =
          expand_below ? std::min(options.context_lines, run_length - visible_prefix) : 0;
      const std::size_t collapsed_count =
          run_length > visible_prefix + visible_suffix
              ? run_length - visible_prefix - visible_suffix
              : 0;

      for (std::size_t i = 0; i < visible_prefix; ++i) {
        presentation.rows.push_back(ComparePresentationRow{
            .kind = ComparePresentationRowKind::Model,
            .model_row_index = run_start + i,
            .summary_text = {},
            .review_marker_label = {},
        });
      }
      if (collapsed_count >= options.collapse_threshold) {
        presentation.rows.push_back(ComparePresentationRow{
            .kind = ComparePresentationRowKind::CollapsedContext,
            .summary_text = std::to_string(collapsed_count) + " unchanged lines hidden",
            .collapsed_line_count = static_cast<int>(collapsed_count),
            .context_above = expand_above,
            .review_marker_label = {},
        });
      } else {
        for (std::size_t i = visible_prefix; i < run_length - visible_suffix; ++i) {
          presentation.rows.push_back(ComparePresentationRow{
              .kind = ComparePresentationRowKind::Model,
              .model_row_index = run_start + i,
              .summary_text = {},
              .review_marker_label = {},
          });
        }
      }
      for (std::size_t i = run_length - visible_suffix; i < run_length; ++i) {
        presentation.rows.push_back(ComparePresentationRow{
            .kind = ComparePresentationRowKind::Model,
            .model_row_index = run_start + i,
            .summary_text = {},
            .review_marker_label = {},
        });
      }
      continue;
    }

    presentation.rows.push_back(ComparePresentationRow{
        .kind = ComparePresentationRowKind::Model,
        .model_row_index = row_index,
        .summary_text = {},
        .review_marker_label = {},
    });
    ++row_index;
  }

  RebuildCompareInlineDiffCache(&presentation, model, model_generation, false);
  return presentation;
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

void RebuildCompareInlineDiffCache(ComparePresentationModel* presentation,
                                   const CompareModel& model,
                                   std::uint64_t model_generation,
                                   bool defer_intraline) {
  if (presentation == nullptr) {
    return;
  }
  presentation->inline_cache.model_generation = model_generation;
  presentation->inline_cache.left_spans_by_row.assign(model.rows.size(), {});
  presentation->inline_cache.right_spans_by_row.assign(model.rows.size(), {});
  if (defer_intraline) {
    return;
  }
  for (std::size_t i = 0; i < model.rows.size(); ++i) {
    presentation->inline_cache.left_spans_by_row[i] = model.rows[i].left_changed_spans;
    presentation->inline_cache.right_spans_by_row[i] = model.rows[i].right_changed_spans;
  }
}

const std::vector<CompareTextSpan>& CompareInlineLeftSpans(
    const ComparePresentationModel& presentation,
    const CompareModel& model,
    std::size_t model_row_index) {
  if (presentation.inline_cache.model_generation != 0 &&
      model_row_index < presentation.inline_cache.left_spans_by_row.size() &&
      !presentation.inline_cache.left_spans_by_row[model_row_index].empty()) {
    return presentation.inline_cache.left_spans_by_row[model_row_index];
  }
  return model.rows[model_row_index].left_changed_spans;
}

const std::vector<CompareTextSpan>& CompareInlineRightSpans(
    const ComparePresentationModel& presentation,
    const CompareModel& model,
    std::size_t model_row_index) {
  if (presentation.inline_cache.model_generation != 0 &&
      model_row_index < presentation.inline_cache.right_spans_by_row.size() &&
      !presentation.inline_cache.right_spans_by_row[model_row_index].empty()) {
    return presentation.inline_cache.right_spans_by_row[model_row_index];
  }
  return model.rows[model_row_index].right_changed_spans;
}

}  // namespace microide::compare
