#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/ColorMath.h"
#include "render/ThemeFile.h"

namespace microide::render {

struct Theme {
  SDL_Color window_background;
  SDL_Color chrome_background;
  SDL_Color chrome_active;
  SDL_Color chrome_text;
  SDL_Color chrome_active_text;
  SDL_Color chrome_text_secondary;
  SDL_Color surface_background;
  SDL_Color surface_raised;
  SDL_Color surface_text;
  SDL_Color editor_background;
  SDL_Color gutter_background;
  SDL_Color overlay_background;
  SDL_Color overlay_backdrop;
  SDL_Color border;
  SDL_Color accent;
  SDL_Color text_primary;
  SDL_Color text_secondary;
  SDL_Color text_muted;
  SDL_Color text_disabled;
  SDL_Color row_highlight;
  SDL_Color selection_fill;
  SDL_Color selection_strong;
  SDL_Color search_match;
  SDL_Color search_match_active;
  SDL_Color bracket_match_background;
  SDL_Color cursor;
  SDL_Color syntax_keyword;
  SDL_Color syntax_type;
  SDL_Color syntax_string;
  SDL_Color syntax_comment;
  SDL_Color syntax_number;
  SDL_Color syntax_constant;
  SDL_Color syntax_preprocessor;
  SDL_Color syntax_operator;
  SDL_Color line_number;
  SDL_Color current_line_number;
  SDL_Color diff_added;
  SDL_Color diff_modified;
  SDL_Color diff_deleted;
  SDL_Color diff_moved;
  SDL_Color diagnostic_error;
  SDL_Color diagnostic_warning;
  SDL_Color diagnostic_info;
  SDL_Color diagnostic_hint;
  SDL_Color breakpoint;
  SDL_Color breakpoint_unverified;
  SDL_Color breakpoint_conditional;
  SDL_Color debug_execution_line;
  SDL_Color debug_execution_arrow;
};

Theme MakeDefaultTheme();
Theme MakeLightTheme();
Theme BuildThemeFromStyles(const ThemeStyleMap& styles);

inline SDL_Color FindScopeFill(const Theme& theme) {
  const SDL_Color selection = theme.selection_fill;
  return SDL_Color{selection.r, selection.g, selection.b, static_cast<Uint8>(selection.a / 2)};
}
std::filesystem::path FindThemeDirectory();
std::vector<std::string> ListAvailableThemeNames(
    const std::filesystem::path& theme_directory = {});

class ThemeNameCatalog {
 public:
  const std::vector<std::string>& Names(const std::filesystem::path& theme_directory = {});

 private:
  std::vector<std::string> names_;
  std::filesystem::path requested_directory_;
  std::filesystem::path resolved_directory_;
  std::filesystem::file_time_type resolved_mtime_{};
  bool valid_ = false;
};
bool LoadThemeByName(std::string_view name,
                     Theme& out_theme,
                     std::string* resolved_name = nullptr,
                     std::string* error = nullptr,
                     const std::filesystem::path& theme_directory = {});

}
