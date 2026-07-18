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
        // The `\ No newline at end of file` marker is only valid on the final
        // emitted line of a side. When this shared line is the old file's last line
        // (no trailing newline) but the new side extends past it — or vice versa —
        // emitting it as a marked *context* line makes git treat it as EOF and fuse
        // the following content onto it (silent data corruption on stage/discard).
        // git instead represents such a line as a delete/add pair so the marker
        // lands on the terminating side only; mirror that.
        const bool left_last_no_nl = left_no_newline(compare_row.left_line);
        const bool right_last_no_nl = right_no_newline(compare_row.right_line);
        const bool new_side_continues = compare_row.right_line < last_right_line;
        const bool old_side_continues = compare_row.left_line < last_left_line;
        // The no-newline marker — and the delete/add split that places it — is only
        // valid when this hunk actually reaches the end of the file. For an isolated
        // NON-terminal hunk (e.g. staging one hunk while a later hunk owns the file
        // end) this shared line is mid-file, so a trailing marker would make git
        // treat the hunk as reaching EOF and reject it. Emit ordinary context there.
        const bool hunk_reaches_model_end = end_row + 1 == model.rows.size();
        if (hunk_reaches_model_end && left_last_no_nl && new_side_continues) {
          body_lines.push_back(PatchBodyLine{'-', context_text, true});
          body_lines.push_back(PatchBodyLine{'+', context_text, right_last_no_nl});
          has_change = true;
          break;
        }
        if (hunk_reaches_model_end && right_last_no_nl && old_side_continues) {
          body_lines.push_back(PatchBodyLine{'-', context_text, false});
          body_lines.push_back(PatchBodyLine{'+', context_text, true});
          has_change = true;
          break;
        }
        body_lines.push_back(PatchBodyLine{
            ' ', context_text,
            hunk_reaches_model_end && (left_last_no_nl || right_last_no_nl)});
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
