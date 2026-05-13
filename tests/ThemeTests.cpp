#include "TestSupport.h"

#include "render/Theme.h"
#include "workspace/WorkspaceProjectPresentation.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

SDL_Color CompositeOver(SDL_Color foreground, SDL_Color background) {
  const float alpha = static_cast<float>(foreground.a) / 255.0f;
  const auto blend = [&](Uint8 fg, Uint8 bg) {
    return static_cast<Uint8>(
        std::clamp(std::lround(static_cast<float>(fg) * alpha +
                               static_cast<float>(bg) * (1.0f - alpha)),
                   0l, 255l));
  };
  return SDL_Color{blend(foreground.r, background.r),
                   blend(foreground.g, background.g),
                   blend(foreground.b, background.b),
                   0xff};
}

void ExpectContrastAtLeast(std::string_view label,
                           SDL_Color foreground,
                           SDL_Color background,
                           float minimum) {
  const float actual = render::Contrast(foreground, background);
  Expect(actual >= minimum,
         std::string(label) + " contrast " + std::to_string(actual) +
             " should be at least " + std::to_string(minimum));
}

void ExpectContrastAtMost(std::string_view label,
                          SDL_Color foreground,
                          SDL_Color background,
                          float maximum) {
  const float actual = render::Contrast(foreground, background);
  Expect(actual <= maximum,
         std::string(label) + " contrast " + std::to_string(actual) +
             " should be at most " + std::to_string(maximum));
}

void ExpectReadableDefaultThemeForegrounds() {
  const render::Theme theme = render::MakeDefaultTheme();
  const SDL_Color editor = theme.editor_background;

  ExpectContrastAtLeast("default text primary", theme.text_primary, editor, 7.0f);
  ExpectContrastAtLeast("default text secondary", theme.text_secondary, editor, 4.5f);
  ExpectContrastAtLeast("default text muted", theme.text_muted, editor, 4.5f);
  ExpectContrastAtLeast("default text disabled", theme.text_disabled, editor, 3.0f);
  ExpectContrastAtLeast("default surface text", theme.surface_text, theme.surface_background, 7.0f);
  ExpectContrastAtLeast("default chrome text", theme.chrome_text, theme.chrome_background, 7.0f);

  ExpectContrastAtLeast("default syntax keyword", theme.syntax_keyword, editor, 4.5f);
  ExpectContrastAtLeast("default syntax type", theme.syntax_type, editor, 4.5f);
  ExpectContrastAtLeast("default syntax string", theme.syntax_string, editor, 4.5f);
  ExpectContrastAtLeast("default syntax comment", theme.syntax_comment, editor, 4.5f);
  ExpectContrastAtLeast("default syntax number", theme.syntax_number, editor, 4.5f);
  ExpectContrastAtLeast("default syntax constant", theme.syntax_constant, editor, 4.5f);
  ExpectContrastAtLeast("default syntax preprocessor", theme.syntax_preprocessor, editor, 4.5f);
  ExpectContrastAtLeast("default syntax operator", theme.syntax_operator, editor, 4.5f);

  ExpectContrastAtLeast("default line number", theme.line_number, theme.gutter_background, 3.0f);
  ExpectContrastAtLeast("default current line number", theme.current_line_number,
                        theme.gutter_background, 4.5f);
  ExpectContrastAtLeast("default diff added", theme.diff_added, editor, 4.5f);
  ExpectContrastAtLeast("default diff modified", theme.diff_modified, editor, 4.5f);
  ExpectContrastAtLeast("default diff deleted", theme.diff_deleted, editor, 4.5f);
  ExpectContrastAtLeast("default diagnostic error", theme.diagnostic_error, editor, 4.5f);
  ExpectContrastAtLeast("default diagnostic warning", theme.diagnostic_warning, editor, 4.5f);
  ExpectContrastAtLeast("default diagnostic info", theme.diagnostic_info, editor, 4.5f);
  ExpectContrastAtLeast("default diagnostic hint", theme.diagnostic_hint, editor, 4.5f);
}

void ExpectReadableDecorationUnderlays(const render::Theme& theme, std::string_view label_prefix) {
  const SDL_Color editor = theme.editor_background;
  const SDL_Color occurrence = CompositeOver(theme.search_match, editor);
  const SDL_Color active_occurrence = CompositeOver(theme.search_match_active, editor);
  const SDL_Color selection = CompositeOver(theme.selection_fill, editor);
  const std::string prefix(label_prefix);

  ExpectContrastAtLeast(prefix + " occurrence is visible", occurrence, editor, 1.10f);
  ExpectContrastAtMost(prefix + " occurrence stays subdued", occurrence, editor, 1.70f);
  ExpectContrastAtLeast(prefix + " active occurrence is visible", active_occurrence, editor, 1.25f);
  ExpectContrastAtMost(prefix + " active occurrence stays behind text", active_occurrence, editor,
                       2.35f);
  ExpectContrastAtMost(prefix + " selection stays behind text", selection, editor, 2.10f);

  ExpectContrastAtLeast(prefix + " primary text over occurrence", theme.text_primary, occurrence,
                        4.5f);
  ExpectContrastAtLeast(prefix + " primary text over active occurrence", theme.text_primary,
                        active_occurrence, 4.5f);
  ExpectContrastAtLeast(prefix + " primary text over selection", theme.text_primary, selection,
                        4.5f);
  ExpectContrastAtLeast(prefix + " comments over occurrence", theme.syntax_comment, occurrence,
                        4.0f);
  ExpectContrastAtLeast(prefix + " comments over active occurrence", theme.syntax_comment,
                        active_occurrence, 2.9f);
  ExpectContrastAtLeast(prefix + " comments over selection", theme.syntax_comment, selection, 2.9f);
}

void ExpectDefaultThemeDecorationUnderlaysStayReadable() {
  ExpectReadableDecorationUnderlays(render::MakeDefaultTheme(), "default");
}

void ExpectProjectAccentDecorationUnderlaysStayReadable() {
  const std::vector<SDL_Color> accents = {
      SDL_Color{0x7f, 0xc9, 0x7f, 0xff},
      SDL_Color{0xbe, 0xae, 0xd4, 0xff},
      SDL_Color{0xfd, 0xc0, 0x86, 0xff},
      SDL_Color{0xff, 0xff, 0x99, 0xff},
      SDL_Color{0x38, 0x6c, 0xb0, 0xff},
      SDL_Color{0xf0, 0x02, 0x7f, 0xff},
      SDL_Color{0xbf, 0x5b, 0x17, 0xff},
      SDL_Color{0x66, 0x66, 0x66, 0xff},
      SDL_Color{0x5a, 0x90, 0xca, 0xff},
      SDL_Color{0xd8, 0x4d, 0xa0, 0xff},
  };
  for (std::size_t index = 0; index < accents.size(); ++index) {
    render::Theme theme = render::MakeDefaultTheme();
    workspace::ApplyProjectAccent(theme, accents[index]);
    ExpectReadableDecorationUnderlays(theme, "project accent " + std::to_string(index));
  }
}

void ExpectClassicDarkThemeAssetLoads() {
  const std::filesystem::path theme_directory = TestRoot().parent_path() / "assets" / "themes";
  render::Theme theme;
  std::string resolved_name;
  std::string error;
  Expect(render::LoadThemeByName("microide-classic-dark", theme, &resolved_name, &error,
                                 theme_directory),
         "classic dark theme asset should load: " + error);
  Expect(resolved_name == "microide-classic-dark",
         "classic dark theme should resolve to its asset name");
  ExpectContrastAtLeast("classic dark text primary", theme.text_primary, theme.editor_background,
                        4.5f);
}

}  // namespace

void RegisterThemeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Theme default foregrounds preserve readable contrast",
          ExpectReadableDefaultThemeForegrounds);
  AddTest(tests, "Theme default decoration underlays stay readable",
          ExpectDefaultThemeDecorationUnderlaysStayReadable);
  AddTest(tests, "Theme project accent decoration underlays stay readable",
          ExpectProjectAccentDecorationUnderlaysStayReadable);
  AddTest(tests, "Theme classic dark asset loads", ExpectClassicDarkThemeAssetLoads);
}

}  // namespace microide::tests
