#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include "editor/SyntaxHighlighter.h"
#include "project/FileOperationService.h"

namespace microide::workspace {

namespace {

constexpr float kMenuBarHeight = 25.0f;
constexpr float kWindowFrameHitThickness = 6.0f;
constexpr float kWindowControlButtonGap = 4.0f;
constexpr float kWindowControlButtonRightInset = 8.0f;
constexpr float kProjectTabStripHeight = 32.0f;
constexpr float kTabStripHeight = 34.0f;
constexpr float kHeaderHeight = 26.0f;
constexpr float kStatusBarHeight = 22.0f;
constexpr float kDivider = 1.0f;
constexpr float kResizeHandleThickness = 6.0f;
constexpr float kSidebarHeaderHeight = 30.0f;
constexpr float kBottomPanelHeaderHeight = 28.0f;
constexpr float kBottomPanelHeaderButtonSize = 18.0f;
constexpr float kSidebarInset = 10.0f;
constexpr float kSidebarRowHeight = 20.0f;
constexpr float kSearchSidebarResultsTop = 88.0f;
constexpr float kTreeIndentWidth = 14.0f;
constexpr float kTreeChevronSlotWidth = 12.0f;
constexpr float kOverlayMinWidth = 520.0f;
constexpr float kOverlayMaxWidth = 840.0f;
constexpr float kOverlayMinHeight = 220.0f;
constexpr float kOverlayMaxHeight = 360.0f;
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
constexpr float kScrollbarThickness = 10.0f;
constexpr float kScrollbarInset = 2.0f;
constexpr float kScrollbarMinThumbLength = 24.0f;
constexpr float kMinSidebarWidth = 160.0f;
constexpr float kMaxSidebarWidth = 520.0f;
constexpr float kMinEditorAreaWidth = 280.0f;
constexpr float kMinBottomPanelHeight = 96.0f;
constexpr float kMinEditorAreaHeight = 120.0f;
constexpr float kEditorSplitDividerThickness = 6.0f;
constexpr float kMinSplitPaneExtent = 180.0f;
constexpr float kBottomPanelCommandReserveHeight = 56.0f;
constexpr float kBottomPanelCommandPromptHeight = 18.0f;
constexpr float kBottomPanelCommandInset = 10.0f;
constexpr float kBottomPanelCommandTopPadding = 8.0f;
constexpr float kBottomPanelCommandBottomPadding = 8.0f;
constexpr float kTabCloseButtonSize = 14.0f;
constexpr float kTabCloseButtonRightInset = 6.0f;
constexpr float kMenuPopupSeparatorHeight = 8.0f;
constexpr float kMenuPopupItemHeight = 22.0f;
constexpr Uint64 kCaretBlinkIntervalMs = 530;

struct WorkspaceLayout {
  SDL_FRect full;
  SDL_FRect menu_bar;
  SDL_FRect project_tab_strip;
  SDL_FRect tab_strip;
  SDL_FRect status_bar;
  SDL_FRect bottom_panel;
  SDL_FRect content;
  SDL_FRect sidebar;
  SDL_FRect editor_area;
  SDL_FRect breadcrumb;
  SDL_FRect editor_surface;
};

struct ScrollbarGeometry {
  SDL_FRect track{};
  SDL_FRect thumb{};
  float total_units = 0.0f;
  float visible_units = 0.0f;
  float scroll_units = 0.0f;
  bool vertical = true;
};

struct ParsedCommandToken {
  std::string text;
  std::size_t start = 0;
};

struct ParsedCommandLine {
  std::vector<ParsedCommandToken> tokens;
  bool trailing_space = false;
  bool dangling_escape = false;
  char open_quote = '\0';
};

struct CommandCompletionCandidate {
  std::string value;
  bool append_space = true;
};

struct PersistedEditorViewState {
  std::size_t leaf_id = 0;
  std::filesystem::path path;
  std::size_t cursor_line = 0;
  std::size_t cursor_column = 0;
  std::size_t scroll_line = 0;
  std::size_t horizontal_scroll = 0;
};

struct PersistedSplitNodeState {
  std::vector<std::size_t> path;
  std::string orientation;
  float size_fraction = 1.0f;
  std::size_t leaf_id = 0;
};

struct PersistedEditorTabState {
  std::string kind = "editor";
  std::size_t active_leaf_id = 0;
  std::vector<PersistedEditorViewState> views;
  std::vector<PersistedSplitNodeState> split_nodes;
  std::filesystem::path compare_path;
  std::string compare_commit_hash;
  std::string compare_commit_short_hash;
  std::size_t compare_selected_row = 0;
};

constexpr std::array<std::string_view, 2> kSidebarToolNames = {
    "search",
    "tree",
};

constexpr std::array<std::string_view, 3> kFocusTargetNames = {
    "editor",
    "panel",
    "sidebar",
};

constexpr std::array<std::string_view, 2> kToggleValues = {
    "off",
    "on",
};

constexpr std::array<std::string_view, 3> kUiScaleCommands = {
    "down",
    "reset",
    "up",
};

constexpr std::array<float, 10> kUiScalePresets = {
    0.75f,
    1.0f,
    1.25f,
    1.5f,
    1.75f,
    2.0f,
    2.25f,
    2.5f,
    2.75f,
    3.0f,
};

constexpr float kMinUiScale = kUiScalePresets.front();
constexpr float kMaxUiScale = kUiScalePresets.back();

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

std::string EncodeSessionNodePath(const std::vector<std::size_t>& path) {
  if (path.empty()) {
    return ".";
  }

  std::string encoded;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (!encoded.empty()) {
      encoded.push_back('/');
    }
    encoded += std::to_string(path[i]);
  }
  return encoded;
}

std::optional<std::vector<std::size_t>> DecodeSessionNodePath(std::string_view text) {
  if (text == ".") {
    return std::vector<std::size_t>{};
  }
  if (text.empty()) {
    return std::nullopt;
  }

  std::vector<std::size_t> path;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t slash = text.find('/', start);
    const std::string_view part = text.substr(start, slash == std::string_view::npos
                                                         ? std::string_view::npos
                                                         : slash - start);
    if (part.empty()) {
      return std::nullopt;
    }
    try {
      path.push_back(static_cast<std::size_t>(std::stoull(std::string(part))));
    } catch (...) {
      return std::nullopt;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return path;
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

SDL_Color BlendColor(SDL_Color base, SDL_Color tint, float amount) {
  const float clamped_amount = std::clamp(amount, 0.0f, 1.0f);
  const auto blend = [&](Uint8 base_component, Uint8 tint_component) {
    return static_cast<Uint8>(std::lround(static_cast<float>(base_component) * (1.0f - clamped_amount) +
                                          static_cast<float>(tint_component) * clamped_amount));
  };
  return SDL_Color{
      blend(base.r, tint.r),
      blend(base.g, tint.g),
      blend(base.b, tint.b),
      0xff,
  };
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

template <std::size_t N>
std::vector<CommandCompletionCandidate> CompleteFromList(
    std::string_view prefix,
    const std::array<std::string_view, N>& suggestions,
    bool append_space = true) {
  std::vector<CommandCompletionCandidate> matches;
  for (std::string_view suggestion : suggestions) {
    if (!StartsWith(suggestion, prefix)) {
      continue;
    }
    matches.push_back(CommandCompletionCandidate{std::string(suggestion), append_space});
  }
  return matches;
}

std::vector<CommandCompletionCandidate> CompleteFromValues(std::string_view prefix,
                                                           const std::vector<std::string>& values,
                                                           bool append_space = true) {
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

SDL_FRect MakeRect(float x, float y, float w, float h) {
  return SDL_FRect{x, y, w, h};
}

SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full) {
  const float width = std::min(kDirtyPromptWidth, full.w - 32.0f);
  const float height = std::min(kDirtyPromptHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog) {
  const float total_width =
      kDirtyPromptButtonWidth * 3.0f + kDirtyPromptButtonGap * 2.0f;
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
  const float total_width =
      kPromptSurfaceButtonWidth * 2.0f + kPromptSurfaceButtonGap;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kPromptSurfaceButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
      MakeRect(start_x + kPromptSurfaceButtonWidth + kPromptSurfaceButtonGap, y,
               kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceInputRect(const SDL_FRect& dialog) {
  return MakeRect(dialog.x + 16.0f, dialog.y + 98.0f, dialog.w - 32.0f, kPromptSurfaceInputHeight);
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
  layout.status_bar =
      MakeRect(0.0f, window_height - kStatusBarHeight, window_width, kStatusBarHeight);
  layout.bottom_panel =
      MakeRect(0.0f, window_height - kStatusBarHeight - resolved_bottom_panel_height,
               window_width, resolved_bottom_panel_height);
  layout.content =
      MakeRect(0.0f, kMenuBarHeight + kProjectTabStripHeight + kTabStripHeight, window_width,
               window_height - kMenuBarHeight - kProjectTabStripHeight - kTabStripHeight -
                   resolved_bottom_panel_height - kStatusBarHeight);
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
      std::max(0.0f, window_height - kMenuBarHeight - kProjectTabStripHeight - kTabStripHeight -
                         kStatusBarHeight);
  const float min_height = std::min(kMinBottomPanelHeight, content_height);
  const float max_height = std::max(min_height, content_height - kMinEditorAreaHeight);
  return std::clamp(height, min_height, max_height);
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
                                                              bool reserve_horizontal = false) {
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
                                                                bool reserve_vertical = false) {
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

void DrawScrollbar(SDL_Renderer* renderer,
                   const render::Theme& theme,
                   const SDL_FRect& track,
                   const SDL_FRect& thumb,
                   bool active = false) {
  if (renderer == nullptr || track.w <= 0.0f || track.h <= 0.0f || thumb.w <= 0.0f ||
      thumb.h <= 0.0f) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, theme.surface_raised.r, theme.surface_raised.g,
                         theme.surface_raised.b, theme.surface_raised.a);
  SDL_RenderFillRect(renderer, &track);

  const SDL_Color thumb_color = active ? theme.accent : theme.text_disabled;
  SDL_SetRenderDrawColor(renderer, thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a);
  SDL_RenderFillRect(renderer, &thumb);
}

std::size_t MaxVisualColumns(const editor::TextViewport& viewport) {
  return viewport.max_visual_columns();
}

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
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

std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return 0;
  }

  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead <= 0x7F) {
    return 1;
  }

  auto continuation = [&](std::size_t count) {
    if (offset + count >= text.size()) {
      return false;
    }
    for (std::size_t i = 1; i <= count; ++i) {
      const unsigned char byte = static_cast<unsigned char>(text[offset + i]);
      if ((byte & 0xC0) != 0x80) {
        return false;
      }
    }
    return true;
  };

  if (lead >= 0xC2 && lead <= 0xDF && continuation(1)) {
    return 2;
  }
  if (lead == 0xE0 && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0xA0 && second <= 0xBF) {
      return 3;
    }
  }
  if (((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) && continuation(2)) {
    return 3;
  }
  if (lead == 0xED && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x9F) {
      return 3;
    }
  }
  if (lead == 0xF0 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x90 && second <= 0xBF) {
      return 4;
    }
  }
  if (lead >= 0xF1 && lead <= 0xF3 && continuation(3)) {
    return 4;
  }
  if (lead == 0xF4 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x8F) {
      return 4;
    }
  }

  return 1;
}

std::size_t Utf8ByteOffsetForCodepointCount(std::string_view text, std::size_t codepoint_count) {
  std::size_t offset = 0;
  for (std::size_t count = 0; count < codepoint_count && offset < text.size(); ++count) {
    offset += Utf8SequenceLength(text, offset);
  }
  return offset;
}

std::size_t Utf8CodepointCount(std::string_view text) {
  std::size_t count = 0;
  for (std::size_t offset = 0; offset < text.size();) {
    offset += Utf8SequenceLength(text, offset);
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

std::string EditorTabLabel(const editor::TextViewport& viewport) {
  if (!viewport.path().empty()) {
    return viewport.path().filename().string();
  }
  return viewport.is_placeholder() ? "welcome" : "untitled";
}

bool ParseLineColumnSpec(std::string_view location,
                         long long* line,
                         std::size_t* column,
                         bool allow_zero_line) {
  if (line == nullptr || column == nullptr || location.empty()) {
    return false;
  }

  const std::size_t colon = location.find(':');
  const std::string line_text(location.substr(0, colon));
  const std::string column_text =
      colon == std::string_view::npos ? std::string{} : std::string(location.substr(colon + 1));

  long long parsed_line = 0;
  std::size_t parsed_column = 0;
  try {
    parsed_line = std::stoll(line_text);
    if (!column_text.empty()) {
      parsed_column = static_cast<std::size_t>(std::stoull(column_text));
    }
  } catch (...) {
    return false;
  }

  if (!allow_zero_line && parsed_line == 0) {
    return false;
  }

  *line = parsed_line;
  *column = parsed_column;
  return true;
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

SDL_Color CompareTokenColor(const render::Theme& theme,
                            editor::SyntaxTokenKind kind,
                            SDL_Color plain_color,
                            bool selected) {
  switch (kind) {
    case editor::SyntaxTokenKind::Keyword:
      return theme.syntax_keyword;
    case editor::SyntaxTokenKind::Type:
      return theme.syntax_type;
    case editor::SyntaxTokenKind::String:
      return theme.syntax_string;
    case editor::SyntaxTokenKind::Comment:
      return theme.syntax_comment;
    case editor::SyntaxTokenKind::Number:
      return theme.syntax_number;
    case editor::SyntaxTokenKind::Constant:
      return theme.syntax_constant;
    case editor::SyntaxTokenKind::Preprocessor:
      return theme.syntax_preprocessor;
    case editor::SyntaxTokenKind::Operator:
      return theme.syntax_operator;
    case editor::SyntaxTokenKind::Plain:
    default:
      return selected ? theme.text_primary : plain_color;
  }
}

char GitMarker(project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Modified:
      return 'M';
    case project::GitFileStatus::Added:
      return 'A';
    case project::GitFileStatus::Deleted:
      return 'D';
    case project::GitFileStatus::Untracked:
      return 'U';
    case project::GitFileStatus::Clean:
    default:
      return ' ';
  }
}

SDL_Color GitMarkerColor(const render::Theme& theme, project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Modified:
      return theme.diff_modified;
    case project::GitFileStatus::Added:
      return theme.diff_added;
    case project::GitFileStatus::Deleted:
      return theme.diff_deleted;
    case project::GitFileStatus::Untracked:
      return theme.accent;
    case project::GitFileStatus::Clean:
    default:
      return theme.text_disabled;
  }
}

void DrawChevron(SDL_Renderer* renderer, float x, float center_y, bool expanded, SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  if (expanded) {
    SDL_RenderLine(renderer, x, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    SDL_RenderLine(renderer, x + 8.0f, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    return;
  }

  SDL_RenderLine(renderer, x + 2.0f, center_y - 4.0f, x + 6.0f, center_y);
  SDL_RenderLine(renderer, x + 2.0f, center_y + 4.0f, x + 6.0f, center_y);
}

SDL_HitTestResult ResizeHitTestResult(bool left, bool right, bool top, bool bottom) {
  if (top && left) {
    return SDL_HITTEST_RESIZE_TOPLEFT;
  }
  if (top && right) {
    return SDL_HITTEST_RESIZE_TOPRIGHT;
  }
  if (bottom && left) {
    return SDL_HITTEST_RESIZE_BOTTOMLEFT;
  }
  if (bottom && right) {
    return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
  }
  if (top) {
    return SDL_HITTEST_RESIZE_TOP;
  }
  if (bottom) {
    return SDL_HITTEST_RESIZE_BOTTOM;
  }
  if (left) {
    return SDL_HITTEST_RESIZE_LEFT;
  }
  if (right) {
    return SDL_HITTEST_RESIZE_RIGHT;
  }
  return SDL_HITTEST_NORMAL;
}

void DrawWindowControlGlyph(SDL_Renderer* renderer,
                            const SDL_FRect& rect,
                            microide::workspace::WorkspaceShell::WindowControlButtonId id,
                            SDL_Color color,
                            bool maximized) {
  if (renderer == nullptr) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float left = rect.x + 4.0f;
  const float right = rect.x + rect.w - 4.0f;
  const float top = rect.y + 4.0f;
  const float bottom = rect.y + rect.h - 4.0f;
  const float center_y = rect.y + rect.h * 0.5f;

  switch (id) {
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Minimize:
      SDL_RenderLine(renderer, left, center_y + 2.0f, right, center_y + 2.0f);
      return;
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Maximize:
      if (maximized) {
        const SDL_FRect back = SDL_FRect{left + 1.5f, top + 3.0f, rect.w - 9.0f, rect.h - 9.0f};
        const SDL_FRect front = SDL_FRect{left - 1.0f, top + 1.0f, rect.w - 9.0f, rect.h - 9.0f};
        SDL_RenderRect(renderer, &back);
        SDL_RenderRect(renderer, &front);
      } else {
        const SDL_FRect outline = SDL_FRect{left, top + 1.0f, rect.w - 8.0f, rect.h - 8.0f};
        SDL_RenderRect(renderer, &outline);
      }
      return;
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Close:
      SDL_RenderLine(renderer, left, top, right, bottom);
      SDL_RenderLine(renderer, right, top, left, bottom);
      return;
  }
}

}  // namespace

std::span<const WorkspaceShell::ActionSpec> WorkspaceShell::ActionSpecs() {
  static const auto kSpecs = std::to_array<ActionSpec>({
      ActionSpec{ActionId::Colorscheme, "colorscheme", "colorscheme [name|list]", "Colorscheme",
                 ""},
      ActionSpec{ActionId::Compare, "compare", "compare [path] [commit-prefix]",
                 "Compare Against...", ""},
      ActionSpec{ActionId::CompareHead, "", "", "Compare Against HEAD", ""},
      ActionSpec{ActionId::CopyAbsolutePath, "", "", "Copy Absolute Path", ""},
      ActionSpec{ActionId::CopyRelativePath, "", "", "Copy Relative Path", ""},
      ActionSpec{ActionId::CreateDirectory, "", "", "New Folder...", ""},
      ActionSpec{ActionId::CreateFile, "", "", "New File...", ""},
      ActionSpec{ActionId::DeletePath, "", "", "Delete...", ""},
      ActionSpec{ActionId::Files, "files", "files [root]", "Find File", "F6"},
      ActionSpec{ActionId::Find, "find", "find <query>", "Find File By Query", ""},
      ActionSpec{ActionId::Focus, "focus", "focus <editor|sidebar|panel>", "Focus", ""},
      ActionSpec{ActionId::Goto, "goto", "goto <line[:col]>", "Go to Line", ""},
      ActionSpec{ActionId::Grep, "grep", "grep <query>", "Show Project Search", ""},
      ActionSpec{ActionId::Help, "help", "help", "Help", ""},
      ActionSpec{ActionId::Hsplit, "hsplit", "hsplit [path]", "Split Down", ""},
      ActionSpec{ActionId::IndentWidth, "indent-width", "indent-width [n]", "Indent Width",
                 ""},
      ActionSpec{ActionId::Jump, "jump", "jump <line[:col]>", "Jump Relative", ""},
      ActionSpec{ActionId::Open, "open", "open <path>", "Open File", ""},
      ActionSpec{ActionId::OpenSelectedTreeItem, "", "", "Open", ""},
      ActionSpec{ActionId::OpenSelectedTreeItemInNewTab, "", "", "Open in New Tab", ""},
      ActionSpec{ActionId::PanelHide, "panel-hide", "panel-hide", "Hide Bottom Panel", ""},
      ActionSpec{ActionId::PanelShow, "panel-show", "panel-show", "Show Bottom Panel", ""},
      ActionSpec{ActionId::ProjectClose, "project-close", "project-close", "Close Project", ""},
      ActionSpec{ActionId::ProjectNext, "project-next", "project-next", "Next Project", ""},
      ActionSpec{ActionId::ProjectOpen, "project-open", "project-open <path>", "Open Project",
                 ""},
      ActionSpec{ActionId::ProjectPrev, "project-prev", "project-prev", "Previous Project", ""},
      ActionSpec{ActionId::Quit, "quit", "quit", "Quit", ""},
      ActionSpec{ActionId::RenamePath, "", "", "Rename...", ""},
      ActionSpec{ActionId::Reopen, "reopen", "reopen", "Reopen", ""},
      ActionSpec{ActionId::Rg, "rg", "rg <query>", "Find in Project", "Ctrl+Shift+F"},
      ActionSpec{ActionId::Save, "save", "save", "Save", "Ctrl+S"},
      ActionSpec{ActionId::Search, "search", "search <query>", "Find in Buffer", "Ctrl+F"},
      ActionSpec{ActionId::SidebarClose, "sidebar-close", "sidebar-close", "Close Sidebar", ""},
      ActionSpec{ActionId::SidebarHide, "sidebar-hide", "sidebar-hide", "Hide Sidebar", ""},
      ActionSpec{ActionId::SidebarShow, "sidebar-show", "sidebar-show [tool]", "Show Sidebar",
                 ""},
      ActionSpec{ActionId::SidebarToggle, "sidebar-toggle", "sidebar-toggle [tool]",
                 "Toggle Sidebar", "F8", true},
      ActionSpec{ActionId::SidebarWidth, "sidebar-width", "sidebar-width <n>", "Sidebar Width",
                 ""},
      ActionSpec{ActionId::SoftTabs, "soft-tabs", "soft-tabs [on|off]", "Soft Tabs", ""},
      ActionSpec{ActionId::SplitFirst, "split-first", "split-first", "First Split", ""},
      ActionSpec{ActionId::SplitLast, "split-last", "split-last", "Last Split", ""},
      ActionSpec{ActionId::SplitNext, "split-next", "split-next", "Next Split", ""},
      ActionSpec{ActionId::SplitPrev, "split-prev", "split-prev", "Previous Split", ""},
      ActionSpec{ActionId::Tab, "tab", "tab [path]", "New Tab", ""},
      ActionSpec{ActionId::TabSize, "tab-size", "tab-size [n]", "Tab Size", ""},
      ActionSpec{ActionId::TabMove, "tabmove", "tabmove <n>", "Move Tab", ""},
      ActionSpec{ActionId::TabSwitch, "tabswitch", "tabswitch <tab>", "Switch Tab", ""},
      ActionSpec{ActionId::Term, "term", "term [command]", "New Terminal", ""},
      ActionSpec{ActionId::Tree, "tree", "tree [root]", "Show Tree", ""},
      ActionSpec{ActionId::TreeRefresh, "tree-refresh", "tree-refresh", "Refresh Tree", ""},
      ActionSpec{ActionId::UiScale, "ui-scale", "ui-scale [n|up|down|reset]", "UI Scale", ""},
      ActionSpec{ActionId::Unsplit, "unsplit", "unsplit", "Close Split", ""},
      ActionSpec{ActionId::Vsplit, "vsplit", "vsplit [path]", "Split Right", ""},
      ActionSpec{ActionId::CloseActiveTab, "", "", "Close Tab", "Ctrl+W"},
      ActionSpec{ActionId::CopySelection, "", "", "Copy", "Ctrl+C"},
      ActionSpec{ActionId::CutSelection, "", "", "Cut", "Ctrl+X"},
      ActionSpec{ActionId::OpenCommandPrompt, "", "", "Command Prompt", "Ctrl+E"},
      ActionSpec{ActionId::PasteClipboard, "", "", "Paste", "Ctrl+V"},
      ActionSpec{ActionId::Redo, "", "", "Redo", "Ctrl+Y / Ctrl+Shift+Z"},
      ActionSpec{ActionId::ReplaceInBuffer, "", "", "Replace in Buffer", "Ctrl+H"},
      ActionSpec{ActionId::SelectAll, "", "", "Select All", "Ctrl+A"},
      ActionSpec{ActionId::ToggleBottomPanel, "", "", "Toggle Bottom Panel", "F9", true},
      ActionSpec{ActionId::Undo, "", "", "Undo", "Ctrl+Z"},
  });
  return kSpecs;
}

const WorkspaceShell::ActionSpec* WorkspaceShell::FindActionSpec(ActionId id) {
  const auto specs = ActionSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [id](const ActionSpec& spec) { return spec.id == id; });
  return it == specs.end() ? nullptr : &(*it);
}

const WorkspaceShell::ActionSpec* WorkspaceShell::FindActionByCommand(std::string_view command_name) {
  const auto specs = ActionSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(), [command_name](const ActionSpec& spec) {
    return !spec.command_name.empty() && spec.command_name == command_name;
  });
  return it == specs.end() ? nullptr : &(*it);
}

const std::vector<std::string>& WorkspaceShell::CommandNames() {
  static const std::vector<std::string> kNames = [] {
    std::vector<std::string> names;
    for (const ActionSpec& spec : ActionSpecs()) {
      if (!spec.command_name.empty()) {
        names.emplace_back(spec.command_name);
      }
    }
    return names;
  }();
  return kNames;
}

const std::string& WorkspaceShell::CommandHelpSummary() {
  static const std::string kSummary = [] {
    std::string summary;
    for (const ActionSpec& spec : ActionSpecs()) {
      if (spec.command_usage.empty()) {
        continue;
      }
      if (!summary.empty()) {
        summary += ", ";
      }
      summary += spec.command_usage;
    }
    return summary;
  }();
  return kSummary;
}

bool WorkspaceShell::IsActionEnabled(ActionId id) const {
  switch (id) {
    case ActionId::Colorscheme:
    case ActionId::Files:
    case ActionId::Help:
    case ActionId::OpenCommandPrompt:
    case ActionId::ProjectOpen:
    case ActionId::Quit:
    case ActionId::SidebarClose:
    case ActionId::SidebarHide:
    case ActionId::SidebarShow:
    case ActionId::SidebarToggle:
    case ActionId::ToggleBottomPanel:
      return true;
    case ActionId::CloseActiveTab:
      return !open_tabs_.empty();
    case ActionId::CompareHead:
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab:
      return !project_root_.empty() &&
             (tree_context_menu_.open ? tree_context_menu_.target : SelectedTreeTargetKind()) ==
                 TreeContextTargetKind::File;
    case ActionId::CreateDirectory:
    case ActionId::CreateFile: {
      if (project_root_.empty()) {
        return false;
      }
      const TreeContextTargetKind target =
          tree_context_menu_.open ? tree_context_menu_.target : SelectedTreeTargetKind();
      return target == TreeContextTargetKind::Directory || target == TreeContextTargetKind::Root ||
             target == TreeContextTargetKind::Background;
    }
    case ActionId::DeletePath:
    case ActionId::RenamePath: {
      if (project_root_.empty()) {
        return false;
      }
      const TreeContextTargetKind target =
          tree_context_menu_.open ? tree_context_menu_.target : SelectedTreeTargetKind();
      return target == TreeContextTargetKind::File || target == TreeContextTargetKind::Directory;
    }
    case ActionId::Compare:
    case ActionId::Find:
    case ActionId::Grep:
    case ActionId::Open:
    case ActionId::ProjectClose:
    case ActionId::Rg:
    case ActionId::Tab:
    case ActionId::Term:
    case ActionId::Tree:
    case ActionId::TreeRefresh:
      return !project_root_.empty();
    case ActionId::CopySelection:
    case ActionId::CutSelection:
    case ActionId::Goto:
    case ActionId::Hsplit:
    case ActionId::Jump:
    case ActionId::PasteClipboard:
    case ActionId::Redo:
    case ActionId::ReplaceInBuffer:
    case ActionId::Reopen:
    case ActionId::Save:
    case ActionId::Search:
    case ActionId::SelectAll:
    case ActionId::SplitFirst:
    case ActionId::SplitLast:
    case ActionId::SplitNext:
    case ActionId::SplitPrev:
    case ActionId::Undo:
    case ActionId::Unsplit:
    case ActionId::Vsplit:
      return ActiveTabIsEditor();
    case ActionId::Focus:
      return true;
    case ActionId::IndentWidth:
    case ActionId::PanelHide:
    case ActionId::PanelShow:
      return true;
    case ActionId::CopyAbsolutePath:
      return !ResolveTreeActionPath(ActionSource::ContextMenu).empty();
    case ActionId::CopyRelativePath: {
      const std::filesystem::path path = ResolveTreeActionPath(ActionSource::ContextMenu);
      return !project_root_.empty() && !path.empty() && path != project_root_;
    }
    case ActionId::SidebarWidth:
    case ActionId::SoftTabs:
    case ActionId::TabSize:
    case ActionId::UiScale:
      return true;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev:
      return !project_root_.empty() && projects_.size() > 1;
    case ActionId::TabMove:
    case ActionId::TabSwitch:
      return !project_root_.empty() && !open_tabs_.empty();
  }

  return true;
}

std::span<const WorkspaceShell::MenuSpec> WorkspaceShell::MenuSpecs() {
  const auto item = [](ActionId action, std::string_view label = {},
                       std::string_view accelerator = {},
                       std::array<std::string_view, 2> args = {}, std::size_t arg_count = 0,
                       bool checkable = false) {
    return MenuItemSpec{action, label, accelerator, args, arg_count, false, checkable};
  };
  const auto separator = [] { return MenuItemSpec{ActionId::Help, {}, {}, {}, 0, true, false}; };

  static const auto kFileItems = std::to_array<MenuItemSpec>({
      item(ActionId::ProjectOpen, "New Project Tab..."),
      separator(),
      item(ActionId::Tab),
      item(ActionId::Save),
      item(ActionId::CloseActiveTab),
      item(ActionId::Reopen),
      separator(),
      item(ActionId::ProjectClose),
      separator(),
      item(ActionId::Quit),
  });
  static const auto kEditItems = std::to_array<MenuItemSpec>({
      item(ActionId::Undo),
      item(ActionId::Redo),
      separator(),
      item(ActionId::CutSelection),
      item(ActionId::CopySelection),
      item(ActionId::PasteClipboard),
      item(ActionId::SelectAll),
  });
  static const auto kViewItems = std::to_array<MenuItemSpec>({
      item(ActionId::SidebarToggle, {}, {}, {}, 0, true),
      item(ActionId::SidebarShow, "Show Tree", {}, std::array<std::string_view, 2>{"tree", {}}, 1,
           true),
      item(ActionId::SidebarShow, "Show Search", {},
           std::array<std::string_view, 2>{"search", {}}, 1, true),
      separator(),
      item(ActionId::ToggleBottomPanel, {}, {}, {}, 0, true),
      separator(),
      item(ActionId::UiScale, "Zoom In", "Ctrl+=", std::array<std::string_view, 2>{"up", {}}, 1),
      item(ActionId::UiScale, "Zoom Out", "Ctrl+-",
           std::array<std::string_view, 2>{"down", {}}, 1),
      item(ActionId::UiScale, "Reset Zoom", "Ctrl+0",
           std::array<std::string_view, 2>{"reset", {}}, 1),
      separator(),
      item(ActionId::Focus, "Focus Editor", {},
           std::array<std::string_view, 2>{"editor", {}}, 1),
      item(ActionId::Focus, "Focus Sidebar", {},
           std::array<std::string_view, 2>{"sidebar", {}}, 1),
      item(ActionId::Focus, "Focus Panel", {},
           std::array<std::string_view, 2>{"panel", {}}, 1),
  });
  static const auto kSearchItems = std::to_array<MenuItemSpec>({
      item(ActionId::Search),
      item(ActionId::ReplaceInBuffer),
      item(ActionId::Files),
      item(ActionId::Rg),
  });
  static const auto kProjectItems = std::to_array<MenuItemSpec>({
      item(ActionId::Compare, "Compare Current File..."),
      item(ActionId::TreeRefresh),
      separator(),
      item(ActionId::ProjectNext),
      item(ActionId::ProjectPrev),
  });
  static const auto kTerminalItems = std::to_array<MenuItemSpec>({
      item(ActionId::Term),
  });
  static const auto kHelpItems = std::to_array<MenuItemSpec>({
      item(ActionId::Help, "Command Summary"),
  });
  static const auto kMenus = std::to_array<MenuSpec>({
      MenuSpec{MenuId::File, "File", kFileItems},
      MenuSpec{MenuId::Edit, "Edit", kEditItems},
      MenuSpec{MenuId::View, "View", kViewItems},
      MenuSpec{MenuId::Search, "Search", kSearchItems},
      MenuSpec{MenuId::Project, "Project", kProjectItems},
      MenuSpec{MenuId::Terminal, "Terminal", kTerminalItems},
      MenuSpec{MenuId::Help, "Help", kHelpItems},
  });
  return kMenus;
}

const WorkspaceShell::MenuSpec* WorkspaceShell::FindMenuSpec(MenuId id) {
  const auto menus = MenuSpecs();
  const auto it = std::find_if(menus.begin(), menus.end(),
                               [id](const MenuSpec& spec) { return spec.id == id; });
  return it == menus.end() ? nullptr : &(*it);
}

std::span<const WorkspaceShell::MenuItemSpec> WorkspaceShell::TreeContextMenuItems(
    TreeContextTargetKind target) {
  const auto item = [](ActionId action, std::string_view label = {}) {
    return MenuItemSpec{action, label, {}, {}, 0, false, false};
  };
  const auto separator = [] { return MenuItemSpec{ActionId::Help, {}, {}, {}, 0, true, false}; };

  static const auto kFileItems = std::to_array<MenuItemSpec>({
      item(ActionId::OpenSelectedTreeItem),
      item(ActionId::OpenSelectedTreeItemInNewTab),
      separator(),
      item(ActionId::CompareHead),
      item(ActionId::Compare),
      separator(),
      item(ActionId::RenamePath),
      item(ActionId::DeletePath),
      separator(),
      item(ActionId::CopyRelativePath),
      item(ActionId::CopyAbsolutePath),
  });
  static const auto kDirectoryItems = std::to_array<MenuItemSpec>({
      item(ActionId::CreateFile),
      item(ActionId::CreateDirectory),
      separator(),
      item(ActionId::RenamePath),
      item(ActionId::DeletePath),
      separator(),
      item(ActionId::TreeRefresh, "Refresh"),
      separator(),
      item(ActionId::CopyRelativePath),
      item(ActionId::CopyAbsolutePath),
  });
  static const auto kRootItems = std::to_array<MenuItemSpec>({
      item(ActionId::CreateFile),
      item(ActionId::CreateDirectory),
      separator(),
      item(ActionId::TreeRefresh, "Refresh"),
      item(ActionId::ProjectClose),
      separator(),
      item(ActionId::CopyAbsolutePath),
  });
  static const auto kBackgroundItems = std::to_array<MenuItemSpec>({
      item(ActionId::CreateFile),
      item(ActionId::CreateDirectory),
      separator(),
      item(ActionId::TreeRefresh, "Refresh"),
  });

  switch (target) {
    case TreeContextTargetKind::File:
      return kFileItems;
    case TreeContextTargetKind::Directory:
      return kDirectoryItems;
    case TreeContextTargetKind::Root:
      return kRootItems;
    case TreeContextTargetKind::Background:
      return kBackgroundItems;
    case TreeContextTargetKind::None:
    default:
      return {};
  }
}

std::vector<WorkspaceShell::VisibleMenuBarItem> WorkspaceShell::ComputeVisibleMenuBarItems(
    const SDL_FRect& menu_bar) const {
  std::vector<VisibleMenuBarItem> items;
  float x = menu_bar.x + 8.0f;
  const float y = menu_bar.y + 3.0f;
  const float height = std::max(18.0f, menu_bar.h - 6.0f);
  const auto window_buttons = ComputeVisibleWindowControlButtons(menu_bar);
  const float max_x = window_buttons.empty()
                          ? menu_bar.x + menu_bar.w - 8.0f
                          : window_buttons.front().rect.x - 8.0f;
  for (const MenuSpec& spec : MenuSpecs()) {
    const float width =
        std::clamp(text_renderer_.MeasureWidth(spec.label) + 22.0f, 52.0f, 108.0f);
    if (x + width > max_x) {
      break;
    }
    items.push_back(VisibleMenuBarItem{
        .id = spec.id,
        .rect = MakeRect(x, y, width, height),
        .active = menu_bar_open_ && spec.id == active_menu_id_,
    });
    x += width + 4.0f;
  }
  return items;
}

std::vector<WorkspaceShell::VisibleWindowControlButton>
WorkspaceShell::ComputeVisibleWindowControlButtons(const SDL_FRect& menu_bar) const {
  std::vector<VisibleWindowControlButton> buttons;
  if (!custom_window_chrome_enabled_) {
    return buttons;
  }

  const float button_size = std::max(18.0f, menu_bar.h - 6.0f);
  const float total_width =
      button_size * 3.0f + kWindowControlButtonGap * 2.0f;
  const float start_x =
      menu_bar.x + std::max(0.0f, menu_bar.w - total_width - kWindowControlButtonRightInset);
  const float y = menu_bar.y + (menu_bar.h - button_size) * 0.5f;
  static constexpr auto kButtonIds = std::to_array<WindowControlButtonId>({
      WindowControlButtonId::Minimize,
      WindowControlButtonId::Maximize,
      WindowControlButtonId::Close,
  });

  float x = start_x;
  for (WindowControlButtonId id : kButtonIds) {
    const SDL_FRect rect = MakeRect(x, y, button_size, button_size);
    buttons.push_back(VisibleWindowControlButton{
        .id = id,
        .rect = rect,
        .hovered =
            last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_),
    });
    x += button_size + kWindowControlButtonGap;
  }
  return buttons;
}

std::optional<SDL_FRect> WorkspaceShell::ComputePopupMenuRect(
    const SDL_FRect& anchor_rect,
    std::span<const MenuItemSpec> items,
    const SDL_FRect& bounds) const {
  if (items.empty()) {
    return std::nullopt;
  }

  float width = 172.0f;
  float height = 12.0f;
  for (const MenuItemSpec& item : items) {
    if (item.separator) {
      height += kMenuPopupSeparatorHeight;
      continue;
    }
    width = std::max(width, text_renderer_.MeasureWidth(MenuItemLabel(item)) +
                                text_renderer_.MeasureWidth(MenuItemAccelerator(item)) + 68.0f);
    height += kMenuPopupItemHeight;
  }

  const float max_width = std::max(172.0f, bounds.w - 8.0f);
  width = std::clamp(width, 172.0f, max_width);
  float x = std::clamp(anchor_rect.x, bounds.x + 4.0f,
                       bounds.x + std::max(4.0f, bounds.w - width - 4.0f));
  float y = anchor_rect.y + std::max(0.0f, anchor_rect.h) - 1.0f;
  if (y + height > bounds.y + bounds.h - 4.0f) {
    y = anchor_rect.y - height + 1.0f;
  }
  y = std::clamp(y, bounds.y + 4.0f, bounds.y + std::max(4.0f, bounds.h - height - 4.0f));
  return MakeRect(x, y, width, height);
}

std::optional<SDL_FRect> WorkspaceShell::ComputePopupMenuRect(const SDL_FRect& menu_bar,
                                                              MenuId id) const {
  const MenuSpec* menu = FindMenuSpec(id);
  if (menu == nullptr) {
    return std::nullopt;
  }

  const auto menu_bar_items = ComputeVisibleMenuBarItems(menu_bar);
  const auto bar_it =
      std::find_if(menu_bar_items.begin(), menu_bar_items.end(),
                   [id](const VisibleMenuBarItem& item) { return item.id == id; });
  if (bar_it == menu_bar_items.end()) {
    return std::nullopt;
  }

  const SDL_FRect bounds =
      MakeRect(0.0f, 0.0f,
               last_window_width_ > 0 ? static_cast<float>(last_window_width_) : menu_bar.w,
               last_window_height_ > 0
                   ? static_cast<float>(last_window_height_)
                   : std::max(menu_bar.y + menu_bar.h + 320.0f, menu_bar.h));
  return ComputePopupMenuRect(bar_it->rect, menu->items, bounds);
}

std::vector<WorkspaceShell::VisiblePopupMenuItem> WorkspaceShell::ComputeVisiblePopupMenuItems(
    std::span<const MenuItemSpec> items,
    int active_item_index,
    const SDL_FRect& popup_rect) const {
  std::vector<VisiblePopupMenuItem> visible_items;
  float y = popup_rect.y + 6.0f;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const MenuItemSpec& item = items[i];
    const float height = item.separator ? kMenuPopupSeparatorHeight : kMenuPopupItemHeight;
    const SDL_FRect rect =
        MakeRect(popup_rect.x + 6.0f, y, std::max(0.0f, popup_rect.w - 12.0f), height);
    visible_items.push_back(VisiblePopupMenuItem{
        .index = i,
        .rect = rect,
        .enabled = IsMenuItemEnabled(item),
        .checked = IsMenuItemChecked(item),
        .hovered = static_cast<int>(i) == active_item_index,
        .separator = item.separator,
    });
    y += height;
  }
  return visible_items;
}

std::vector<WorkspaceShell::VisiblePopupMenuItem> WorkspaceShell::ComputeVisiblePopupMenuItems(
    MenuId id,
    const SDL_FRect& popup_rect) const {
  const MenuSpec* menu = FindMenuSpec(id);
  return menu == nullptr ? std::vector<VisiblePopupMenuItem>{}
                         : ComputeVisiblePopupMenuItems(menu->items, active_menu_item_index_,
                                                        popup_rect);
}

std::string WorkspaceShell::MenuItemLabel(const MenuItemSpec& item) const {
  if (!item.label.empty()) {
    return std::string(item.label);
  }
  if (const ActionSpec* action = FindActionSpec(item.action); action != nullptr &&
                                                        !action->label.empty()) {
    return std::string(action->label);
  }
  if (const ActionSpec* action = FindActionSpec(item.action); action != nullptr &&
                                                        !action->command_name.empty()) {
    return std::string(action->command_name);
  }
  return {};
}

std::string WorkspaceShell::MenuItemAccelerator(const MenuItemSpec& item) const {
  if (!item.accelerator.empty()) {
    return std::string(item.accelerator);
  }
  if (const ActionSpec* action = FindActionSpec(item.action); action != nullptr &&
                                                        !action->accelerator.empty()) {
    return std::string(action->accelerator);
  }
  return {};
}

bool WorkspaceShell::IsMenuItemEnabled(const MenuItemSpec& item) const {
  if (item.separator) {
    return false;
  }

  if (item.action == ActionId::Files) {
    return !project_root_.empty();
  }
  if (item.action == ActionId::Focus && item.arg_count > 0) {
    if (item.args[0] == "sidebar") {
      return sidebar_visible_;
    }
    if (item.args[0] == "panel") {
      return bottom_panel_visible_ && BottomPanelShowsTerminal();
    }
    return true;
  }
  if ((item.action == ActionId::SidebarShow || item.action == ActionId::SidebarToggle) &&
      item.arg_count > 0 && (item.args[0] == "tree" || item.args[0] == "search")) {
    return !project_root_.empty();
  }

  return IsActionEnabled(item.action);
}

bool WorkspaceShell::IsMenuItemChecked(const MenuItemSpec& item) const {
  if (!item.checkable) {
    return false;
  }

  if (item.action == ActionId::SidebarToggle) {
    return sidebar_visible_;
  }
  if (item.action == ActionId::SidebarShow && item.arg_count > 0) {
    if (item.args[0] == "tree") {
      return sidebar_visible_ && sidebar_mode_ == SidebarMode::Tree;
    }
    if (item.args[0] == "search") {
      return sidebar_visible_ && sidebar_mode_ == SidebarMode::Search && !sidebar_temporary_;
    }
  }
  if (item.action == ActionId::ToggleBottomPanel) {
    return bottom_panel_visible_;
  }

  return false;
}

int WorkspaceShell::FirstEnabledMenuItemIndex(MenuId id) const {
  const MenuSpec* menu = FindMenuSpec(id);
  if (menu == nullptr) {
    return -1;
  }

  for (std::size_t i = 0; i < menu->items.size(); ++i) {
    if (IsMenuItemEnabled(menu->items[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int WorkspaceShell::NextEnabledMenuItemIndex(MenuId id, int current_index, int delta) const {
  const MenuSpec* menu = FindMenuSpec(id);
  if (menu == nullptr || menu->items.empty() || delta == 0) {
    return -1;
  }

  const int item_count = static_cast<int>(menu->items.size());
  int index = current_index < 0 ? (delta > 0 ? -1 : 0) : current_index;
  for (int step = 0; step < item_count; ++step) {
    index = (index + delta + item_count) % item_count;
    if (IsMenuItemEnabled(menu->items[static_cast<std::size_t>(index)])) {
      return index;
    }
  }
  return current_index;
}

void WorkspaceShell::OpenMenuBarMenu(MenuId id) {
  if (id == MenuId::None) {
    CloseMenuBar();
    return;
  }
  CloseTreeContextMenu();
  menu_bar_open_ = true;
  active_menu_id_ = id;
  active_menu_item_index_ = FirstEnabledMenuItemIndex(id);
}

void WorkspaceShell::CloseMenuBar() {
  menu_bar_open_ = false;
  active_menu_id_ = MenuId::None;
  active_menu_item_index_ = -1;
}

bool WorkspaceShell::ExecuteMenuItem(MenuId menu_id, std::size_t item_index) {
  const MenuSpec* menu = FindMenuSpec(menu_id);
  if (menu == nullptr || item_index >= menu->items.size()) {
    return false;
  }

  const MenuItemSpec& item = menu->items[item_index];
  if (!IsMenuItemEnabled(item)) {
    return true;
  }

  std::vector<std::string> args;
  args.reserve(item.arg_count);
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    args.emplace_back(item.args[i]);
  }
  CloseMenuBar();
  return ExecuteAction(item.action, args, ActionSource::Menu);
}

bool WorkspaceShell::SwitchMenuBarMenu(int delta) {
  const auto menus = MenuSpecs();
  if (menus.empty() || active_menu_id_ == MenuId::None || delta == 0) {
    return false;
  }

  auto current_it = std::find_if(menus.begin(), menus.end(),
                                 [this](const MenuSpec& spec) { return spec.id == active_menu_id_; });
  if (current_it == menus.end()) {
    return false;
  }

  const int current_index = static_cast<int>(std::distance(menus.begin(), current_it));
  const int next_index =
      (current_index + delta + static_cast<int>(menus.size())) % static_cast<int>(menus.size());
  OpenMenuBarMenu(menus[static_cast<std::size_t>(next_index)].id);
  return true;
}

bool WorkspaceShell::MoveActiveMenuItem(int delta) {
  if (!menu_bar_open_ || active_menu_id_ == MenuId::None) {
    return false;
  }
  active_menu_item_index_ = NextEnabledMenuItemIndex(active_menu_id_, active_menu_item_index_, delta);
  return active_menu_item_index_ >= 0;
}

const project::TreeEntry* WorkspaceShell::SelectedTreeEntry() const {
  if (sidebar_mode_ != SidebarMode::Tree) {
    return nullptr;
  }
  const auto& entries = directory_tree_.entries();
  if (directory_tree_.selected_index() >= entries.size()) {
    return nullptr;
  }
  return &entries[directory_tree_.selected_index()];
}

std::filesystem::path WorkspaceShell::SelectedTreePath() const {
  const project::TreeEntry* entry = SelectedTreeEntry();
  return entry == nullptr ? std::filesystem::path{} : entry->path.lexically_normal();
}

WorkspaceShell::TreeContextTargetKind WorkspaceShell::SelectedTreeTargetKind() const {
  const project::TreeEntry* entry = SelectedTreeEntry();
  if (entry == nullptr) {
    return TreeContextTargetKind::None;
  }
  if (!entry->is_directory) {
    return TreeContextTargetKind::File;
  }
  return entry->path == project_root_ ? TreeContextTargetKind::Root
                                      : TreeContextTargetKind::Directory;
}

std::filesystem::path WorkspaceShell::ResolveTreeActionPath(ActionSource source) const {
  if (source == ActionSource::ContextMenu && tree_context_menu_.open &&
      !tree_context_menu_.path.empty()) {
    return tree_context_menu_.path.lexically_normal();
  }
  return SelectedTreePath();
}

std::optional<SDL_FRect> WorkspaceShell::ComputeTreeContextMenuRect() const {
  if (!tree_context_menu_.open || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return std::nullopt;
  }
  return ComputePopupMenuRect(tree_context_menu_.anchor_rect,
                              TreeContextMenuItems(tree_context_menu_.target),
                              MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                       static_cast<float>(last_window_height_)));
}

void WorkspaceShell::OpenTreeContextMenu(TreeContextTargetKind target,
                                         const std::filesystem::path& path,
                                         const SDL_FRect& anchor_rect) {
  CloseMenuBar();
  tree_context_menu_.open = true;
  tree_context_menu_.target = target;
  tree_context_menu_.path = path.lexically_normal();
  tree_context_menu_.anchor_rect = anchor_rect;
  tree_context_menu_.active_item_index = FirstEnabledTreeContextMenuItemIndex();
}

void WorkspaceShell::CloseTreeContextMenu() {
  tree_context_menu_ = TreeContextMenuState{};
}

bool WorkspaceShell::ExecuteTreeContextMenuItem(std::size_t item_index) {
  const auto items = TreeContextMenuItems(tree_context_menu_.target);
  if (item_index >= items.size()) {
    return false;
  }

  const MenuItemSpec& item = items[item_index];
  if (!IsMenuItemEnabled(item)) {
    return true;
  }

  std::vector<std::string> args;
  args.reserve(item.arg_count);
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    args.emplace_back(item.args[i]);
  }
  const bool handled = ExecuteAction(item.action, args, ActionSource::ContextMenu);
  CloseTreeContextMenu();
  return handled;
}

int WorkspaceShell::FirstEnabledTreeContextMenuItemIndex() const {
  const auto items = TreeContextMenuItems(tree_context_menu_.target);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (IsMenuItemEnabled(items[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int WorkspaceShell::NextEnabledTreeContextMenuItemIndex(int current_index, int delta) const {
  const auto items = TreeContextMenuItems(tree_context_menu_.target);
  if (items.empty() || delta == 0) {
    return -1;
  }

  const int item_count = static_cast<int>(items.size());
  int index = current_index < 0 ? (delta > 0 ? -1 : 0) : current_index;
  for (int step = 0; step < item_count; ++step) {
    index = (index + delta + item_count) % item_count;
    if (IsMenuItemEnabled(items[static_cast<std::size_t>(index)])) {
      return index;
    }
  }
  return current_index;
}

bool WorkspaceShell::ConfigureProjectState(ProjectWorkspaceState& state,
                                          const std::filesystem::path& project_root) {
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(project_root, error);
  if (error || absolute_root.empty()) {
    return false;
  }

  state.root = absolute_root.lexically_normal();
  if (!state.directory_tree.SetRoot(state.root)) {
    return false;
  }
  if (!state.file_index.SetRoot(state.root)) {
    return false;
  }
  state.file_finder.SetIndex(&state.file_index);
  return true;
}

void WorkspaceShell::RebindProjectState(ProjectWorkspaceState& state) {
  state.file_finder.SetIndex(&state.file_index);
}

std::filesystem::path WorkspaceShell::ResolveProjectRootInput(
    const std::filesystem::path& project_root) const {
  if (project_root.empty()) {
    return {};
  }

  std::error_code error;
  if (project_root.is_absolute()) {
    const auto absolute_root = std::filesystem::absolute(project_root, error);
    return error ? std::filesystem::path{} : absolute_root.lexically_normal();
  }

  const std::filesystem::path base_root =
      project_root_.empty() ? std::filesystem::current_path(error) : project_root_;
  if (error || base_root.empty()) {
    return {};
  }

  const auto absolute_root = std::filesystem::absolute(base_root / project_root, error);
  return error ? std::filesystem::path{} : absolute_root.lexically_normal();
}

void WorkspaceShell::SetWelcomePlaceholder() {
  text_viewport_.SetPlaceholderText(
      "microide\n\n"
      "Welcome.\n"
      "Press Ctrl+E and run project-open <path>.\n");
  ApplyEditorPreferences(text_viewport_);
}

void WorkspaceShell::ResetProjectScopedState(bool show_welcome) {
  StopProjectSearch();
  CloseTreeContextMenu();

  project_root_.clear();
  directory_tree_ = project::DirectoryTree{};
  file_index_ = project::FileIndex{};
  file_finder_ = project::FileFinder{};
  text_viewport_ = editor::TextViewport{};
  open_tabs_.clear();
  active_tab_index_ = 0;
  tab_scroll_index_ = 0;
  sidebar_visible_ = !show_welcome;
  sidebar_mode_ = SidebarMode::Tree;
  sidebar_prev_mode_ = SidebarMode::None;
  sidebar_temporary_ = false;
  bottom_panel_visible_ = false;
  bottom_panel_mode_ = BottomPanelMode::Logs;
  overlay_visible_ = false;
  overlay_mode_ = OverlayMode::FileFinder;
  buffer_search_field_ = BufferSearchField::Search;
  command_mode_ = false;
  focus_ = show_welcome ? FocusTarget::Editor : FocusTarget::Sidebar;
  sidebar_width_ = 288.0f;
  bottom_panel_height_ = 184.0f;
  sidebar_scroll_row_ = 0;
  bottom_panel_scroll_row_ = 0;
  overlay_scroll_row_ = 0;
  bottom_panel_follow_tail_ = true;
  terminal_tabs_.clear();
  active_terminal_tab_index_ = 0;
  buffer_search_query_.clear();
  buffer_replace_text_.clear();
  buffer_search_matches_.clear();
  buffer_search_selected_index_ = 0;
  project_search_query_.clear();
  project_search_edit_buffer_.clear();
  project_search_editing_ = false;
  project_search_edit_field_ = ProjectSearchEditField::Query;
  project_replace_text_.clear();
  project_search_results_.clear();
  project_search_selected_index_ = 0;
  project_search_running_ = false;
  project_search_error_.clear();
  project_search_run_id_ = 0;
  compare_picker_path_.clear();
  compare_picker_query_.clear();
  compare_picker_commits_.clear();
  compare_picker_matches_.clear();
  compare_picker_selected_index_ = 0;
  command_input_.clear();
  command_history_.clear();
  command_history_index_.reset();
  command_history_pending_input_.clear();
  command_completion_feedback_.clear();
  log_messages_.clear();
  active_colorscheme_name_ = "default";
  editor_preferences_ = EditorPreferences{};
  file_finder_.SetIndex(&file_index_);
  ApplyColorscheme(active_colorscheme_name_, false, false);
  ApplyEditorPreferences(text_viewport_);
  if (show_welcome) {
    SetWelcomePlaceholder();
  }
}

bool WorkspaceShell::InitializeCurrentProject(const std::filesystem::path& project_root,
                                             bool restore_persistence,
                                             bool log_feedback) {
  ResetProjectScopedState(false);
  if (!SetProjectRoot(project_root)) {
    return false;
  }

  ApplyColorscheme(active_colorscheme_name_, false, false);
  ApplyEditorPreferences(text_viewport_);
  if (log_feedback) {
    LogMessage("Project loaded: " + project_root_.lexically_normal().string());
  }
  if (restore_persistence && RestoreConfigState() && log_feedback) {
    LogMessage("Restored editor preferences and colorscheme");
  }

  if (restore_persistence && RestoreSessionState()) {
    ApplyEditorPreferencesToAllTabs();
    if (log_feedback) {
      LogMessage("Restored workspace session");
    }
    return true;
  }

  const std::vector<std::filesystem::path> preferred_files = {
      project_root_ / "docs" / "implementation-guide.md",
      project_root_ / "README.md",
  };

  for (const auto& candidate : preferred_files) {
    editor::TextViewport startup_view;
    if (std::filesystem::exists(candidate) && startup_view.OpenFile(candidate)) {
      ApplyEditorPreferences(startup_view);
      text_viewport_ = startup_view;
      directory_tree_.SelectPath(candidate);
      open_tabs_.push_back(TabEntry{
          .kind = TabEntry::Kind::Editor,
          .path = candidate,
          .title = candidate.filename().string(),
          .editor_state = MakeEditorTabState(startup_view),
          .compare = std::nullopt,
      });
      active_tab_index_ = 0;
      if (log_feedback) {
        LogMessage("Opened startup file: " + candidate.filename().string());
      }
      return true;
    }
  }

  text_viewport_.SetPlaceholderText(
      "microide\n\n"
      "Project loaded.\n"
      "Use the sidebar to open files.\n");
  ApplyEditorPreferences(text_viewport_);
  return true;
}

void WorkspaceShell::StoreCurrentProjectState(ProjectWorkspaceState& state) {
  SyncActiveEditorTab();
  StopProjectSearch();
  CloseTreeContextMenu();

  state.root = project_root_;
  state.directory_tree = std::move(directory_tree_);
  state.file_index = std::move(file_index_);
  state.file_finder = std::move(file_finder_);
  state.text_viewport = std::move(text_viewport_);
  state.open_tabs = std::move(open_tabs_);
  state.active_tab_index = active_tab_index_;
  state.tab_scroll_index = tab_scroll_index_;
  state.sidebar_visible = sidebar_visible_;
  state.sidebar_mode = sidebar_mode_;
  state.sidebar_prev_mode = sidebar_prev_mode_;
  state.sidebar_temporary = sidebar_temporary_;
  state.bottom_panel_visible = bottom_panel_visible_;
  state.bottom_panel_mode = bottom_panel_mode_;
  state.overlay_visible = overlay_visible_;
  state.overlay_mode = overlay_mode_;
  state.buffer_search_field = buffer_search_field_;
  state.command_mode = command_mode_;
  state.focus = focus_;
  state.sidebar_width = sidebar_width_;
  state.bottom_panel_height = bottom_panel_height_;
  state.sidebar_scroll_row = sidebar_scroll_row_;
  state.bottom_panel_scroll_row = bottom_panel_scroll_row_;
  state.overlay_scroll_row = overlay_scroll_row_;
  state.bottom_panel_follow_tail = bottom_panel_follow_tail_;
  state.terminal_tabs = std::move(terminal_tabs_);
  state.active_terminal_tab_index = active_terminal_tab_index_;
  state.buffer_search_query = std::move(buffer_search_query_);
  state.buffer_replace_text = std::move(buffer_replace_text_);
  state.buffer_search_matches = std::move(buffer_search_matches_);
  state.buffer_search_selected_index = buffer_search_selected_index_;
  state.project_search_query = std::move(project_search_query_);
  state.project_search_edit_buffer = std::move(project_search_edit_buffer_);
  state.project_search_editing = project_search_editing_;
  state.project_search_edit_field = project_search_edit_field_;
  state.project_replace_text = std::move(project_replace_text_);
  state.project_search_results = std::move(project_search_results_);
  state.project_search_selected_index = project_search_selected_index_;
  state.project_search_running = false;
  state.project_search_error = std::move(project_search_error_);
  state.compare_picker_path = std::move(compare_picker_path_);
  state.compare_picker_query = std::move(compare_picker_query_);
  state.compare_picker_commits = std::move(compare_picker_commits_);
  state.compare_picker_matches = std::move(compare_picker_matches_);
  state.compare_picker_selected_index = compare_picker_selected_index_;
  state.command_input = std::move(command_input_);
  state.command_history = std::move(command_history_);
  state.command_history_index = command_history_index_;
  state.command_history_pending_input = std::move(command_history_pending_input_);
  state.command_completion_feedback = std::move(command_completion_feedback_);
  state.log_messages = std::move(log_messages_);
  state.active_colorscheme_name = active_colorscheme_name_;
  state.editor_preferences = editor_preferences_;
  RebindProjectState(state);
}

void WorkspaceShell::LoadProjectState(ProjectWorkspaceState& state) {
  StopProjectSearch();
  CloseTreeContextMenu();

  project_root_ = state.root;
  directory_tree_ = std::move(state.directory_tree);
  file_index_ = std::move(state.file_index);
  file_finder_ = std::move(state.file_finder);
  text_viewport_ = std::move(state.text_viewport);
  open_tabs_ = std::move(state.open_tabs);
  active_tab_index_ = state.active_tab_index;
  tab_scroll_index_ = state.tab_scroll_index;
  sidebar_visible_ = state.sidebar_visible;
  sidebar_mode_ = state.sidebar_mode;
  sidebar_prev_mode_ = state.sidebar_prev_mode;
  sidebar_temporary_ = state.sidebar_temporary;
  bottom_panel_visible_ = state.bottom_panel_visible;
  bottom_panel_mode_ = state.bottom_panel_mode;
  overlay_visible_ = state.overlay_visible;
  overlay_mode_ = state.overlay_mode;
  buffer_search_field_ = state.buffer_search_field;
  command_mode_ = state.command_mode;
  focus_ = state.focus;
  sidebar_width_ = state.sidebar_width;
  bottom_panel_height_ = state.bottom_panel_height;
  sidebar_scroll_row_ = state.sidebar_scroll_row;
  bottom_panel_scroll_row_ = state.bottom_panel_scroll_row;
  overlay_scroll_row_ = state.overlay_scroll_row;
  bottom_panel_follow_tail_ = state.bottom_panel_follow_tail;
  terminal_tabs_ = std::move(state.terminal_tabs);
  active_terminal_tab_index_ = state.active_terminal_tab_index;
  buffer_search_query_ = std::move(state.buffer_search_query);
  buffer_replace_text_ = std::move(state.buffer_replace_text);
  buffer_search_matches_ = std::move(state.buffer_search_matches);
  buffer_search_selected_index_ = state.buffer_search_selected_index;
  project_search_query_ = std::move(state.project_search_query);
  project_search_edit_buffer_ = std::move(state.project_search_edit_buffer);
  project_search_editing_ = state.project_search_editing;
  project_search_edit_field_ = state.project_search_edit_field;
  project_replace_text_ = std::move(state.project_replace_text);
  project_search_results_ = std::move(state.project_search_results);
  project_search_selected_index_ = state.project_search_selected_index;
  project_search_running_ = false;
  project_search_error_ = std::move(state.project_search_error);
  project_search_run_id_ = 0;
  compare_picker_path_ = std::move(state.compare_picker_path);
  compare_picker_query_ = std::move(state.compare_picker_query);
  compare_picker_commits_ = std::move(state.compare_picker_commits);
  compare_picker_matches_ = std::move(state.compare_picker_matches);
  compare_picker_selected_index_ = state.compare_picker_selected_index;
  command_input_ = std::move(state.command_input);
  command_history_ = std::move(state.command_history);
  command_history_index_ = state.command_history_index;
  command_history_pending_input_ = std::move(state.command_history_pending_input);
  command_completion_feedback_ = std::move(state.command_completion_feedback);
  log_messages_ = std::move(state.log_messages);
  active_colorscheme_name_ = state.active_colorscheme_name;
  editor_preferences_ = state.editor_preferences;

  state.root = project_root_;
  file_finder_.SetIndex(&file_index_);
  ApplyColorscheme(active_colorscheme_name_, false, false);
  ApplyEditorPreferencesToAllTabs();
  if (text_viewport_.is_placeholder()) {
    ApplyEditorPreferences(text_viewport_);
  }
}

bool WorkspaceShell::Initialize(const std::filesystem::path& project_root) {
  caret_blink_epoch_ms_ = SDL_GetTicks();
  cursor_kind_ = CursorKind::Default;
  last_mouse_position_valid_ = false;
  quit_requested_ = false;
  dirty_prompt_visible_ = false;
  projects_.clear();
  active_project_index_ = 0;
  project_tab_scroll_index_ = 0;

  project_search_event_type_ = SDL_RegisterEvents(1);
  if (project_search_event_type_ != static_cast<Uint32>(-1)) {
    project_search_service_.SetWakeEventType(project_search_event_type_);
  } else {
    project_search_event_type_ = 0;
  }

  terminal_event_type_ = SDL_RegisterEvents(1);
  if (terminal_event_type_ == static_cast<Uint32>(-1)) {
    terminal_event_type_ = 0;
  }

  RestoreUserConfig();
  RefreshAvailableColorschemeNames();
  ResetProjectScopedState(true);

  if (RestoreWorkspaceSession()) {
    return true;
  }

  if (project_root.empty()) {
    return true;
  }

  return OpenProjectTab(project_root, true, true);
}

void WorkspaceShell::Shutdown() {
  SaveUserConfig();

  if (!projects_.empty() && !project_root_.empty() && active_project_index_ < projects_.size()) {
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[active_project_index_]);
  }

  for (std::size_t i = 0; i < projects_.size(); ++i) {
    if (projects_[i] == nullptr) {
      continue;
    }
    LoadProjectState(*projects_[i]);
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[i]);
  }
  SaveWorkspaceSession();

  StopProjectSearch();
  terminal_tabs_.clear();

  if (SDL_Cursor* default_cursor = SDL_GetDefaultCursor(); default_cursor != nullptr) {
    SDL_SetCursor(default_cursor);
  }

  if (text_cursor_ != nullptr) {
    SDL_DestroyCursor(text_cursor_);
    text_cursor_ = nullptr;
  }
  if (pointer_cursor_ != nullptr) {
    SDL_DestroyCursor(pointer_cursor_);
    pointer_cursor_ = nullptr;
  }
  if (ew_resize_cursor_ != nullptr) {
    SDL_DestroyCursor(ew_resize_cursor_);
    ew_resize_cursor_ = nullptr;
  }
  if (ns_resize_cursor_ != nullptr) {
    SDL_DestroyCursor(ns_resize_cursor_);
    ns_resize_cursor_ = nullptr;
  }

  cursor_kind_ = CursorKind::Default;
  last_mouse_position_valid_ = false;
}

void WorkspaceShell::RequestQuit() {
  if (dirty_prompt_visible_) {
    focus_ = FocusTarget::Overlay;
    return;
  }

  std::size_t dirty_count = DirtyEditorTabIndices().size();
  for (std::size_t i = 0; i < projects_.size(); ++i) {
    if (!project_root_.empty() && i == active_project_index_) {
      continue;
    }
    dirty_count += DirtyEditorTabIndicesForProject(i).size();
  }

  if (dirty_count == 0) {
    quit_requested_ = true;
    return;
  }

  ShowDirtyPromptForQuit();
}

bool WorkspaceShell::ConsumeQuitRequested() {
  const bool requested = quit_requested_;
  quit_requested_ = false;
  return requested;
}

void WorkspaceShell::SetWindowChromeState(int width,
                                          int height,
                                          bool maximized,
                                          bool custom_enabled) {
  if (width > 0) {
    last_window_width_ = width;
  }
  if (height > 0) {
    last_window_height_ = height;
  }
  window_maximized_ = maximized;
  custom_window_chrome_enabled_ = custom_enabled;
}

SDL_HitTestResult WorkspaceShell::WindowHitTest(float x, float y) const {
  if (!custom_window_chrome_enabled_ || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return SDL_HITTEST_NORMAL;
  }

  const float window_width = static_cast<float>(last_window_width_);
  const float window_height = static_cast<float>(last_window_height_);
  if (x < 0.0f || y < 0.0f || x >= window_width || y >= window_height) {
    return SDL_HITTEST_NORMAL;
  }

  if (!window_maximized_) {
    const bool left = x < kWindowFrameHitThickness;
    const bool right = x >= window_width - kWindowFrameHitThickness;
    const bool top = y < kWindowFrameHitThickness;
    const bool bottom = y >= window_height - kWindowFrameHitThickness;
    if (left || right || top || bottom) {
      return ResizeHitTestResult(left, right, top, bottom);
    }
  }

  if (menu_bar_open_ || tree_context_menu_.open) {
    return SDL_HITTEST_NORMAL;
  }

  const WorkspaceLayout layout =
      ComputeLayout(window_width, window_height, sidebar_visible_, bottom_panel_visible_,
                    sidebar_width_, bottom_panel_height_);
  if (!Contains(layout.menu_bar, x, y)) {
    return SDL_HITTEST_NORMAL;
  }

  for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
    if (Contains(item.rect, x, y)) {
      return SDL_HITTEST_NORMAL;
    }
  }
  for (const VisibleWindowControlButton& button :
       ComputeVisibleWindowControlButtons(layout.menu_bar)) {
    if (Contains(button.rect, x, y)) {
      return SDL_HITTEST_NORMAL;
    }
  }

  return SDL_HITTEST_DRAGGABLE;
}

WorkspaceShell::WindowAction WorkspaceShell::ConsumeWindowAction() {
  const WindowAction action = pending_window_action_;
  pending_window_action_ = WindowAction::None;
  return action;
}

std::optional<Uint32> WorkspaceShell::NextAnimationDelayMs() const {
  if (!ShouldBlinkCaret()) {
    return std::nullopt;
  }

  const Uint64 elapsed = SDL_GetTicks() - caret_blink_epoch_ms_;
  const Uint64 remaining = kCaretBlinkIntervalMs - (elapsed % kCaretBlinkIntervalMs);
  return static_cast<Uint32>(std::max<Uint64>(1, remaining));
}

void WorkspaceShell::ResetCaretBlink() {
  caret_blink_epoch_ms_ = SDL_GetTicks();
}

bool WorkspaceShell::ShouldBlinkCaret() const {
  return focus_ == FocusTarget::Editor && !command_mode_ && !dirty_prompt_visible_ &&
         !overlay_visible_ && !menu_bar_open_ && !tree_context_menu_.open &&
         ActiveTabIsEditor() && !text_viewport_.is_placeholder();
}

bool WorkspaceShell::CaretVisibleNow() const {
  if (!ShouldBlinkCaret()) {
    return false;
  }

  const Uint64 elapsed = SDL_GetTicks() - caret_blink_epoch_ms_;
  return ((elapsed / kCaretBlinkIntervalMs) % 2) == 0;
}

bool WorkspaceShell::SetProjectRoot(const std::filesystem::path& project_root) {
  const std::filesystem::path absolute_root = ResolveProjectRootInput(project_root);
  if (absolute_root.empty()) {
    return false;
  }

  StopProjectSearch();
  project_root_ = absolute_root.lexically_normal();
  if (!directory_tree_.SetRoot(project_root_)) {
    return false;
  }
  if (!file_index_.SetRoot(project_root_)) {
    return false;
  }
  file_finder_.SetIndex(&file_index_);
  sidebar_scroll_row_ = 0;

  if (sidebar_mode_ == SidebarMode::Search && !project_search_query_.empty()) {
    RefreshProjectSearch();
  }
  return true;
}

bool WorkspaceShell::OpenProjectTab(const std::filesystem::path& project_root,
                                    bool restore_persistence,
                                    bool log_feedback) {
  const std::filesystem::path normalized_root = ResolveProjectRootInput(project_root);
  if (normalized_root.empty()) {
    if (log_feedback) {
      LogMessage("Failed to open project: " + project_root.lexically_normal().string());
    }
    return false;
  }

  if (!project_root_.empty() && normalized_root == project_root_) {
    EnsureActiveProjectVisible();
    return true;
  }

  for (std::size_t i = 0; i < projects_.size(); ++i) {
    const std::filesystem::path open_root =
        (!project_root_.empty() && i == active_project_index_) ? project_root_
        : projects_[i] != nullptr                               ? projects_[i]->root
                                                               : std::filesystem::path{};
    if (open_root == normalized_root) {
      return SwitchProject(i, log_feedback);
    }
  }

  const bool had_active_project = !project_root_.empty() && active_project_index_ < projects_.size();
  const std::size_t previous_active_index = active_project_index_;
  if (had_active_project) {
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[active_project_index_]);
  }

  auto project_state = std::make_unique<ProjectWorkspaceState>();
  project_state->root = normalized_root;
  projects_.push_back(std::move(project_state));
  active_project_index_ = projects_.size() - 1;

  if (!InitializeCurrentProject(normalized_root, restore_persistence, log_feedback)) {
    projects_.pop_back();
    if (had_active_project && previous_active_index < projects_.size()) {
      active_project_index_ = previous_active_index;
      LoadProjectState(*projects_[active_project_index_]);
    } else {
      active_project_index_ = 0;
      ResetProjectScopedState(true);
    }
    if (log_feedback) {
      LogMessage("Failed to open project: " + normalized_root.string());
    }
    return false;
  }

  EnsureActiveProjectVisible();
  SaveWorkspaceSession();
  return true;
}

bool WorkspaceShell::SwitchProject(std::size_t index, bool log_feedback) {
  if (index >= projects_.size()) {
    return false;
  }
  CloseTreeContextMenu();
  if (!project_root_.empty() && index == active_project_index_) {
    EnsureActiveProjectVisible();
    return true;
  }

  if (!project_root_.empty() && active_project_index_ < projects_.size()) {
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[active_project_index_]);
  }

  active_project_index_ = index;
  LoadProjectState(*projects_[index]);
  EnsureActiveProjectVisible();
  SaveWorkspaceSession();
  if (log_feedback) {
    LogMessage("Project switched: " + ProjectLabel());
  }
  return true;
}

void WorkspaceShell::RequestCloseProject(std::size_t index) {
  if (index >= projects_.size()) {
    return;
  }
  if (!DirtyEditorTabIndicesForProject(index).empty()) {
    ShowDirtyPromptForProject(index);
    return;
  }
  CloseProject(index);
}

void WorkspaceShell::CloseProject(std::size_t index) {
  if (index >= projects_.size()) {
    return;
  }

  const bool closing_active = !project_root_.empty() && index == active_project_index_;
  const std::filesystem::path project_root =
      closing_active ? project_root_
                     : (projects_[index] != nullptr ? projects_[index]->root
                                                    : std::filesystem::path{});
  const std::string closed_label = ProjectLabelForRoot(project_root);

  if (closing_active) {
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[index]);
  }

  projects_.erase(projects_.begin() + static_cast<std::ptrdiff_t>(index));
  if (projects_.empty()) {
    active_project_index_ = 0;
    project_tab_scroll_index_ = 0;
    ResetProjectScopedState(true);
    SaveWorkspaceSession();
    LogMessage("Closed project: " + closed_label);
    return;
  }

  if (closing_active) {
    active_project_index_ = std::min(index, projects_.size() - 1);
    LoadProjectState(*projects_[active_project_index_]);
  } else if (active_project_index_ > index) {
    --active_project_index_;
  }

  EnsureActiveProjectVisible();
  SaveWorkspaceSession();
  LogMessage("Closed project: " + closed_label);
}

std::filesystem::path WorkspaceShell::ConfigStatePath() const {
  return project_root_.empty() ? std::filesystem::path{}
                               : project_root_ / ".microide-config";
}

std::filesystem::path WorkspaceShell::UserConfigPath() const {
  if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
      xdg_config_home != nullptr && *xdg_config_home != '\0') {
    return std::filesystem::path(xdg_config_home) / "microide" / "config";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".config" / "microide" / "config";
  }
  return {};
}

void WorkspaceShell::RefreshAvailableColorschemeNames() {
  available_colorscheme_names_ = render::ListAvailableThemeNames();
}

bool WorkspaceShell::ApplyColorscheme(std::string_view name, bool persist, bool log_feedback) {
  render::Theme loaded_theme;
  std::string resolved_name;
  std::string error;
  const std::string requested_name = name.empty() ? "default" : std::string(name);
  if (!render::LoadThemeByName(requested_name, loaded_theme, &resolved_name, &error)) {
    if (log_feedback) {
      LogMessage(error.empty() ? "Failed to load colorscheme" : error);
    }
    return false;
  }

  theme_ = loaded_theme;
  active_colorscheme_name_ = resolved_name.empty() ? requested_name : resolved_name;
  if (std::find(available_colorscheme_names_.begin(), available_colorscheme_names_.end(),
                active_colorscheme_name_) == available_colorscheme_names_.end()) {
    available_colorscheme_names_.push_back(active_colorscheme_name_);
    std::sort(available_colorscheme_names_.begin(), available_colorscheme_names_.end());
  }

  if (persist) {
    SaveConfigState();
  }
  if (log_feedback) {
    LogMessage("Colorscheme set to " + active_colorscheme_name_);
  }
  return true;
}

bool WorkspaceShell::ApplyUiScale(float scale, bool persist, bool log_feedback) {
  if (!std::isfinite(scale)) {
    return false;
  }

  ui_scale_ = std::clamp(scale, kMinUiScale, kMaxUiScale);
  if (persist) {
    SaveUserConfig();
  }
  if (log_feedback) {
    LogMessage("UI scale set to " + UiScaleLabel(ui_scale_));
  }
  return true;
}

bool WorkspaceShell::RestoreUserConfig() {
  const std::filesystem::path config_path = UserConfigPath();
  if (config_path.empty()) {
    return false;
  }

  std::ifstream file(config_path);
  if (!file) {
    return false;
  }

  bool version_ok = false;
  float restored_scale = ui_scale_;
  std::string line;
  while (std::getline(file, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.empty()) {
      continue;
    }

    const auto& tokens = parsed.tokens;
    const std::string& command = tokens.front().text;
    if (command == "version") {
      version_ok = tokens.size() == 2 && tokens[1].text == "1";
      continue;
    }
    if (!version_ok) {
      return false;
    }
    if (command == "ui-scale" && tokens.size() == 2) {
      if (const auto scale = ParseUiScaleValue(tokens[1].text); scale.has_value()) {
        restored_scale = *scale;
      }
    }
  }

  if (!version_ok) {
    return false;
  }

  return ApplyUiScale(restored_scale, false, false);
}

void WorkspaceShell::SaveUserConfig() const {
  const std::filesystem::path config_path = UserConfigPath();
  if (config_path.empty()) {
    return;
  }

  std::error_code error;
  std::filesystem::create_directories(config_path.parent_path(), error);

  std::ofstream file(config_path, std::ios::trunc);
  if (!file) {
    return;
  }

  file << "version 1\n";
  file << "ui-scale " << ui_scale_ << '\n';
}

bool WorkspaceShell::RestoreConfigState() {
  const std::filesystem::path config_path = ConfigStatePath();
  std::ifstream file(config_path);
  if (!file) {
    return false;
  }

  bool version_ok = false;
  EditorPreferences restored = editor_preferences_;
  std::string restored_colorscheme = active_colorscheme_name_;
  std::string line;
  while (std::getline(file, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.empty()) {
      continue;
    }

    const auto& tokens = parsed.tokens;
    const std::string& command = tokens.front().text;
    if (command == "version") {
      version_ok = tokens.size() == 2 && tokens[1].text == "1";
      continue;
    }
    if (!version_ok) {
      return false;
    }
    if (command == "editor-tab-size" && tokens.size() == 2) {
      try {
        restored.tab_size =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(tokens[1].text)), 1, 16);
      } catch (...) {
      }
      continue;
    }
    if (command == "editor-indent-width" && tokens.size() == 2) {
      try {
        restored.indent_width =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(tokens[1].text)), 1, 16);
      } catch (...) {
      }
      continue;
    }
    if (command == "editor-soft-tabs" && tokens.size() == 2) {
      restored.soft_tabs = tokens[1].text == "1" || tokens[1].text == "on" ||
                           tokens[1].text == "true";
      continue;
    }
    if (command == "colorscheme" && tokens.size() == 2) {
      restored_colorscheme = tokens[1].text;
    }
  }

  if (!version_ok) {
    return false;
  }

  editor_preferences_ = restored;
  ApplyEditorPreferencesToAllTabs();
  ApplyColorscheme(restored_colorscheme, false, false);
  return true;
}

void WorkspaceShell::SaveConfigState() const {
  if (project_root_.empty()) {
    return;
  }

  std::ofstream file(ConfigStatePath(), std::ios::trunc);
  if (!file) {
    return;
  }

  file << "version 1\n";
  file << "editor-tab-size " << editor_preferences_.tab_size << '\n';
  file << "editor-indent-width " << editor_preferences_.indent_width << '\n';
  file << "editor-soft-tabs " << (editor_preferences_.soft_tabs ? 1 : 0) << '\n';
  file << "colorscheme " << QuoteCommandArg(active_colorscheme_name_) << '\n';
}

std::filesystem::path WorkspaceShell::SessionStatePath() const {
  return project_root_.empty() ? std::filesystem::path{}
                               : project_root_ / ".microide-session";
}

void WorkspaceShell::ApplyEditorPreferences(editor::TextViewport& viewport) const {
  viewport.SetTabSize(editor_preferences_.tab_size);
  viewport.SetIndentWidth(editor_preferences_.indent_width);
  viewport.SetSoftTabs(editor_preferences_.soft_tabs);
}

void WorkspaceShell::ApplyEditorPreferencesToAllTabs() {
  ApplyEditorPreferences(text_viewport_);
  for (auto& tab : open_tabs_) {
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
      continue;
    }
    for (auto& view : tab.editor_state->views) {
      ApplyEditorPreferences(view.viewport);
    }
  }
}

bool WorkspaceShell::RestoreSessionState() {
  const std::filesystem::path session_path = SessionStatePath();
  std::ifstream file(session_path);
  if (!file) {
    return false;
  }

  bool version_ok = false;
  bool restored_sidebar_visible = sidebar_visible_;
  float restored_sidebar_width = sidebar_width_;
  bool restored_bottom_panel_visible = bottom_panel_visible_;
  float restored_bottom_panel_height = bottom_panel_height_;
  std::optional<std::size_t> restored_active_tab_index;
  std::vector<PersistedEditorTabState> persisted_tabs;
  std::optional<PersistedEditorTabState> current_tab;

  std::string line;
  while (std::getline(file, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.empty()) {
      continue;
    }

    const std::vector<ParsedCommandToken>& tokens = parsed.tokens;
    const std::string& command = tokens.front().text;
    if (command == "version") {
      version_ok = tokens.size() == 2 && tokens[1].text == "1";
      continue;
    }
    if (!version_ok) {
      return false;
    }
    if (command == "sidebar-visible" && tokens.size() == 2) {
      restored_sidebar_visible = tokens[1].text == "1";
      continue;
    }
    if (command == "sidebar-width" && tokens.size() == 2) {
      try {
        restored_sidebar_width = std::stof(tokens[1].text);
      } catch (...) {
      }
      continue;
    }
    if (command == "bottom-panel-visible" && tokens.size() == 2) {
      restored_bottom_panel_visible = tokens[1].text == "1";
      continue;
    }
    if (command == "bottom-panel-height" && tokens.size() == 2) {
      try {
        restored_bottom_panel_height = std::stof(tokens[1].text);
      } catch (...) {
      }
      continue;
    }
    if (command == "active-tab" && tokens.size() == 2) {
      try {
        restored_active_tab_index = static_cast<std::size_t>(std::stoull(tokens[1].text));
      } catch (...) {
      }
      continue;
    }
    if (command == "tab-begin") {
      current_tab = PersistedEditorTabState{};
      continue;
    }
    if (!current_tab.has_value()) {
      continue;
    }
    if (command == "tab-end") {
      persisted_tabs.push_back(*current_tab);
      current_tab.reset();
      continue;
    }
    if (command == "active-leaf" && tokens.size() == 2) {
      try {
        current_tab->active_leaf_id = static_cast<std::size_t>(std::stoull(tokens[1].text));
      } catch (...) {
      }
      continue;
    }
    if (command == "kind" && tokens.size() == 2) {
      current_tab->kind = tokens[1].text;
      continue;
    }
    if (command == "view" && tokens.size() == 7) {
      try {
        current_tab->views.push_back(PersistedEditorViewState{
            .leaf_id = static_cast<std::size_t>(std::stoull(tokens[1].text)),
            .path = std::filesystem::path(tokens[2].text),
            .cursor_line = static_cast<std::size_t>(std::stoull(tokens[3].text)),
            .cursor_column = static_cast<std::size_t>(std::stoull(tokens[4].text)),
            .scroll_line = static_cast<std::size_t>(std::stoull(tokens[5].text)),
            .horizontal_scroll = static_cast<std::size_t>(std::stoull(tokens[6].text)),
        });
      } catch (...) {
      }
      continue;
    }
    if (command == "compare-path" && tokens.size() == 2) {
      current_tab->compare_path = std::filesystem::path(tokens[1].text);
      continue;
    }
    if (command == "compare-commit" && tokens.size() == 3) {
      current_tab->compare_commit_hash = tokens[1].text;
      current_tab->compare_commit_short_hash = tokens[2].text;
      continue;
    }
    if (command == "compare-selected-row" && tokens.size() == 2) {
      try {
        current_tab->compare_selected_row = static_cast<std::size_t>(std::stoull(tokens[1].text));
      } catch (...) {
      }
      continue;
    }
    if (command == "split-node" && tokens.size() == 5) {
      const auto path = DecodeSessionNodePath(tokens[1].text);
      if (!path.has_value()) {
        continue;
      }
      try {
        current_tab->split_nodes.push_back(PersistedSplitNodeState{
            .path = *path,
            .orientation = tokens[2].text,
            .size_fraction = std::stof(tokens[3].text),
            .leaf_id = static_cast<std::size_t>(std::stoull(tokens[4].text)),
        });
      } catch (...) {
      }
      continue;
    }
  }

  if (!version_ok) {
    return false;
  }

  open_tabs_.clear();
  active_tab_index_ = 0;
  bottom_panel_mode_ = BottomPanelMode::Logs;
  overlay_visible_ = false;
  command_mode_ = false;
  compare_picker_matches_.clear();
  compare_picker_commits_.clear();
  compare_picker_selected_index_ = 0;

  for (const PersistedEditorTabState& persisted_tab : persisted_tabs) {
    if (persisted_tab.kind == "compare") {
      std::filesystem::path compare_path = persisted_tab.compare_path;
      if (compare_path.is_relative()) {
        compare_path = project_root_ / compare_path;
      }
      compare_path = compare_path.lexically_normal();

      if (compare_path.empty() || persisted_tab.compare_commit_hash.empty() ||
          persisted_tab.compare_commit_short_hash.empty()) {
        continue;
      }

      const project::GitCommitEntry commit{
          .hash = persisted_tab.compare_commit_hash,
          .short_hash = persisted_tab.compare_commit_short_hash,
          .subject = {},
      };
      auto compare_tab =
          BuildCompareTabEntry(compare_path, commit, persisted_tab.compare_selected_row);
      if (!compare_tab.has_value()) {
        continue;
      }
      open_tabs_.push_back(std::move(*compare_tab));
      continue;
    }

    TabEntry::EditorTabState editor_state;
    editor_state.active_leaf_id = persisted_tab.active_leaf_id;

    for (const PersistedEditorViewState& persisted_view : persisted_tab.views) {
      std::filesystem::path view_path = persisted_view.path;
      if (view_path.is_relative()) {
        view_path = project_root_ / view_path;
      }
      view_path = view_path.lexically_normal();

      editor::TextViewport viewport;
      if (!viewport.OpenFile(view_path)) {
        continue;
      }
      viewport.MoveCursorTo(persisted_view.cursor_line, persisted_view.cursor_column);
      viewport.SetScrollLine(persisted_view.scroll_line);
      viewport.SetHorizontalScroll(persisted_view.horizontal_scroll);
      editor_state.views.push_back(TabEntry::EditorTabState::EditorViewState{
          .leaf_id = persisted_view.leaf_id,
          .viewport = viewport,
      });
    }

    if (editor_state.views.empty()) {
      continue;
    }

    if (!persisted_tab.split_nodes.empty()) {
      std::vector<PersistedSplitNodeState> split_nodes = persisted_tab.split_nodes;
      std::sort(split_nodes.begin(), split_nodes.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.path.size() != rhs.path.size()) {
          return lhs.path.size() < rhs.path.size();
        }
        return lhs.path < rhs.path;
      });

      for (const PersistedSplitNodeState& node_state : split_nodes) {
        auto make_node = [&]() {
          auto node = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
          node->leaf_id = node_state.leaf_id;
          node->size_fraction = std::max(0.0f, node_state.size_fraction);
          if (node_state.orientation == "vertical") {
            node->orientation = EditorSplitOrientation::Vertical;
          } else if (node_state.orientation == "horizontal") {
            node->orientation = EditorSplitOrientation::Horizontal;
          } else {
            node->orientation = EditorSplitOrientation::None;
          }
          return node;
        };

        if (node_state.path.empty()) {
          editor_state.split_root = make_node();
          continue;
        }

        std::vector<std::size_t> parent_path(node_state.path.begin(), node_state.path.end() - 1);
        auto* parent = FindEditorSplitNode(editor_state.split_root.get(), parent_path);
        if (parent == nullptr) {
          continue;
        }
        const std::size_t child_index = node_state.path.back();
        if (parent->children.size() <= child_index) {
          parent->children.resize(child_index + 1);
        }
        parent->children[child_index] = make_node();
      }
    }

    NormalizeEditorSplitTree(editor_state);
    const editor::TextViewport* active_view =
        FindEditorView(editor_state, editor_state.active_leaf_id);
    if (active_view == nullptr) {
      active_view = &editor_state.views.front().viewport;
      editor_state.active_leaf_id = editor_state.views.front().leaf_id;
    }

    const std::filesystem::path tab_path = active_view->path().lexically_normal();
    open_tabs_.push_back(TabEntry{
        .kind = TabEntry::Kind::Editor,
        .path = tab_path,
        .title = EditorTabLabel(*active_view),
        .editor_state = std::move(editor_state),
        .compare = std::nullopt,
    });
  }

  sidebar_visible_ = restored_sidebar_visible;
  sidebar_width_ = restored_sidebar_width;
  bottom_panel_visible_ = restored_bottom_panel_visible;
  bottom_panel_height_ = restored_bottom_panel_height;

  if (open_tabs_.empty()) {
    text_viewport_.SetPlaceholderText(
        "microide\n\n"
        "Project loaded.\n"
        "Use the sidebar to open files.\n");
    focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
    return true;
  }

  const std::size_t active_index = std::min(restored_active_tab_index.value_or(0),
                                            open_tabs_.size() - 1);
  active_tab_index_ = open_tabs_.size();
  ActivateTab(active_index);
  focus_ = FocusTarget::Editor;
  return true;
}

void WorkspaceShell::SaveSessionState() {
  if (project_root_.empty()) {
    return;
  }

  SyncActiveEditorTab();

  std::ofstream file(SessionStatePath(), std::ios::trunc);
  if (!file) {
    return;
  }

  file << "version 1\n";
  file << "sidebar-visible " << (sidebar_visible_ ? 1 : 0) << '\n';
  file << "sidebar-width " << sidebar_width_ << '\n';
  file << "bottom-panel-visible " << (bottom_panel_visible_ ? 1 : 0) << '\n';
  file << "bottom-panel-height " << bottom_panel_height_ << '\n';

  std::size_t persisted_active_tab = 0;
  std::size_t persisted_tab_count = 0;
  for (std::size_t tab_index = 0; tab_index < open_tabs_.size(); ++tab_index) {
    auto& tab = open_tabs_[tab_index];
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
      if (tab_index == active_tab_index_) {
        persisted_active_tab = persisted_tab_count;
      }

      file << "tab-begin\n";
      file << "kind compare\n";
      file << "compare-path " << QuoteCommandArg(tab.compare->path.lexically_normal().string()) << '\n';
      file << "compare-commit " << QuoteCommandArg(tab.compare->commit_hash) << ' '
           << QuoteCommandArg(tab.compare->left_label) << '\n';
      file << "compare-selected-row " << tab.compare->selected_row << '\n';
      file << "tab-end\n";
      ++persisted_tab_count;
      continue;
    }

    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
        tab.editor_state->views.empty()) {
      continue;
    }

    auto& editor_state = tab.editor_state.value();
    NormalizeEditorSplitTree(editor_state);
    if (tab_index == active_tab_index_) {
      persisted_active_tab = persisted_tab_count;
    }

    file << "tab-begin\n";
    file << "kind editor\n";
    file << "active-leaf " << editor_state.active_leaf_id << '\n';
    for (const auto& view : editor_state.views) {
      const std::filesystem::path normalized_path = view.viewport.path().lexically_normal();
      if (normalized_path.empty()) {
        continue;
      }
      file << "view " << view.leaf_id << ' '
           << QuoteCommandArg(normalized_path.string()) << ' '
           << view.viewport.cursor_line() << ' ' << view.viewport.cursor_column() << ' '
           << view.viewport.scroll_line() << ' ' << view.viewport.horizontal_scroll() << '\n';
    }

    std::vector<std::size_t> node_path;
    const auto write_split_node =
        [&](auto&& self,
            const TabEntry::EditorTabState::EditorSplitNode* node) -> void {
          if (node == nullptr) {
            return;
          }

          std::string orientation = "leaf";
          if (!node->IsLeaf()) {
            orientation = node->orientation == EditorSplitOrientation::Horizontal ? "horizontal"
                                                                                 : "vertical";
          }
          file << "split-node " << EncodeSessionNodePath(node_path) << ' ' << orientation << ' '
               << node->size_fraction << ' ' << node->leaf_id << '\n';
          for (std::size_t child_index = 0; child_index < node->children.size(); ++child_index) {
            node_path.push_back(child_index);
            self(self, node->children[child_index].get());
            node_path.pop_back();
          }
        };
    write_split_node(write_split_node, editor_state.split_root.get());
    file << "tab-end\n";
    ++persisted_tab_count;
  }

  file << "active-tab " << persisted_active_tab << '\n';
}

std::filesystem::path WorkspaceShell::WorkspaceSessionStatePath() const {
  if (const char* xdg_state_home = std::getenv("XDG_STATE_HOME");
      xdg_state_home != nullptr && *xdg_state_home != '\0') {
    return std::filesystem::path(xdg_state_home) / "microide" / "workspace-session";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".local" / "state" / "microide" / "workspace-session";
  }
  return {};
}

bool WorkspaceShell::RestoreWorkspaceSession() {
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty()) {
    return false;
  }

  std::ifstream file(session_path);
  if (!file) {
    return false;
  }

  bool version_ok = false;
  std::vector<std::filesystem::path> project_roots;
  std::optional<std::size_t> restored_active_project;
  std::string line;
  while (std::getline(file, line)) {
    const ParsedCommandLine parsed = ParseCommandLine(line);
    if (parsed.tokens.empty()) {
      continue;
    }

    const auto& tokens = parsed.tokens;
    const std::string& command = tokens.front().text;
    if (command == "version") {
      version_ok = tokens.size() == 2 && tokens[1].text == "1";
      continue;
    }
    if (!version_ok) {
      return false;
    }
    if (command == "project" && tokens.size() == 2) {
      project_roots.push_back(std::filesystem::path(tokens[1].text));
      continue;
    }
    if (command == "active-project" && tokens.size() == 2) {
      try {
        restored_active_project = static_cast<std::size_t>(std::stoull(tokens[1].text));
      } catch (...) {
      }
    }
  }

  if (!version_ok) {
    return false;
  }

  projects_.clear();
  active_project_index_ = 0;
  project_tab_scroll_index_ = 0;

  if (project_roots.empty()) {
    ResetProjectScopedState(true);
    return true;
  }

  for (const auto& root : project_roots) {
    if (!InitializeCurrentProject(root, true, false)) {
      continue;
    }
    auto project_state = std::make_unique<ProjectWorkspaceState>();
    StoreCurrentProjectState(*project_state);
    projects_.push_back(std::move(project_state));
  }

  if (projects_.empty()) {
    ResetProjectScopedState(true);
    return true;
  }

  active_project_index_ =
      std::min(restored_active_project.value_or(0), projects_.size() - 1);
  LoadProjectState(*projects_[active_project_index_]);
  EnsureActiveProjectVisible();
  LogMessage("Restored workspace session");
  return true;
}

void WorkspaceShell::SaveWorkspaceSession() {
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty()) {
    return;
  }

  std::error_code error;
  std::filesystem::create_directories(session_path.parent_path(), error);

  std::ofstream file(session_path, std::ios::trunc);
  if (!file) {
    return;
  }

  file << "version 1\n";
  for (std::size_t i = 0; i < projects_.size(); ++i) {
    const std::filesystem::path project_root =
        (!project_root_.empty() && i == active_project_index_) ? project_root_
        : projects_[i] != nullptr                               ? projects_[i]->root
                                                               : std::filesystem::path{};
    if (project_root.empty()) {
      continue;
    }
    file << "project " << QuoteCommandArg(project_root.lexically_normal().string()) << '\n';
  }
  file << "active-project "
       << (projects_.empty() ? 0 : std::min(active_project_index_, projects_.size() - 1)) << '\n';
}

bool WorkspaceShell::ActiveTabIsCompare() const {
  return active_tab_index_ < open_tabs_.size() &&
         open_tabs_[active_tab_index_].kind == TabEntry::Kind::Compare &&
         open_tabs_[active_tab_index_].compare.has_value();
}

WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() {
  if (!ActiveTabIsCompare()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].compare.value();
}

const WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() const {
  if (!ActiveTabIsCompare()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].compare.value();
}

int WorkspaceShell::CompareVisibleRows(const SDL_FRect& rect) const {
  const float line_height = text_renderer_.LineHeight();
  const float rows_y = rect.y + line_height + 12.0f;
  return std::max(
      1, static_cast<int>((rect.h - (rows_y - rect.y) - 8.0f) / std::max(1.0f, line_height)));
}

int WorkspaceShell::CompareMaxScrollRow(const CompareTabState& compare_tab, int visible_rows) const {
  return std::max(0, static_cast<int>(compare_tab.model.rows.size()) - std::max(1, visible_rows));
}

void WorkspaceShell::ClampCompareScrollRow(CompareTabState& compare_tab, int visible_rows) const {
  compare_tab.scroll_row =
      std::clamp(compare_tab.scroll_row, 0, CompareMaxScrollRow(compare_tab, visible_rows));
}

void WorkspaceShell::RevealActiveCompareSelection() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  const int visible_rows = CompareVisibleRows(layout.editor_surface);
  ClampCompareScrollRow(*compare_tab, visible_rows);
  if (compare_tab->selected_row < static_cast<std::size_t>(compare_tab->scroll_row)) {
    compare_tab->scroll_row = static_cast<int>(compare_tab->selected_row);
  } else if (compare_tab->selected_row >=
             static_cast<std::size_t>(compare_tab->scroll_row + visible_rows)) {
    compare_tab->scroll_row = static_cast<int>(compare_tab->selected_row) - visible_rows + 1;
  }
  ClampCompareScrollRow(*compare_tab, visible_rows);
}

std::string WorkspaceShell::ActiveTabTitle() const {
  if (active_tab_index_ >= open_tabs_.size()) {
    return EditorTabLabel(text_viewport_);
  }
  return open_tabs_[active_tab_index_].title;
}

bool WorkspaceShell::HandleEvent(const SDL_Event& event) {
  if (project_search_event_type_ != 0 && event.type == project_search_event_type_) {
    ConsumeProjectSearchUpdates();
    return true;
  }
  if (terminal_event_type_ != 0 && event.type == terminal_event_type_) {
    ReapExitedTerminalTabs();
    return true;
  }

  SyncTextInputSurface(nullptr);

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      return HandleMouseButtonDown(event);
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return HandleMouseButtonUp(event);
    case SDL_EVENT_MOUSE_MOTION:
      return HandleMouseMotion(event);
    case SDL_EVENT_MOUSE_WHEEL:
      return HandleMouseWheel(event);
    case SDL_EVENT_TEXT_EDITING:
      return HandleTextEditing(event.edit);
    case SDL_EVENT_TEXT_INPUT:
      return HandleTextInput(event.text);
    case SDL_EVENT_KEY_DOWN:
      break;
    default:
      return false;
  }

  if (dirty_prompt_visible_) {
    switch (event.key.key) {
      case SDLK_ESCAPE:
        DismissDirtyPrompt(true);
        LogMessage("Close cancelled");
        return true;
      case SDLK_LEFT:
        dirty_prompt_state_.selected_action =
            std::max(0, dirty_prompt_state_.selected_action - 1);
        return true;
      case SDLK_RIGHT:
      case SDLK_TAB:
        dirty_prompt_state_.selected_action =
            std::min(2, dirty_prompt_state_.selected_action + 1);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        ConfirmDirtyPrompt();
        return true;
      default: {
        const char input_character = KeycodeToAscii(event.key.key, SDL_GetModState());
        if (input_character == 's') {
          dirty_prompt_state_.selected_action = 0;
          ConfirmDirtyPrompt();
          return true;
        }
        if (input_character == 'd') {
          dirty_prompt_state_.selected_action = 1;
          ConfirmDirtyPrompt();
          return true;
        }
        if (input_character == 'c') {
          DismissDirtyPrompt(true);
          LogMessage("Close cancelled");
          return true;
        }
        return true;
      }
    }
  }

  const SDL_Keymod modifiers = SDL_GetModState();
  if (tree_context_menu_.open) {
    switch (event.key.key) {
      case SDLK_ESCAPE:
        CloseTreeContextMenu();
        return true;
      case SDLK_DOWN:
        tree_context_menu_.active_item_index =
            NextEnabledTreeContextMenuItemIndex(tree_context_menu_.active_item_index, 1);
        return true;
      case SDLK_UP:
        tree_context_menu_.active_item_index =
            NextEnabledTreeContextMenuItemIndex(tree_context_menu_.active_item_index, -1);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (tree_context_menu_.active_item_index >= 0) {
          return ExecuteTreeContextMenuItem(
              static_cast<std::size_t>(tree_context_menu_.active_item_index));
        }
        return true;
      default:
        return true;
    }
  }
  if (menu_bar_open_) {
    switch (event.key.key) {
      case SDLK_ESCAPE:
        CloseMenuBar();
        return true;
      case SDLK_LEFT:
        return SwitchMenuBarMenu(-1);
      case SDLK_RIGHT:
        return SwitchMenuBarMenu(1);
      case SDLK_TAB:
        return SwitchMenuBarMenu((modifiers & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
      case SDLK_DOWN:
        return MoveActiveMenuItem(1);
      case SDLK_UP:
        return MoveActiveMenuItem(-1);
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (active_menu_item_index_ >= 0) {
          return ExecuteMenuItem(active_menu_id_,
                                 static_cast<std::size_t>(active_menu_item_index_));
        }
        return true;
      default:
        return true;
    }
  }
  if (CompositionConsumesKey(event.key.key, modifiers)) {
    return true;
  }
  if (prompt_surface_visible_) {
    if (prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput) {
      switch (event.key.key) {
        case SDLK_ESCAPE:
          {
            const std::string title = PromptSurfaceTitle();
            DismissPromptSurface(true);
            LogMessage(title + " cancelled");
          }
          return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          prompt_surface_state_.selected_button = 0;
          ConfirmPromptSurface();
          return true;
        case SDLK_BACKSPACE:
          RemoveLastUtf8Codepoint(&prompt_surface_state_.input);
          return true;
        default:
          return true;
      }
    }

    switch (event.key.key) {
      case SDLK_ESCAPE:
        {
          const std::string title = PromptSurfaceTitle();
          DismissPromptSurface(true);
          LogMessage(title + " cancelled");
        }
        return true;
      case SDLK_LEFT:
        prompt_surface_state_.selected_button =
            std::max(0, prompt_surface_state_.selected_button - 1);
        return true;
      case SDLK_RIGHT:
      case SDLK_TAB:
        prompt_surface_state_.selected_button =
            std::min(1, prompt_surface_state_.selected_button + 1);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        ConfirmPromptSurface();
        return true;
      default:
        return true;
    }
  }
  const bool active_compare_tab = ActiveTabIsCompare();
  if ((modifiers & SDL_KMOD_CTRL) && !command_mode_ && !overlay_visible_ &&
      focus_ == FocusTarget::Editor && !active_compare_tab && event.key.key == SDLK_A) {
    ExecuteAction(ActionId::SelectAll, {}, ActionSource::Shortcut);
    return true;
  }

  if ((modifiers & SDL_KMOD_CTRL) && !command_mode_ && !overlay_visible_ &&
      focus_ == FocusTarget::Editor && !active_compare_tab) {
    if ((modifiers & SDL_KMOD_SHIFT) && event.key.key == SDLK_F) {
      ExecuteAction(ActionId::Rg, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_H) {
      ExecuteAction(ActionId::ReplaceInBuffer, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_F) {
      ExecuteAction(ActionId::Search, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_W) {
      ExecuteAction(ActionId::CloseActiveTab, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_Z) {
      ExecuteAction((modifiers & SDL_KMOD_SHIFT) != 0 ? ActionId::Redo : ActionId::Undo, {},
                    ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_Y) {
      ExecuteAction(ActionId::Redo, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_C) {
      ExecuteAction(ActionId::CopySelection, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_X) {
      ExecuteAction(ActionId::CutSelection, {}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_V) {
      ExecuteAction(ActionId::PasteClipboard, {}, ActionSource::Shortcut);
      return true;
    }
  }

  if ((modifiers & SDL_KMOD_CTRL) && !active_compare_tab && event.key.key == SDLK_S) {
    ExecuteAction(ActionId::Save, {}, ActionSource::Shortcut);
    return true;
  }

  if (modifiers & SDL_KMOD_CTRL) {
    if (event.key.key == SDLK_0 || event.key.key == SDLK_KP_0) {
      ExecuteAction(ActionId::UiScale, {"reset"}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_MINUS || event.key.key == SDLK_KP_MINUS) {
      ExecuteAction(ActionId::UiScale, {"down"}, ActionSource::Shortcut);
      return true;
    }
    if (event.key.key == SDLK_EQUALS || event.key.key == SDLK_PLUS ||
        event.key.key == SDLK_KP_PLUS) {
      ExecuteAction(ActionId::UiScale, {"up"}, ActionSource::Shortcut);
      return true;
    }
  }

  if ((modifiers & SDL_KMOD_CTRL) && event.key.key == SDLK_E) {
    ExecuteAction(ActionId::OpenCommandPrompt, {}, ActionSource::Shortcut);
    return true;
  }

  if (command_mode_) {
    switch (event.key.key) {
      case SDLK_ESCAPE:
        command_mode_ = false;
        command_input_.clear();
        ResetCommandSessionState();
        LogMessage("Command mode closed");
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (command_input_.empty() || ExecuteCommand(command_input_)) {
          command_mode_ = false;
          command_input_.clear();
          ResetCommandSessionState();
        }
        return true;
      case SDLK_BACKSPACE:
        RemoveLastUtf8Codepoint(&command_input_);
        command_history_index_.reset();
        command_history_pending_input_.clear();
        ClearCommandCompletionFeedback();
        return true;
      case SDLK_UP:
        StepCommandHistory(-1);
        return true;
      case SDLK_DOWN:
        StepCommandHistory(1);
        return true;
      case SDLK_TAB:
        CompleteCommandInput();
        return true;
      default:
        return false;
    }
  }

  switch (event.key.key) {
    case SDLK_F8:
      ExecuteAction(ActionId::SidebarToggle, {}, ActionSource::Shortcut);
      return true;
    case SDLK_F6:
      ExecuteAction(ActionId::Files, {}, ActionSource::Shortcut);
      return true;
    case SDLK_F9:
      ExecuteAction(ActionId::ToggleBottomPanel, {}, ActionSource::Shortcut);
      return true;
    case SDLK_TAB:
      if (modifiers & SDL_KMOD_CTRL) {
        if (overlay_visible_) {
          focus_ = FocusTarget::Overlay;
          return true;
        }
        const bool include_panel = bottom_panel_visible_ && BottomPanelShowsTerminal();
        if (include_panel) {
          if (sidebar_visible_) {
            if (modifiers & SDL_KMOD_SHIFT) {
              focus_ = focus_ == FocusTarget::Sidebar
                           ? FocusTarget::Panel
                           : focus_ == FocusTarget::Panel ? FocusTarget::Editor
                                                          : FocusTarget::Sidebar;
            } else {
              focus_ = focus_ == FocusTarget::Sidebar
                           ? FocusTarget::Editor
                           : focus_ == FocusTarget::Editor ? FocusTarget::Panel
                                                           : FocusTarget::Sidebar;
            }
          } else {
            focus_ = focus_ == FocusTarget::Panel ? FocusTarget::Editor : FocusTarget::Panel;
          }
        } else if (sidebar_visible_ && !(modifiers & SDL_KMOD_SHIFT)) {
          focus_ = focus_ == FocusTarget::Sidebar ? FocusTarget::Editor : FocusTarget::Sidebar;
        } else if (sidebar_visible_) {
          focus_ = focus_ == FocusTarget::Editor ? FocusTarget::Sidebar : FocusTarget::Editor;
        } else {
          focus_ = FocusTarget::Editor;
        }
        return true;
      }
      break;
    case SDLK_ESCAPE:
      if (overlay_visible_) {
        overlay_visible_ = false;
        focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
        LogMessage("Finder overlay closed");
        return true;
      }
      if (focus_ == FocusTarget::Sidebar && sidebar_visible_ &&
          sidebar_temporary_ && sidebar_mode_ == SidebarMode::Search) {
        CloseSidebar();
        return true;
      }
      return false;
    default:
      break;
  }

  if (focus_ == FocusTarget::Overlay) {
    if (overlay_mode_ == OverlayMode::CommitPicker) {
      switch (event.key.key) {
        case SDLK_ESCAPE:
          overlay_visible_ = false;
          focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
          LogMessage("Compare picker closed");
          return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          return ActivateOverlaySelection();
        case SDLK_UP:
          MoveComparePickerSelection(-1);
          return true;
        case SDLK_DOWN:
          MoveComparePickerSelection(1);
          return true;
        case SDLK_HOME:
          if (!compare_picker_matches_.empty()) {
            compare_picker_selected_index_ = 0;
            if (last_window_width_ > 0 && last_window_height_ > 0) {
              const WorkspaceLayout layout = ComputeLayout(
                  static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                  sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
              RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
            }
          }
          return true;
        case SDLK_END:
          if (!compare_picker_matches_.empty()) {
            compare_picker_selected_index_ = compare_picker_matches_.size() - 1;
            if (last_window_width_ > 0 && last_window_height_ > 0) {
              const WorkspaceLayout layout = ComputeLayout(
                  static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                  sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
              RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
            }
          }
          return true;
        case SDLK_PAGEUP:
          MoveComparePickerSelection(-8);
          return true;
        case SDLK_PAGEDOWN:
          MoveComparePickerSelection(8);
          return true;
        case SDLK_BACKSPACE:
          if (RemoveLastUtf8Codepoint(&compare_picker_query_)) {
            RefreshComparePicker();
          }
          return true;
        default:
          return false;
      }
    }
    if (overlay_mode_ == OverlayMode::BufferSearch) {
      switch (event.key.key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          return ActivateOverlaySelection();
        case SDLK_UP:
          MoveBufferSearchSelection(-1);
          return true;
        case SDLK_DOWN:
          MoveBufferSearchSelection(1);
          return true;
        case SDLK_PAGEUP:
          MoveBufferSearchSelection(-8);
          return true;
        case SDLK_PAGEDOWN:
          MoveBufferSearchSelection(8);
          return true;
        case SDLK_BACKSPACE:
          if (RemoveLastUtf8Codepoint(&buffer_search_query_)) {
            RefreshBufferSearch();
          }
          return true;
        default:
          return false;
      }
    }

    if (overlay_mode_ == OverlayMode::BufferReplace) {
      switch (event.key.key) {
        case SDLK_ESCAPE:
          overlay_visible_ = false;
          focus_ = FocusTarget::Editor;
          LogMessage("Buffer replace closed");
          return true;
        case SDLK_TAB:
          buffer_search_field_ = buffer_search_field_ == BufferSearchField::Search
                                     ? BufferSearchField::Replace
                                     : BufferSearchField::Search;
          return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          if (modifiers & SDL_KMOD_CTRL) {
            ReplaceAllBufferSearchMatches();
          } else {
            ReplaceCurrentBufferSearchMatch();
          }
          return true;
        case SDLK_UP:
          MoveBufferSearchSelection(-1);
          return true;
        case SDLK_DOWN:
          MoveBufferSearchSelection(1);
          return true;
        case SDLK_PAGEUP:
          MoveBufferSearchSelection(-8);
          return true;
        case SDLK_PAGEDOWN:
          MoveBufferSearchSelection(8);
          return true;
        case SDLK_BACKSPACE:
          if (buffer_search_field_ == BufferSearchField::Search) {
            if (RemoveLastUtf8Codepoint(&buffer_search_query_)) {
              RefreshBufferSearch();
            }
          } else {
            RemoveLastUtf8Codepoint(&buffer_replace_text_);
          }
          return true;
        default:
          return false;
      }
    }

    if (overlay_mode_ == OverlayMode::ProjectSearch) {
      switch (event.key.key) {
        case SDLK_ESCAPE:
          overlay_visible_ = false;
          focus_ = FocusTarget::Editor;
          LogMessage("Project search closed");
          return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          return ActivateOverlaySelection();
        case SDLK_UP:
          MoveProjectSearchSelection(-1);
          return true;
        case SDLK_DOWN:
          MoveProjectSearchSelection(1);
          return true;
        case SDLK_PAGEUP:
          MoveProjectSearchSelection(-8);
          return true;
        case SDLK_PAGEDOWN:
          MoveProjectSearchSelection(8);
          return true;
        case SDLK_BACKSPACE:
          if (RemoveLastUtf8Codepoint(&project_search_query_)) {
            RefreshProjectSearch();
          }
          return true;
        default:
          return false;
      }
    }

    switch (event.key.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return ActivateOverlaySelection();
      case SDLK_UP:
        MoveFileFinderSelection(-1);
        return true;
      case SDLK_DOWN:
        MoveFileFinderSelection(1);
        return true;
      case SDLK_PAGEUP:
        MoveFileFinderSelection(-8);
        return true;
      case SDLK_PAGEDOWN:
        MoveFileFinderSelection(8);
        return true;
      case SDLK_BACKSPACE:
        file_finder_.Backspace();
        ResetOverlayScroll();
        return true;
      default:
        return false;
    }
  }

  if (focus_ == FocusTarget::Sidebar && sidebar_visible_) {
    if (sidebar_mode_ == SidebarMode::Search) {
      const char input_character = KeycodeToAscii(event.key.key, modifiers);
      if (project_search_editing_) {
        switch (event.key.key) {
          case SDLK_ESCAPE:
            CancelProjectSearchEdit();
            return true;
          case SDLK_RETURN:
          case SDLK_KP_ENTER:
            CommitProjectSearchEdit();
            return true;
          case SDLK_BACKSPACE:
            RemoveLastUtf8Codepoint(&project_search_edit_buffer_);
            return true;
          default:
            return false;
        }
      }

      switch (event.key.key) {
        case SDLK_ESCAPE:
          if (sidebar_temporary_) {
            CloseSidebar();
            return true;
          }
          return false;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_RIGHT:
          if (!project_search_results_.empty() &&
              project_search_selected_index_ < project_search_results_.size()) {
            const auto& result = project_search_results_[project_search_selected_index_];
            OpenFile(project_root_ / result.relative_path);
            text_viewport_.MoveCursorTo(result.line, result.column);
            if (sidebar_temporary_) {
              RestorePreviousSidebar();
            }
            focus_ = FocusTarget::Editor;
            LogMessage("Project search result opened");
          }
          return true;
        case SDLK_UP:
          MoveProjectSearchSelection(-1);
          return true;
        case SDLK_DOWN:
          MoveProjectSearchSelection(1);
          return true;
        case SDLK_HOME:
          if (!project_search_results_.empty()) {
            project_search_selected_index_ = 0;
          }
          return true;
        case SDLK_END:
          if (!project_search_results_.empty()) {
            project_search_selected_index_ = project_search_results_.size() - 1;
          }
          return true;
        case SDLK_PAGEUP:
          MoveProjectSearchSelection(-8);
          return true;
        case SDLK_PAGEDOWN:
          MoveProjectSearchSelection(8);
          return true;
        case SDLK_R:
          if (input_character == 'R') {
            ReplaceAllProjectSearchMatches();
          } else {
            RefreshProjectSearch();
            LogMessage("Project search refreshed");
          }
          return true;
        case SDLK_EQUALS:
          BeginProjectSearchEdit(ProjectSearchEditField::Replace);
          return true;
        case SDLK_SLASH:
          BeginProjectSearchEdit(ProjectSearchEditField::Query);
          return true;
        default:
          if (event.key.key == SDLK_J && input_character == 'j') {
            MoveProjectSearchSelection(1);
            return true;
          }
          if (event.key.key == SDLK_K && input_character == 'k') {
            MoveProjectSearchSelection(-1);
            return true;
          }
          return false;
      }
    }

    switch (event.key.key) {
      case SDLK_UP:
        directory_tree_.MoveSelection(-1);
        return true;
      case SDLK_DOWN:
        directory_tree_.MoveSelection(1);
        return true;
      case SDLK_LEFT:
        directory_tree_.CollapseSelection();
        return true;
      case SDLK_RIGHT:
        directory_tree_.ExpandSelection();
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER: {
        const auto opened = directory_tree_.ActivateSelection();
        if (opened.has_value()) {
          OpenFile(*opened);
        } else {
          LogMessage("Tree selection toggled");
        }
        return true;
      }
      case SDLK_R:
        RefreshProjectFiles();
        LogMessage("Project tree refreshed");
        return true;
      case SDLK_D:
        OpenComparePicker();
        return true;
      default:
        return false;
    }
  }

  if (focus_ == FocusTarget::Panel && BottomPanelShowsTerminal()) {
    return HandleTerminalKeyDown(event.key, modifiers);
  }

  if (focus_ == FocusTarget::Editor && active_compare_tab) {
    switch (event.key.key) {
      case SDLK_ESCAPE:
        RequestCloseTab(active_tab_index_);
        return true;
      case SDLK_UP:
        MoveCompareSelection(-1);
        return true;
      case SDLK_DOWN:
        MoveCompareSelection(1);
        return true;
      case SDLK_PAGEUP:
        MoveCompareSelection(-20);
        return true;
      case SDLK_PAGEDOWN:
        MoveCompareSelection(20);
        return true;
      case SDLK_HOME:
        if (auto* compare_tab = ActiveCompareTab(); compare_tab != nullptr) {
          compare_tab->selected_row = 0;
          RevealActiveCompareSelection();
        }
        return true;
      case SDLK_END:
        if (auto* compare_tab = ActiveCompareTab();
            compare_tab != nullptr && !compare_tab->model.rows.empty()) {
          compare_tab->selected_row = compare_tab->model.rows.size() - 1;
          RevealActiveCompareSelection();
        }
        return true;
      case SDLK_LEFTBRACKET:
        JumpCompareHunk(-1);
        return true;
      case SDLK_RIGHTBRACKET:
        JumpCompareHunk(1);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        OpenWorkingFileFromCompare();
        return true;
      default: {
        const char input_character = KeycodeToAscii(event.key.key, modifiers);
        if (input_character == 'j') {
          MoveCompareSelection(1);
          return true;
        }
        if (input_character == 'k') {
          MoveCompareSelection(-1);
          return true;
        }
        if (input_character == 'o') {
          OpenWorkingFileFromCompare();
          return true;
        }
        return false;
      }
    }
  }

  switch (event.key.key) {
    case SDLK_TAB:
      text_viewport_.InsertTab();
      ResetCaretBlink();
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      text_viewport_.InsertNewline();
      ResetCaretBlink();
      return true;
    case SDLK_BACKSPACE:
      text_viewport_.Backspace();
      ResetCaretBlink();
      return true;
    case SDLK_DELETE:
      text_viewport_.DeleteForward();
      ResetCaretBlink();
      return true;
    default:
      break;
  }

  switch (event.key.key) {
    case SDLK_UP:
      text_viewport_.MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      ResetCaretBlink();
      return true;
    case SDLK_DOWN:
      text_viewport_.MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      ResetCaretBlink();
      return true;
    case SDLK_LEFT:
      text_viewport_.MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      ResetCaretBlink();
      return true;
    case SDLK_RIGHT:
      text_viewport_.MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      ResetCaretBlink();
      return true;
    case SDLK_PAGEUP:
      text_viewport_.Page(-1);
      ResetCaretBlink();
      return true;
    case SDLK_PAGEDOWN:
      text_viewport_.Page(1);
      ResetCaretBlink();
      return true;
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        text_viewport_.MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        text_viewport_.MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      ResetCaretBlink();
      return true;
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line =
            text_viewport_.line_count() == 0 ? 0 : text_viewport_.line_count() - 1;
        text_viewport_.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                                    (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        text_viewport_.MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      ResetCaretBlink();
      return true;
    default:
      return false;
  }
}

void WorkspaceShell::ActivateTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  if (active_tab_index_ < open_tabs_.size() && active_tab_index_ != index) {
    SyncActiveEditorTab();
  }

  active_tab_index_ = index;
  auto& tab = open_tabs_[index];
  if (tab.kind == TabEntry::Kind::Editor) {
    if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
      NormalizeEditorSplitTree(*tab.editor_state);
      editor::TextViewport* active_view =
          FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
      if (active_view == nullptr && !tab.editor_state->views.empty()) {
        tab.editor_state->active_leaf_id = tab.editor_state->views.front().leaf_id;
        active_view = &tab.editor_state->views.front().viewport;
      }
      if (active_view != nullptr) {
        text_viewport_ = *active_view;
        ApplyEditorPreferences(text_viewport_);
      }
    } else {
      editor::TextViewport loaded_view;
      if (!loaded_view.OpenFile(tab.path)) {
        LogMessage("Failed to open file: " + tab.path.lexically_normal().string());
        return;
      }
      ApplyEditorPreferences(loaded_view);
      text_viewport_ = loaded_view;
      tab.editor_state = MakeEditorTabState(loaded_view);
    }
  }
  SyncActiveEditorTabMetadata();
  if (tab.kind == TabEntry::Kind::Compare) {
    RevealActiveCompareSelection();
  }
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
}

void WorkspaceShell::SyncActiveEditorTab() {
  if (active_tab_index_ >= open_tabs_.size()) {
    return;
  }

  auto& tab = open_tabs_[active_tab_index_];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return;
  }

  if (tab.editor_state->views.empty()) {
    tab.editor_state = MakeEditorTabState(text_viewport_);
    return;
  }

  NormalizeEditorSplitTree(*tab.editor_state);
  if (auto* active_view = FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
      active_view != nullptr) {
    *active_view = text_viewport_;
  }
  if (active_tab_index_ < open_tabs_.size() && &tab == &open_tabs_[active_tab_index_]) {
    SyncActiveEditorTabMetadata();
  }
}

bool WorkspaceShell::SaveTab(std::size_t index) {
  if (index >= open_tabs_.size() || open_tabs_[index].kind != TabEntry::Kind::Editor) {
    return false;
  }

  auto& editor_state = open_tabs_[index].editor_state;
  if (!editor_state.has_value() || editor_state->views.empty()) {
    return false;
  }

  if (index == active_tab_index_) {
    SyncActiveEditorTab();
  }

  bool attempted_save = false;
  for (auto& view : editor_state->views) {
    editor::TextViewport* candidate = &view.viewport;
    if (index == active_tab_index_ && view.leaf_id == editor_state->active_leaf_id) {
      candidate = &text_viewport_;
    }

    if (candidate->path().empty()) {
      if (candidate->dirty()) {
        return false;
      }
      continue;
    }

    if (!candidate->dirty()) {
      continue;
    }

    if (!candidate->Save()) {
      return false;
    }
    attempted_save = true;

    if (index == active_tab_index_ && candidate == &text_viewport_) {
      view.viewport = text_viewport_;
    }
  }

  if (index == active_tab_index_) {
    SyncActiveEditorTab();
  }
  return attempted_save || !editor_state->views.empty();
}

bool WorkspaceShell::TabIsDirty(std::size_t index) const {
  if (index >= open_tabs_.size() || open_tabs_[index].kind != TabEntry::Kind::Editor) {
    return false;
  }

  const auto& editor_state = open_tabs_[index].editor_state;
  if (!editor_state.has_value() || editor_state->views.empty()) {
    return false;
  }

  for (const auto& view : editor_state->views) {
    if (index == active_tab_index_ && view.leaf_id == editor_state->active_leaf_id) {
      if (text_viewport_.dirty()) {
        return true;
      }
      continue;
    }
    if (view.viewport.dirty()) {
      return true;
    }
  }
  return false;
}

std::string WorkspaceShell::TabDisplayTitle(std::size_t index) const {
  if (index >= open_tabs_.size()) {
    return {};
  }

  const std::string& title = open_tabs_[index].title;
  return TabIsDirty(index) ? "*" + title : title;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices() const {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(open_tabs_.size());
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    if (TabIsDirty(i)) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices(
    const ProjectWorkspaceState& state) {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state.open_tabs.size());
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    const auto& tab = state.open_tabs[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
        tab.editor_state->views.empty()) {
      continue;
    }
    const bool dirty = std::any_of(tab.editor_state->views.begin(), tab.editor_state->views.end(),
                                   [](const auto& view) { return view.viewport.dirty(); });
    if (dirty) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndicesForProject(
    std::size_t project_index) const {
  if (project_index >= projects_.size()) {
    return {};
  }
  if (!project_root_.empty() && project_index == active_project_index_) {
    return DirtyEditorTabIndices();
  }
  return projects_[project_index] == nullptr ? std::vector<std::size_t>{}
                                             : DirtyEditorTabIndices(*projects_[project_index]);
}

void WorkspaceShell::ShowDirtyPromptForTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  dirty_prompt_visible_ = true;
  dirty_prompt_previous_focus_ = focus_;
  dirty_prompt_state_.kind = DirtyPromptState::Kind::CloseTab;
  dirty_prompt_state_.tab_index = index;
  dirty_prompt_state_.dirty_tabs = {index};
  dirty_prompt_state_.dirty_count = 1;
  dirty_prompt_state_.selected_action = 0;
  focus_ = FocusTarget::Overlay;
}

void WorkspaceShell::ShowDirtyPromptForProject(std::size_t index) {
  if (index >= projects_.size()) {
    return;
  }

  const std::vector<std::size_t> dirty_tabs = DirtyEditorTabIndicesForProject(index);
  if (dirty_tabs.empty()) {
    CloseProject(index);
    return;
  }

  dirty_prompt_visible_ = true;
  dirty_prompt_previous_focus_ = focus_;
  dirty_prompt_state_.kind = DirtyPromptState::Kind::CloseProject;
  dirty_prompt_state_.project_index = index;
  dirty_prompt_state_.dirty_tabs = dirty_tabs;
  dirty_prompt_state_.dirty_count = dirty_tabs.size();
  dirty_prompt_state_.selected_action = 0;
  focus_ = FocusTarget::Overlay;
}

void WorkspaceShell::ShowDirtyPromptForQuit() {
  std::size_t dirty_count = DirtyEditorTabIndices().size();
  for (std::size_t i = 0; i < projects_.size(); ++i) {
    if (!project_root_.empty() && i == active_project_index_) {
      continue;
    }
    dirty_count += DirtyEditorTabIndicesForProject(i).size();
  }

  dirty_prompt_visible_ = true;
  dirty_prompt_previous_focus_ = focus_;
  dirty_prompt_state_.kind = DirtyPromptState::Kind::Quit;
  dirty_prompt_state_.tab_index = active_tab_index_;
  dirty_prompt_state_.project_index = active_project_index_;
  dirty_prompt_state_.dirty_tabs = DirtyEditorTabIndices();
  dirty_prompt_state_.dirty_count = dirty_count;
  dirty_prompt_state_.selected_action = 0;
  focus_ = FocusTarget::Overlay;
}

void WorkspaceShell::DismissDirtyPrompt(bool restore_focus) {
  dirty_prompt_visible_ = false;
  dirty_prompt_state_ = DirtyPromptState{};
  if (restore_focus) {
    focus_ = dirty_prompt_previous_focus_;
  }
}

void WorkspaceShell::ConfirmDirtyPrompt() {
  if (!dirty_prompt_visible_) {
    return;
  }

  const DirtyPromptState prompt = dirty_prompt_state_;
  if (prompt.selected_action == 2) {
    DismissDirtyPrompt(true);
    LogMessage(prompt.kind == DirtyPromptState::Kind::Quit ? "Quit cancelled" : "Close cancelled");
    return;
  }

  if (prompt.kind == DirtyPromptState::Kind::CloseTab) {
    if (prompt.selected_action == 0 && !SaveTab(prompt.tab_index)) {
      LogMessage("Save failed");
      return;
    }
    DismissDirtyPrompt(false);
    CloseTab(prompt.tab_index);
    return;
  }

  if (prompt.kind == DirtyPromptState::Kind::CloseProject) {
    if (prompt.project_index >= projects_.size()) {
      DismissDirtyPrompt(true);
      return;
    }
    if (prompt.selected_action == 0 &&
        (prompt.project_index != active_project_index_ || project_root_.empty())) {
      if (!SwitchProject(prompt.project_index, false)) {
        DismissDirtyPrompt(true);
        LogMessage("Failed to switch project");
        return;
      }
    }
    if (prompt.selected_action == 0) {
      for (std::size_t index : prompt.dirty_tabs) {
        if (!SaveTab(index)) {
          LogMessage("Save failed");
          return;
        }
      }
    }
    DismissDirtyPrompt(false);
    CloseProject(active_project_index_);
    return;
  }

  if (prompt.selected_action == 0) {
    const std::size_t project_count = projects_.size();
    const std::size_t original_active_index = active_project_index_;
    const bool had_active_project = !project_root_.empty();
    for (std::size_t i = 0; i < project_count; ++i) {
      if (i >= projects_.size()) {
        break;
      }
      if (!SwitchProject(i, false)) {
        continue;
      }
      for (std::size_t index : DirtyEditorTabIndices()) {
        if (!SaveTab(index)) {
          LogMessage("Save failed");
          return;
        }
      }
    }
    if (had_active_project && original_active_index < projects_.size()) {
      SwitchProject(original_active_index, false);
    }
  }

  DismissDirtyPrompt(false);
  quit_requested_ = true;
  LogMessage(prompt.selected_action == 0 ? "Quit confirmed" : "Quit without saving");
}

std::array<std::string, 3> WorkspaceShell::DirtyPromptActionLabels() const {
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::Quit ||
      dirty_prompt_state_.kind == DirtyPromptState::Kind::CloseProject) {
    return {
        dirty_prompt_state_.dirty_count > 1 ? "Save all" : "Save",
        dirty_prompt_state_.dirty_count > 1 ? "Discard all" : "Discard",
        "Cancel",
    };
  }

  return {"Save", "Discard", "Cancel"};
}

std::string WorkspaceShell::DirtyPromptTitle() const {
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::Quit) {
    return "Unsaved changes before quit";
  }
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::CloseProject) {
    return "Unsaved changes before closing project";
  }
  return "Unsaved changes";
}

std::string WorkspaceShell::DirtyPromptMessage() const {
  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::Quit) {
    const std::size_t dirty_count = dirty_prompt_state_.dirty_count;
    return dirty_count == 1 ? "Save the dirty tab before quitting microide?"
                            : "Save the " + std::to_string(dirty_count) +
                                  " dirty tabs before quitting microide?";
  }

  if (dirty_prompt_state_.kind == DirtyPromptState::Kind::CloseProject) {
    const std::filesystem::path project_root =
        dirty_prompt_state_.project_index < projects_.size() &&
                projects_[dirty_prompt_state_.project_index] != nullptr
            ? projects_[dirty_prompt_state_.project_index]->root
            : project_root_;
    const std::string label = ProjectLabelForRoot(project_root);
    return dirty_prompt_state_.dirty_count == 1
               ? "Save the dirty tab before closing " + label + "?"
               : "Save the " + std::to_string(dirty_prompt_state_.dirty_count) +
                     " dirty tabs before closing " + label + "?";
  }

  const std::size_t index = dirty_prompt_state_.tab_index;
  const std::string label = index < open_tabs_.size() ? open_tabs_[index].title : "this tab";
  return "Save changes to " + label + " before closing it?";
}

void WorkspaceShell::OpenPromptSurface(PromptSurfaceState::Action action,
                                       PromptSurfaceState::Kind kind,
                                       const std::filesystem::path& path,
                                       std::string input) {
  prompt_surface_visible_ = true;
  prompt_surface_previous_focus_ = focus_;
  prompt_surface_state_.kind = kind;
  prompt_surface_state_.action = action;
  prompt_surface_state_.path = path.lexically_normal();
  prompt_surface_state_.input = std::move(input);
  prompt_surface_state_.selected_button = 0;
  focus_ = FocusTarget::Overlay;
}

void WorkspaceShell::DismissPromptSurface(bool restore_focus) {
  prompt_surface_visible_ = false;
  prompt_surface_state_ = PromptSurfaceState{};
  if (restore_focus) {
    focus_ = prompt_surface_previous_focus_;
  }
}

std::string WorkspaceShell::PromptSurfaceTitle() const {
  switch (prompt_surface_state_.action) {
    case PromptSurfaceState::Action::CreateFile:
      return "New File";
    case PromptSurfaceState::Action::CreateDirectory:
      return "New Folder";
    case PromptSurfaceState::Action::RenamePath:
      return "Rename";
    case PromptSurfaceState::Action::DeletePath:
      return "Delete";
  }
  return "Prompt";
}

std::string WorkspaceShell::PromptSurfaceMessage() const {
  const std::string label =
      prompt_surface_state_.path == project_root_
          ? ProjectLabel()
          : RelativePathLabel(project_root_, prompt_surface_state_.path);
  switch (prompt_surface_state_.action) {
    case PromptSurfaceState::Action::CreateFile:
      return "Create inside " + (label.empty() ? ProjectLabel() : label) + ".";
    case PromptSurfaceState::Action::CreateDirectory:
      return "Create inside " + (label.empty() ? ProjectLabel() : label) + ".";
    case PromptSurfaceState::Action::RenamePath:
      return "Enter a new path for " + label + ".";
    case PromptSurfaceState::Action::DeletePath:
      return "Move " + label + " to trash?";
  }
  return {};
}

std::array<std::string, 2> WorkspaceShell::PromptSurfaceActionLabels() const {
  switch (prompt_surface_state_.action) {
    case PromptSurfaceState::Action::CreateFile:
      return {"Create File", "Cancel"};
    case PromptSurfaceState::Action::CreateDirectory:
      return {"Create Folder", "Cancel"};
    case PromptSurfaceState::Action::RenamePath:
      return {"Rename", "Cancel"};
    case PromptSurfaceState::Action::DeletePath:
      return {"Delete", "Cancel"};
  }
  return {"OK", "Cancel"};
}

std::filesystem::path WorkspaceShell::TreeMutationBasePath(ActionSource source) const {
  if (project_root_.empty()) {
    return {};
  }
  if (source == ActionSource::ContextMenu && tree_context_menu_.open &&
      tree_context_menu_.target == TreeContextTargetKind::Background) {
    return project_root_;
  }

  std::filesystem::path path = ResolveTreeActionPath(source);
  if (path.empty()) {
    return project_root_;
  }

  std::error_code error;
  if (std::filesystem::is_directory(path, error) && !error) {
    return path.lexically_normal();
  }
  return path.parent_path().lexically_normal();
}

bool WorkspaceShell::EditorTabReferencesPath(std::size_t tab_index,
                                             const std::filesystem::path& path) const {
  if (tab_index >= open_tabs_.size()) {
    return false;
  }
  const TabEntry& tab = open_tabs_[tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }

  for (const auto& view : tab.editor_state->views) {
    const editor::TextViewport& viewport =
        tab_index == active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id
            ? text_viewport_
            : view.viewport;
    if (!viewport.path().empty() &&
        PathEqualsOrWithin(viewport.path().lexically_normal(), path.lexically_normal())) {
      return true;
    }
  }
  return false;
}

bool WorkspaceShell::EditorTabHasDirtyPath(std::size_t tab_index,
                                           const std::filesystem::path& path) const {
  if (tab_index >= open_tabs_.size()) {
    return false;
  }
  const TabEntry& tab = open_tabs_[tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }

  for (const auto& view : tab.editor_state->views) {
    const editor::TextViewport& viewport =
        tab_index == active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id
            ? text_viewport_
            : view.viewport;
    if (viewport.path().empty() || !viewport.dirty()) {
      continue;
    }
    if (PathEqualsOrWithin(viewport.path().lexically_normal(), path.lexically_normal())) {
      return true;
    }
  }
  return false;
}

std::vector<std::size_t> WorkspaceShell::AffectedEditorTabIndices(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    if (EditorTabReferencesPath(i, path)) {
      indices.push_back(i);
    }
  }
  return indices;
}

std::vector<std::size_t> WorkspaceShell::AffectedCompareTabIndices(
    const std::filesystem::path& path) const {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    const TabEntry& tab = open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
      continue;
    }
    if (PathEqualsOrWithin(tab.compare->path.lexically_normal(), path.lexically_normal())) {
      indices.push_back(i);
    }
  }
  return indices;
}

bool WorkspaceShell::HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                               std::string* blocking_label) const {
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    if (!EditorTabHasDirtyPath(i, path)) {
      continue;
    }
    if (blocking_label != nullptr) {
      *blocking_label = open_tabs_[i].title;
    }
    return true;
  }
  return false;
}

void WorkspaceShell::RefreshProjectViewsAfterMutation(
    const std::filesystem::path& preferred_tree_path) {
  RefreshProjectFiles();
  if (!preferred_tree_path.empty() && std::filesystem::exists(preferred_tree_path)) {
    directory_tree_.SelectPath(preferred_tree_path);
  } else if (!project_root_.empty()) {
    directory_tree_.SelectPath(project_root_);
  }
  if (!project_search_query_.empty()) {
    RefreshProjectSearch();
  }
}

void WorkspaceShell::RetargetOpenTabsForRename(const std::filesystem::path& old_path,
                                               const std::filesystem::path& new_path) {
  if (ActiveTabIsEditor()) {
    SyncActiveEditorTab();
  }

  std::vector<std::size_t> compare_tabs_to_close;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    TabEntry& tab = open_tabs_[i];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      bool retargeted = false;
      for (auto& view : tab.editor_state->views) {
        if (view.viewport.path().empty() ||
            !PathEqualsOrWithin(view.viewport.path().lexically_normal(), old_path)) {
          continue;
        }
        const std::filesystem::path updated_path =
            ReplacePathPrefix(view.viewport.path(), old_path, new_path);
        view.viewport.SetPath(updated_path);
        if (i == active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id) {
          text_viewport_.SetPath(updated_path);
        }
        retargeted = true;
      }

      if (retargeted) {
        if (i == active_tab_index_) {
          SyncActiveEditorTabMetadata();
        } else if (auto* active_view =
                       FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
                   active_view != nullptr) {
          tab.path = active_view->path().lexically_normal();
          tab.title = EditorTabLabel(*active_view);
        }
      }
      continue;
    }

    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
        !PathEqualsOrWithin(tab.compare->path.lexically_normal(), old_path)) {
      continue;
    }

    const std::filesystem::path updated_path =
        ReplacePathPrefix(tab.compare->path, old_path, new_path);
    const project::GitCommitEntry commit{
        .hash = tab.compare->commit_hash,
        .short_hash = tab.compare->left_label,
        .subject = tab.compare->left_label,
    };
    auto rebuilt = BuildCompareTabEntry(updated_path, commit, tab.compare->selected_row);
    if (!rebuilt.has_value()) {
      compare_tabs_to_close.push_back(i);
      continue;
    }
    tab = std::move(*rebuilt);
  }

  std::sort(compare_tabs_to_close.rbegin(), compare_tabs_to_close.rend());
  for (std::size_t index : compare_tabs_to_close) {
    CloseTab(index);
  }

  if (!compare_picker_path_.empty() && PathEqualsOrWithin(compare_picker_path_, old_path)) {
    compare_picker_path_ = ReplacePathPrefix(compare_picker_path_, old_path, new_path);
    if (overlay_visible_ && overlay_mode_ == OverlayMode::CommitPicker) {
      overlay_visible_ = false;
      LogMessage("Compare picker closed after rename");
    }
  }
}

void WorkspaceShell::CloseOpenTabsForPath(const std::filesystem::path& path) {
  if (ActiveTabIsEditor()) {
    SyncActiveEditorTab();
  }

  std::vector<std::size_t> indices = AffectedEditorTabIndices(path);
  const std::vector<std::size_t> compare_indices = AffectedCompareTabIndices(path);
  indices.insert(indices.end(), compare_indices.begin(), compare_indices.end());
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  std::sort(indices.rbegin(), indices.rend());
  for (std::size_t index : indices) {
    CloseTab(index);
  }

  if (!compare_picker_path_.empty() && PathEqualsOrWithin(compare_picker_path_, path)) {
    compare_picker_path_.clear();
    compare_picker_query_.clear();
    compare_picker_commits_.clear();
    compare_picker_matches_.clear();
    if (overlay_visible_ && overlay_mode_ == OverlayMode::CommitPicker) {
      overlay_visible_ = false;
      LogMessage("Compare picker closed after delete");
    }
  }
}

void WorkspaceShell::ConfirmPromptSurface() {
  if (!prompt_surface_visible_) {
    return;
  }

  const PromptSurfaceState state = prompt_surface_state_;
  if (state.selected_button == 1) {
    const std::string title = PromptSurfaceTitle();
    DismissPromptSurface(true);
    LogMessage(title + " cancelled");
    return;
  }

  if (state.kind == PromptSurfaceState::Kind::TextInput) {
    if (state.input.empty()) {
      LogMessage("A path is required");
      return;
    }

    std::filesystem::path typed_path(state.input);
    if (typed_path.is_absolute()) {
      LogMessage("Enter a project-relative path");
      return;
    }

    std::filesystem::path destination;
    if (state.action == PromptSurfaceState::Action::RenamePath) {
      std::string blocking_label;
      if (HasDirtyEditorTabsForPath(state.path, &blocking_label)) {
        DismissPromptSurface(true);
        LogMessage("Rename blocked by dirty tab: " + blocking_label);
        return;
      }
      destination = (state.path.parent_path() / typed_path).lexically_normal();
    } else {
      destination = (state.path / typed_path).lexically_normal();
    }

    if (!PathEqualsOrWithin(destination, project_root_)) {
      LogMessage("Path must stay inside the current project");
      return;
    }

    project::FileOperationResult result;
    if (state.action == PromptSurfaceState::Action::CreateFile) {
      result = project::FileOperationService::CreateFile(destination);
    } else if (state.action == PromptSurfaceState::Action::CreateDirectory) {
      result = project::FileOperationService::CreateDirectory(destination);
    } else {
      result = project::FileOperationService::RenamePath(state.path, destination);
    }

    if (!result.ok) {
      LogMessage(result.error_message);
      return;
    }

    DismissPromptSurface(false);
    if (state.action == PromptSurfaceState::Action::CreateFile) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      OpenFile(result.resulting_path);
      LogMessage("Created file: " + RelativePathLabel(project_root_, result.resulting_path));
      return;
    }
    if (state.action == PromptSurfaceState::Action::CreateDirectory) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      focus_ = FocusTarget::Sidebar;
      LogMessage("Created folder: " + RelativePathLabel(project_root_, result.resulting_path));
      return;
    }

    RetargetOpenTabsForRename(state.path, result.resulting_path);
    RefreshProjectViewsAfterMutation(result.resulting_path);
    LogMessage("Renamed: " + RelativePathLabel(project_root_, result.resulting_path));
    return;
  }

  std::string blocking_label;
  if (HasDirtyEditorTabsForPath(state.path, &blocking_label)) {
    DismissPromptSurface(true);
    LogMessage("Delete blocked by dirty tab: " + blocking_label);
    return;
  }

  const project::FileOperationResult result = project::FileOperationService::TrashPath(state.path);
  if (!result.ok) {
    LogMessage(result.error_message);
    return;
  }

  const std::filesystem::path parent = state.path.parent_path();
  DismissPromptSurface(false);
  CloseOpenTabsForPath(state.path);
  RefreshProjectViewsAfterMutation(parent);
  focus_ = FocusTarget::Sidebar;
  LogMessage("Moved to trash: " + RelativePathLabel(project_root_, state.path));
}

bool WorkspaceShell::ActiveTabIsEditor() const {
  return active_tab_index_ < open_tabs_.size() &&
         open_tabs_[active_tab_index_].kind == TabEntry::Kind::Editor &&
         open_tabs_[active_tab_index_].editor_state.has_value();
}

WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].editor_state.value();
}

const WorkspaceShell::TabEntry::EditorTabState* WorkspaceShell::ActiveEditorTab() const {
  if (!ActiveTabIsEditor()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].editor_state.value();
}

WorkspaceShell::TabEntry::EditorTabState WorkspaceShell::MakeEditorTabState(
    const editor::TextViewport& view) {
  TabEntry::EditorTabState state;
  state.views.push_back(TabEntry::EditorTabState::EditorViewState{
      .leaf_id = 1,
      .viewport = view,
  });
  state.active_leaf_id = 1;
  state.next_leaf_id = 2;
  state.split_root = MakeEditorLeafNode(1);
  return state;
}

std::unique_ptr<WorkspaceShell::TabEntry::EditorTabState::EditorSplitNode>
WorkspaceShell::MakeEditorLeafNode(std::size_t leaf_id, float size_fraction) {
  auto leaf = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
  leaf->leaf_id = leaf_id;
  leaf->orientation = EditorSplitOrientation::None;
  leaf->size_fraction = size_fraction;
  return leaf;
}

void WorkspaceShell::SyncActiveEditorTabMetadata() {
  if (active_tab_index_ >= open_tabs_.size()) {
    return;
  }

  auto& tab = open_tabs_[active_tab_index_];
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }

  const std::filesystem::path active_path = text_viewport_.path().lexically_normal();
  tab.path = active_path;
  tab.title = EditorTabLabel(text_viewport_);
  if (!active_path.empty()) {
    directory_tree_.SelectPath(active_path);
  }
}

bool WorkspaceShell::ReplaceActiveEditorView(const editor::TextViewport& viewport) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return false;
  }

  editor::TextViewport configured_view = viewport;
  ApplyEditorPreferences(configured_view);

  NormalizeEditorSplitTree(*editor_tab);
  if (auto* active_view = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
      active_view != nullptr) {
    *active_view = configured_view;
    text_viewport_ = configured_view;
    SyncActiveEditorTabMetadata();
    ResetCaretBlink();
    return true;
  }
  return false;
}

editor::TextViewport* WorkspaceShell::FindEditorView(TabEntry::EditorTabState& editor_tab,
                                                     std::size_t leaf_id) {
  auto it = std::find_if(editor_tab.views.begin(), editor_tab.views.end(), [&](const auto& view) {
    return view.leaf_id == leaf_id;
  });
  return it == editor_tab.views.end() ? nullptr : &it->viewport;
}

const editor::TextViewport* WorkspaceShell::FindEditorView(
    const TabEntry::EditorTabState& editor_tab,
    std::size_t leaf_id) const {
  auto it = std::find_if(editor_tab.views.begin(), editor_tab.views.end(), [&](const auto& view) {
    return view.leaf_id == leaf_id;
  });
  return it == editor_tab.views.end() ? nullptr : &it->viewport;
}

WorkspaceShell::EditorSplitSlot WorkspaceShell::FindEditorLeafSlot(
    TabEntry::EditorTabState& editor_tab,
    std::size_t leaf_id) {
  EditorSplitSlot result;
  const auto find_slot =
      [&](auto&& self,
          std::unique_ptr<TabEntry::EditorTabState::EditorSplitNode>* slot,
          TabEntry::EditorTabState::EditorSplitNode* parent,
          std::size_t index) -> bool {
    if (slot == nullptr || slot->get() == nullptr) {
      return false;
    }

    auto* node = slot->get();
    if (node->IsLeaf()) {
      if (node->leaf_id != leaf_id) {
        return false;
      }
      result.parent = parent;
      result.index = index;
      result.slot = slot;
      return true;
    }

    for (std::size_t child_index = 0; child_index < node->children.size(); ++child_index) {
      if (self(self, &node->children[child_index], node, child_index)) {
        return true;
      }
    }
    return false;
  };
  find_slot(find_slot, &editor_tab.split_root, nullptr, 0);
  return result;
}

WorkspaceShell::TabEntry::EditorTabState::EditorSplitNode* WorkspaceShell::FindEditorSplitNode(
    TabEntry::EditorTabState::EditorSplitNode* node,
    const std::vector<std::size_t>& path) {
  auto* current = node;
  for (std::size_t index : path) {
    if (current == nullptr || index >= current->children.size()) {
      return nullptr;
    }
    current = current->children[index].get();
  }
  return current;
}

const WorkspaceShell::TabEntry::EditorTabState::EditorSplitNode*
WorkspaceShell::FindEditorSplitNode(const TabEntry::EditorTabState::EditorSplitNode* node,
                                    const std::vector<std::size_t>& path) const {
  auto* current = node;
  for (std::size_t index : path) {
    if (current == nullptr || index >= current->children.size()) {
      return nullptr;
    }
    current = current->children[index].get();
  }
  return current;
}

void WorkspaceShell::NormalizeEditorSplitNode(TabEntry::EditorTabState::EditorSplitNode& node) {
  if (node.IsLeaf()) {
    node.orientation = EditorSplitOrientation::None;
    node.size_fraction = std::max(0.0f, node.size_fraction);
    return;
  }

  float total = 0.0f;
  for (auto& child : node.children) {
    NormalizeEditorSplitNode(*child);
    child->size_fraction = std::max(0.0f, child->size_fraction);
    total += child->size_fraction;
  }

  if (total <= 0.0f) {
    const float even_fraction = node.children.empty() ? 1.0f : 1.0f / node.children.size();
    for (auto& child : node.children) {
      child->size_fraction = even_fraction;
    }
  } else {
    for (auto& child : node.children) {
      child->size_fraction /= total;
    }
  }
}

void WorkspaceShell::NormalizeEditorSplitTree(TabEntry::EditorTabState& editor_tab) {
  if (editor_tab.views.empty()) {
    editor_tab.active_leaf_id = 0;
    editor_tab.next_leaf_id = 1;
    editor_tab.split_root.reset();
    return;
  }

  if (editor_tab.split_root == nullptr) {
    editor_tab.split_root = MakeEditorLeafNode(editor_tab.views.front().leaf_id);
  }

  while (editor_tab.split_root != nullptr && !editor_tab.split_root->IsLeaf() &&
         editor_tab.split_root->children.size() == 1) {
    editor_tab.split_root = std::move(editor_tab.split_root->children.front());
  }

  if (editor_tab.split_root != nullptr) {
    editor_tab.split_root->size_fraction = 1.0f;
    NormalizeEditorSplitNode(*editor_tab.split_root);
  }

  std::vector<std::size_t> leaf_ids = EditorLeafOrder(editor_tab);
  if (leaf_ids.empty()) {
    editor_tab.split_root = MakeEditorLeafNode(editor_tab.views.front().leaf_id);
    leaf_ids = EditorLeafOrder(editor_tab);
  }

  const auto active_it =
      std::find(leaf_ids.begin(), leaf_ids.end(), editor_tab.active_leaf_id);
  if (active_it == leaf_ids.end()) {
    editor_tab.active_leaf_id = leaf_ids.front();
  }

  std::size_t next_leaf_id = 1;
  for (const auto& view : editor_tab.views) {
    next_leaf_id = std::max(next_leaf_id, view.leaf_id + 1);
  }
  editor_tab.next_leaf_id = next_leaf_id;
}

void WorkspaceShell::CollectEditorLeafOrder(
    const TabEntry::EditorTabState::EditorSplitNode* node,
    std::vector<std::size_t>& order) const {
  if (node == nullptr) {
    return;
  }
  if (node->IsLeaf()) {
    order.push_back(node->leaf_id);
    return;
  }
  for (const auto& child : node->children) {
    CollectEditorLeafOrder(child.get(), order);
  }
}

std::vector<std::size_t> WorkspaceShell::EditorLeafOrder(
    const TabEntry::EditorTabState& editor_tab) const {
  std::vector<std::size_t> order;
  CollectEditorLeafOrder(editor_tab.split_root.get(), order);
  return order;
}

void WorkspaceShell::SetActiveEditorSplit(std::size_t index) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return;
  }

  NormalizeEditorSplitTree(*editor_tab);
  if (auto* current_view = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
      current_view != nullptr) {
    *current_view = text_viewport_;
  }

  if (auto* target_view = FindEditorView(*editor_tab, index); target_view != nullptr) {
    editor_tab->active_leaf_id = index;
    text_viewport_ = *target_view;
    SyncActiveEditorTabMetadata();
    ResetCaretBlink();
  }
  focus_ = FocusTarget::Editor;
}

bool WorkspaceShell::ActivateOrderedEditorSplit(std::size_t order_index) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2) {
    return false;
  }

  NormalizeEditorSplitTree(*editor_tab);
  const std::vector<std::size_t> leaf_order = EditorLeafOrder(*editor_tab);
  if (order_index >= leaf_order.size()) {
    return false;
  }

  SetActiveEditorSplit(leaf_order[order_index]);
  return true;
}

bool WorkspaceShell::SplitActiveEditor(EditorSplitOrientation orientation) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || orientation == EditorSplitOrientation::None) {
    return false;
  }

  if (editor_tab->views.empty()) {
    *editor_tab = MakeEditorTabState(text_viewport_);
  }

  NormalizeEditorSplitTree(*editor_tab);
  SyncActiveEditorTab();
  auto* active_view = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
  if (active_view == nullptr) {
    return false;
  }

  const std::size_t new_leaf_id = editor_tab->next_leaf_id++;
  editor_tab->views.push_back(TabEntry::EditorTabState::EditorViewState{
      .leaf_id = new_leaf_id,
      .viewport = *active_view,
  });

  EditorSplitSlot active_slot = FindEditorLeafSlot(*editor_tab, editor_tab->active_leaf_id);
  if (active_slot.slot == nullptr || active_slot.slot->get() == nullptr) {
    editor_tab->views.pop_back();
    return false;
  }

  if (active_slot.parent != nullptr && active_slot.parent->orientation == orientation) {
    auto sibling = MakeEditorLeafNode(new_leaf_id, (*active_slot.slot)->size_fraction * 0.5f);
    (*active_slot.slot)->size_fraction *= 0.5f;
    active_slot.parent->children.insert(
        active_slot.parent->children.begin() +
            static_cast<std::ptrdiff_t>(active_slot.index + 1),
        std::move(sibling));
    NormalizeEditorSplitNode(*active_slot.parent);
  } else {
    const float branch_fraction =
        std::max(0.0f, (*active_slot.slot)->size_fraction);
    auto group = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
    group->orientation = orientation;
    group->size_fraction = branch_fraction > 0.0f ? branch_fraction : 1.0f;
    (*active_slot.slot)->size_fraction = 0.5f;
    group->children.push_back(std::move(*active_slot.slot));
    group->children.push_back(MakeEditorLeafNode(new_leaf_id, 0.5f));
    NormalizeEditorSplitNode(*group);
    *active_slot.slot = std::move(group);
  }

  NormalizeEditorSplitTree(*editor_tab);
  editor_tab->active_leaf_id = new_leaf_id;
  if (auto* new_view = FindEditorView(*editor_tab, new_leaf_id); new_view != nullptr) {
    text_viewport_ = *new_view;
  }
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  return true;
}

bool WorkspaceShell::UnsplitActiveEditor() {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2) {
    return false;
  }

  SyncActiveEditorTab();
  auto* active_view = FindEditorView(*editor_tab, editor_tab->active_leaf_id);
  if (active_view == nullptr) {
    return false;
  }

  const editor::TextViewport preserved_view = *active_view;
  *editor_tab = MakeEditorTabState(preserved_view);
  text_viewport_ = preserved_view;
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  return true;
}

bool WorkspaceShell::CycleEditorSplit(int delta) {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2 || delta == 0) {
    return false;
  }

  NormalizeEditorSplitTree(*editor_tab);
  const std::vector<std::size_t> leaf_order = EditorLeafOrder(*editor_tab);
  if (leaf_order.size() < 2) {
    return false;
  }

  const int size = static_cast<int>(leaf_order.size());
  const auto current_it =
      std::find(leaf_order.begin(), leaf_order.end(), editor_tab->active_leaf_id);
  const int current =
      current_it == leaf_order.end() ? 0 : static_cast<int>(current_it - leaf_order.begin());
  const int next = (current + delta % size + size) % size;
  SetActiveEditorSplit(leaf_order[static_cast<std::size_t>(next)]);
  return true;
}

void WorkspaceShell::CollectEditorPaneLayouts(
    const TabEntry::EditorTabState& editor_tab,
    const TabEntry::EditorTabState::EditorSplitNode* node,
    const SDL_FRect& rect,
    std::vector<EditorPaneLayout>& panes,
    std::vector<EditorSplitDividerLayout>* dividers,
    std::vector<std::size_t>* path) const {
  if (node == nullptr) {
    return;
  }

  if (node->IsLeaf() || node->orientation == EditorSplitOrientation::None ||
      node->children.empty()) {
    panes.push_back(EditorPaneLayout{
        .leaf_id = node->leaf_id,
        .rect = rect,
        .active = node->leaf_id == editor_tab.active_leaf_id,
    });
    return;
  }

  const bool vertical = node->orientation == EditorSplitOrientation::Vertical;
  const std::size_t child_count = node->children.size();
  const float total_extent =
      std::max(0.0f,
               (vertical ? rect.w : rect.h) -
                   kEditorSplitDividerThickness * static_cast<float>(child_count - 1));
  std::vector<float> weights(child_count, 0.0f);
  float total_weight = 0.0f;
  for (std::size_t i = 0; i < child_count; ++i) {
    weights[i] = std::max(0.0f, node->children[i]->size_fraction);
    total_weight += weights[i];
  }
  if (total_weight <= 0.0f) {
    std::fill(weights.begin(), weights.end(), 1.0f);
    total_weight = static_cast<float>(weights.size());
  }

  float cursor = vertical ? rect.x : rect.y;
  float remaining_extent = total_extent;
  float remaining_weight = total_weight;
  for (std::size_t i = 0; i < child_count; ++i) {
    const std::size_t remaining_children = child_count - i;
    float child_extent = remaining_children == 1
                             ? remaining_extent
                             : std::floor(remaining_weight > 0.0f
                                              ? remaining_extent * (weights[i] / remaining_weight)
                                              : remaining_extent /
                                                    static_cast<float>(remaining_children));
    if (remaining_extent > kMinSplitPaneExtent * static_cast<float>(remaining_children)) {
      child_extent = std::clamp(
          child_extent, kMinSplitPaneExtent,
          remaining_extent -
              kMinSplitPaneExtent * static_cast<float>(remaining_children - 1));
    }

    const SDL_FRect child_rect =
        vertical ? MakeRect(cursor, rect.y, std::max(0.0f, child_extent), rect.h)
                 : MakeRect(rect.x, cursor, rect.w, std::max(0.0f, child_extent));
    if (path != nullptr) {
      path->push_back(i);
    }
    CollectEditorPaneLayouts(editor_tab, node->children[i].get(), child_rect, panes, dividers,
                             path);
    if (path != nullptr) {
      path->pop_back();
    }

    cursor += child_extent;
    remaining_extent = std::max(0.0f, remaining_extent - child_extent);
    remaining_weight = std::max(0.0f, remaining_weight - weights[i]);

    if (i + 1 < child_count) {
      if (dividers != nullptr && path != nullptr) {
        dividers->push_back(EditorSplitDividerLayout{
            .node_path = *path,
            .divider_index = i,
            .rect = vertical ? MakeRect(cursor, rect.y, kEditorSplitDividerThickness, rect.h)
                             : MakeRect(rect.x, cursor, rect.w, kEditorSplitDividerThickness),
        });
      }
      cursor += kEditorSplitDividerThickness;
    }
  }
}

std::optional<SDL_FRect> WorkspaceShell::ComputeEditorSplitNodeRect(
    const SDL_FRect& editor_surface,
    const std::vector<std::size_t>& path) const {
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->split_root == nullptr) {
    return std::nullopt;
  }

  const auto compute_rect =
      [&](auto&& self,
          const TabEntry::EditorTabState::EditorSplitNode* node,
          const SDL_FRect& rect,
          std::size_t depth) -> std::optional<SDL_FRect> {
    if (node == nullptr) {
      return std::nullopt;
    }
    if (depth >= path.size()) {
      return rect;
    }
    if (node->IsLeaf() || node->orientation == EditorSplitOrientation::None ||
        node->children.empty()) {
      return std::nullopt;
    }

    const std::size_t child_index = path[depth];
    if (child_index >= node->children.size()) {
      return std::nullopt;
    }

    const bool vertical = node->orientation == EditorSplitOrientation::Vertical;
    const std::size_t child_count = node->children.size();
    const float total_extent =
        std::max(0.0f,
                 (vertical ? rect.w : rect.h) -
                     kEditorSplitDividerThickness * static_cast<float>(child_count - 1));
    std::vector<float> weights(child_count, 0.0f);
    float total_weight = 0.0f;
    for (std::size_t i = 0; i < child_count; ++i) {
      weights[i] = std::max(0.0f, node->children[i]->size_fraction);
      total_weight += weights[i];
    }
    if (total_weight <= 0.0f) {
      std::fill(weights.begin(), weights.end(), 1.0f);
      total_weight = static_cast<float>(weights.size());
    }

    float cursor = vertical ? rect.x : rect.y;
    float remaining_extent = total_extent;
    float remaining_weight = total_weight;
    for (std::size_t i = 0; i < child_count; ++i) {
      const std::size_t remaining_children = child_count - i;
      float child_extent = remaining_children == 1
                               ? remaining_extent
                               : std::floor(remaining_weight > 0.0f
                                                ? remaining_extent * (weights[i] / remaining_weight)
                                                : remaining_extent /
                                                      static_cast<float>(remaining_children));
      if (remaining_extent > kMinSplitPaneExtent * static_cast<float>(remaining_children)) {
        child_extent = std::clamp(
            child_extent, kMinSplitPaneExtent,
            remaining_extent -
                kMinSplitPaneExtent * static_cast<float>(remaining_children - 1));
      }

      const SDL_FRect child_rect =
          vertical ? MakeRect(cursor, rect.y, std::max(0.0f, child_extent), rect.h)
                   : MakeRect(rect.x, cursor, rect.w, std::max(0.0f, child_extent));
      if (i == child_index) {
        return self(self, node->children[i].get(), child_rect, depth + 1);
      }

      cursor += child_extent + kEditorSplitDividerThickness;
      remaining_extent = std::max(0.0f, remaining_extent - child_extent);
      remaining_weight = std::max(0.0f, remaining_weight - weights[i]);
    }
    return std::nullopt;
  };

  return compute_rect(compute_rect, editor_tab->split_root.get(), editor_surface, 0);
}

std::vector<WorkspaceShell::EditorPaneLayout> WorkspaceShell::ComputeEditorPaneLayouts(
    const SDL_FRect& editor_surface) const {
  std::vector<EditorPaneLayout> panes;
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.empty()) {
    return panes;
  }

  CollectEditorPaneLayouts(*editor_tab, editor_tab->split_root.get(), editor_surface, panes,
                           nullptr, nullptr);
  return panes;
}

std::vector<WorkspaceShell::EditorSplitDividerLayout>
WorkspaceShell::ComputeEditorSplitDividerLayouts(
    const SDL_FRect& editor_surface) const {
  std::vector<EditorSplitDividerLayout> dividers;
  const auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr || editor_tab->views.size() < 2 || editor_tab->split_root == nullptr) {
    return dividers;
  }

  std::vector<std::size_t> path;
  std::vector<EditorPaneLayout> ignored_panes;
  CollectEditorPaneLayouts(*editor_tab, editor_tab->split_root.get(), editor_surface, ignored_panes,
                           &dividers, &path);
  return dividers;
}

void WorkspaceShell::RequestCloseTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  if (TabIsDirty(index)) {
    ShowDirtyPromptForTab(index);
    return;
  }

  CloseTab(index);
}

void WorkspaceShell::CloseTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return;
  }

  if (active_tab_index_ < open_tabs_.size() && index != active_tab_index_) {
    SyncActiveEditorTab();
  }

  const std::string closed_title = open_tabs_[index].title;
  open_tabs_.erase(open_tabs_.begin() + static_cast<std::ptrdiff_t>(index));

  if (open_tabs_.empty()) {
    active_tab_index_ = 0;
    tab_scroll_index_ = 0;
    text_viewport_.SetPlaceholderText(
        "microide\n\n"
        "Project loaded.\n"
        "Use the sidebar to open files.\n");
    focus_ = FocusTarget::Editor;
    LogMessage("Closed tab: " + closed_title);
    return;
  }

  if (index < active_tab_index_) {
    --active_tab_index_;
  } else if (index == active_tab_index_) {
    active_tab_index_ = std::min(index, open_tabs_.size() - 1);
    auto& tab = open_tabs_[active_tab_index_];
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value() &&
        !tab.editor_state->views.empty()) {
      NormalizeEditorSplitTree(*tab.editor_state);
      if (auto* active_view = FindEditorView(*tab.editor_state, tab.editor_state->active_leaf_id);
          active_view != nullptr) {
        text_viewport_ = *active_view;
        ApplyEditorPreferences(text_viewport_);
      }
    } else if (tab.kind == TabEntry::Kind::Editor) {
      editor::TextViewport loaded_view;
      if (!loaded_view.OpenFile(tab.path)) {
        LogMessage("Failed to open file: " + tab.path.lexically_normal().string());
      } else {
        ApplyEditorPreferences(loaded_view);
        text_viewport_ = loaded_view;
        tab.editor_state = MakeEditorTabState(loaded_view);
      }
    }
    if (!tab.path.empty()) {
      directory_tree_.SelectPath(tab.path);
    }
    focus_ = FocusTarget::Editor;
  }

  tab_scroll_index_ =
      std::clamp(tab_scroll_index_, 0, std::max(0, static_cast<int>(open_tabs_.size()) - 1));
  EnsureActiveTabVisible();
  LogMessage("Closed tab: " + closed_title);
}

void WorkspaceShell::ShowSidebarMode(SidebarMode mode, bool temporary) {
  if (mode == SidebarMode::None) {
    CloseSidebar();
    return;
  }
  if (mode != SidebarMode::Tree) {
    CloseTreeContextMenu();
  }

  if (sidebar_mode_ == SidebarMode::Search && mode != SidebarMode::Search) {
    StopProjectSearch();
  }

  if (temporary) {
    if (!sidebar_temporary_ && sidebar_visible_) {
      sidebar_prev_mode_ = sidebar_mode_;
    }
  } else {
    sidebar_prev_mode_ = SidebarMode::None;
  }

  sidebar_mode_ = mode;
  sidebar_temporary_ = temporary;
  sidebar_visible_ = true;
  focus_ = FocusTarget::Sidebar;
  sidebar_scroll_row_ = 0;
}

void WorkspaceShell::ShowTreeSidebar(const std::filesystem::path& root) {
  if (!root.empty()) {
    if (!OpenProjectTab(root, true, true)) {
      return;
    }
  }

  ShowSidebarMode(SidebarMode::Tree, false);
}

void WorkspaceShell::ShowSearchSidebar(std::string query, bool temporary) {
  project_search_query_ = std::move(query);
  project_search_edit_buffer_ = project_search_query_;
  project_search_editing_ = project_search_query_.empty();
  project_search_edit_field_ = ProjectSearchEditField::Query;
  project_search_selected_index_ = 0;
  RefreshProjectSearch();
  ShowSidebarMode(SidebarMode::Search, temporary);
  LogMessage(temporary ? "Temporary project search opened" : "Project search sidebar opened");
}

void WorkspaceShell::CloseSidebar() {
  if (sidebar_mode_ == SidebarMode::Search) {
    StopProjectSearch();
  }
  CloseTreeContextMenu();

  if (sidebar_temporary_ && sidebar_prev_mode_ != SidebarMode::None) {
    RestorePreviousSidebar();
    LogMessage("Previous sidebar restored");
    return;
  }

  sidebar_visible_ = false;
  sidebar_temporary_ = false;
  sidebar_prev_mode_ = SidebarMode::None;
  if (focus_ == FocusTarget::Sidebar) {
    focus_ = FocusTarget::Editor;
  }
  LogMessage("Sidebar closed");
}

void WorkspaceShell::ToggleSidebar() {
  if (sidebar_visible_) {
    CloseSidebar();
    return;
  }

  if (sidebar_mode_ == SidebarMode::None) {
    sidebar_mode_ = SidebarMode::Tree;
  }
  sidebar_visible_ = true;
  sidebar_temporary_ = false;
  focus_ = FocusTarget::Sidebar;
  LogMessage("Sidebar shown");
}

void WorkspaceShell::RestorePreviousSidebar() {
  if (sidebar_mode_ == SidebarMode::Search && sidebar_prev_mode_ != SidebarMode::Search) {
    StopProjectSearch();
  }

  if (sidebar_prev_mode_ == SidebarMode::None) {
    sidebar_temporary_ = false;
    return;
  }

  sidebar_mode_ = sidebar_prev_mode_;
  sidebar_prev_mode_ = SidebarMode::None;
  sidebar_temporary_ = false;
  sidebar_visible_ = true;
  focus_ = FocusTarget::Sidebar;
  sidebar_scroll_row_ = 0;
}

void WorkspaceShell::RefreshProjectFiles() {
  directory_tree_.Refresh();
  file_index_.Refresh();
  file_finder_.SetIndex(&file_index_);
}

void WorkspaceShell::OpenComparePicker() {
  if (!sidebar_visible_ || sidebar_mode_ != SidebarMode::Tree) {
    return;
  }

  const auto& entries = directory_tree_.entries();
  if (directory_tree_.selected_index() >= entries.size()) {
    return;
  }

  const auto& entry = entries[directory_tree_.selected_index()];
  if (entry.is_directory) {
    return;
  }

  OpenComparePickerForPath(entry.path);
}

bool WorkspaceShell::OpenComparePickerForPath(const std::filesystem::path& path,
                                              std::string_view commit_spec) {
  if (project_root_.empty()) {
    LogMessage("No project is loaded");
    return false;
  }
  if (path.empty()) {
    LogMessage("No file is available for compare");
    return false;
  }

  compare_picker_path_ = path.lexically_normal();
  compare_picker_query_.clear();
  compare_picker_commits_ = project::CollectGitFileHistory(project_root_, compare_picker_path_);
  RefreshComparePicker();
  if (compare_picker_matches_.empty()) {
    LogMessage("No git history available for file");
    return false;
  }

  if (!commit_spec.empty()) {
    const std::string lowered_commit_spec = ToLower(commit_spec);
    std::vector<std::size_t> matching_indices;
    for (std::size_t i = 0; i < compare_picker_matches_.size(); ++i) {
      const auto& commit = compare_picker_matches_[i];
      const std::string lowered_hash = ToLower(commit.hash);
      const std::string lowered_short_hash = ToLower(commit.short_hash);
      if (StartsWith(lowered_hash, lowered_commit_spec) ||
          StartsWith(lowered_short_hash, lowered_commit_spec)) {
        matching_indices.push_back(i);
      }
    }

    if (matching_indices.empty()) {
      LogMessage("No compare commit matches: " + std::string(commit_spec));
      return false;
    }
    if (matching_indices.size() > 1) {
      LogMessage("Compare commit is ambiguous: " + std::string(commit_spec));
      return false;
    }

    compare_picker_selected_index_ = matching_indices.front();
    OpenSelectedCompareCommit();
    return true;
  }

  overlay_visible_ = true;
  overlay_mode_ = OverlayMode::CommitPicker;
  focus_ = FocusTarget::Overlay;
  ResetOverlayScroll();
  LogMessage("Compare picker opened");
  return true;
}

std::optional<WorkspaceShell::TabEntry> WorkspaceShell::BuildCompareTabEntry(
    const std::filesystem::path& path,
    const project::GitCommitEntry& commit,
    std::size_t selected_row) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const auto content =
      project::ReadGitFileAtCommit(project_root_, normalized_path, commit.hash);
  if (!content.has_value()) {
    return std::nullopt;
  }

  std::ifstream working_file(normalized_path);
  if (!working_file) {
    return std::nullopt;
  }
  std::ostringstream working_stream;
  working_stream << working_file.rdbuf();

  CompareTabState compare_tab;
  compare_tab.path = normalized_path;
  compare_tab.title = "compare: " + normalized_path.filename().string();
  compare_tab.commit_hash = commit.hash;
  compare_tab.left_label = commit.short_hash;
  compare_tab.right_label = "Working tree";
  const std::string working_content = working_stream.str();
  compare_tab.left_initial_syntax_state =
      editor::SyntaxHighlighter::InitialState(normalized_path, SplitSyntaxLines(content->content));
  compare_tab.right_initial_syntax_state =
      editor::SyntaxHighlighter::InitialState(normalized_path, SplitSyntaxLines(working_content));
  compare_tab.model = compare::BuildCompareModel(content->content, working_content);
  compare_tab.left_tokens_by_row.reserve(compare_tab.model.rows.size());
  compare_tab.right_tokens_by_row.reserve(compare_tab.model.rows.size());
  editor::SyntaxState left_state = compare_tab.left_initial_syntax_state;
  editor::SyntaxState right_state = compare_tab.right_initial_syntax_state;
  for (const auto& compare_row : compare_tab.model.rows) {
    const bool reuse_tokens =
        compare_row.kind == compare::CompareRowKind::Unchanged && compare_row.left_line > 0 &&
        compare_row.right_line > 0 && compare_row.left_text == compare_row.right_text &&
        left_state.definition_id == right_state.definition_id &&
        left_state.region_id == right_state.region_id;
    if (reuse_tokens) {
      editor::HighlightedLine highlighted =
          editor::SyntaxHighlighter::HighlightLine(compare_row.left_text, normalized_path, left_state);
      left_state = highlighted.end_state;
      right_state = highlighted.end_state;
      compare_tab.left_tokens_by_row.push_back(highlighted.tokens);
      compare_tab.right_tokens_by_row.push_back(std::move(highlighted.tokens));
      continue;
    }

    if (compare_row.left_line > 0) {
      editor::HighlightedLine highlighted =
          editor::SyntaxHighlighter::HighlightLine(compare_row.left_text, normalized_path, left_state);
      left_state = highlighted.end_state;
      compare_tab.left_tokens_by_row.push_back(std::move(highlighted.tokens));
    } else {
      compare_tab.left_tokens_by_row.push_back({});
    }

    if (compare_row.right_line > 0) {
      editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(
          compare_row.right_text, normalized_path, right_state);
      right_state = highlighted.end_state;
      compare_tab.right_tokens_by_row.push_back(std::move(highlighted.tokens));
    } else {
      compare_tab.right_tokens_by_row.push_back({});
    }
  }
  compare_tab.selected_row =
      compare_tab.model.rows.empty()
          ? 0
          : std::min(selected_row, compare_tab.model.rows.size() - 1);
  compare_tab.scroll_row = 0;

  return TabEntry{
      .kind = TabEntry::Kind::Compare,
      .path = normalized_path,
      .title = compare_tab.title,
      .editor_state = std::nullopt,
      .compare = std::move(compare_tab),
  };
}

void WorkspaceShell::RefreshComparePicker() {
  compare_picker_matches_.clear();
  compare_picker_selected_index_ = 0;

  const std::string lowered_query = ToLower(compare_picker_query_);
  for (const auto& commit : compare_picker_commits_) {
    if (!lowered_query.empty()) {
      const std::string text = ToLower(commit.short_hash + " " + commit.subject);
      if (text.find(lowered_query) == std::string::npos) {
        continue;
      }
    }
    compare_picker_matches_.push_back(commit);
  }
  ResetOverlayScroll();
}

void WorkspaceShell::MoveComparePickerSelection(int delta) {
  if (compare_picker_matches_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(compare_picker_selected_index_);
  const int max_index = static_cast<int>(compare_picker_matches_.size()) - 1;
  compare_picker_selected_index_ =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  if (overlay_visible_ && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::MoveFileFinderSelection(int delta) {
  file_finder_.MoveSelection(delta);
  if (overlay_visible_ && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::OpenSelectedCompareCommit() {
  if (compare_picker_matches_.empty() ||
      compare_picker_selected_index_ >= compare_picker_matches_.size()) {
    return;
  }

  OpenComparison(compare_picker_matches_[compare_picker_selected_index_]);
}

void WorkspaceShell::OpenComparison(const project::GitCommitEntry& commit) {
  auto compare_tab = BuildCompareTabEntry(compare_picker_path_, commit);
  if (!compare_tab.has_value()) {
    LogMessage("Failed to read file content at selected commit");
    return;
  }

  SyncActiveEditorTab();
  open_tabs_.push_back(std::move(*compare_tab));
  active_tab_index_ = open_tabs_.size() - 1;
  RevealActiveCompareSelection();
  EnsureActiveTabVisible();
  overlay_visible_ = false;
  focus_ = FocusTarget::Editor;
  LogMessage("Comparison opened");
}

void WorkspaceShell::OpenWorkingFileFromCompare() {
  const CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->model.rows.empty()) {
    return;
  }

  const auto& row = compare_tab->model.rows[compare_tab->selected_row];
  int target_line = row.right_line;
  if (target_line == 0) {
    for (std::size_t i = compare_tab->selected_row + 1; i < compare_tab->model.rows.size(); ++i) {
      if (compare_tab->model.rows[i].right_line > 0) {
        target_line = compare_tab->model.rows[i].right_line;
        break;
      }
    }
  }
  if (target_line == 0) {
    for (std::size_t i = compare_tab->selected_row; i-- > 0;) {
      if (compare_tab->model.rows[i].right_line > 0) {
        target_line = compare_tab->model.rows[i].right_line;
        break;
      }
    }
  }

  OpenFile(compare_tab->path);
  if (target_line > 0) {
    text_viewport_.MoveCursorTo(static_cast<std::size_t>(target_line - 1), 0);
  }
}

void WorkspaceShell::MoveCompareSelection(int delta) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->model.rows.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(compare_tab->selected_row);
  const int max_index = static_cast<int>(compare_tab->model.rows.size()) - 1;
  compare_tab->selected_row = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealActiveCompareSelection();
}

void WorkspaceShell::JumpCompareHunk(int delta) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || compare_tab->model.hunks.empty()) {
    return;
  }

  int target = 0;
  for (std::size_t i = 0; i < compare_tab->model.hunks.size(); ++i) {
    if (compare_tab->model.hunks[i].start_row >= static_cast<int>(compare_tab->selected_row)) {
      target = static_cast<int>(i);
      break;
    }
    target = static_cast<int>(i);
  }
  target = std::clamp(target + delta, 0, static_cast<int>(compare_tab->model.hunks.size()) - 1);
  compare_tab->selected_row =
      static_cast<std::size_t>(compare_tab->model.hunks[static_cast<std::size_t>(target)].start_row);
  RevealActiveCompareSelection();
}

void WorkspaceShell::ScrollCompareRows(int delta) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || delta == 0 || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  const int visible_rows = CompareVisibleRows(layout.editor_surface);
  const int max_scroll = CompareMaxScrollRow(*compare_tab, visible_rows);
  compare_tab->scroll_row = std::clamp(compare_tab->scroll_row + delta, 0, max_scroll);
}

bool WorkspaceShell::HandleMouseButtonDown(const SDL_Event& event) {
  if ((event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE &&
       event.button.button != SDL_BUTTON_RIGHT) ||
      last_window_width_ <= 0 || last_window_height_ <= 0) {
    return false;
  }

  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (dirty_prompt_visible_) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const SDL_FRect dialog = ComputeDirtyPromptRect(full);
    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        dirty_prompt_state_.selected_action = static_cast<int>(i);
        ConfirmDirtyPrompt();
        return true;
      }
    }
    return true;
  }

  if (prompt_surface_visible_) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const SDL_FRect dialog = ComputePromptSurfaceRect(full);
    const auto buttons = ComputePromptSurfaceButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        prompt_surface_state_.selected_button = static_cast<int>(i);
        if (event.button.button == SDL_BUTTON_LEFT) {
          ConfirmPromptSurface();
        }
        return true;
      }
    }
    return true;
  }

  if (!text_composition_.text.empty()) {
    text_composition_ = TextCompositionState{};
    if (SDL_Window* window = SDL_GetKeyboardFocus(); window != nullptr) {
      SDL_ClearComposition(window);
    }
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  mouse_selecting_ = false;

  if (tree_context_menu_.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect();
        popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
      for (const VisiblePopupMenuItem& item : ComputeVisiblePopupMenuItems(
               TreeContextMenuItems(tree_context_menu_.target),
               tree_context_menu_.active_item_index, *popup_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        tree_context_menu_.active_item_index = item.enabled ? static_cast<int>(item.index) : -1;
        if (event.button.button == SDL_BUTTON_LEFT && !item.separator && item.enabled) {
          ExecuteTreeContextMenuItem(item.index);
        }
        return true;
      }
      return true;
    }
    CloseTreeContextMenu();
  }

  if (menu_bar_open_ && event.button.button != SDL_BUTTON_LEFT) {
    CloseMenuBar();
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    const auto menu_bar_items = ComputeVisibleMenuBarItems(layout.menu_bar);
    const auto window_buttons = ComputeVisibleWindowControlButtons(layout.menu_bar);
    for (const VisibleWindowControlButton& button : window_buttons) {
      if (!Contains(button.rect, event.button.x, event.button.y)) {
        continue;
      }
      CloseMenuBar();
      switch (button.id) {
        case WindowControlButtonId::Minimize:
          pending_window_action_ = WindowAction::Minimize;
          break;
        case WindowControlButtonId::Maximize:
          pending_window_action_ = WindowAction::ToggleMaximize;
          break;
        case WindowControlButtonId::Close:
          RequestQuit();
          break;
      }
      return true;
    }
    if (menu_bar_open_) {
      for (const VisibleMenuBarItem& item : menu_bar_items) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        if (item.id == active_menu_id_) {
          CloseMenuBar();
        } else {
          OpenMenuBarMenu(item.id);
        }
        return true;
      }

      if (const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, active_menu_id_);
          popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
        for (const VisiblePopupMenuItem& item :
             ComputeVisiblePopupMenuItems(active_menu_id_, *popup_rect)) {
          if (!Contains(item.rect, event.button.x, event.button.y)) {
            continue;
          }
          active_menu_item_index_ = item.enabled ? static_cast<int>(item.index) : -1;
          if (!item.separator && item.enabled) {
            ExecuteMenuItem(active_menu_id_, item.index);
          }
          return true;
        }
        return true;
      }

      CloseMenuBar();
      return true;
    }

    if (Contains(layout.menu_bar, event.button.x, event.button.y)) {
      for (const VisibleMenuBarItem& item : menu_bar_items) {
        if (Contains(item.rect, event.button.x, event.button.y)) {
          OpenMenuBarMenu(item.id);
          return true;
        }
      }
      return true;
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && sidebar_visible_ &&
      Contains(SidebarResizeHandleRect(layout), event.button.x, event.button.y)) {
    drag_target_ = DragTarget::SidebarDivider;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && bottom_panel_visible_ &&
      Contains(BottomPanelResizeHandleRect(layout), event.button.x, event.button.y)) {
    drag_target_ = DragTarget::BottomPanelDivider;
    return true;
  }

  if (overlay_visible_ && event.button.button == SDL_BUTTON_LEFT) {
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);

    if (!Contains(overlay, event.button.x, event.button.y)) {
      overlay_visible_ = false;
      focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
      LogMessage("Overlay closed");
      return true;
    }

    ClampOverlayScrollRow(overlay);
    constexpr float kOverlayRowHeight = 22.0f;
    const float list_y = overlay.y + OverlayListStartOffset();
    const int visible_rows = OverlayVisibleRows(overlay);
    const int max_scroll =
        std::max(0, static_cast<int>(OverlayItemCount()) - visible_rows);
    const auto scrollbar = MakeVerticalScrollbarGeometry(
        MakeRect(overlay.x, list_y, overlay.w,
                 std::max(0.0f, overlay.y + overlay.h - list_y - 8.0f)),
        static_cast<float>(OverlayItemCount()), static_cast<float>(visible_rows),
        static_cast<float>(overlay_scroll_row_));
    if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::OverlayScrollbar;
      drag_scrollbar_offset_ =
          Contains(scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - scrollbar->thumb.y
              : scrollbar->thumb.h * 0.5f;
      overlay_scroll_row_ =
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                              drag_scrollbar_offset_))),
                     0, max_scroll);
      focus_ = FocusTarget::Overlay;
      return true;
    }

    const int row = static_cast<int>((event.button.y - list_y) / kOverlayRowHeight);
    if (row >= 0 && row < visible_rows) {
      const int item_index = overlay_scroll_row_ + row;
      if (item_index >= 0 && item_index < static_cast<int>(OverlayItemCount())) {
        SetOverlaySelectedIndex(static_cast<std::size_t>(item_index));
        RevealOverlaySelection(overlay);
        if (overlay_mode_ == OverlayMode::CommitPicker) {
          ActivateOverlaySelection();
        }
      }
    }
    focus_ = FocusTarget::Overlay;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && sidebar_visible_) {
    if (sidebar_mode_ == SidebarMode::Search) {
      const float list_y = layout.sidebar.y + kSearchSidebarResultsTop;
      const auto line_map = BuildProjectSearchLineMap();
      const int visible_rows = std::max(
          1, static_cast<int>((layout.sidebar.h - kSearchSidebarResultsTop) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
      const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const auto scrollbar = MakeVerticalScrollbarGeometry(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(line_map.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row));
      if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
        drag_target_ = DragTarget::SidebarScrollbar;
        drag_scrollbar_offset_ =
            Contains(scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.y) - scrollbar->thumb.y
                : scrollbar->thumb.h * 0.5f;
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
        focus_ = FocusTarget::Sidebar;
        return true;
      }
    } else {
      const auto& entries = directory_tree_.entries();
      const float list_y = layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
      const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const auto scrollbar = MakeVerticalScrollbarGeometry(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(entries.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row));
      if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
        drag_target_ = DragTarget::SidebarScrollbar;
        drag_scrollbar_offset_ =
            Contains(scrollbar->thumb, event.button.x, event.button.y)
                ? static_cast<float>(event.button.y) - scrollbar->thumb.y
                : scrollbar->thumb.h * 0.5f;
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
        focus_ = FocusTarget::Sidebar;
        return true;
      }
    }
  }

  if (Contains(layout.project_tab_strip, event.button.x, event.button.y)) {
    for (const VisibleProjectTab& tab : ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (!Contains(tab.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE ||
          (event.button.button == SDL_BUTTON_LEFT &&
           Contains(tab.close_rect, event.button.x, event.button.y))) {
        RequestCloseProject(tab.index);
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        SwitchProject(tab.index, true);
      }
      return true;
    }
  }

  if (Contains(layout.tab_strip, event.button.x, event.button.y)) {
    if (project_root_.empty()) {
      return false;
    }
    if (open_tabs_.empty()) {
      const SDL_FRect placeholder_tab =
          MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 5.0f, 220.0f, 24.0f);
      if (event.button.button == SDL_BUTTON_LEFT &&
          Contains(placeholder_tab, event.button.x, event.button.y)) {
        focus_ = FocusTarget::Editor;
        return true;
      }
      return false;
    }

    for (const VisibleTab& tab : ComputeVisibleTabs(layout.tab_strip)) {
      if (Contains(tab.rect, event.button.x, event.button.y)) {
        if (event.button.button == SDL_BUTTON_MIDDLE ||
            (event.button.button == SDL_BUTTON_LEFT &&
             Contains(tab.close_rect, event.button.x, event.button.y))) {
          RequestCloseTab(tab.index);
        } else if (event.button.button == SDL_BUTTON_LEFT) {
          ActivateTab(tab.index);
        }
        return true;
      }
    }
  }

  if (bottom_panel_visible_) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kBottomPanelHeaderHeight);
    if (BottomPanelShowsTerminal() && Contains(panel_header, event.button.x, event.button.y)) {
      if (event.button.button == SDL_BUTTON_LEFT &&
          Contains(BottomPanelTerminalNewTabRect(panel_header), event.button.x, event.button.y)) {
        OpenTerminal({});
        return true;
      }

      for (const VisibleTerminalTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
        if (!Contains(tab.rect, event.button.x, event.button.y)) {
          continue;
        }

        if (event.button.button == SDL_BUTTON_MIDDLE ||
            (event.button.button == SDL_BUTTON_LEFT &&
             Contains(tab.close_rect, event.button.x, event.button.y))) {
          CloseTerminalTab(tab.index);
        } else if (event.button.button == SDL_BUTTON_LEFT) {
          active_terminal_tab_index_ = tab.index;
          bottom_panel_mode_ = BottomPanelMode::Terminal;
          focus_ = FocusTarget::Panel;
        }
        return true;
      }
    }
  }

  if (sidebar_visible_ && Contains(layout.sidebar, event.button.x, event.button.y)) {
    focus_ = FocusTarget::Sidebar;
    const float header_height = kSidebarHeaderHeight + 6.0f;
    const float inset = kSidebarInset;
    const float row_height = kSidebarRowHeight;
    const float list_top = layout.sidebar.y + header_height;
    const float local_y = event.button.y - list_top;

    if (sidebar_mode_ == SidebarMode::Search) {
      if (event.button.button != SDL_BUTTON_LEFT) {
        return true;
      }
      if (event.button.y < layout.sidebar.y + 48.0f) {
        BeginProjectSearchEdit(ProjectSearchEditField::Query);
        return true;
      }
      if (event.button.y < layout.sidebar.y + 66.0f) {
        BeginProjectSearchEdit(ProjectSearchEditField::Replace);
        return true;
      }
      if (local_y < 0.0f) {
        return true;
      }

      const auto line_map = BuildProjectSearchLineMap();
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - kSearchSidebarResultsTop) / row_height));
      const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
      const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const int clicked_row =
          static_cast<int>((local_y - (kSearchSidebarResultsTop - header_height)) / row_height);
      if (clicked_row >= 0) {
        const int line_index = scroll_row + clicked_row;
        if (line_index >= 0 && line_index < static_cast<int>(line_map.size()) &&
            line_map[static_cast<std::size_t>(line_index)] >= 0) {
          project_search_selected_index_ =
              static_cast<std::size_t>(line_map[static_cast<std::size_t>(line_index)]);
          const auto& result = project_search_results_[project_search_selected_index_];
          OpenFile(project_root_ / result.relative_path);
          text_viewport_.MoveCursorTo(result.line, result.column);
          if (sidebar_temporary_) {
            RestorePreviousSidebar();
          }
          focus_ = FocusTarget::Editor;
          LogMessage("Project search result opened");
        }
      }
      return true;
    }

    if (local_y < 0.0f) {
      if (event.button.button == SDL_BUTTON_RIGHT) {
        OpenTreeContextMenu(TreeContextTargetKind::Background, {},
                            MakeRect(static_cast<float>(event.button.x),
                                     static_cast<float>(event.button.y), 1.0f, 1.0f));
      }
      return true;
    }

    const auto& entries = directory_tree_.entries();
    const int visible_rows = std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / row_height));
    const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
    const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
    const float row_width =
        std::max(0.0f, layout.sidebar.w - inset * 2.0f -
                           (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));

    const int clicked_row = static_cast<int>(local_y / row_height);
    const int entry_index = scroll_row + clicked_row;
    if (entry_index >= 0 && entry_index < static_cast<int>(entries.size())) {
      directory_tree_.SetSelectedIndex(static_cast<std::size_t>(entry_index));
      const SDL_FRect row_rect = MakeRect(
          layout.sidebar.x + inset,
          list_top + static_cast<float>(clicked_row) * row_height,
          row_width,
          row_height - 2.0f);
      if (Contains(row_rect, event.button.x, event.button.y) &&
          event.button.button == SDL_BUTTON_RIGHT) {
        const auto& entry = entries[static_cast<std::size_t>(entry_index)];
        const TreeContextTargetKind target =
            !entry.is_directory ? TreeContextTargetKind::File
            : entry.path == project_root_ ? TreeContextTargetKind::Root
                                          : TreeContextTargetKind::Directory;
        OpenTreeContextMenu(target, entry.path,
                            MakeRect(static_cast<float>(event.button.x),
                                     static_cast<float>(event.button.y), 1.0f, 1.0f));
        return true;
      }
      if (Contains(row_rect, event.button.x, event.button.y) &&
          event.button.button != SDL_BUTTON_RIGHT) {
        const auto opened = directory_tree_.ActivateSelection();
        if (opened.has_value()) {
          OpenFile(*opened);
        } else {
          LogMessage("Tree selection toggled");
        }
      }
      return true;
    }
    if (event.button.button == SDL_BUTTON_RIGHT) {
      OpenTreeContextMenu(TreeContextTargetKind::Background, {},
                          MakeRect(static_cast<float>(event.button.x),
                                   static_cast<float>(event.button.y), 1.0f, 1.0f));
    }
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && bottom_panel_visible_) {
    const std::size_t line_count = BottomPanelShowsTerminal()
                                       ? (ActiveTerminalTab() != nullptr
                                              ? ActiveTerminalTab()->session.SnapshotLines().size()
                                              : 0)
                                       : log_messages_.size();
    const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
    const int max_scroll = std::max(0, static_cast<int>(line_count) - visible_rows);
    const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
    const auto scrollbar =
        MakeVerticalScrollbarGeometry(BottomPanelContentRect(layout, command_mode_),
                                      static_cast<float>(line_count),
                                      static_cast<float>(visible_rows),
                                      static_cast<float>(scroll_row));
    if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::BottomPanelScrollbar;
      drag_scrollbar_offset_ =
          Contains(scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - scrollbar->thumb.y
              : scrollbar->thumb.h * 0.5f;
      SetBottomPanelScrollRow(
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                              drag_scrollbar_offset_))),
                     0, max_scroll),
          line_count, visible_rows);
      return true;
    }
  }

  if (bottom_panel_visible_ && BottomPanelShowsTerminal()) {
    auto* terminal_tab = ActiveTerminalTab();
    if (terminal_tab != nullptr) {
      const auto viewport_position =
          TerminalViewportPositionForPoint(event.button.x, event.button.y);
      const auto mouse_button = TerminalMouseButtonForSdl(event.button.button);
      if (viewport_position.has_value() &&
          mouse_button != terminal::TerminalSession::MouseButton::None &&
          terminal_tab->session.WantsMouseCapture()) {
        ClearTerminalSelection();
        terminal_tab->session.SendMouseButton(mouse_button, true, viewport_position->row,
                                              viewport_position->column, SDL_GetModState());
        focus_ = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && bottom_panel_visible_ &&
      Contains(layout.bottom_panel, event.button.x, event.button.y)) {
    if (BottomPanelShowsTerminal()) {
      const SDL_FRect panel_content = BottomPanelContentRect(layout, command_mode_);
      if (Contains(panel_content, event.button.x, event.button.y)) {
        const auto terminal_lines =
            ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.SnapshotLines()
                                           : std::vector<terminal::TerminalLine>{};
        if (const auto position =
                TerminalSelectionPositionForPoint(event.button.x, event.button.y, terminal_lines);
            position.has_value()) {
          if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
            terminal_tab->selection_anchor = *position;
            terminal_tab->selection_head = *position;
            terminal_tab->mouse_selecting = true;
            terminal_tab->follow_tail = false;
          }
        } else {
          ClearTerminalSelection();
        }
      } else {
        ClearTerminalSelection();
      }
      focus_ = FocusTarget::Panel;
    } else {
      ClearTerminalSelection();
    }
    return true;
  }

  if (event.button.button != SDL_BUTTON_LEFT ||
      !Contains(layout.editor_surface, event.button.x, event.button.y)) {
    return false;
  }

  if (ActiveTabIsCompare()) {
    CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return false;
    }

    const float line_height = text_renderer_.LineHeight();
    const float rows_y = layout.editor_surface.y + line_height + 12.0f;
    const int visible_rows = CompareVisibleRows(layout.editor_surface);
    ClampCompareScrollRow(*compare_tab, visible_rows);

    const auto scrollbar = MakeVerticalScrollbarGeometry(
        layout.editor_surface, static_cast<float>(compare_tab->model.rows.size()),
        static_cast<float>(visible_rows), static_cast<float>(compare_tab->scroll_row));
    if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::CompareScrollbar;
      drag_scrollbar_offset_ =
          Contains(scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - scrollbar->thumb.y
              : scrollbar->thumb.h * 0.5f;
      const int target_scroll = std::clamp(
          static_cast<int>(std::lround(ScrollUnitsForPointer(
              *scrollbar, static_cast<float>(event.button.y), drag_scrollbar_offset_))),
          0, CompareMaxScrollRow(*compare_tab, visible_rows));
      compare_tab->scroll_row = target_scroll;
      focus_ = FocusTarget::Editor;
      return true;
    }

    const int clicked_row = static_cast<int>((event.button.y - rows_y) / line_height);
    const int model_row = compare_tab->scroll_row + clicked_row;
    if (clicked_row >= 0 && model_row >= 0 &&
        model_row < static_cast<int>(compare_tab->model.rows.size())) {
      compare_tab->selected_row = static_cast<std::size_t>(model_row);
      focus_ = FocusTarget::Editor;
      return true;
    }
    return false;
  }

  const auto dividers = ComputeEditorSplitDividerLayouts(layout.editor_surface);
  const auto divider_it = std::find_if(
      dividers.begin(), dividers.end(), [&](const EditorSplitDividerLayout& divider) {
        return Contains(divider.rect, event.button.x, event.button.y);
      });
  if (divider_it != dividers.end()) {
    drag_target_ = DragTarget::EditorSplitDivider;
    drag_editor_split_path_ = divider_it->node_path;
    drag_editor_split_divider_index_ = divider_it->divider_index;
    focus_ = FocusTarget::Editor;
    return true;
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const auto pane_it = std::find_if(panes.begin(), panes.end(), [&](const EditorPaneLayout& pane) {
    return Contains(pane.rect, event.button.x, event.button.y);
  });
  if (pane_it == panes.end()) {
    return false;
  }
  if (!pane_it->active) {
    SetActiveEditorSplit(pane_it->leaf_id);
  }
  const SDL_FRect editor_rect = pane_it->rect;

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, text_viewport_, editor_rect);
  text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const std::size_t total_columns =
      std::max<std::size_t>(text_viewport_.visible_columns(), MaxVisualColumns(text_viewport_));
  const bool show_vertical = text_viewport_.line_count() > text_viewport_.visible_lines();
  const bool show_horizontal = total_columns > text_viewport_.visible_columns();
  if (show_vertical) {
    const auto scrollbar = MakeVerticalScrollbarGeometry(
        editor_rect, static_cast<float>(text_viewport_.line_count()),
        static_cast<float>(text_viewport_.visible_lines()),
        static_cast<float>(text_viewport_.scroll_line()), show_horizontal);
    if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::EditorVerticalScrollbar;
      drag_scrollbar_offset_ =
          Contains(scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.y) - scrollbar->thumb.y
              : scrollbar->thumb.h * 0.5f;
      text_viewport_.SetScrollLine(static_cast<std::size_t>(std::max(
          0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.y),
                                                drag_scrollbar_offset_)))));
      focus_ = FocusTarget::Editor;
      return true;
    }
  }
  if (show_horizontal) {
    const auto scrollbar = MakeHorizontalScrollbarGeometry(
        editor_rect, static_cast<float>(total_columns),
        static_cast<float>(text_viewport_.visible_columns()),
        static_cast<float>(text_viewport_.horizontal_scroll()), show_vertical);
    if (scrollbar.has_value() && Contains(scrollbar->track, event.button.x, event.button.y)) {
      drag_target_ = DragTarget::EditorHorizontalScrollbar;
      drag_scrollbar_offset_ =
          Contains(scrollbar->thumb, event.button.x, event.button.y)
              ? static_cast<float>(event.button.x) - scrollbar->thumb.x
              : scrollbar->thumb.w * 0.5f;
      text_viewport_.SetHorizontalScroll(static_cast<std::size_t>(std::max(
          0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.button.x),
                                                drag_scrollbar_offset_)))));
      focus_ = FocusTarget::Editor;
      return true;
    }
  }

  const float local_y = std::max(0.0f, event.button.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const std::size_t line = std::min(text_viewport_.scroll_line() + row,
                                    text_viewport_.line_count() == 0 ? 0 : text_viewport_.line_count() - 1);
  const float text_offset_x = std::max(0.0f, event.button.x - metrics.text_x);
  const std::size_t visual_column =
      text_viewport_.horizontal_scroll() +
      static_cast<std::size_t>(
          std::max(0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth()))));

  text_viewport_.MoveCursorToVisualColumn(line, visual_column, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
  ResetCaretBlink();
  focus_ = FocusTarget::Editor;
  mouse_selecting_ = true;
  return true;
}

bool WorkspaceShell::HandleMouseButtonUp(const SDL_Event& event) {
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (dirty_prompt_visible_) {
    return true;
  }
  if (prompt_surface_visible_) {
    return true;
  }

  if (bottom_panel_visible_ && BottomPanelShowsTerminal()) {
    auto* terminal_tab = ActiveTerminalTab();
    const auto viewport_position =
        TerminalViewportPositionForPoint(event.button.x, event.button.y);
    const auto mouse_button = TerminalMouseButtonForSdl(event.button.button);
    if (terminal_tab != nullptr && viewport_position.has_value() &&
        mouse_button != terminal::TerminalSession::MouseButton::None &&
        terminal_tab->session.WantsMouseCapture()) {
      ClearTerminalSelection();
      terminal_tab->session.SendMouseButton(mouse_button, false, viewport_position->row,
                                            viewport_position->column, SDL_GetModState());
      focus_ = FocusTarget::Panel;
      return true;
    }
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }
  if (drag_target_ != DragTarget::None) {
    drag_target_ = DragTarget::None;
    drag_scrollbar_offset_ = 0.0f;
    drag_editor_split_path_.clear();
    drag_editor_split_divider_index_ = 0;
    mouse_selecting_ = false;
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    return true;
  }
  if (auto* terminal_tab = ActiveTerminalTab();
      terminal_tab != nullptr && terminal_tab->mouse_selecting) {
    terminal_tab->mouse_selecting = false;
    return true;
  }
  const bool was_selecting = mouse_selecting_;
  mouse_selecting_ = false;
  return was_selecting;
}

bool WorkspaceShell::HandleMouseMotion(const SDL_Event& event) {
  if (last_window_width_ > 0 && last_window_height_ > 0) {
    UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
  }

  if (dirty_prompt_visible_) {
    return true;
  }
  if (prompt_surface_visible_) {
    return true;
  }

  if (last_window_width_ <= 0 || last_window_height_ <= 0) {
    return false;
  }

  if (tree_context_menu_.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect();
        popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
      tree_context_menu_.active_item_index = -1;
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(TreeContextMenuItems(tree_context_menu_.target),
                                        tree_context_menu_.active_item_index, *popup_rect)) {
        if (Contains(item.rect, event.motion.x, event.motion.y)) {
          tree_context_menu_.active_item_index = item.enabled ? static_cast<int>(item.index) : -1;
          break;
        }
      }
      return true;
    }
    tree_context_menu_.active_item_index = -1;
    return true;
  }

  if (menu_bar_open_) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (!Contains(item.rect, event.motion.x, event.motion.y)) {
        continue;
      }
      if (item.id != active_menu_id_) {
        OpenMenuBarMenu(item.id);
      }
      return true;
    }
    if (const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, active_menu_id_);
        popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
      active_menu_item_index_ = -1;
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(active_menu_id_, *popup_rect)) {
        if (Contains(item.rect, event.motion.x, event.motion.y)) {
          active_menu_item_index_ = item.enabled ? static_cast<int>(item.index) : -1;
          break;
        }
      }
      return true;
    }
    active_menu_item_index_ = -1;
    return true;
  }

  if (drag_target_ != DragTarget::None) {
    if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
      drag_target_ = DragTarget::None;
      drag_scrollbar_offset_ = 0.0f;
      drag_editor_split_path_.clear();
      drag_editor_split_divider_index_ = 0;
      UpdateMouseCursor(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
      return false;
    }

    if (drag_target_ == DragTarget::SidebarDivider) {
      sidebar_width_ =
          ClampSidebarWidth(static_cast<float>(event.motion.x), static_cast<float>(last_window_width_));
      return true;
    }

    if (drag_target_ == DragTarget::BottomPanelDivider) {
      const float desired_height =
          static_cast<float>(last_window_height_) - kStatusBarHeight - static_cast<float>(event.motion.y);
      bottom_panel_height_ =
          ClampBottomPanelHeight(desired_height, static_cast<float>(last_window_height_));
      return true;
    }

    const WorkspaceLayout drag_layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);

    if (drag_target_ == DragTarget::EditorSplitDivider) {
      auto* editor_tab = ActiveEditorTab();
      if (editor_tab == nullptr || editor_tab->views.size() < 2 || editor_tab->split_root == nullptr) {
        drag_target_ = DragTarget::None;
        return false;
      }

      NormalizeEditorSplitTree(*editor_tab);
      auto* split_node = FindEditorSplitNode(editor_tab->split_root.get(), drag_editor_split_path_);
      const auto node_rect =
          ComputeEditorSplitNodeRect(drag_layout.editor_surface, drag_editor_split_path_);
      if (split_node == nullptr || node_rect == std::nullopt || split_node->IsLeaf() ||
          split_node->orientation == EditorSplitOrientation::None ||
          drag_editor_split_divider_index_ + 1 >= split_node->children.size()) {
        drag_target_ = DragTarget::None;
        return false;
      }

      const bool vertical = split_node->orientation == EditorSplitOrientation::Vertical;
      const std::size_t child_count = split_node->children.size();
      const float total_extent =
          std::max(0.0f,
                   (vertical ? node_rect->w : node_rect->h) -
                       kEditorSplitDividerThickness * static_cast<float>(child_count - 1));
      if (total_extent <= 0.0f) {
        return false;
      }

      std::vector<float> weights(child_count, 0.0f);
      float total_weight = 0.0f;
      for (std::size_t i = 0; i < child_count; ++i) {
        weights[i] = std::max(0.0f, split_node->children[i]->size_fraction);
        total_weight += weights[i];
      }
      if (total_weight <= 0.0f) {
        std::fill(weights.begin(), weights.end(), 1.0f);
        total_weight = static_cast<float>(child_count);
      }

      std::vector<float> extents(child_count, 0.0f);
      float remaining_extent = total_extent;
      float remaining_weight = total_weight;
      for (std::size_t i = 0; i < child_count; ++i) {
        const std::size_t remaining_children = child_count - i;
        extents[i] = remaining_children == 1
                         ? remaining_extent
                         : std::floor(remaining_weight > 0.0f
                                          ? remaining_extent * (weights[i] / remaining_weight)
                                          : remaining_extent /
                                                static_cast<float>(remaining_children));
        if (remaining_extent > kMinSplitPaneExtent * static_cast<float>(remaining_children)) {
          extents[i] = std::clamp(
              extents[i], kMinSplitPaneExtent,
              remaining_extent -
                  kMinSplitPaneExtent * static_cast<float>(remaining_children - 1));
        }
        remaining_extent = std::max(0.0f, remaining_extent - extents[i]);
        remaining_weight = std::max(0.0f, remaining_weight - weights[i]);
      }

      float before_extent = 0.0f;
      for (std::size_t i = 0; i < drag_editor_split_divider_index_; ++i) {
        before_extent += extents[i];
      }
      const float pair_extent =
          extents[drag_editor_split_divider_index_] + extents[drag_editor_split_divider_index_ + 1];
      const float min_extent =
          total_extent > kMinSplitPaneExtent * static_cast<float>(child_count) ? kMinSplitPaneExtent
                                                                                : 0.0f;
      float leading_extent =
          vertical ? static_cast<float>(event.motion.x) - node_rect->x - before_extent -
                         kEditorSplitDividerThickness *
                             static_cast<float>(drag_editor_split_divider_index_) -
                         kEditorSplitDividerThickness * 0.5f
                   : static_cast<float>(event.motion.y) - node_rect->y - before_extent -
                         kEditorSplitDividerThickness *
                             static_cast<float>(drag_editor_split_divider_index_) -
                         kEditorSplitDividerThickness * 0.5f;
      leading_extent =
          pair_extent <= min_extent * 2.0f
              ? std::clamp(leading_extent, 0.0f, pair_extent)
              : std::clamp(leading_extent, min_extent, pair_extent - min_extent);
      const float trailing_extent = std::max(0.0f, pair_extent - leading_extent);
      split_node->children[drag_editor_split_divider_index_]->size_fraction =
          leading_extent / total_extent;
      split_node->children[drag_editor_split_divider_index_ + 1]->size_fraction =
          trailing_extent / total_extent;
      NormalizeEditorSplitNode(*split_node);
      focus_ = FocusTarget::Editor;
      return true;
    }

    if (drag_target_ == DragTarget::SidebarScrollbar && sidebar_visible_) {
      if (sidebar_mode_ == SidebarMode::Search) {
        const float list_y = drag_layout.sidebar.y + kSearchSidebarResultsTop;
        const auto line_map = BuildProjectSearchLineMap();
        const int visible_rows = std::max(
            1, static_cast<int>((drag_layout.sidebar.h - kSearchSidebarResultsTop) / kSidebarRowHeight));
        const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
        const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
        const auto scrollbar = MakeVerticalScrollbarGeometry(
            MakeRect(drag_layout.sidebar.x, list_y, drag_layout.sidebar.w,
                     std::max(0.0f, drag_layout.sidebar.y + drag_layout.sidebar.h - list_y)),
            static_cast<float>(line_map.size()), static_cast<float>(visible_rows),
            static_cast<float>(scroll_row));
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
      } else {
        const auto& entries = directory_tree_.entries();
        const float list_y = drag_layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
        const int visible_rows =
            std::max(1, static_cast<int>((drag_layout.sidebar.h - 36.0f) / kSidebarRowHeight));
        const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
        const int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
        const auto scrollbar = MakeVerticalScrollbarGeometry(
            MakeRect(drag_layout.sidebar.x, list_y, drag_layout.sidebar.w,
                     std::max(0.0f, drag_layout.sidebar.y + drag_layout.sidebar.h - list_y)),
            static_cast<float>(entries.size()), static_cast<float>(visible_rows),
            static_cast<float>(scroll_row));
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        sidebar_scroll_row_ =
            std::clamp(static_cast<int>(std::lround(
                           ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                                drag_scrollbar_offset_))),
                       0, max_scroll);
      }
      focus_ = FocusTarget::Sidebar;
      return true;
    }

    if (drag_target_ == DragTarget::BottomPanelScrollbar && bottom_panel_visible_) {
      const std::size_t line_count = BottomPanelShowsTerminal()
                                         ? (ActiveTerminalTab() != nullptr
                                                ? ActiveTerminalTab()->session.SnapshotLines().size()
                                                : 0)
                                         : log_messages_.size();
      const int visible_rows = BottomPanelVisibleRows(drag_layout.bottom_panel.h);
      const int max_scroll = std::max(0, static_cast<int>(line_count) - visible_rows);
      const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
      const auto scrollbar =
          MakeVerticalScrollbarGeometry(BottomPanelContentRect(drag_layout, command_mode_),
                                        static_cast<float>(line_count),
                                        static_cast<float>(visible_rows),
                                        static_cast<float>(scroll_row));
      if (!scrollbar.has_value()) {
        drag_target_ = DragTarget::None;
        drag_scrollbar_offset_ = 0.0f;
        return false;
      }
      SetBottomPanelScrollRow(
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                              drag_scrollbar_offset_))),
                     0, max_scroll),
          line_count, visible_rows);
      if (BottomPanelShowsTerminal()) {
        focus_ = FocusTarget::Panel;
      }
      return true;
    }

    if (drag_target_ == DragTarget::OverlayScrollbar && overlay_visible_) {
      const SDL_FRect overlay = ComputeOverlayRect(drag_layout.editor_area);
      const float list_y = overlay.y + OverlayListStartOffset();
      const int visible_rows = OverlayVisibleRows(overlay);
      const int max_scroll =
          std::max(0, static_cast<int>(OverlayItemCount()) - visible_rows);
      const auto scrollbar = MakeVerticalScrollbarGeometry(
          MakeRect(overlay.x, list_y, overlay.w,
                   std::max(0.0f, overlay.y + overlay.h - list_y - 8.0f)),
          static_cast<float>(OverlayItemCount()), static_cast<float>(visible_rows),
          static_cast<float>(overlay_scroll_row_));
      if (!scrollbar.has_value()) {
        drag_target_ = DragTarget::None;
        drag_scrollbar_offset_ = 0.0f;
        return false;
      }
      overlay_scroll_row_ =
          std::clamp(static_cast<int>(std::lround(
                         ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                              drag_scrollbar_offset_))),
                     0, max_scroll);
      focus_ = FocusTarget::Overlay;
      return true;
    }

    if (drag_target_ == DragTarget::CompareScrollbar && ActiveTabIsCompare()) {
      CompareTabState* compare_tab = ActiveCompareTab();
      if (compare_tab == nullptr) {
        drag_target_ = DragTarget::None;
        drag_scrollbar_offset_ = 0.0f;
        return false;
      }
      const int visible_rows = CompareVisibleRows(drag_layout.editor_surface);
      ClampCompareScrollRow(*compare_tab, visible_rows);
      const auto scrollbar = MakeVerticalScrollbarGeometry(
          drag_layout.editor_surface, static_cast<float>(compare_tab->model.rows.size()),
          static_cast<float>(visible_rows), static_cast<float>(compare_tab->scroll_row));
      if (!scrollbar.has_value()) {
        drag_target_ = DragTarget::None;
        drag_scrollbar_offset_ = 0.0f;
        return false;
      }
      const int target_scroll = std::clamp(
          static_cast<int>(std::lround(ScrollUnitsForPointer(
              *scrollbar, static_cast<float>(event.motion.y), drag_scrollbar_offset_))),
          0, CompareMaxScrollRow(*compare_tab, visible_rows));
      compare_tab->scroll_row = target_scroll;
      focus_ = FocusTarget::Editor;
      return true;
    }

    if (drag_target_ == DragTarget::EditorVerticalScrollbar ||
        drag_target_ == DragTarget::EditorHorizontalScrollbar) {
      const auto panes = ComputeEditorPaneLayouts(drag_layout.editor_surface);
      const auto active_pane = std::find_if(
          panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
      const SDL_FRect editor_rect =
          active_pane != panes.end() ? active_pane->rect : drag_layout.editor_surface;
      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          text_renderer_, text_viewport_, editor_rect);
      text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      const std::size_t total_columns =
          std::max<std::size_t>(text_viewport_.visible_columns(), MaxVisualColumns(text_viewport_));
      const bool show_vertical = text_viewport_.line_count() > text_viewport_.visible_lines();
      const bool show_horizontal = total_columns > text_viewport_.visible_columns();

      if (drag_target_ == DragTarget::EditorVerticalScrollbar) {
        const auto scrollbar = MakeVerticalScrollbarGeometry(
            editor_rect, static_cast<float>(text_viewport_.line_count()),
            static_cast<float>(text_viewport_.visible_lines()),
            static_cast<float>(text_viewport_.scroll_line()), show_horizontal);
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        text_viewport_.SetScrollLine(static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.y),
                                                  drag_scrollbar_offset_)))));
      } else {
        const auto scrollbar = MakeHorizontalScrollbarGeometry(
            editor_rect, static_cast<float>(total_columns),
            static_cast<float>(text_viewport_.visible_columns()),
            static_cast<float>(text_viewport_.horizontal_scroll()), show_vertical);
        if (!scrollbar.has_value()) {
          drag_target_ = DragTarget::None;
          drag_scrollbar_offset_ = 0.0f;
          return false;
        }
        text_viewport_.SetHorizontalScroll(static_cast<std::size_t>(std::max(
            0L, std::lround(ScrollUnitsForPointer(*scrollbar, static_cast<float>(event.motion.x),
                                                  drag_scrollbar_offset_)))));
      }
      focus_ = FocusTarget::Editor;
      return true;
    }

    drag_target_ = DragTarget::None;
    drag_scrollbar_offset_ = 0.0f;
    return false;
  }

  if (bottom_panel_visible_ && BottomPanelShowsTerminal()) {
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
      const bool buttons_down =
          (event.motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK)) != 0;
      if (terminal_tab->session.WantsMouseMotionCapture(buttons_down)) {
        if (const auto viewport_position =
                TerminalViewportPositionForPoint(event.motion.x, event.motion.y);
            viewport_position.has_value()) {
          terminal::TerminalSession::MouseButton button =
              terminal::TerminalSession::MouseButton::None;
          if ((event.motion.state & SDL_BUTTON_LMASK) != 0) {
            button = terminal::TerminalSession::MouseButton::Left;
          } else if ((event.motion.state & SDL_BUTTON_MMASK) != 0) {
            button = terminal::TerminalSession::MouseButton::Middle;
          } else if ((event.motion.state & SDL_BUTTON_RMASK) != 0) {
            button = terminal::TerminalSession::MouseButton::Right;
          }
          ClearTerminalSelection();
          terminal_tab->session.SendMouseMotion(button, viewport_position->row,
                                                viewport_position->column, SDL_GetModState());
          focus_ = FocusTarget::Panel;
          return true;
        }
      }
    }
  }

  if (auto* terminal_tab = ActiveTerminalTab();
      terminal_tab != nullptr && terminal_tab->mouse_selecting &&
      (event.motion.state & SDL_BUTTON_LMASK) != 0 && bottom_panel_visible_ &&
      BottomPanelShowsTerminal()) {
    const WorkspaceLayout layout = ComputeLayout(static_cast<float>(last_window_width_),
                                                 static_cast<float>(last_window_height_),
                                                 sidebar_visible_, bottom_panel_visible_,
                                                 sidebar_width_, bottom_panel_height_);
    if (!Contains(layout.bottom_panel, event.motion.x, event.motion.y)) {
      return false;
    }

    const auto terminal_lines = terminal_tab->session.SnapshotLines();
    if (const auto position =
            TerminalSelectionPositionForPoint(event.motion.x, event.motion.y, terminal_lines);
        position.has_value()) {
      terminal_tab->selection_head = *position;
      focus_ = FocusTarget::Panel;
      return true;
    }
  }

  if (!mouse_selecting_ || (event.motion.state & SDL_BUTTON_LMASK) == 0) {
    return false;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  if (!Contains(layout.editor_surface, event.motion.x, event.motion.y)) {
    return false;
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const auto active_pane = std::find_if(
      panes.begin(), panes.end(), [](const EditorPaneLayout& pane) { return pane.active; });
  const SDL_FRect editor_rect =
      active_pane != panes.end() ? active_pane->rect : layout.editor_surface;
  if (!Contains(editor_rect, event.motion.x, event.motion.y)) {
    return false;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, text_viewport_, editor_rect);
  text_viewport_.SetViewportSize(metrics.visible_rows, metrics.visible_columns);

  const float local_y = std::max(0.0f, event.motion.y - metrics.first_line_y);
  const std::size_t row = static_cast<std::size_t>(local_y / metrics.line_height);
  const std::size_t line = std::min(text_viewport_.scroll_line() + row,
                                    text_viewport_.line_count() == 0 ? 0 : text_viewport_.line_count() - 1);
  const float text_offset_x = std::max(0.0f, event.motion.x - metrics.text_x);
  const std::size_t visual_column =
      text_viewport_.horizontal_scroll() +
      static_cast<std::size_t>(
          std::max(0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth()))));

  text_viewport_.MoveCursorToVisualColumn(line, visual_column, true);
  ResetCaretBlink();
  focus_ = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::HandleMouseWheel(const SDL_Event& event) {
  if (dirty_prompt_visible_) {
    return true;
  }
  if (prompt_surface_visible_) {
    return true;
  }

  if (menu_bar_open_ || tree_context_menu_.open) {
    return true;
  }

  if (last_window_width_ <= 0 || last_window_height_ <= 0) {
    return false;
  }

  const int ticks = event.wheel.integer_y != 0
                        ? event.wheel.integer_y
                        : static_cast<int>(std::lround(event.wheel.y));
  if (ticks == 0) {
    return false;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);

  if (overlay_visible_) {
    if (overlay_mode_ == OverlayMode::CommitPicker) {
      MoveComparePickerSelection(-ticks);
    } else if (overlay_mode_ == OverlayMode::BufferSearch || overlay_mode_ == OverlayMode::BufferReplace) {
      MoveBufferSearchSelection(-ticks);
    } else if (overlay_mode_ == OverlayMode::ProjectSearch) {
      MoveProjectSearchSelection(-ticks);
    } else {
      MoveFileFinderSelection(-ticks);
    }
    return true;
  }

  if (Contains(layout.project_tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) &&
      !projects_.empty()) {
    const int max_scroll = std::max(0, static_cast<int>(projects_.size()) - 1);
    project_tab_scroll_index_ = std::clamp(project_tab_scroll_index_ - ticks, 0, max_scroll);
    return true;
  }

  if (Contains(layout.tab_strip, event.wheel.mouse_x, event.wheel.mouse_y) && !open_tabs_.empty()) {
    const int max_scroll = std::max(0, static_cast<int>(open_tabs_.size()) - 1);
    tab_scroll_index_ = std::clamp(tab_scroll_index_ - ticks, 0, max_scroll);
    return true;
  }

  if (sidebar_visible_ && Contains(layout.sidebar, event.wheel.mouse_x, event.wheel.mouse_y)) {
    int visible_rows = 1;
    int max_scroll = 0;
    if (sidebar_mode_ == SidebarMode::Search) {
      const auto line_map = BuildProjectSearchLineMap();
      visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - kSearchSidebarResultsTop) / 20.0f));
      max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
    } else {
      const auto& entries = directory_tree_.entries();
      visible_rows = std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / 20.0f));
      max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
    }
    sidebar_scroll_row_ = std::clamp(sidebar_scroll_row_ - ticks, 0, max_scroll);
    focus_ = FocusTarget::Sidebar;
    return true;
  }

  if (bottom_panel_visible_ && BottomPanelShowsTerminal()) {
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr &&
                                                terminal_tab->session.WantsMouseCapture()) {
      if (const auto viewport_position =
              TerminalViewportPositionForPoint(event.wheel.mouse_x, event.wheel.mouse_y);
          viewport_position.has_value()) {
        const terminal::TerminalSession::MouseButton button =
            ticks > 0 ? terminal::TerminalSession::MouseButton::WheelUp
                      : terminal::TerminalSession::MouseButton::WheelDown;
        const int step_count = std::abs(ticks);
        ClearTerminalSelection();
        for (int i = 0; i < step_count; ++i) {
          terminal_tab->session.SendMouseButton(button, true, viewport_position->row,
                                                viewport_position->column, SDL_GetModState());
        }
        focus_ = FocusTarget::Panel;
        return true;
      }
    }
  }

  if (bottom_panel_visible_ && Contains(layout.bottom_panel, event.wheel.mouse_x, event.wheel.mouse_y)) {
    const std::size_t line_count = BottomPanelShowsTerminal()
                                       ? (ActiveTerminalTab() != nullptr
                                              ? ActiveTerminalTab()->session.SnapshotLines().size()
                                              : 0)
                                       : log_messages_.size();
    const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
    const int max_scroll = std::max(0, static_cast<int>(line_count) - visible_rows);
    const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
    SetBottomPanelScrollRow(std::clamp(scroll_row - ticks, 0, max_scroll), line_count, visible_rows);
    if (BottomPanelShowsTerminal()) {
      focus_ = FocusTarget::Panel;
    }
    return true;
  }

  if (Contains(layout.editor_surface, event.wheel.mouse_x, event.wheel.mouse_y)) {
    if (ActiveTabIsCompare()) {
      ScrollCompareRows(-ticks * 3);
      focus_ = FocusTarget::Editor;
      return true;
    }
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    const auto hovered_pane = std::find_if(panes.begin(), panes.end(), [&](const EditorPaneLayout& pane) {
      return Contains(pane.rect, event.wheel.mouse_x, event.wheel.mouse_y);
    });
    if (hovered_pane != panes.end() && !hovered_pane->active) {
      SetActiveEditorSplit(hovered_pane->leaf_id);
    }
    text_viewport_.ScrollVertical(-ticks * 3);
    focus_ = FocusTarget::Editor;
    return true;
  }

  return false;
}

bool WorkspaceShell::OpenUntitledTab() {
  if (project_root_.empty()) {
    LogMessage("No project is loaded");
    return false;
  }
  SyncActiveEditorTab();

  editor::TextViewport untitled_view;
  untitled_view.SetUntitledBuffer();
  ApplyEditorPreferences(untitled_view);
  text_viewport_ = untitled_view;

  open_tabs_.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = {},
      .title = "untitled",
      .editor_state = MakeEditorTabState(untitled_view),
      .compare = std::nullopt,
  });
  active_tab_index_ = open_tabs_.size() - 1;
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  return true;
}

bool WorkspaceShell::OpenFileInNewTab(const std::filesystem::path& path) {
  if (project_root_.empty()) {
    return false;
  }
  SyncActiveEditorTab();

  auto existing = std::find_if(open_tabs_.begin(), open_tabs_.end(), [&](const TabEntry& tab) {
    return tab.kind == TabEntry::Kind::Editor && tab.path == path;
  });

  directory_tree_.SelectPath(path);

  if (existing != open_tabs_.end()) {
    ActivateTab(static_cast<std::size_t>(std::distance(open_tabs_.begin(), existing)));
    return true;
  }

  editor::TextViewport opened_view;
  if (!opened_view.OpenFile(path)) {
    return false;
  }
  ApplyEditorPreferences(opened_view);
  text_viewport_ = opened_view;

  open_tabs_.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = path,
      .title = path.filename().string(),
      .editor_state = MakeEditorTabState(opened_view),
      .compare = std::nullopt,
  });
  active_tab_index_ = open_tabs_.size() - 1;
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  return true;
}

bool WorkspaceShell::MoveActiveTabTo(std::size_t index) {
  if (active_tab_index_ >= open_tabs_.size() || index >= open_tabs_.size()) {
    return false;
  }

  if (active_tab_index_ == index) {
    return true;
  }

  SyncActiveEditorTab();

  TabEntry moved_tab = std::move(open_tabs_[active_tab_index_]);
  open_tabs_.erase(open_tabs_.begin() + static_cast<std::ptrdiff_t>(active_tab_index_));
  open_tabs_.insert(open_tabs_.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved_tab));

  active_tab_index_ = index;
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  return true;
}

std::optional<std::size_t> WorkspaceShell::FindTabIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  if (specifier.empty()) {
    if (error_message != nullptr) {
      *error_message = "usage: tabswitch <tab>";
    }
    return std::nullopt;
  }

  const std::string lowered_specifier = ToLower(specifier);
  try {
    std::size_t parsed_length = 0;
    const int tab_number = std::stoi(std::string(specifier), &parsed_length);
    if (parsed_length == specifier.size()) {
      if (tab_number >= 1 && static_cast<std::size_t>(tab_number) <= open_tabs_.size()) {
        return static_cast<std::size_t>(tab_number - 1);
      }
      if (error_message != nullptr) {
        *error_message = "Invalid tab index";
      }
      return std::nullopt;
    }
  } catch (...) {
  }

  std::vector<std::size_t> exact_matches;
  std::vector<std::size_t> partial_matches;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    const TabEntry& tab = open_tabs_[i];
    const std::string lowered_title = ToLower(tab.title);
    const std::string lowered_path = ToLower(RelativePathLabel(project_root_, tab.path));
    const std::string lowered_absolute_path = ToLower(tab.path.lexically_normal().string());
    const bool exact_match = lowered_title == lowered_specifier ||
                             (!lowered_path.empty() && lowered_path == lowered_specifier) ||
                             (!lowered_absolute_path.empty() &&
                              lowered_absolute_path == lowered_specifier);
    const bool partial_match = lowered_title.find(lowered_specifier) != std::string::npos ||
                               (!lowered_path.empty() &&
                                lowered_path.find(lowered_specifier) != std::string::npos) ||
                               (!lowered_absolute_path.empty() &&
                                lowered_absolute_path.find(lowered_specifier) != std::string::npos);
    if (exact_match) {
      exact_matches.push_back(i);
    } else if (partial_match) {
      partial_matches.push_back(i);
    }
  }

  if (exact_matches.size() == 1) {
    return exact_matches.front();
  }
  if (exact_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (partial_matches.size() == 1) {
    return partial_matches.front();
  }
  if (partial_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (error_message != nullptr) {
    *error_message = "Unknown tab: " + std::string(specifier);
  }
  return std::nullopt;
}

void WorkspaceShell::OpenFile(const std::filesystem::path& path) {
  if (!OpenFileInNewTab(path)) {
    LogMessage("Failed to open file: " + path.lexically_normal().string());
    return;
  }
  LogMessage("Opened file: " + path.lexically_normal().string());
}

void WorkspaceShell::OpenBufferSearch() {
  overlay_visible_ = true;
  overlay_mode_ = OverlayMode::BufferSearch;
  buffer_search_field_ = BufferSearchField::Search;
  focus_ = FocusTarget::Overlay;
  buffer_search_query_.clear();
  buffer_replace_text_.clear();
  buffer_search_matches_.clear();
  buffer_search_selected_index_ = 0;
  ResetOverlayScroll();
  LogMessage("Buffer search opened");
}

void WorkspaceShell::OpenBufferReplace() {
  overlay_visible_ = true;
  overlay_mode_ = OverlayMode::BufferReplace;
  buffer_search_field_ = BufferSearchField::Search;
  focus_ = FocusTarget::Overlay;
  buffer_search_query_.clear();
  buffer_replace_text_.clear();
  buffer_search_matches_.clear();
  buffer_search_selected_index_ = 0;
  ResetOverlayScroll();
  LogMessage("Buffer replace opened");
}

void WorkspaceShell::OpenProjectSearch() {
  if (project_root_.empty()) {
    LogMessage("No project is loaded");
    return;
  }
  project_search_query_.clear();
  project_search_results_.clear();
  project_search_selected_index_ = 0;
  project_replace_text_.clear();
  ResetOverlayScroll();
  ShowSearchSidebar("", true);
}

void WorkspaceShell::RefreshBufferSearch() {
  buffer_search_matches_.clear();
  buffer_search_selected_index_ = 0;

  if (buffer_search_query_.empty()) {
    ResetOverlayScroll();
    return;
  }

  const std::string lowered_query = ToLower(buffer_search_query_);
  const auto& lines = text_viewport_.lines();
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const std::string lowered_line = ToLower(lines[line_index]);
    std::size_t offset = lowered_line.find(lowered_query);
    while (offset != std::string::npos) {
      buffer_search_matches_.push_back(editor::SelectionRange{
          .start = editor::TextPosition{line_index, offset},
          .end = editor::TextPosition{line_index, offset + lowered_query.size()},
      });
      offset = lowered_line.find(lowered_query, offset + 1);
    }
  }

  if (!buffer_search_matches_.empty()) {
    const auto& match = buffer_search_matches_.front();
    text_viewport_.MoveCursorTo(match.start.line, match.start.column);
  }
  ResetOverlayScroll();
}

void WorkspaceShell::RefreshProjectSearch() {
  StopProjectSearch();
  project_search_results_.clear();
  project_search_selected_index_ = 0;
  project_search_error_.clear();

  if (project_root_.empty()) {
    ResetOverlayScroll();
    return;
  }

  if (project_search_query_.empty()) {
    ResetOverlayScroll();
    return;
  }

  project_search_running_ = true;
  project_search_run_id_ = project_search_service_.Start(project_root_, project_search_query_, false);
  ResetOverlayScroll();
}

void WorkspaceShell::StopProjectSearch() {
  project_search_service_.Stop();
  project_search_running_ = false;
  project_search_run_id_ = 0;
}

void WorkspaceShell::ConsumeProjectSearchUpdates() {
  static constexpr std::size_t kMaxProjectSearchResults = 200;

  auto update = project_search_service_.TakePendingUpdate();
  if (update.run_id == 0 || update.run_id != project_search_run_id_) {
    return;
  }

  for (auto& result : update.results) {
    if (project_search_results_.size() >= kMaxProjectSearchResults) {
      StopProjectSearch();
      break;
    }
    project_search_results_.push_back(std::move(result));
  }

  if (!update.error.empty()) {
    project_search_error_ = std::move(update.error);
  }
  if (update.finished) {
    project_search_running_ = false;
  }
  if (overlay_visible_ && overlay_mode_ == OverlayMode::ProjectSearch &&
      last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::ResetOverlayScroll() {
  overlay_scroll_row_ = 0;
}

float WorkspaceShell::OverlayListStartOffset() const {
  switch (overlay_mode_) {
    case OverlayMode::FileFinder:
      return 74.0f;
    case OverlayMode::BufferReplace:
      return 106.0f;
    case OverlayMode::BufferSearch:
    case OverlayMode::ProjectSearch:
    case OverlayMode::CommitPicker:
    default:
      return 86.0f;
  }
}

int WorkspaceShell::OverlayVisibleRows(const SDL_FRect& overlay) const {
  constexpr float kOverlayRowHeight = 22.0f;
  const float available_height = overlay.h - OverlayListStartOffset() - 16.0f;
  return std::max(1, static_cast<int>(std::floor(std::max(0.0f, available_height) / kOverlayRowHeight)));
}

std::size_t WorkspaceShell::OverlayItemCount() const {
  switch (overlay_mode_) {
    case OverlayMode::CommitPicker:
      return compare_picker_matches_.size();
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return buffer_search_matches_.size();
    case OverlayMode::ProjectSearch:
      return project_search_results_.size();
    case OverlayMode::FileFinder:
    default:
      return file_finder_.results().size();
  }
}

std::size_t WorkspaceShell::OverlaySelectedIndex() const {
  switch (overlay_mode_) {
    case OverlayMode::CommitPicker:
      return compare_picker_selected_index_;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      return buffer_search_selected_index_;
    case OverlayMode::ProjectSearch:
      return project_search_selected_index_;
    case OverlayMode::FileFinder:
    default:
      return file_finder_.selected_index();
  }
}

void WorkspaceShell::SetOverlaySelectedIndex(std::size_t index) {
  const std::size_t item_count = OverlayItemCount();
  if (item_count == 0) {
    return;
  }
  const std::size_t clamped_index = std::min(index, item_count - 1);
  switch (overlay_mode_) {
    case OverlayMode::CommitPicker:
      compare_picker_selected_index_ = clamped_index;
      break;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace:
      buffer_search_selected_index_ = clamped_index;
      if (!buffer_search_matches_.empty()) {
        const auto& match = buffer_search_matches_[buffer_search_selected_index_];
        text_viewport_.MoveCursorTo(match.start.line, match.start.column);
      }
      break;
    case OverlayMode::ProjectSearch:
      project_search_selected_index_ = clamped_index;
      break;
    case OverlayMode::FileFinder:
    default: {
      const std::size_t current_index = file_finder_.selected_index();
      file_finder_.MoveSelection(static_cast<int>(clamped_index) - static_cast<int>(current_index));
      break;
    }
  }
}

void WorkspaceShell::ClampOverlayScrollRow(const SDL_FRect& overlay) {
  const int visible_rows = OverlayVisibleRows(overlay);
  const int max_scroll =
      std::max(0, static_cast<int>(OverlayItemCount()) - visible_rows);
  overlay_scroll_row_ = std::clamp(overlay_scroll_row_, 0, max_scroll);
}

void WorkspaceShell::RevealOverlaySelection(const SDL_FRect& overlay) {
  ClampOverlayScrollRow(overlay);
  if (OverlayItemCount() == 0) {
    return;
  }

  const int visible_rows = OverlayVisibleRows(overlay);
  const int max_scroll =
      std::max(0, static_cast<int>(OverlayItemCount()) - visible_rows);
  const int selected = static_cast<int>(std::min(OverlaySelectedIndex(), OverlayItemCount() - 1));
  if (selected < overlay_scroll_row_) {
    overlay_scroll_row_ = selected;
  } else if (selected >= overlay_scroll_row_ + visible_rows) {
    overlay_scroll_row_ = selected - visible_rows + 1;
  }
  overlay_scroll_row_ = std::clamp(overlay_scroll_row_, 0, max_scroll);
}

bool WorkspaceShell::ActivateOverlaySelection() {
  switch (overlay_mode_) {
    case OverlayMode::CommitPicker:
      OpenSelectedCompareCommit();
      return true;
    case OverlayMode::BufferSearch:
      if (!buffer_search_matches_.empty()) {
        const auto& match = buffer_search_matches_[buffer_search_selected_index_];
        text_viewport_.MoveCursorTo(match.start.line, match.start.column);
      }
      overlay_visible_ = false;
      focus_ = FocusTarget::Editor;
      LogMessage("Buffer search closed");
      return true;
    case OverlayMode::BufferReplace:
      ReplaceCurrentBufferSearchMatch();
      return true;
    case OverlayMode::ProjectSearch:
      if (!project_search_results_.empty() &&
          project_search_selected_index_ < project_search_results_.size()) {
        const auto& result = project_search_results_[project_search_selected_index_];
        OpenFile(project_root_ / result.relative_path);
        text_viewport_.MoveCursorTo(result.line, result.column);
        overlay_visible_ = false;
        focus_ = FocusTarget::Editor;
        LogMessage("Project search result opened");
      }
      return true;
    case OverlayMode::FileFinder:
    default:
      if (const auto selected = file_finder_.SelectedPath(); selected.has_value()) {
        OpenFile(project_root_ / *selected);
      }
      overlay_visible_ = false;
      focus_ = FocusTarget::Editor;
      LogMessage("Finder selection opened");
      return true;
  }
}

void WorkspaceShell::BeginProjectSearchEdit(ProjectSearchEditField field) {
  project_search_edit_field_ = field;
  project_search_edit_buffer_ =
      field == ProjectSearchEditField::Query ? project_search_query_ : project_replace_text_;
  project_search_editing_ = true;
}

void WorkspaceShell::CommitProjectSearchEdit() {
  project_search_editing_ = false;
  if (project_search_edit_field_ == ProjectSearchEditField::Query) {
    project_search_query_ = project_search_edit_buffer_;
    RefreshProjectSearch();
    LogMessage("Project search updated");
    return;
  }

  project_replace_text_ = project_search_edit_buffer_;
  LogMessage("Project replacement text updated");
}

void WorkspaceShell::CancelProjectSearchEdit() {
  project_search_edit_buffer_ =
      project_search_edit_field_ == ProjectSearchEditField::Query ? project_search_query_
                                                                  : project_replace_text_;
  project_search_editing_ = false;
  LogMessage("Project search edit cancelled");
}

void WorkspaceShell::ReplaceAllProjectSearchMatches() {
  if (project_search_query_.empty()) {
    LogMessage("Project replace needs a search query");
    return;
  }

  if (!QuerySupportsLiteralReplace(project_search_query_)) {
    LogMessage("Project replace currently supports literal queries only");
    return;
  }

  const bool case_sensitive = UsesCaseSensitiveLiteralMatch(project_search_query_);
  struct PendingProjectReplace {
    std::filesystem::path relative_path;
    std::filesystem::path absolute_path;
    std::string content;
    std::size_t replacements = 0;
  };

  std::vector<PendingProjectReplace> pending;
  std::size_t replaced_total = 0;

  for (const auto& relative_path : file_index_.files()) {
    const std::filesystem::path absolute_path = project_root_ / relative_path;
    const std::filesystem::path normalized_absolute = absolute_path.lexically_normal();

    std::ifstream input(absolute_path, std::ios::binary);
    if (!input) {
      continue;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string content = buffer.str();
    if (content.find('\0') != std::string::npos) {
      continue;
    }

    std::string updated_content = content;
    const std::size_t replacements = ReplaceLiteralMatchesInText(
        updated_content, project_search_query_, project_replace_text_, case_sensitive);
    if (replacements == 0) {
      continue;
    }

    for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
      if (open_tabs_[i].kind == TabEntry::Kind::Editor &&
          open_tabs_[i].path.lexically_normal() == normalized_absolute &&
          TabIsDirty(i)) {
        LogMessage("Project replace blocked by dirty tab: " + relative_path.string());
        return;
      }
    }
    replaced_total += replacements;
    pending.push_back(PendingProjectReplace{
        .relative_path = relative_path,
        .absolute_path = normalized_absolute,
        .content = std::move(updated_content),
        .replacements = replacements,
    });
  }

  if (pending.empty()) {
    LogMessage("Project replace found no literal matches");
    return;
  }

  for (const auto& change : pending) {
    std::ofstream output(change.absolute_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      LogMessage("Project replace failed to write: " + change.relative_path.string());
      return;
    }
    output.write(change.content.data(), static_cast<std::streamsize>(change.content.size()));
    if (!output.good()) {
      LogMessage("Project replace failed to write: " + change.relative_path.string());
      return;
    }

    for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
      auto& tab = open_tabs_[i];
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          tab.editor_state->views.empty()) {
        continue;
      }

      editor::TextViewport reopened_view;
      if (!reopened_view.OpenFile(change.absolute_path)) {
        continue;
      }
      ApplyEditorPreferences(reopened_view);
      bool reloaded_any = false;
      for (auto& view : tab.editor_state->views) {
        const bool active_view =
            i == active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id;
        const std::filesystem::path current_path =
            active_view ? text_viewport_.path().lexically_normal()
                        : view.viewport.path().lexically_normal();
        if (current_path != change.absolute_path) {
          continue;
        }
        view.viewport = reopened_view;
        if (active_view) {
          text_viewport_ = reopened_view;
        }
        reloaded_any = true;
      }
      if (reloaded_any && i == active_tab_index_) {
        NormalizeEditorSplitTree(*tab.editor_state);
        SyncActiveEditorTabMetadata();
      }
    }
  }

  RefreshProjectFiles();
  RefreshProjectSearch();
  LogMessage("Replaced " + std::to_string(replaced_total) + " matches in " +
             std::to_string(pending.size()) + " files");
}

std::vector<int> WorkspaceShell::BuildProjectSearchLineMap() const {
  std::vector<int> line_map;
  line_map.reserve(project_search_results_.size() * 2);

  std::filesystem::path current_path;
  for (std::size_t i = 0; i < project_search_results_.size(); ++i) {
    const auto& result = project_search_results_[i];
    if (result.relative_path != current_path) {
      current_path = result.relative_path;
      line_map.push_back(-1);
    }
    line_map.push_back(static_cast<int>(i));
  }

  return line_map;
}

int WorkspaceShell::ProjectSearchLineForResult(std::size_t index) const {
  const auto line_map = BuildProjectSearchLineMap();
  for (std::size_t line = 0; line < line_map.size(); ++line) {
    if (line_map[line] == static_cast<int>(index)) {
      return static_cast<int>(line);
    }
  }
  return 0;
}

void WorkspaceShell::MoveBufferSearchSelection(int delta) {
  if (buffer_search_matches_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(buffer_search_selected_index_);
  const int max_index = static_cast<int>(buffer_search_matches_.size()) - 1;
  buffer_search_selected_index_ =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  const auto& match = buffer_search_matches_[buffer_search_selected_index_];
  text_viewport_.MoveCursorTo(match.start.line, match.start.column);
  if (overlay_visible_ && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::MoveProjectSearchSelection(int delta) {
  if (project_search_results_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(project_search_selected_index_);
  const int max_index = static_cast<int>(project_search_results_.size()) - 1;
  project_search_selected_index_ =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  if (overlay_visible_ && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

void WorkspaceShell::ReplaceCurrentBufferSearchMatch() {
  if (buffer_search_matches_.empty() ||
      buffer_search_selected_index_ >= buffer_search_matches_.size()) {
    return;
  }

  const auto match = buffer_search_matches_[buffer_search_selected_index_];
  if (!text_viewport_.ReplaceRange(match, buffer_replace_text_)) {
    return;
  }

  RefreshBufferSearch();
  if (!buffer_search_matches_.empty()) {
    buffer_search_selected_index_ =
        std::min(buffer_search_selected_index_, buffer_search_matches_.size() - 1);
    const auto& next_match = buffer_search_matches_[buffer_search_selected_index_];
    text_viewport_.MoveCursorTo(next_match.start.line, next_match.start.column);
  }
  LogMessage("Replaced current match");
}

void WorkspaceShell::ReplaceAllBufferSearchMatches() {
  if (buffer_search_query_.empty()) {
    return;
  }

  const std::size_t replaced =
      text_viewport_.ReplaceAll(buffer_search_query_, buffer_replace_text_);
  RefreshBufferSearch();
  LogMessage("Replaced " + std::to_string(replaced) + " matches");
}

std::optional<editor::SelectionRange> WorkspaceShell::ActiveBufferSearchMatch() const {
  if (!overlay_visible_ || (overlay_mode_ != OverlayMode::BufferSearch &&
                            overlay_mode_ != OverlayMode::BufferReplace) ||
      buffer_search_matches_.empty() ||
      buffer_search_selected_index_ >= buffer_search_matches_.size()) {
    return std::nullopt;
  }
  return buffer_search_matches_[buffer_search_selected_index_];
}

void WorkspaceShell::OpenTerminal(std::string command) {
  if (project_root_.empty()) {
    LogMessage("No project is loaded");
    return;
  }
  const std::filesystem::path working_directory = project_root_;
  auto terminal_tab = std::make_unique<TerminalTabState>();
  if (terminal_event_type_ != 0) {
    terminal_tab->session.SetWakeEventType(terminal_event_type_);
  }
  if (!terminal_tab->session.Start(working_directory, command)) {
    bottom_panel_mode_ = BottomPanelMode::Logs;
    LogMessage("Failed to start terminal");
    return;
  }

  terminal_tabs_.push_back(std::move(terminal_tab));
  active_terminal_tab_index_ = terminal_tabs_.size() - 1;
  bottom_panel_mode_ = BottomPanelMode::Terminal;
  SetBottomPanelVisible(true);
  focus_ = FocusTarget::Panel;
  if (auto* active_terminal = ActiveTerminalTab(); active_terminal != nullptr) {
    LogMessage("Terminal started: " + active_terminal->session.LaunchLabel());
  }
}

WorkspaceShell::TerminalTabState* WorkspaceShell::ActiveTerminalTab() {
  if (active_terminal_tab_index_ >= terminal_tabs_.size()) {
    return nullptr;
  }
  return terminal_tabs_[active_terminal_tab_index_].get();
}

const WorkspaceShell::TerminalTabState* WorkspaceShell::ActiveTerminalTab() const {
  if (active_terminal_tab_index_ >= terminal_tabs_.size()) {
    return nullptr;
  }
  return terminal_tabs_[active_terminal_tab_index_].get();
}

void WorkspaceShell::CloseTerminalTab(std::size_t index) {
  if (index >= terminal_tabs_.size()) {
    return;
  }

  terminal_tabs_.erase(terminal_tabs_.begin() + static_cast<std::ptrdiff_t>(index));
  if (terminal_tabs_.empty()) {
    active_terminal_tab_index_ = 0;
    bottom_panel_mode_ = BottomPanelMode::Logs;
    if (focus_ == FocusTarget::Panel) {
      focus_ = FocusTarget::Editor;
    }
    return;
  }

  active_terminal_tab_index_ =
      std::min(active_terminal_tab_index_ > index ? active_terminal_tab_index_ - 1
                                                   : active_terminal_tab_index_,
               terminal_tabs_.size() - 1);
}

void WorkspaceShell::ReapExitedTerminalTabs() {
  for (std::size_t i = 0; i < terminal_tabs_.size();) {
    if (terminal_tabs_[i] != nullptr && !terminal_tabs_[i]->session.running()) {
      CloseTerminalTab(i);
      continue;
    }
    ++i;
  }
}

WorkspaceShell::TextInputSurface WorkspaceShell::CurrentTextInputSurface() const {
  if (dirty_prompt_visible_) {
    return TextInputSurface::None;
  }

  if (prompt_surface_visible_) {
    return prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput
               ? TextInputSurface::PromptInput
               : TextInputSurface::None;
  }

  if (menu_bar_open_ || tree_context_menu_.open) {
    return TextInputSurface::None;
  }

  if (command_mode_) {
    return TextInputSurface::Command;
  }

  if (overlay_visible_) {
    switch (overlay_mode_) {
      case OverlayMode::BufferSearch:
        return TextInputSurface::BufferSearch;
      case OverlayMode::BufferReplace:
        return buffer_search_field_ == BufferSearchField::Search
                   ? TextInputSurface::BufferReplaceSearch
                   : TextInputSurface::BufferReplaceReplace;
      case OverlayMode::ProjectSearch:
        return TextInputSurface::ProjectSearchOverlay;
      case OverlayMode::CommitPicker:
        return TextInputSurface::CommitPicker;
      case OverlayMode::FileFinder:
      default:
        return TextInputSurface::FileFinder;
    }
  }

  if (focus_ == FocusTarget::Sidebar && sidebar_visible_ && sidebar_mode_ == SidebarMode::Search &&
      project_search_editing_) {
    return project_search_edit_field_ == ProjectSearchEditField::Query
               ? TextInputSurface::SidebarSearchQuery
               : TextInputSurface::SidebarSearchReplace;
  }

  if (focus_ == FocusTarget::Editor && !ActiveTabIsCompare()) {
    return TextInputSurface::Editor;
  }

  if (focus_ == FocusTarget::Panel && BottomPanelShowsTerminal()) {
    return TextInputSurface::Terminal;
  }

  return TextInputSurface::None;
}

void WorkspaceShell::SyncTextInputSurface(SDL_Window* window) {
  const TextInputSurface current_surface = CurrentTextInputSurface();
  if (current_surface == active_text_input_surface_) {
    return;
  }

  active_text_input_surface_ = current_surface;
  text_composition_ = TextCompositionState{};
  SDL_Window* target_window = window != nullptr ? window : SDL_GetKeyboardFocus();
  if (target_window != nullptr) {
    SDL_ClearComposition(target_window);
  }
}

bool WorkspaceShell::CompositionConsumesKey(SDL_Keycode key, SDL_Keymod modifiers) const {
  if (text_composition_.text.empty() || text_composition_.surface != CurrentTextInputSurface() ||
      (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0) {
    return false;
  }

  switch (key) {
    case SDLK_BACKSPACE:
    case SDLK_DELETE:
    case SDLK_ESCAPE:
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_TAB:
    case SDLK_UP:
    case SDLK_DOWN:
    case SDLK_LEFT:
    case SDLK_RIGHT:
    case SDLK_HOME:
    case SDLK_END:
    case SDLK_PAGEUP:
    case SDLK_PAGEDOWN:
      return true;
    default:
      return false;
  }
}

bool WorkspaceShell::HandleTextEditing(const SDL_TextEditingEvent& event) {
  if (menu_bar_open_ || tree_context_menu_.open) {
    text_composition_ = TextCompositionState{};
    return true;
  }
  SyncTextInputSurface(nullptr);
  const TextInputSurface surface = CurrentTextInputSurface();
  if (surface == TextInputSurface::None || surface == TextInputSurface::Terminal) {
    text_composition_ = TextCompositionState{};
    return false;
  }

  if (event.text == nullptr || event.text[0] == '\0') {
    text_composition_ = TextCompositionState{};
    return true;
  }

  text_composition_.surface = surface;
  text_composition_.text = event.text;
  text_composition_.start = event.start;
  text_composition_.length = event.length;
  return true;
}

bool WorkspaceShell::HandleTextInput(const SDL_TextInputEvent& event) {
  if (menu_bar_open_ || tree_context_menu_.open) {
    return true;
  }
  if (event.text == nullptr || event.text[0] == '\0' || dirty_prompt_visible_) {
    return false;
  }

  SyncTextInputSurface(nullptr);
  text_composition_ = TextCompositionState{};
  const std::string_view input(event.text);
  if (prompt_surface_visible_ &&
      prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput) {
    prompt_surface_state_.input.append(input);
    return true;
  }
  if (command_mode_) {
    command_input_.append(input);
    command_history_index_.reset();
    command_history_pending_input_.clear();
    ClearCommandCompletionFeedback();
    return true;
  }

  if (overlay_visible_) {
    switch (overlay_mode_) {
      case OverlayMode::CommitPicker:
        compare_picker_query_.append(input);
        RefreshComparePicker();
        return true;
      case OverlayMode::BufferSearch:
        buffer_search_query_.append(input);
        RefreshBufferSearch();
        return true;
      case OverlayMode::BufferReplace:
        if (buffer_search_field_ == BufferSearchField::Search) {
          buffer_search_query_.append(input);
          RefreshBufferSearch();
        } else {
          buffer_replace_text_.append(input);
        }
        return true;
      case OverlayMode::ProjectSearch:
        project_search_query_.append(input);
        RefreshProjectSearch();
        return true;
      case OverlayMode::FileFinder:
      default:
        file_finder_.AppendQueryText(input);
        ResetOverlayScroll();
        return true;
    }
  }

  if (focus_ == FocusTarget::Sidebar && sidebar_visible_ && sidebar_mode_ == SidebarMode::Search &&
      project_search_editing_) {
    project_search_edit_buffer_.append(input);
    return true;
  }

  if (focus_ == FocusTarget::Editor && !ActiveTabIsCompare()) {
    text_viewport_.InsertText(input);
    ResetCaretBlink();
    return true;
  }

  if (focus_ == FocusTarget::Panel && BottomPanelShowsTerminal()) {
    ClearTerminalSelection();
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
      terminal_tab->session.SendBytes(input);
    }
    return true;
  }

  return false;
}

bool WorkspaceShell::HandleTerminalKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers) {
  auto* terminal_tab = ActiveTerminalTab();
  if (!BottomPanelShowsTerminal() || terminal_tab == nullptr) {
    return false;
  }

  if ((modifiers & SDL_KMOD_CTRL) && event.key == SDLK_C && TerminalHasSelection()) {
    const std::string text = SelectedTerminalText(terminal_tab->session.SnapshotLines());
    if (!text.empty() && SDL_SetClipboardText(text.c_str())) {
      LogMessage("Terminal selection copied");
    }
    return true;
  }

  if (event.key == SDLK_ESCAPE && TerminalHasSelection()) {
    ClearTerminalSelection();
    LogMessage("Terminal selection cleared");
    return true;
  }

  if (modifiers & SDL_KMOD_CTRL) {
    if (event.key >= SDLK_A && event.key <= SDLK_Z) {
      const char control =
          static_cast<char>(1 + (event.key - SDLK_A));
      terminal_tab->session.SendBytes(std::string(1, control));
      return true;
    }
    switch (event.key) {
      case SDLK_LEFTBRACKET:
        terminal_tab->session.SendBytes("\x1b");
        return true;
      case SDLK_BACKSLASH:
        terminal_tab->session.SendBytes("\x1c");
        return true;
      case SDLK_RIGHTBRACKET:
        terminal_tab->session.SendBytes("\x1d");
        return true;
      case SDLK_SPACE:
        terminal_tab->session.SendBytes(std::string(1, '\0'));
        return true;
      default:
        break;
    }
  }

  if (modifiers & SDL_KMOD_ALT) {
    const char input_character = KeycodeToAscii(event.key, modifiers);
    if (input_character != '\0') {
      std::string bytes(1, '\x1b');
      bytes.push_back(input_character);
      terminal_tab->session.SendBytes(bytes);
      return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      terminal_tab->session.SendBytes("\x1b");
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      terminal_tab->session.SendBytes("\r");
      return true;
    case SDLK_BACKSPACE:
      terminal_tab->session.SendBytes("\x7f");
      return true;
    case SDLK_TAB:
      terminal_tab->session.SendBytes("\t");
      return true;
    case SDLK_UP:
      terminal_tab->session.SendBytes("\x1b[A");
      return true;
    case SDLK_DOWN:
      terminal_tab->session.SendBytes("\x1b[B");
      return true;
    case SDLK_RIGHT:
      terminal_tab->session.SendBytes("\x1b[C");
      return true;
    case SDLK_LEFT:
      terminal_tab->session.SendBytes("\x1b[D");
      return true;
    case SDLK_HOME:
      terminal_tab->session.SendBytes("\x1b[H");
      return true;
    case SDLK_END:
      terminal_tab->session.SendBytes("\x1b[F");
      return true;
    case SDLK_PAGEUP:
      terminal_tab->session.SendBytes("\x1b[5~");
      return true;
    case SDLK_PAGEDOWN:
      terminal_tab->session.SendBytes("\x1b[6~");
      return true;
    case SDLK_INSERT:
      terminal_tab->session.SendBytes("\x1b[2~");
      return true;
    case SDLK_DELETE:
      terminal_tab->session.SendBytes("\x1b[3~");
      return true;
    default:
      break;
  }

  return false;
}

void WorkspaceShell::SetBottomPanelVisible(bool visible) {
  bottom_panel_visible_ = visible;
  if (!bottom_panel_visible_) {
    ClearTerminalSelection();
    if (focus_ == FocusTarget::Panel) {
      focus_ = FocusTarget::Editor;
    }
    command_mode_ = false;
    command_input_.clear();
    ResetCommandSessionState();
  }
}

bool WorkspaceShell::ReopenActiveTab() {
  if (active_tab_index_ >= open_tabs_.size()) {
    LogMessage("No editor tab is active");
    return false;
  }

  auto& tab = open_tabs_[active_tab_index_];
  if (tab.kind != TabEntry::Kind::Editor) {
    LogMessage("Reopen only works for editor tabs");
    return false;
  }
  const std::filesystem::path reopen_path = text_viewport_.path().empty()
                                                ? tab.path.lexically_normal()
                                                : text_viewport_.path().lexically_normal();
  if (reopen_path.empty()) {
    LogMessage("No file is open");
    return false;
  }
  if (text_viewport_.dirty()) {
    LogMessage("Reopen blocked by unsaved changes");
    return false;
  }

  editor::TextViewport reopened_view;
  if (!reopened_view.OpenFile(reopen_path)) {
    LogMessage("Failed to reopen file: " + reopen_path.string());
    return false;
  }
  ApplyEditorPreferences(reopened_view);

  if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
    NormalizeEditorSplitTree(*tab.editor_state);
    for (auto& view : tab.editor_state->views) {
      if (view.leaf_id == tab.editor_state->active_leaf_id ||
          view.viewport.path().lexically_normal() == reopen_path) {
        view.viewport = reopened_view;
      }
    }
    text_viewport_ = reopened_view;
  } else {
    text_viewport_ = reopened_view;
    tab.editor_state = MakeEditorTabState(reopened_view);
  }
  SyncActiveEditorTabMetadata();
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  LogMessage("Reopened file from disk: " + reopen_path.string());
  return true;
}

void WorkspaceShell::ResetCommandSessionState() {
  command_history_index_.reset();
  command_history_pending_input_.clear();
  ClearCommandCompletionFeedback();
}

void WorkspaceShell::ClearCommandCompletionFeedback() {
  command_completion_feedback_.clear();
}

void WorkspaceShell::PushCommandHistory(std::string command_line) {
  if (command_line.empty()) {
    return;
  }
  if (!command_history_.empty() && command_history_.back() == command_line) {
    return;
  }

  command_history_.push_back(std::move(command_line));
  if (command_history_.size() > 64) {
    command_history_.erase(command_history_.begin());
  }
}

void WorkspaceShell::StepCommandHistory(int delta) {
  if (delta == 0 || command_history_.empty()) {
    return;
  }

  if (!command_history_index_.has_value()) {
    if (delta > 0) {
      return;
    }
    command_history_pending_input_ = command_input_;
    command_history_index_ = command_history_.size() - 1;
  } else if (delta < 0) {
    if (*command_history_index_ > 0) {
      --(*command_history_index_);
    }
  } else if (*command_history_index_ + 1 < command_history_.size()) {
    ++(*command_history_index_);
  } else {
    command_history_index_.reset();
    command_input_ = command_history_pending_input_;
    command_history_pending_input_.clear();
    ClearCommandCompletionFeedback();
    return;
  }

  command_input_ = command_history_[*command_history_index_];
  ClearCommandCompletionFeedback();
}

void WorkspaceShell::CompleteCommandInput() {
  const ParsedCommandLine parsed = ParseCommandLine(command_input_);
  if (parsed.dangling_escape) {
    command_completion_feedback_ = "Command completion stopped at a trailing escape";
    return;
  }

  const bool starts_new_token = parsed.open_quote == '\0' && parsed.trailing_space;
  const std::size_t active_index =
      starts_new_token ? parsed.tokens.size()
                       : (parsed.tokens.empty() ? 0 : parsed.tokens.size() - 1);
  const std::string command = parsed.tokens.empty() ? std::string{} : parsed.tokens.front().text;
  const std::string active_prefix =
      starts_new_token || parsed.tokens.empty() ? std::string{}
                                                : parsed.tokens.back().text;
  const std::size_t replace_start =
      starts_new_token || parsed.tokens.empty() ? command_input_.size() : parsed.tokens.back().start;
  const std::filesystem::path completion_root =
      project_root_.empty() ? std::filesystem::current_path() : project_root_;
  const std::vector<std::string>& command_names = CommandNames();

  std::vector<CommandCompletionCandidate> candidates;
  if (active_index == 0) {
    candidates = CompleteFromValues(active_prefix, command_names);
  } else if (command == "colorscheme" && active_index == 1) {
    candidates = CompleteFromValues(active_prefix, available_colorscheme_names_);
    if (StartsWith("list", active_prefix)) {
      candidates.push_back(CommandCompletionCandidate{"list", true});
    }
  } else if (command == "focus") {
    candidates = CompleteFromList(active_prefix, kFocusTargetNames);
  } else if (command == "project-open") {
    candidates = CompletePath(completion_root, active_prefix, true);
  } else if (command == "open" || command == "tab" || command == "compare" ||
             command == "term" || command == "vsplit" || command == "hsplit") {
    candidates = CompletePath(completion_root, active_prefix, false);
  } else if (command == "tabswitch") {
    const std::string lowered_prefix = ToLower(active_prefix);
    std::vector<std::string> seen_values;
    auto add_candidate = [&](std::string value) {
      if (value.empty()) {
        return;
      }
      const std::string lowered_value = ToLower(value);
      if (!StartsWith(lowered_value, lowered_prefix)) {
        return;
      }
      if (std::find(seen_values.begin(), seen_values.end(), value) != seen_values.end()) {
        return;
      }
      seen_values.push_back(value);
      candidates.push_back(CommandCompletionCandidate{std::move(value), true});
    };

    for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
      add_candidate(std::to_string(i + 1));
      add_candidate(open_tabs_[i].title);
      add_candidate(RelativePathLabel(project_root_, open_tabs_[i].path));
    }
  } else if (command == "tree" || command == "files") {
    candidates = CompletePath(completion_root, active_prefix, true);
  } else if (command == "sidebar-show" || command == "sidebar-toggle") {
    if (active_index == 1) {
      candidates = CompleteFromList(active_prefix, kSidebarToolNames);
    } else if (parsed.tokens.size() >= 2 && parsed.tokens[1].text == "tree" && active_index == 2) {
      candidates = CompletePath(completion_root, active_prefix, true);
    }
  } else if (command == "soft-tabs" && active_index == 1) {
    candidates = CompleteFromList(active_prefix, kToggleValues);
  } else if (command == "ui-scale" && active_index == 1) {
    candidates = CompleteFromList(active_prefix, kUiScaleCommands);
  } else if (command == "help") {
    candidates = CompleteFromValues(active_prefix, command_names);
  }

  if (candidates.empty()) {
    command_completion_feedback_ = "No completion matches";
    return;
  }

  const std::string common_prefix = CommonPrefix(candidates);
  const bool can_extend_prefix = common_prefix.size() > active_prefix.size();
  if (candidates.size() == 1 || can_extend_prefix) {
    CommandCompletionCandidate candidate =
        candidates.size() == 1 ? candidates.front()
                               : CommandCompletionCandidate{common_prefix, false};
    std::string replacement = FormatCommandCompletionToken(candidate);
    command_input_.erase(replace_start);
    command_input_ += replacement;
  }

  if (candidates.size() == 1) {
    command_completion_feedback_ = "Completed " + candidates.front().value;
    return;
  }

  std::string matches = "Matches:";
  const std::size_t visible_count = std::min<std::size_t>(6, candidates.size());
  for (std::size_t i = 0; i < visible_count; ++i) {
    matches += (i == 0 ? " " : "  ");
    matches += candidates[i].value;
  }
  if (candidates.size() > visible_count) {
    matches += "  ...";
  }
  command_completion_feedback_ = std::move(matches);
}

std::string WorkspaceShell::CommandPromptStatusText() const {
  if (!command_completion_feedback_.empty()) {
    return command_completion_feedback_;
  }
  if (command_history_index_.has_value()) {
    return "History " + std::to_string(*command_history_index_ + 1) + " / " +
           std::to_string(command_history_.size()) + "  |  Enter run  Esc cancel  Tab complete";
  }
  return "Enter run  Esc cancel  Up/Down history  Tab complete";
}

bool WorkspaceShell::BottomPanelShowsTerminal() const {
  return bottom_panel_mode_ == BottomPanelMode::Terminal && ActiveTerminalTab() != nullptr;
}

int WorkspaceShell::BottomPanelVisibleRows(float panel_height) const {
  const float available_height = panel_height - kBottomPanelHeaderHeight - 18.0f -
                                 BottomPanelCommandReservedHeight(command_mode_);
  return std::max(1, static_cast<int>(available_height / text_renderer_.LineHeight()));
}

int WorkspaceShell::BottomPanelScrollRow(std::size_t line_count, int visible_rows) const {
  const int max_scroll = std::max(0, static_cast<int>(line_count) - visible_rows);
  if (const auto* terminal_tab = ActiveTerminalTab(); BottomPanelShowsTerminal() &&
                                                     terminal_tab != nullptr) {
    return terminal_tab->follow_tail ? max_scroll
                                     : std::clamp(terminal_tab->scroll_row, 0, max_scroll);
  }
  return bottom_panel_follow_tail_ ? max_scroll
                                   : std::clamp(bottom_panel_scroll_row_, 0, max_scroll);
}

void WorkspaceShell::SetBottomPanelScrollRow(int scroll_row,
                                             std::size_t line_count,
                                             int visible_rows) {
  const int max_scroll = std::max(0, static_cast<int>(line_count) - visible_rows);
  const int clamped_scroll = std::clamp(scroll_row, 0, max_scroll);
  if (auto* terminal_tab = ActiveTerminalTab(); BottomPanelShowsTerminal() &&
                                               terminal_tab != nullptr) {
    terminal_tab->scroll_row = clamped_scroll;
    terminal_tab->follow_tail = clamped_scroll >= max_scroll;
    return;
  }
  bottom_panel_scroll_row_ = clamped_scroll;
  bottom_panel_follow_tail_ = clamped_scroll >= max_scroll;
}

void WorkspaceShell::ClearTerminalSelection() {
  if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    terminal_tab->mouse_selecting = false;
    terminal_tab->selection_anchor.reset();
    terminal_tab->selection_head.reset();
  }
}

bool WorkspaceShell::TerminalHasSelection() const {
  const auto* terminal_tab = ActiveTerminalTab();
  return terminal_tab != nullptr && terminal_tab->selection_anchor.has_value() &&
         terminal_tab->selection_head.has_value() &&
         (terminal_tab->selection_anchor->row != terminal_tab->selection_head->row ||
          terminal_tab->selection_anchor->column != terminal_tab->selection_head->column);
}

std::optional<WorkspaceShell::TerminalSelectionPosition>
WorkspaceShell::TerminalSelectionPositionForPoint(
    int x,
    int y,
    const std::vector<terminal::TerminalLine>& lines) const {
  if (!bottom_panel_visible_ || !BottomPanelShowsTerminal() || lines.empty() ||
      last_window_width_ <= 0 || last_window_height_ <= 0) {
    return std::nullopt;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  const SDL_FRect panel_content = BottomPanelContentRect(layout, command_mode_);
  const float text_x = panel_content.x + 12.0f;
  const float text_y = panel_content.y + 8.0f;
  const float line_height = text_renderer_.LineHeight();
  if (line_height <= 0.0f || y < text_y || y >= panel_content.y + panel_content.h) {
    return std::nullopt;
  }

  const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
  const int scroll_row = BottomPanelScrollRow(lines.size(), visible_rows);
  const int local_row = static_cast<int>((static_cast<float>(y) - text_y) / line_height);
  if (local_row < 0 || local_row >= visible_rows) {
    return std::nullopt;
  }

  const std::size_t row = std::min<std::size_t>(static_cast<std::size_t>(scroll_row + local_row),
                                                lines.size() - 1);
  const float local_x = std::max(0.0f, static_cast<float>(x) - text_x);
  const std::size_t column = static_cast<std::size_t>(
      std::max(0L, std::lround(local_x / std::max(1.0f, text_renderer_.CharWidth()))));
  return TerminalSelectionPosition{
      .row = row,
      .column = std::min(column, lines[row].cells.size()),
  };
}

std::optional<WorkspaceShell::TerminalSelectionPosition>
WorkspaceShell::TerminalViewportPositionForPoint(int x, int y) const {
  if (!bottom_panel_visible_ || !BottomPanelShowsTerminal() || last_window_width_ <= 0 ||
      last_window_height_ <= 0) {
    return std::nullopt;
  }

  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return std::nullopt;
  }

  const std::size_t rows = terminal_tab->session.rows();
  const std::size_t columns = terminal_tab->session.columns();
  if (rows == 0 || columns == 0) {
    return std::nullopt;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  const SDL_FRect panel_content = BottomPanelContentRect(layout, command_mode_);
  if (!Contains(panel_content, x, y)) {
    return std::nullopt;
  }

  const float text_x = panel_content.x + 12.0f;
  const float text_y = panel_content.y + 8.0f;
  const float line_height = text_renderer_.LineHeight();
  const float char_width = std::max(1.0f, text_renderer_.CharWidth());
  if (line_height <= 0.0f || y < text_y) {
    return std::nullopt;
  }

  const std::size_t row = static_cast<std::size_t>(
      std::max(0.0f, static_cast<float>(std::floor((static_cast<float>(y) - text_y) / line_height))));
  if (row >= rows) {
    return std::nullopt;
  }

  const float local_x = std::max(0.0f, static_cast<float>(x) - text_x);
  const std::size_t column = static_cast<std::size_t>(std::floor(local_x / char_width));
  return TerminalSelectionPosition{
      .row = row,
      .column = std::min(column, columns - 1),
  };
}

terminal::TerminalSession::MouseButton WorkspaceShell::TerminalMouseButtonForSdl(Uint8 button) const {
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

std::string WorkspaceShell::SelectedTerminalText(
    const std::vector<terminal::TerminalLine>& lines) const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr || !TerminalHasSelection() || lines.empty()) {
    return {};
  }

  TerminalSelectionPosition start = *terminal_tab->selection_anchor;
  TerminalSelectionPosition end = *terminal_tab->selection_head;
  if (start.row > end.row || (start.row == end.row && start.column > end.column)) {
    std::swap(start, end);
  }

  if (start.row >= lines.size()) {
    return {};
  }
  end.row = std::min(end.row, lines.size() - 1);

  std::string text;
  for (std::size_t row = start.row; row <= end.row; ++row) {
    const auto& line = lines[row];
    const std::size_t line_size = line.cells.size();
    const std::size_t start_column = row == start.row ? std::min(start.column, line_size) : 0;
    const std::size_t end_column = row == end.row ? std::min(end.column, line_size) : line_size;
    for (std::size_t column = start_column; column < end_column; ++column) {
      text.push_back(line.cells[column].character);
    }
    if (row != end.row) {
      text.push_back('\n');
    }
  }
  return text;
}

bool WorkspaceShell::TerminalCellSelected(std::size_t row, std::size_t column) const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr || !TerminalHasSelection()) {
    return false;
  }

  TerminalSelectionPosition start = *terminal_tab->selection_anchor;
  TerminalSelectionPosition end = *terminal_tab->selection_head;
  if (start.row > end.row || (start.row == end.row && start.column > end.column)) {
    std::swap(start, end);
  }

  if (row < start.row || row > end.row) {
    return false;
  }
  if (start.row == end.row) {
    return row == start.row && column >= start.column && column < end.column;
  }
  if (row == start.row) {
    return column >= start.column;
  }
  if (row == end.row) {
    return column < end.column;
  }
  return true;
}

std::string WorkspaceShell::BottomPanelHeaderLabel() const {
  if (!BottomPanelShowsTerminal()) {
    return "Bottom Panel | Logs";
  }

  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return "Bottom Panel | Logs";
  }

  const std::string launch_label = terminal_tab->session.LaunchLabel();
  if (launch_label.empty()) {
    return "Bottom Panel | Terminal";
  }
  return "Bottom Panel | Terminal | " + launch_label;
}

void WorkspaceShell::ResizeTerminalToPanel(const SDL_FRect& panel_rect) {
  auto* terminal_tab = ActiveTerminalTab();
  if (!BottomPanelShowsTerminal() || terminal_tab == nullptr) {
    return;
  }

  const int rows = BottomPanelVisibleRows(panel_rect.h);
  const float usable_width =
      std::max(16.0f, panel_rect.w - 24.0f - kScrollbarThickness - 6.0f);
  const int columns = std::max(
      1, static_cast<int>(std::floor(usable_width / std::max(1.0f, text_renderer_.CharWidth()))));
  terminal_tab->session.Resize(static_cast<std::size_t>(rows), static_cast<std::size_t>(columns));
}

bool WorkspaceShell::ExecuteAction(ActionId id,
                                   const std::vector<std::string>& args,
                                   ActionSource source) {
  if (source != ActionSource::ContextMenu) {
    CloseTreeContextMenu();
  }

  const auto require_project = [&]() {
    if (!project_root_.empty()) {
      return false;
    }
    LogMessage("No project is loaded");
    return true;
  };

  switch (id) {
    case ActionId::Help:
      if (source == ActionSource::Menu) {
        bottom_panel_mode_ = BottomPanelMode::Logs;
        SetBottomPanelVisible(true);
      }
      LogMessage("Commands: " + CommandHelpSummary());
      return true;
    case ActionId::Colorscheme:
      if (args.empty()) {
        LogMessage("Colorscheme: " + active_colorscheme_name_);
        return true;
      }
      if (args[0] == "list") {
        RefreshAvailableColorschemeNames();
        if (available_colorscheme_names_.empty()) {
          LogMessage("No bundled colorschemes found");
        } else {
          LogMessage("Colorschemes: " + JoinCommandArguments(available_colorscheme_names_, 0));
        }
        return true;
      }
      RefreshAvailableColorschemeNames();
      ApplyColorscheme(args[0], true, true);
      return true;
    case ActionId::ProjectOpen:
      if (args.empty()) {
        if (source == ActionSource::Menu) {
          command_mode_ = true;
          SetBottomPanelVisible(true);
          command_input_ = "project-open ";
          ResetCommandSessionState();
          LogMessage("Enter a project path");
          return true;
        }
        LogMessage("usage: project-open <path>");
        return true;
      }
      OpenProjectTab(std::filesystem::path(args[0]), true, true);
      return true;
    case ActionId::ProjectClose:
      if (projects_.empty() || project_root_.empty()) {
        LogMessage("No project is loaded");
        return true;
      }
      RequestCloseProject(active_project_index_);
      return true;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev: {
      if (projects_.empty() || project_root_.empty()) {
        LogMessage("No project is loaded");
        return true;
      }
      if (projects_.size() == 1) {
        LogMessage("Only one project is open");
        return true;
      }
      const int delta = id == ActionId::ProjectNext ? 1 : -1;
      const int project_count = static_cast<int>(projects_.size());
      const int next_index =
          (static_cast<int>(active_project_index_) + delta + project_count) % project_count;
      SwitchProject(static_cast<std::size_t>(next_index), true);
      return true;
    }
    case ActionId::SidebarToggle: {
      const std::string tool = args.empty() ? std::string{} : args[0];
      if (tool == "tree") {
        if (sidebar_visible_ && sidebar_mode_ == SidebarMode::Tree) {
          CloseSidebar();
        } else {
          const std::filesystem::path root_arg =
              args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::path{};
          ShowTreeSidebar(root_arg);
        }
        return true;
      }
      if (tool == "search") {
        const std::string query = JoinCommandArguments(args, 1);
        if (sidebar_visible_ && sidebar_mode_ == SidebarMode::Search && !sidebar_temporary_) {
          CloseSidebar();
        } else {
          ShowSearchSidebar(query, false);
        }
        return true;
      }
      ToggleSidebar();
      return true;
    }
    case ActionId::SidebarShow: {
      const std::string tool = args.empty() ? std::string{} : args[0];
      if (tool == "tree") {
        const std::filesystem::path root_arg =
            args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::path{};
        ShowTreeSidebar(root_arg);
        return true;
      }
      if (tool == "search") {
        ShowSearchSidebar(JoinCommandArguments(args, 1), false);
        return true;
      }
      sidebar_visible_ = true;
      focus_ = FocusTarget::Sidebar;
      LogMessage("Sidebar shown");
      return true;
    }
    case ActionId::SidebarHide:
    case ActionId::SidebarClose:
      CloseSidebar();
      return true;
    case ActionId::SidebarWidth:
      if (args.empty()) {
        LogMessage("usage: sidebar-width <n>");
        return true;
      }
      try {
        const float width = std::stof(args[0]);
        sidebar_width_ =
            ClampSidebarWidth(width, static_cast<float>(std::max(1, last_window_width_)));
        LogMessage("Sidebar width updated");
      } catch (...) {
        LogMessage("Invalid sidebar width");
      }
      return true;
    case ActionId::TabSize:
      if (args.empty()) {
        LogMessage("Tab size: " + std::to_string(editor_preferences_.tab_size));
        return true;
      }
      try {
        editor_preferences_.tab_size =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(args[0])), 1, 16);
        ApplyEditorPreferencesToAllTabs();
        SaveConfigState();
        LogMessage("Tab size set to " + std::to_string(editor_preferences_.tab_size));
      } catch (...) {
        LogMessage("Invalid tab size");
      }
      return true;
    case ActionId::IndentWidth:
      if (args.empty()) {
        LogMessage("Indent width: " + std::to_string(editor_preferences_.indent_width));
        return true;
      }
      try {
        editor_preferences_.indent_width =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(args[0])), 1, 16);
        ApplyEditorPreferencesToAllTabs();
        SaveConfigState();
        LogMessage("Indent width set to " + std::to_string(editor_preferences_.indent_width));
      } catch (...) {
        LogMessage("Invalid indent width");
      }
      return true;
    case ActionId::UiScale:
      if (args.empty()) {
        LogMessage("UI scale: " + UiScaleLabel(ui_scale_));
        return true;
      }
      if (args[0] == "up") {
        ApplyUiScale(StepUiScale(ui_scale_, 1), true, true);
        return true;
      }
      if (args[0] == "down") {
        ApplyUiScale(StepUiScale(ui_scale_, -1), true, true);
        return true;
      }
      if (args[0] == "reset") {
        ApplyUiScale(1.0f, true, true);
        return true;
      }
      if (const auto scale = ParseUiScaleValue(args[0]); scale.has_value()) {
        ApplyUiScale(*scale, true, true);
      } else {
        LogMessage("usage: ui-scale <n|up|down|reset>");
      }
      return true;
    case ActionId::SoftTabs:
      if (args.empty()) {
        LogMessage(std::string("Soft tabs: ") + (editor_preferences_.soft_tabs ? "on" : "off"));
        return true;
      }
      if (const std::string value = ToLower(args[0]);
          value != "on" && value != "off" && value != "true" && value != "false" &&
          value != "1" && value != "0") {
        LogMessage("usage: soft-tabs <on|off>");
        return true;
      } else {
        editor_preferences_.soft_tabs =
            value == "on" || value == "true" || value == "1";
      }
      ApplyEditorPreferencesToAllTabs();
      SaveConfigState();
      LogMessage(std::string("Soft tabs ") + (editor_preferences_.soft_tabs ? "enabled"
                                                                            : "disabled"));
      return true;
    case ActionId::PanelShow:
      SetBottomPanelVisible(true);
      LogMessage("Bottom panel shown");
      return true;
    case ActionId::PanelHide:
      SetBottomPanelVisible(false);
      LogMessage("Bottom panel hidden");
      return true;
    case ActionId::ToggleBottomPanel:
      SetBottomPanelVisible(!bottom_panel_visible_);
      LogMessage(std::string("Bottom panel ") + (bottom_panel_visible_ ? "shown" : "hidden"));
      return true;
    case ActionId::TreeRefresh:
      if (require_project()) {
        return true;
      }
      RefreshProjectFiles();
      LogMessage("Project tree refreshed");
      return true;
    case ActionId::CreateFile:
    case ActionId::CreateDirectory: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path base_path = TreeMutationBasePath(source);
      if (base_path.empty()) {
        LogMessage("No tree directory is selected");
        return true;
      }
      OpenPromptSurface(id == ActionId::CreateFile ? PromptSurfaceState::Action::CreateFile
                                                   : PromptSurfaceState::Action::CreateDirectory,
                        PromptSurfaceState::Kind::TextInput, base_path);
      return true;
    }
    case ActionId::RenamePath: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree path is selected");
        return true;
      }
      OpenPromptSurface(PromptSurfaceState::Action::RenamePath,
                        PromptSurfaceState::Kind::TextInput, path, path.filename().string());
      return true;
    }
    case ActionId::DeletePath: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree path is selected");
        return true;
      }
      OpenPromptSurface(PromptSurfaceState::Action::DeletePath,
                        PromptSurfaceState::Kind::Confirm, path);
      return true;
    }
    case ActionId::CopyRelativePath:
    case ActionId::CopyAbsolutePath: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree path is selected");
        return true;
      }

      std::string clipboard_text;
      if (id == ActionId::CopyRelativePath) {
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(path, project_root_, error);
        if (error || relative.empty()) {
          LogMessage("Failed to compute relative path");
          return true;
        }
        clipboard_text = relative.generic_string();
      } else {
        clipboard_text = path.lexically_normal().string();
      }

      if (SDL_SetClipboardText(clipboard_text.c_str())) {
        LogMessage(std::string(id == ActionId::CopyRelativePath ? "Relative" : "Absolute") +
                   " path copied");
      } else {
        LogMessage("Failed to copy path");
      }
      return true;
    }
    case ActionId::Focus: {
      const std::string target = args.empty() ? std::string{} : args[0];
      if (target == "sidebar" && sidebar_visible_) {
        focus_ = FocusTarget::Sidebar;
        LogMessage("Focus moved to sidebar");
        return true;
      }
      if (target == "editor") {
        focus_ = FocusTarget::Editor;
        LogMessage("Focus moved to editor");
        return true;
      }
      if (target == "panel" && bottom_panel_visible_ && BottomPanelShowsTerminal()) {
        focus_ = FocusTarget::Panel;
        LogMessage("Focus moved to terminal panel");
        return true;
      }
      LogMessage("Unknown focus target");
      return true;
    }
    case ActionId::Term:
      if (require_project()) {
        return true;
      }
      OpenTerminal(JoinCommandArguments(args, 0));
      return true;
    case ActionId::Find:
      if (require_project()) {
        return true;
      }
      file_index_.Refresh();
      file_finder_.SetIndex(&file_index_);
      file_finder_.SetQuery(JoinCommandArguments(args, 0));
      overlay_visible_ = true;
      overlay_mode_ = OverlayMode::FileFinder;
      focus_ = FocusTarget::Overlay;
      ResetOverlayScroll();
      LogMessage("Finder opened from command");
      return true;
    case ActionId::Files: {
      const std::string root_arg = args.empty() ? std::string{} : args[0];
      if (!root_arg.empty() && !OpenProjectTab(root_arg, true, true)) {
        return true;
      }
      if (source == ActionSource::Shortcut && overlay_visible_) {
        overlay_visible_ = false;
        focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
        LogMessage("Finder overlay closed");
        return true;
      }
      if (source != ActionSource::Shortcut && require_project()) {
        return true;
      }
      overlay_visible_ = true;
      overlay_mode_ = OverlayMode::FileFinder;
      file_index_.Refresh();
      file_finder_.SetIndex(&file_index_);
      file_finder_.SetQuery("");
      focus_ = FocusTarget::Overlay;
      ResetOverlayScroll();
      LogMessage(source == ActionSource::Shortcut ? "Finder overlay opened" : "Finder opened");
      return true;
    }
    case ActionId::Tree: {
      const std::filesystem::path root_arg =
          args.empty() ? std::filesystem::path{} : std::filesystem::path(args[0]);
      if (root_arg.empty() && require_project()) {
        return true;
      }
      ShowTreeSidebar(root_arg);
      return true;
    }
    case ActionId::Grep:
      if (require_project()) {
        return true;
      }
      ShowSearchSidebar(JoinCommandArguments(args, 0), false);
      return true;
    case ActionId::Rg:
      if (require_project()) {
        return true;
      }
      ShowSearchSidebar(JoinCommandArguments(args, 0), true);
      return true;
    case ActionId::Search:
      if (require_project()) {
        return true;
      }
      if (ActiveTabIsCompare()) {
        LogMessage("search only works in editor tabs");
        return true;
      }
      OpenBufferSearch();
      buffer_search_query_ = JoinCommandArguments(args, 0);
      RefreshBufferSearch();
      LogMessage("Buffer search opened");
      return true;
    case ActionId::ReplaceInBuffer:
      OpenBufferReplace();
      return true;
    case ActionId::Open:
      if (require_project()) {
        return true;
      }
      if (args.empty()) {
        LogMessage("usage: open <path>");
        return true;
      }
      {
        std::filesystem::path path = args[0];
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();

        auto* editor_tab = ActiveEditorTab();
        if (editor_tab != nullptr && editor_tab->views.size() > 1) {
          editor::TextViewport opened_view;
          if (!opened_view.OpenFile(path)) {
            LogMessage("Failed to open file: " + path.string());
            return true;
          }
          if (!ReplaceActiveEditorView(opened_view)) {
            LogMessage("Failed to open file in active split: " + path.string());
            return true;
          }
          LogMessage("Opened file in active split: " + path.string());
          return true;
        }

        OpenFile(path);
        return true;
      }
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree file is selected");
        return true;
      }
      if (id == ActionId::OpenSelectedTreeItemInNewTab) {
        if (!OpenFileInNewTab(path)) {
          LogMessage("Failed to open file: " + path.string());
          return true;
        }
        LogMessage("Opened tab: " + open_tabs_[active_tab_index_].title);
      } else {
        OpenFile(path);
      }
      return true;
    }
    case ActionId::Compare: {
      if (require_project()) {
        return true;
      }
      std::filesystem::path path;
      if (!args.empty()) {
        path = std::filesystem::path(args[0]);
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();
      } else if (source == ActionSource::ContextMenu) {
        path = ResolveTreeActionPath(source);
      } else if (!text_viewport_.path().empty()) {
        path = text_viewport_.path().lexically_normal();
      } else if (sidebar_visible_ && sidebar_mode_ == SidebarMode::Tree) {
        const auto& entries = directory_tree_.entries();
        if (directory_tree_.selected_index() < entries.size() &&
            !entries[directory_tree_.selected_index()].is_directory) {
          path = entries[directory_tree_.selected_index()].path.lexically_normal();
        }
      }

      if (path.empty()) {
        LogMessage("usage: compare [path] [commit-prefix]");
        return true;
      }

      if (!std::filesystem::exists(path)) {
        LogMessage("File does not exist: " + path.string());
        return true;
      }

      const std::string commit_spec = args.size() > 1 ? args[1] : "";
      OpenComparePickerForPath(path, commit_spec);
      return true;
    }
    case ActionId::CompareHead: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree file is selected");
        return true;
      }
      if (!std::filesystem::exists(path)) {
        LogMessage("File does not exist: " + path.string());
        return true;
      }
      compare_picker_path_ = path.lexically_normal();
      OpenComparison(project::GitCommitEntry{
          .hash = "HEAD",
          .short_hash = "HEAD",
          .subject = "HEAD",
      });
      return true;
    }
    case ActionId::Tab:
      if (require_project()) {
        return true;
      }
      if (args.empty()) {
        OpenUntitledTab();
        LogMessage("Opened untitled tab");
        return true;
      }

      for (const std::string& arg : args) {
        std::filesystem::path path = arg;
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();

        if (!OpenFileInNewTab(path)) {
          LogMessage("Failed to open file: " + path.string());
          return true;
        }
      }

      LogMessage("Opened tab: " + open_tabs_[active_tab_index_].title);
      return true;
    case ActionId::TabSwitch: {
      if (require_project()) {
        return true;
      }
      std::string error_message;
      const std::optional<std::size_t> tab_index =
          FindTabIndexBySpecifier(JoinCommandArguments(args, 0), &error_message);
      if (!tab_index.has_value()) {
        LogMessage(error_message);
        return true;
      }
      ActivateTab(*tab_index);
      LogMessage("Switched to tab: " + open_tabs_[*tab_index].title);
      return true;
    }
    case ActionId::TabMove:
      if (require_project()) {
        return true;
      }
      if (args.empty()) {
        LogMessage("usage: tabmove <n>");
        return true;
      }
      if (open_tabs_.empty()) {
        LogMessage("No tabs are open");
        return true;
      }
      {
        std::size_t parsed_length = 0;
        int slot = 0;
        try {
          slot = std::stoi(args[0], &parsed_length);
        } catch (...) {
          LogMessage("Invalid tab slot");
          return true;
        }
        if (parsed_length != args[0].size()) {
          LogMessage("Invalid tab slot");
          return true;
        }

        const bool relative = !args[0].empty() && (args[0].front() == '+' || args[0].front() == '-');
        const int current_slot = static_cast<int>(active_tab_index_) + 1;
        const int requested_slot = relative ? current_slot + slot : slot;
        const int clamped_slot =
            std::clamp(requested_slot, 1, static_cast<int>(open_tabs_.size()));
        MoveActiveTabTo(static_cast<std::size_t>(clamped_slot - 1));
        LogMessage("Moved tab to slot " + std::to_string(clamped_slot));
        return true;
      }
    case ActionId::Reopen:
      if (require_project()) {
        return true;
      }
      ReopenActiveTab();
      return true;
    case ActionId::Save:
      if (require_project()) {
        return true;
      }
      if (SaveTab(active_tab_index_)) {
        if (source == ActionSource::Shortcut) {
          ResetCaretBlink();
        }
        LogMessage("Saved file: " + text_viewport_.path().lexically_normal().string());
      } else {
        LogMessage("Save failed");
      }
      return true;
    case ActionId::Vsplit:
    case ActionId::Hsplit: {
      if (require_project()) {
        return true;
      }
      const EditorSplitOrientation orientation =
          id == ActionId::Vsplit ? EditorSplitOrientation::Vertical
                                 : EditorSplitOrientation::Horizontal;
      const std::string command = id == ActionId::Vsplit ? "vsplit" : "hsplit";
      const std::string split_label = id == ActionId::Vsplit ? "Vertical" : "Horizontal";

      if (args.empty()) {
        if (!SplitActiveEditor(orientation)) {
          LogMessage(command + " only works in editor tabs");
        } else {
          LogMessage(split_label + " split opened");
        }
        return true;
      }

      for (const std::string& arg : args) {
        std::filesystem::path path = arg;
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();

        editor::TextViewport opened_view;
        if (!opened_view.OpenFile(path)) {
          LogMessage("Failed to open file: " + path.string());
          return true;
        }
        if (!SplitActiveEditor(orientation)) {
          LogMessage(command + " only works in editor tabs");
          return true;
        }
        if (!ReplaceActiveEditorView(opened_view)) {
          LogMessage("Failed to load file into split: " + path.string());
          return true;
        }
      }

      LogMessage(split_label + " split opened");
      return true;
    }
    case ActionId::Unsplit:
      if (!UnsplitActiveEditor()) {
        LogMessage("No editor split is active");
      } else {
        LogMessage("Editor split closed");
      }
      return true;
    case ActionId::SplitNext:
      if (!CycleEditorSplit(1)) {
        LogMessage("No other split is available");
      } else {
        LogMessage("Focus moved to the next split");
      }
      return true;
    case ActionId::SplitPrev:
      if (!CycleEditorSplit(-1)) {
        LogMessage("No other split is available");
      } else {
        LogMessage("Focus moved to the previous split");
      }
      return true;
    case ActionId::SplitFirst:
      if (!ActivateOrderedEditorSplit(0)) {
        LogMessage("No other split is available");
      } else {
        LogMessage("Focus moved to the first split");
      }
      return true;
    case ActionId::SplitLast: {
      auto* editor_tab = ActiveEditorTab();
      const std::size_t last_index =
          editor_tab == nullptr || editor_tab->views.empty() ? 0 : editor_tab->views.size() - 1;
      if (!ActivateOrderedEditorSplit(last_index)) {
        LogMessage("No other split is available");
      } else {
        LogMessage("Focus moved to the last split");
      }
      return true;
    }
    case ActionId::Quit:
      RequestQuit();
      return true;
    case ActionId::Goto:
    case ActionId::Jump: {
      const std::string command = id == ActionId::Goto ? "goto" : "jump";
      if (ActiveTabIsCompare()) {
        LogMessage(command + " only works in editor tabs");
        return true;
      }
      if (args.empty()) {
        LogMessage("usage: " + command + " <line[:col]>");
        return true;
      }

      long long requested_line = 0;
      std::size_t column = 0;
      if (!ParseLineColumnSpec(args[0], &requested_line, &column, id == ActionId::Jump)) {
        LogMessage("Invalid " + command + " target");
        return true;
      }

      if (id == ActionId::Goto && requested_line == 0) {
        LogMessage("goto expects 1-based positions");
        return true;
      }

      const std::size_t line_count = std::max<std::size_t>(1, text_viewport_.line_count());
      std::size_t line = 0;
      if (id == ActionId::Jump) {
        const long long current_line = static_cast<long long>(text_viewport_.cursor_line()) + 1;
        const long long target_line = current_line + requested_line;
        line = static_cast<std::size_t>(
            std::clamp(target_line - 1, 0LL, static_cast<long long>(line_count - 1)));
      } else if (requested_line > 0) {
        line = static_cast<std::size_t>(requested_line - 1);
      } else {
        const std::size_t from_end = static_cast<std::size_t>(-requested_line);
        line = from_end >= line_count ? 0 : line_count - from_end;
      }

      text_viewport_.MoveCursorTo(line, column > 0 ? column - 1 : 0);
      focus_ = FocusTarget::Editor;
      LogMessage("Cursor moved to requested location");
      return true;
    }
    case ActionId::CloseActiveTab:
      if (!open_tabs_.empty()) {
        RequestCloseTab(active_tab_index_);
      }
      return true;
    case ActionId::OpenCommandPrompt:
      command_mode_ = true;
      SetBottomPanelVisible(true);
      command_input_.clear();
      ResetCommandSessionState();
      LogMessage("Command mode opened");
      return true;
    case ActionId::SelectAll:
      text_viewport_.SelectAll();
      ResetCaretBlink();
      focus_ = FocusTarget::Editor;
      return true;
    case ActionId::Undo:
      if (text_viewport_.Undo()) {
        LogMessage("Undo");
        ResetCaretBlink();
      }
      return true;
    case ActionId::Redo:
      if (text_viewport_.Redo()) {
        LogMessage("Redo");
        ResetCaretBlink();
      }
      return true;
    case ActionId::CopySelection: {
      const std::string text = text_viewport_.SelectedText();
      if (!text.empty() && SDL_SetClipboardText(text.c_str())) {
        LogMessage("Selection copied");
      }
      return true;
    }
    case ActionId::CutSelection: {
      const std::string text = text_viewport_.SelectedText();
      if (!text.empty() && SDL_SetClipboardText(text.c_str())) {
        text_viewport_.DeleteSelectedText();
        ResetCaretBlink();
        LogMessage("Selection cut");
      }
      return true;
    }
    case ActionId::PasteClipboard: {
      char* clipboard_text = SDL_GetClipboardText();
      if (clipboard_text != nullptr) {
        text_viewport_.InsertText(clipboard_text);
        ResetCaretBlink();
        SDL_free(clipboard_text);
        LogMessage("Clipboard pasted");
      }
      return true;
    }
  }

  return true;
}

bool WorkspaceShell::ExecuteCommand(const std::string& command_line) {
  const ParsedCommandLine parsed = ParseCommandLine(command_line);
  if (parsed.dangling_escape) {
    command_completion_feedback_ = "Command parse error: trailing escape";
    LogMessage(command_completion_feedback_);
    return false;
  }
  if (parsed.open_quote != '\0') {
    command_completion_feedback_ = std::string("Command parse error: unterminated ") +
                                   (parsed.open_quote == '\'' ? "single" : "double") + " quote";
    LogMessage(command_completion_feedback_);
    return false;
  }
  if (parsed.tokens.empty()) {
    return true;
  }

  PushCommandHistory(command_line);
  ClearCommandCompletionFeedback();
  const std::string& command = parsed.tokens.front().text;
  const ActionSpec* action = FindActionByCommand(command);
  if (action == nullptr) {
    LogMessage("Unknown command: " + command);
    return true;
  }

  std::vector<std::string> args;
  args.reserve(parsed.tokens.size() - 1);
  for (std::size_t i = 1; i < parsed.tokens.size(); ++i) {
    args.push_back(parsed.tokens[i].text);
  }
  return ExecuteAction(action->id, args, ActionSource::Command);
}

void WorkspaceShell::LogMessage(std::string message) {
  if (message.empty()) {
    return;
  }
  if (bottom_panel_follow_tail_) {
    bottom_panel_scroll_row_ = std::max(0, static_cast<int>(log_messages_.size()) - 1);
  }
  log_messages_.push_back(std::move(message));
  if (log_messages_.size() > 128) {
    log_messages_.erase(log_messages_.begin(),
                        log_messages_.begin() + static_cast<std::ptrdiff_t>(log_messages_.size() - 128));
    bottom_panel_scroll_row_ = std::max(0, bottom_panel_scroll_row_ - 1);
  }
}

void WorkspaceShell::Render(SDL_Renderer* renderer, int width, int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return;
  }

  ConsumeProjectSearchUpdates();
  text_renderer_.EnsureInitialized(renderer);
  last_window_width_ = width;
  last_window_height_ = height;
  sidebar_width_ = ClampSidebarWidth(sidebar_width_, static_cast<float>(width));
  bottom_panel_height_ = ClampBottomPanelHeight(bottom_panel_height_, static_cast<float>(height));

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(width), static_cast<float>(height), sidebar_visible_,
                    bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  SyncTextInputSurface(render_window);
  if (ActiveTabIsEditor()) {
    SyncActiveEditorTab();
    if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
      NormalizeEditorSplitTree(*editor_tab);
    }
  }
  if (bottom_panel_visible_ && BottomPanelShowsTerminal()) {
    ResizeTerminalToPanel(layout.bottom_panel);
  }
  float mouse_x = 0.0f;
  float mouse_y = 0.0f;
  SDL_GetMouseState(&mouse_x, &mouse_y);
  UpdateMouseCursor(mouse_x, mouse_y);
  const std::vector<terminal::TerminalLine> terminal_lines =
      bottom_panel_visible_ && BottomPanelShowsTerminal() && ActiveTerminalTab() != nullptr
          ? ActiveTerminalTab()->session.SnapshotLines()
          : std::vector<terminal::TerminalLine>{};
  std::optional<SDL_FRect> active_editor_pane_rect;
  const bool draw_editor_caret =
      CaretVisibleNow() &&
      !(CurrentTextInputSurface() == TextInputSurface::Editor && !text_composition_.text.empty());

  DrawFilledRect(renderer, layout.full, theme_.window_background);
  DrawFilledRect(renderer, layout.menu_bar, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.menu_bar.x, layout.menu_bar.y + layout.menu_bar.h - kDivider,
                          layout.menu_bar.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.project_tab_strip, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.project_tab_strip.x,
                          layout.project_tab_strip.y + layout.project_tab_strip.h - kDivider,
                          layout.project_tab_strip.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.tab_strip, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.tab_strip.x, layout.tab_strip.y + layout.tab_strip.h - kDivider,
                          layout.tab_strip.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.breadcrumb, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.breadcrumb.x, layout.breadcrumb.y + layout.breadcrumb.h - kDivider,
                          layout.breadcrumb.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.status_bar, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.status_bar.x, layout.status_bar.y, layout.status_bar.w, kDivider),
                 theme_.border);

  if (sidebar_visible_) {
    DrawFilledRect(renderer, layout.sidebar, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.sidebar.x + layout.sidebar.w, layout.sidebar.y, kDivider,
                            layout.sidebar.h),
                   drag_target_ == DragTarget::SidebarDivider ? theme_.accent : theme_.border);
    const SDL_FRect sidebar_header =
        MakeRect(layout.sidebar.x, layout.sidebar.y, layout.sidebar.w, kSidebarHeaderHeight);
    DrawFilledRect(renderer, sidebar_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(sidebar_header.x, sidebar_header.y + sidebar_header.h - kDivider,
                            sidebar_header.w, kDivider),
                   theme_.border);
  }

  if (bottom_panel_visible_) {
    DrawFilledRect(renderer, layout.bottom_panel, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                            kDivider),
                   drag_target_ == DragTarget::BottomPanelDivider ? theme_.accent : theme_.border);
    const SDL_FRect panel_header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y,
                                            layout.bottom_panel.w, kBottomPanelHeaderHeight);
    DrawFilledRect(renderer, panel_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(panel_header.x, panel_header.y + panel_header.h - kDivider,
                            panel_header.w, kDivider),
                   theme_.border);
  }

  if (ActiveTabIsCompare()) {
    RenderCompareSurface(renderer, layout.editor_surface);
  } else {
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    if (panes.empty() && text_viewport_.is_placeholder()) {
      active_editor_pane_rect = layout.editor_surface;
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, text_viewport_,
                                   layout.editor_surface, draw_editor_caret, "", std::nullopt);
    }
    auto* editor_tab = ActiveEditorTab();
    for (const EditorPaneLayout& pane : panes) {
      editor::TextViewport* viewport =
          pane.active ? &text_viewport_
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id)
                                               : nullptr);
      if (viewport == nullptr) {
        continue;
      }
      if (pane.active) {
        active_editor_pane_rect = pane.rect;
      }
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, *viewport, pane.rect,
                                   pane.active && draw_editor_caret,
                                   pane.active && (overlay_mode_ == OverlayMode::BufferSearch ||
                                                   overlay_mode_ == OverlayMode::BufferReplace)
                                       ? buffer_search_query_
                                       : "",
                                   pane.active ? ActiveBufferSearchMatch() : std::nullopt);
    }
  }

  const auto draw_text_on =
      [&](float x, float y, SDL_Color foreground, SDL_Color background, std::string_view text) {
        text_renderer_.DrawStringOn(renderer, x, y, foreground, background, text);
      };
  struct TextInputVisual {
    TextInputSurface surface = TextInputSurface::None;
    SDL_FRect area{};
    float text_x = 0.0f;
    float text_y = 0.0f;
    float cursor_x = 0.0f;
    SDL_Color foreground{};
    SDL_Color background{};
  };
  const auto active_text_input_visual = [&]() -> std::optional<TextInputVisual> {
    const TextInputSurface surface = CurrentTextInputSurface();
    const float line_height = text_renderer_.LineHeight();
    const float char_width = std::max(1.0f, text_renderer_.CharWidth());

    switch (surface) {
      case TextInputSurface::Editor: {
        if (!active_editor_pane_rect.has_value()) {
          return std::nullopt;
        }
        const editor::EditorViewMetrics metrics =
            editor::EditorViewRenderer::ComputeMetrics(text_renderer_, text_viewport_,
                                                       *active_editor_pane_rect);
        const float cursor_x =
            metrics.text_x +
            static_cast<float>(text_viewport_.cursor_visual_column() - text_viewport_.horizontal_scroll()) *
                char_width;
        const float cursor_y =
            metrics.first_line_y +
            static_cast<float>(text_viewport_.cursor_line() - text_viewport_.scroll_line()) *
                metrics.line_height;
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(cursor_x, cursor_y - 1.0f, char_width, metrics.line_height),
            .text_x = cursor_x,
            .text_y = cursor_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.row_highlight,
        };
      }
      case TextInputSurface::Command: {
        const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
        const float text_x = prompt_rect.x + 6.0f;
        const float text_y = prompt_rect.y + 4.0f;
        const float cursor_x = text_x + text_renderer_.MeasureWidth("> " + command_input_);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, prompt_rect.w - 12.0f), line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.chrome_active,
        };
      }
      case TextInputSurface::PromptInput: {
        const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
        const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
        const float text_x = input_rect.x + 6.0f;
        const float text_y = input_rect.y + 4.0f;
        const float cursor_x = text_x + text_renderer_.MeasureWidth(prompt_surface_state_.input);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, input_rect.w - 12.0f), line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.surface_background,
        };
      }
      case TextInputSurface::FileFinder:
      case TextInputSurface::BufferSearch:
      case TextInputSurface::BufferReplaceSearch:
      case TextInputSurface::BufferReplaceReplace:
      case TextInputSurface::ProjectSearchOverlay:
      case TextInputSurface::CommitPicker: {
        if (!overlay_visible_) {
          return std::nullopt;
        }
        const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
        const float inset = 18.0f;
        float text_x = overlay.x + inset;
        float text_y = overlay.y + 44.0f;
        std::string prefix = "> ";
        switch (surface) {
          case TextInputSurface::BufferSearch:
            prefix += buffer_search_query_;
            break;
          case TextInputSurface::BufferReplaceSearch:
            prefix = "find: " + buffer_search_query_;
            break;
          case TextInputSurface::BufferReplaceReplace:
            text_y = overlay.y + 62.0f;
            prefix = "replace: " + buffer_replace_text_;
            break;
          case TextInputSurface::ProjectSearchOverlay:
            prefix += project_search_query_;
            break;
          case TextInputSurface::CommitPicker:
            text_y = overlay.y + 62.0f;
            prefix += compare_picker_query_;
            break;
          case TextInputSurface::FileFinder:
          default:
            prefix += file_finder_.query();
            break;
        }
        const float cursor_x = text_x + text_renderer_.MeasureWidth(prefix);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, overlay.w - inset * 2.0f), line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_secondary,
            .background = theme_.overlay_background,
        };
      }
      case TextInputSurface::SidebarSearchQuery:
      case TextInputSurface::SidebarSearchReplace: {
        if (!sidebar_visible_ || sidebar_mode_ != SidebarMode::Search || !project_search_editing_) {
          return std::nullopt;
        }
        const float text_x = layout.sidebar.x + kSidebarInset;
        const float text_y =
            layout.sidebar.y + (surface == TextInputSurface::SidebarSearchQuery ? 38.0f : 54.0f);
        const std::string prefix =
            surface == TextInputSurface::SidebarSearchQuery ? "rg> " + project_search_edit_buffer_
                                                            : "replace> " + project_search_edit_buffer_;
        const float cursor_x = text_x + text_renderer_.MeasureWidth(prefix);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, layout.sidebar.w - kSidebarInset * 2.0f),
                             line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.surface_background,
        };
      }
      case TextInputSurface::Terminal:
      case TextInputSurface::None:
      default:
        return std::nullopt;
    }
  }();
  const auto update_text_input_area = [&](const std::optional<TextInputVisual>& visual) {
    if (render_window == nullptr) {
      return;
    }

    if (!visual.has_value()) {
      SDL_SetTextInputArea(render_window, nullptr, 0);
      return;
    }

    const SDL_Rect area = SDL_Rect{
        static_cast<int>(std::floor(visual->area.x)),
        static_cast<int>(std::floor(visual->area.y)),
        std::max(1, static_cast<int>(std::ceil(visual->area.w))),
        std::max(1, static_cast<int>(std::ceil(visual->area.h))),
    };
    const int cursor =
        std::max(0, static_cast<int>(std::round(visual->cursor_x - visual->area.x)));
    SDL_SetTextInputArea(render_window, &area, cursor);
  };
  const auto render_text_composition = [&](const std::optional<TextInputVisual>& visual) {
    if (!visual.has_value() || text_composition_.text.empty() ||
        text_composition_.surface != visual->surface) {
      return;
    }

    const std::string_view composition = text_composition_.text;
    const std::size_t total_codepoints = Utf8CodepointCount(composition);
    const std::size_t selection_start_codepoints =
        text_composition_.start < 0
            ? total_codepoints
            : std::min<std::size_t>(static_cast<std::size_t>(text_composition_.start),
                                    total_codepoints);
    const std::size_t selection_end_codepoints =
        text_composition_.length <= 0
            ? selection_start_codepoints
            : std::min(total_codepoints,
                       selection_start_codepoints +
                           static_cast<std::size_t>(text_composition_.length));
    const std::size_t selection_start =
        Utf8ByteOffsetForCodepointCount(composition, selection_start_codepoints);
    const std::size_t selection_end =
        Utf8ByteOffsetForCodepointCount(composition, selection_end_codepoints);
    const std::string_view prefix = composition.substr(0, selection_start);
    const std::string_view selected =
        composition.substr(selection_start, selection_end - selection_start);
    const std::string_view suffix = composition.substr(selection_end);
    const float prefix_width = text_renderer_.MeasureWidth(prefix);
    const float selected_width = text_renderer_.MeasureWidth(selected);
    const float total_width = text_renderer_.MeasureWidth(composition);

    if (!selected.empty()) {
      DrawFilledRect(renderer,
                     MakeRect(visual->cursor_x + prefix_width, visual->text_y - 1.0f, selected_width,
                              text_renderer_.LineHeight()),
                     theme_.selection_fill);
    }

    float segment_x = visual->cursor_x;
    if (!prefix.empty()) {
      text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.accent,
                                  visual->background, prefix);
      segment_x += prefix_width;
    }
    if (!selected.empty()) {
      text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.text_primary,
                                  theme_.selection_fill, selected);
      segment_x += selected_width;
    }
    if (!suffix.empty()) {
      text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.accent,
                                  visual->background, suffix);
    }

    DrawFilledRect(renderer,
                   MakeRect(visual->cursor_x, visual->text_y + text_renderer_.LineHeight() - 1.0f,
                            total_width, 1.0f),
                   theme_.accent);
    DrawFilledRect(renderer,
                   MakeRect(visual->cursor_x + prefix_width + selected_width, visual->text_y - 1.0f,
                            1.5f, text_renderer_.LineHeight()),
                   theme_.accent);
  };
  const auto draw_vertical_scrollbar =
      [&](const SDL_FRect& area, float total_units, float visible_units, float scroll_units,
          bool active = false, bool reserve_horizontal = false) {
        if (const auto geometry = MakeVerticalScrollbarGeometry(area, total_units, visible_units,
                                                                scroll_units, reserve_horizontal);
            geometry.has_value()) {
          DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, active);
        }
      };
  const auto draw_horizontal_scrollbar =
      [&](const SDL_FRect& area, float total_units, float visible_units, float scroll_units,
          bool active = false, bool reserve_vertical = false) {
        if (const auto geometry = MakeHorizontalScrollbarGeometry(area, total_units, visible_units,
                                                                  scroll_units, reserve_vertical);
            geometry.has_value()) {
          DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, active);
        }
      };
  const auto terminal_styles_equal = [](const terminal::TerminalStyle& lhs,
                                        const terminal::TerminalStyle& rhs) {
    const auto colors_equal = [](const std::optional<SDL_Color>& left,
                                 const std::optional<SDL_Color>& right) {
      return left.has_value() == right.has_value() &&
             (!left.has_value() ||
              (left->r == right->r && left->g == right->g && left->b == right->b &&
               left->a == right->a));
    };
    return colors_equal(lhs.foreground, rhs.foreground) &&
           colors_equal(lhs.background, rhs.background) && lhs.bold == rhs.bold;
  };
  const auto draw_terminal_line =
      [&](float x, float y, float width, const terminal::TerminalLine& line, std::size_t row_index) {
    if (width <= 0.0f || line.cells.empty()) {
      return;
    }

    const float char_width = std::max(1.0f, text_renderer_.CharWidth());
    const std::size_t max_chars =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(width / char_width)));
    std::size_t drawn_chars = 0;
    std::size_t segment_start = 0;
    std::string segment;
    terminal::TerminalStyle segment_style;
    bool has_segment = false;
    bool segment_selected = false;

    const auto flush_segment = [&]() {
      if (!has_segment || segment.empty()) {
        return;
      }
      const SDL_Color foreground =
          segment_selected ? theme_.text_primary
                           : segment_style.foreground.value_or(theme_.text_muted);
      const SDL_Color background =
          segment_selected ? theme_.row_highlight
                           : segment_style.background.value_or(theme_.surface_background);
      const float segment_x = x + static_cast<float>(segment_start) * char_width;
      text_renderer_.DrawStringOn(renderer, segment_x, y, foreground, background, segment);
      segment.clear();
      has_segment = false;
    };

    for (std::size_t column = 0; column < line.cells.size(); ++column) {
      if (drawn_chars >= max_chars) {
        break;
      }
      const auto& cell = line.cells[column];
      const bool selected = TerminalCellSelected(row_index, column);
      if (!has_segment) {
        segment_start = column;
        segment_style = cell.style;
        segment_selected = selected;
        has_segment = true;
      } else if (!terminal_styles_equal(segment_style, cell.style) ||
                 segment_selected != selected) {
        flush_segment();
        segment_start = column;
        segment_style = cell.style;
        segment_selected = selected;
        has_segment = true;
      }
      segment.push_back(cell.character);
      ++drawn_chars;
    }
    flush_segment();
  };

  const auto visible_menu_items = ComputeVisibleMenuBarItems(layout.menu_bar);
  const auto window_buttons = ComputeVisibleWindowControlButtons(layout.menu_bar);
  for (const VisibleMenuBarItem& item : visible_menu_items) {
    const MenuSpec* menu = FindMenuSpec(item.id);
    if (menu == nullptr) {
      continue;
    }
    const SDL_Color background = item.active ? theme_.chrome_active : theme_.chrome_background;
    DrawFilledRect(renderer, item.rect, background);
    if (item.active) {
      DrawFilledRect(renderer,
                     MakeRect(item.rect.x, item.rect.y + item.rect.h - 2.0f, item.rect.w, 2.0f),
                     theme_.accent);
    }
    draw_text_on(item.rect.x + 10.0f, item.rect.y + 4.0f,
                 item.active ? theme_.text_primary : theme_.text_secondary, background,
                 menu->label);
  }

  if (custom_window_chrome_enabled_) {
    const std::string title = "microide";
    const float title_width = text_renderer_.MeasureWidth(title);
    const float left_limit =
        visible_menu_items.empty() ? layout.menu_bar.x + 12.0f
                                   : visible_menu_items.back().rect.x +
                                         visible_menu_items.back().rect.w + 16.0f;
    const float right_limit =
        window_buttons.empty() ? layout.menu_bar.x + layout.menu_bar.w - 12.0f
                               : window_buttons.front().rect.x - 16.0f;
    const float title_x =
        std::floor(layout.menu_bar.x + (layout.menu_bar.w - title_width) * 0.5f);
    if (title_x >= left_limit && title_x + title_width <= right_limit) {
      draw_text_on(title_x, layout.menu_bar.y + 4.0f, theme_.text_muted, theme_.chrome_background,
                   title);
    }
  }

  for (const VisibleWindowControlButton& button : window_buttons) {
    SDL_Color background = button.hovered ? theme_.row_highlight : theme_.chrome_background;
    SDL_Color glyph = button.hovered ? theme_.text_primary : theme_.text_secondary;
    if (button.id == WindowControlButtonId::Close && button.hovered) {
      background = theme_.diff_deleted;
      glyph = theme_.text_primary;
    } else if (button.id == WindowControlButtonId::Maximize && window_maximized_ &&
               !button.hovered) {
      background = theme_.chrome_active;
      glyph = theme_.text_primary;
    }

    DrawFilledRect(renderer, button.rect, background);
    DrawWindowControlGlyph(renderer, button.rect, button.id, glyph, window_maximized_);
  }

  if (ActiveTabIsCompare()) {
    CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab != nullptr) {
      const int visible_rows = CompareVisibleRows(layout.editor_surface);
      ClampCompareScrollRow(*compare_tab, visible_rows);
      draw_vertical_scrollbar(layout.editor_surface, static_cast<float>(compare_tab->model.rows.size()),
                              static_cast<float>(visible_rows),
                              static_cast<float>(compare_tab->scroll_row),
                              drag_target_ == DragTarget::CompareScrollbar);
    }
  } else {
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    auto* editor_tab = ActiveEditorTab();
    for (const EditorPaneLayout& pane : panes) {
      editor::TextViewport* viewport =
          pane.active ? &text_viewport_
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id)
                                               : nullptr);
      if (viewport == nullptr || viewport->is_placeholder()) {
        continue;
      }

      const editor::EditorViewMetrics metrics =
          editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane.rect);
      viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      const std::size_t total_columns =
          std::max<std::size_t>(viewport->visible_columns(), MaxVisualColumns(*viewport));
      const bool show_vertical = viewport->line_count() > viewport->visible_lines();
      const bool show_horizontal = total_columns > viewport->visible_columns();
      if (show_vertical) {
        draw_vertical_scrollbar(pane.rect, static_cast<float>(viewport->line_count()),
                                static_cast<float>(viewport->visible_lines()),
                                static_cast<float>(viewport->scroll_line()),
                                pane.active && drag_target_ == DragTarget::EditorVerticalScrollbar,
                                show_horizontal);
      }
      if (show_horizontal) {
        draw_horizontal_scrollbar(
            pane.rect, static_cast<float>(total_columns),
            static_cast<float>(viewport->visible_columns()),
            static_cast<float>(viewport->horizontal_scroll()),
            pane.active && drag_target_ == DragTarget::EditorHorizontalScrollbar, show_vertical);
      }
    }
    for (const EditorSplitDividerLayout& divider :
         ComputeEditorSplitDividerLayouts(layout.editor_surface)) {
      const bool divider_active =
          drag_target_ == DragTarget::EditorSplitDivider &&
          divider.divider_index == drag_editor_split_divider_index_ &&
          divider.node_path == drag_editor_split_path_;
      DrawFilledRect(renderer, divider.rect, divider_active ? theme_.accent : theme_.border);
    }
  }

  for (const VisibleProjectTab& tab : ComputeVisibleProjectTabs(layout.project_tab_strip)) {
    DrawFilledRect(renderer, tab.rect, tab.active ? theme_.chrome_active : theme_.surface_raised);
    if (tab.active) {
      DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f), theme_.accent);
    }
    draw_text_on(tab.rect.x + 10.0f, tab.rect.y + 5.0f,
                 tab.active ? theme_.text_primary : theme_.text_secondary,
                 tab.active ? theme_.chrome_active : theme_.surface_raised,
                 TruncateLabel(ProjectTabDisplayTitle(tab.index), tab.rect.w - 46.0f));
    draw_text_on(tab.close_rect.x + 3.0f, tab.close_rect.y + 1.0f,
                 tab.active ? theme_.text_secondary : theme_.text_disabled,
                 tab.active ? theme_.chrome_active : theme_.surface_raised, "x");
  }

  if (!project_root_.empty() && open_tabs_.empty()) {
    const SDL_FRect placeholder_tab =
        MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 5.0f, 220.0f, 24.0f);
    DrawFilledRect(renderer, placeholder_tab, theme_.chrome_active);
    DrawFilledRect(renderer, MakeRect(placeholder_tab.x, placeholder_tab.y, placeholder_tab.w, 2.0f),
                   theme_.accent);
    draw_text_on(placeholder_tab.x + 10.0f, placeholder_tab.y + 6.0f, theme_.text_primary,
                 theme_.chrome_active, "welcome");
  } else if (!project_root_.empty()) {
    for (const VisibleTab& tab : ComputeVisibleTabs(layout.tab_strip)) {
      DrawFilledRect(renderer, tab.rect, tab.active ? theme_.chrome_active : theme_.surface_raised);
      if (tab.active) {
        DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f),
                       theme_.accent);
      }
      const std::string display_title = TabDisplayTitle(tab.index);
      draw_text_on(tab.rect.x + 10.0f, tab.rect.y + 6.0f,
                   tab.active ? theme_.text_primary : theme_.text_secondary,
                   tab.active ? theme_.chrome_active : theme_.surface_raised,
                   TruncateLabel(display_title, tab.rect.w - 46.0f));
      draw_text_on(tab.close_rect.x + 3.0f, tab.close_rect.y + 1.0f,
                   tab.active ? theme_.text_secondary : theme_.text_disabled,
                   tab.active ? theme_.chrome_active : theme_.surface_raised, "x");
    }
  }

  const std::string project_label = ProjectLabel();
  const float project_label_width = text_renderer_.MeasureWidth(project_label);
  if (!project_root_.empty()) {
    draw_text_on(layout.tab_strip.x + layout.tab_strip.w - project_label_width - 16.0f,
                 layout.tab_strip.y + 9.0f, theme_.text_muted, theme_.chrome_background,
                 project_label);
  }

  const float breadcrumb_label_x = layout.breadcrumb.x + 12.0f;
  draw_text_on(breadcrumb_label_x, layout.breadcrumb.y + 7.0f, theme_.text_muted,
               theme_.chrome_background, project_label);
  const float breadcrumb_text_x =
      breadcrumb_label_x + project_label_width + (project_label.empty() ? 0.0f : 14.0f);
  draw_text_on(breadcrumb_text_x, layout.breadcrumb.y + 7.0f, theme_.text_primary,
               theme_.chrome_background,
               TruncateLabel(BreadcrumbLabel(),
                             layout.breadcrumb.w - (breadcrumb_text_x - layout.breadcrumb.x) - 14.0f));

  if (sidebar_visible_) {
    const float text_y_offset = 4.0f;
    if (sidebar_mode_ == SidebarMode::Search) {
      const std::string active_query =
          project_search_editing_ && project_search_edit_field_ == ProjectSearchEditField::Query
              ? project_search_edit_buffer_
              : project_search_query_;
      const std::string active_replace =
          project_search_editing_ && project_search_edit_field_ == ProjectSearchEditField::Replace
              ? project_search_edit_buffer_
              : project_replace_text_;
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + 8.0f,
                   theme_.text_secondary, theme_.chrome_background,
                   sidebar_temporary_ ? "Search*" : "Search");
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + 38.0f,
                   project_search_editing_ &&
                           project_search_edit_field_ == ProjectSearchEditField::Query
                       ? theme_.text_primary
                       : theme_.text_secondary,
                   theme_.surface_background,
                   TruncateLabel("rg> " + active_query, layout.sidebar.w - kSidebarInset * 2.0f));
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + 54.0f,
                   project_search_editing_ &&
                           project_search_edit_field_ == ProjectSearchEditField::Replace
                       ? theme_.text_primary
                       : theme_.text_secondary,
                   theme_.surface_background,
                   TruncateLabel("replace> " + active_replace,
                                 layout.sidebar.w - kSidebarInset * 2.0f));
      const std::string status_text =
          project_search_editing_
              ? (project_search_edit_field_ == ProjectSearchEditField::Query
                     ? "Editing query  |  Enter apply  Esc cancel"
                     : "Editing replace  |  Enter apply  Esc cancel")
          : !project_search_error_.empty()
              ? "Error  |  / query  = replace  r rerun"
          : project_search_running_
              ? "Searching " + std::to_string(project_search_results_.size()) + " matches"
          : project_search_results_.empty()
              ? (project_search_query_.empty() ? "/ query  = replace"
                                               : "No matches  |  / query  = replace  r rerun  R replace all")
              : std::to_string(project_search_results_.size()) +
                    " matches  |  / query  = replace  r rerun  R replace all";
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + 70.0f, theme_.text_muted,
                   theme_.surface_background,
                   TruncateLabel(status_text, layout.sidebar.w - kSidebarInset * 2.0f));

      const float list_y = layout.sidebar.y + kSearchSidebarResultsTop;
      const auto line_map = BuildProjectSearchLineMap();
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - kSearchSidebarResultsTop) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const int selected_line = ProjectSearchLineForResult(project_search_selected_index_);
      if (selected_line < scroll_row) {
        scroll_row = selected_line;
      } else if (selected_line >= scroll_row + visible_rows) {
        scroll_row = selected_line - visible_rows + 1;
      }
      sidebar_scroll_row_ = scroll_row;

      for (int row = 0; row < visible_rows; ++row) {
        const int line_index = scroll_row + row;
        if (line_index >= static_cast<int>(line_map.size())) {
          break;
        }

        SDL_FRect row_rect = MakeRect(
            layout.sidebar.x + kSidebarInset,
            list_y + static_cast<float>(row) * kSidebarRowHeight,
            row_width,
            kSidebarRowHeight - 2.0f);

        const int result_index = line_map[static_cast<std::size_t>(line_index)];
        if (result_index < 0) {
          const std::size_t next_result_index =
              static_cast<std::size_t>(std::min(line_index + 1, static_cast<int>(line_map.size()) - 1));
          const auto& file_result =
              project_search_results_[static_cast<std::size_t>(line_map[next_result_index])];
          draw_text_on(row_rect.x + 4.0f, row_rect.y + text_y_offset, theme_.text_primary,
                       theme_.surface_background,
                       TruncateLabel(file_result.relative_path.string(), row_rect.w - 8.0f));
          continue;
        }

        const auto& result = project_search_results_[static_cast<std::size_t>(result_index)];
        const bool selected = static_cast<std::size_t>(result_index) == project_search_selected_index_;
        if (selected) {
          DrawFilledRect(renderer, row_rect, theme_.row_highlight);
        }

        const std::string snippet = CollapseWhitespace(result.preview);
        const std::string label =
            std::to_string(result.line + 1) + ":" + std::to_string(result.column + 1) + "  " + snippet;
        draw_text_on(row_rect.x + 6.0f, row_rect.y + text_y_offset,
                     selected ? theme_.text_primary : theme_.text_secondary,
                     selected ? theme_.row_highlight : theme_.surface_background,
                     TruncateLabel(label, row_rect.w - 12.0f));
      }

      if (line_map.empty()) {
        const std::string placeholder = !project_search_error_.empty()
                                            ? "Error: " + project_search_error_
                                        : project_search_running_
                                            ? "Searching..."
                                        : project_search_query_.empty() ? "Project search is idle"
                                                                        : "No matches";
        draw_text_on(layout.sidebar.x + kSidebarInset, list_y + 4.0f, theme_.text_muted,
                     theme_.surface_background,
                     TruncateLabel(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
      }

      draw_vertical_scrollbar(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(line_map.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row), drag_target_ == DragTarget::SidebarScrollbar);
    } else {
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + 8.0f,
                   theme_.text_secondary, theme_.chrome_background, "Project");
      const std::string tree_root_label = ProjectLabel();
      const float root_label_width = text_renderer_.MeasureWidth(tree_root_label);
      draw_text_on(layout.sidebar.x + layout.sidebar.w - root_label_width - kSidebarInset,
                   layout.sidebar.y + 8.0f, theme_.text_muted, theme_.chrome_background,
                   tree_root_label);

      const auto& entries = directory_tree_.entries();
      const float list_y = layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      if (directory_tree_.selected_index() < static_cast<std::size_t>(scroll_row)) {
        scroll_row = static_cast<int>(directory_tree_.selected_index());
      } else if (directory_tree_.selected_index() >=
                 static_cast<std::size_t>(scroll_row + visible_rows)) {
        scroll_row = static_cast<int>(directory_tree_.selected_index()) - visible_rows + 1;
      }
      sidebar_scroll_row_ = scroll_row;

      for (int row = 0; row < visible_rows; ++row) {
        const int entry_index = scroll_row + row;
        if (entry_index >= static_cast<int>(entries.size())) {
          break;
        }

        const auto& entry = entries[entry_index];
        SDL_FRect row_rect = MakeRect(
            layout.sidebar.x + kSidebarInset,
            list_y + static_cast<float>(row) * kSidebarRowHeight,
            row_width,
            kSidebarRowHeight - 2.0f);

        const bool selected = static_cast<std::size_t>(entry_index) == directory_tree_.selected_index();
        if (selected) {
          DrawFilledRect(renderer, row_rect, theme_.row_highlight);
          DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h),
                         theme_.accent);
        }

        const float depth_offset = static_cast<float>(entry.depth) * kTreeIndentWidth;
        const float tree_x = row_rect.x + 6.0f + depth_offset;
        const float chevron_x = tree_x;
        const float label_x = tree_x + kTreeChevronSlotWidth + 4.0f;
        const float chevron_center_y = row_rect.y + row_rect.h * 0.5f;
        const char git_marker = GitMarker(entry.git_status);
        const bool has_git_marker = git_marker != ' ';
        const std::string git_marker_text = has_git_marker ? std::string(1, git_marker) : "";
        const float marker_width =
            has_git_marker ? text_renderer_.MeasureWidth(git_marker_text) : 0.0f;
        const float marker_x = row_rect.x + row_rect.w - marker_width - 8.0f;
        const float label_width =
            has_git_marker ? std::max(20.0f, marker_x - label_x - 8.0f)
                           : std::max(20.0f, row_rect.x + row_rect.w - label_x - 8.0f);

        if (entry.is_directory) {
          DrawChevron(renderer, chevron_x, chevron_center_y, entry.expanded,
                      selected ? theme_.text_primary : theme_.text_muted);
        }

        draw_text_on(label_x, row_rect.y + text_y_offset,
                     selected ? theme_.text_primary
                              : (entry.is_directory ? theme_.text_primary : theme_.text_secondary),
                     selected ? theme_.row_highlight : theme_.surface_background,
                     TruncateLabel(entry.label, label_width));
        if (has_git_marker) {
          draw_text_on(marker_x, row_rect.y + text_y_offset,
                       selected ? theme_.text_primary : GitMarkerColor(theme_, entry.git_status),
                       selected ? theme_.row_highlight : theme_.surface_background,
                       git_marker_text);
        }
      }

      draw_vertical_scrollbar(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(entries.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row), drag_target_ == DragTarget::SidebarScrollbar);
    }
  }

  if (overlay_visible_) {
    DrawFilledRect(renderer, layout.editor_area, theme_.overlay_backdrop);
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
    const SDL_FRect overlay_header = MakeRect(overlay.x, overlay.y, overlay.w, 30.0f);
    DrawFilledRect(renderer, overlay, theme_.overlay_background);
    DrawRect(renderer, overlay, theme_.border);
    DrawFilledRect(renderer, overlay_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(overlay_header.x, overlay_header.y + overlay_header.h - kDivider,
                            overlay_header.w, kDivider),
                   theme_.border);

    const float overlay_inset = 18.0f;
    const float overlay_row_height = 22.0f;
    ClampOverlayScrollRow(overlay);
    const float overlay_list_y = overlay.y + OverlayListStartOffset();
    const int overlay_visible_rows = OverlayVisibleRows(overlay);
    const int overlay_max_scroll =
        std::max(0, static_cast<int>(OverlayItemCount()) - overlay_visible_rows);
    const float overlay_row_width =
        overlay.w - overlay_inset * 2.0f -
        (overlay_max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f);
    const auto draw_overlay_row =
        [&](int row_index, int selected_index, std::string_view label) {
          const bool selected = row_index == selected_index;
          SDL_FRect row = MakeRect(overlay.x + overlay_inset,
                                   overlay_list_y +
                                       static_cast<float>(row_index) * overlay_row_height,
                                   overlay_row_width, 18.0f);
          DrawFilledRect(renderer, row, selected ? theme_.row_highlight : theme_.surface_raised);
          draw_text_on(row.x + 6.0f, row.y + 2.0f,
                       selected ? theme_.text_primary : theme_.text_secondary,
                       selected ? theme_.row_highlight : theme_.surface_raised,
                       TruncateLabel(label, row.w - 12.0f));
        };

    if (overlay_mode_ == OverlayMode::BufferSearch) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Search Buffer");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + buffer_search_query_);
      const std::string summary =
          buffer_search_matches_.empty()
              ? "No matches"
              : std::to_string(buffer_search_selected_index_ + 1) + " / " +
                    std::to_string(buffer_search_matches_.size()) + " matches";
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f, theme_.text_muted,
                   theme_.overlay_background, summary);
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(buffer_search_matches_.size())) {
          break;
        }
        const auto& match = buffer_search_matches_[static_cast<std::size_t>(item_index)];
        const std::string label =
            "Ln " + std::to_string(match.start.line + 1) + ", Col " +
            std::to_string(match.start.column + 1) + "  " +
            TruncateLabel(text_viewport_.lines()[match.start.line], overlay.w - 150.0f);
        draw_overlay_row(row, static_cast<int>(buffer_search_selected_index_) - overlay_scroll_row_,
                         label);
      }
    } else if (overlay_mode_ == OverlayMode::BufferReplace) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Replace Buffer");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f,
                   buffer_search_field_ == BufferSearchField::Search ? theme_.text_primary
                                                                     : theme_.text_secondary,
                   theme_.overlay_background, "find: " + buffer_search_query_);
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f,
                   buffer_search_field_ == BufferSearchField::Replace ? theme_.text_primary
                                                                      : theme_.text_secondary,
                   theme_.overlay_background, "replace: " + buffer_replace_text_);
      const std::string summary =
          buffer_search_matches_.empty()
              ? "No matches"
              : std::to_string(buffer_search_selected_index_ + 1) + " / " +
                    std::to_string(buffer_search_matches_.size()) +
                    " matches  |  Enter replace  Ctrl+Enter replace all";
      draw_text_on(overlay.x + overlay_inset, overlay.y + 82.0f, theme_.text_muted,
                   theme_.overlay_background, TruncateLabel(summary, overlay.w - 36.0f));
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(buffer_search_matches_.size())) {
          break;
        }
        const auto& match = buffer_search_matches_[static_cast<std::size_t>(item_index)];
        const std::string label =
            "Ln " + std::to_string(match.start.line + 1) + ", Col " +
            std::to_string(match.start.column + 1) + "  " +
            TruncateLabel(text_viewport_.lines()[match.start.line], overlay.w - 150.0f);
        draw_overlay_row(row, static_cast<int>(buffer_search_selected_index_) - overlay_scroll_row_,
                         label);
      }
    } else if (overlay_mode_ == OverlayMode::ProjectSearch) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Project Search");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + project_search_query_);
      const std::string summary =
          project_search_results_.empty()
              ? "No results"
              : std::to_string(project_search_selected_index_ + 1) + " / " +
                    std::to_string(project_search_results_.size()) + " results";
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f, theme_.text_muted,
                   theme_.overlay_background, summary);
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(project_search_results_.size())) {
          break;
        }
        const auto& result = project_search_results_[static_cast<std::size_t>(item_index)];
        const std::string label =
            result.relative_path.string() + ":" + std::to_string(result.line + 1) + ":" +
            std::to_string(result.column + 1) + "  " +
            TruncateLabel(result.preview, overlay.w - 220.0f);
        draw_overlay_row(row, static_cast<int>(project_search_selected_index_) - overlay_scroll_row_,
                         label);
      }
    } else if (overlay_mode_ == OverlayMode::CommitPicker) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Compare against commit");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_muted,
                   theme_.overlay_background, compare_picker_path_.filename().string());
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + compare_picker_query_);
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(compare_picker_matches_.size())) {
          break;
        }
        const auto& commit = compare_picker_matches_[static_cast<std::size_t>(item_index)];
        draw_overlay_row(row, static_cast<int>(compare_picker_selected_index_) - overlay_scroll_row_,
                         commit.short_hash + "  " + commit.subject);
      }
      if (compare_picker_matches_.empty()) {
        draw_text_on(overlay.x + overlay_inset, overlay.y + 92.0f, theme_.text_muted,
                     theme_.overlay_background, "No matching commits");
      }
    } else {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Find File");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + file_finder_.query());

      const auto& results = file_finder_.results();
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(results.size())) {
          break;
        }
        draw_overlay_row(row, static_cast<int>(file_finder_.selected_index()) - overlay_scroll_row_,
                         results[static_cast<std::size_t>(item_index)].relative_path.string());
      }
      if (results.empty()) {
        draw_text_on(overlay.x + overlay_inset, overlay.y + 80.0f, theme_.text_muted,
                     theme_.overlay_background, "No matching files");
      }
    }

    draw_vertical_scrollbar(
        MakeRect(overlay.x, overlay_list_y, overlay.w,
                 std::max(0.0f, overlay.y + overlay.h - overlay_list_y - 8.0f)),
        static_cast<float>(OverlayItemCount()), static_cast<float>(overlay_visible_rows),
        static_cast<float>(overlay_scroll_row_), drag_target_ == DragTarget::OverlayScrollbar);
  }

  if (bottom_panel_visible_) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kBottomPanelHeaderHeight);
    const bool terminal_panel = BottomPanelShowsTerminal();
    if (terminal_panel) {
      for (const VisibleTerminalTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
        const auto* terminal_tab =
            tab.index < terminal_tabs_.size() ? terminal_tabs_[tab.index].get() : nullptr;
        if (terminal_tab == nullptr) {
          continue;
        }

        const SDL_Color background = tab.active ? theme_.chrome_active : theme_.surface_raised;
        const SDL_Color foreground = tab.active ? theme_.text_primary : theme_.text_secondary;
        DrawFilledRect(renderer, tab.rect, background);
        if (tab.active) {
          DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f),
                         theme_.accent);
        }
        std::string label = terminal_tab->session.LaunchLabel();
        if (label.empty()) {
          label = "terminal";
        }
        draw_text_on(tab.rect.x + 8.0f, tab.rect.y + 4.0f, foreground, background,
                     TruncateLabel(label, tab.rect.w - 40.0f));
        draw_text_on(tab.close_rect.x + 3.0f, tab.close_rect.y + 1.0f, foreground, background,
                     "x");
      }
      const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(panel_header);
      DrawFilledRect(renderer, new_tab_rect, theme_.surface_raised);
      DrawRect(renderer, new_tab_rect, theme_.border);
      draw_text_on(new_tab_rect.x + 5.0f, new_tab_rect.y + 2.0f, theme_.text_secondary,
                   theme_.surface_raised, "+");
    } else {
      std::string panel_header_label = BottomPanelHeaderLabel();
      if (command_mode_) {
        panel_header_label += " | Command";
      }
      draw_text_on(layout.bottom_panel.x + 12.0f, layout.bottom_panel.y + 8.0f,
                   theme_.text_secondary, theme_.chrome_background,
                   TruncateLabel(panel_header_label, layout.bottom_panel.w - 24.0f));
    }

    const SDL_FRect panel_content = BottomPanelContentRect(layout, command_mode_);
    const float logs_y = panel_content.y + 8.0f;
    const std::size_t panel_line_count = terminal_panel ? terminal_lines.size() : log_messages_.size();
    const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
    const int max_scroll = std::max(0, static_cast<int>(panel_line_count) - visible_rows);
    const int scroll_row = BottomPanelScrollRow(panel_line_count, visible_rows);
    SetBottomPanelScrollRow(scroll_row, panel_line_count, visible_rows);
    const float log_width =
        panel_content.w - 24.0f - (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f);
    for (int row = 0; row < visible_rows; ++row) {
      const int index = scroll_row + row;
      if (index >= static_cast<int>(panel_line_count)) {
        break;
      }
      const float line_y = logs_y + static_cast<float>(row) * text_renderer_.LineHeight();
      if (terminal_panel) {
        draw_terminal_line(panel_content.x + 12.0f, line_y, log_width,
                           terminal_lines[static_cast<std::size_t>(index)],
                           static_cast<std::size_t>(index));
      } else {
        draw_text_on(panel_content.x + 12.0f, line_y, theme_.text_muted,
                     theme_.surface_background,
                     TruncateLabel(log_messages_[static_cast<std::size_t>(index)], log_width));
      }
    }

    if (terminal_panel) {
      if (auto* active_terminal = ActiveTerminalTab(); active_terminal != nullptr &&
                                                   active_terminal->session.cursor_visible()) {
        const std::size_t cursor_row = active_terminal->session.cursor_row();
        const std::size_t cursor_column = active_terminal->session.cursor_column();
        if (cursor_row >= static_cast<std::size_t>(scroll_row) &&
            cursor_row < static_cast<std::size_t>(scroll_row + visible_rows) &&
            (focus_ != FocusTarget::Panel || CaretVisibleNow())) {
          const float char_width = std::max(1.0f, text_renderer_.CharWidth());
          const float cursor_x =
              panel_content.x + 12.0f + static_cast<float>(cursor_column) * char_width;
          const float cursor_y =
              logs_y + static_cast<float>(cursor_row - static_cast<std::size_t>(scroll_row)) *
                           text_renderer_.LineHeight();
          if (cursor_x < panel_content.x + panel_content.w - 2.0f) {
            DrawFilledRect(renderer,
                           MakeRect(cursor_x, cursor_y - 1.0f, 1.5f, text_renderer_.LineHeight()),
                           theme_.cursor);
          }
        }
      }
    }

    if (command_mode_) {
      const SDL_FRect command_area = BottomPanelCommandAreaRect(layout);
      DrawFilledRect(renderer, command_area, theme_.surface_raised);
      DrawFilledRect(renderer, MakeRect(command_area.x, command_area.y, command_area.w, kDivider),
                     theme_.border);

      const float status_y = command_area.y + kBottomPanelCommandTopPadding;
      draw_text_on(command_area.x + 12.0f, status_y, theme_.text_muted, theme_.surface_raised,
                   TruncateLabel(CommandPromptStatusText(), command_area.w - 24.0f));

      SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
      const float prompt_y = prompt_rect.y + 4.0f;
      DrawFilledRect(renderer, prompt_rect, theme_.chrome_active);
      draw_text_on(prompt_rect.x + 6.0f, prompt_y, theme_.text_primary, theme_.chrome_active,
                   "> " + command_input_);
    }

    draw_vertical_scrollbar(
        panel_content,
        static_cast<float>(panel_line_count), static_cast<float>(visible_rows),
        static_cast<float>(scroll_row), drag_target_ == DragTarget::BottomPanelScrollbar);
  }

  if (menu_bar_open_) {
    const MenuSpec* menu = FindMenuSpec(active_menu_id_);
    const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, active_menu_id_);
    if (menu != nullptr && popup_rect.has_value()) {
      DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
      DrawRect(renderer, *popup_rect, theme_.border);
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(active_menu_id_, *popup_rect)) {
        if (item.separator) {
          DrawFilledRect(renderer,
                         MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                                  std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                         theme_.border);
          continue;
        }

        const MenuItemSpec& spec = menu->items[item.index];
        const SDL_Color background =
            item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
        const SDL_Color text_color = !item.enabled ? theme_.text_disabled
                                   : item.hovered ? theme_.text_primary
                                                  : theme_.text_secondary;
        const SDL_Color accel_color = !item.enabled ? theme_.text_disabled : theme_.text_muted;
        DrawFilledRect(renderer, item.rect, background);
        if (item.checked) {
          draw_text_on(item.rect.x + 8.0f, item.rect.y + 3.0f,
                       item.enabled ? theme_.accent : theme_.text_disabled, background, "x");
        }
        const std::string accelerator = MenuItemAccelerator(spec);
        const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
        const float label_width =
            std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
        draw_text_on(item.rect.x + 24.0f, item.rect.y + 3.0f, text_color, background,
                     TruncateLabel(MenuItemLabel(spec), label_width));
        if (!accelerator.empty()) {
          draw_text_on(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y + 3.0f,
                       accel_color, background, accelerator);
        }
      }
    }
  }

  if (tree_context_menu_.open) {
    const auto items = TreeContextMenuItems(tree_context_menu_.target);
    const auto popup_rect = ComputeTreeContextMenuRect();
    if (!items.empty() && popup_rect.has_value()) {
      DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
      DrawRect(renderer, *popup_rect, theme_.border);
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(items, tree_context_menu_.active_item_index, *popup_rect)) {
        if (item.separator) {
          DrawFilledRect(renderer,
                         MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                                  std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                         theme_.border);
          continue;
        }

        const MenuItemSpec& spec = items[item.index];
        const SDL_Color background =
            item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
        const SDL_Color text_color = !item.enabled ? theme_.text_disabled
                                   : item.hovered ? theme_.text_primary
                                                  : theme_.text_secondary;
        const SDL_Color accel_color = !item.enabled ? theme_.text_disabled : theme_.text_muted;
        DrawFilledRect(renderer, item.rect, background);
        if (item.checked) {
          draw_text_on(item.rect.x + 8.0f, item.rect.y + 3.0f,
                       item.enabled ? theme_.accent : theme_.text_disabled, background, "x");
        }
        const std::string accelerator = MenuItemAccelerator(spec);
        const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
        const float label_width =
            std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
        draw_text_on(item.rect.x + 24.0f, item.rect.y + 3.0f, text_color, background,
                     TruncateLabel(MenuItemLabel(spec), label_width));
        if (!accelerator.empty()) {
          draw_text_on(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y + 3.0f,
                       accel_color, background, accelerator);
        }
      }
    }
  }

  const std::string focus_text =
      focus_ == FocusTarget::Sidebar ? "sidebar"
      : focus_ == FocusTarget::Panel ? "panel"
      : focus_ == FocusTarget::Overlay ? "overlay"
                                       : "editor";
  const std::string sidebar_text =
      sidebar_mode_ == SidebarMode::Search ? (sidebar_temporary_ ? "search*" : "search")
      : sidebar_mode_ == SidebarMode::Tree ? "tree"
                                           : "none";
  const std::string left_status =
      ProjectLabel() + "  |  " + std::string(text_renderer_.BackendName()) + "  |  focus " +
      focus_text + "  |  sidebar " + sidebar_text + "  |  scale " + UiScaleLabel(ui_scale_);
  std::string right_status;
  if (ActiveTabIsCompare()) {
    right_status =
        "Compare  |  Row " +
        std::to_string(ActiveCompareTab() == nullptr ? 1
                                                     : ActiveCompareTab()->selected_row + 1);
  } else {
    const std::string dirty_prefix = text_viewport_.dirty() ? "* " : "";
    right_status =
        dirty_prefix + text_viewport_.EncodingLabel() + "  |  " +
        text_viewport_.LineEndingLabel() + "  |  Ln " +
        std::to_string(text_viewport_.cursor_line() + 1) + ", Col " +
        std::to_string(text_viewport_.cursor_column() + 1);
  }
  const float right_status_width = text_renderer_.MeasureWidth(right_status);
  const float right_status_x = std::max(layout.status_bar.x + 10.0f,
                                        layout.status_bar.x + layout.status_bar.w -
                                            right_status_width - 12.0f);
  const float left_max_width =
      std::max(0.0f, right_status_x - (layout.status_bar.x + 10.0f) - 12.0f);
  draw_text_on(layout.status_bar.x + 10.0f, layout.status_bar.y + 5.0f, theme_.text_secondary,
               theme_.chrome_background, TruncateLabel(left_status, left_max_width));
  draw_text_on(right_status_x, layout.status_bar.y + 5.0f, theme_.text_muted,
               theme_.chrome_background, right_status);

  if (prompt_surface_visible_) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

    const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
    const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
    const SDL_FRect message_rect = MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 36.0f);
    DrawFilledRect(renderer, dialog, theme_.overlay_background);
    DrawRect(renderer, dialog, theme_.border);
    DrawFilledRect(renderer, header, theme_.chrome_background);
    DrawFilledRect(renderer, MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                   theme_.border);
    draw_text_on(header.x + 16.0f, header.y + 8.0f, theme_.text_primary, theme_.chrome_background,
                 PromptSurfaceTitle());
    draw_text_on(message_rect.x, message_rect.y, theme_.text_muted, theme_.overlay_background,
                 TruncateLabel(PromptSurfaceMessage(), message_rect.w));

    if (prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput) {
      const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
      DrawFilledRect(renderer, input_rect, theme_.surface_background);
      DrawRect(renderer, input_rect, theme_.border);
      draw_text_on(input_rect.x + 6.0f, input_rect.y + 4.0f, theme_.text_primary,
                   theme_.surface_background,
                   TruncateLabel(prompt_surface_state_.input, input_rect.w - 12.0f));
    }

    const auto buttons = ComputePromptSurfaceButtonRects(dialog);
    const auto labels = PromptSurfaceActionLabels();
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const bool selected = prompt_surface_state_.selected_button == static_cast<int>(i);
      const SDL_Color background = selected ? theme_.chrome_active : theme_.surface_raised;
      DrawFilledRect(renderer, buttons[i], background);
      DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
      const float text_width = text_renderer_.MeasureWidth(labels[i]);
      draw_text_on(buttons[i].x + std::floor((buttons[i].w - text_width) * 0.5f),
                   buttons[i].y + 6.0f, theme_.text_primary, background, labels[i]);
    }
  }

  render_text_composition(active_text_input_visual);
  update_text_input_area(active_text_input_visual);

  if (dirty_prompt_visible_) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

    const SDL_FRect dialog = ComputeDirtyPromptRect(layout.full);
    const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
    const SDL_FRect message_rect = MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 54.0f);
    DrawFilledRect(renderer, dialog, theme_.overlay_background);
    DrawRect(renderer, dialog, theme_.border);
    DrawFilledRect(renderer, header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                   theme_.border);

    draw_text_on(header.x + 12.0f, header.y + 9.0f, theme_.text_primary, theme_.chrome_background,
                 DirtyPromptTitle());
    draw_text_on(message_rect.x, message_rect.y, theme_.text_secondary, theme_.overlay_background,
                 TruncateLabel(DirtyPromptMessage(), message_rect.w));
    draw_text_on(message_rect.x, message_rect.y + 22.0f, theme_.text_muted, theme_.overlay_background,
                 "Enter confirm  Left/Right choose  Esc cancel");

    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    const auto labels = DirtyPromptActionLabels();
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const bool selected = dirty_prompt_state_.selected_action == static_cast<int>(i);
      DrawFilledRect(renderer, buttons[i],
                     selected ? theme_.chrome_active : theme_.surface_raised);
      DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
      draw_text_on(buttons[i].x + 12.0f, buttons[i].y + 7.0f,
                   selected ? theme_.text_primary : theme_.text_secondary,
                   selected ? theme_.chrome_active : theme_.surface_raised, labels[i]);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  }
}

void WorkspaceShell::RenderCompareSurface(SDL_Renderer* renderer, const SDL_FRect& rect) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (renderer == nullptr || compare_tab == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  static const std::vector<editor::SyntaxTokenKind> kEmptyTokens;

  DrawFilledRect(renderer, rect, theme_.editor_background);

  const float line_height = text_renderer_.LineHeight();
  const float gutter_width = std::max(
      28.0f,
      text_renderer_.MeasureWidth(std::to_string(compare_tab->model.rows.size() + 1)) + 12.0f);
  const float divider_width = 18.0f;
  const float content_width = std::max(40.0f, rect.w - gutter_width * 2.0f - divider_width - 16.0f);
  const float left_width = std::floor(content_width * 0.5f);
  const float right_width = content_width - left_width;
  const float left_x = rect.x + 8.0f;
  const float center_x = left_x + gutter_width + left_width;
  const float right_x = center_x + divider_width + gutter_width;
  const float header_y = rect.y + 6.0f;
  const float rows_y = rect.y + line_height + 12.0f;
  const int visible_rows = CompareVisibleRows(rect);
  ClampCompareScrollRow(*compare_tab, visible_rows);

  DrawFilledRect(renderer, MakeRect(rect.x, rows_y - 6.0f, rect.w, 1.0f), theme_.border);
  DrawFilledRect(renderer, MakeRect(center_x - 6.0f, rect.y, 1.0f, rect.h), theme_.border);
  DrawFilledRect(renderer, MakeRect(right_x - 6.0f, rect.y, 1.0f, rect.h), theme_.border);

  text_renderer_.DrawString(renderer, left_x + gutter_width, header_y,
                            theme_.text_secondary,
                            TruncateLabel(compare_tab->left_label, left_width - 8.0f));
  text_renderer_.DrawString(renderer, right_x + gutter_width, header_y,
                            theme_.text_secondary,
                            TruncateLabel(compare_tab->right_label, right_width - 8.0f));

  for (int row = 0; row < visible_rows; ++row) {
    const int model_index = compare_tab->scroll_row + row;
    if (model_index >= static_cast<int>(compare_tab->model.rows.size())) {
      break;
    }

    const auto& compare_row = compare_tab->model.rows[static_cast<std::size_t>(model_index)];
    const float y = rows_y + static_cast<float>(row) * line_height;
    const bool selected = static_cast<std::size_t>(model_index) == compare_tab->selected_row;
    if (selected) {
      DrawFilledRect(renderer, MakeRect(rect.x + 1.0f, y - 1.0f, rect.w - 2.0f, line_height),
                     theme_.row_highlight);
    }

    const SDL_Color row_background = selected ? theme_.row_highlight : theme_.editor_background;
    const auto draw_text = [&](float x, float width, SDL_Color color, const std::string& text) {
      const std::string display_text = TruncateLabel(text, width);
      if (display_text.empty()) {
        return;
      }
      text_renderer_.DrawStringOn(renderer, x, y, color, row_background, display_text);
    };
    const auto draw_syntax_text = [&](float x,
                                      float width,
                                      SDL_Color plain_color,
                                      const std::string& text,
                                      const std::vector<editor::SyntaxTokenKind>& full_tokens,
                                      const std::vector<compare::CompareTextSpan>& changed_spans,
                                      SDL_Color changed_background) {
      if (text.empty()) {
        return;
      }

      const std::string display_text = TruncateLabel(text, width);
      if (display_text.empty()) {
        return;
      }

      const std::size_t visible_text_bytes =
          display_text != text && EndsWith(display_text, "...") && display_text.size() >= 3
              ? display_text.size() - 3
              : display_text.size();
      const auto token_kind_at = [&](std::size_t byte_offset) {
        if (byte_offset < visible_text_bytes && byte_offset < full_tokens.size()) {
          return full_tokens[byte_offset];
        }
        return editor::SyntaxTokenKind::Plain;
      };
      std::size_t changed_span_index = 0;
      const auto byte_is_changed = [&](std::size_t byte_offset) {
        if (byte_offset >= visible_text_bytes) {
          return false;
        }
        while (changed_span_index < changed_spans.size() &&
               changed_spans[changed_span_index].end <= byte_offset) {
          ++changed_span_index;
        }
        return changed_span_index < changed_spans.size() &&
               byte_offset >= changed_spans[changed_span_index].start &&
               byte_offset < changed_spans[changed_span_index].end;
      };

      float segment_x = x;
      for (std::size_t segment_start = 0; segment_start < display_text.size();) {
        const editor::SyntaxTokenKind kind = token_kind_at(segment_start);
        const bool changed = byte_is_changed(segment_start);
        std::size_t segment_end = segment_start;
        while (segment_end < display_text.size()) {
          const std::size_t next = segment_end + Utf8SequenceLength(display_text, segment_end);
          if (next >= display_text.size()) {
            segment_end = display_text.size();
            break;
          }
          if (token_kind_at(next) != kind || byte_is_changed(next) != changed) {
            segment_end = next;
            break;
          }
          segment_end = next;
        }

        const std::string_view segment_text(display_text.data() + segment_start,
                                            segment_end - segment_start);
        text_renderer_.DrawStringOn(
            renderer, segment_x, y,
            CompareTokenColor(theme_, kind, plain_color, selected),
            changed ? changed_background : row_background, segment_text);
        segment_x += text_renderer_.MeasureWidth(segment_text);
        segment_start = segment_end;
      }
    };

    if (compare_row.left_line > 0) {
      draw_text(left_x, gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number,
                std::to_string(compare_row.left_line));
    }
    if (compare_row.right_line > 0) {
      draw_text(right_x, gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number,
                std::to_string(compare_row.right_line));
    }

    SDL_Color left_color = theme_.text_secondary;
    SDL_Color right_color = theme_.text_secondary;
    SDL_Color marker_color = selected ? theme_.text_secondary : theme_.text_muted;
    char marker = ' ';
    switch (compare_row.kind) {
      case compare::CompareRowKind::Added:
        right_color = theme_.diff_added;
        marker_color = theme_.diff_added;
        marker = '+';
        break;
      case compare::CompareRowKind::Deleted:
        left_color = theme_.diff_deleted;
        marker_color = theme_.diff_deleted;
        marker = '-';
        break;
      case compare::CompareRowKind::Modified:
        left_color = theme_.diff_modified;
        right_color = theme_.diff_modified;
        marker_color = theme_.diff_modified;
        marker = '~';
        break;
      case compare::CompareRowKind::Unchanged:
      default:
        break;
    }

    const SDL_Color left_changed_background = BlendColor(
        row_background,
        compare_row.kind == compare::CompareRowKind::Deleted ? theme_.diff_deleted
                                                             : theme_.diff_modified,
        selected ? 0.42f : 0.28f);
    const SDL_Color right_changed_background = BlendColor(
        row_background,
        compare_row.kind == compare::CompareRowKind::Added ? theme_.diff_added
                                                           : theme_.diff_modified,
        selected ? 0.42f : 0.28f);

    if (compare_row.left_line > 0) {
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->left_tokens_by_row.size()
              ? &compare_tab->left_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      draw_syntax_text(left_x + gutter_width, left_width - 8.0f, left_color, compare_row.left_text,
                       *cached_tokens, compare_row.left_changed_spans, left_changed_background);
    }
    if (compare_row.right_line > 0) {
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->right_tokens_by_row.size()
              ? &compare_tab->right_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      draw_syntax_text(right_x + gutter_width, right_width - 8.0f, right_color,
                       compare_row.right_text, *cached_tokens, compare_row.right_changed_spans,
                       right_changed_background);
    }
    draw_text(center_x + 4.0f, divider_width - 6.0f, marker_color, std::string(1, marker));
  }
}

std::vector<WorkspaceShell::VisibleTab> WorkspaceShell::ComputeVisibleTabs(
    const SDL_FRect& tab_strip) const {
  std::vector<VisibleTab> tabs;
  if (open_tabs_.empty()) {
    return tabs;
  }

  const float tab_y = tab_strip.y + 5.0f;
  const float tab_height = 24.0f;
  const float gap = 6.0f;
  const float start_x = tab_strip.x + 12.0f;
  const float right_reserve = std::clamp(tab_strip.w * 0.22f, 160.0f, 240.0f);
  const float max_tab_x = std::max(start_x + 120.0f, tab_strip.x + tab_strip.w - right_reserve);
  const std::size_t first_tab =
      static_cast<std::size_t>(std::clamp(tab_scroll_index_, 0,
                                          std::max(0, static_cast<int>(open_tabs_.size()) - 1)));

  float tab_x = start_x;
  for (std::size_t i = first_tab; i < open_tabs_.size(); ++i) {
    const float tab_width = TabWidthForIndex(i);
    if (tab_x + tab_width > max_tab_x) {
      break;
    }

    VisibleTab tab;
    tab.index = i;
    tab.active = i == active_tab_index_;
    tab.rect = MakeRect(tab_x, tab_y, tab_width, tab_height);
    tab.close_rect =
        MakeRect(tab.rect.x + tab.rect.w - kTabCloseButtonRightInset - kTabCloseButtonSize,
                 tab.rect.y + 4.0f, kTabCloseButtonSize, kTabCloseButtonSize);
    tabs.push_back(tab);
    tab_x += tab_width + gap;
  }

  return tabs;
}

float WorkspaceShell::ProjectTabWidthForIndex(std::size_t index) const {
  if (index >= projects_.size()) {
    return 156.0f;
  }
  return std::clamp(text_renderer_.MeasureWidth(ProjectTabDisplayTitle(index)) + 58.0f, 156.0f,
                    260.0f);
}

void WorkspaceShell::EnsureActiveProjectVisible() {
  if (projects_.empty()) {
    project_tab_scroll_index_ = 0;
    return;
  }

  const float strip_width =
      last_window_width_ > 0 ? static_cast<float>(last_window_width_) : 1440.0f;
  const float start_x = 12.0f;
  const float gap = 6.0f;
  const float max_tab_x = std::max(start_x + 120.0f, strip_width - 12.0f);

  const auto active_visible_from = [&](std::size_t first_tab) {
    float tab_x = start_x;
    for (std::size_t i = first_tab; i < projects_.size(); ++i) {
      const float tab_width = ProjectTabWidthForIndex(i);
      if (tab_x + tab_width > max_tab_x) {
        return false;
      }
      if (i == active_project_index_) {
        return true;
      }
      tab_x += tab_width + gap;
    }
    return false;
  };

  const std::size_t current_first =
      static_cast<std::size_t>(std::clamp(project_tab_scroll_index_, 0,
                                          std::max(0, static_cast<int>(projects_.size()) - 1)));
  if (active_visible_from(current_first)) {
    return;
  }

  float used_width = ProjectTabWidthForIndex(active_project_index_);
  std::size_t first_visible = active_project_index_;
  while (first_visible > 0) {
    const float candidate_width = used_width + gap + ProjectTabWidthForIndex(first_visible - 1);
    if (start_x + candidate_width > max_tab_x) {
      break;
    }
    used_width = candidate_width;
    --first_visible;
  }

  project_tab_scroll_index_ = static_cast<int>(first_visible);
}

std::vector<WorkspaceShell::VisibleProjectTab> WorkspaceShell::ComputeVisibleProjectTabs(
    const SDL_FRect& project_tab_strip) const {
  std::vector<VisibleProjectTab> tabs;
  if (projects_.empty()) {
    return tabs;
  }

  const float tab_y = project_tab_strip.y + 4.0f;
  const float tab_height = std::max(18.0f, project_tab_strip.h - 8.0f);
  const float gap = 6.0f;
  const float start_x = project_tab_strip.x + 12.0f;
  const float max_tab_x = std::max(start_x + 120.0f, project_tab_strip.x + project_tab_strip.w - 12.0f);
  const std::size_t first_tab =
      static_cast<std::size_t>(std::clamp(project_tab_scroll_index_, 0,
                                          std::max(0, static_cast<int>(projects_.size()) - 1)));

  float tab_x = start_x;
  for (std::size_t i = first_tab; i < projects_.size(); ++i) {
    const float tab_width = ProjectTabWidthForIndex(i);
    if (tab_x + tab_width > max_tab_x) {
      break;
    }

    VisibleProjectTab tab;
    tab.index = i;
    tab.active = i == active_project_index_;
    tab.rect = MakeRect(tab_x, tab_y, tab_width, tab_height);
    tab.close_rect =
        MakeRect(tab.rect.x + tab.rect.w - kTabCloseButtonRightInset - kTabCloseButtonSize,
                 tab.rect.y + 3.0f, kTabCloseButtonSize, kTabCloseButtonSize);
    tabs.push_back(tab);
    tab_x += tab_width + gap;
  }

  return tabs;
}

std::vector<WorkspaceShell::VisibleTerminalTab> WorkspaceShell::ComputeVisibleTerminalTabs(
    const SDL_FRect& panel_header) const {
  std::vector<VisibleTerminalTab> tabs;
  if (terminal_tabs_.empty()) {
    return tabs;
  }

  const float tab_y = panel_header.y + 3.0f;
  const float tab_height = std::max(18.0f, panel_header.h - 6.0f);
  const float gap = 4.0f;
  const float start_x = panel_header.x + 12.0f;
  const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(panel_header);
  const float max_tab_x = std::max(start_x, new_tab_rect.x - 8.0f);

  float tab_x = start_x;
  for (std::size_t i = 0; i < terminal_tabs_.size(); ++i) {
    const TerminalTabState* terminal_tab = terminal_tabs_[i].get();
    if (terminal_tab == nullptr) {
      continue;
    }

    std::string label = terminal_tab->session.LaunchLabel();
    if (label.empty()) {
      label = "terminal";
    }
    const float tab_width = std::clamp(text_renderer_.MeasureWidth(label) + 38.0f, 84.0f, 220.0f);
    if (tab_x + tab_width > max_tab_x) {
      break;
    }

    VisibleTerminalTab tab;
    tab.index = i;
    tab.active = i == active_terminal_tab_index_;
    tab.rect = MakeRect(tab_x, tab_y, tab_width, tab_height);
    tab.close_rect =
        MakeRect(tab.rect.x + tab.rect.w - kTabCloseButtonRightInset - kTabCloseButtonSize,
                 tab.rect.y + 2.0f, kTabCloseButtonSize, kTabCloseButtonSize);
    tabs.push_back(tab);
    tab_x += tab_width + gap;
  }

  return tabs;
}

SDL_FRect WorkspaceShell::BottomPanelTerminalNewTabRect(const SDL_FRect& panel_header) const {
  const float button_size =
      std::min(kBottomPanelHeaderButtonSize, std::max(14.0f, panel_header.h - 8.0f));
  return MakeRect(panel_header.x + panel_header.w - button_size - 8.0f,
                  panel_header.y + (panel_header.h - button_size) * 0.5f, button_size,
                  button_size);
}

float WorkspaceShell::TabWidthForIndex(std::size_t index) const {
  if (index >= open_tabs_.size()) {
    return 132.0f;
  }
  return std::clamp(text_renderer_.MeasureWidth(TabDisplayTitle(index)) + 58.0f, 132.0f, 220.0f);
}

void WorkspaceShell::EnsureActiveTabVisible() {
  if (open_tabs_.empty()) {
    tab_scroll_index_ = 0;
    return;
  }

  const float tab_strip_width = last_window_width_ > 0 ? static_cast<float>(last_window_width_) : 1440.0f;
  const float start_x = 12.0f;
  const float gap = 6.0f;
  const float right_reserve = std::clamp(tab_strip_width * 0.22f, 160.0f, 240.0f);
  const float max_tab_x = std::max(start_x + 120.0f, tab_strip_width - right_reserve);

  const auto active_visible_from = [&](std::size_t first_tab) {
    float tab_x = start_x;
    for (std::size_t i = first_tab; i < open_tabs_.size(); ++i) {
      const float tab_width = TabWidthForIndex(i);
      if (tab_x + tab_width > max_tab_x) {
        return false;
      }
      if (i == active_tab_index_) {
        return true;
      }
      tab_x += tab_width + gap;
    }
    return false;
  };

  const std::size_t current_first =
      static_cast<std::size_t>(std::clamp(tab_scroll_index_, 0,
                                          std::max(0, static_cast<int>(open_tabs_.size()) - 1)));
  if (active_visible_from(current_first)) {
    return;
  }

  float used_width = TabWidthForIndex(active_tab_index_);
  std::size_t first_visible = active_tab_index_;
  while (first_visible > 0) {
    const float candidate_width = used_width + gap + TabWidthForIndex(first_visible - 1);
    if (start_x + candidate_width > max_tab_x) {
      break;
    }
    used_width = candidate_width;
    --first_visible;
  }

  tab_scroll_index_ = static_cast<int>(first_visible);
}

SDL_FRect WorkspaceShell::ComputeOverlayRect(const SDL_FRect& editor_area) const {
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

std::string WorkspaceShell::BreadcrumbLabel() const {
  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return "compare";
    }
    return RelativePathLabel(project_root_, compare_tab->path) + "  |  " + compare_tab->left_label +
           " -> " + compare_tab->right_label;
  }
  if (text_viewport_.path().empty()) {
    return text_viewport_.is_placeholder() ? "welcome" : "untitled";
  }
  return RelativePathLabel(project_root_, text_viewport_.path());
}

std::string WorkspaceShell::ProjectLabel() const {
  return project_root_.empty() ? "microide" : ProjectLabelForRoot(project_root_);
}

std::string WorkspaceShell::ProjectLabelForRoot(const std::filesystem::path& root) const {
  if (root.empty()) {
    return "welcome";
  }
  const std::string filename = root.filename().string();
  return filename.empty() ? root.lexically_normal().string() : filename;
}

std::string WorkspaceShell::ProjectTabDisplayTitle(std::size_t index) const {
  if (index >= projects_.size()) {
    return {};
  }
  const std::filesystem::path root =
      (!project_root_.empty() && index == active_project_index_) ? project_root_
      : projects_[index] != nullptr                               ? projects_[index]->root
                                                                 : std::filesystem::path{};
  const std::string label = ProjectLabelForRoot(root);
  return DirtyEditorTabIndicesForProject(index).empty() ? label : "*" + label;
}

void WorkspaceShell::DrawFilledRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderFillRect(renderer, &rect);
}

void WorkspaceShell::DrawRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderRect(renderer, &rect);
}

std::string WorkspaceShell::TruncateLabel(std::string_view text, float max_width) const {
  return text_renderer_.TruncateToWidth(text, max_width);
}

WorkspaceShell::CursorKind WorkspaceShell::CursorKindForPosition(float x, float y) const {
  switch (drag_target_) {
    case DragTarget::SidebarDivider:
      return CursorKind::EwResize;
    case DragTarget::BottomPanelDivider:
      return CursorKind::NsResize;
    case DragTarget::EditorSplitDivider: {
      const auto* editor_tab = ActiveEditorTab();
      const auto* split_node = editor_tab != nullptr
                                   ? FindEditorSplitNode(editor_tab->split_root.get(),
                                                         drag_editor_split_path_)
                                   : nullptr;
      return split_node != nullptr &&
                     split_node->orientation == EditorSplitOrientation::Horizontal
                 ? CursorKind::NsResize
                 : CursorKind::EwResize;
    }
    default:
      break;
  }

  if (last_window_width_ <= 0 || last_window_height_ <= 0) {
    return CursorKind::Default;
  }

  if (dirty_prompt_visible_) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const auto buttons = ComputeDirtyPromptButtonRects(ComputeDirtyPromptRect(full));
    for (const SDL_FRect& button : buttons) {
      if (Contains(button, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (prompt_surface_visible_) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const SDL_FRect dialog = ComputePromptSurfaceRect(full);
    for (const SDL_FRect& button : ComputePromptSurfaceButtonRects(dialog)) {
      if (Contains(button, x, y)) {
        return CursorKind::Pointer;
      }
    }
    if (prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput &&
        Contains(ComputePromptSurfaceInputRect(dialog), x, y)) {
      return CursorKind::Text;
    }
    return CursorKind::Default;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);

  if (tree_context_menu_.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect();
        popup_rect.has_value() && Contains(*popup_rect, x, y)) {
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(TreeContextMenuItems(tree_context_menu_.target),
                                        tree_context_menu_.active_item_index, *popup_rect)) {
        if (Contains(item.rect, x, y)) {
          return item.separator ? CursorKind::Default : CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
  }

  if (menu_bar_open_) {
    if (const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, active_menu_id_);
        popup_rect.has_value() && Contains(*popup_rect, x, y)) {
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(active_menu_id_, *popup_rect)) {
        if (Contains(item.rect, x, y)) {
          return item.separator ? CursorKind::Default : CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
  }

  if (Contains(layout.menu_bar, x, y)) {
    for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (Contains(item.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    for (const VisibleWindowControlButton& button :
         ComputeVisibleWindowControlButtons(layout.menu_bar)) {
      if (Contains(button.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (overlay_visible_) {
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
    if (!Contains(overlay, x, y)) {
      return CursorKind::Default;
    }

    const float overlay_list_y = overlay.y + OverlayListStartOffset();
    const int overlay_visible_rows = OverlayVisibleRows(overlay);
    const int overlay_max_scroll =
        std::max(0, static_cast<int>(OverlayItemCount()) - overlay_visible_rows);
    const auto overlay_scrollbar = MakeVerticalScrollbarGeometry(
        MakeRect(overlay.x, overlay_list_y, overlay.w,
                 std::max(0.0f, overlay.y + overlay.h - overlay_list_y - 8.0f)),
        static_cast<float>(OverlayItemCount()), static_cast<float>(overlay_visible_rows),
        static_cast<float>(std::clamp(overlay_scroll_row_, 0, overlay_max_scroll)));
    if (overlay_scrollbar.has_value() && Contains(overlay_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    if (y >= overlay_list_y && y < overlay.y + overlay.h) {
      return CursorKind::Pointer;
    }

    if (overlay_mode_ == OverlayMode::BufferReplace) {
      return y >= overlay.y + 40.0f && y < overlay.y + 82.0f ? CursorKind::Text
                                                              : CursorKind::Default;
    }
    if (overlay_mode_ == OverlayMode::CommitPicker) {
      return y >= overlay.y + 58.0f && y < overlay.y + 78.0f ? CursorKind::Text
                                                              : CursorKind::Default;
    }
    return y >= overlay.y + 40.0f && y < overlay.y + 60.0f ? CursorKind::Text
                                                            : CursorKind::Default;
  }

  if (sidebar_visible_ && Contains(SidebarResizeHandleRect(layout), x, y)) {
    return CursorKind::EwResize;
  }
  if (bottom_panel_visible_ && Contains(BottomPanelResizeHandleRect(layout), x, y)) {
    return CursorKind::NsResize;
  }

  if (Contains(layout.project_tab_strip, x, y)) {
    for (const VisibleProjectTab& tab : ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (Contains(tab.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (Contains(layout.tab_strip, x, y)) {
    if (project_root_.empty()) {
      return CursorKind::Default;
    }
    if (open_tabs_.empty()) {
      return Contains(MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 5.0f, 220.0f, 24.0f),
                      x, y)
                 ? CursorKind::Pointer
                 : CursorKind::Default;
    }
    for (const VisibleTab& tab : ComputeVisibleTabs(layout.tab_strip)) {
      if (Contains(tab.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (sidebar_visible_ && Contains(layout.sidebar, x, y)) {
    if (sidebar_mode_ == SidebarMode::Search) {
      if (y < layout.sidebar.y + 66.0f) {
        return CursorKind::Text;
      }

      const auto line_map = BuildProjectSearchLineMap();
      const int visible_rows = std::max(
          1, static_cast<int>((layout.sidebar.h - kSearchSidebarResultsTop) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      const float list_y = layout.sidebar.y + kSearchSidebarResultsTop;
      const int clicked_row = static_cast<int>((y - list_y) / kSidebarRowHeight);
      const int line_index = std::clamp(sidebar_scroll_row_, 0, max_scroll) + clicked_row;
      if (clicked_row >= 0 && line_index >= 0 && line_index < static_cast<int>(line_map.size()) &&
          line_map[static_cast<std::size_t>(line_index)] >= 0) {
        const SDL_FRect row_rect = MakeRect(
            layout.sidebar.x + kSidebarInset,
            list_y + static_cast<float>(clicked_row) * kSidebarRowHeight, row_width,
            kSidebarRowHeight - 2.0f);
        return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
      }
      return CursorKind::Default;
    }

    const auto& entries = directory_tree_.entries();
    const float list_y = layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
    const int visible_rows =
        std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / kSidebarRowHeight));
    const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
    const float row_width =
        std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                           (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
    const int clicked_row = static_cast<int>((y - list_y) / kSidebarRowHeight);
    const int entry_index = std::clamp(sidebar_scroll_row_, 0, max_scroll) + clicked_row;
    if (clicked_row >= 0 && entry_index >= 0 && entry_index < static_cast<int>(entries.size())) {
      const SDL_FRect row_rect = MakeRect(layout.sidebar.x + kSidebarInset,
                                          list_y + static_cast<float>(clicked_row) * kSidebarRowHeight,
                                          row_width, kSidebarRowHeight - 2.0f);
      return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
    }
    return CursorKind::Default;
  }

  if (bottom_panel_visible_ && Contains(layout.bottom_panel, x, y)) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kBottomPanelHeaderHeight);
    if (BottomPanelShowsTerminal() && Contains(panel_header, x, y)) {
      if (Contains(BottomPanelTerminalNewTabRect(panel_header), x, y)) {
        return CursorKind::Pointer;
      }
      for (const VisibleTerminalTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
        if (Contains(tab.rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
    if (BottomPanelShowsTerminal()) {
      const std::size_t line_count =
          ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.LineCount() : 0;
      const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
      const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
      const auto scrollbar =
          MakeVerticalScrollbarGeometry(BottomPanelContentRect(layout, command_mode_),
                                        static_cast<float>(line_count),
                                        static_cast<float>(visible_rows),
                                        static_cast<float>(scroll_row));
      if (scrollbar.has_value() && Contains(scrollbar->track, x, y)) {
        return CursorKind::Default;
      }
    }
    if (command_mode_ && Contains(BottomPanelCommandPromptRect(layout), x, y)) {
      return CursorKind::Text;
    }
    if (BottomPanelShowsTerminal() && y >= layout.bottom_panel.y + kBottomPanelHeaderHeight) {
      return CursorKind::Text;
    }
    return CursorKind::Default;
  }

  if (!Contains(layout.editor_surface, x, y)) {
    return CursorKind::Default;
  }

  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return CursorKind::Default;
    }
    const int visible_rows = CompareVisibleRows(layout.editor_surface);
    const int scroll_row =
        std::clamp(compare_tab->scroll_row, 0, CompareMaxScrollRow(*compare_tab, visible_rows));
    const auto scrollbar = MakeVerticalScrollbarGeometry(
        layout.editor_surface, static_cast<float>(compare_tab->model.rows.size()),
        static_cast<float>(visible_rows), static_cast<float>(scroll_row));
    if (scrollbar.has_value() && Contains(scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    return CursorKind::Pointer;
  }

  for (const EditorSplitDividerLayout& divider :
       ComputeEditorSplitDividerLayouts(layout.editor_surface)) {
    if (Contains(divider.rect, x, y)) {
      return divider.rect.h > divider.rect.w ? CursorKind::EwResize : CursorKind::NsResize;
    }
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const auto pane_it =
      std::find_if(panes.begin(), panes.end(),
                   [&](const EditorPaneLayout& pane) { return Contains(pane.rect, x, y); });
  if (pane_it == panes.end()) {
    return CursorKind::Default;
  }

  const TabEntry::EditorTabState* editor_tab = ActiveEditorTab();
  const editor::TextViewport* viewport =
      pane_it->active ? &text_viewport_
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane_it->leaf_id)
                                               : nullptr);
  if (viewport == nullptr || viewport->is_placeholder()) {
    return CursorKind::Text;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane_it->rect);
  const std::size_t total_columns =
      std::max<std::size_t>(metrics.visible_columns, MaxVisualColumns(*viewport));
  const bool show_vertical = viewport->line_count() > metrics.visible_rows;
  const bool show_horizontal = total_columns > metrics.visible_columns;
  if (show_vertical) {
    const auto scrollbar = MakeVerticalScrollbarGeometry(
        pane_it->rect, static_cast<float>(viewport->line_count()),
        static_cast<float>(metrics.visible_rows), static_cast<float>(viewport->scroll_line()),
        show_horizontal);
    if (scrollbar.has_value() && Contains(scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
  }
  if (show_horizontal) {
    const auto scrollbar = MakeHorizontalScrollbarGeometry(
        pane_it->rect, static_cast<float>(total_columns),
        static_cast<float>(metrics.visible_columns),
        static_cast<float>(viewport->horizontal_scroll()), show_vertical);
    if (scrollbar.has_value() && Contains(scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
  }
  return CursorKind::Text;
}

SDL_Cursor* WorkspaceShell::CursorHandle(CursorKind kind) {
  switch (kind) {
    case CursorKind::Default:
      return SDL_GetDefaultCursor();
    case CursorKind::Text:
      if (text_cursor_ == nullptr) {
        text_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
      }
      return text_cursor_;
    case CursorKind::Pointer:
      if (pointer_cursor_ == nullptr) {
        pointer_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
      }
      return pointer_cursor_;
    case CursorKind::EwResize:
      if (ew_resize_cursor_ == nullptr) {
        ew_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
      }
      return ew_resize_cursor_;
    case CursorKind::NsResize:
      if (ns_resize_cursor_ == nullptr) {
        ns_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
      }
      return ns_resize_cursor_;
  }

  return SDL_GetDefaultCursor();
}

void WorkspaceShell::UpdateMouseCursor(float x, float y) {
  last_mouse_x_ = x;
  last_mouse_y_ = y;
  last_mouse_position_valid_ = true;

  const CursorKind next_kind = CursorKindForPosition(x, y);
  if (next_kind == cursor_kind_) {
    return;
  }

  if (SDL_Cursor* cursor = CursorHandle(next_kind); cursor != nullptr && SDL_SetCursor(cursor)) {
    cursor_kind_ = next_kind;
    return;
  }

  if (SDL_Cursor* default_cursor = CursorHandle(CursorKind::Default);
      default_cursor != nullptr && SDL_SetCursor(default_cursor)) {
    cursor_kind_ = CursorKind::Default;
  }
}

char WorkspaceShell::KeycodeToAscii(SDL_Keycode keycode, SDL_Keymod modifiers) {
  const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;

  if (keycode >= SDLK_A && keycode <= SDLK_Z) {
    const char base = static_cast<char>('a' + (keycode - SDLK_A));
    return shift ? static_cast<char>(std::toupper(static_cast<unsigned char>(base))) : base;
  }

  if (keycode >= SDLK_0 && keycode <= SDLK_9) {
    static constexpr char shifted_digits[] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
    const int index = keycode - SDLK_0;
    return shift ? shifted_digits[index] : static_cast<char>('0' + index);
  }

  switch (keycode) {
    case SDLK_SPACE:
      return ' ';
    case SDLK_SLASH:
      return shift ? '?' : '/';
    case SDLK_BACKSLASH:
      return shift ? '|' : '\\';
    case SDLK_PERIOD:
      return shift ? '>' : '.';
    case SDLK_COMMA:
      return shift ? '<' : ',';
    case SDLK_MINUS:
      return shift ? '_' : '-';
    case SDLK_EQUALS:
      return shift ? '+' : '=';
    case SDLK_SEMICOLON:
      return shift ? ':' : ';';
    case SDLK_APOSTROPHE:
      return shift ? '"' : '\'';
    case SDLK_LEFTBRACKET:
      return shift ? '{' : '[';
    case SDLK_RIGHTBRACKET:
      return shift ? '}' : ']';
    default:
      return '\0';
  }
}

}  // namespace microide::workspace
