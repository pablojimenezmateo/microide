#include "project/EditorConfig.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "project/GlobMatch.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::project {
namespace {

// EditorConfig keys and the boolean/enum values are case-insensitive; section
// globs are not.
std::string LowerAscii(std::string_view text) {
  std::string lowered(text);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return lowered;
}

std::optional<bool> ParseBool(std::string_view value) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  return std::nullopt;
}

// Append glob fragments matching exactly the canonical decimal spellings of the
// numbers in [lo, hi], both the same digit count: "120".."345" becomes
// "1[2-9][0-9]", "2[0-9][0-9]", "3[0-3][0-9]", "34[0-5]". O(digits) fragments.
void AppendSameWidthRange(std::string_view lo, std::string_view hi, std::string& prefix,
                          std::vector<std::string>& out) {
  if (lo.empty()) {
    out.push_back(prefix);
    return;
  }
  const auto digit_class = [](char from, char to) {
    if (from == to) {
      return std::string(1, from);
    }
    return std::string{'[', from, '-', to, ']'};
  };
  const std::size_t prefix_size = prefix.size();
  if (lo.front() == hi.front()) {
    prefix.push_back(lo.front());
    AppendSameWidthRange(lo.substr(1), hi.substr(1), prefix, out);
    prefix.resize(prefix_size);
    return;
  }
  const std::string_view lo_rest = lo.substr(1);
  const std::string_view hi_rest = hi.substr(1);
  const bool lo_rest_floor = lo_rest.find_first_not_of('0') == std::string_view::npos;
  const bool hi_rest_ceiling = hi_rest.find_first_not_of('9') == std::string_view::npos;
  char middle_from = lo.front();
  char middle_to = hi.front();
  if (!lo_rest_floor) {
    prefix.push_back(lo.front());
    AppendSameWidthRange(lo_rest, std::string(lo_rest.size(), '9'), prefix, out);
    prefix.resize(prefix_size);
    ++middle_from;
  }
  if (!hi_rest_ceiling) {
    --middle_to;
  }
  if (middle_from <= middle_to) {
    std::string fragment = prefix + digit_class(middle_from, middle_to);
    for (std::size_t index = 0; index < lo_rest.size(); ++index) {
      fragment += "[0-9]";
    }
    out.push_back(std::move(fragment));
  }
  if (!hi_rest_ceiling) {
    prefix.push_back(hi.front());
    AppendSameWidthRange(std::string(hi_rest.size(), '0'), hi_rest, prefix, out);
    prefix.resize(prefix_size);
  }
}

// Fragments for every unsigned value in [from, to], split at each digit-count
// boundary so each piece is a same-width range.
void AppendUnsignedRange(std::uint64_t from, std::uint64_t to, std::vector<std::string>& out) {
  std::string prefix;
  std::uint64_t width_floor = 1;
  while (true) {
    while (width_floor <= from / 10) {
      width_floor *= 10;
    }
    const std::uint64_t width_ceiling =
        width_floor > UINT64_MAX / 10 ? UINT64_MAX : width_floor * 10 - 1;
    const std::uint64_t piece_to = std::min(width_ceiling, to);
    AppendSameWidthRange(std::to_string(from), std::to_string(piece_to), prefix, out);
    if (piece_to == to) {
      return;
    }
    from = piece_to + 1;
  }
}

// Fragments matching the canonical decimal spelling of every integer in [lo, hi]
// (none when lo > hi), the meaning EditorConfig gives `{lo..hi}`.
void AppendIntegerRange(std::int64_t lo, std::int64_t hi, std::vector<std::string>& out) {
  if (lo > hi) {
    return;
  }
  if (lo < 0) {
    // -5..-2 is "-" followed by 2..5. Magnitudes stay in uint64 so INT64_MIN fits.
    std::vector<std::string> magnitudes;
    AppendUnsignedRange(hi < 0 ? 0 - static_cast<std::uint64_t>(hi) : 1,
                        0 - static_cast<std::uint64_t>(lo), magnitudes);
    for (const std::string& magnitude : magnitudes) {
      out.push_back("-" + magnitude);
    }
    if (hi < 0) {
      return;
    }
    lo = 0;
  }
  AppendUnsignedRange(static_cast<std::uint64_t>(lo), static_cast<std::uint64_t>(hi), out);
}

// `{lo..hi}` with two signed decimal integers, as EditorConfig spells a range.
std::optional<std::pair<std::int64_t, std::int64_t>> ParseBraceRange(std::string_view body) {
  const std::size_t dots = body.find("..");
  if (dots == std::string_view::npos) {
    return std::nullopt;
  }
  const auto parse = [](std::string_view text) -> std::optional<std::int64_t> {
    if (text.starts_with('+')) {
      text.remove_prefix(1);
    }
    const std::string_view digits = text.starts_with('-') ? text.substr(1) : text;
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string_view::npos) {
      return std::nullopt;
    }
    return util::ParseInt64(text);
  };
  const auto lo = parse(body.substr(0, dots));
  const auto hi = parse(body.substr(dots + 2));
  if (!lo.has_value() || !hi.has_value()) {
    return std::nullopt;
  }
  return std::pair{*lo, *hi};
}

// Brace groups one header may hold. Each expanded group is one level of
// recursion, so this, not the header's length, bounds the stack.
constexpr std::size_t kMaxSectionBraceGroups = 64;

// Expansion steps one header may take. The pattern cap alone does not bound the
// work: a path ending in an empty range (`{a,b}...{5..1}`) emits nothing, so
// 2^groups of them never fill it.
constexpr std::size_t kMaxSectionExpansionSteps = 16 * 1024;

// EditorConfig's brace rules, which are not the search box's `{a,b}`: a group with
// no top-level ',' is either an integer range `{n..m}` or literal text -- `[{a}]`
// names the file "{a}". `steps_left` is the remaining kMaxSectionExpansionSteps.
void ExpandSectionBraces(std::string pattern, std::vector<std::string>& out,
                         std::size_t& steps_left) {
  if (out.size() >= kMaxExpandedGlobPatterns || steps_left == 0) {
    return;
  }
  --steps_left;
  std::size_t depth = 0;
  std::size_t open = std::string::npos;
  std::vector<std::size_t> commas;
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    const char c = pattern[index];
    if (c == '\\') {
      ++index;
      continue;
    }
    if (c == '{') {
      if (depth++ == 0) {
        open = index;
        commas.clear();
      }
    } else if (c == '}' && depth > 0) {
      if (--depth != 0) {
        continue;
      }
      const std::string_view head = std::string_view(pattern).substr(0, open);
      const std::string_view tail = std::string_view(pattern).substr(index + 1);
      const auto emit = [&](std::string_view middle) {
        std::string expanded(head);
        expanded += middle;
        expanded += tail;
        ExpandSectionBraces(std::move(expanded), out, steps_left);
      };
      if (!commas.empty()) {
        // Alternation. Split here rather than through ExpandGlobBraces, which
        // would flatten a nested `{1..3}` or `{a}` before these rules saw it.
        std::size_t alternative_start = open + 1;
        commas.push_back(index);
        for (const std::size_t comma : commas) {
          emit(std::string_view(pattern).substr(alternative_start, comma - alternative_start));
          alternative_start = comma + 1;
        }
        return;
      }
      const std::string_view body = std::string_view(pattern).substr(open + 1, index - open - 1);
      if (const auto range = ParseBraceRange(body); range.has_value()) {
        std::vector<std::string> numbers;
        AppendIntegerRange(range->first, range->second, numbers);
        for (const std::string& number : numbers) {
          emit(number);
        }
        return;
      }
      // Literal: escape both braces so neither expander nor matcher reads them.
      pattern.insert(index, 1, '\\');
      pattern.insert(open, 1, '\\');
      index += 2;
    } else if (c == ',' && depth == 1) {
      commas.push_back(index);
    }
  }
  out.push_back(std::move(pattern));
}

// Turn one section header into the concrete patterns it means, normalized to match
// against a path relative to the .editorconfig's own directory.
//
// EditorConfig's rules, which differ from the search box's:
//   - a pattern containing no '/' matches in any directory ("*.py" is "**/*.py")
//   - a pattern with a leading '/' is anchored to this directory
//   - a pattern that otherwise contains '/' is also anchored here
//   - '{a,b}' alternation and '{n..m}' integer ranges are expanded, and any other
//     brace group is literal (GlobMatches has no brace support)
// There is deliberately no "bare name also means its subtree" expansion: that is a
// search-box affordance and would make "[build]" silently capture build/**.
void AppendSectionPatterns(std::string_view header, std::vector<std::string>& out) {
  std::string_view pattern = util::TrimAsciiWhitespaceView(header);
  if (pattern.empty()) {
    return;
  }
  std::string normalized;
  if (pattern.front() == '/') {
    normalized = std::string(pattern.substr(1));
  } else if (pattern.find('/') == std::string_view::npos) {
    normalized = "**/";
    normalized += pattern;
  } else {
    normalized = std::string(pattern);
  }
  if (std::count(normalized.begin(), normalized.end(), '{') >
      static_cast<std::ptrdiff_t>(kMaxSectionBraceGroups)) {
    return;  // dropped, as the parser drops any header with no pattern
  }
  std::size_t steps_left = kMaxSectionExpansionSteps;
  ExpandSectionBraces(std::move(normalized), out, steps_left);
}

void ApplyProperty(std::string_view key, std::string_view raw_value,
                   EditorConfigProperties& properties) {
  const std::string value = LowerAscii(util::TrimAsciiWhitespaceView(raw_value));
  // "unset" removes the inherited value when the sections fold, so the editor's
  // own setting rules again (the spec: as if the property were never set). It
  // used to return before recording anything, which left the parent section's
  // value standing -- `[plain/*] indent_size = unset` under `[*] indent_size = 2`
  // still gave 2 (found by the QA editorconfig flow).
  if (value == "unset") {
    static constexpr std::pair<std::string_view, unsigned> kUnsetKeys[] = {
        {"indent_style", EditorConfigProperties::kUnsetIndentStyle},
        {"indent_size", EditorConfigProperties::kUnsetIndentSize},
        {"tab_width", EditorConfigProperties::kUnsetTabWidth},
        {"end_of_line", EditorConfigProperties::kUnsetEndOfLine},
        {"trim_trailing_whitespace", EditorConfigProperties::kUnsetTrimTrailingWhitespace},
        {"insert_final_newline", EditorConfigProperties::kUnsetInsertFinalNewline},
        {"max_line_length", EditorConfigProperties::kUnsetMaxLineLength},
    };
    for (const auto& [name, flag] : kUnsetKeys) {
      if (key == name) {
        properties.unset |= flag;
      }
    }
    return;
  }

  if (key == "indent_style") {
    if (value == "space") {
      properties.soft_tabs = true;
    } else if (value == "tab") {
      properties.soft_tabs = false;
    }
    return;
  }
  if (key == "indent_size") {
    if (value == "tab") {
      // Resolved against tab_width after the whole chain is folded, per the spec.
      properties.indent_width = 0;
      return;
    }
    if (const auto parsed = util::ParseInt(value); parsed.has_value() && *parsed > 0) {
      properties.indent_width = *parsed;
    }
    return;
  }
  if (key == "tab_width") {
    if (const auto parsed = util::ParseInt(value); parsed.has_value() && *parsed > 0) {
      properties.tab_size = *parsed;
    }
    return;
  }
  if (key == "end_of_line") {
    if (value == "lf") {
      properties.line_ending = util::LineEnding::LF;
    } else if (value == "crlf") {
      properties.line_ending = util::LineEnding::CRLF;
    } else if (value == "cr") {
      properties.line_ending = util::LineEnding::CR;
    }
    return;
  }
  if (key == "trim_trailing_whitespace") {
    if (const auto parsed = ParseBool(value); parsed.has_value()) {
      properties.trim_trailing_whitespace = *parsed;
    }
    return;
  }
  if (key == "insert_final_newline") {
    if (const auto parsed = ParseBool(value); parsed.has_value()) {
      properties.insert_final_newline = *parsed;
    }
    return;
  }
  if (key == "max_line_length") {
    // `off` is a value, recorded as 0 so a later section's `off` overrides an
    // earlier limit (dropping it left the earlier limit standing); Resolve folds
    // it back to "no limit".
    if (value == "off") {
      properties.max_line_length = 0;
      return;
    }
    if (const auto parsed = util::ParseInt(value); parsed.has_value() && *parsed > 0) {
      properties.max_line_length = *parsed;
    }
    return;
  }
  // Unknown key: ignored, per the EditorConfig spec.
}

}  // namespace

void EditorConfigProperties::MergeOver(const EditorConfigProperties& other) {
  if (other.unset & kUnsetIndentStyle) {
    soft_tabs.reset();
  }
  if (other.unset & kUnsetIndentSize) {
    indent_width.reset();
  }
  if (other.unset & kUnsetTabWidth) {
    tab_size.reset();
  }
  if (other.unset & kUnsetEndOfLine) {
    line_ending.reset();
  }
  if (other.unset & kUnsetTrimTrailingWhitespace) {
    trim_trailing_whitespace.reset();
  }
  if (other.unset & kUnsetInsertFinalNewline) {
    insert_final_newline.reset();
  }
  if (other.unset & kUnsetMaxLineLength) {
    max_line_length.reset();
  }
  if (other.soft_tabs.has_value()) {
    soft_tabs = other.soft_tabs;
  }
  if (other.indent_width.has_value()) {
    indent_width = other.indent_width;
  }
  if (other.tab_size.has_value()) {
    tab_size = other.tab_size;
  }
  if (other.line_ending.has_value()) {
    line_ending = other.line_ending;
  }
  if (other.trim_trailing_whitespace.has_value()) {
    trim_trailing_whitespace = other.trim_trailing_whitespace;
  }
  if (other.insert_final_newline.has_value()) {
    insert_final_newline = other.insert_final_newline;
  }
  if (other.max_line_length.has_value()) {
    max_line_length = other.max_line_length;
  }
}

EditorConfigFile ParseEditorConfig(std::string_view text) {
  EditorConfigFile file;
  // Properties before the first section header are the preamble; only `root`
  // is meaningful there.
  bool in_preamble = true;
  // True while the current section header was not kept (over the cap, or a
  // header with no usable pattern such as `[]`): its properties belong to it,
  // not to the last section that was kept, so they are skipped.
  bool in_dropped_section = false;

  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t newline = text.find('\n', position);
    const std::string_view raw_line =
        newline == std::string_view::npos ? text.substr(position)
                                          : text.substr(position, newline - position);
    position = newline == std::string_view::npos ? text.size() + 1 : newline + 1;

    const std::string_view line = util::TrimAsciiWhitespaceView(raw_line);
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      continue;
    }

    if (line.front() == '[') {
      const std::size_t close = line.rfind(']');
      if (close == std::string_view::npos || close == 0 ||
          close - 1 > kMaxEditorConfigSectionNameBytes) {
        // Unterminated header: skip the line rather than swallowing the rest of
        // the file into a bogus section. An over-long name is skipped the same
        // way; the reference core truncates it, which matches nothing it meant.
        continue;
      }
      in_preamble = false;
      in_dropped_section = true;
      if (file.sections.size() >= kMaxEditorConfigSections) {
        continue;
      }
      EditorConfigFile::Section section;
      AppendSectionPatterns(line.substr(1, close - 1), section.patterns);
      if (section.patterns.empty()) {
        continue;
      }
      file.sections.push_back(std::move(section));
      in_dropped_section = false;
      continue;
    }

    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    const std::string key = LowerAscii(util::TrimAsciiWhitespaceView(line.substr(0, equals)));
    const std::string_view value = line.substr(equals + 1);
    if (key.empty()) {
      continue;
    }

    if (in_preamble) {
      if (key == "root") {
        file.root = ParseBool(LowerAscii(util::TrimAsciiWhitespaceView(value))).value_or(false);
      }
      continue;
    }
    if (in_dropped_section || file.sections.empty()) {
      continue;
    }
    ApplyProperty(key, value, file.sections.back().properties);
  }

  return file;
}

void EditorConfigResolver::SetProjectRoot(std::filesystem::path project_root) {
  std::filesystem::path normalized = project_root.lexically_normal();
  if (normalized == project_root_) {
    return;
  }
  project_root_ = std::move(normalized);
  Invalidate();
}

void EditorConfigResolver::EnsureProjectRoot(
    const std::filesystem::path& normalized_project_root) const {
  if (project_root_ == normalized_project_root) {
    return;
  }
  project_root_ = normalized_project_root;
  Invalidate();
}

void EditorConfigResolver::Invalidate() const {
  util::AddPerformanceCounter(util::PerfCounterId::EditorConfigInvalidations);
  directories_.clear();
  resolved_.clear();
  found_any_config_ = false;
}

const EditorConfigResolver::DirectoryEntry& EditorConfigResolver::EntryForDirectory(
    const std::filesystem::path& directory) const {
  const std::string key = directory.generic_string();
  if (const auto it = directories_.find(key); it != directories_.end()) {
    return it->second;
  }

  // A miss here is the expensive case: a stat, and possibly a read + parse.
  // Counted so a regression that defeats the cache shows up as a rising ratio
  // against EditorConfigResolveQueries rather than as unexplained frame time.
  util::AddPerformanceCounter(util::PerfCounterId::EditorConfigDirectoryReads);
  DirectoryEntry entry;
  const std::filesystem::path config_path = directory / ".editorconfig";
  std::error_code error;
  const bool exists = std::filesystem::is_regular_file(config_path, error);
  if (exists && !error) {
    const auto size = std::filesystem::file_size(config_path, error);
    if (!error && size <= kMaxEditorConfigBytes) {
      if (auto text = util::ReadTextFile(config_path); text.has_value()) {
        entry.config = ParseEditorConfig(*text);
        entry.has_config = true;
        found_any_config_ = true;
      }
    }
  }
  return directories_.emplace(key, std::move(entry)).first->second;
}

const EditorConfigProperties& EditorConfigResolver::Resolve(
    const std::filesystem::path& absolute_path) const {
  static const EditorConfigProperties kEmpty;
  if (project_root_.empty() || absolute_path.empty()) {
    return kEmpty;
  }

  util::AddPerformanceCounter(util::PerfCounterId::EditorConfigResolveQueries);
  // path::native() IS the std::string, so this is a view over storage the path
  // already owns — no allocation on a memo hit, which is the whole point of the
  // memo.
  const std::string_view path_key{absolute_path.native()};
  if (const auto it = resolved_.find(path_key); it != resolved_.end()) {
    return it->second;
  }

  // Everything below is the cold path: ApplyEditorPreferences runs for every tab
  // in every group on any settings change, so the warm ratio here is what keeps
  // this off the frame budget. queries/misses makes that ratio observable.
  util::AddPerformanceCounter(util::PerfCounterId::EditorConfigResolveMisses);
  util::PerformanceTrace::Scope perf_scope("EditorConfigResolver::Resolve");

  // Only files inside the project participate: the upward walk stops at the root,
  // so a `.editorconfig` in a parent of the project (or in $HOME) cannot silently
  // reconfigure it.
  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(absolute_path, project_root_, error);
  const std::string relative_text = error ? std::string{} : relative.generic_string();
  if (relative_text.empty() || relative_text == ".." || relative_text.starts_with("../")) {
    return kEmpty;
  }

  // Collect the chain from the file's directory upward to the project root,
  // stopping at the first `root = true`. Innermost first.
  struct ChainEntry {
    const EditorConfigFile* config;
    // Path of the file being resolved, relative to that config's directory —
    // what the section globs are matched against.
    std::string relative_to_config;
  };
  std::vector<ChainEntry> chain;

  std::filesystem::path directory = absolute_path.parent_path().lexically_normal();
  std::filesystem::path suffix = absolute_path.filename();
  while (true) {
    const DirectoryEntry& entry = EntryForDirectory(directory);
    if (entry.has_config) {
      chain.push_back(ChainEntry{
          .config = &entry.config,
          .relative_to_config = suffix.generic_string(),
      });
      if (entry.config.root) {
        break;
      }
    }
    if (directory == project_root_) {
      break;
    }
    const std::filesystem::path parent = directory.parent_path();
    // parent_path() of "/" is "/": stop rather than spin.
    if (parent.empty() || parent == directory) {
      break;
    }
    suffix = directory.filename() / suffix;
    directory = parent;
  }

  // Fold outermost-first so that a nearer `.editorconfig` wins, and within one
  // file a later matching section wins over an earlier one.
  EditorConfigProperties resolved;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (const EditorConfigFile::Section& section : it->config->sections) {
      bool matched = false;
      for (const std::string& pattern : section.patterns) {
        // EditorConfig's '**' crosses '/' wherever it appears, unlike gitignore's.
        if (GlobMatches(pattern, it->relative_to_config, GlobDoubleStar::Always)) {
          matched = true;
          break;
        }
      }
      if (matched) {
        resolved.MergeOver(section.properties);
      }
    }
  }

  // `indent_size = tab` (recorded as 0) means "follow tab_width"; resolve it here
  // so no consumer has to know the rule. With no tab_width the spec leaves it
  // implementation-defined — drop the opinion and let the user's setting stand.
  if (resolved.indent_width.has_value() && *resolved.indent_width == 0) {
    resolved.indent_width = resolved.tab_size;
  }
  // `indent_style = tab` with no indent_size means `indent_size = tab` per the
  // spec (and the reference implementation), so it follows tab_width as well.
  if (resolved.soft_tabs.has_value() && !*resolved.soft_tabs &&
      !resolved.indent_width.has_value() && resolved.tab_size.has_value()) {
    resolved.indent_width = resolved.tab_size;
  }
  // Per the spec, tab_width defaults to indent_size when unset.
  if (!resolved.tab_size.has_value() && resolved.indent_width.has_value()) {
    resolved.tab_size = resolved.indent_width;
  }
  if (resolved.max_line_length.has_value() && *resolved.max_line_length == 0) {
    resolved.max_line_length.reset();  // `off`
  }

  if (resolved_.size() >= kMaxEditorConfigResolvedPaths) {
    resolved_.clear();
  }
  return resolved_.emplace(path_key, resolved).first->second;
}

}  // namespace microide::project
