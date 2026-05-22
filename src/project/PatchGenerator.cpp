#include "project/PatchGenerator.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace microide::project {

namespace {

using compare::CompareRow;
using compare::CompareRowKind;

std::string NormalizePatchLineText(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

void AppendPatchLine(std::ostringstream& stream, char prefix, const std::string& text) {
  stream << prefix << NormalizePatchLineText(text) << '\n';
}

std::size_t ExpandRangeStart(const compare::CompareModel& model,
                             std::size_t start_row,
                             std::size_t context_lines) {
  std::size_t context_remaining = context_lines;
  while (start_row > 0 && context_remaining > 0) {
    const CompareRow& row = model.rows[start_row - 1];
    if (row.kind != CompareRowKind::Unchanged) {
      break;
    }
    --start_row;
    --context_remaining;
  }
  return start_row;
}

std::size_t ExpandRangeEnd(const compare::CompareModel& model,
                           std::size_t end_row,
                           std::size_t context_lines) {
  std::size_t context_remaining = context_lines;
  while (end_row + 1 < model.rows.size() && context_remaining > 0) {
    const CompareRow& row = model.rows[end_row + 1];
    if (row.kind != CompareRowKind::Unchanged) {
      break;
    }
    ++end_row;
    --context_remaining;
  }
  return end_row;
}

int FirstPatchLineNumber(const compare::CompareModel& model,
                         std::size_t start_row,
                         std::size_t end_row,
                         bool right_side) {
  for (std::size_t row = start_row; row <= end_row; ++row) {
    const CompareRow& compare_row = model.rows[row];
    const bool participates =
        right_side ? compare_row.kind == CompareRowKind::Unchanged ||
                         compare_row.kind == CompareRowKind::Added ||
                         compare_row.kind == CompareRowKind::Modified
                   : compare_row.kind == CompareRowKind::Unchanged ||
                         compare_row.kind == CompareRowKind::Deleted ||
                         compare_row.kind == CompareRowKind::Modified;
    if (!participates) {
      continue;
    }
    const int line = right_side ? compare_row.right_line : compare_row.left_line;
    if (line > 0) {
      return line;
    }
  }
  return 1;
}

std::optional<std::string> BuildUnifiedPatch(const compare::CompareModel& model,
                                             const std::filesystem::path& relative_path,
                                             std::size_t start_row,
                                             std::size_t end_row,
                                             const PatchGenerationOptions& options) {
  if (model.rows.empty() || start_row >= model.rows.size()) {
    return std::nullopt;
  }
  end_row = std::min(end_row, model.rows.size() - 1);
  start_row = ExpandRangeStart(model, start_row, options.context_lines);
  end_row = ExpandRangeEnd(model, end_row, options.context_lines);

  struct PatchBodyLine {
    char prefix = ' ';
    std::string text;
  };
  std::vector<PatchBodyLine> body_lines;
  body_lines.reserve(end_row - start_row + 2);
  bool has_change = false;
  for (std::size_t row = start_row; row <= end_row; ++row) {
    const CompareRow& compare_row = model.rows[row];
    switch (compare_row.kind) {
      case CompareRowKind::Unchanged: {
        const std::string context_text = NormalizePatchLineText(
            compare_row.left_text.empty() ? compare_row.right_text : compare_row.left_text);
        if (context_text.empty()) {
          break;
        }
        if (!body_lines.empty() && body_lines.back().prefix == ' ' &&
            NormalizePatchLineText(body_lines.back().text) == context_text) {
          break;
        }
        body_lines.push_back(PatchBodyLine{' ', context_text});
        break;
      }
      case CompareRowKind::Deleted:
        body_lines.push_back(PatchBodyLine{'-', compare_row.left_text});
        has_change = true;
        break;
      case CompareRowKind::Added:
        body_lines.push_back(PatchBodyLine{'+', compare_row.right_text});
        has_change = true;
        break;
      case CompareRowKind::Modified:
        body_lines.push_back(PatchBodyLine{'-', compare_row.left_text});
        body_lines.push_back(PatchBodyLine{'+', compare_row.right_text});
        has_change = true;
        break;
    }
  }

  if (!has_change) {
    return std::nullopt;
  }

  int old_count = 0;
  int new_count = 0;
  for (const PatchBodyLine& line : body_lines) {
    if (line.prefix == ' ' || line.prefix == '-') {
      ++old_count;
    }
    if (line.prefix == ' ' || line.prefix == '+') {
      ++new_count;
    }
  }

  const int old_start = FirstPatchLineNumber(model, start_row, end_row, false);
  const int new_start = FirstPatchLineNumber(model, start_row, end_row, true);
  std::ostringstream body;
  for (const PatchBodyLine& line : body_lines) {
    AppendPatchLine(body, line.prefix, line.text);
  }
  const std::string path = relative_path.generic_string();

  std::ostringstream stream;
  stream << "diff --git a/" << path << " b/" << path << '\n';
  stream << "--- a/" << path << '\n';
  stream << "+++ b/" << path << '\n';
  stream << "@@ -" << old_start << ',' << std::max(old_count, 1) << " +"
         << new_start << ',' << std::max(new_count, 1) << " @@\n";
  stream << body.str();
  return stream.str();
}

}  // namespace

std::optional<std::string> GenerateComparePatch(const compare::CompareModel& model,
                                                const std::filesystem::path& relative_path,
                                                const int hunk_index,
                                                const PatchGenerationOptions& options) {
  if (hunk_index < 0 || static_cast<std::size_t>(hunk_index) >= model.hunks.size()) {
    return std::nullopt;
  }
  const compare::CompareHunk& hunk = model.hunks[static_cast<std::size_t>(hunk_index)];
  return BuildUnifiedPatch(model, relative_path,
                           static_cast<std::size_t>(std::max(0, hunk.start_row)),
                           static_cast<std::size_t>(std::max(0, hunk.end_row)), options);
}

std::optional<std::string> GenerateComparePatchForRows(
    const compare::CompareModel& model,
    const std::filesystem::path& relative_path,
    std::size_t first_model_row,
    std::size_t last_model_row,
    const PatchGenerationOptions& options) {
  if (first_model_row > last_model_row || model.rows.empty()) {
    return std::nullopt;
  }
  last_model_row = std::min(last_model_row, model.rows.size() - 1);
  first_model_row = std::min(first_model_row, last_model_row);
  return BuildUnifiedPatch(model, relative_path, first_model_row, last_model_row, options);
}

std::optional<PatchLineSelection> PatchLineSelectionFromModelRows(
    const compare::CompareModel& model,
    std::size_t first_model_row,
    std::size_t last_model_row) {
  if (model.rows.empty() || first_model_row >= model.rows.size()) {
    return std::nullopt;
  }
  last_model_row = std::min(last_model_row, model.rows.size() - 1);
  first_model_row = std::min(first_model_row, last_model_row);
  return PatchLineSelection{
      .first_model_row = first_model_row,
      .last_model_row = last_model_row,
  };
}

bool PatchLineSelectionHasChanges(const compare::CompareModel& model,
                                  const PatchLineSelection& selection) {
  if (selection.first_model_row >= model.rows.size()) {
    return false;
  }
  const std::size_t last =
      std::min(selection.last_model_row, model.rows.size() - 1);
  for (std::size_t row = selection.first_model_row; row <= last; ++row) {
    if (model.rows[row].kind != CompareRowKind::Unchanged) {
      return true;
    }
  }
  return false;
}

}  // namespace microide::project
