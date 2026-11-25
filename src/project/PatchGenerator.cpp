#include "project/PatchGenerator.h"


#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::project {

namespace {

using compare::CompareRow;
using compare::CompareRowKind;

std::string QuoteGitHeaderPath(std::string_view prefix, std::string_view path) {
  std::string combined;
  combined.reserve(prefix.size() + path.size());
  combined.append(prefix);
  combined.append(path);

  const auto needs_escape = [](unsigned char byte) {
    return byte < 0x20 || byte == 0x7f || byte == '"' || byte == '\\' || byte >= 0x80;
  };

  bool needs_quoting = false;
  for (const char raw : combined) {
    if (needs_escape(static_cast<unsigned char>(raw))) {
      needs_quoting = true;
      break;
    }
  }
  if (!needs_quoting) {
    return combined;
  }

  std::string out;
  out.reserve(combined.size() + 2);
  out.push_back('"');
  for (const char raw : combined) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '\a': out += "\\a"; break;
      case '\b': out += "\\b"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\v': out += "\\v"; break;
      case '\f': out += "\\f"; break;
      case '\r': out += "\\r"; break;
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:
        if (byte < 0x20 || byte == 0x7f || byte >= 0x80) {
          out.push_back('\\');
          out.push_back(static_cast<char>('0' + ((byte >> 6) & 0x7)));
          out.push_back(static_cast<char>('0' + ((byte >> 3) & 0x7)));
          out.push_back(static_cast<char>('0' + (byte & 0x7)));
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

void AppendPatchLine(std::ostringstream& stream, char prefix, std::string_view text, bool crlf) {
  stream << prefix << text;
  if (crlf) {
    stream << '\r';
  }
  stream << '\n';
}

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

int PrecedingSideLine(const compare::CompareModel& model, std::size_t start_row, bool right_side) {
  for (std::size_t row = start_row; row-- > 0;) {
    const CompareRow& compare_row = model.rows[row];
    const int line = right_side ? compare_row.right_line : compare_row.left_line;
    if (line > 0) {
      return line;
    }
  }
  return 0;
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

  const bool is_new_file = model.left_absent && !model.right_absent;
  const bool is_deleted_file = !model.left_absent && model.right_absent;

  int last_left_line = 0;
  int last_right_line = 0;
  for (const CompareRow& row : model.rows) {
    last_left_line = std::max(last_left_line, row.left_line);
    last_right_line = std::max(last_right_line, row.right_line);
  }

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
  if (end_row >= start_row) {
    body_lines.reserve(end_row - start_row + 2);
  }
  std::vector<std::uint8_t> adds_after(end_row - start_row + 2, 0);
  std::vector<std::uint8_t> deletes_after(end_row - start_row + 2, 0);
  for (std::size_t row = end_row + 1; row-- > start_row;) {
    const std::size_t slot = row - start_row;
    const CompareRowKind kind = model.rows[row].kind;
    adds_after[slot] = adds_after[slot + 1] ||
                       ((kind == CompareRowKind::Added || kind == CompareRowKind::Modified) ? 1 : 0);
    deletes_after[slot] =
        deletes_after[slot + 1] ||
        ((kind == CompareRowKind::Deleted || kind == CompareRowKind::Modified) ? 1 : 0);
  }
  bool has_change = false;
  for (std::size_t row = start_row; row <= end_row; ++row) {
    const CompareRow& compare_row = model.rows[row];
    switch (compare_row.kind) {
      case CompareRowKind::Unchanged: {
        const std::string_view context_text =
            compare_row.left_text.empty() ? std::string_view(compare_row.right_text)
                                          : std::string_view(compare_row.left_text);
        if (context_text.empty()) {
          const bool left_is_phantom = compare_row.left_line == last_left_line &&
                                       !model.left_final_newline_missing;
          const bool right_is_phantom = compare_row.right_line == last_right_line &&
                                        !model.right_final_newline_missing;
          if (left_is_phantom && right_is_phantom) {
            break;
          }
          if (left_is_phantom) {
            body_lines.push_back(PatchBodyLine{'+', std::string_view(compare_row.right_text),
                                               right_no_newline(compare_row.right_line)});
            has_change = true;
            break;
          }
          if (right_is_phantom) {
            body_lines.push_back(PatchBodyLine{'-', std::string_view(compare_row.left_text),
                                               left_no_newline(compare_row.left_line)});
            has_change = true;
            break;
          }
        }
        const bool left_last_no_nl = left_no_newline(compare_row.left_line);
        const bool right_last_no_nl = right_no_newline(compare_row.right_line);
        const bool new_side_continues = adds_after[row - start_row + 1] != 0;
        const bool old_side_continues = deletes_after[row - start_row + 1] != 0;
        if (left_last_no_nl && new_side_continues) {
          body_lines.push_back(PatchBodyLine{'-', context_text, true});
          body_lines.push_back(PatchBodyLine{'+', context_text, right_last_no_nl});
          has_change = true;
          break;
        }
        if (right_last_no_nl && old_side_continues) {
          body_lines.push_back(PatchBodyLine{'-', context_text, false});
          body_lines.push_back(PatchBodyLine{'+', context_text, true});
          has_change = true;
          break;
        }
        body_lines.push_back(PatchBodyLine{' ', context_text, left_last_no_nl});
        break;
      }
      case CompareRowKind::Deleted:
        if (compare_row.left_text.empty() && compare_row.left_line == last_left_line &&
            !model.left_final_newline_missing) {
          break;
        }
        body_lines.push_back(
            PatchBodyLine{'-', compare_row.left_text, left_no_newline(compare_row.left_line)});
        has_change = true;
        break;
      case CompareRowKind::Added:
        if (compare_row.right_text.empty() && compare_row.right_line == last_right_line &&
            !model.right_final_newline_missing) {
          break;
        }
        body_lines.push_back(
            PatchBodyLine{'+', compare_row.right_text, right_no_newline(compare_row.right_line)});
        has_change = true;
        break;
      case CompareRowKind::Modified: {
        const bool left_is_phantom = compare_row.left_text.empty() &&
                                     compare_row.left_line == last_left_line &&
                                     !model.left_final_newline_missing;
        const bool right_is_phantom = compare_row.right_text.empty() &&
                                      compare_row.right_line == last_right_line &&
                                      !model.right_final_newline_missing;
        if (!left_is_phantom) {
          body_lines.push_back(
              PatchBodyLine{'-', compare_row.left_text, left_no_newline(compare_row.left_line)});
        }
        if (!right_is_phantom) {
          body_lines.push_back(PatchBodyLine{'+', compare_row.right_text,
                                             right_no_newline(compare_row.right_line)});
        }
        has_change = has_change || !left_is_phantom || !right_is_phantom;
        break;
      }
    }
  }

  if (!has_change) {
    return std::nullopt;
  }

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

  const int old_range_count = is_new_file ? 0 : old_count;
  const int new_range_count = is_deleted_file ? 0 : new_count;
  const int old_range_start =
      is_new_file ? 0
      : old_range_count == 0 ? PrecedingSideLine(model, start_row, false)
                             : FirstPatchLineNumber(model, start_row, end_row, false);
  const int new_range_start =
      is_deleted_file ? 0
      : is_new_file   ? 1
      : old_range_count == 0 ? old_range_start + 1
      : new_range_count == 0 ? (old_range_start > 0 ? old_range_start - 1 : 0)
                             : old_range_start;

  const std::string path = relative_path.generic_string();
  const std::string a_path = QuoteGitHeaderPath("a/", path);
  const std::string b_path = QuoteGitHeaderPath("b/", path);

  std::ostringstream stream;
  stream << "diff --git " << a_path << ' ' << b_path << '\n';
  if (is_new_file) {
    stream << "new file mode 100644\n";
    stream << "--- /dev/null\n";
    stream << "+++ " << b_path << '\n';
  } else if (is_deleted_file) {
    stream << "deleted file mode 100644\n";
    stream << "--- " << a_path << '\n';
    stream << "+++ /dev/null\n";
  } else {
    stream << "--- " << a_path << '\n';
    stream << "+++ " << b_path << '\n';
  }
  stream << "@@ -" << old_range_start << ',' << old_range_count << " +" << new_range_start << ','
         << new_range_count << " @@\n";

  for (const PatchBodyLine& line : body_lines) {
    const bool side_crlf = line.prefix == '+' ? model.right_uses_crlf : model.left_uses_crlf;
    AppendPatchLine(stream, line.prefix, line.text, side_crlf && !line.no_newline_after);
    if (line.no_newline_after) {
      stream << kNoNewlineMarker;
    }
    if (options.max_patch_bytes != 0 &&
        static_cast<std::size_t>(stream.tellp()) > options.max_patch_bytes) {
      return std::nullopt;
    }
  }
  return stream.str();
}

}

namespace {

struct LineText {
  std::vector<std::string_view> lines;
  bool final_newline = false;
};

LineText SplitLineText(std::string_view text) {
  LineText out;
  if (text.empty()) {
    return out;
  }
  out.final_newline = text.back() == '\n';
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) {
      out.lines.push_back(text.substr(start));
      break;
    }
    out.lines.push_back(text.substr(start, newline - start));
    start = newline + 1;
  }
  return out;
}

std::string JoinLineText(const std::vector<std::string_view>& lines, bool final_newline) {
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    out.append(lines[i]);
    if (i + 1 < lines.size() || final_newline) {
      out.push_back('\n');
    }
  }
  return out;
}

void SideRangeForRows(const compare::CompareModel& model, std::size_t first_row,
                      std::size_t last_row, bool right_side, std::size_t& first,
                      std::size_t& last) {
  std::size_t previous = 0;
  for (std::size_t row = first_row; row-- > 0;) {
    const int line = right_side ? model.rows[row].right_line : model.rows[row].left_line;
    if (line > 0) {
      previous = static_cast<std::size_t>(line);
      break;
    }
  }
  std::size_t end = previous;
  for (std::size_t row = first_row; row <= last_row; ++row) {
    const int line = right_side ? model.rows[row].right_line : model.rows[row].left_line;
    if (line > 0) {
      end = std::max(end, static_cast<std::size_t>(line));
    }
  }
  first = previous + 1;
  last = end;
}

bool MapRangeThroughDiff(const compare::CompareModel& diff, std::size_t first, std::size_t last,
                         std::size_t& to_first, std::size_t& to_last) {
  if (last + 1 == first) {
    std::size_t after = 0;
    for (const CompareRow& row : diff.rows) {
      if (row.left_line <= 0 || row.left_line >= static_cast<int>(first)) {
        continue;
      }
      if (row.kind == CompareRowKind::Unchanged && row.right_line > 0) {
        after = std::max(after, static_cast<std::size_t>(row.right_line));
      }
    }
    to_first = after + 1;
    to_last = after;
    return true;
  }
  std::optional<std::size_t> lo;
  std::optional<std::size_t> hi;
  std::size_t seen = 0;
  for (const CompareRow& row : diff.rows) {
    if (row.left_line <= 0) {
      continue;
    }
    const auto line = static_cast<std::size_t>(row.left_line);
    if (line < first || line > last) {
      continue;
    }
    if (row.kind != CompareRowKind::Unchanged || row.right_line <= 0) {
      return false;
    }
    const auto to = static_cast<std::size_t>(row.right_line);
    lo = lo.has_value() ? std::min(*lo, to) : to;
    hi = hi.has_value() ? std::max(*hi, to) : to;
    ++seen;
  }
  if (!lo.has_value() || seen != last - first + 1 || *hi - *lo + 1 != seen) {
    return false;
  }
  to_first = *lo;
  to_last = *hi;
  return true;
}

}

PatchChangeSpan ChangeSpanForRows(const compare::CompareModel& model,
                                  std::size_t first_row,
                                  std::size_t last_row) {
  PatchChangeSpan span;
  if (model.rows.empty() || first_row >= model.rows.size()) {
    return span;
  }
  last_row = std::min(last_row, model.rows.size() - 1);
  SideRangeForRows(model, first_row, last_row, false, span.head_first,
                   span.head_last);
  SideRangeForRows(model, first_row, last_row, true, span.worktree_first,
                   span.worktree_last);
  span.covers_end = last_row + 1 >= model.rows.size();
  int last_left = 0;
  int last_right = 0;
  for (const CompareRow& row : model.rows) {
    last_left = std::max(last_left, row.left_line);
    last_right = std::max(last_right, row.right_line);
  }
  const bool left_phantom = !model.left_final_newline_missing;
  const bool right_phantom = !model.right_final_newline_missing;
  const auto clamp_to_real = [](std::size_t& first, std::size_t& last, int last_line,
                                bool phantom) {
    const std::size_t real = static_cast<std::size_t>(std::max(0, last_line)) -
                             ((phantom && last_line > 0) ? 1 : 0);
    first = std::min(first, real + 1);
    last = std::min(last, real);
  };
  clamp_to_real(span.head_first, span.head_last, last_left, left_phantom);
  clamp_to_real(span.worktree_first, span.worktree_last, last_right, right_phantom);
  return span;
}

StagingPatchOutcome BuildStagingPatch(std::string_view head_text,
                                      std::string_view current_index_text,
                                      std::string_view worktree_text,
                                      const PatchChangeSpan& span,
                                      bool stage,
                                      const std::filesystem::path& relative_path,
                                      bool head_exists,
                                      bool index_exists,
                                      bool worktree_exists,
                                      const PatchGenerationOptions& options) {
  StagingPatchOutcome outcome;
  const LineText head = SplitLineText(head_text);
  const LineText index = SplitLineText(current_index_text);
  const LineText worktree = SplitLineText(worktree_text);

  const LineText& from = stage ? head : worktree;
  const LineText& to = stage ? worktree : head;
  const std::size_t from_first = stage ? span.head_first : span.worktree_first;
  const std::size_t from_last = stage ? span.head_last : span.worktree_last;
  const std::size_t to_first = stage ? span.worktree_first : span.head_first;
  const std::size_t to_last = stage ? span.worktree_last : span.head_last;
  if (from_last + 1 < from_first || from_last > from.lines.size() || to_last + 1 < to_first ||
      to_last > to.lines.size()) {
    outcome.span_not_intact = true;
    return outcome;
  }

  const std::string from_joined = JoinLineText(from.lines, from.final_newline);
  const std::string index_joined = JoinLineText(index.lines, index.final_newline);
  compare::CompareBuildOptions lines_only;
  lines_only.refine_hunks = false;
  const compare::CompareModel from_to_index =
      compare::BuildCompareModel(from_joined, index_joined, lines_only);
  std::size_t index_first = 0;
  std::size_t index_last = 0;
  if (!MapRangeThroughDiff(from_to_index, from_first, from_last, index_first, index_last)) {
    outcome.span_not_intact = true;
    return outcome;
  }

  std::vector<std::string_view> desired;
  desired.reserve(index.lines.size() + (to_last + 1 - to_first));
  for (std::size_t i = 1; i < index_first; ++i) {
    desired.push_back(index.lines[i - 1]);
  }
  for (std::size_t i = to_first; i <= to_last; ++i) {
    desired.push_back(to.lines[i - 1]);
  }
  for (std::size_t i = index_last + 1; i <= index.lines.size(); ++i) {
    desired.push_back(index.lines[i - 1]);
  }
  const bool desired_final_newline =
      span.covers_end ? (desired.empty() ? false : to.final_newline) : index.final_newline;
  const std::string desired_joined = JoinLineText(desired, desired_final_newline);
  if (desired_joined == index_joined) {
    outcome.already_applied = true;
    return outcome;
  }

  compare::CompareBuildOptions existence;
  existence.refine_hunks = false;
  existence.left_exists = index_exists;
  existence.right_exists =
      !desired_joined.empty() || (stage ? worktree_exists : head_exists);
  const compare::CompareModel index_to_desired =
      compare::BuildCompareModel(index_joined, desired_joined, existence);
  std::optional<std::size_t> first_change;
  std::optional<std::size_t> last_change;
  for (std::size_t row = 0; row < index_to_desired.rows.size(); ++row) {
    if (index_to_desired.rows[row].kind == CompareRowKind::Unchanged) {
      continue;
    }
    if (!first_change.has_value()) {
      first_change = row;
    }
    last_change = row;
  }
  if (!first_change.has_value()) {
    outcome.already_applied = true;
    return outcome;
  }
  outcome.patch = GenerateComparePatchForRows(index_to_desired, relative_path, *first_change,
                                              *last_change, options);
  return outcome;
}

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

}
