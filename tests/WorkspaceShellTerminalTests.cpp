#include "TestSupport.h"

#include "support/SoftwareCanvas.h"

#include "TerminalSessionTestAccess.h"
#include "workspace/ListSelection.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "workspace/WorkspaceTerminalSelection.h"

#include "render/TextRenderer.h"
#include "render/Theme.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

void EnsureDummySdlVideo() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "SDL should initialize with the dummy video driver");
}


int CountNonBackgroundPixels(SDL_Surface* surface,
                             const SDL_Rect& rect,
                             SDL_Color background) {
  Expect(surface != nullptr, "pixel counting should receive a readable surface");
  int count = 0;
  for (int y = rect.y; y < rect.y + rect.h; ++y) {
    for (int x = rect.x; x < rect.x + rect.w; ++x) {
      Uint8 r = 0;
      Uint8 g = 0;
      Uint8 b = 0;
      Uint8 a = 0;
      Expect(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a),
             "pixel counting should read render output");
      if (r != background.r || g != background.g || b != background.b || a != background.a) {
        ++count;
      }
    }
  }
  return count;
}

SDL_Rect InflateClipRect(const SDL_FRect& rect,
                         microide::render::TextClipPadding padding,
                         int width,
                         int height) {
  const int x0 =
      std::max(0, static_cast<int>(std::floor(rect.x - padding.left)));
  const int y0 =
      std::max(0, static_cast<int>(std::floor(rect.y - padding.top)));
  const int x1 = std::min(
      width, static_cast<int>(std::ceil(rect.x + rect.w + padding.right)));
  const int y1 = std::min(
      height, static_cast<int>(std::ceil(rect.y + rect.h + padding.bottom)));
  return SDL_Rect{
      .x = x0,
      .y = y0,
      .w = std::max(0, x1 - x0),
      .h = std::max(0, y1 - y0),
  };
}

std::size_t CountPixelDifferences(SDL_Surface* lhs, SDL_Surface* rhs) {
  Expect(lhs != nullptr && rhs != nullptr,
         "surface comparisons should receive readable surfaces");
  Expect(lhs->w == rhs->w && lhs->h == rhs->h,
         "surface comparisons should use canvases with matching dimensions");

  std::size_t differences = 0;
  for (int y = 0; y < lhs->h; ++y) {
    for (int x = 0; x < lhs->w; ++x) {
      Uint8 lhs_r = 0;
      Uint8 lhs_g = 0;
      Uint8 lhs_b = 0;
      Uint8 lhs_a = 0;
      Uint8 rhs_r = 0;
      Uint8 rhs_g = 0;
      Uint8 rhs_b = 0;
      Uint8 rhs_a = 0;
      Expect(SDL_ReadSurfacePixel(lhs, x, y, &lhs_r, &lhs_g, &lhs_b, &lhs_a),
             "surface comparisons should read actual pixels");
      Expect(SDL_ReadSurfacePixel(rhs, x, y, &rhs_r, &rhs_g, &rhs_b, &rhs_a),
             "surface comparisons should read reference pixels");
      if (lhs_r != rhs_r || lhs_g != rhs_g || lhs_b != rhs_b || lhs_a != rhs_a) {
        ++differences;
      }
    }
  }
  return differences;
}

SDL_Color ReadSurfacePixelOrThrow(SDL_Surface* surface, int x, int y) {
  Expect(surface != nullptr, "pixel reads should receive a readable surface");
  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  Expect(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a),
         "pixel reads should read render output");
  return SDL_Color{r, g, b, a};
}

void TestWorkspaceShellCtrlShiftVPastesBracketedClipboard() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() -> std::optional<std::string> { return std::string("printf 'hi'\n"); });

  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(
             shell, SDLK_V, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
         "Ctrl+Shift+V should be handled by the terminal");
  Expect(TerminalSessionTestAccess::SentBytes(session) ==
             "\x1b[200~printf 'hi'\r\x1b[201~",
         "Ctrl+Shift+V should paste clipboard text using bracketed paste mode");
}

void TestWorkspaceShellShiftInsertPastesRawClipboard() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() -> std::optional<std::string> { return std::string("git status\n"); });

  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_INSERT, SDL_KMOD_SHIFT),
         "Shift+Insert should be handled by the terminal");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "git status\r",
         "Shift+Insert should paste raw clipboard text when bracketed paste mode is disabled");
}

void TestWorkspaceShellCtrlVStillSendsControlV() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_V, SDL_KMOD_CTRL),
         "Ctrl+V should still be handled by the terminal");
  Expect(TerminalSessionTestAccess::SentBytes(session) == std::string(1, '\x16'),
         "Ctrl+V should still send the literal control-V byte");
}

void TestWorkspaceShellArrowKeysHonorApplicationCursorMode() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1h");

  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_UP, SDL_KMOD_NONE),
         "Up should be handled by the terminal");
  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE),
         "Home should be handled by the terminal");
  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_END, SDL_KMOD_NONE),
         "End should be handled by the terminal");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1bOA\x1bOH\x1bOF",
         "workspace terminal navigation should switch to SS3 sequences in application cursor mode");
}

void TestWorkspaceShellTerminalTabsReflectOscTitles() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetLaunchLabel(session, "bash");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]2;server logs\x07");

  Expect(WorkspaceShellTestAccess::TerminalLaunchLabels(shell) ==
             std::vector<std::string>{"server logs"},
         "workspace terminal tabs should reflect OSC title updates from the terminal");
}

void TestWorkspaceShellTerminalOsc52CopiesToClipboard() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  // Default: OSC 52 clipboard writes are refused (silent-poisoning guard). The
  // pending text is still drained, but the clipboard writer is never invoked.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]52;c;Y29waWVkIGZyb20gdGVybQ==\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(clipboard_text.empty(),
         "OSC 52 must not write the system clipboard by default (poisoning guard)");
  // A blocked write must not be silent: surface a hint.
  Expect(WorkspaceShellTestAccess::ActiveNotifications(shell).size() == 1,
         "a blocked OSC 52 write should surface a hint");

  // Every blocked write notifies (the toast service bounds/expires them).
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]52;c;Y29waWVkIGZyb20gdGVybQ==\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(clipboard_text.empty(),
         "OSC 52 stays blocked before opt-in");
  Expect(WorkspaceShellTestAccess::ActiveNotifications(shell).size() == 2,
         "each blocked OSC 52 write should surface its own hint");

  // Opt-in: enabling the setting routes OSC 52 text to the clipboard writer.
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "set-setting terminal.osc52_clipboard_write true"),
         "the OSC 52 opt-in setting should flip");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]52;c;Y29waWVkIGZyb20gdGVybQ==\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(clipboard_text == "copied from term",
         "with the opt-in enabled, OSC 52 clipboard text reaches the clipboard writer");
}

void TestWorkspaceShellTerminalFocusModeTracksPanelFocus() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1004h");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[I",
         "focused terminal tabs should receive an initial focus-in notification when focus mode is enabled");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_CTRL),
         "Ctrl+Tab should move focus away from the terminal panel");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_CTRL),
         "Ctrl+Tab should keep cycling focus targets");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_CTRL),
         "Ctrl+Tab should return focus to the terminal panel");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[I\x1b[O\x1b[I",
         "terminal focus mode should emit focus-out and focus-in notifications as panel focus changes");
}

void TestWorkspaceShellTerminalFocusModeTracksWindowFocus() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1004h");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);

  Expect(SendWindowFocus(shell, false),
         "window focus loss should be handled");
  Expect(SendWindowFocus(shell, true),
         "window focus gain should be handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[I\x1b[O\x1b[I",
         "terminal focus mode should emit focus notifications when the IDE window focus changes");
}

void TestWorkspaceShellFocusedTerminalParticipatesInCaretBlinking() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);

  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "terminal caret fixture should focus the panel");
  Expect(WorkspaceShellTestAccess::ShouldBlinkCaret(shell),
         "focused terminal panels should participate in shared caret blinking");

  WorkspaceShellTestAccess::ResetCaretBlink(shell);
  Expect(WorkspaceShellTestAccess::CaretVisibleNow(shell),
         "focused terminal panels should show the caret immediately after a blink reset");
}

void TestWorkspaceShellCaretBlinkFreezesAfterIdleStop() {
  // Power-saving contract: after ~8s of caret-blink-resetting inactivity, the
  // shell stops emitting blink wake-ups and freezes the caret visible. The
  // next ResetCaretBlink() resumes animation.
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);

  WorkspaceShellTestAccess::ResetCaretBlink(shell);
  Expect(WorkspaceShellTestAccess::CaretBlinkAnimating(shell),
         "blink should be animating right after a reset");
  Expect(WorkspaceShellTestAccess::NextCaretBlinkDelayMs(shell).has_value(),
         "blink wakes should be scheduled right after a reset");

  // Simulate the idle-stop threshold elapsing.
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks() - 8000);
  Expect(!WorkspaceShellTestAccess::CaretBlinkAnimating(shell),
         "blink should freeze after the idle-stop threshold");
  Expect(WorkspaceShellTestAccess::CaretVisibleNow(shell),
         "caret should freeze in its visible phase after idle-stop");
  Expect(!WorkspaceShellTestAccess::NextCaretBlinkDelayMs(shell).has_value(),
         "no blink wakes should be scheduled while frozen");

  // Resume: a fresh reset should re-arm blink wakes immediately.
  WorkspaceShellTestAccess::ResetCaretBlink(shell);
  Expect(WorkspaceShellTestAccess::CaretBlinkAnimating(shell),
         "blink should resume after reset");
  Expect(WorkspaceShellTestAccess::NextCaretBlinkDelayMs(shell).has_value(),
         "blink wakes should resume after reset");
}

void TestWorkspaceShellScheduledWakeSkipsCaretAfterIdleStop() {
  // The wake controller must not emit a caret-rect partial-redraw after the
  // blink has frozen. Before idle-stop it should still emit one; after, it
  // should be a no-op.
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  WorkspaceShellTestAccess::ResetCaretBlink(shell);
  const auto active = WorkspaceShellTestAccess::HandleScheduledWake(shell);
  Expect(active.handled && !active.redraw.full && active.redraw.SingleRectIfOnlyOne().has_value(),
         "scheduled wake during active blink should emit a caret-rect partial redraw");

  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks() - 8000);
  const auto frozen = WorkspaceShellTestAccess::HandleScheduledWake(shell);
  Expect(!frozen.handled,
         "scheduled wake after blink freeze should not request any redraw");
}

void TestWorkspaceShellTerminalCaretDirtyRectTracksVisibleCursor() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "prompt");

  const std::optional<SDL_FRect> caret_rect = shell.CurrentCaretDirtyRect();
  Expect(caret_rect.has_value(),
         "focused terminal panels should expose a caret dirty rect for partial redraws");
  Expect(caret_rect->w > 0.0f && caret_rect->h > 0.0f,
         "terminal caret dirty rects should have a visible size");
}

void TestWorkspaceShellTerminalKeysReturnPartialPanelInvalidation() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_UP;
  const auto result = shell.HandleEvent(event);
  const SDL_FRect panel_content = WorkspaceShellTestAccess::BottomPanelContentRect(shell);
  const auto redraw_rect = result.redraw.SingleRectIfOnlyOne();

  Expect(result.handled, "terminal navigation keys should be handled");
  Expect(!result.redraw.full && redraw_rect.has_value(),
         "terminal navigation should request a partial redraw");
  Expect(RectsIntersect(*redraw_rect, panel_content),
         "terminal key redraws should include the terminal content area");
  Expect(redraw_rect->y >= panel_content.y,
         "terminal key redraws should avoid repainting the terminal tab strip");
}

void TestWorkspaceShellTerminalAsciiPromptMatchesDirectStringRendering() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#else
  EnsureDummySdlVideo();
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetCursorVisible(session, false);

  const std::string prompt = "user@host ~/projects/example-app";
  TerminalSessionTestAccess::AppendOutput(session, prompt);

  SoftwareCanvas shell_canvas(1280, 720);
  shell.Render(shell_canvas.renderer(), 1280, 720);

  const SDL_FPoint text_origin = WorkspaceShellTestAccess::BottomPanelTextOrigin(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  const float line_height = WorkspaceShellTestAccess::TextLineHeight(shell);

  SoftwareCanvas reference_canvas(1280, 720);
  microide::render::TextRenderer reference_renderer;
  reference_renderer.EnsureInitialized(reference_canvas.renderer());
  Expect(reference_renderer.BackendName() == "sdl3_ttf",
         "terminal renderer regression test should exercise the real font backend");
  const float text_x = text_origin.x;
  const float text_y = text_origin.y;

  const microide::render::Theme theme = microide::render::MakeDefaultTheme();
  SDL_SetRenderDrawColor(reference_canvas.renderer(), theme.surface_background.r,
                         theme.surface_background.g, theme.surface_background.b,
                         theme.surface_background.a);
  Expect(SDL_RenderClear(reference_canvas.renderer()),
         "terminal renderer regression test should clear the reference canvas");
  for (std::size_t column = 0; column < prompt.size(); ++column) {
    const float cell_x = text_x + static_cast<float>(column) * char_width;
    SDL_SetRenderDrawColor(reference_canvas.renderer(), theme.surface_background.r,
                           theme.surface_background.g, theme.surface_background.b,
                           theme.surface_background.a);
    const SDL_FRect cell_rect = SDL_FRect{cell_x, text_y - 1.0f, char_width, line_height};
    Expect(SDL_RenderFillRect(reference_canvas.renderer(), &cell_rect),
           "terminal renderer regression test should paint legacy cell backgrounds");
    if (prompt[column] != ' ') {
      reference_renderer.DrawString(reference_canvas.renderer(), cell_x, text_y, theme.text_primary,
                                    std::string_view(prompt.data() + column, 1));
    }
  }

  SDL_Surface* actual_pixels = SDL_RenderReadPixels(shell_canvas.renderer(), nullptr);
  SDL_Surface* legacy_pixels = SDL_RenderReadPixels(reference_canvas.renderer(), nullptr);

  const auto column_band = [&](std::size_t column, std::size_t span = 1) {
    return SDL_Rect{
        .x = std::max(0, static_cast<int>(std::floor(text_x + static_cast<float>(column) * char_width - 1.0f))),
        .y = std::max(0, static_cast<int>(std::floor(text_y - 1.0f))),
        .w = std::max(1, static_cast<int>(std::ceil(static_cast<float>(span) * char_width + 2.0f))),
        .h = std::max(1, static_cast<int>(std::ceil(line_height + 1.0f))),
    };
  };
  const int actual_pablo = CountNonBackgroundPixels(actual_pixels, column_band(2, 3),
                                                    theme.surface_background);
  const int legacy_pablo = CountNonBackgroundPixels(legacy_pixels, column_band(2, 3),
                                                    theme.surface_background);
  const std::size_t space_column = prompt.find(' ');
  Expect(space_column != std::string::npos,
         "terminal renderer regression test should include a space cell");
  const int space_x = static_cast<int>(std::floor(
      text_x + static_cast<float>(space_column) * char_width + char_width * 0.5f));
  const int space_y = static_cast<int>(std::floor(text_y + line_height * 0.5f));
  const SDL_Color actual_space = ReadSurfacePixelOrThrow(actual_pixels, space_x, space_y);

  Expect(actual_pablo >= legacy_pablo,
         "terminal ASCII rendering should not regress the early prompt glyphs");
  Expect(actual_space.r == theme.surface_background.r &&
             actual_space.g == theme.surface_background.g &&
             actual_space.b == theme.surface_background.b &&
             actual_space.a == theme.surface_background.a,
         "terminal ASCII rendering should keep blank prompt cells on the terminal background color");

  if (actual_pixels != nullptr) {
    SDL_DestroySurface(actual_pixels);
  }
  if (legacy_pixels != nullptr) {
    SDL_DestroySurface(legacy_pixels);
  }
#endif
}

void TestWorkspaceShellTerminalCaretBlinkRetainedRedrawPreservesGlyphEdges() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#else
  EnsureDummySdlVideo();

  static constexpr int kCanvasWidth = 1280;
  static constexpr int kCanvasHeight = 720;
  const std::string prompt = "user@host";
  const std::size_t cursor_column = prompt.find('@');
  Expect(cursor_column != std::string::npos,
         "caret redraw regression test should target a visible prompt glyph");

  const auto configure_shell = [&](WorkspaceShell& shell) {
    WorkspaceShellTestAccess::EnsureTerminalTab(shell);
    WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
    auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
    TerminalSessionTestAccess::Reset(session, 24, 80);
    TerminalSessionTestAccess::SetCursorVisible(session, true);
    TerminalSessionTestAccess::AppendOutput(session, prompt);
    TerminalSessionTestAccess::SetCursorPosition(session, 0, cursor_column);
  };

  WorkspaceShell retained_shell;
  configure_shell(retained_shell);
  WorkspaceShellTestAccess::ResetCaretBlink(retained_shell);
  Expect(WorkspaceShellTestAccess::CaretVisibleNow(retained_shell),
         "caret redraw regression test should begin from the visible blink phase");

  SoftwareCanvas retained_canvas(kCanvasWidth, kCanvasHeight);
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  const std::optional<SDL_FRect> caret_rect =
      retained_shell.CurrentCaretDirtyRect();
  Expect(caret_rect.has_value(),
         "caret redraw regression test should have a terminal caret dirty rect");

  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(retained_shell, SDL_GetTicks() - 530);
  Expect(!WorkspaceShellTestAccess::CaretVisibleNow(retained_shell),
         "caret redraw regression test should switch to the hidden blink phase");
  const SDL_Rect clip_rect = InflateClipRect(
      *caret_rect, retained_shell.PartialRedrawClipPadding(), kCanvasWidth, kCanvasHeight);
  Expect(clip_rect.w > 0 && clip_rect.h > 0,
         "caret redraw regression test should compute a non-empty partial clip");
  Expect(SDL_SetRenderClipRect(retained_canvas.renderer(), &clip_rect),
         "caret redraw regression test should set a retained-scene clip rect");
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);
  Expect(SDL_SetRenderClipRect(retained_canvas.renderer(), nullptr),
         "caret redraw regression test should clear the retained-scene clip rect");

  WorkspaceShell reference_shell;
  configure_shell(reference_shell);
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(reference_shell, SDL_GetTicks() - 530);
  Expect(!WorkspaceShellTestAccess::CaretVisibleNow(reference_shell),
         "reference render should use the same hidden blink phase");

  SoftwareCanvas reference_canvas(kCanvasWidth, kCanvasHeight);
  reference_shell.Render(reference_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  SDL_Surface* retained_pixels = SDL_RenderReadPixels(retained_canvas.renderer(), nullptr);
  SDL_Surface* reference_pixels = SDL_RenderReadPixels(reference_canvas.renderer(), nullptr);
  const std::size_t pixel_differences =
      CountPixelDifferences(retained_pixels, reference_pixels);

  if (retained_pixels != nullptr) {
    SDL_DestroySurface(retained_pixels);
  }
  if (reference_pixels != nullptr) {
    SDL_DestroySurface(reference_pixels);
  }

  Expect(pixel_differences == 0,
         "retained caret redraws should match a full redraw after the terminal cursor blink toggles");
#endif
}

void TestWorkspaceShellTypingReenablesTerminalTailFollow() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, false);
  WorkspaceShellTestAccess::SetActiveTerminalScrollRow(shell, 0);

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "x"),
         "terminal text input should be handled while scrolled away from the tail");
  Expect(WorkspaceShellTestAccess::ActiveTerminalFollowTail(shell),
         "typing into the terminal should resume tail-follow mode");
}

void TestWorkspaceShellHandleEventPassesEscapeToTerminal() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "top-level key handling should deliver Escape to the terminal");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b",
         "Escape should reach terminal apps instead of being dropped early");
}

void TestWorkspaceShellCopyLastTerminalCommandIncludesOutput() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  TerminalSessionTestAccess::AppendOutput(session, "user@host:~/repo$ ");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "ll"),
         "terminal text input should be handled");
  TerminalSessionTestAccess::AppendOutput(session, "ll");
  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should submit the terminal command");
  TerminalSessionTestAccess::AppendOutput(
      session, "\nfile-a.txt\nfile-b.txt\nuser@host:~/repo$ ");

  Expect(WorkspaceShellTestAccess::ExecuteCopyLastTerminalCommand(shell),
         "copy last terminal command should execute");
  Expect(clipboard_text ==
             "user@host:~/repo$ ll\nfile-a.txt\nfile-b.txt",
         "copy last terminal command should include the submitted command and rendered output");
}

// TD-2026-07-17A-068: pending_input (host-side copy-last-command capture, never sent to
// the PTY) must be bounded so repeated pastes/programmatic input before a newline cannot
// grow it without limit. Once the budget is hit further bytes are dropped and the
// truncation flag is set; command execution is unaffected.
void TestWorkspaceShellTerminalPendingInputIsCapped() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  const std::size_t cap = microide::workspace::TerminalTabState::kMaxPendingInputBytes;
  // Multi-byte UTF-8 so a naive byte-cut could split a codepoint; the cap must back off
  // to a codepoint boundary.
  std::string chunk;
  for (int i = 0; i < 4096; ++i) {
    chunk += "\xC3\xA9";  // 'é'
  }
  // Push well past the cap across many appends (simulating repeated pastes).
  for (int i = 0; i < 400; ++i) {
    WorkspaceShellTestAccess::AppendTerminalPendingInput(shell, chunk);
  }
  const std::size_t size = WorkspaceShellTestAccess::ActiveTerminalPendingInputSize(shell);
  Expect(size <= cap, "pending_input must never exceed the capture budget");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPendingInputTruncated(shell),
         "hitting the pending-input budget must set the truncated flag");
  Expect(size % 2 == 0,
         "the truncation must land on a UTF-8 codepoint boundary (2-byte 'é' units)");

  // Submitting clears the capture and the flag so the next command starts clean.
  WorkspaceShellTestAccess::SubmitTerminalPendingInput(shell);
  Expect(WorkspaceShellTestAccess::ActiveTerminalPendingInputSize(shell) == 0,
         "submit clears the pending-input capture");
  Expect(!WorkspaceShellTestAccess::ActiveTerminalPendingInputTruncated(shell),
         "submit resets the truncation flag");
}

// Copy Last Command enablement uses the cheap HasLastTerminalCommand() predicate, not the
// full transcript build (TD-2026-07-17A-065). Disabled before any command, enabled after —
// and the predicate must agree with the expensive builder returning a value.
void TestWorkspaceShellCopyLastTerminalCommandEnablementUsesCheapPredicate() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  Expect(!WorkspaceShellTestAccess::IsActionEnabled(
             shell, WorkspaceShell::ActionId::CopyLastTerminalCommand),
         "Copy Last Command is disabled before any command has run");

  TerminalSessionTestAccess::AppendOutput(session, "user@host:~/repo$ ");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "ll"),
         "terminal text input should be handled");
  TerminalSessionTestAccess::AppendOutput(session, "ll");
  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should submit the terminal command");
  TerminalSessionTestAccess::AppendOutput(session, "\nout\nuser@host:~/repo$ ");

  Expect(WorkspaceShellTestAccess::IsActionEnabled(
             shell, WorkspaceShell::ActionId::CopyLastTerminalCommand),
         "Copy Last Command is enabled once a command has run");
}

void TestWorkspaceShellCopyLastTerminalCommandFallsBackDuringAlternateScreen() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  TerminalSessionTestAccess::AppendOutput(session, "user@host:~/repo$ ");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "vim"),
         "terminal text input should be handled");
  TerminalSessionTestAccess::AppendOutput(session, "vim");
  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should submit the terminal command");
  TerminalSessionTestAccess::AppendOutput(session, "\n\x1b[?1049hvim screen");

  Expect(WorkspaceShellTestAccess::ExecuteCopyLastTerminalCommand(shell),
         "copy last terminal command should execute during alternate screen use");
  Expect(clipboard_text == "user@host:~/repo$ vim",
         "alternate-screen apps should fall back to copying only the invoked command");
}

void TestWorkspaceShellCopyLastTerminalCommandIgnoresPrecedingFullWidthOutput() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 4);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  TerminalSessionTestAccess::AppendOutput(session, "ABCD\n$ ");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "ls"),
         "terminal text input should be handled");
  TerminalSessionTestAccess::AppendOutput(session, "ls");
  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should submit the terminal command");
  TerminalSessionTestAccess::AppendOutput(session, "\nok\n$ ");

  Expect(WorkspaceShellTestAccess::ExecuteCopyLastTerminalCommand(shell),
         "copy last terminal command should execute after a full-width output row");
  Expect(clipboard_text == "$ ls\nok",
         "copy last terminal command should not pull a preceding full-width output row into the invocation");
}

void TestWorkspaceShellCopyLastTerminalCommandPreservesSoftWrappedInvocation() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 4);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  TerminalSessionTestAccess::AppendOutput(session, "$ ");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "long"),
         "terminal text input should be handled");
  TerminalSessionTestAccess::AppendOutput(session, "long");
  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should submit the terminal command");
  TerminalSessionTestAccess::AppendOutput(session, "\nok\n$ ");

  Expect(WorkspaceShellTestAccess::ExecuteCopyLastTerminalCommand(shell),
         "copy last terminal command should execute after a soft-wrapped invocation");
  Expect(clipboard_text == "$ lo\nng\nok",
         "copy last terminal command should preserve soft-wrapped invocation rows in the copied transcript");
}

void TestWorkspaceShellTerminalTabRightClickOpensContextMenu() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::ActiveTerminalTabRect(shell);
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_RIGHT;
  event.button.x = static_cast<float>(tab_rect.x + tab_rect.w * 0.5f);
  event.button.y = static_cast<float>(tab_rect.y + tab_rect.h * 0.5f);

  Expect(shell.HandleEvent(event).handled, "right-clicking a terminal tab should be handled");
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell),
         "right-clicking a terminal tab should open a popup menu");
  Expect(WorkspaceShellTestAccess::TerminalTabContextMenuOpen(shell),
         "right-clicking a terminal tab should open the terminal tab context menu");
}

void TestWorkspaceShellTerminalTabsStartAtPanelEdge() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect tab_rect = WorkspaceShellTestAccess::ActiveTerminalTabRect(shell);
  Expect(std::fabs(tab_rect.x - layout.bottom_panel.x) <= 0.01f,
         "terminal tab strip should not keep extra left padding before the first tab");
}

void TestWorkspaceShellTerminalPanelRightClickOpensContextMenu() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect panel_rect = WorkspaceShellTestAccess::BottomPanelContentRect(shell);
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_RIGHT;
  event.button.x = static_cast<float>(panel_rect.x + 12.0f);
  event.button.y = static_cast<float>(panel_rect.y + 12.0f);

  Expect(shell.HandleEvent(event).handled, "right-clicking the terminal panel should be handled");
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell),
         "right-clicking the terminal panel should open a popup menu");
  Expect(WorkspaceShellTestAccess::TerminalContextMenuOpen(shell),
         "right-clicking the terminal panel should open the terminal context menu");
}

void TestWorkspaceShellTerminalPasteActionTargetsPanelFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "main.txt";
  WriteFile(file, "editor\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() -> std::optional<std::string> { return std::string("pwd"); });

  const std::vector<std::string> editor_before =
      WorkspaceShellTestAccess::ActiveEditor(shell).lines().Snapshot();
  Expect(WorkspaceShellTestAccess::ExecutePasteClipboard(shell),
         "paste should execute while the terminal owns focus");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "pwd",
         "terminal-focused paste should send clipboard text to the terminal");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines().Snapshot() == editor_before,
         "terminal-focused paste should not modify the editor buffer");
}

void TestWorkspaceShellTerminalPasteActionCapsClipboardBytes() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  constexpr std::size_t kMaxPasteBytes = 64u << 20;
  WorkspaceShellTestAccess::SetClipboardTextReader(shell, []() -> std::optional<std::string> {
    return std::string(kMaxPasteBytes + 4096, 'x');
  });

  Expect(WorkspaceShellTestAccess::ExecutePasteClipboard(shell),
         "terminal paste action should accept a large clipboard");
  const std::string sent = TerminalSessionTestAccess::SentBytes(session);
  Expect(sent.size() <= kMaxPasteBytes && sent.size() >= kMaxPasteBytes - 4,
         "terminal paste action should keep sent bytes within the clipboard cap, sent " +
             std::to_string(sent.size()) + " bytes");
  Expect(!sent.empty() && sent.front() == 'x' && sent.back() == 'x',
         "terminal paste cap should preserve the retained payload bytes");
}

// Regression: the cap lives in TerminalSession::PasteText itself, so the direct
// paste path (middle-click paste calls session.PasteText without the workspace
// clamp) is also bounded — a huge clipboard cannot allocate a huge formatted
// buffer and block the backend write.
void TestWorkspaceShellTerminalDirectPasteIsCapped() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  constexpr std::size_t kMaxPasteBytes = 64u << 20;
  session.PasteText(std::string(kMaxPasteBytes + 4096, 'x'));
  const std::string sent = TerminalSessionTestAccess::SentBytes(session);
  Expect(sent.size() <= kMaxPasteBytes && sent.size() >= kMaxPasteBytes - 4,
         "direct PasteText should cap sent bytes, sent " + std::to_string(sent.size()));
}

void TestWorkspaceShellTerminalTabsDragReorderToStart() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 24, 80);
  TerminalSessionTestAccess::SetLaunchLabel(WorkspaceShellTestAccess::ActiveTerminalSession(shell),
                                            "one");
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 24, 80);
  TerminalSessionTestAccess::SetLaunchLabel(WorkspaceShellTestAccess::ActiveTerminalSession(shell),
                                            "two");
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 24, 80);
  TerminalSessionTestAccess::SetLaunchLabel(WorkspaceShellTestAccess::ActiveTerminalSession(shell),
                                            "three");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::TerminalTabRect(shell, 2);
  Expect(SendMouseDown(
             shell, source_rect.x + source_rect.w * 0.5f, source_rect.y + source_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "dragging should start from a terminal tab press");

  const SDL_FRect first_rect = WorkspaceShellTestAccess::TerminalTabRect(shell, 0);
  const float drop_x = first_rect.x + 1.0f;
  const float drop_y = first_rect.y + first_rect.h * 0.5f;
  Expect(SendMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK),
         "dragging across terminal tabs should be handled");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT),
         "releasing a dragged terminal tab should be handled");

  Expect(WorkspaceShellTestAccess::TerminalLaunchLabels(shell) ==
             std::vector<std::string>{"three", "one", "two"},
         "dragging a terminal tab to the start should reorder the terminal strip");
  Expect(WorkspaceShellTestAccess::ActiveTerminalTabIndex(shell) == 0,
         "dragged terminal tab should stay active after reordering");
}

void TestWorkspaceShellTerminalTabsOverflowReachableViaHeaderWheel() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  for (int i = 0; i < 8; ++i) {
    WorkspaceShellTestAccess::AddTerminalTab(shell);
    TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 24, 80);
    // `+=`, not chained `operator+`: see ThemeTests' palette-parity helper for
    // the GCC 13 -Warray-bounds false positive the concatenation form triggers.
    std::string launch_label = "terminal-session-";
    launch_label += std::to_string(i);
    TerminalSessionTestAccess::SetLaunchLabel(
        WorkspaceShellTestAccess::ActiveTerminalSession(shell), std::move(launch_label));
  }
  // Narrow window so the eight terminal tabs cannot all fit in the strip.
  WorkspaceShellTestAccess::SetWindowSize(shell, 520, 600);

  const auto initial_titles = WorkspaceShellTestAccess::BottomPanelTabDisplayTitles(shell);
  Expect(initial_titles.size() < 8,
         "a narrow window should leave some terminal tabs hidden (overflow)");
  Expect(WorkspaceShellTestAccess::BottomPanelTabScrollIndex(shell) == 0,
         "the bottom-panel tab strip starts unscrolled");

  const SDL_FRect header = WorkspaceShellTestAccess::BottomPanelHeaderRect(shell);
  Expect(SendMouseWheel(shell, header.x + header.w * 0.5f, header.y + header.h * 0.5f, -2),
         "a wheel over the bottom-panel header should be handled");
  Expect(WorkspaceShellTestAccess::BottomPanelTabScrollIndex(shell) > 0,
         "wheeling over the header should scroll the bottom-panel tab strip into the overflow");
  const auto scrolled_titles = WorkspaceShellTestAccess::BottomPanelTabDisplayTitles(shell);
  Expect(scrolled_titles != initial_titles,
         "scrolling the bottom-panel strip should reveal previously hidden terminal tabs");
}

void TestWorkspaceShellBottomPanelWheelScrollsTranscript() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  std::string transcript;
  for (int i = 0; i < 40; ++i) {
    transcript += "line " + std::to_string(i) + "\n";
  }
  TerminalSessionTestAccess::AppendOutput(session, transcript);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, false);
  WorkspaceShellTestAccess::SetActiveTerminalScrollRow(shell, 0);

  WorkspaceShellTestAccess::SetFocusEditor(shell);
  const SDL_FRect panel_rect = WorkspaceShellTestAccess::BottomPanelContentRect(shell);
  Expect(SendMouseWheel(
             shell, panel_rect.x + 12.0f, panel_rect.y + 12.0f, -3),
         "mouse wheel over the bottom panel should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell) ==
             3 * microide::workspace::kWheelScrollRows,
         "mouse wheel over the bottom panel should advance the transcript by the shared "
         "per-tick row step");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "scrolling the panel must not pull keyboard focus out of the editor");
}

void TestWorkspaceShellTerminalDragSelectsTranscriptText() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "select me");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FPoint start = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  const SDL_FPoint end = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 6);
  Expect(SendMouseDown(shell, start.x, start.y,
                                                         SDL_BUTTON_LEFT),
         "pressing inside the terminal panel should start transcript selection");
  Expect(SendMouseMotion(shell, end.x, end.y, SDL_BUTTON_LMASK),
         "dragging inside the terminal panel should update transcript selection");
  Expect(SendMouseUp(shell, end.x, end.y,
                                                       SDL_BUTTON_LEFT),
         "releasing inside the terminal panel should end transcript selection");

  Expect(WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "dragging across terminal cells should create a selection");
  Expect(WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell) == "select",
         "terminal drag selection should capture the selected transcript text");
}

// TD-2026-08-13-205: the terminal was the fourth text surface to drag a selection
// and the only one that both refused to track a pointer held outside its panel and
// never autoscrolled. Holding past the bottom edge must keep scrolling the
// transcript on the idle wake, exactly as the editor / compare / merge panes do.
void TestWorkspaceShellTerminalDragAutoscrollsPastTheEdge() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  for (int i = 0; i < 300; ++i) {
    TerminalSessionTestAccess::AppendOutput(session, "line " + std::to_string(i) + "\r\n");
  }
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, false);
  WorkspaceShellTestAccess::SetActiveTerminalScrollRow(shell, 0);

  const SDL_FPoint start = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  const SDL_FRect panel_rect = WorkspaceShellTestAccess::BottomPanelContentRect(shell);
  Expect(SendMouseDown(shell, start.x, start.y, SDL_BUTTON_LEFT),
         "pressing inside the terminal panel should start transcript selection");

  // Well below the panel: the old handler refused this outright.
  const float below_y = panel_rect.y + panel_rect.h + 200.0f;
  Expect(SendMouseMotion(shell, start.x, below_y, SDL_BUTTON_LMASK),
         "dragging below the terminal panel should still be handled");

  const int scroll_after_motion = WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell);
  for (int tick = 0; tick < 5; ++tick) {
    const auto result = WorkspaceShellTestAccess::HandleScheduledWake(shell);
    Expect(result.handled, "an armed terminal autoscroll should keep the wake busy");
  }
  Expect(WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell) > scroll_after_motion,
         "holding the pointer past the bottom of the terminal must keep scrolling");

  Expect(SendMouseUp(shell, start.x, below_y, SDL_BUTTON_LEFT), "release should be handled");
  const int scroll_after_release = WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell);
  for (int tick = 0; tick < 3; ++tick) {
    (void)WorkspaceShellTestAccess::HandleScheduledWake(shell);
  }
  Expect(WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell) == scroll_after_release,
         "releasing the button disarms the autoscroll");
}

// Double-click selects the word under the pointer and triple-click the whole row, the
// same gestures the editor surface answers. The terminal used to offer drag-select
// only, so the two text surfaces in one window disagreed about what a click means.
void TestWorkspaceShellTerminalDoubleClickSelectsWordTripleClickSelectsLine() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "warn src/foo/bar.cpp:42 done");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  // Column 8 is inside "src/foo/bar.cpp:42" — the path separators and the ':' line
  // suffix are word characters, so the whole reference comes out as one token.
  const SDL_FPoint inside_path = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 8);
  Expect(SendMouseDown(shell, inside_path.x, inside_path.y, SDL_BUTTON_LEFT, 2),
         "double-clicking inside the terminal should be handled");
  Expect(WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "double-clicking a word should create a selection");
  Expect(WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell) == "src/foo/bar.cpp:42",
         "double-click should select the whole path token under the pointer");

  Expect(SendMouseDown(shell, inside_path.x, inside_path.y, SDL_BUTTON_LEFT, 3),
         "triple-clicking inside the terminal should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell) ==
             "warn src/foo/bar.cpp:42 done",
         "triple-click should select the whole row without its trailing blanks");

  // A double-click on blank space leaves the empty caret-style selection alone rather
  // than selecting the run of spaces.
  const SDL_FPoint blank = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 60);
  Expect(SendMouseDown(shell, blank.x, blank.y, SDL_BUTTON_LEFT, 2),
         "double-clicking blank terminal space should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell).empty(),
         "double-clicking blank space should not select anything");
}

void TestWorkspaceShellTerminalSelectionWritesPrimaryBufferAndMiddleClickPastes() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "select me");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string primary_selection;
  WorkspaceShellTestAccess::SetPrimarySelectionTextWriter(
      shell, [&](std::string_view text) {
        primary_selection = std::string(text);
        return true;
      });
  WorkspaceShellTestAccess::SetPrimarySelectionTextReader(
      shell, [&]() -> std::optional<std::string> { return primary_selection; });

  const SDL_FPoint start = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  const SDL_FPoint end = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 6);
  Expect(SendMouseDown(shell, start.x, start.y,
                                                         SDL_BUTTON_LEFT),
         "pressing inside the terminal panel should start transcript selection");
  Expect(SendMouseMotion(shell, end.x, end.y, SDL_BUTTON_LMASK),
         "dragging inside the terminal panel should update transcript selection");
  Expect(SendMouseUp(shell, end.x, end.y,
                                                       SDL_BUTTON_LEFT),
         "releasing inside the terminal panel should end transcript selection");
  Expect(primary_selection == "select",
         "terminal drag selection should update the primary selection buffer");

  const SDL_FPoint paste_point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 8);
  Expect(SendMouseDown(shell, paste_point.x, paste_point.y,
                                                         SDL_BUTTON_MIDDLE),
         "middle-clicking the terminal panel should be handled");
  Expect(TerminalSessionTestAccess::SentBytes(session).find("select") != std::string::npos,
         "middle-clicking the terminal panel should paste the primary selection");
}

void TestWorkspaceShellTerminalLeftClickOpensUrls() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "visit https://example.com/path?a=1");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string opened_url;
  WorkspaceShellTestAccess::SetExternalUrlOpener(
      shell, [&](std::string_view url) {
        opened_url = std::string(url);
        return true;
      });

  const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 10);
  Expect(SendMouseDown(shell, point.x, point.y,
                                                         SDL_BUTTON_LEFT),
         "left-clicking a terminal URL should be handled");
  Expect(opened_url == "https://example.com/path?a=1",
         "left-clicking a terminal URL should open the detected link target");
  Expect(!WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "opening a terminal URL should not start a text selection");
}

// Regression: URL hit-testing maps the clicked grid column to a byte offset in the
// line text before matching, so a multibyte glyph before the URL no longer desyncs
// the detection. Previously TerminalUrlAtColumn compared a grid column directly
// against a byte offset, so an accented/wide glyph ahead of the link shifted the
// match and a click on the URL start missed.
void TestWorkspaceShellTerminalUrlHitTestHonorsMultibytePrefix() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  // "é" is one column but two UTF-8 bytes, so the URL's byte offset (3) is one
  // greater than its grid column (2). Clicking the URL's first grid column must
  // still resolve the link.
  TerminalSessionTestAccess::AppendOutput(session, "\xc3\xa9 https://example.com/x");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string opened_url;
  WorkspaceShellTestAccess::SetExternalUrlOpener(shell, [&](std::string_view url) {
    opened_url = std::string(url);
    return true;
  });

  // Grid column 2 is the 'h' of the URL (é at 0, space at 1).
  const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 2);
  Expect(SendMouseDown(shell, point.x, point.y, SDL_BUTTON_LEFT),
         "left-clicking a terminal URL after a multibyte glyph should be handled");
  Expect(opened_url == "https://example.com/x",
         "URL hit-testing must map the grid column to a byte offset before matching");
}

// A closing bracket at the end of a URL is prose punctuation only when the URL has
// no opener for it: a Wikipedia-style `.../Foo_(bar)` keeps its `)`, while the `)`
// that closes a parenthesised link and a trailing `.` still come off (VS Code).
void TestWorkspaceShellTerminalUrlKeepsBalancedClosingBracket() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(
      session, "https://en.wikipedia.org/wiki/Foo_(bar). (https://example.com/x)");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string opened_url;
  WorkspaceShellTestAccess::SetExternalUrlOpener(shell, [&](std::string_view url) {
    opened_url = std::string(url);
    return true;
  });

  const SDL_FPoint first = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  Expect(SendMouseDown(shell, first.x, first.y, SDL_BUTTON_LEFT), "clicking the first URL is handled");
  Expect(opened_url == "https://en.wikipedia.org/wiki/Foo_(bar)",
         "the balanced ) stays and the trailing . goes: " + opened_url);

  // "https://en.wikipedia.org/wiki/Foo_(bar). (" is 42 columns; the second URL starts at 42.
  const SDL_FPoint second = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 42);
  Expect(SendMouseDown(shell, second.x, second.y, SDL_BUTTON_LEFT), "clicking the second URL is handled");
  Expect(opened_url == "https://example.com/x",
         "the ) that closes the parenthesised link is stripped: " + opened_url);
}

// TD-2026-07-17A-027: hover cursor-kind resolution reuses the render frame's cached
// visible-line snapshot instead of re-snapshotting per pointer move. After a render
// populates the snapshot, hovering a URL cell must still resolve to the Pointer
// cursor, and a non-URL cell to the Text cursor.
void TestWorkspaceShellTerminalUrlHoverUsesRenderedSnapshot() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "see https://example.com/x done");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  // Render the terminal panel so the visible-line snapshot cache is populated; the
  // hover path then reuses it rather than taking a fresh session snapshot.
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);

  // Column 4 is the 'h' of the URL ("see " is columns 0-3).
  const SDL_FPoint url_point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 4);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, url_point.x, url_point.y),
         "hovering a terminal URL cell resolves to the Pointer cursor via the cached snapshot");
  // Column 0 ('s' of "see") is plain text, not a link.
  const SDL_FPoint text_point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsText(shell, text_point.x, text_point.y),
         "hovering a non-URL terminal cell resolves to the Text cursor");
}

// Regression: URL schemes are case-insensitive, so an uppercase or mixed-case
// scheme (`HTTPS://`) must still be detected. Previously the scheme match was a
// literal case-sensitive find and only lowercase schemes were recognized.
void TestWorkspaceShellTerminalUrlHitTestHonorsUppercaseScheme() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "go HTTPS://Example.COM/Path");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string opened_url;
  WorkspaceShellTestAccess::SetExternalUrlOpener(shell, [&](std::string_view url) {
    opened_url = std::string(url);
    return true;
  });

  // Grid column 3 is the 'H' of the URL ("go " is columns 0-2).
  const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 3);
  Expect(SendMouseDown(shell, point.x, point.y, SDL_BUTTON_LEFT),
         "left-clicking an uppercase-scheme terminal URL should be handled");
  // The path casing is preserved from the original text; only scheme matching
  // is case-insensitive.
  Expect(opened_url == "HTTPS://Example.COM/Path",
         "uppercase-scheme URLs must be detected with original casing preserved");
}

void TestWorkspaceShellTerminalMouseCaptureSendsButtonEvents() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetMouseTracking(session, true, false, false);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  Expect(SendMouseDown(shell, point.x, point.y,
                                                         SDL_BUTTON_LEFT),
         "mouse presses should be handled when the terminal requests mouse capture");
  Expect(!WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "mouse-captured presses should not create a transcript selection");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "mouse-captured presses should keep panel focus");

  Expect(SendMouseUp(shell, point.x, point.y,
                                                       SDL_BUTTON_LEFT),
         "mouse releases should be handled when the terminal requests mouse capture");
  Expect(!WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "mouse-captured releases should leave transcript selection disabled");
}

std::string TerminalScreenText(microide::terminal::TerminalSession& session) {
  std::string text;
  for (const auto& line : session.SnapshotLines()) {
    for (const auto& cell : line.cells) {
      text += cell.DisplayText();
    }
    text += '\n';
  }
  return text;
}

void TestWorkspaceShellTerminalExitMarkerSurvivesOpenEscape() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  // The child emits an OSC that is never terminated (no ST/BEL) and then dies,
  // leaving the parser stuck in the OSC string-payload state across the read.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]0;my title");
  TerminalSessionTestAccess::EmitProcessExitMarker(session);
  Expect(TerminalScreenText(session).find("[process exited]") != std::string::npos,
         "exit marker must display even when the child died mid-escape-sequence");
}

void TestWorkspaceShellTerminalExitMarkerDisplaysAfterCleanOutput() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "done\r\n");
  TerminalSessionTestAccess::EmitProcessExitMarker(session);
  const std::string text = TerminalScreenText(session);
  Expect(text.find("done") != std::string::npos, "prior output should remain");
  Expect(text.find("[process exited]") != std::string::npos,
         "exit marker should display in the normal (no open escape) case");
}

// An exited terminal tab must remain open (retained) after the session update
// loop so the `[process exited]` marker and prior output stay inspectable, rather
// than being reaped in the same update that records the marker. TD-2026-07-17A-130.
void TestWorkspaceShellExitedTerminalTabIsRetained() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);
  TerminalSessionTestAccess::AppendOutput(session, "hi\r\n");

  // The child process exits: session reports !running() and appends the marker.
  TerminalSessionTestAccess::EmitProcessExitMarker(session);
  TerminalSessionTestAccess::SetRunning(session, false);

  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1,
         "exited terminal tab should exist before the update loop runs");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1,
         "an exited terminal tab must be retained (not reaped) so output stays inspectable");
  Expect(TerminalScreenText(session).find("[process exited]") != std::string::npos,
         "the retained tab still shows the exit marker after the update loop");
}

// Terminal selection copy must use the same cell-to-text rules as whole-line
// copy: empty cells render as a single space (so internal spacing survives) and
// the trailing spacer of a double-width glyph is skipped (so no phantom space is
// copied). These cases assert selection extraction matches a whole-line slice of
// the same span.
microide::terminal::TerminalCell MakeAsciiCell(char c) {
  microide::terminal::TerminalCell cell;
  cell.SetAscii(c);
  return cell;
}

microide::terminal::TerminalCell MakeEmptyCell() {
  return microide::terminal::TerminalCell{};
}

void TestWorkspaceShellSelectionPreservesInternalSpaces() {
  using microide::workspace::ExtractTerminalSelectionText;
  using microide::workspace::TerminalLineSliceText;
  using microide::workspace::TerminalSelectionBounds;
  using microide::workspace::TerminalSelectionPoint;

  // "A  B": two empty cells stand in for the internal spaces.
  microide::terminal::TerminalLine line;
  line.cells = {MakeAsciiCell('A'), MakeEmptyCell(), MakeEmptyCell(), MakeAsciiCell('B')};
  const std::vector<microide::terminal::TerminalLine> lines = {line};

  const std::string whole_line =
      TerminalLineSliceText(line, 0, line.cells.size(), /*trim_trailing=*/false);
  Expect(whole_line == "A  B", "whole-line copy should render empty cells as spaces");

  const TerminalSelectionBounds selection{
      .start = TerminalSelectionPoint{.row = 0, .column = 0},
      .end = TerminalSelectionPoint{.row = 0, .column = line.cells.size()},
  };
  Expect(ExtractTerminalSelectionText(lines, selection) == whole_line,
         "selection copy should preserve internal spaces exactly like whole-line copy");
}

// Rows are stored at the full terminal width, so a multi-row selection covers each
// row's padding. A copy drops that padding (as xterm and VS Code do) but keeps blanks
// inside a soft-wrapped line and blanks that sit before a glyph.
void TestWorkspaceShellSelectionCopyDropsRowPaddingButNotWrappedBlanks() {
  using microide::workspace::ExtractTerminalSelectionText;
  using microide::workspace::TerminalSelectionBounds;
  using microide::workspace::TerminalSelectionPoint;

  const auto padded_row = [](std::string_view text, std::size_t width, bool wrapped) {
    microide::terminal::TerminalLine line;
    for (const char ch : text) line.cells.push_back(MakeAsciiCell(ch));
    while (line.cells.size() < width) line.cells.push_back(MakeAsciiCell(' '));
    line.wrapped_from_previous = wrapped;
    return line;
  };
  const std::vector<microide::terminal::TerminalLine> lines = {
      padded_row("ab", 8, false),
      padded_row("cd  ", 8, false),   // continued by the next row: its blanks are real
      padded_row("ef", 8, true),
      padded_row(" g h", 8, false),
  };
  const TerminalSelectionBounds all{
      .start = TerminalSelectionPoint{.row = 0, .column = 0},
      .end = TerminalSelectionPoint{.row = 3, .column = 8},
  };
  const std::string copied = ExtractTerminalSelectionText(lines, all);
  Expect(copied == "ab\ncd      ef\n g h",
         "row padding is dropped, wrapped blanks and leading blanks kept: [" + copied + "]");

  // A selection ending inside a row's padding is trimmed to the glyphs too.
  const TerminalSelectionBounds into_padding{
      .start = TerminalSelectionPoint{.row = 0, .column = 0},
      .end = TerminalSelectionPoint{.row = 0, .column = 6},
  };
  Expect(ExtractTerminalSelectionText(lines, into_padding) == "ab",
         "a selection that ends in the padding copies only the glyphs");
  // Blanks BEFORE a glyph within the selection are text, not padding.
  const TerminalSelectionBounds before_glyph{
      .start = TerminalSelectionPoint{.row = 3, .column = 0},
      .end = TerminalSelectionPoint{.row = 3, .column = 3},
  };
  Expect(ExtractTerminalSelectionText(lines, before_glyph) == " g ",
         "blanks followed by a glyph on the row are kept");
}

void TestWorkspaceShellSelectionSkipsWideTrailingSpacer() {
  using microide::workspace::ExtractTerminalSelectionText;
  using microide::workspace::TerminalLineSliceText;
  using microide::workspace::TerminalSelectionBounds;
  using microide::workspace::TerminalSelectionPoint;

  // A CJK wide glyph occupies a lead cell plus a trailing spacer; the spacer
  // here even carries a stray space to prove the skip is attribute-driven.
  microide::terminal::TerminalCell lead;
  lead.SetUtf8("\xE4\xB8\xAD");  // U+4E2D "中"
  microide::terminal::TerminalCell trailing;
  trailing.SetAscii(' ');
  trailing.style.set(microide::terminal::cell_attr::kWideTrailing, true);

  microide::terminal::TerminalLine line;
  line.cells = {lead, trailing, MakeAsciiCell('X')};
  const std::vector<microide::terminal::TerminalLine> lines = {line};

  const std::string whole_line =
      TerminalLineSliceText(line, 0, line.cells.size(), /*trim_trailing=*/false);
  Expect(whole_line == "\xE4\xB8\xAD"
                       "X",
         "whole-line copy should skip the wide-trailing spacer");

  const TerminalSelectionBounds selection{
      .start = TerminalSelectionPoint{.row = 0, .column = 0},
      .end = TerminalSelectionPoint{.row = 0, .column = line.cells.size()},
  };
  Expect(ExtractTerminalSelectionText(lines, selection) == whole_line,
         "selection copy should skip the wide-trailing spacer exactly like whole-line copy");
}

}  // namespace


// Ctrl+F in a focused terminal opens the find bar rather than sending ^F to the
// shell, which is what VSCode does and the only reason the bar is reachable
// without the palette.
void TestWorkspaceShellTerminalCtrlFOpensFindBar() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "alpha needle beta\r\n");

  Expect(!WorkspaceShellTestAccess::TerminalFind(shell).visible(),
         "the find bar should start hidden");
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should be handled while the terminal has focus");
  auto& find = WorkspaceShellTestAccess::TerminalFind(shell);
  Expect(find.visible() && find.focused(),
         "Ctrl+F should open the find bar and give it the keyboard");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "Ctrl+F must not also reach the shell as a control byte");
}

// Typing a query scans the scrollback, and the first result lands on the newest
// hit because terminal output is read from the bottom.
void TestWorkspaceShellTerminalFindSelectsNewestMatchFirst() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  for (int index = 0; index < 5; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle line\r\n");
  }

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should open the find bar");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "needle"),
         "typing should route into the find query, not the terminal");
  auto& find = WorkspaceShellTestAccess::TerminalFind(shell);
  Expect(find.matches().size() == 5, "every matching row should be found");
  Expect(find.selected_index() == 4, "a fresh query should select the newest hit");
  Expect(find.count_text() == "5/5", "the counter should read selected-of-total");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "the query text must never reach the PTY");
}

// Shift+Enter walks back up the history, Enter forward, and both wrap.
void TestWorkspaceShellTerminalFindNavigationWrapsAndReveals() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 4, 80);
  for (int index = 0; index < 40; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle line\r\n");
  }
  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, true);

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should open the find bar");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "needle"),
         "the query should be typed into the bar");
  auto& find = WorkspaceShellTestAccess::TerminalFind(shell);
  const std::size_t newest = find.selected_index();
  Expect(newest + 1 == find.matches().size(), "the newest hit should be selected first");

  Expect(WorkspaceShellTestAccess::HandleTerminalFindKeyDown(shell, SDLK_RETURN, SDL_KMOD_SHIFT),
         "Shift+Enter should be handled by the find bar");
  Expect(find.selected_index() == newest - 1, "Shift+Enter should step back up the history");
  Expect(!WorkspaceShellTestAccess::ActiveTerminalFollowTail(shell),
         "revealing an off-screen match should detach the terminal from its tail");

  Expect(WorkspaceShellTestAccess::HandleTerminalFindKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should be handled by the find bar");
  Expect(find.selected_index() == newest, "Enter should step back forward");
  Expect(WorkspaceShellTestAccess::HandleTerminalFindKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter past the last match should still be handled");
  Expect(find.selected_index() == 0, "stepping past the end should wrap to the first match");
}

void TestWorkspaceShellTerminalFindTogglesNarrowMatches() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "Cat concatenate cat\r\n");

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should open the find bar");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "cat"),
         "the query should be typed into the bar");
  auto& find = WorkspaceShellTestAccess::TerminalFind(shell);
  Expect(find.matches().size() == 3, "case-insensitive substring search should find all three");

  Expect(WorkspaceShellTestAccess::HandleTerminalFindKeyDown(shell, SDLK_C, SDL_KMOD_ALT),
         "Alt+C should toggle case sensitivity");
  Expect(find.case_sensitive() && find.matches().size() == 2,
         "case sensitivity should drop the capitalised hit");

  Expect(WorkspaceShellTestAccess::HandleTerminalFindKeyDown(shell, SDLK_W, SDL_KMOD_ALT),
         "Alt+W should toggle whole-word matching");
  Expect(find.whole_word() && find.matches().size() == 1,
         "whole-word matching should drop the substring hit");
}

// Escape closes the bar and hands the keyboard back, so the next keystroke drives
// the shell again.
void TestWorkspaceShellTerminalFindEscapeReturnsKeyboardToTerminal() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "needle\r\n");

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should open the find bar");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "needle"),
         "the query should be typed into the bar");
  Expect(WorkspaceShellTestAccess::HandleTerminalFindKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should be handled by the find bar");
  auto& find = WorkspaceShellTestAccess::TerminalFind(shell);
  Expect(!find.visible() && !find.focused(), "Escape should close the bar");
  Expect(find.matches().empty(), "closing should drop the retained match list");

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_A, SDL_KMOD_CTRL),
         "the terminal should own the keyboard again");
  Expect(TerminalSessionTestAccess::SentBytes(session) == std::string(1, '\x01'),
         "Ctrl+A should reach the shell as a control byte once the bar is closed");
}

// New output must extend the match set in place: the incremental rescan only
// re-walks the visible grid, so a bug there shows up as dropped or duplicated
// scrollback hits.
void TestWorkspaceShellTerminalFindTracksStreamingOutput() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 4, 80);
  for (int index = 0; index < 30; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle line\r\n");
  }

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should open the find bar");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "needle"),
         "the query should be typed into the bar");
  auto& find = WorkspaceShellTestAccess::TerminalFind(shell);
  Expect(find.matches().size() == 30, "the initial scan should see every row");

  for (int index = 0; index < 7; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle again\r\n");
  }
  WorkspaceShellTestAccess::RefreshTerminalFind(shell);
  Expect(find.matches().size() == 37, "streamed rows should extend the match set");
  for (std::size_t index = 1; index < find.matches().size(); ++index) {
    Expect(find.matches()[index - 1].row < find.matches()[index].row,
           "matches should stay row-ordered with no duplicates");
  }
}


// The bar floats over a terminal that may itself carry a selection, so the
// clipboard shortcuts have to resolve against the focused field, not the panel.
void TestWorkspaceShellTerminalFindClipboardShortcutsTargetTheQuery() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "transcript needle text\r\n");

  std::string clipboard;
  WorkspaceShellTestAccess::SetClipboardTextWriter(shell, [&](std::string_view text) {
    clipboard = std::string(text);
    return true;
  });

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should open the find bar");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "needle"),
         "the query should be typed into the bar");
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_A, SDL_KMOD_CTRL),
         "Ctrl+A should be routed to the focused find field");
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_C, SDL_KMOD_CTRL),
         "Ctrl+C should be routed to the focused find field");
  Expect(clipboard == "needle",
         "Ctrl+A then Ctrl+C in the find bar should copy the query, not the transcript");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "neither shortcut should reach the shell as a control byte");
}

// The bottom panel is one of the four surfaces in the Ctrl+Tab ring, but only its
// terminal content ever answered a key. With an Output channel showing, every
// navigation key fell through to the editor behind the panel and scrolled *that*,
// leaving a focused, visibly-scrollable surface with no keyboard at all.
void TestWorkspaceShellOutputPanelIsKeyboardNavigable() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowOutputChannelWithLines(shell, "test.output", 400);
  WorkspaceShellTestAccess::SetFocusPanel(shell);

  const int visible_rows = WorkspaceShellTestAccess::BottomPanelVisibleRowsForTest(shell);
  Expect(visible_rows > 0 && visible_rows < 400,
         "the output fixture should overflow the panel");
  const int max_scroll = 400 - visible_rows;

  Expect(WorkspaceShellTestAccess::OutputPanelScrollRow(shell) == 0,
         "the output fixture should start at the top");
  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE),
         "a focused output panel should answer Down");
  Expect(WorkspaceShellTestAccess::OutputPanelScrollRow(shell) == 1,
         "Down should advance the output panel by one row");

  Expect(SendKeyDown(shell, SDLK_PAGEDOWN, SDL_KMOD_NONE),
         "a focused output panel should answer Page Down");
  Expect(WorkspaceShellTestAccess::OutputPanelScrollRow(shell) == 1 + workspace::kListPageStep,
         "Page Down should advance the output panel by the shared page step");

  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE),
         "a focused output panel should answer End");
  Expect(WorkspaceShellTestAccess::OutputPanelScrollRow(shell) == max_scroll,
         "End should land on the last screenful of output");

  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE),
         "a focused output panel should answer Home");
  Expect(WorkspaceShellTestAccess::OutputPanelScrollRow(shell) == 0,
         "Home should land back on the first line of output");
}

// Clicking the channel body focuses the panel, the way clicking a terminal or a
// plugin surface already did. Output was the one content kind whose body could not
// take focus, so its keyboard was unreachable without Ctrl+Tab.
void TestWorkspaceShellOutputPanelBodyClickFocusesThePanel() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowOutputChannelWithLines(shell, "test.output", 400);
  WorkspaceShellTestAccess::SetFocusEditor(shell);

  const SDL_FRect body = WorkspaceShellTestAccess::BottomPanelContentRect(shell);
  Expect(SendMouseDown(shell, body.x + body.w * 0.5f, body.y + body.h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the output panel body should be handled");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "clicking the output panel body should focus the panel");
}

// Editor tabs close from the File menu, a context menu, Ctrl+W and the palette;
// project tabs close from their context menu. Terminal tabs could only be closed
// with the mouse — the ✕ or a middle click — with no command, no menu item and so
// no keyboard route at all.
void TestWorkspaceShellTerminalClosesFromTheCommandLine() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 2,
         "the close fixture should start with two terminals");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-close"),
         "term-close should close the active terminal");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1,
         "term-close should leave the other terminal open");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-close"),
         "term-close should close the last terminal too");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 0,
         "term-close should close the last terminal");

  // With nothing left to close the action reports rather than silently doing
  // nothing, and the menu/palette entry greys out.
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-close"),
         "term-close should reject when there is no terminal");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, WorkspaceShell::ActionId::TermClose),
         "Close Terminal should be disabled with no terminal open");
}

void RegisterWorkspaceShellTerminalTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/TerminalClosesFromTheCommandLine",
          TestWorkspaceShellTerminalClosesFromTheCommandLine);
  AddTest(tests, "WorkspaceShell/OutputPanelIsKeyboardNavigable",
          TestWorkspaceShellOutputPanelIsKeyboardNavigable);
  AddTest(tests, "WorkspaceShell/OutputPanelBodyClickFocusesThePanel",
          TestWorkspaceShellOutputPanelBodyClickFocusesThePanel);
  AddTest(tests, "WorkspaceShell/TerminalSelectionPreservesInternalSpaces",
          TestWorkspaceShellSelectionPreservesInternalSpaces);
  AddTest(tests, "WorkspaceShell/TerminalSelectionCopyDropsRowPaddingButNotWrappedBlanks",
          TestWorkspaceShellSelectionCopyDropsRowPaddingButNotWrappedBlanks);
  AddTest(tests, "WorkspaceShell/TerminalSelectionSkipsWideTrailingSpacer",
          TestWorkspaceShellSelectionSkipsWideTrailingSpacer);
  AddTest(tests, "WorkspaceShell/TerminalExitMarkerSurvivesOpenEscape",
          TestWorkspaceShellTerminalExitMarkerSurvivesOpenEscape);
  AddTest(tests, "WorkspaceShell/TerminalExitMarkerDisplaysAfterCleanOutput",
          TestWorkspaceShellTerminalExitMarkerDisplaysAfterCleanOutput);
  AddTest(tests, "WorkspaceShell/ExitedTerminalTabIsRetained",
          TestWorkspaceShellExitedTerminalTabIsRetained);
  AddTest(tests, "WorkspaceShell/TerminalCtrlShiftVPastesBracketedClipboard",
          TestWorkspaceShellCtrlShiftVPastesBracketedClipboard);
  AddTest(tests, "WorkspaceShell/TerminalShiftInsertPastesRawClipboard",
          TestWorkspaceShellShiftInsertPastesRawClipboard);
  AddTest(tests, "WorkspaceShell/TerminalCtrlVStillSendsControlV",
          TestWorkspaceShellCtrlVStillSendsControlV);
  AddTest(tests, "WorkspaceShell/TerminalArrowKeysHonorApplicationCursorMode",
          TestWorkspaceShellArrowKeysHonorApplicationCursorMode);
  AddTest(tests, "WorkspaceShell/TerminalTabsReflectOscTitles",
          TestWorkspaceShellTerminalTabsReflectOscTitles);
  AddTest(tests, "WorkspaceShell/TerminalOsc52CopiesToClipboard",
          TestWorkspaceShellTerminalOsc52CopiesToClipboard);
  AddTest(tests, "WorkspaceShell/TerminalFocusModeTracksPanelFocus",
          TestWorkspaceShellTerminalFocusModeTracksPanelFocus);
  AddTest(tests, "WorkspaceShell/TerminalFocusModeTracksWindowFocus",
          TestWorkspaceShellTerminalFocusModeTracksWindowFocus);
  AddTest(tests, "WorkspaceShell/FocusedTerminalParticipatesInCaretBlinking",
          TestWorkspaceShellFocusedTerminalParticipatesInCaretBlinking);
  AddTest(tests, "WorkspaceShell/CaretBlinkFreezesAfterIdleStop",
          TestWorkspaceShellCaretBlinkFreezesAfterIdleStop);
  AddTest(tests, "WorkspaceShell/ScheduledWakeSkipsCaretAfterIdleStop",
          TestWorkspaceShellScheduledWakeSkipsCaretAfterIdleStop);
  AddTest(tests, "WorkspaceShell/TerminalCaretDirtyRectTracksVisibleCursor",
          TestWorkspaceShellTerminalCaretDirtyRectTracksVisibleCursor);
  AddTest(tests, "WorkspaceShell/TerminalKeysReturnPartialPanelInvalidation",
          TestWorkspaceShellTerminalKeysReturnPartialPanelInvalidation);
  AddTest(tests, "WorkspaceShell/TerminalAsciiPromptMatchesDirectStringRendering",
          TestWorkspaceShellTerminalAsciiPromptMatchesDirectStringRendering);
  AddTest(tests, "WorkspaceShell/TerminalCaretBlinkRetainedRedrawPreservesGlyphEdges",
          TestWorkspaceShellTerminalCaretBlinkRetainedRedrawPreservesGlyphEdges);
  AddTest(tests, "WorkspaceShell/TypingReenablesTerminalTailFollow",
          TestWorkspaceShellTypingReenablesTerminalTailFollow);
  AddTest(tests, "WorkspaceShell/HandleEventPassesEscapeToTerminal",
          TestWorkspaceShellHandleEventPassesEscapeToTerminal);
  AddTest(tests, "WorkspaceShell/CopyLastTerminalCommandIncludesOutput",
          TestWorkspaceShellCopyLastTerminalCommandIncludesOutput);
  AddTest(tests, "WorkspaceShell/TerminalPendingInputIsCapped",
          TestWorkspaceShellTerminalPendingInputIsCapped);
  AddTest(tests, "WorkspaceShell/CopyLastTerminalCommandEnablementUsesCheapPredicate",
          TestWorkspaceShellCopyLastTerminalCommandEnablementUsesCheapPredicate);
  AddTest(tests, "WorkspaceShell/CopyLastTerminalCommandFallsBackDuringAlternateScreen",
          TestWorkspaceShellCopyLastTerminalCommandFallsBackDuringAlternateScreen);
  AddTest(tests, "WorkspaceShell/CopyLastTerminalCommandIgnoresPrecedingFullWidthOutput",
          TestWorkspaceShellCopyLastTerminalCommandIgnoresPrecedingFullWidthOutput);
  AddTest(tests, "WorkspaceShell/CopyLastTerminalCommandPreservesSoftWrappedInvocation",
          TestWorkspaceShellCopyLastTerminalCommandPreservesSoftWrappedInvocation);
  AddTest(tests, "WorkspaceShell/TerminalTabRightClickOpensContextMenu",
          TestWorkspaceShellTerminalTabRightClickOpensContextMenu);
  AddTest(tests, "WorkspaceShell/TerminalTabsStartAtPanelEdge",
          TestWorkspaceShellTerminalTabsStartAtPanelEdge);
  AddTest(tests, "WorkspaceShell/TerminalPanelRightClickOpensContextMenu",
          TestWorkspaceShellTerminalPanelRightClickOpensContextMenu);
  AddTest(tests, "WorkspaceShell/TerminalPasteActionTargetsPanelFocus",
          TestWorkspaceShellTerminalPasteActionTargetsPanelFocus);
  AddTest(tests, "WorkspaceShell/TerminalPasteActionCapsClipboardBytes",
          TestWorkspaceShellTerminalPasteActionCapsClipboardBytes);
  AddTest(tests, "WorkspaceShell/TerminalDirectPasteIsCapped",
          TestWorkspaceShellTerminalDirectPasteIsCapped);
  AddTest(tests, "WorkspaceShell/TerminalTabsDragReorderToStart",
          TestWorkspaceShellTerminalTabsDragReorderToStart);
  AddTest(tests, "WorkspaceShell/TerminalTabsOverflowReachableViaHeaderWheel",
          TestWorkspaceShellTerminalTabsOverflowReachableViaHeaderWheel);
  AddTest(tests, "WorkspaceShell/BottomPanelWheelScrollsTranscript",
          TestWorkspaceShellBottomPanelWheelScrollsTranscript);
  AddTest(tests, "WorkspaceShell/TerminalDragAutoscrollsPastTheEdge",
          TestWorkspaceShellTerminalDragAutoscrollsPastTheEdge);
  AddTest(tests, "WorkspaceShell/TerminalDragSelectsTranscriptText",
          TestWorkspaceShellTerminalDragSelectsTranscriptText);
  AddTest(tests, "WorkspaceShell/TerminalDoubleClickSelectsWordTripleClickSelectsLine",
          TestWorkspaceShellTerminalDoubleClickSelectsWordTripleClickSelectsLine);
  AddTest(tests, "WorkspaceShell/TerminalSelectionWritesPrimaryBufferAndMiddleClickPastes",
          TestWorkspaceShellTerminalSelectionWritesPrimaryBufferAndMiddleClickPastes);
  AddTest(tests, "WorkspaceShell/TerminalLeftClickOpensUrls",
          TestWorkspaceShellTerminalLeftClickOpensUrls);
  AddTest(tests, "WorkspaceShellTerminal/UrlHitTestHonorsMultibytePrefix",
          TestWorkspaceShellTerminalUrlHitTestHonorsMultibytePrefix);
  AddTest(tests, "WorkspaceShellTerminal/TerminalUrlKeepsBalancedClosingBracket",
          TestWorkspaceShellTerminalUrlKeepsBalancedClosingBracket);
  AddTest(tests, "WorkspaceShellTerminal/UrlHitTestHonorsUppercaseScheme",
          TestWorkspaceShellTerminalUrlHitTestHonorsUppercaseScheme);
  AddTest(tests, "WorkspaceShellTerminal/UrlHoverUsesRenderedSnapshot",
          TestWorkspaceShellTerminalUrlHoverUsesRenderedSnapshot);
  AddTest(tests, "WorkspaceShell/TerminalMouseCaptureSendsButtonEvents",
          TestWorkspaceShellTerminalMouseCaptureSendsButtonEvents);
  AddTest(tests, "WorkspaceShell/TerminalCtrlFOpensFindBar",
          TestWorkspaceShellTerminalCtrlFOpensFindBar);
  AddTest(tests, "WorkspaceShell/TerminalFindSelectsNewestMatchFirst",
          TestWorkspaceShellTerminalFindSelectsNewestMatchFirst);
  AddTest(tests, "WorkspaceShell/TerminalFindNavigationWrapsAndReveals",
          TestWorkspaceShellTerminalFindNavigationWrapsAndReveals);
  AddTest(tests, "WorkspaceShell/TerminalFindTogglesNarrowMatches",
          TestWorkspaceShellTerminalFindTogglesNarrowMatches);
  AddTest(tests, "WorkspaceShell/TerminalFindEscapeReturnsKeyboardToTerminal",
          TestWorkspaceShellTerminalFindEscapeReturnsKeyboardToTerminal);
  AddTest(tests, "WorkspaceShell/TerminalFindTracksStreamingOutput",
          TestWorkspaceShellTerminalFindTracksStreamingOutput);
  AddTest(tests, "WorkspaceShell/TerminalFindClipboardShortcutsTargetTheQuery",
          TestWorkspaceShellTerminalFindClipboardShortcutsTargetTheQuery);
}

}  // namespace microide::tests
