#include "project/EditorConfig.h"

#include <system_error>
#include <utility>

#include "project/GlobMatch.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/TextFileIO.h"

namespace microide::project {
namespace {

std::string_view TrimAscii(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

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

// Turn one section header into the concrete patterns it means, normalized to match
// against a path relative to the .editorconfig's own directory.
//
// EditorConfig's rules, which differ from the search box's:
//   - a pattern containing no '/' matches in any directory ("*.py" is "**/*.py")
//   - a pattern with a leading '/' is anchored to this directory
//   - a pattern that otherwise contains '/' is also anchored here
//   - '{a,b}' alternation is expanded (GlobMatches has no brace support)
// There is deliberately no "bare name also means its subtree" expansion: that is a
// search-box affordance and would make "[build]" silently capture build/**.
void AppendSectionPatterns(std::string_view header, std::vector<std::string>& out) {
  std::string_view pattern = TrimAscii(header);
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
  ExpandGlobBraces(std::move(normalized), out);
}

void ApplyProperty(std::string_view key, std::string_view raw_value,
                   EditorConfigProperties& properties) {
  const std::string value = LowerAscii(TrimAscii(raw_value));
  // "unset" restores the default by removing any inherited value; modelling that
  // as "no opinion" is right for us because our fallback is the user's setting.
  if (value == "unset") {
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

    const std::string_view line = TrimAscii(raw_line);
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      continue;
    }

    if (line.front() == '[') {
      const std::size_t close = line.rfind(']');
      if (close == std::string_view::npos || close == 0) {
        // Unterminated header: skip the line rather than swallowing the rest of
        // the file into a bogus section.
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
    const std::string key = LowerAscii(TrimAscii(line.substr(0, equals)));
    const std::string_view value = line.substr(equals + 1);
    if (key.empty()) {
      continue;
    }

    if (in_preamble) {
      if (key == "root") {
        file.root = ParseBool(LowerAscii(TrimAscii(value))).value_or(false);
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
  // POSIX path::native() IS the std::string, so this is a view over storage the
  // path already owns — no allocation on a memo hit, which is the whole point of
  // the memo. The generic (forward-slash) form is identical there; Windows would
  // need a materialized conversion, which is why this is guarded rather than
  // unconditional.
#if defined(_WIN32)
  const std::string key_storage = absolute_path.generic_string();
  const std::string_view path_key{key_storage};
#else
  const std::string_view path_key{absolute_path.native()};
#endif
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
  return resolved_.emplace(path_key, std::move(resolved)).first->second;
}

}  // namespace microide::project
