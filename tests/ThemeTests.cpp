#include "TestSupport.h"

#include "render/AnsiPalette.h"
#include "render/Theme.h"
#include "terminal/TerminalAnsiColors.h"
#include "workspace/WorkspaceProjectPresentation.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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

  // selection_strong is the opaque highlight used on solid panels (e.g. the
  // Settings overlay), so nothing behind it may ghost through and primary text
  // must stay readable on top of it.
  Expect(theme.selection_strong.a == 0xff, prefix + " selection_strong is opaque");
  ExpectContrastAtLeast(prefix + " primary text over selection_strong", theme.text_primary,
                        theme.selection_strong, 4.5f);
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

void ExpectSharedAnsiPaletteParity() {
  // The terminal palette delegates to render::AnsiPalette; lock that they agree
  // across the full 16-colour table and representative 256-colour indices so the
  // shared module can never silently diverge from a re-introduced copy.
  const auto same = [](SDL_Color a, SDL_Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
  };
  for (int i = 0; i < 8; ++i) {
    Expect(same(render::BasicAnsiColor(i, false), terminal::BasicAnsiColor(i, false)),
           "normal ANSI colour " + std::to_string(i) + " should match the terminal palette");
    Expect(same(render::BasicAnsiColor(i, true), terminal::BasicAnsiColor(i, true)),
           "bright ANSI colour " + std::to_string(i) + " should match the terminal palette");
  }
  for (int index : {0, 7, 15, 16, 100, 231, 232, 255}) {
    Expect(same(render::Ansi256Color(index), terminal::Ansi256Color(index)),
           "256-colour index " + std::to_string(index) + " should match the terminal palette");
  }
}

void ExpectAnsi256ColorClampsOutOfRangeIndices() {
  const auto same = [](SDL_Color a, SDL_Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
  };
  const SDL_Color fallback = render::Ansi256Color(-1);
  // Regression: indices above 255 used to fall through the grayscale ramp with no
  // upper bound and wrap via Uint8 truncation (e.g. 257 -> near-black, 256 ->
  // near-white). They must now resolve to the same out-of-range fallback as < 0.
  Expect(same(render::Ansi256Color(256), fallback),
         "index 256 is out of range and must not wrap into the grayscale ramp");
  Expect(same(render::Ansi256Color(300), fallback),
         "index 300 is out of range and must clamp to the fallback colour");
  Expect(same(render::Ansi256Color(1'000'000), fallback),
         "a very large index must clamp rather than overflow the ramp math");
  // The last valid grayscale index (255) stays a bright gray, distinct from fallback.
  const SDL_Color last_gray = render::Ansi256Color(255);
  Expect(last_gray.r == 238 && last_gray.g == 238 && last_gray.b == 238,
         "index 255 remains the top of the grayscale ramp (0xEE)");
}

void ExpectSelfIncludingThemeLoadsWithoutRecursion() {
  // A colorscheme that includes itself must terminate via the include-cycle
  // guard rather than recursing forever, and still apply its own color-links.
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "microide_theme_cycle_test";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  Expect(!ec, "should be able to create a temp theme directory");

  {
    std::ofstream out(dir / "loop.microide");
    out << "include \"loop\"\n";
    out << "color-link default \"#ffffff,#101010\"\n";
  }

  render::Theme theme;
  std::string resolved_name;
  std::string error;
  const bool loaded = render::LoadThemeByName("loop", theme, &resolved_name, &error, dir);
  std::filesystem::remove_all(dir, ec);

  Expect(loaded, "a self-including colorscheme should load: " + error);
  Expect(resolved_name == "loop", "self-including colorscheme should resolve to its own name");
  ExpectContrastAtLeast("self-include default text", theme.text_primary, theme.editor_background,
                        4.5f);
}

void ExpectBuiltinLightThemeIsSelectableAndReadable() {
  // The built-in light theme resolves by name without any bundled assets.
  microide::render::Theme theme;
  std::string resolved_name;
  std::string error;
  const bool loaded = microide::render::LoadThemeByName("light", theme, &resolved_name, &error);
  Expect(loaded, "the built-in light theme should load: " + error);
  Expect(resolved_name == "light", "light colorscheme should resolve to its own name");

  // It must actually be light: a bright editor background and dark primary text.
  const auto brightness = [](SDL_Color c) {
    return static_cast<int>(c.r) + static_cast<int>(c.g) + static_cast<int>(c.b);
  };
  Expect(brightness(theme.editor_background) > brightness(theme.text_primary) + 300,
         "the light theme should have a bright background and dark text");
  ExpectContrastAtLeast("light theme primary text", theme.text_primary, theme.editor_background,
                        4.5f);

  // Both built-in themes are always offered, even with no theme directory.
  const std::vector<std::string> names = microide::render::ListAvailableThemeNames({});
  Expect(std::find(names.begin(), names.end(), "light") != names.end(),
         "the light theme should appear in the available colorscheme list");
  Expect(std::find(names.begin(), names.end(), "default") != names.end(),
         "the default theme should appear in the available colorscheme list");
}

}  // namespace

void RegisterThemeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Theme built-in light is selectable and readable",
          ExpectBuiltinLightThemeIsSelectableAndReadable);
  AddTest(tests, "Theme shared ANSI palette parity", ExpectSharedAnsiPaletteParity);
  AddTest(tests, "Theme ANSI 256 colour clamps out-of-range indices",
          ExpectAnsi256ColorClampsOutOfRangeIndices);
  AddTest(tests, "Theme self-including colorscheme loads without recursion",
          ExpectSelfIncludingThemeLoadsWithoutRecursion);
  AddTest(tests, "Theme default foregrounds preserve readable contrast",
          ExpectReadableDefaultThemeForegrounds);
  AddTest(tests, "Theme default decoration underlays stay readable",
          ExpectDefaultThemeDecorationUnderlaysStayReadable);
  AddTest(tests, "Theme project accent decoration underlays stay readable",
          ExpectProjectAccentDecorationUnderlaysStayReadable);
  AddTest(tests, "Theme classic dark asset loads", ExpectClassicDarkThemeAssetLoads);
}

}  // namespace microide::tests
