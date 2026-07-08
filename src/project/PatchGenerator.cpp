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

// CompareRow line text never carries a trailing newline (SplitLineViews strips
// endings), so patch lines are emitted verbatim with a single terminator.
void AppendPatchLine(std::ostringstream& stream, char prefix, std::string_view text) {
  stream << prefix << text << '\n';
}

// git's exact marker (leading backslash-space) after a line whose source file
// lacks a trailing newline.
constexpr std::string_view kNoNewlineMarker = "\\ No newline at end of file\n";

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

  // Whole-file add/delete need `/dev/null` headers and a zero-length range on the
  // empty side, otherwise `git apply --check` rejects staging the first hunk of a
  // created/removed file. The diff alone can't distinguish "new file" from "a hunk
  // that is all additions" (an empty source buffer becomes one phantom empty
  // line), so the model records the raw emptiness of each side.
  const bool is_new_file = model.left_empty && !model.right_empty;
  const bool is_deleted_file = !model.left_empty && model.right_empty;

  // Highest line number present on each side, used to place the
  // `\ No newline at end of file` marker after the file's final line. (A file
  // ending in a newline has a trailing phantom empty line here, but in that case
  // *_final_newline_missing is false, so the marker is never emitted.)
  int last_left_line = 0;
  int last_right_line = 0;
  for (const CompareRow& row : model.rows) {
    last_left_line = std::max(last_left_line, row.left_line);
    last_right_line = std::max(last_right_line, row.right_line);
  }

  // The last line of a side needs a `\ No newline at end of file` marker when
  // that source buffer had no trailing newline.
  const auto left_no_newline = [&](int left_line) {
    return left_line == last_left_line && model.left_final_newline_missing;
  };
  const auto right_no_newline = [&](int right_line) {
    return right_line == last_right_line && model.right_final_newline_missing;
  };

  struct PatchBodyLine {
    char prefix = ' ';
    std::string_view text;
    bool no_newline_after = false;
  };
  std::vector<PatchBodyLine> body_lines;
  // Guard the size_t subtraction: if start_row > end_row (defensive — the compare
  // engine maintains start_row <= end_row, but this hunk-index entry point lacks
  // the explicit first>last guard its GenerateComparePatchForRows sibling has), the
  // reserve would underflow to a near-SIZE_MAX request and throw. The loop below
  // is already a no-op in that case.
  if (end_row >= start_row) {
    body_lines.reserve(end_row - start_row + 2);
  }
  bool has_change = false;
  for (std::size_t row = start_row; row <= end_row; ++row) {
    const CompareRow& compare_row = model.rows[row];
    switch (compare_row.kind) {
      case CompareRowKind::Unchanged: {
        const std::string_view context_text =
            compare_row.left_text.empty() ? std::string_view(compare_row.right_text)
                                          : std::string_view(compare_row.left_text);
        // An empty Unchanged row is either a genuine blank line — which must be
        // emitted as a context line — or the phantom trailing element that
        // SplitLineViews appends for a file ending in a newline, which is not a
        // real line and must be dropped. The phantom is uniquely the globally
        // last row on both axes, and exists only when both sides ended in a
        // newline. A genuine blank line's left/right numbering DIVERGES after an
        // earlier insertion or deletion, so `left_line == right_line` is NOT a
        // valid "real line" test: using it dropped interior blank context lines
        // and desynced the @@ header (making `git apply --check` reject the hunk).
        if (context_text.empty()) {
          const bool is_phantom_trailing_eol =
              compare_row.left_line == last_left_line && !model.left_final_newline_missing &&
              compare_row.right_line == last_right_line && !model.right_final_newline_missing;
          if (is_phantom_trailing_eol) {
            break;
          }
        }
        body_lines.push_back(PatchBodyLine{' ', context_text,
                                           left_no_newline(compare_row.left_line) ||
                                               right_no_newline(compare_row.right_line)});
        break;
      }
      case CompareRowKind::Deleted:
        body_lines.push_back(
            PatchBodyLine{'-', compare_row.left_text, left_no_newline(compare_row.left_line)});
        has_change = true;
        break;
      case CompareRowKind::Added:
        body_lines.push_back(
            PatchBodyLine{'+', compare_row.right_text, right_no_newline(compare_row.right_line)});
        has_change = true;
        break;
      case CompareRowKind::Modified:
        body_lines.push_back(
            PatchBodyLine{'-', compare_row.left_text, left_no_newline(compare_row.left_line)});
        body_lines.push_back(
            PatchBodyLine{'+', compare_row.right_text, right_no_newline(compare_row.right_line)});
        has_change = true;
        break;
    }
  }

  if (!has_change) {
    return std::nullopt;
  }

  // A whole-file add/delete patch must contain only the created (`+`) or removed
  // (`-`) side; the phantom empty line that represents the empty buffer would
  // otherwise emit a spurious context/deletion line that contradicts the
  // `-0,0` / `+0,0` range.
  if (is_new_file) {
    std::erase_if(body_lines, [](const PatchBodyLine& line) { return line.prefix != '+'; });
  } else if (is_deleted_file) {
    std::erase_if(body_lines, [](const PatchBodyLine& line) { return line.prefix != '-'; });
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

  const int old_range_start = is_new_file ? 0 : FirstPatchLineNumber(model, start_row, end_row, false);
  const int new_range_start =
      is_deleted_file ? 0 : FirstPatchLineNumber(model, start_row, end_row, true);
  const int old_range_count = is_new_file ? 0 : std::max(old_count, 1);
  const int new_range_count = is_deleted_file ? 0 : std::max(new_count, 1);

  std::ostringstream body;
  for (const PatchBodyLine& line : body_lines) {
    AppendPatchLine(body, line.prefix, line.text);
    if (line.no_newline_after) {
      body << kNoNewlineMarker;
    }
  }
  const std::string path = relative_path.generic_string();

  std::ostringstream stream;
  stream << "diff --git a/" << path << " b/" << path << '\n';
  if (is_new_file) {
    stream << "new file mode 100644\n";
    stream << "--- /dev/null\n";
    stream << "+++ b/" << path << '\n';
  } else if (is_deleted_file) {
    stream << "deleted file mode 100644\n";
    stream << "--- a/" << path << '\n';
    stream << "+++ /dev/null\n";
  } else {
    stream << "--- a/" << path << '\n';
    stream << "+++ b/" << path << '\n';
  }
  stream << "@@ -" << old_range_start << ',' << old_range_count << " +" << new_range_start << ','
         << new_range_count << " @@\n";
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
