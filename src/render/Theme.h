#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace microide::render {

struct Theme {
  SDL_Color window_background;
  SDL_Color chrome_background;
  SDL_Color chrome_active;
  SDL_Color surface_background;
  SDL_Color surface_raised;
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
  SDL_Color search_match;
  SDL_Color search_match_active;
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
};

Theme MakeDefaultTheme();
std::filesystem::path FindThemeDirectory();
std::vector<std::string> ListAvailableThemeNames(
    const std::filesystem::path& theme_directory = {});
bool LoadThemeByName(std::string_view name,
                     Theme& out_theme,
                     std::string* resolved_name = nullptr,
                     std::string* error = nullptr,
                     const std::filesystem::path& theme_directory = {});

}  // namespace microide::render
