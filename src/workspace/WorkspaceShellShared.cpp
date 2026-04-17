#include "workspace/WorkspaceShellShared.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

std::uint64_t StablePathHash(std::string_view text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : text) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string HashToHex(std::uint64_t value) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string hex(16, '0');
  for (int i = 15; i >= 0; --i) {
    hex[static_cast<std::size_t>(i)] = kDigits[value & 0xfu];
    value >>= 4;
  }
  return hex;
}

}  // namespace

std::string UiScaleLabel(float scale) {
  const int percent = static_cast<int>(std::lround(scale * 100.0f));
  return std::to_string(percent) + "%";
}

std::optional<float> ParseUiScaleValue(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }

  bool percent = false;
  if (text.back() == '%') {
    percent = true;
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  try {
    float scale = std::stof(std::string(text));
    if (!percent && scale > 10.0f) {
      scale *= 0.01f;
    }
    if (percent) {
      scale *= 0.01f;
    }
    if (!std::isfinite(scale)) {
      return std::nullopt;
    }
    return std::clamp(scale, kMinUiScale, kMaxUiScale);
  } catch (...) {
    return std::nullopt;
  }
}

float StepUiScale(float current_scale, int delta) {
  const float clamped_current = std::clamp(current_scale, kMinUiScale, kMaxUiScale);
  if (delta > 0) {
    for (float candidate : kUiScalePresets) {
      if (candidate > clamped_current + 0.001f) {
        return candidate;
      }
    }
    return kMaxUiScale;
  }
  if (delta < 0) {
    for (auto it = kUiScalePresets.rbegin(); it != kUiScalePresets.rend(); ++it) {
      if (*it < clamped_current - 0.001f) {
        return *it;
      }
    }
    return kMinUiScale;
  }
  return clamped_current;
}

GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged) {
  const std::filesystem::path normalized_path = relative_path.lexically_normal();

  GitSidebarEntryTextModel model;
  model.primary_label = normalized_path.filename().string();
  if (model.primary_label.empty()) {
    model.primary_label = normalized_path.empty() ? "." : normalized_path.string();
  }

  const std::filesystem::path parent = normalized_path.parent_path();
  if (!parent.empty() && parent != ".") {
    model.secondary_label = parent.string();
  }
  if (staged) {
    if (!model.secondary_label.empty()) {
      model.secondary_label += "  ";
    }
    model.secondary_label += "[staged]";
  }
  return model;
}

WorkspaceTabTextModel BuildWorkspaceTabTextModel(const std::filesystem::path& project_root,
                                                 const std::filesystem::path& path,
                                                 std::string_view fallback_title,
                                                 bool dirty) {
  std::string title = path.filename().string();
  if (title.empty()) {
    title = fallback_title.empty() ? "untitled" : std::string(fallback_title);
  }

  WorkspaceTabTextModel model;
  model.display_title = dirty ? "*" + title : title;
  model.tooltip_label =
      path.empty() ? (fallback_title.empty() ? "untitled" : std::string(fallback_title))
                   : RelativePathLabel(project_root, path);
  return model;
}

std::string BuildEditorBreadcrumbLabel(const std::filesystem::path& project_root,
                                       const std::filesystem::path& path,
                                       bool placeholder) {
  if (path.empty()) {
    return placeholder ? "welcome" : "untitled";
  }

  return RelativePathLabel(project_root, path);
}

std::string BuildCompareBreadcrumbLabel(const std::filesystem::path& project_root,
                                        const std::filesystem::path& path,
                                        std::string_view left_label,
                                        std::string_view right_label) {
  return RelativePathLabel(project_root, path) + "  |  " + std::string(left_label) + " -> " +
         std::string(right_label);
}

std::string BuildMergeBreadcrumbLabel(const std::filesystem::path& project_root,
                                      const std::filesystem::path& output_path,
                                      std::string_view incoming_label,
                                      std::string_view current_label) {
  return RelativePathLabel(project_root, output_path) + "  |  " + std::string(incoming_label) +
         " -> " + std::string(current_label);
}

std::vector<std::string> SplitSyntaxLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, newline - start));
    start = newline + 1;
  }
  return lines;
}

std::string SerializeLines(const std::vector<std::string>& lines,
                           editor::TextViewport::LineEnding line_ending) {
  std::string separator = "\n";
  if (line_ending == editor::TextViewport::LineEnding::CRLF) {
    separator = "\r\n";
  } else if (line_ending == editor::TextViewport::LineEnding::CR) {
    separator = "\r";
  }

  std::string text;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      text += separator;
    }
    text += lines[i];
  }
  return text;
}

editor::TextViewport::LineEnding DetectLineEnding(std::string_view text) {
  std::size_t crlf_count = 0;
  std::size_t lf_count = 0;
  std::size_t cr_count = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++crlf_count;
        ++i;
      } else {
        ++cr_count;
      }
    } else if (text[i] == '\n') {
      ++lf_count;
    }
  }

  if (crlf_count >= lf_count && crlf_count >= cr_count && crlf_count > 0) {
    return editor::TextViewport::LineEnding::CRLF;
  }
  if (lf_count >= cr_count && lf_count > 0) {
    return editor::TextViewport::LineEnding::LF;
  }
  if (cr_count > 0) {
    return editor::TextViewport::LineEnding::CR;
  }
  return editor::TextViewport::LineEnding::LF;
}

bool RemoveLastUtf8Codepoint(std::string* text) {
  if (text == nullptr || text->empty()) {
    return false;
  }

  std::size_t index = text->size();
  do {
    --index;
  } while (index > 0 &&
           (static_cast<unsigned char>((*text)[index]) & 0xC0u) == 0x80u);
  text->erase(index);
  return true;
}

std::size_t Utf8ByteOffsetForCodepointCount(std::string_view text, std::size_t codepoint_count) {
  std::size_t offset = 0;
  for (std::size_t count = 0; count < codepoint_count && offset < text.size(); ++count) {
    offset += util::Utf8SequenceLength(text, offset);
  }
  return offset;
}

std::size_t Utf8CodepointCount(std::string_view text) {
  std::size_t count = 0;
  for (std::size_t offset = 0; offset < text.size();) {
    offset += util::Utf8SequenceLength(text, offset);
    ++count;
  }
  return count;
}

std::string CollapseWhitespace(std::string_view text) {
  std::string collapsed;
  collapsed.reserve(text.size());
  bool space = false;
  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      space = !collapsed.empty();
      continue;
    }
    if (space) {
      collapsed.push_back(' ');
      space = false;
    }
    collapsed.push_back(c);
  }
  return collapsed;
}

bool QuerySupportsLiteralReplace(std::string_view query) {
  static constexpr std::string_view kRegexMetacharacters = R"(\.^$|()[]{}*+?)";
  return !query.empty() &&
         query.find_first_of(kRegexMetacharacters) == std::string_view::npos;
}

bool UsesCaseSensitiveLiteralMatch(std::string_view query) {
  return std::any_of(query.begin(), query.end(), [](unsigned char c) {
    return std::isupper(c);
  });
}

std::size_t ReplaceLiteralMatchesInText(std::string& content,
                                        std::string_view query,
                                        std::string_view replacement,
                                        bool case_sensitive) {
  if (content.empty() || query.empty()) {
    return 0;
  }

  std::size_t replacements = 0;
  std::size_t search_from = 0;
  if (case_sensitive) {
    while (true) {
      const std::size_t position = content.find(query, search_from);
      if (position == std::string::npos) {
        break;
      }
      content.replace(position, query.size(), replacement);
      search_from = position + replacement.size();
      ++replacements;
    }
    return replacements;
  }

  const std::string lowered_query = ToLower(query);
  std::string lowered_content = ToLower(content);
  const std::string lowered_replacement = ToLower(replacement);
  while (true) {
    const std::size_t position = lowered_content.find(lowered_query, search_from);
    if (position == std::string::npos) {
      break;
    }
    content.replace(position, query.size(), replacement);
    lowered_content.replace(position, query.size(), lowered_replacement);
    search_from = position + replacement.size();
    ++replacements;
  }
  return replacements;
}

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const std::vector<std::string>& lines,
    std::string_view query) {
  std::vector<editor::SelectionRange> matches;
  if (query.empty()) {
    return matches;
  }

  const std::string lowered_query = ToLower(query);
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const std::string lowered_line = ToLower(lines[line_index]);
    std::size_t offset = lowered_line.find(lowered_query);
    while (offset != std::string::npos) {
      matches.push_back(editor::SelectionRange{
          .start = editor::TextPosition{line_index, offset},
          .end = editor::TextPosition{line_index, offset + lowered_query.size()},
      });
      offset = lowered_line.find(lowered_query, offset + 1);
    }
  }

  return matches;
}

ParsedCommandLine ParseCommandLine(std::string_view input) {
  ParsedCommandLine parsed;
  std::string current;
  std::size_t token_start = 0;
  bool token_active = false;
  char quote = '\0';
  bool escaping = false;

  const auto begin_token = [&](std::size_t start) {
    if (!token_active) {
      token_active = true;
      token_start = start;
    }
  };
  const auto flush_token = [&]() {
    if (!token_active) {
      return;
    }
    parsed.tokens.push_back(ParsedCommandToken{current, token_start});
    current.clear();
    token_active = false;
  };

  for (std::size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (escaping) {
      begin_token(i > 0 ? i - 1 : 0);
      current.push_back(c);
      escaping = false;
      parsed.trailing_space = false;
      continue;
    }

    if (quote == '\'') {
      parsed.trailing_space = false;
      if (c == '\'') {
        quote = '\0';
      } else {
        current.push_back(c);
      }
      continue;
    }

    if (quote == '"') {
      parsed.trailing_space = false;
      if (c == '"') {
        quote = '\0';
        continue;
      }
      if (c == '\\') {
        begin_token(token_active ? token_start : i);
        escaping = true;
        continue;
      }
      current.push_back(c);
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(c))) {
      flush_token();
      parsed.trailing_space = true;
      continue;
    }

    parsed.trailing_space = false;
    if (c == '\\') {
      begin_token(i);
      escaping = true;
      continue;
    }
    if (c == '\'' || c == '"') {
      begin_token(i);
      quote = c;
      continue;
    }

    begin_token(i);
    current.push_back(c);
  }

  if (token_active) {
    flush_token();
  }
  parsed.dangling_escape = escaping;
  parsed.open_quote = quote;
  return parsed;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size(), suffix.size()) == suffix;
}

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

std::string CommonPrefix(const std::vector<CommandCompletionCandidate>& candidates) {
  if (candidates.empty()) {
    return {};
  }

  std::string prefix = candidates.front().value;
  for (std::size_t i = 1; i < candidates.size() && !prefix.empty(); ++i) {
    const std::string& value = candidates[i].value;
    std::size_t length = 0;
    while (length < prefix.size() && length < value.size() && prefix[length] == value[length]) {
      ++length;
    }
    prefix.resize(length);
  }
  return prefix;
}

bool CommandArgNeedsQuoting(std::string_view argument) {
  if (argument.empty()) {
    return true;
  }

  return std::any_of(argument.begin(), argument.end(), [](unsigned char c) {
    return !std::isalnum(c) && c != '_' && c != '-' && c != '.' && c != '/' && c != ':';
  });
}

std::string QuoteCommandArg(std::string_view argument) {
  if (!CommandArgNeedsQuoting(argument)) {
    return std::string(argument);
  }

  std::string quoted;
  quoted.reserve(argument.size() + 2);
  quoted.push_back('\'');
  for (char c : argument) {
    if (c == '\'') {
      quoted += "'\\''";
      continue;
    }
    quoted.push_back(c);
  }
  quoted.push_back('\'');
  return quoted;
}

std::string FormatCommandCompletionToken(const CommandCompletionCandidate& candidate) {
  std::string formatted = QuoteCommandArg(candidate.value);
  if (candidate.append_space) {
    formatted.push_back(' ');
  }
  return formatted;
}

std::vector<CommandCompletionCandidate> CompleteFromValues(std::string_view prefix,
                                                           const std::vector<std::string>& values,
                                                           bool append_space) {
  std::vector<CommandCompletionCandidate> matches;
  for (const std::string& value : values) {
    if (!StartsWith(value, prefix)) {
      continue;
    }
    matches.push_back(CommandCompletionCandidate{value, append_space});
  }
  return matches;
}

std::vector<CommandCompletionCandidate> CompletePath(const std::filesystem::path& project_root,
                                                     std::string_view token,
                                                     bool directories_only) {
  std::vector<CommandCompletionCandidate> matches;
  const std::string token_string(token);
  const bool absolute = !token_string.empty() && token_string.front() == '/';
  const bool ends_with_separator =
      !token_string.empty() && (token_string.back() == '/' || token_string.back() == '\\');

  std::filesystem::path typed_path(token_string);
  std::filesystem::path typed_base = ends_with_separator ? typed_path : typed_path.parent_path();
  const std::string leaf_prefix =
      ends_with_separator ? std::string{} : typed_path.filename().string();
  std::filesystem::path search_directory =
      absolute ? (typed_base.empty() ? std::filesystem::path("/") : typed_base)
               : (typed_base.empty() ? project_root : project_root / typed_base);

  std::error_code error;
  std::filesystem::directory_iterator iterator(search_directory, error);
  if (error) {
    return matches;
  }

  for (const auto& entry : iterator) {
    const std::string name = entry.path().filename().string();
    if (!StartsWith(name, leaf_prefix)) {
      continue;
    }

    std::error_code type_error;
    const bool is_directory = entry.is_directory(type_error);
    if (type_error || (directories_only && !is_directory)) {
      continue;
    }

    std::filesystem::path completed_path = typed_base / name;
    if (absolute) {
      completed_path = (typed_base.empty() ? std::filesystem::path("/") : typed_base) / name;
    }

    std::string value = completed_path.lexically_normal().generic_string();
    if (is_directory) {
      if (value.empty()) {
        value = name;
      }
      if (!value.empty() && value.back() != '/') {
        value.push_back('/');
      }
    }

    matches.push_back(CommandCompletionCandidate{
        .value = std::move(value),
        .append_space = !is_directory,
    });
  }

  std::sort(matches.begin(), matches.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.value < rhs.value;
  });
  return matches;
}

std::string JoinCommandArguments(const std::vector<std::string>& args, std::size_t start_index) {
  if (start_index >= args.size()) {
    return {};
  }

  std::string joined;
  for (std::size_t i = start_index; i < args.size(); ++i) {
    if (!joined.empty()) {
      joined.push_back(' ');
    }
    joined += args[i];
  }
  return joined;
}

std::string RelativePathLabel(const std::filesystem::path& root,
                              const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }

  std::error_code error;
  const auto relative = std::filesystem::relative(path, root, error);
  if (!error && !relative.empty() && relative.native().rfind("..", 0) != 0) {
    return relative.lexically_normal().string();
  }
  return path.lexically_normal().string();
}

bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root) {
  const std::filesystem::path normalized_candidate = candidate.lexically_normal();
  const std::filesystem::path normalized_root = root.lexically_normal();
  if (normalized_candidate.empty() || normalized_root.empty()) {
    return false;
  }
  if (normalized_candidate == normalized_root) {
    return true;
  }

  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(normalized_candidate, normalized_root, error);
  if (error || relative.empty()) {
    return false;
  }
  const std::string relative_text = relative.generic_string();
  return relative_text != "." && relative_text != ".." && relative_text.rfind("../", 0) != 0;
}

std::filesystem::path ReplacePathPrefix(const std::filesystem::path& path,
                                        const std::filesystem::path& old_prefix,
                                        const std::filesystem::path& new_prefix) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::filesystem::path normalized_old_prefix = old_prefix.lexically_normal();
  const std::filesystem::path normalized_new_prefix = new_prefix.lexically_normal();
  if (!PathEqualsOrWithin(normalized_path, normalized_old_prefix)) {
    return normalized_path;
  }
  if (normalized_path == normalized_old_prefix) {
    return normalized_new_prefix;
  }

  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(normalized_path, normalized_old_prefix, error);
  if (error || relative.empty()) {
    return normalized_path;
  }
  return (normalized_new_prefix / relative).lexically_normal();
}

std::vector<GitSidebarLineSpec> BuildGitSidebarLineSpecs(
    const std::vector<GitSidebarSection>& entry_sections,
    bool git_repo_available,
    std::string_view git_base_ref,
    std::string_view git_base_label) {
  std::vector<GitSidebarLineSpec> lines;
  std::size_t modified_count = 0;
  std::size_t outgoing_count = 0;
  for (GitSidebarSection section : entry_sections) {
    if (section == GitSidebarSection::Modified) {
      ++modified_count;
    } else {
      ++outgoing_count;
    }
  }

  lines.push_back(GitSidebarLineSpec{
      .kind = GitSidebarLineKind::Header,
      .section = GitSidebarSection::Modified,
      .label = "Changes (" + std::to_string(modified_count) + ")",
  });
  if (modified_count == 0) {
    lines.push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Empty,
        .section = GitSidebarSection::Modified,
        .label = git_repo_available ? "Working tree is clean" : "Not a git repository",
    });
  } else {
    for (std::size_t i = 0; i < entry_sections.size(); ++i) {
      if (entry_sections[i] != GitSidebarSection::Modified) {
        continue;
      }
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Entry,
          .section = GitSidebarSection::Modified,
          .label = {},
          .entry_index = static_cast<int>(i),
      });
    }
  }

  const std::string outgoing_header =
      git_base_label.empty()
          ? "Outgoing files (" + std::to_string(outgoing_count) + ")"
          : "Outgoing files (" + std::to_string(outgoing_count) + ")  " + std::string(git_base_label);
  lines.push_back(GitSidebarLineSpec{
      .kind = GitSidebarLineKind::Header,
      .section = GitSidebarSection::Outgoing,
      .label = outgoing_header,
  });
  if (outgoing_count == 0) {
    lines.push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Empty,
        .section = GitSidebarSection::Outgoing,
        .label = git_base_ref.empty() ? "Base branch unavailable" : "No outgoing files",
    });
  } else {
    for (std::size_t i = 0; i < entry_sections.size(); ++i) {
      if (entry_sections[i] != GitSidebarSection::Outgoing) {
        continue;
      }
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Entry,
          .section = GitSidebarSection::Outgoing,
          .label = {},
          .entry_index = static_cast<int>(i),
      });
    }
  }

  return lines;
}

std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    std::size_t selected_entry_index) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].kind == GitSidebarLineKind::Entry &&
        lines[i].entry_index == static_cast<int>(selected_entry_index)) {
      return i;
    }
  }
  return std::nullopt;
}

std::vector<int> BuildProjectSearchResultLineMap(
    const std::vector<project::ProjectSearchResult>& results) {
  std::vector<int> line_map;
  line_map.reserve(results.size() * 2);

  std::filesystem::path current_path;
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    if (result.relative_path != current_path) {
      current_path = result.relative_path;
      line_map.push_back(-1);
    }
    line_map.push_back(static_cast<int>(i));
  }

  return line_map;
}

int FindProjectSearchResultLine(const std::vector<int>& line_map, std::size_t result_index) {
  for (std::size_t line = 0; line < line_map.size(); ++line) {
    if (line_map[line] == static_cast<int>(result_index)) {
      return static_cast<int>(line);
    }
  }
  return 0;
}

std::string ProjectStateDirectoryName(const std::filesystem::path& project_root) {
  const std::string label =
      project_root.filename().empty() ? "project" : project_root.filename().string();
  std::string sanitized;
  sanitized.reserve(label.size());
  for (const unsigned char c : label) {
    if (std::isalnum(c) != 0 || c == '-' || c == '_') {
      sanitized.push_back(static_cast<char>(c));
    } else {
      sanitized.push_back('-');
    }
  }
  if (sanitized.empty()) {
    sanitized = "project";
  }
  return sanitized + "-" + HashToHex(StablePathHash(project_root.lexically_normal().string()));
}

std::optional<SDL_Color> ParseProjectColor(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  const std::string token(text.substr(start, end - start));
  if (token.size() != 7 || token.front() != '#') {
    return std::nullopt;
  }
  const auto parse_pair = [&](std::size_t offset) -> std::optional<Uint8> {
    const std::string pair = token.substr(offset, 2);
    char* end_ptr = nullptr;
    const long value = std::strtol(pair.c_str(), &end_ptr, 16);
    if (end_ptr == nullptr || *end_ptr != '\0' || value < 0 || value > 255) {
      return std::nullopt;
    }
    return static_cast<Uint8>(value);
  };

  const auto red = parse_pair(1);
  const auto green = parse_pair(3);
  const auto blue = parse_pair(5);
  if (!red.has_value() || !green.has_value() || !blue.has_value()) {
    return std::nullopt;
  }
  return SDL_Color{*red, *green, *blue, 0xff};
}

std::string FormatProjectColor(SDL_Color color) {
  std::ostringstream stream;
  stream << '#'
         << std::hex << std::setfill('0') << std::nouppercase
         << std::setw(2) << static_cast<int>(color.r)
         << std::setw(2) << static_cast<int>(color.g)
         << std::setw(2) << static_cast<int>(color.b);
  return stream.str();
}

SDL_Color DefaultProjectBaseColor(const std::filesystem::path& project_root) {
  static constexpr std::array<SDL_Color, 8> kPalette = {
      SDL_Color{0x66, 0xa4, 0xff, 0xff},
      SDL_Color{0x5d, 0xd0, 0xb4, 0xff},
      SDL_Color{0xff, 0x9d, 0x5c, 0xff},
      SDL_Color{0xe7, 0x7a, 0x9f, 0xff},
      SDL_Color{0xf0, 0xc3, 0x55, 0xff},
      SDL_Color{0x9c, 0x8d, 0xff, 0xff},
      SDL_Color{0xff, 0x75, 0x75, 0xff},
      SDL_Color{0x7a, 0xd5, 0xff, 0xff},
  };
  const std::uint64_t hash = StablePathHash(project_root.lexically_normal().string());
  return kPalette[static_cast<std::size_t>(hash % kPalette.size())];
}

void ApplyProjectAccent(render::Theme& theme, SDL_Color accent) {
  const auto blend = [&](SDL_Color base, SDL_Color tint, float amount) {
    const float clamped_amount = std::clamp(amount, 0.0f, 1.0f);
    const auto mix_component = [&](Uint8 base_component, Uint8 tint_component) {
      return static_cast<Uint8>(
          std::lround(static_cast<float>(base_component) * (1.0f - clamped_amount) +
                      static_cast<float>(tint_component) * clamped_amount));
    };
    return SDL_Color{
        mix_component(base.r, tint.r),
        mix_component(base.g, tint.g),
        mix_component(base.b, tint.b),
        0xff,
    };
  };

  theme.accent = blend(theme.accent, accent, 0.45f);
  theme.chrome_active = blend(theme.chrome_background, accent, 0.18f);
  theme.row_highlight = blend(theme.editor_background, accent, 0.14f);
  const SDL_Color selection = blend(theme.editor_background, accent, 0.36f);
  theme.selection_fill = SDL_Color{selection.r, selection.g, selection.b, 0xb4};
  const SDL_Color search_match = blend(accent, theme.editor_background, 0.52f);
  theme.search_match = SDL_Color{search_match.r, search_match.g, search_match.b, 0x8f};
  const SDL_Color search_match_active = blend(accent, theme.editor_background, 0.38f);
  theme.search_match_active =
      SDL_Color{search_match_active.r, search_match_active.g, search_match_active.b, 0xc8};
}

}  // namespace microide::workspace
