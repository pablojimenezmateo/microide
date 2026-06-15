#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Parsing of `.microide` colorscheme files into a style map. The map is then
// consumed by the theme derivation in Theme.cpp; this unit only understands the
// file format (color-link/include directives, colour tokens), not how those
// styles become a Theme.
namespace microide::render {

// One parsed `color-link` entry: foreground/background colours plus the reverse
// flag (which swaps the two). Absent colours stay nullopt so derivation can fall
// back to computed defaults.
struct ThemeStyle {
  std::optional<SDL_Color> foreground;
  std::optional<SDL_Color> background;
  bool reverse = false;
};

// Map from lower-cased highlight-group name (e.g. "statement", "comment") to its
// parsed style.
using ThemeStyleMap = std::map<std::string, ThemeStyle>;

// Resolve `<theme_directory>/<name>.microide` (case-insensitively), returning an
// empty path when not found.
std::filesystem::path FindThemeFile(const std::filesystem::path& theme_directory,
                                    std::string_view name);

// Load `name` (and any files it `include`s) into `styles`. `include_stack`
// guards against include cycles; pass an empty vector for the top-level call.
// Returns false and sets `error` on a missing/unreadable file.
bool LoadThemeStyles(const std::filesystem::path& theme_directory,
                     std::string_view name,
                     ThemeStyleMap& styles,
                     std::vector<std::string>& include_stack,
                     std::string& error);

}  // namespace microide::render
