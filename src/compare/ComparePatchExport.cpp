#include "compare/ComparePatchExport.h"

#include <sstream>

namespace microide::compare {

namespace {

void AppendPatchLine(std::ostringstream& stream, char prefix, std::string_view text) {
  stream << prefix << text;
  if (text.empty() || text.back() != '\n') {
    stream << '\n';
  }
}

// Emit the +/-/context body for rows [start_row, end_row]. Shared by the
// hunk-scoped and whole-file exporters so the row-kind switch lives once.
void AppendHunkBody(std::ostringstream& stream, const CompareModel& model, int start_row,
                    int end_row) {
  for (int row = start_row; row <= end_row; ++row) {
    if (row < 0 || static_cast<std::size_t>(row) >= model.rows.size()) {
      continue;
    }
    const CompareRow& compare_row = model.rows[static_cast<std::size_t>(row)];
    switch (compare_row.kind) {
      case CompareRowKind::Deleted:
        AppendPatchLine(stream, '-', compare_row.left_text);
        break;
      case CompareRowKind::Added:
        AppendPatchLine(stream, '+', compare_row.right_text);
        break;
      case CompareRowKind::Modified:
        AppendPatchLine(stream, '-', compare_row.left_text);
        AppendPatchLine(stream, '+', compare_row.right_text);
        break;
      case CompareRowKind::Unchanged:
        AppendPatchLine(stream, ' ', compare_row.left_text);
        break;
    }
  }
}

}  // namespace

std::string FormatCompareHunkPatch(const CompareModel& model,
                                   int hunk_index,
                                   const std::filesystem::path& relative_path) {
  if (hunk_index < 0 || static_cast<std::size_t>(hunk_index) >= model.hunks.size()) {
    return {};
  }
  const CompareHunk& hunk = model.hunks[static_cast<std::size_t>(hunk_index)];
  std::ostringstream stream;
  stream << "--- a/" << relative_path.generic_string() << '\n';
  stream << "+++ b/" << relative_path.generic_string() << '\n';
  stream << "@@ hunk " << (hunk_index + 1) << " @@\n";
  AppendHunkBody(stream, model, hunk.start_row, hunk.end_row);
  return stream.str();
}

std::string FormatCompareFilePatch(const CompareModel& model,
                                   const std::filesystem::path& relative_path) {
  std::ostringstream stream;
  stream << "--- a/" << relative_path.generic_string() << '\n';
  stream << "+++ b/" << relative_path.generic_string() << '\n';
  for (const CompareHunk& hunk : model.hunks) {
    stream << "@@ hunk " << (hunk.index + 1) << " @@\n";
    AppendHunkBody(stream, model, hunk.start_row, hunk.end_row);
  }
  return stream.str();
}

}  // namespace microide::compare
