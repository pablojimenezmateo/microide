#include "render/ThemeFile.h"

#include <algorithm>
#include <fstream>

#include "platform/Filesystem.h"
#include "render/AnsiPalette.h"
#include "util/Hex.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microide::render {

namespace {

SDL_Color MakeColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 0xff) {
  return SDL_Color{r, g, b, a};
}

std::optional<SDL_Color> ParseThemeColor(std::string_view text) {
  const std::string token = util::ToLowerAscii(util::TrimAsciiWhitespace(text));
  if (token.empty() || token == "default") {
    return std::nullopt;
  }

  if (token.size() == 7 && token[0] == '#') {
    if (const auto rgb = util::DecodeHexColor(token)) {
      return MakeColor((*rgb)[0], (*rgb)[1], (*rgb)[2]);
    }
    return std::nullopt;
  }

  if (util::IsAllAsciiDigits(token)) {
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
  const std::vector<std::string_view> parts = util::SplitAsciiWhitespace(text);
  if (parts.empty()) {
    return style;
  }

  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    if (util::ToLowerAscii(parts[i]) == "reverse") {
      style.reverse = true;
    }
  }

  const std::string colors(parts.back());
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
  const std::string trimmed = util::TrimAsciiWhitespace(line);
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
  const std::string trimmed = util::TrimAsciiWhitespace(line);
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

  group = util::ToLowerAscii(trimmed.substr(group_start, group_end - group_start));
  style = ParseThemeStyle(trimmed.substr(first_quote + 1, second_quote - first_quote - 1));
  return !group.empty();
}

}  // namespace

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
    if (util::ToLowerAscii(candidate.stem().string()) == util::ToLowerAscii(name)) {
      return candidate.lexically_normal();
    }
  }
  return {};
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

  const std::string normalized_name = util::ToLowerAscii(theme_path.stem().string());
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
    const std::string trimmed = util::TrimAsciiWhitespace(line);
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

}  // namespace microide::render
