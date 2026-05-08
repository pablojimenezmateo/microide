#include "render/Theme.h"

#include "platform/RuntimePaths.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "platform/Filesystem.h"
#include "util/Parse.h"

namespace microide::render {

float RelativeLuminance(SDL_Color color) {
  auto srgb_to_linear = [](Uint8 value) -> float {
    float v = static_cast<float>(value) / 255.0f;
    if (v <= 0.04045f) {
      return v / 12.92f;
    }
    return std::pow((v + 0.055f) / 1.055f, 2.4f);
  };

  float r = srgb_to_linear(color.r);
  float g = srgb_to_linear(color.g);
  float b = srgb_to_linear(color.b);
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

float Contrast(SDL_Color c1, SDL_Color c2) {
  float l1 = RelativeLuminance(c1);
  float l2 = RelativeLuminance(c2);
  if (l1 < l2) {
    std::swap(l1, l2);
  }
  return (l1 + 0.05f) / (l2 + 0.05f);
}

namespace {

struct ThemeStyle {
  std::optional<SDL_Color> foreground;
  std::optional<SDL_Color> background;
  bool reverse = false;
};

using ThemeStyleMap = std::map<std::string, ThemeStyle>;

SDL_Color MakeColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 0xff) {
  return SDL_Color{r, g, b, a};
}

SDL_Color WithAlpha(SDL_Color color, Uint8 alpha) {
  color.a = alpha;
  return color;
}

SDL_Color Mix(SDL_Color left, SDL_Color right, float t) {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  const auto mix_channel = [&](Uint8 a, Uint8 b) -> Uint8 {
    return static_cast<Uint8>(std::lround(static_cast<float>(a) * (1.0f - clamped) +
                                          static_cast<float>(b) * clamped));
  };
  return MakeColor(mix_channel(left.r, right.r), mix_channel(left.g, right.g),
                   mix_channel(left.b, right.b), mix_channel(left.a, right.a));
}

SDL_Color Lighten(SDL_Color color, float amount) {
  return Mix(color, MakeColor(0xff, 0xff, 0xff, color.a), amount);
}

SDL_Color Darken(SDL_Color color, float amount) {
  return Mix(color, MakeColor(0x00, 0x00, 0x00, color.a), amount);
}

bool IsLight(SDL_Color color) {
  return RelativeLuminance(color) >= 0.179f;
}

bool SamePolarity(SDL_Color left, SDL_Color right) {
  return IsLight(left) == IsLight(right);
}

SDL_Color EnsureContrast(SDL_Color foreground, SDL_Color background, float minimum_contrast) {
  if (Contrast(foreground, background) >= minimum_contrast) {
    return foreground;
  }

  const SDL_Color target =
      IsLight(background) ? MakeColor(0x00, 0x00, 0x00, foreground.a)
                          : MakeColor(0xff, 0xff, 0xff, foreground.a);
  SDL_Color best = foreground;
  float best_contrast = Contrast(foreground, background);
  for (int step = 1; step <= 24; ++step) {
    const float t = static_cast<float>(step) / 24.0f;
    SDL_Color candidate = Mix(foreground, target, t);
    candidate.a = foreground.a;
    const float candidate_contrast = Contrast(candidate, background);
    if (candidate_contrast > best_contrast) {
      best = candidate;
      best_contrast = candidate_contrast;
    }
    if (candidate_contrast >= minimum_contrast) {
      return candidate;
    }
  }
  return best;
}

SDL_Color EnsureBackgroundSeparation(SDL_Color background,
                                     SDL_Color reference,
                                     float minimum_contrast) {
  if (Contrast(background, reference) >= minimum_contrast) {
    return background;
  }
  return IsLight(reference) ? Darken(background, 0.12f) : Lighten(background, 0.12f);
}

std::string Trim(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }

  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

bool IsDigits(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::vector<std::string> SplitWhitespace(std::string_view text) {
  std::vector<std::string> parts;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) {
      ++index;
    }
    if (index >= text.size()) {
      break;
    }
    const std::size_t start = index;
    while (index < text.size() && !std::isspace(static_cast<unsigned char>(text[index]))) {
      ++index;
    }
    parts.emplace_back(text.substr(start, index - start));
  }
  return parts;
}

SDL_Color BasicAnsiColor(int index, bool bright) {
  static const std::array<SDL_Color, 8> kNormal = {
      MakeColor(0x1f, 0x24, 0x2c), MakeColor(0xc3, 0x4b, 0x59),
      MakeColor(0x8a, 0xb1, 0x66), MakeColor(0xd8, 0xb2, 0x5d),
      MakeColor(0x5a, 0x8c, 0xe6), MakeColor(0xb0, 0x72, 0xd1),
      MakeColor(0x56, 0xa8, 0xc9), MakeColor(0xb8, 0xc0, 0xcc),
  };
  static const std::array<SDL_Color, 8> kBright = {
      MakeColor(0x4a, 0x51, 0x5c), MakeColor(0xf0, 0x71, 0x78),
      MakeColor(0xa4, 0xc7, 0x6d), MakeColor(0xe7, 0xc5, 0x47),
      MakeColor(0x72, 0xa7, 0xff), MakeColor(0xcb, 0x8f, 0xf8),
      MakeColor(0x74, 0xc7, 0xec), MakeColor(0xf5, 0xf7, 0xfa),
  };
  const int clamped_index = std::clamp(index, 0, 7);
  return bright ? kBright[clamped_index] : kNormal[clamped_index];
}

SDL_Color Ansi256Color(int index) {
  if (index < 0) {
    return BasicAnsiColor(0, false);
  }
  if (index < 8) {
    return BasicAnsiColor(index, false);
  }
  if (index < 16) {
    return BasicAnsiColor(index - 8, true);
  }
  if (index < 232) {
    const int value = index - 16;
    const int red = value / 36;
    const int green = (value / 6) % 6;
    const int blue = value % 6;
    static constexpr std::array<Uint8, 6> kCube = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
    return MakeColor(kCube[red], kCube[green], kCube[blue]);
  }
  const Uint8 gray = static_cast<Uint8>(8 + (index - 232) * 10);
  return MakeColor(gray, gray, gray);
}

std::optional<SDL_Color> ParseThemeColor(std::string_view text) {
  const std::string token = ToLower(Trim(text));
  if (token.empty() || token == "default") {
    return std::nullopt;
  }

  if (token.size() == 7 && token[0] == '#') {
    const auto parse_pair = [&](std::size_t offset) -> std::optional<Uint8> {
      const std::string pair = token.substr(offset, 2);
      char* end = nullptr;
      const long value = std::strtol(pair.c_str(), &end, 16);
      if (end == nullptr || *end != '\0' || value < 0 || value > 255) {
        return std::nullopt;
      }
      return static_cast<Uint8>(value);
    };

    const auto red = parse_pair(1);
    const auto green = parse_pair(3);
    const auto blue = parse_pair(5);
    if (red.has_value() && green.has_value() && blue.has_value()) {
      return MakeColor(*red, *green, *blue);
    }
    return std::nullopt;
  }

  if (IsDigits(token)) {
    return Ansi256Color(util::ParseInt(token).value_or(0));
  }

  if (token == "black") {
    return MakeColor(0x00, 0x00, 0x00);
  }
  if (token == "red") {
    return MakeColor(0x80, 0x00, 0x00);
  }
  if (token == "green") {
    return MakeColor(0x00, 0x80, 0x00);
  }
  if (token == "yellow") {
    return MakeColor(0x80, 0x80, 0x00);
  }
  if (token == "blue") {
    return MakeColor(0x00, 0x00, 0x80);
  }
  if (token == "magenta") {
    return MakeColor(0x80, 0x00, 0x80);
  }
  if (token == "cyan") {
    return MakeColor(0x00, 0x80, 0x80);
  }
  if (token == "white") {
    return MakeColor(0xc0, 0xc0, 0xc0);
  }
  if (token == "brightblack" || token == "lightblack") {
    return MakeColor(0x80, 0x80, 0x80);
  }
  if (token == "brightred" || token == "lightred") {
    return MakeColor(0xff, 0x00, 0x00);
  }
  if (token == "brightgreen" || token == "lightgreen") {
    return MakeColor(0x00, 0xff, 0x00);
  }
  if (token == "brightyellow" || token == "lightyellow") {
    return MakeColor(0xff, 0xff, 0x00);
  }
  if (token == "brightblue" || token == "lightblue") {
    return MakeColor(0x00, 0x00, 0xff);
  }
  if (token == "brightmagenta" || token == "lightmagenta") {
    return MakeColor(0xff, 0x00, 0xff);
  }
  if (token == "brightcyan" || token == "lightcyan") {
    return MakeColor(0x00, 0xff, 0xff);
  }
  if (token == "brightwhite" || token == "lightwhite") {
    return MakeColor(0xff, 0xff, 0xff);
  }
  return std::nullopt;
}

ThemeStyle ParseThemeStyle(std::string_view text) {
  ThemeStyle style;
  const std::vector<std::string> parts = SplitWhitespace(text);
  if (parts.empty()) {
    return style;
  }

  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    if (ToLower(parts[i]) == "reverse") {
      style.reverse = true;
    }
  }

  const std::string colors = parts.back();
  const std::size_t comma = colors.find(',');
  const std::string foreground = comma == std::string::npos ? colors : colors.substr(0, comma);
  const std::string background =
      comma == std::string::npos ? std::string{} : colors.substr(comma + 1);
  style.foreground = ParseThemeColor(foreground);
  style.background = ParseThemeColor(background);
  if (style.reverse) {
    std::swap(style.foreground, style.background);
  }
  return style;
}

std::optional<std::string> ParseQuotedDirectiveValue(std::string_view line,
                                                     std::string_view prefix) {
  const std::string trimmed = Trim(line);
  if (trimmed.size() <= prefix.size() || trimmed.substr(0, prefix.size()) != prefix) {
    return std::nullopt;
  }

  const std::size_t first_quote = trimmed.find('"', prefix.size());
  if (first_quote == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t second_quote = trimmed.find('"', first_quote + 1);
  if (second_quote == std::string::npos || second_quote <= first_quote + 1) {
    return std::nullopt;
  }
  return trimmed.substr(first_quote + 1, second_quote - first_quote - 1);
}

bool ParseColorLink(std::string_view line, std::string& group, ThemeStyle& style) {
  const std::string trimmed = Trim(line);
  static constexpr std::string_view kPrefix = "color-link ";
  if (trimmed.size() <= kPrefix.size() || trimmed.substr(0, kPrefix.size()) != kPrefix) {
    return false;
  }

  const std::size_t group_start = kPrefix.size();
  const std::size_t group_end = trimmed.find_first_of(" \t", group_start);
  if (group_end == std::string::npos) {
    return false;
  }

  const std::size_t first_quote = trimmed.find('"', group_end);
  if (first_quote == std::string::npos) {
    return false;
  }
  const std::size_t second_quote = trimmed.find('"', first_quote + 1);
  if (second_quote == std::string::npos) {
    return false;
  }

  group = ToLower(trimmed.substr(group_start, group_end - group_start));
  style = ParseThemeStyle(trimmed.substr(first_quote + 1, second_quote - first_quote - 1));
  return !group.empty();
}

std::filesystem::path ResolveThemeDirectory(const std::filesystem::path& theme_directory) {
  if (!theme_directory.empty()) {
    return theme_directory;
  }
  return FindThemeDirectory();
}

std::filesystem::path FindThemeFile(const std::filesystem::path& theme_directory,
                                    std::string_view name) {
  if (theme_directory.empty()) {
    return {};
  }

  const std::filesystem::path direct_path = theme_directory / (std::string(name) + ".microide");
  if (platform::ReadPathType(direct_path) == platform::PathType::RegularFile) {
    return direct_path.lexically_normal();
  }

  for (const auto& entry : platform::ListDirectory(theme_directory)) {
    if (entry.type != platform::PathType::RegularFile) {
      continue;
    }
    const auto& candidate = entry.path;
    if (candidate.extension() != ".microide") {
      continue;
    }
    if (ToLower(candidate.stem().string()) == ToLower(name)) {
      return candidate.lexically_normal();
    }
  }
  return {};
}

const ThemeStyle* LookupThemeStyle(const ThemeStyleMap& styles, std::string_view key) {
  std::string current = ToLower(key);
  while (!current.empty()) {
    if (auto it = styles.find(current); it != styles.end()) {
      return &it->second;
    }
    const std::size_t dot = current.rfind('.');
    if (dot == std::string::npos) {
      break;
    }
    current.resize(dot);
  }
  return nullptr;
}

SDL_Color ResolveForeground(const ThemeStyleMap& styles,
                            std::string_view key,
                            SDL_Color fallback) {
  if (const ThemeStyle* style = LookupThemeStyle(styles, key);
      style != nullptr && style->foreground.has_value()) {
    return *style->foreground;
  }
  return fallback;
}

SDL_Color ResolveBackground(const ThemeStyleMap& styles,
                            std::string_view key,
                            SDL_Color fallback) {
  if (const ThemeStyle* style = LookupThemeStyle(styles, key);
      style != nullptr && style->background.has_value()) {
    return *style->background;
  }
  return fallback;
}

std::optional<SDL_Color> ResolveForegroundOverride(const ThemeStyleMap& styles,
                                                   std::string_view key) {
  if (const ThemeStyle* style = LookupThemeStyle(styles, key);
      style != nullptr && style->foreground.has_value()) {
    return *style->foreground;
  }
  return std::nullopt;
}

std::optional<SDL_Color> ResolveBackgroundOverride(const ThemeStyleMap& styles,
                                                   std::string_view key) {
  if (const ThemeStyle* style = LookupThemeStyle(styles, key);
      style != nullptr && style->background.has_value()) {
    return *style->background;
  }
  return std::nullopt;
}

SDL_Color ResolveUiBackground(const ThemeStyleMap& styles,
                              std::string_view key,
                              SDL_Color reference,
                              SDL_Color fallback) {
  const std::optional<SDL_Color> candidate = ResolveBackgroundOverride(styles, key);
  if (!candidate.has_value()) {
    return fallback;
  }
  return SamePolarity(*candidate, reference) ? *candidate : fallback;
}

bool LoadThemeStyles(const std::filesystem::path& theme_directory,
                     std::string_view name,
                     ThemeStyleMap& styles,
                     std::vector<std::string>& include_stack,
                     std::string& error) {
  const std::filesystem::path theme_path = FindThemeFile(theme_directory, name);
  if (theme_path.empty()) {
    error = "Unknown colorscheme: " + std::string(name);
    return false;
  }

  const std::string normalized_name = ToLower(theme_path.stem().string());
  if (std::find(include_stack.begin(), include_stack.end(), normalized_name) != include_stack.end()) {
    return true;
  }

  std::ifstream file(theme_path);
  if (!file) {
    error = "Failed to open colorscheme: " + theme_path.string();
    return false;
  }

  include_stack.push_back(normalized_name);
  std::string line;
  while (std::getline(file, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    if (const std::optional<std::string> include_name =
            ParseQuotedDirectiveValue(trimmed, "include ");
        include_name.has_value()) {
      if (!LoadThemeStyles(theme_directory, *include_name, styles, include_stack, error)) {
        include_stack.pop_back();
        return false;
      }
      continue;
    }

    std::string group;
    ThemeStyle style;
    if (ParseColorLink(trimmed, group, style)) {
      styles[group] = style;
      continue;
    }
  }

  include_stack.pop_back();
  return true;
}

Theme BuildThemeFromStyles(const ThemeStyleMap& styles) {
  Theme theme = MakeDefaultTheme();

  const SDL_Color default_foreground = ResolveForeground(styles, "default", theme.text_primary);
  const SDL_Color default_background = ResolveBackground(styles, "default", theme.editor_background);
  const SDL_Color derived_chrome_background =
      Darken(default_background, IsLight(default_background) ? 0.06f : 0.1f);
  const SDL_Color tabbar_background =
      ResolveUiBackground(styles, "tabbar", default_background, derived_chrome_background);
  const SDL_Color tabbar_foreground = ResolveForeground(styles, "tabbar", default_foreground);
  const SDL_Color statusline_background =
      ResolveUiBackground(styles, "statusline", default_background, tabbar_background);
  const SDL_Color statusline_foreground = ResolveForeground(styles, "statusline", tabbar_foreground);
  const SDL_Color chrome_active =
      ResolveUiBackground(styles, "tabbar.active", tabbar_background,
                          ResolveUiBackground(
                              styles, "cursor-line", tabbar_background,
                              Lighten(tabbar_background,
                                      IsLight(tabbar_background) ? 0.04f : 0.08f)));
  const SDL_Color chrome_active_foreground =
      ResolveForeground(styles, "tabbar.active",
                        ResolveForeground(styles, "statusline", default_foreground));
  const SDL_Color accent =
      ResolveBackground(styles, "match-brace",
                        ResolveForeground(styles, "special",
                                          ResolveForeground(styles, "identifier",
                                                            ResolveForeground(styles, "statement",
                                                                              theme.accent))));
  const SDL_Color line_number_background =
      ResolveUiBackground(styles, "line-number", default_background,
                          Mix(default_background, tabbar_background, 0.45f));
  const SDL_Color line_number_foreground =
      ResolveForeground(styles, "line-number",
                        Mix(default_foreground, default_background, 0.55f));
  const SDL_Color current_line_number =
      ResolveForeground(styles, "current-line-number",
                        ResolveForeground(styles, "line-number", default_foreground));
  const SDL_Color row_highlight =
      ResolveBackground(styles, "cursor-line",
                        Mix(default_background, accent,
                            IsLight(default_background) ? 0.08f : 0.14f));
  const SDL_Color search_match =
      ResolveBackground(styles, "hlsearch",
                        Mix(accent, default_background, IsLight(default_background) ? 0.35f : 0.55f));
  const SDL_Color selection =
      ResolveBackground(styles, "selection",
                        ResolveBackground(styles, "match-brace",
                                          Mix(accent, default_background,
                                              IsLight(default_background) ? 0.3f : 0.5f)));

  theme.window_background = Darken(default_background, IsLight(default_background) ? 0.05f : 0.18f);
  theme.chrome_background =
      EnsureBackgroundSeparation(statusline_background, default_background, 1.08f);
  theme.chrome_active =
      EnsureBackgroundSeparation(chrome_active, theme.chrome_background, 1.12f);
  theme.chrome_text = EnsureContrast(statusline_foreground, theme.chrome_background, 4.5f);
  theme.chrome_active_text =
      EnsureContrast(chrome_active_foreground, theme.chrome_active, 4.5f);
  theme.chrome_text_secondary =
      EnsureContrast(Mix(theme.chrome_text, theme.chrome_background, 0.25f),
                     theme.chrome_background, 3.0f);
  theme.surface_background = EnsureBackgroundSeparation(
      Mix(default_background, theme.chrome_background, 0.18f), default_background, 1.04f);
  theme.surface_raised = EnsureBackgroundSeparation(
      Mix(theme.surface_background, theme.chrome_active, 0.55f), theme.surface_background, 1.08f);
  theme.surface_text = EnsureContrast(
      ResolveForegroundOverride(styles, "tabbar").value_or(default_foreground),
      theme.surface_background, 4.5f);
  theme.editor_background = default_background;
  theme.gutter_background = line_number_background;
  theme.overlay_background = WithAlpha(theme.surface_raised, 0xf6);
  theme.overlay_backdrop =
      WithAlpha(Darken(default_background, IsLight(default_background) ? 0.28f : 0.36f), 0x94);
  theme.border = EnsureContrast(Mix(default_foreground, default_background, 0.72f),
                                theme.surface_background, 1.5f);
  theme.accent = accent;
  theme.text_primary = EnsureContrast(default_foreground, theme.editor_background, 4.5f);
  theme.text_secondary =
      EnsureContrast(Mix(default_foreground, default_background, 0.22f), theme.editor_background,
                     3.0f);
  theme.text_muted =
      EnsureContrast(Mix(default_foreground, default_background, 0.4f), theme.editor_background,
                     2.2f);
  theme.text_disabled =
      EnsureContrast(Mix(default_foreground, default_background, 0.58f), theme.editor_background,
                     1.7f);
  theme.row_highlight = row_highlight;
  theme.selection_fill = WithAlpha(selection, 0xb4);
  theme.search_match = WithAlpha(search_match, 0x8f);
  theme.search_match_active =
      WithAlpha(Lighten(search_match, IsLight(search_match) ? 0.04f : 0.12f), 0xc8);
  theme.bracket_match_background = WithAlpha(Mix(selection, theme.accent, 0.35f), 0xa6);
  theme.cursor = EnsureContrast(default_foreground, theme.editor_background, 4.5f);
  theme.syntax_keyword = ResolveForeground(styles, "statement", theme.accent);
  theme.syntax_type = ResolveForeground(styles, "type", theme.text_primary);
  theme.syntax_string =
      ResolveForeground(styles, "constant.string",
                        ResolveForeground(styles, "constant", theme.text_primary));
  theme.syntax_comment = EnsureContrast(
      ResolveForeground(styles, "comment", theme.text_muted), theme.editor_background, 2.1f);
  theme.syntax_number =
      ResolveForeground(styles, "constant.number",
                        ResolveForeground(styles, "constant", theme.text_primary));
  theme.syntax_constant = ResolveForeground(styles, "constant", theme.text_primary);
  theme.syntax_preprocessor =
      ResolveForeground(styles, "preproc",
                        ResolveForeground(styles, "special", theme.accent));
  theme.syntax_operator =
      ResolveForeground(styles, "symbol.operator",
                        ResolveForeground(styles, "symbol",
                                          ResolveForeground(styles, "statement", theme.accent)));
  theme.line_number = EnsureContrast(line_number_foreground, theme.gutter_background, 2.3f);
  theme.current_line_number =
      EnsureContrast(current_line_number, theme.gutter_background, 4.0f);
  theme.diff_added = ResolveForeground(styles, "diff-added", theme.diff_added);
  theme.diff_modified = ResolveForeground(styles, "diff-modified", theme.diff_modified);
  theme.diff_deleted = ResolveForeground(styles, "diff-deleted", theme.diff_deleted);
  theme.diagnostic_error = ResolveForeground(styles, "diagnostic-error", theme.diagnostic_error);
  theme.diagnostic_warning =
      ResolveForeground(styles, "diagnostic-warning", theme.diagnostic_warning);
  theme.diagnostic_info = ResolveForeground(styles, "diagnostic-info", theme.diagnostic_info);
  theme.diagnostic_hint = ResolveForeground(styles, "diagnostic-hint", theme.diagnostic_hint);
  return theme;
}

}  // namespace

Theme MakeDefaultTheme() {
  return Theme{
      .window_background = SDL_Color{0x0a, 0x0d, 0x14, 0xff},
      .chrome_background = SDL_Color{0x1a, 0x1f, 0x2b, 0xff},
      .chrome_active = SDL_Color{0x24, 0x2b, 0x3a, 0xff},
      .chrome_text = SDL_Color{0xda, 0xe0, 0xe8, 0xff},
      .chrome_active_text = SDL_Color{0xff, 0xff, 0xff, 0xff},
      .chrome_text_secondary = SDL_Color{0xb4, 0xbe, 0xcc, 0xff},
      .surface_background = SDL_Color{0x16, 0x1b, 0x26, 0xff},
      .surface_raised = SDL_Color{0x1d, 0x23, 0x31, 0xff},
      .surface_text = SDL_Color{0xda, 0xe0, 0xe8, 0xff},
      .editor_background = SDL_Color{0x11, 0x15, 0x1d, 0xff},
      .gutter_background = SDL_Color{0x0f, 0x13, 0x1b, 0xff},
      .overlay_background = SDL_Color{0x1a, 0x20, 0x2c, 0xf6},
      .overlay_backdrop = SDL_Color{0x06, 0x08, 0x0d, 0xaa},
      .border = SDL_Color{0x2c, 0x34, 0x45, 0xff},
      .accent = SDL_Color{0x66, 0xa4, 0xff, 0xff},
      .text_primary = SDL_Color{0xda, 0xe0, 0xe8, 0xff},
      .text_secondary = SDL_Color{0xb4, 0xbe, 0xcc, 0xff},
      .text_muted = SDL_Color{0x8a, 0x95, 0xa6, 0xff},
      .text_disabled = SDL_Color{0x66, 0x70, 0x80, 0xff},
      .row_highlight = SDL_Color{0x1e, 0x25, 0x33, 0xff},
      .selection_fill = SDL_Color{0x2b, 0x4f, 0x7a, 0xc0},
      .search_match = SDL_Color{0x65, 0x56, 0x1b, 0x96},
      .search_match_active = SDL_Color{0xc0, 0x95, 0x3d, 0xd0},
      .bracket_match_background = SDL_Color{0x3a, 0x52, 0x73, 0xa6},
      .cursor = SDL_Color{0xff, 0xff, 0xff, 0xff},
      .syntax_keyword = SDL_Color{0xc6, 0x78, 0xdd, 0xff},
      .syntax_type = SDL_Color{0x61, 0xaf, 0xef, 0xff},
      .syntax_string = SDL_Color{0x98, 0xc3, 0x79, 0xff},
      .syntax_comment = SDL_Color{0x6b, 0x72, 0x80, 0xff},
      .syntax_number = SDL_Color{0xd1, 0x9a, 0x66, 0xff},
      .syntax_constant = SDL_Color{0xe0, 0x6c, 0x75, 0xff},
      .syntax_preprocessor = SDL_Color{0xe5, 0xc0, 0x7b, 0xff},
      .syntax_operator = SDL_Color{0x56, 0xb6, 0xc2, 0xff},
      .line_number = SDL_Color{0x5f, 0x69, 0x79, 0xff},
      .current_line_number = SDL_Color{0x9f, 0xa9, 0xb8, 0xff},
      .diff_added = SDL_Color{0x73, 0xa9, 0x83, 0xff},
      .diff_modified = SDL_Color{0xb8, 0xa0, 0x70, 0xff},
      .diff_deleted = SDL_Color{0xbc, 0x85, 0x8b, 0xff},
      .diagnostic_error = SDL_Color{0xe0, 0x72, 0x7b, 0xff},
      .diagnostic_warning = SDL_Color{0xe0, 0xbc, 0x6d, 0xff},
      .diagnostic_info = SDL_Color{0x66, 0xa4, 0xff, 0xff},
      .diagnostic_hint = SDL_Color{0x7f, 0xc9, 0x7f, 0xff},
  };
}

std::filesystem::path FindThemeDirectory() {
  const std::vector<std::filesystem::path> candidates = {
      platform::ResolveBundledAssetPath("themes"),
      std::filesystem::path("assets") / "themes",
      std::filesystem::path("microide") / "assets" / "themes",
  };

  for (const auto& candidate : candidates) {
    if (platform::ReadPathType(candidate) == platform::PathType::Directory) {
      return candidate.lexically_normal();
    }
  }
  return {};
}

std::vector<std::string> ListAvailableThemeNames(const std::filesystem::path& theme_directory) {
  const std::filesystem::path resolved_directory = ResolveThemeDirectory(theme_directory);
  std::vector<std::string> names;
  if (resolved_directory.empty()) {
    names.push_back("default");
    return names;
  }

  for (const auto& entry : platform::ListDirectory(resolved_directory)) {
    if (entry.type != platform::PathType::RegularFile) {
      continue;
    }
    const auto& path = entry.path;
    if (path.extension() != ".microide") {
      continue;
    }
    names.push_back(path.stem().string());
  }

  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  if (names.empty()) {
    names.push_back("default");
  }
  return names;
}

bool LoadThemeByName(std::string_view name,
                     Theme& out_theme,
                     std::string* resolved_name,
                     std::string* error,
                     const std::filesystem::path& theme_directory) {
  const std::string requested_name = name.empty() ? "default" : std::string(name);
  if (ToLower(requested_name) == "default") {
    out_theme = MakeDefaultTheme();
    if (resolved_name != nullptr) {
      *resolved_name = "default";
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const std::filesystem::path resolved_directory = ResolveThemeDirectory(theme_directory);
  if (resolved_directory.empty()) {
    if (error != nullptr) {
      *error = "No bundled colorscheme assets found";
    }
    return false;
  }

  ThemeStyleMap styles;
  std::vector<std::string> include_stack;
  std::string load_error;
  if (!LoadThemeStyles(resolved_directory, requested_name, styles, include_stack, load_error)) {
    if (error != nullptr) {
      *error = load_error;
    }
    return false;
  }

  const std::filesystem::path theme_file = FindThemeFile(resolved_directory, requested_name);
  out_theme = BuildThemeFromStyles(styles);
  if (resolved_name != nullptr) {
    *resolved_name = theme_file.empty() ? requested_name : theme_file.stem().string();
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

}  // namespace microide::render
