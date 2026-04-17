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

constexpr float kMenuBarHeight = 25.0f;
constexpr float kProjectTabStripHeight = 32.0f;
constexpr float kTabStripHeight = 34.0f;
constexpr float kHeaderHeight = kWorkspaceHeaderHeight;
constexpr float kDivider = kWorkspaceDividerThickness;
constexpr float kResizeHandleThickness = kWorkspaceResizeHandleThickness;
constexpr float kScrollbarThickness = kWorkspaceScrollbarThickness;
constexpr float kScrollbarInset = kWorkspaceScrollbarInset;
constexpr float kScrollbarMinThumbLength = kWorkspaceScrollbarMinThumbLength;
constexpr float kMinSidebarWidth = kWorkspaceMinSidebarWidth;
constexpr float kMaxSidebarWidth = kWorkspaceMaxSidebarWidth;
constexpr float kMinEditorAreaWidth = kWorkspaceMinEditorAreaWidth;
constexpr float kMinBottomPanelHeight = kWorkspaceMinBottomPanelHeight;
constexpr float kMinEditorAreaHeight = kWorkspaceMinEditorAreaHeight;
constexpr float kBottomPanelHeaderHeight = kWorkspaceBottomPanelHeaderHeight;
constexpr float kBottomPanelCommandReserveHeight = kWorkspaceBottomPanelCommandReserveHeight;
constexpr float kBottomPanelCommandPromptHeight = kWorkspaceBottomPanelCommandPromptHeight;
constexpr float kBottomPanelCommandInset = kWorkspaceBottomPanelCommandInset;
constexpr float kBottomPanelCommandBottomPadding = kWorkspaceBottomPanelCommandBottomPadding;
constexpr float kOverlayMinWidth = 520.0f;
constexpr float kOverlayMaxWidth = 840.0f;
constexpr float kOverlayMinHeight = 220.0f;
constexpr float kOverlayMaxHeight = kWorkspaceOverlayMaxHeight;
constexpr float kDirtyPromptWidth = 460.0f;
constexpr float kDirtyPromptHeight = 176.0f;
constexpr float kDirtyPromptButtonWidth = 96.0f;
constexpr float kDirtyPromptButtonHeight = 28.0f;
constexpr float kDirtyPromptButtonGap = 10.0f;
constexpr float kPromptSurfaceWidth = 520.0f;
constexpr float kPromptSurfaceHeight = 188.0f;
constexpr float kPromptSurfaceInputHeight = 24.0f;
constexpr float kPromptSurfaceButtonWidth = 108.0f;
constexpr float kPromptSurfaceButtonHeight = 28.0f;
constexpr float kPromptSurfaceButtonGap = 10.0f;
constexpr float kEditorSplitDividerThickness = kWorkspaceEditorSplitDividerThickness;
constexpr float kMinSplitPaneExtent = kWorkspaceMinSplitPaneExtent;

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

SDL_FRect MakeRect(float x, float y, float w, float h) {
  return SDL_FRect{x, y, w, h};
}

WorkspaceLayout ComputeLayout(float window_width,
                              float window_height,
                              bool sidebar_visible,
                              bool bottom_panel_visible,
                              float sidebar_width,
                              float bottom_panel_height) {
  const float resolved_bottom_panel_height = bottom_panel_visible ? bottom_panel_height : 0.0f;
  const float resolved_sidebar_width = sidebar_visible ? sidebar_width : 0.0f;

  WorkspaceLayout layout;
  layout.full = MakeRect(0.0f, 0.0f, window_width, window_height);
  layout.menu_bar = MakeRect(0.0f, 0.0f, window_width, kMenuBarHeight);
  layout.project_tab_strip =
      MakeRect(0.0f, kMenuBarHeight, window_width, kProjectTabStripHeight);
  layout.tab_strip =
      MakeRect(0.0f, kMenuBarHeight + kProjectTabStripHeight, window_width, kTabStripHeight);
  layout.bottom_panel =
      MakeRect(0.0f, window_height - resolved_bottom_panel_height, window_width,
               resolved_bottom_panel_height);
  layout.content =
      MakeRect(0.0f, kMenuBarHeight + kProjectTabStripHeight + kTabStripHeight, window_width,
               window_height - kMenuBarHeight - kProjectTabStripHeight - kTabStripHeight -
                   resolved_bottom_panel_height);
  layout.sidebar = MakeRect(0.0f, layout.content.y, resolved_sidebar_width, layout.content.h);
  layout.editor_area =
      MakeRect(resolved_sidebar_width + (sidebar_visible ? kDivider : 0.0f), layout.content.y,
               window_width - resolved_sidebar_width - (sidebar_visible ? kDivider : 0.0f),
               layout.content.h);
  layout.breadcrumb =
      MakeRect(layout.editor_area.x, layout.editor_area.y, layout.editor_area.w, kHeaderHeight);
  layout.editor_surface =
      MakeRect(layout.editor_area.x, layout.editor_area.y + kHeaderHeight + kDivider,
               layout.editor_area.w, layout.editor_area.h - kHeaderHeight - kDivider);
  return layout;
}

std::optional<EditorSplitAxisLayout> ComputeEditorSplitAxisLayout(
    const SDL_FRect& rect,
    bool vertical,
    std::span<const float> size_fractions) {
  if (size_fractions.empty()) {
    return std::nullopt;
  }

  EditorSplitAxisLayout layout;
  layout.vertical = vertical;
  layout.divider_thickness = kEditorSplitDividerThickness;
  layout.min_pane_extent = kMinSplitPaneExtent;
  layout.extents.resize(size_fractions.size(), 0.0f);
  layout.child_rects.reserve(size_fractions.size());
  if (size_fractions.size() > 1) {
    layout.divider_rects.reserve(size_fractions.size() - 1);
  }

  layout.total_extent = std::max(
      0.0f, (vertical ? rect.w : rect.h) -
                kEditorSplitDividerThickness * static_cast<float>(size_fractions.size() - 1));
  std::vector<float> weights(size_fractions.size(), 0.0f);
  float total_weight = 0.0f;
  for (std::size_t i = 0; i < size_fractions.size(); ++i) {
    weights[i] = std::max(0.0f, size_fractions[i]);
    total_weight += weights[i];
  }
  if (total_weight <= 0.0f) {
    std::fill(weights.begin(), weights.end(), 1.0f);
    total_weight = static_cast<float>(weights.size());
  }

  float cursor = vertical ? rect.x : rect.y;
  float remaining_extent = layout.total_extent;
  float remaining_weight = total_weight;
  for (std::size_t i = 0; i < weights.size(); ++i) {
    const std::size_t remaining_children = weights.size() - i;
    float child_extent = remaining_children == 1
                             ? remaining_extent
                             : std::floor(remaining_weight > 0.0f
                                              ? remaining_extent * (weights[i] / remaining_weight)
                                              : remaining_extent /
                                                    static_cast<float>(remaining_children));
    if (remaining_extent > kMinSplitPaneExtent * static_cast<float>(remaining_children)) {
      child_extent = std::clamp(
          child_extent, kMinSplitPaneExtent,
          remaining_extent - kMinSplitPaneExtent * static_cast<float>(remaining_children - 1));
    }

    layout.extents[i] = std::max(0.0f, child_extent);
    layout.child_rects.push_back(vertical ? MakeRect(cursor, rect.y, layout.extents[i], rect.h)
                                          : MakeRect(rect.x, cursor, rect.w, layout.extents[i]));

    cursor += layout.extents[i];
    remaining_extent = std::max(0.0f, remaining_extent - layout.extents[i]);
    remaining_weight = std::max(0.0f, remaining_weight - weights[i]);

    if (i + 1 < weights.size()) {
      layout.divider_rects.push_back(
          vertical ? MakeRect(cursor, rect.y, kEditorSplitDividerThickness, rect.h)
                   : MakeRect(rect.x, cursor, rect.w, kEditorSplitDividerThickness));
      cursor += kEditorSplitDividerThickness;
    }
  }

  return layout;
}

bool Contains(const SDL_FRect& rect, float x, float y) {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

float ClampSidebarWidth(float width, float window_width) {
  const float max_width = std::min(
      kMaxSidebarWidth,
      std::max(kMinSidebarWidth, window_width - kMinEditorAreaWidth - kDivider));
  return std::clamp(width, kMinSidebarWidth, max_width);
}

float ClampBottomPanelHeight(float height, float window_height) {
  const float content_height =
      std::max(0.0f, window_height - kMenuBarHeight - kProjectTabStripHeight - kTabStripHeight);
  const float min_height = std::min(kMinBottomPanelHeight, content_height);
  const float max_height = std::max(min_height, content_height - kMinEditorAreaHeight);
  return std::clamp(height, min_height, max_height);
}

int BottomPanelVisibleRowsForHeight(float panel_height, float line_height, bool command_mode) {
  const float available_height = panel_height - kBottomPanelHeaderHeight - 18.0f -
                                 BottomPanelCommandReservedHeight(command_mode);
  if (line_height <= 0.0f) {
    return 1;
  }
  return std::max(1, static_cast<int>(available_height / line_height));
}

int TailScrollRowForContent(std::size_t line_count, int visible_rows) {
  return std::max(0, static_cast<int>(line_count) - visible_rows);
}

int ClampScrollRowToContent(int scroll_row, std::size_t line_count, int visible_rows) {
  return std::clamp(scroll_row, 0, TailScrollRowForContent(line_count, visible_rows));
}

SDL_FRect SidebarResizeHandleRect(const WorkspaceLayout& layout) {
  if (layout.sidebar.w <= 0.0f || layout.sidebar.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return MakeRect(layout.sidebar.x + layout.sidebar.w - kResizeHandleThickness * 0.5f,
                  layout.sidebar.y, kResizeHandleThickness + kDivider, layout.sidebar.h);
}

SDL_FRect BottomPanelResizeHandleRect(const WorkspaceLayout& layout) {
  if (layout.bottom_panel.w <= 0.0f || layout.bottom_panel.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return MakeRect(layout.bottom_panel.x,
                  layout.bottom_panel.y - kResizeHandleThickness * 0.5f, layout.bottom_panel.w,
                  kResizeHandleThickness + kDivider);
}

float BottomPanelCommandReservedHeight(bool command_mode) {
  return command_mode ? kBottomPanelCommandReserveHeight : 0.0f;
}

SDL_FRect BottomPanelContentRect(const WorkspaceLayout& layout, bool command_mode) {
  return MakeRect(layout.bottom_panel.x, layout.bottom_panel.y + kBottomPanelHeaderHeight,
                  layout.bottom_panel.w,
                  std::max(0.0f, layout.bottom_panel.h - kBottomPanelHeaderHeight -
                                      BottomPanelCommandReservedHeight(command_mode)));
}

SDL_FRect BottomPanelCommandAreaRect(const WorkspaceLayout& layout) {
  const float height = BottomPanelCommandReservedHeight(true);
  return MakeRect(layout.bottom_panel.x, layout.bottom_panel.y + layout.bottom_panel.h - height,
                  layout.bottom_panel.w, height);
}

SDL_FRect BottomPanelCommandPromptRect(const WorkspaceLayout& layout) {
  const SDL_FRect command_area = BottomPanelCommandAreaRect(layout);
  return MakeRect(command_area.x + kBottomPanelCommandInset,
                  command_area.y + command_area.h - kBottomPanelCommandBottomPadding -
                      kBottomPanelCommandPromptHeight,
                  command_area.w - kBottomPanelCommandInset * 2.0f,
                  kBottomPanelCommandPromptHeight);
}

SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full) {
  const float width = std::min(kDirtyPromptWidth, full.w - 32.0f);
  const float height = std::min(kDirtyPromptHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog) {
  const float total_width = kDirtyPromptButtonWidth * 3.0f + kDirtyPromptButtonGap * 2.0f;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kDirtyPromptButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + kDirtyPromptButtonWidth + kDirtyPromptButtonGap, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + (kDirtyPromptButtonWidth + kDirtyPromptButtonGap) * 2.0f, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceRect(const SDL_FRect& full) {
  const float width = std::min(kPromptSurfaceWidth, full.w - 32.0f);
  const float height = std::min(kPromptSurfaceHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 2> ComputePromptSurfaceButtonRects(const SDL_FRect& dialog) {
  const float total_width = kPromptSurfaceButtonWidth * 2.0f + kPromptSurfaceButtonGap;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kPromptSurfaceButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
      MakeRect(start_x + kPromptSurfaceButtonWidth + kPromptSurfaceButtonGap, y,
               kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceInputRect(const SDL_FRect& dialog) {
  return MakeRect(dialog.x + 16.0f, dialog.y + 98.0f, dialog.w - 32.0f,
                  kPromptSurfaceInputHeight);
}

TextGridInteractionLayout ComputeTextGridInteractionLayout(const SDL_FRect& rect,
                                                           float text_x,
                                                           float first_line_y,
                                                           float line_height,
                                                           float char_width,
                                                           std::size_t scroll_line,
                                                           std::size_t line_count,
                                                           std::size_t horizontal_scroll,
                                                           std::size_t visible_rows,
                                                           std::size_t visible_columns) {
  TextGridInteractionLayout layout;
  layout.rect = rect;
  layout.text_x = text_x;
  layout.first_line_y = first_line_y;
  layout.line_height = std::max(1.0f, line_height);
  layout.char_width = std::max(1.0f, char_width);
  layout.line_count = line_count;
  layout.visible_rows = std::max<std::size_t>(1, visible_rows);
  layout.visible_columns = std::max<std::size_t>(1, visible_columns);
  const std::size_t max_scroll_line =
      line_count > layout.visible_rows ? line_count - layout.visible_rows : 0;
  layout.scroll_line = std::min(scroll_line, max_scroll_line);
  layout.horizontal_scroll = horizontal_scroll;
  return layout;
}

std::optional<std::size_t> VisibleTextGridLineAtY(const TextGridInteractionLayout& layout,
                                                  float y) {
  if (layout.line_count == 0 || layout.line_height <= 0.0f || y < layout.first_line_y) {
    return std::nullopt;
  }

  const std::size_t row = static_cast<std::size_t>(
      std::floor((y - layout.first_line_y) / layout.line_height));
  if (row >= layout.visible_rows) {
    return std::nullopt;
  }

  const std::size_t line = layout.scroll_line + row;
  if (line >= layout.line_count) {
    return std::nullopt;
  }
  return line;
}

std::size_t ClampTextGridLineAtY(const TextGridInteractionLayout& layout, float y) {
  if (layout.line_count == 0) {
    return 0;
  }

  const float local_y = std::max(0.0f, y - layout.first_line_y);
  const std::size_t row =
      static_cast<std::size_t>(std::floor(local_y / std::max(1.0f, layout.line_height)));
  return std::min(layout.scroll_line + row, layout.line_count - 1);
}

std::size_t TextGridVisualColumnAtX(const TextGridInteractionLayout& layout, float x) {
  const float local_x = std::max(0.0f, x - layout.text_x);
  return layout.horizontal_scroll +
         static_cast<std::size_t>(
             std::max(0L, std::lround(local_x / std::max(1.0f, layout.char_width))));
}

float TextGridCursorX(const TextGridInteractionLayout& layout, std::size_t visual_column) {
  const std::size_t visible_column =
      visual_column > layout.horizontal_scroll ? visual_column - layout.horizontal_scroll : 0;
  return layout.text_x + static_cast<float>(visible_column) * layout.char_width;
}

float TextGridLineY(const TextGridInteractionLayout& layout, std::size_t line_index) {
  const long long relative_line = static_cast<long long>(line_index) -
                                  static_cast<long long>(layout.scroll_line);
  return layout.first_line_y + static_cast<float>(relative_line) * layout.line_height;
}

ScrollSurfaceLayout ComputeScrollSurfaceLayout(const SDL_FRect& area,
                                               std::size_t total_rows,
                                               int visible_rows,
                                               int requested_vertical_scroll,
                                               std::size_t total_columns,
                                               std::size_t visible_columns,
                                               std::size_t requested_horizontal_scroll) {
  ScrollSurfaceLayout layout;
  layout.visible_rows = std::max(1, visible_rows);
  layout.max_vertical_scroll = TailScrollRowForContent(total_rows, layout.visible_rows);
  layout.vertical_scroll =
      ClampScrollRowToContent(requested_vertical_scroll, total_rows, layout.visible_rows);
  layout.visible_columns = std::max<std::size_t>(1, visible_columns);
  layout.max_horizontal_scroll =
      total_columns > layout.visible_columns ? total_columns - layout.visible_columns : 0;
  layout.horizontal_scroll = std::min(requested_horizontal_scroll, layout.max_horizontal_scroll);
  layout.show_vertical = layout.max_vertical_scroll > 0;
  layout.show_horizontal = layout.max_horizontal_scroll > 0;

  const float reserved_width = layout.show_vertical ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
  const float reserved_height =
      layout.show_horizontal ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
  layout.content_rect =
      MakeRect(area.x, area.y, std::max(0.0f, area.w - reserved_width),
               std::max(0.0f, area.h - reserved_height));
  layout.vertical_scrollbar = MakeVerticalScrollbarGeometry(
      area, static_cast<float>(total_rows), static_cast<float>(layout.visible_rows),
      static_cast<float>(layout.vertical_scroll), layout.show_horizontal);
  layout.horizontal_scrollbar = MakeHorizontalScrollbarGeometry(
      area, static_cast<float>(total_columns), static_cast<float>(layout.visible_columns),
      static_cast<float>(layout.horizontal_scroll), layout.show_vertical);
  return layout;
}

ScrollableListLayout ComputeScrollableListLayout(const SDL_FRect& container,
                                                 float list_y,
                                                 std::size_t item_count,
                                                 int requested_scroll_row,
                                                 float horizontal_inset,
                                                 float row_step,
                                                 float row_height,
                                                 float list_bottom_padding,
                                                 float scrollbar_bottom_padding,
                                                 bool fractional_visible_units) {
  ScrollableListLayout layout;
  layout.row_x = container.x + horizontal_inset;
  layout.row_y = list_y;
  layout.row_step = row_step;
  layout.row_height = row_height;

  const float available_height =
      std::max(0.0f, container.y + container.h - list_y - list_bottom_padding);
  const float raw_visible_units =
      row_step > 0.0f ? available_height / row_step : 0.0f;
  layout.visible_units =
      fractional_visible_units
          ? std::max(1.0f, raw_visible_units)
          : static_cast<float>(
                std::max(1, static_cast<int>(std::floor(std::max(0.0f, raw_visible_units)))));
  layout.visible_rows =
      std::max(1, static_cast<int>(std::floor(std::max(1.0f, layout.visible_units))));
  layout.max_scroll =
      std::max(0, static_cast<int>(std::ceil(static_cast<float>(item_count) - layout.visible_units)));
  layout.scroll_row = std::clamp(requested_scroll_row, 0, layout.max_scroll);
  layout.row_width =
      std::max(0.0f, container.w - horizontal_inset * 2.0f -
                         (layout.max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
  layout.list_rect =
      MakeRect(container.x, list_y, container.w,
               std::max(0.0f, container.y + container.h - list_y - scrollbar_bottom_padding));
  layout.scrollbar =
      MakeVerticalScrollbarGeometry(layout.list_rect, static_cast<float>(item_count),
                                    layout.visible_units, static_cast<float>(layout.scroll_row));
  return layout;
}

int RevealScrollableListIndex(const ScrollableListLayout& layout, int selected_index) {
  if (selected_index < 0) {
    return layout.scroll_row;
  }

  int scroll_row = layout.scroll_row;
  if (selected_index < scroll_row) {
    scroll_row = selected_index;
  } else if (selected_index >= scroll_row + layout.visible_rows) {
    scroll_row = selected_index - layout.visible_rows + 1;
  }
  return std::clamp(scroll_row, 0, layout.max_scroll);
}

std::optional<int> ScrollableListIndexAtY(const ScrollableListLayout& layout, float y) {
  if (layout.row_step <= 0.0f || y < layout.row_y ||
      y >= layout.row_y + static_cast<float>(layout.visible_rows) * layout.row_step) {
    return std::nullopt;
  }

  const int row = static_cast<int>(std::floor((y - layout.row_y) / layout.row_step));
  return layout.scroll_row + row;
}

SDL_FRect ScrollableListRowRect(const ScrollableListLayout& layout, int visible_row) {
  return MakeRect(layout.row_x, layout.row_y + static_cast<float>(visible_row) * layout.row_step,
                  layout.row_width, layout.row_height);
}

std::optional<TerminalSelectionBounds> NormalizeTerminalSelection(
    std::optional<TerminalSelectionPoint> anchor,
    std::optional<TerminalSelectionPoint> head) {
  if (!anchor.has_value() || !head.has_value()) {
    return std::nullopt;
  }

  TerminalSelectionBounds bounds{.start = *anchor, .end = *head};
  if (bounds.start.row > bounds.end.row ||
      (bounds.start.row == bounds.end.row && bounds.start.column > bounds.end.column)) {
    std::swap(bounds.start, bounds.end);
  }
  return bounds;
}

terminal::TerminalSession::MouseButton TerminalMouseButtonFromSdl(Uint8 button) {
  switch (button) {
    case SDL_BUTTON_LEFT:
      return terminal::TerminalSession::MouseButton::Left;
    case SDL_BUTTON_MIDDLE:
      return terminal::TerminalSession::MouseButton::Middle;
    case SDL_BUTTON_RIGHT:
      return terminal::TerminalSession::MouseButton::Right;
    default:
      return terminal::TerminalSession::MouseButton::None;
  }
}

std::string ExtractTerminalSelectionText(const std::vector<terminal::TerminalLine>& lines,
                                         const TerminalSelectionBounds& selection) {
  if (lines.empty() || selection.start.row >= lines.size()) {
    return {};
  }

  const std::size_t end_row = std::min(selection.end.row, lines.size() - 1);
  std::string text;
  for (std::size_t row = selection.start.row; row <= end_row; ++row) {
    const auto& line = lines[row];
    const std::size_t line_size = line.cells.size();
    const std::size_t start_column =
        row == selection.start.row ? std::min(selection.start.column, line_size) : 0;
    const std::size_t end_column =
        row == end_row ? std::min(selection.end.column, line_size) : line_size;
    for (std::size_t column = start_column; column < end_column; ++column) {
      text.append(line.cells[column].DisplayText());
    }
    if (row != end_row) {
      text.push_back('\n');
    }
  }
  return text;
}

bool TerminalSelectionContainsCell(const TerminalSelectionBounds& selection,
                                   std::size_t row,
                                   std::size_t column) {
  if (row < selection.start.row || row > selection.end.row) {
    return false;
  }
  if (selection.start.row == selection.end.row) {
    return row == selection.start.row && column >= selection.start.column &&
           column < selection.end.column;
  }
  if (row == selection.start.row) {
    return column >= selection.start.column;
  }
  if (row == selection.end.row) {
    return column < selection.end.column;
  }
  return true;
}

std::optional<SDL_FRect> ComputeScrollbarThumb(const SDL_FRect& track,
                                               float total_units,
                                               float visible_units,
                                               float scroll_units,
                                               bool vertical) {
  if (track.w <= 0.0f || track.h <= 0.0f || total_units <= visible_units || visible_units <= 0.0f) {
    return std::nullopt;
  }

  const float track_length = vertical ? track.h : track.w;
  if (track_length <= 0.0f) {
    return std::nullopt;
  }

  const float max_scroll = std::max(0.0f, total_units - visible_units);
  const float clamped_scroll = std::clamp(scroll_units, 0.0f, max_scroll);
  const float thumb_length = std::clamp(track_length * (visible_units / total_units),
                                        kScrollbarMinThumbLength, track_length);
  const float travel = std::max(0.0f, track_length - thumb_length);
  const float offset =
      (travel <= 0.0f || max_scroll <= 0.0f) ? 0.0f : (clamped_scroll / max_scroll) * travel;

  if (vertical) {
    return MakeRect(track.x, track.y + offset, track.w, thumb_length);
  }
  return MakeRect(track.x + offset, track.y, thumb_length, track.h);
}

std::optional<ScrollbarGeometry> MakeVerticalScrollbarGeometry(const SDL_FRect& area,
                                                              float total_units,
                                                              float visible_units,
                                                              float scroll_units,
                                                              bool reserve_horizontal) {
  const SDL_FRect track = MakeRect(
      area.x + area.w - kScrollbarThickness - kScrollbarInset, area.y + kScrollbarInset,
      kScrollbarThickness,
      std::max(0.0f, area.h - kScrollbarInset * 2.0f -
                           (reserve_horizontal ? kScrollbarThickness + kScrollbarInset : 0.0f)));
  const auto thumb = ComputeScrollbarThumb(track, total_units, visible_units, scroll_units, true);
  if (!thumb.has_value()) {
    return std::nullopt;
  }
  return ScrollbarGeometry{
      .track = track,
      .thumb = *thumb,
      .total_units = total_units,
      .visible_units = visible_units,
      .scroll_units = scroll_units,
      .vertical = true,
  };
}

std::optional<ScrollbarGeometry> MakeHorizontalScrollbarGeometry(const SDL_FRect& area,
                                                                float total_units,
                                                                float visible_units,
                                                                float scroll_units,
                                                                bool reserve_vertical) {
  const SDL_FRect track = MakeRect(
      area.x + kScrollbarInset, area.y + area.h - kScrollbarThickness - kScrollbarInset,
      std::max(0.0f, area.w - kScrollbarInset * 2.0f -
                           (reserve_vertical ? kScrollbarThickness + kScrollbarInset : 0.0f)),
      kScrollbarThickness);
  const auto thumb = ComputeScrollbarThumb(track, total_units, visible_units, scroll_units, false);
  if (!thumb.has_value()) {
    return std::nullopt;
  }
  return ScrollbarGeometry{
      .track = track,
      .thumb = *thumb,
      .total_units = total_units,
      .visible_units = visible_units,
      .scroll_units = scroll_units,
      .vertical = false,
  };
}

float ScrollUnitsForPointer(const ScrollbarGeometry& geometry,
                            float pointer_coordinate,
                            float grab_offset) {
  const float track_start = geometry.vertical ? geometry.track.y : geometry.track.x;
  const float track_length = geometry.vertical ? geometry.track.h : geometry.track.w;
  const float thumb_length = geometry.vertical ? geometry.thumb.h : geometry.thumb.w;
  const float max_scroll = std::max(0.0f, geometry.total_units - geometry.visible_units);
  const float travel = std::max(0.0f, track_length - thumb_length);
  if (travel <= 0.0f || max_scroll <= 0.0f) {
    return 0.0f;
  }

  const float thumb_start = std::clamp(pointer_coordinate - grab_offset, track_start,
                                       track_start + travel);
  return ((thumb_start - track_start) / travel) * max_scroll;
}

std::vector<CompareScrollbarMarker> BuildCompareScrollbarMarkers(
    const SDL_FRect& track,
    const compare::CompareModel& model) {
  std::vector<CompareScrollbarMarker> markers;
  if (track.w <= 0.0f || track.h <= 0.0f || model.rows.empty()) {
    return markers;
  }

  const float total_rows = static_cast<float>(model.rows.size());
  const float track_end = track.y + track.h;
  auto push_marker = [&](int start_row, int end_row, compare::CompareRowKind kind) {
    if (kind == compare::CompareRowKind::Unchanged || start_row < 0 || end_row <= start_row) {
      return;
    }

    const float top = track.y + (static_cast<float>(start_row) / total_rows) * track.h;
    const float bottom = track.y + (static_cast<float>(end_row) / total_rows) * track.h;
    float y = std::clamp(std::floor(top), track.y, std::max(track.y, track_end - 1.0f));
    float height = std::max(2.0f, std::ceil(bottom) - y);
    if (y + height > track_end) {
      y = std::max(track.y, track_end - height);
      height = std::min(height, track_end - y);
    }
    if (height <= 0.0f) {
      return;
    }

    markers.push_back(CompareScrollbarMarker{
        .kind = kind,
        .start_row = start_row,
        .end_row = end_row,
        .rect = MakeRect(track.x, y, track.w, height),
    });
  };

  int run_start = -1;
  compare::CompareRowKind run_kind = compare::CompareRowKind::Unchanged;
  for (std::size_t i = 0; i < model.rows.size(); ++i) {
    const compare::CompareRowKind kind = model.rows[i].kind;
    if (kind == compare::CompareRowKind::Unchanged) {
      push_marker(run_start, static_cast<int>(i), run_kind);
      run_start = -1;
      run_kind = compare::CompareRowKind::Unchanged;
      continue;
    }

    if (run_start >= 0 && kind == run_kind) {
      continue;
    }

    push_marker(run_start, static_cast<int>(i), run_kind);
    run_start = static_cast<int>(i);
    run_kind = kind;
  }
  push_marker(run_start, static_cast<int>(model.rows.size()), run_kind);
  return markers;
}

std::vector<MergeScrollbarMarker> BuildMergeScrollbarMarkers(
    const SDL_FRect& track,
    std::size_t total_rows,
    const std::vector<MergeScrollbarMarkerInput>& inputs) {
  std::vector<MergeScrollbarMarker> markers;
  if (track.w <= 0.0f || track.h <= 0.0f || total_rows == 0 || inputs.empty()) {
    return markers;
  }

  const float total_units = static_cast<float>(total_rows);
  const float track_end = track.y + track.h;
  for (const MergeScrollbarMarkerInput& input : inputs) {
    if (input.end_row <= input.start_row || input.start_row < 0) {
      continue;
    }

    const float top = track.y + (static_cast<float>(input.start_row) / total_units) * track.h;
    const float bottom = track.y + (static_cast<float>(input.end_row) / total_units) * track.h;
    float y = std::clamp(std::floor(top), track.y, std::max(track.y, track_end - 1.0f));
    float height = std::max(2.0f, std::ceil(bottom) - y);
    if (y + height > track_end) {
      y = std::max(track.y, track_end - height);
      height = std::min(height, track_end - y);
    }
    if (height <= 0.0f) {
      continue;
    }

    markers.push_back(MergeScrollbarMarker{
        .start_row = input.start_row,
        .end_row = input.end_row,
        .choice = input.choice,
        .valid = input.valid,
        .rect = MakeRect(track.x, y, track.w, height),
    });
  }
  return markers;
}

float ComputeChromeButtonWidth(float measured_label_width) {
  return std::clamp(measured_label_width + 18.0f, 64.0f, 160.0f);
}

std::vector<StripSlotLayout> ComputeVisibleStripLayouts(const std::vector<float>& widths,
                                                        float start_x,
                                                        float gap,
                                                        float max_x,
                                                        std::size_t first_index) {
  std::vector<StripSlotLayout> slots;
  if (widths.empty()) {
    return slots;
  }

  const std::size_t clamped_first =
      std::min(first_index, widths.empty() ? 0 : widths.size() - 1);
  float x = start_x;
  for (std::size_t i = clamped_first; i < widths.size(); ++i) {
    const float width = widths[i];
    if (x + width > max_x) {
      break;
    }
    slots.push_back(StripSlotLayout{
        .index = i,
        .x = x,
        .width = width,
    });
    x += width + gap;
  }

  return slots;
}

std::size_t EnsureVisibleStripIndex(const std::vector<float>& widths,
                                    float start_x,
                                    float gap,
                                    float max_x,
                                    std::size_t current_first_index,
                                    std::size_t active_index) {
  if (widths.empty()) {
    return 0;
  }

  const std::size_t clamped_active = std::min(active_index, widths.size() - 1);
  const std::size_t clamped_first = std::min(current_first_index, widths.size() - 1);
  const auto visible = ComputeVisibleStripLayouts(widths, start_x, gap, max_x, clamped_first);
  if (std::any_of(visible.begin(), visible.end(), [&](const StripSlotLayout& slot) {
        return slot.index == clamped_active;
      })) {
    return clamped_first;
  }

  float used_width = widths[clamped_active];
  std::size_t first_visible = clamped_active;
  while (first_visible > 0) {
    const float candidate_width = used_width + gap + widths[first_visible - 1];
    if (start_x + candidate_width > max_x) {
      break;
    }
    used_width = candidate_width;
    --first_visible;
  }

  return first_visible;
}

std::vector<ChromeTabRenderItem> BuildChromeTabRenderItems(
    std::span<const StripSlotLayout> slots,
    float tab_y,
    float tab_height,
    std::span<const std::size_t> model_indices,
    std::size_t active_index,
    std::span<const std::string> display_titles,
    std::span<const std::string> tooltip_labels,
    float close_button_size,
    float close_button_right_inset) {
  std::vector<ChromeTabRenderItem> items;
  items.reserve(slots.size());

  for (const StripSlotLayout& slot : slots) {
    const std::size_t model_index =
        slot.index < model_indices.size() ? model_indices[slot.index] : slot.index;
    const SDL_FRect rect = MakeRect(slot.x, tab_y, slot.width, tab_height);
    ChromeTabRenderItem item;
    item.index = model_index;
    item.rect = rect;
    item.close_rect = MakeRect(
        rect.x + rect.w - close_button_right_inset - close_button_size,
        rect.y + std::floor(std::max(0.0f, rect.h - close_button_size) * 0.5f), close_button_size,
        close_button_size);
    item.active = model_index == active_index;
    if (slot.index < display_titles.size()) {
      item.display_title = display_titles[slot.index];
    }
    if (slot.index < tooltip_labels.size()) {
      item.tooltip_label = tooltip_labels[slot.index];
    }
    items.push_back(std::move(item));
  }

  return items;
}

SDL_FRect ComputeMergeResultViewportRect(const SDL_FRect& editor_surface,
                                         float center_x,
                                         float rows_y,
                                         float gutter_width,
                                         float center_width,
                                         bool show_horizontal) {
  const float bottom_reserved = show_horizontal ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
  const float content_height = std::max(0.0f, editor_surface.h - bottom_reserved);
  return MakeRect(center_x, rows_y - 8.0f, gutter_width + center_width,
                  std::max(0.0f, editor_surface.y + content_height - (rows_y - 8.0f)));
}

std::optional<SDL_FRect> ComputeVisibleLineRangeRect(const SDL_FRect& viewport_rect,
                                                     const VisibleLineRangeLayout& layout,
                                                     std::size_t start_line,
                                                     std::size_t end_line) {
  if (viewport_rect.w <= 0.0f || viewport_rect.h <= 0.0f || end_line <= start_line ||
      layout.visible_rows == 0 || layout.line_height <= 0.0f) {
    return std::nullopt;
  }

  const std::size_t visible_end_line = layout.scroll_line + layout.visible_rows;
  const std::size_t rect_start = std::max(start_line, layout.scroll_line);
  const std::size_t rect_end = std::max(end_line, start_line + 1);
  if (rect_end <= layout.scroll_line || rect_start >= visible_end_line) {
    return std::nullopt;
  }

  const float y =
      layout.first_line_y + static_cast<float>(rect_start - layout.scroll_line) * layout.line_height;
  const float h =
      static_cast<float>(std::min(rect_end, visible_end_line) - rect_start) * layout.line_height;
  return MakeRect(viewport_rect.x, y - 1.0f, viewport_rect.w, h);
}

SDL_FRect ComputeMergeSourceActionButtonRect(float pane_x,
                                             float gutter_width,
                                             float rows_y,
                                             float line_height,
                                             int scroll_row,
                                             std::size_t end_line,
                                             float content_bottom,
                                             float button_width,
                                             float button_height) {
  float y = rows_y +
            static_cast<float>(static_cast<long long>(end_line) - scroll_row) * line_height + 2.0f;
  y = std::min(y, content_bottom - button_height - 4.0f);
  return MakeRect(pane_x + gutter_width, y, button_width, button_height);
}

std::array<SDL_FRect, 4> ComputeMergeResultActionButtonRects(
    float start_x,
    float rows_y,
    float content_bottom,
    const std::optional<SDL_FRect>& conflict_rect,
    const std::array<float, 4>& widths,
    float button_height,
    float button_gap) {
  float y = conflict_rect.has_value() ? conflict_rect->y + conflict_rect->h + 2.0f : rows_y + 2.0f;
  if (y + button_height > content_bottom - 4.0f && conflict_rect.has_value()) {
    y = std::max(rows_y + 2.0f, conflict_rect->y - button_height - 2.0f);
  }

  float x = start_x;
  std::array<SDL_FRect, 4> rects{};
  for (std::size_t i = 0; i < rects.size(); ++i) {
    rects[i] = MakeRect(x, y, widths[i], button_height);
    x += widths[i] + button_gap;
  }
  return rects;
}

std::optional<std::size_t> FindMergeTrackedConflictAtSourceLine(
    std::span<const MergeTrackedConflict> conflicts,
    std::size_t line,
    bool incoming) {
  for (std::size_t i = 0; i < conflicts.size(); ++i) {
    const auto& conflict = conflicts[i];
    const std::size_t start = incoming ? conflict.incoming_start_line : conflict.current_start_line;
    const std::size_t end = incoming ? conflict.incoming_end_line : conflict.current_end_line;
    if (line >= start && line < end) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> FindMergeTrackedConflictAtResultLine(
    std::span<const MergeTrackedConflict> conflicts,
    std::size_t line) {
  for (std::size_t i = 0; i < conflicts.size(); ++i) {
    const auto& conflict = conflicts[i];
    const std::size_t end = std::max(conflict.end_line, conflict.start_line + std::size_t{1});
    if (line >= conflict.start_line && line < end) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<MergeHoverState> ClassifyMergeHoverState(
    const MergeHoverSurfaceLayout& surface,
    const MergeHoverInteractionLayout& interaction,
    std::span<const MergeTrackedConflict> conflicts,
    float x,
    float y) {
  for (std::size_t i = 0; i < conflicts.size(); ++i) {
    const auto& conflict = conflicts[i];
    if (!conflict.valid) {
      continue;
    }

    if (Contains(ComputeMergeSourceActionButtonRect(surface.left_x, surface.gutter_width,
                                                    surface.rows_y, surface.line_height,
                                                    static_cast<int>(interaction.result.text.scroll_line),
                                                    conflict.incoming_end_line,
                                                    interaction.content_bottom,
                                                    interaction.incoming_accept_button_width,
                                                    interaction.button_height),
                 x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::IncomingAccept,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Incoming,
      };
    }
    if (Contains(ComputeMergeSourceActionButtonRect(surface.right_x, surface.gutter_width,
                                                    surface.rows_y, surface.line_height,
                                                    static_cast<int>(interaction.result.text.scroll_line),
                                                    conflict.current_end_line,
                                                    interaction.content_bottom,
                                                    interaction.current_accept_button_width,
                                                    interaction.button_height),
                 x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::CurrentAccept,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Current,
      };
    }

    const auto action_rects = ComputeMergeResultActionButtonRects(
        surface.center_x + surface.gutter_width, surface.rows_y, interaction.content_bottom,
        ComputeVisibleLineRangeRect(interaction.result.rect, interaction.result.lines,
                                    conflict.start_line,
                                    std::max(conflict.end_line, conflict.start_line + std::size_t{1})),
        interaction.result_action_widths, interaction.button_height, interaction.button_gap);
    if (Contains(action_rects[0], x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultAction,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Base,
      };
    }
    if (Contains(action_rects[1], x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultAction,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Incoming,
      };
    }
    if (Contains(action_rects[2], x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultAction,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Current,
      };
    }
    if (Contains(action_rects[3], x, y)) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultAction,
          .conflict_index = i,
          .preview_choice = compare::MergeChoice::Both,
      };
    }
  }

  if (x < surface.center_x) {
    if (const auto line = VisibleTextGridLineAtY(interaction.incoming, y); line.has_value()) {
      if (const auto conflict_index = FindMergeTrackedConflictAtSourceLine(conflicts, *line, true);
          conflict_index.has_value()) {
        return MergeHoverState{
            .kind = MergeHoverState::Kind::IncomingConflict,
            .conflict_index = *conflict_index,
            .preview_choice = compare::MergeChoice::Incoming,
        };
      }
    }
  } else if (x >= surface.right_x) {
    if (const auto line = VisibleTextGridLineAtY(interaction.current, y); line.has_value()) {
      if (const auto conflict_index = FindMergeTrackedConflictAtSourceLine(conflicts, *line, false);
          conflict_index.has_value()) {
        return MergeHoverState{
            .kind = MergeHoverState::Kind::CurrentConflict,
            .conflict_index = *conflict_index,
            .preview_choice = compare::MergeChoice::Current,
        };
      }
    }
  }

  if (Contains(interaction.result.rect, x, y)) {
    const std::size_t line = ClampTextGridLineAtY(interaction.result.text, y);
    if (const auto conflict_index = FindMergeTrackedConflictAtResultLine(conflicts, line);
        conflict_index.has_value()) {
      return MergeHoverState{
          .kind = MergeHoverState::Kind::ResultConflict,
          .conflict_index = *conflict_index,
          .preview_choice = conflicts[*conflict_index].last_choice,
      };
    }
  }

  return std::nullopt;
}

SDL_FRect ComputeOverlaySurfaceRect(const SDL_FRect& editor_area) {
  const float overlay_width =
      std::clamp(editor_area.w * 0.58f, kOverlayMinWidth, kOverlayMaxWidth);
  const float overlay_height =
      std::clamp(editor_area.h * 0.44f, kOverlayMinHeight, kOverlayMaxHeight);
  const float final_width = std::min(overlay_width, std::max(260.0f, editor_area.w - 56.0f));
  const float final_height = std::min(overlay_height, std::max(160.0f, editor_area.h - 48.0f));
  return MakeRect(editor_area.x + (editor_area.w - final_width) * 0.5f,
                  editor_area.y + (editor_area.h - final_height) * 0.22f, final_width,
                  final_height);
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
