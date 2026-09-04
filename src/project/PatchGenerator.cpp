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

// Git-compatible C-style quoting for the path token in a patch header. Git wraps
// a pathname in double quotes and C-escapes it whenever it contains a control
// byte, a double quote, a backslash, or a high-bit byte (the core.quotePath
// default) — see git's quote_two_c_style. The `a/` / `b/` prefix is quoted
// together with the path as one token, so `git apply` parses the `--- ` / `+++ `
// / `diff --git` lines unambiguously even when the filename embeds tabs,
// newlines, quotes, or backslashes. Raw emission of such bytes would split or
// corrupt the header. Safe paths (including spaces, which git leaves unquoted)
// are returned verbatim.
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
          // Three-digit octal escape, matching git's `\ooo`.
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

// CompareRow line text never carries a trailing newline (SplitLineViews strips
// both `\n` and `\r\n` endings). git keeps the `\r` of a CRLF file as part of
// each line's blob content, so for a CRLF source the body line must re-emit the
// carriage return *before* the patch's own `\n` separator — otherwise `git
// apply` can't byte-match the context against the CRLF blob. `crlf` is set from
// the originating side's line terminator; it is false for a no-newline final
// line (which has no trailing `\r` either).
void AppendPatchLine(std::ostringstream& stream, char prefix, std::string_view text, bool crlf) {
  stream << prefix << text;
  if (crlf) {
    stream << '\r';
  }
  stream << '\n';
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

// For a zero-length hunk side (a pure insertion has 0 old-side lines; a pure
// deletion has 0 new-side lines), git's `@@ -L,0 @@` / `@@ +M,0 @@` convention
// sets L/M to the line number on that side *immediately preceding* the change —
// i.e. the line after which the opposite side's content is inserted. Scan
// backward from the hunk start for the nearest row that carries a real line
// number on this side; 0 means the change sits at the top of the file.
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

  // Whole-file add/delete need `/dev/null` headers and a zero-length range on the
  // empty side, otherwise `git apply --check` rejects staging the first hunk of a
  // created/removed file. The diff alone can't distinguish "new file" from "a hunk
  // that is all additions" (an empty source buffer becomes one phantom empty
  // line), so the model records the raw emptiness of each side.
  // Absence, not emptiness: an existing file edited down to nothing is an
  // ordinary patch against its (empty) index entry, and staging it as a deletion
  // dropped the entry while the file stayed on disk.
  const bool is_new_file = model.left_absent && !model.right_absent;
  const bool is_deleted_file = !model.left_absent && model.right_absent;

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
  // For each row of the range, whether this patch emits right-side content (a
  // `+` line, or the final-newline addition the trailing phantom stands for)
  // or left-side content after it. A shared line that is a side's LAST line
  // without a newline must become a delete/add pair when this patch continues
  // that side past it — the `\ No newline at end of file` marker is only valid
  // on the final emitted line of a side — and stays a context line otherwise.
  // This used to be decided by whether the range reached the model's end, which
  // fused the new lines onto the old last line whenever the phantom row sat in
  // a later hunk, and dropped a needed marker whenever it did not.
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
          // An Unchanged row's two sides are equal, so both texts are empty here.
          // Each side is a phantom trailing element only if it sits at that side's
          // last line AND that side ended in a newline. Classify each independently:
          const bool left_is_phantom = compare_row.left_line == last_left_line &&
                                       !model.left_final_newline_missing;
          const bool right_is_phantom = compare_row.right_line == last_right_line &&
                                        !model.right_final_newline_missing;
          if (left_is_phantom && right_is_phantom) {
            break;  // Both-sides phantom: not a real line on either side; drop it.
          }
          // One-sided phantom: the phantom side has NO real line here while the other
          // side has a genuine blank line. Emitting it as shared context would add a
          // non-existent line to the phantom side's @@ count and git would reject the
          // pre/post-image. Emit the real side as an add (left phantom) or delete
          // (right phantom) instead.
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
        // See adds_after/deletes_after above.
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
        // A context line that is the OLD file's last line with no newline needs
        // the marker whichever hunk is being staged: the pre-image is matched
        // with its newline state, and this patch leaves the line — and its
        // missing newline — in place. A newline only the NEW side lacks is a
        // change, and it is expressed by the pair above when this patch carries
        // it (the phantom deletion follows the line); otherwise it is not this
        // patch's to state and the line keeps its newline.
        body_lines.push_back(PatchBodyLine{' ', context_text, left_last_no_nl});
        break;
      }
      case CompareRowKind::Deleted:
        // Drop the old side's trailing phantom: SplitLineViews appends one empty
        // element after a file that ends in '\n'. It is not real content, so
        // emitting it as a deletion would add a spurious blank line to the hunk.
        if (compare_row.left_text.empty() && compare_row.left_line == last_left_line &&
            !model.left_final_newline_missing) {
          break;
        }
        body_lines.push_back(
            PatchBodyLine{'-', compare_row.left_text, left_no_newline(compare_row.left_line)});
        has_change = true;
        break;
      case CompareRowKind::Added:
        // Drop the new side's trailing phantom (symmetric with the Deleted case).
        if (compare_row.right_text.empty() && compare_row.right_line == last_right_line &&
            !model.right_final_newline_missing) {
          break;
        }
        body_lines.push_back(
            PatchBodyLine{'+', compare_row.right_text, right_no_newline(compare_row.right_line)});
        has_change = true;
        break;
      case CompareRowKind::Modified: {
        // A side's trailing phantom (see the Deleted/Added cases) can pair with a
        // real line on the other side: a file that gained a last line while
        // losing its final newline aligns the old phantom with the new line.
        // The phantom is not a line, so only the real side is emitted — a `-`
        // for a line that never existed made git reject the hunk.
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

  // A genuine 0 count is valid for a partial selection that touches only one side
  // (e.g. staging just the trailing inserted lines of a file whose preceding row
  // is a modification, so no context is attached). Clamping such a side to 1 emits
  // a header that claims a line the body does not contain, which `git apply`
  // rejects as corrupt. Emit the real 0 count and point the range start at the
  // line preceding the change, matching git's `-L,0` / `+M,0` convention.
  const int old_range_count = is_new_file ? 0 : old_count;
  const int new_range_count = is_deleted_file ? 0 : new_count;
  const int old_range_start =
      is_new_file ? 0
      : old_range_count == 0 ? PrecedingSideLine(model, start_row, false)
                             : FirstPatchLineNumber(model, start_row, end_row, false);
  const int new_range_start =
      is_deleted_file ? 0
      : new_range_count == 0 ? PrecedingSideLine(model, start_row, true)
                             : FirstPatchLineNumber(model, start_row, end_row, true);

  const std::string path = relative_path.generic_string();
  const std::string a_path = QuoteGitHeaderPath("a/", path);
  const std::string b_path = QuoteGitHeaderPath("b/", path);

  // Stream the header then the body into a single buffer (no intermediate
  // full-patch-sized `body` copy), and enforce a byte budget so a whole-file or
  // huge-selection patch copy/stage cannot allocate an arbitrarily large string on
  // the UI/apply path — over budget returns nullopt (surfaced as "no patch").
  // TD-2026-07-17A-099.
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

  // A `+` line is new content destined for the post-image (right) side; a context
  // (` `) or `-` line must byte-match the pre-image (left) side. Re-emit each
  // line's carriage return from its own side's terminator so a CRLF file stages
  // and discards cleanly. A no-newline final line has no trailing `\r`.
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

}  // namespace

namespace {

// A text as lines plus whether it ended in a newline, and back. Split on '\n'
// only, so a CRLF file keeps its '\r' bytes inside the lines and rejoins
// byte-for-byte.
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

// One side's 1-based line range for rows [first_row, last_row]: an insertion
// on that side is `last == first - 1` just after the preceding line.
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

// Where `first..last` (1-based lines of `from`, an insertion point when
// last == first - 1) lands in `to`, given the diff between them: the lines must
// all be Unchanged rows, and contiguous on the `to` side. Returns false when
// they are not — the change has been staged, or the region edited, since.
bool MapRangeThroughDiff(const compare::CompareModel& diff, std::size_t first, std::size_t last,
                         std::size_t& to_first, std::size_t& to_last) {
  if (last + 1 == first) {
    // Insertion point: after `from` line first - 1 (0 = the very start). If that
    // anchor line is itself gone from `to` (a deletion staged earlier), the
    // place is still right after wherever it was, which is after the nearest
    // surviving line before it.
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

}  // namespace

PatchChangeSpan ChangeSpanForRows(const compare::CompareModel& model,
                                  std::size_t first_row,
                                  std::size_t last_row) {
  PatchChangeSpan span;
  if (model.rows.empty() || first_row >= model.rows.size()) {
    return span;
  }
  last_row = std::min(last_row, model.rows.size() - 1);
  SideRangeForRows(model, first_row, last_row, /*right_side=*/false, span.head_first,
                   span.head_last);
  SideRangeForRows(model, first_row, last_row, /*right_side=*/true, span.worktree_first,
                   span.worktree_last);
  // The change reaches the end of the file when its rows do; the final-newline
  // state is then part of it. The trailing phantom element a newline-terminated
  // side carries is not a line: clip it off the range.
  span.covers_end = last_row + 1 >= model.rows.size();
  int last_left = 0;
  int last_right = 0;
  for (const CompareRow& row : model.rows) {
    last_left = std::max(last_left, row.left_line);
    last_right = std::max(last_right, row.right_line);
  }
  // An empty side has the phantom too (its one and only element).
  const bool left_phantom = !model.left_final_newline_missing;
  const bool right_phantom = !model.right_final_newline_missing;
  const auto clamp_to_real = [](std::size_t& first, std::size_t& last, int last_line,
                                bool phantom) {
    const std::size_t real = static_cast<std::size_t>(std::max(0, last_line)) -
                             ((phantom && last_line > 0) ? 1 : 0);
    // An anchor on the phantom is an anchor on the last real line.
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

  // `from` is the side whose lines the change replaces (HEAD for a stage, the
  // working tree for an unstage); `to` is what they become.
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

  // Where the `from` lines sit in the index now.
  const std::string from_joined = JoinLineText(from.lines, from.final_newline);
  const std::string index_joined = JoinLineText(index.lines, index.final_newline);
  const compare::CompareModel from_to_index = compare::BuildCompareModel(from_joined, index_joined);
  std::size_t index_first = 0;
  std::size_t index_last = 0;
  if (!MapRangeThroughDiff(from_to_index, from_first, from_last, index_first, index_last)) {
    outcome.span_not_intact = true;
    return outcome;
  }

  // Splice the `to` lines in.
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

  // The desired state exists whenever it has content, and otherwise exactly
  // when the side it comes from does (staging a deletion removes the entry;
  // staging an emptied file keeps it).
  compare::CompareBuildOptions existence;
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

}  // namespace microide::project
