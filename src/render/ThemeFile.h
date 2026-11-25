#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::render {

struct ThemeStyle {
  std::optional<SDL_Color> foreground;
  std::optional<SDL_Color> background;
  bool reverse = false;
};

using ThemeStyleMap = std::map<std::string, ThemeStyle>;

std::optional<SDL_Color> ParseThemeColor(std::string_view text);

std::filesystem::path FindThemeFile(const std::filesystem::path& theme_directory,
                                    std::string_view name);

bool LoadThemeStyles(const std::filesystem::path& theme_directory,
                     std::string_view name,
                     ThemeStyleMap& styles,
                     std::vector<std::string>& include_stack,
                     std::string& error);

}
