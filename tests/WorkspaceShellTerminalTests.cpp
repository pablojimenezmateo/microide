#include "TestSupport.h"

#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"
#include "support/TerminalHostMirror.h"

#include "TerminalSessionTestAccess.h"
#include "util/AnsiPalette.h"
#include "util/JsonValue.h"
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

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]52;c;Y29waWVkIGZyb20gdGVybQ==\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(clipboard_text.empty(),
         "OSC 52 must not write the system clipboard by default (poisoning guard)");
  Expect(WorkspaceShellTestAccess::ActiveNotifications(shell).size() == 1,
         "a blocked OSC 52 write should surface a hint");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]52;c;Y29waWVkIGZyb20gdGVybQ==\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(clipboard_text.empty(),
         "OSC 52 stays blocked before opt-in");
  const auto& blocked_hints = WorkspaceShellTestAccess::ActiveNotifications(shell);
  Expect(blocked_hints.size() == 1,
         "a repeated blocked OSC 52 write should refresh the hint, not stack a duplicate");
  Expect(!blocked_hints.front().message.empty(),
         "the blocked-write hint should still be on screen after the repeat");

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
  TerminalSessionTestAccess::ClearSentBytes(session);
  Expect(SendKeyDown(shell, SDLK_F6, SDL_KMOD_NONE), "F6 in the terminal is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[17~", "F6 reaches the program");
  TerminalSessionTestAccess::ClearSentBytes(session);
  for (int step = 0; step < 3; ++step) {
    Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::FocusNextPart, {}),
           "Focus Next Part cycles the focus targets");
    WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  }
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell), "three steps return to the terminal panel");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[O\x1b[I",
         "terminal focus mode should emit focus-out and focus-in notifications as panel focus changes: [" +
             TerminalSessionTestAccess::SentBytes(session) + "]");
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
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);

  WorkspaceShellTestAccess::ResetCaretBlink(shell);
  Expect(WorkspaceShellTestAccess::CaretBlinkAnimating(shell),
         "blink should be animating right after a reset");
  Expect(WorkspaceShellTestAccess::NextCaretBlinkDelayMs(shell).has_value(),
         "blink wakes should be scheduled right after a reset");

  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks() - 8000);
  Expect(!WorkspaceShellTestAccess::CaretBlinkAnimating(shell),
         "blink should freeze after the idle-stop threshold");
  Expect(WorkspaceShellTestAccess::CaretVisibleNow(shell),
         "caret should freeze in its visible phase after idle-stop");
  Expect(!WorkspaceShellTestAccess::NextCaretBlinkDelayMs(shell).has_value(),
         "no blink wakes should be scheduled while frozen");

  WorkspaceShellTestAccess::ResetCaretBlink(shell);
  Expect(WorkspaceShellTestAccess::CaretBlinkAnimating(shell),
         "blink should resume after reset");
  Expect(WorkspaceShellTestAccess::NextCaretBlinkDelayMs(shell).has_value(),
         "blink wakes should resume after reset");
}

void TestWorkspaceShellScheduledWakeSkipsCaretAfterIdleStop() {
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

void TestWorkspaceShellTerminalCaretCoversAWideGlyphWhole() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(1280, 720);
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\xF0\x9F\x98\x86\x1b[2D");
  shell.Render(canvas.renderer(), 1280, 720);
  const std::optional<SDL_FRect> on_wide = shell.CurrentCaretDirtyRect();
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2C");
  shell.Render(canvas.renderer(), 1280, 720);
  const std::optional<SDL_FRect> on_narrow = shell.CurrentCaretDirtyRect();
  Expect(on_wide.has_value() && on_narrow.has_value(), "the caret is on screen both times");
  Expect(on_wide->w == 2.0f * on_narrow->w,
         "a caret on a wide glyph covers both its cells: " + std::to_string(on_wide->w) + " vs one cell " +
             std::to_string(on_narrow->w));
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

void TestWorkspaceShellTerminalKeysOweAFrame() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_UP;
  const auto result = shell.HandleEvent(event);

  Expect(result.handled, "terminal navigation keys should be handled");
  Expect(result.redraw.frame && !result.redraw.full && result.redraw.rects.empty(),
         "a terminal key owes a frame and names no rect: rects=" + std::to_string(result.redraw.rects.size()));
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

void TestWorkspaceShellTerminalReverseVideoPaintsThePane() {
  EnsureDummySdlVideo();
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "editor.caret_blink.enabled", "false"),
         "the caret blink can be turned off");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "$ ");

  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const SDL_FPoint blank = WorkspaceShellTestAccess::TerminalCellPoint(shell, 3, 20);
  const auto pixel = [&]() {
    return canvas.PixelAt(static_cast<int>(blank.x), static_cast<int>(blank.y));
  };
  const auto same = [](SDL_Color a, SDL_Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
  };
  const SDL_Color normal = pixel();

  workspace::RenderInvalidation unrelated;
  unrelated.rects.push_back(SDL_FRect{0.0f, 0.0f, 8.0f, 8.0f});

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?5h");
  const auto on = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated);
  Expect(on.count == 0, "the reversed pane is painted by the next frame: " + DescribePixelDiff(on));
  Expect(!same(pixel(), normal), "the blank below the text takes the reversed colour");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?5l");
  const auto off = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated);
  Expect(off.count == 0, "and the restored pane too: " + DescribePixelDiff(off));
  Expect(same(pixel(), normal), "the pane is back to its normal colour");
}

void TestWorkspaceShellTerminalSelectionWashesCellsAndLineBreaks() {
  EnsureDummySdlVideo();
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[42m  \x1b[0m ab\r\nsecond row");
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "editor.caret_blink.enabled", "false"),
         "the fixture turns caret blink off");

  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto pixel = [&](std::size_t row, std::size_t column) {
    const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, row, column);
    return canvas.PixelAt(static_cast<int>(point.x), static_cast<int>(point.y));
  };
  const auto same = [](SDL_Color a, SDL_Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
  const SDL_Color green = pixel(0, 0);
  const SDL_Color blank = pixel(0, 2);
  const SDL_Color past_end = pixel(0, 60);
  Expect(same(blank, past_end) && !same(green, blank), "the fixture: a green cell on a plain row");

  workspace::RenderInvalidation unrelated;
  unrelated.rects.push_back(SDL_FRect{0.0f, 0.0f, 8.0f, 8.0f});
  WorkspaceShellTestAccess::SetActiveTerminalSelectionForTest(
      shell, workspace::TerminalSelectionPoint{.row = 0, .column = 0},
      workspace::TerminalSelectionPoint{.row = 1, .column = 3});
  const auto selected = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated);
  Expect(selected.count == 0, "the selection is painted by the next frame: " + DescribePixelDiff(selected));
  const SDL_Color washed_blank = pixel(0, 2);
  const SDL_Color washed_green = pixel(0, 0);
  Expect(!same(washed_blank, blank), "a selected blank cell is washed");
  Expect(!same(washed_green, green) && !same(washed_green, washed_blank),
         "a selected green cell is washed over its green, not replaced by the plain wash");
  Expect(same(pixel(0, 60), washed_blank),
         "the row the selection continues past is washed to the pane's edge");
  Expect(same(pixel(1, 60), past_end), "the selection's last row stops at its end");

  WorkspaceShellTestAccess::SetFocusEditor(shell);
  const auto unfocused = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated);
  Expect(unfocused.count == 0, "the focus change repaints the selection: " + DescribePixelDiff(unfocused));
  const SDL_Color inactive = pixel(0, 2);
  Expect(!same(inactive, washed_blank) && !same(inactive, blank),
         "an unfocused terminal's selection is a weaker wash, still visible");
}

void TestWorkspaceShellTerminalAlternateScreenDropsTheSelection() {
  EnsureDummySdlVideo();
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "shell output\r\n$ ");
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "editor.caret_blink.enabled", "false"),
         "the fixture turns caret blink off");
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  WorkspaceShellTestAccess::SetActiveTerminalSelectionForTest(
      shell, workspace::TerminalSelectionPoint{.row = 0, .column = 0},
      workspace::TerminalSelectionPoint{.row = 0, .column = 5});
  workspace::RenderInvalidation unrelated;
  unrelated.rects.push_back(SDL_FRect{0.0f, 0.0f, 8.0f, 8.0f});
  (void)PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated);
  Expect(WorkspaceShellTestAccess::TerminalHasSelection(shell), "the fixture selected 'shell'");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049h\x1b[Hvim screen");
  const auto entered = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated);
  Expect(entered.count == 0, "the alternate screen frame is fresh: " + DescribePixelDiff(entered));
  Expect(!WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "entering the alternate screen dropped the selection");
  TerminalSessionTestAccess::ClearSentBytes(session);
  Expect(WorkspaceShellTestAccess::HandleTerminalKeyDown(shell, SDLK_C, SDL_KMOD_CTRL), "Ctrl+C is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == std::string(1, '\x03'),
         "Ctrl+C interrupts the program instead of copying the old selection");

  WorkspaceShellTestAccess::SetActiveTerminalSelectionForTest(
      shell, workspace::TerminalSelectionPoint{.row = 0, .column = 0},
      workspace::TerminalSelectionPoint{.row = 0, .column = 3});
  (void)PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049l");
  const auto left = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated);
  Expect(left.count == 0, "the frame after leaving is fresh: " + DescribePixelDiff(left));
  Expect(!WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "leaving the alternate screen dropped vim's selection");
}

void TestWorkspaceShellTerminalCursorTakesTheProgramsShape() {
  EnsureDummySdlVideo();
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetCursorVisible(session, true);
  TerminalSessionTestAccess::AppendOutput(session, "$ ");
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  const auto visible_phase = [&]() { WorkspaceShellTestAccess::ResetCaretBlink(shell); };
  const auto hidden_phase = [&]() {
    WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks() - 530);
  };

  SoftwareCanvas canvas(kWidth, kHeight);
  visible_phase();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const std::optional<SDL_FRect> cell = WorkspaceShellTestAccess::CaretDirtyRect(shell);
  Expect(cell.has_value() && cell->w >= 6.0f, "the fixture has a terminal caret");
  const auto pixel = [&](float fx, float fy) {
    return canvas.PixelAt(static_cast<int>(cell->x + cell->w * fx), static_cast<int>(cell->y + cell->h * fy));
  };
  const auto same = [](SDL_Color a, SDL_Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
  const SDL_Color caret = pixel(0.75f, 0.5f);
  const SDL_Color blank = canvas.PixelAt(static_cast<int>(cell->x + cell->w * 6.5f),
                                         static_cast<int>(cell->y + cell->h * 0.5f));
  Expect(!same(caret, blank), "a block caret fills its cell");

  workspace::RenderInvalidation unrelated;
  unrelated.rects.push_back(SDL_FRect{0.0f, 0.0f, 8.0f, 8.0f});
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[5 q");
  auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated, visible_phase);
  Expect(diff.count == 0, "the switch to a bar is painted by the next frame: " + DescribePixelDiff(diff));
  Expect(same(pixel(0.05f, 0.5f), caret), "a bar at the cell's left edge");
  Expect(same(pixel(0.75f, 0.5f), blank), "and not across the cell");
  diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated, hidden_phase);
  Expect(diff.count == 0, "the blink is repainted: " + DescribePixelDiff(diff));
  Expect(same(pixel(0.05f, 0.5f), blank), "a blinking bar blinks");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[4 q");
  diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated, hidden_phase);
  Expect(diff.count == 0, "the switch to an underline is painted: " + DescribePixelDiff(diff));
  Expect(same(pixel(0.5f, 0.97f), caret), "a steady underline shows in the blink's hidden phase");
  Expect(same(pixel(0.5f, 0.4f), blank), "an underline only along the bottom");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[0 q");
  diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated, visible_phase);
  Expect(diff.count == 0, "back to the default block: " + DescribePixelDiff(diff));
  Expect(same(pixel(0.75f, 0.5f), caret), "the default is a block");
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

void TestWorkspaceShellTerminalPendingInputIsCapped() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);

  const std::size_t cap = microide::workspace::TerminalPaneState::kMaxPendingInputBytes;
  std::string chunk;
  for (int i = 0; i < 4096; ++i) {
    chunk += "\xC3\xA9";
  }
  for (int i = 0; i < 400; ++i) {
    WorkspaceShellTestAccess::AppendTerminalPendingInput(shell, chunk);
  }
  const std::size_t size = WorkspaceShellTestAccess::ActiveTerminalPendingInputSize(shell);
  Expect(size <= cap, "pending_input must never exceed the capture budget");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPendingInputTruncated(shell),
         "hitting the pending-input budget must set the truncated flag");
  Expect(size % 2 == 0,
         "the truncation must land on a UTF-8 codepoint boundary (2-byte 'é' units)");

  WorkspaceShellTestAccess::SubmitTerminalPendingInput(shell);
  Expect(WorkspaceShellTestAccess::ActiveTerminalPendingInputSize(shell) == 0,
         "submit clears the pending-input capture");
  Expect(!WorkspaceShellTestAccess::ActiveTerminalPendingInputTruncated(shell),
         "submit resets the truncation flag");
}

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
    std::string launch_label = "terminal-session-";
    launch_label += std::to_string(i);
    TerminalSessionTestAccess::SetLaunchLabel(
        WorkspaceShellTestAccess::ActiveTerminalSession(shell), std::move(launch_label));
  }
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

void TestWorkspaceShellTerminalShiftPageKeysScrollTheTranscript() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  std::string transcript;
  for (int i = 0; i < 200; ++i) {
    transcript += "line " + std::to_string(i) + "\n";
  }
  TerminalSessionTestAccess::AppendOutput(session, transcript);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, false);
  WorkspaceShellTestAccess::SetActiveTerminalScrollRow(shell, 0);
  WorkspaceShellTestAccess::SetFocusPanel(shell);

  const int visible_rows = WorkspaceShellTestAccess::BottomPanelVisibleRowsForTest(shell);
  Expect(visible_rows > 2, "the fixture needs a panel taller than two rows");
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_PAGEDOWN, SDL_KMOD_SHIFT),
         "Shift+PageDown is handled by the terminal panel");
  Expect(WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell) == visible_rows - 1,
         "Shift+PageDown scrolls one page (visible rows less one) toward the tail");
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_SHIFT),
         "Shift+PageUp is handled by the terminal panel");
  Expect(WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell) == 0,
         "Shift+PageUp scrolls the page back");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "neither shifted page key reaches the shell");

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_NONE),
         "plain PageUp is handled");
  Expect(!TerminalSessionTestAccess::SentBytes(session).empty(),
         "plain PageUp still goes to the shell as a key");
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

void TestWorkspaceShellTerminalCtrlShiftCNeverInterrupts() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "select me");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusPanel(shell);

  std::string clipboard;
  WorkspaceShellTestAccess::SetClipboardTextWriter(shell, [&](std::string_view text) {
    clipboard = std::string(text);
    return true;
  });

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_C, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+C without a selection is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "Ctrl+Shift+C without a selection sends nothing to the shell (no ^C)");
  Expect(clipboard.empty(), "and copies nothing");

  const SDL_FPoint start = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  const SDL_FPoint end = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 6);
  Expect(SendMouseDown(shell, start.x, start.y, SDL_BUTTON_LEFT), "press starts a selection");
  Expect(SendMouseMotion(shell, end.x, end.y, SDL_BUTTON_LMASK), "drag extends it");
  Expect(SendMouseUp(shell, end.x, end.y, SDL_BUTTON_LEFT), "release ends it");
  Expect(WorkspaceShellTestAccess::TerminalHasSelection(shell), "the fixture selected text");
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_C, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+C with a selection is handled");
  Expect(clipboard == "select", "Ctrl+Shift+C copies the selection: [" + clipboard + "]");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(), "and still sends nothing");

  WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE);
  Expect(!WorkspaceShellTestAccess::TerminalHasSelection(shell), "Escape cleared the selection");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b",
         "and the program got the Escape: [" + TerminalSessionTestAccess::SentBytes(session) + "]");
  TerminalSessionTestAccess::ClearSentBytes(session);
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_C, SDL_KMOD_CTRL), "Ctrl+C is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x03",
         "plain Ctrl+C without a selection reaches the shell as ^C");
}


void TestWorkspaceShellTerminalCtrlSlashReachesTheShell() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusPanel(shell);

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_SLASH, SDL_KMOD_CTRL),
         "Ctrl+/ in the terminal is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == std::string(1, '\x1f'),
         "Ctrl+/ reaches the shell as US (0x1f)");
  TerminalSessionTestAccess::ClearSentBytes(session);
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_2, SDL_KMOD_CTRL),
         "Ctrl+2 in the terminal is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == std::string(1, '\0'),
         "Ctrl+2 reaches the shell as NUL");
}

void TestWorkspaceShellTerminalOwnsGlobalControlChords() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  const bool sidebar_visible = WorkspaceShellTestAccess::SidebarVisible(shell);

  const struct {
    SDL_Keycode key;
    char byte;
    const char* name;
  } chords[] = {
      {SDLK_O, '\x0f', "Ctrl+O"},
      {SDLK_S, '\x13', "Ctrl+S"},
      {SDLK_N, '\x0e', "Ctrl+N"},
      {SDLK_B, '\x02', "Ctrl+B"},
      {SDLK_P, '\x10', "Ctrl+P"},
      {SDLK_J, '\n', "Ctrl+J"},
      {SDLK_Q, '\x11', "Ctrl+Q"},
      {SDLK_F, '\x06', "Ctrl+F"},
      {SDLK_MINUS, '\x1f', "Ctrl+-"},
  };
  for (const auto& chord : chords) {
    TerminalSessionTestAccess::ClearSentBytes(session);
    Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, chord.key, SDL_KMOD_CTRL),
           std::string(chord.name) + " in the terminal is handled");
    Expect(TerminalSessionTestAccess::SentBytes(session) == std::string(1, chord.byte),
           std::string(chord.name) + " reaches the shell as its control byte");
  }
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "no global binding fired: nothing opened an overlay");
  Expect(WorkspaceShellTestAccess::SidebarVisible(shell) == sidebar_visible,
         "Ctrl+B in the terminal does not toggle the sidebar");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell), "the terminal keeps focus");

  TerminalSessionTestAccess::ClearSentBytes(session);
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+P in the terminal is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "Ctrl+Shift+P skips the shell");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
                 WorkspaceShell::OverlayMode::CommandPalette,
         "Ctrl+Shift+P still opens the command palette from the terminal");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape closes the palette");
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049h");
  TerminalSessionTestAccess::ClearSentBytes(session);
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_SHIFT),
         "Shift+PageUp on the alternate screen is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[5;2~",
         "and reaches the program: [" + TerminalSessionTestAccess::SentBytes(session) + "]");
}

void TestWorkspaceShellTerminalSkipShellSettingEditsTheSet() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "terminal.commands_to_skip_shell",
                                                   "-file-finder, new-tab"),
         "the setting is accepted");

  TerminalSessionTestAccess::ClearSentBytes(session);
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_P, SDL_KMOD_CTRL),
         "Ctrl+P in the terminal is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == std::string(1, '\x10'),
         "Ctrl+P reaches the shell once the user takes it off the skip list");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell), "Go to File did not open");

  TerminalSessionTestAccess::ClearSentBytes(session);
  (void)WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_N, SDL_KMOD_CTRL);
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "Ctrl+N skips the shell once the user lists new-tab");
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

void TestWorkspaceShellTerminalGapOnScreenFillsAndTheSelectionFollows() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetPanelContentTerminal(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  Mirror mirror(session, 3, 40);
  mirror.host.SetMaxScrollbackLines(5000);
  session.SetMaxScrollbackLines(5000);
  mirror.channel->history_source = &mirror.host;
  Expect(mirror.Sync(), "sync");
  mirror.Write("kept above\r\n");
  Expect(mirror.Sync(), "the line above the gap arrives");
  for (int i = 0; i < 300; ++i) {
    mirror.Write("flood " + std::to_string(i) + "\r\n");
  }
  Expect(mirror.Sync(0), "the flood is withheld behind a rule");
  mirror.Write("below the gap\r\n");
  for (int i = 0; i < 100; ++i) {
    mirror.Write("tail " + std::to_string(i) + "\r\n");
  }
  Expect(mirror.Sync(), "and the lines below it arrive");

  const auto row_of = [&](std::string_view needle) -> std::optional<std::size_t> {
    const std::vector<terminal::TerminalLine> lines = session.SnapshotLines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (LineText(lines[i]).find(needle) != std::string::npos) {
        return i;
      }
    }
    return std::nullopt;
  };
  const std::optional<std::size_t> kept = row_of("kept above");
  const std::optional<std::size_t> below = row_of("below the gap");
  Expect(kept.has_value() && below.has_value() && row_of("lines not transferred").has_value(),
         "a rule between two real lines");
  if (!kept.has_value() || !below.has_value()) {
    return;
  }
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  WorkspaceShellTestAccess::SetActiveTerminalSelectionForTest(
      shell, workspace::TerminalSelectionPoint{.row = *kept, .column = 0},
      workspace::TerminalSelectionPoint{.row = *below, .column = 13});
  Expect(WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell).find("lines not transferred") !=
             std::string::npos,
         "before the fill the selection runs across the rule");

  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, false);
  WorkspaceShellTestAccess::SetActiveTerminalScrollRow(shell, static_cast<int>(*kept));
  for (int frame = 0; frame < 4; ++frame) {
    shell.Render(canvas.renderer(), 1280, 720);
    (void)shell.ConsumePendingRenderInvalidation();
  }
  Expect(!row_of("lines not transferred").has_value(), "the rule's lines came in");
  const std::string selected = WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell);
  Expect(selected.rfind("kept above", 0) == 0 && selected.find("flood 0") != std::string::npos &&
             selected.find("flood 299") != std::string::npos &&
             selected.find("below the gap") != std::string::npos &&
             selected.find("lines not transferred") == std::string::npos,
         "the selection followed its rows and copies the text the rule stood for: '" +
             selected.substr(0, 80) + "' ... '" +
             selected.substr(selected.size() > 80 ? selected.size() - 80 : 0) + "'");
  Expect(WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell) == static_cast<int>(*kept),
         "a view anchored above the gap stays where it was");
}

void TestWorkspaceShellUiTerminalGeometryResolvesCells() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "select me");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  const util::JsonValue terminal = WorkspaceShellTestAccess::ControlUi(shell)["terminal"];
  Expect(terminal.IsObject(), "the ui query should describe the terminal pane");
  const auto point = [&](int row, int column) {
    return SDL_FPoint{
        static_cast<float>(terminal["textX"].AsDouble() + (column + 0.3) * terminal["charWidth"].AsDouble()),
        static_cast<float>(terminal["firstLineY"].AsDouble() +
                           (row - terminal["firstRow"].AsInt() + 0.5) * terminal["lineHeight"].AsDouble())};
  };
  const SDL_FPoint start = point(0, 0);
  const SDL_FPoint end = point(0, 6);
  (void)SendMouseDown(shell, start.x, start.y, SDL_BUTTON_LEFT);
  (void)SendMouseMotion(shell, end.x, end.y, SDL_BUTTON_LMASK);
  (void)SendMouseUp(shell, end.x, end.y, SDL_BUTTON_LEFT);
  Expect(WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell) == "select",
         "a drag between the ui query's cell points should select 'select', got '" +
             WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell) + "'");
}

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

void TestWorkspaceShellTerminalAutoscrollNeverScrollsTheEditor() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "project" / "long.txt";
  std::string text;
  for (int i = 0; i < 400; ++i) {
    text += "editor line " + std::to_string(i) + "\n";
  }
  WriteFile(file, "\n");
  WorkspaceShellTestAccess::SetProjectRoot(shell, file.parent_path());
  WorkspaceShellTestAccess::OpenFile(shell, file);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText(text);
  WorkspaceShellTestAccess::ActiveEditor(shell).SetViewportSize(20, 80);
  WorkspaceShellTestAccess::ActiveEditor(shell).SetScrollLine(0);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  for (int i = 0; i < 300; ++i) {
    TerminalSessionTestAccess::AppendOutput(session, "line " + std::to_string(i) + "\r\n");
  }
  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, false);
  WorkspaceShellTestAccess::SetActiveTerminalScrollRow(shell, 0);

  const SDL_FPoint start = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  const SDL_FRect panel_rect = WorkspaceShellTestAccess::BottomPanelContentRect(shell);
  Expect(SendMouseDown(shell, start.x, start.y, SDL_BUTTON_LEFT), "a press in the terminal starts a selection");
  Expect(SendMouseMotion(shell, start.x, panel_rect.y + panel_rect.h + 200.0f, SDL_BUTTON_LMASK),
         "a drag below the terminal arms its autoscroll");
  Expect(WorkspaceShellTestAccess::HandleScheduledWake(shell).handled,
         "the terminal's autoscroll steps (the case under test)");

  Expect(SendKeyDown(shell, SDLK_A, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
         "Ctrl+Shift+A selects the whole transcript, ending the drag");
  const std::size_t editor_scroll = WorkspaceShellTestAccess::ActiveEditor(shell).scroll_line();
  for (int tick = 0; tick < 5; ++tick) {
    (void)WorkspaceShellTestAccess::HandleScheduledWake(shell);
  }
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).scroll_line() == editor_scroll,
         "the terminal's autoscroll scrolls the editor (from " + std::to_string(editor_scroll) + " to " +
             std::to_string(WorkspaceShellTestAccess::ActiveEditor(shell).scroll_line()) + ")");
}

void TestWorkspaceShellTerminalDoubleClickSelectsWordTripleClickSelectsLine() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "warn src/foo/bar.cpp:42 done");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

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

  const SDL_FPoint blank = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 60);
  Expect(SendMouseDown(shell, blank.x, blank.y, SDL_BUTTON_LEFT, 2),
         "double-clicking blank terminal space should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell).empty(),
         "double-clicking blank space should not select anything");
}

void TestWorkspaceShellTerminalMultiClickDragsAndShiftClickExtend() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "one two three four\r\nsecond row\r\nthird row");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  const auto at = [&](std::size_t row, std::size_t column) {
    return WorkspaceShellTestAccess::TerminalCellPoint(shell, row, column);
  };
  const auto selected = [&]() { return WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell); };

  Expect(SendMouseDown(shell, at(0, 9).x, at(0, 9).y, SDL_BUTTON_LEFT, 2), "double-click 'three'");
  Expect(selected() == "three", "the word: [" + selected() + "]");
  Expect(SendMouseMotion(shell, at(0, 1).x, at(0, 1).y, SDL_BUTTON_LMASK), "drag back");
  Expect(selected() == "one two three", "back by words, keeping the word: [" + selected() + "]");
  Expect(SendMouseMotion(shell, at(0, 15).x, at(0, 15).y, SDL_BUTTON_LMASK), "drag forward");
  Expect(selected() == "three four", "forward by words: [" + selected() + "]");
  Expect(SendMouseUp(shell, at(0, 15).x, at(0, 15).y, SDL_BUTTON_LEFT), "release");

  {
    ScopedHeldModifiers held(shell, SDL_KMOD_SHIFT);
    Expect(SendMouseDown(shell, at(1, 2).x, at(1, 2).y, SDL_BUTTON_LEFT), "Shift+click");
    Expect(SendMouseUp(shell, at(1, 2).x, at(1, 2).y, SDL_BUTTON_LEFT), "release");
  }
  Expect(selected() == "three four\nsecond", "Shift+click extends by words: [" + selected() + "]");

  Expect(SendMouseDown(shell, at(1, 3).x, at(1, 3).y, SDL_BUTTON_LEFT, 3), "triple-click row 1");
  Expect(SendMouseMotion(shell, at(2, 1).x, at(2, 1).y, SDL_BUTTON_LMASK), "drag down");
  Expect(SendMouseUp(shell, at(2, 1).x, at(2, 1).y, SDL_BUTTON_LEFT), "release");
  Expect(selected() == "second row\nthird row", "a row drag takes rows whole: [" + selected() + "]");

  Expect(SendMouseDown(shell, at(0, 4).x, at(0, 4).y, SDL_BUTTON_LEFT), "click");
  Expect(SendMouseUp(shell, at(0, 4).x, at(0, 4).y, SDL_BUTTON_LEFT), "release");
  {
    ScopedHeldModifiers held(shell, SDL_KMOD_SHIFT);
    Expect(SendMouseDown(shell, at(0, 7).x, at(0, 7).y, SDL_BUTTON_LEFT), "Shift+click");
    Expect(SendMouseUp(shell, at(0, 7).x, at(0, 7).y, SDL_BUTTON_LEFT), "release");
  }
  Expect(selected() == "two", "click, Shift+click selects between them: [" + selected() + "]");
}

void TestWorkspaceShellTerminalSelectAllCopiesTheWholeTranscript() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 40);
  std::string expected;
  std::string output;
  for (int line = 0; line < 700; ++line) {
    std::string text = "line " + std::to_string(line);
    if (line == 255) {
      text += std::string(60, 'w');
    }
    output += text + "\r\n";
    expected += text + "\n";
  }
  output += "$ ";
  expected += "$";
  TerminalSessionTestAccess::AppendOutput(session, output);
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  std::string primary;
  WorkspaceShellTestAccess::SetPrimarySelectionTextWriter(shell, [&](std::string_view text) {
    primary = std::string(text);
    return true;
  });

  Expect(SendKeyDown(shell, SDLK_A, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
         "Ctrl+Shift+A is handled in the terminal");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(), "and is not sent to the shell");
  const std::string selected = WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell);
  Expect(selected == expected, "Select All takes the whole transcript, in order (" +
                                   std::to_string(selected.size()) + " of " +
                                   std::to_string(expected.size()) + " bytes)");
  Expect(primary == expected, "and offers it as the primary selection, as a drag does");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-select-all"),
         "term-select-all runs as a command too");
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

void TestWorkspaceShellTerminalFileReferenceClickOpensTheFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int x;\nint \xc3\xa9 = y;\nint z;\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "src/main.cpp:2:7: error: y undeclared");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string opened_url;
  WorkspaceShellTestAccess::SetExternalUrlOpener(shell, [&](std::string_view url) {
    opened_url = std::string(url);
    return true;
  });

  const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 5);
  Expect(SendMouseDown(shell, point.x, point.y, SDL_BUTTON_LEFT),
         "clicking a file reference in the terminal is handled");
  Expect(opened_url.empty(), "a file reference is not handed to the external opener");
  const auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(viewport.path().lexically_normal() == source.lexically_normal(),
         "the referenced file opens in the editor: " + viewport.path().string());
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 7,
         "the caret lands on the referenced line and character column");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell), "the editor takes focus");

  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "error nope/missing.cpp:3:1 done");
  const SDL_FPoint word = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 2);
  Expect(!WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, word.x, word.y).has_value(),
         "a bare word is not a link");
  const SDL_FPoint missing = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 10);
  Expect(!WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, missing.x, missing.y).has_value(),
         "a path that does not exist is not a link");
}

void TestWorkspaceShellTerminalMouseOwnerShowsAHandOnLitControls() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, temp_dir.path());
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);
  TerminalSessionTestAccess::AppendOutput(session, "plain text here\r\n");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  const SDL_FPoint plain = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 2);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsText(shell, plain.x, plain.y),
         "a shell that does not own the mouse keeps the I-beam (selection)");

  TerminalSessionTestAccess::AppendOutput(
      session, "\x1b[?1003h\x1b[?1006h\x1b[2;3H\x1b[38;2;153;153;153mListed \x1b[1m1\x1b[22m directory\x1b[39m");
  const SDL_FPoint group = WorkspaceShellTestAccess::TerminalCellPoint(shell, 1, 4);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsDefault(shell, plain.x, plain.y),
         "over a program that owns the mouse, plain text shows the arrow");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsDefault(shell, group.x, group.y),
         "a control the program has not lit is not known to be one");

  Expect(SendMouseMotion(shell, group.x, group.y, 0), "the move is reported to the program");
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;3HListed \x1b[1m1\x1b[22m directory\x1b[K");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  shell.Render(canvas.renderer(), 1280, 720);
  Expect(WorkspaceShellTestAccess::CachedCursorIsPointer(shell),
         "the program lit the group under the still pointer: the window shows the hand");
  const SDL_FPoint digit = WorkspaceShellTestAccess::TerminalCellPoint(shell, 1, 9);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, digit.x, digit.y),
         "the whole lit run is the control, its bold digit included");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsDefault(shell, plain.x, plain.y),
         "text the program did not light keeps the arrow");

  SDL_Event shift{};
  shift.type = SDL_EVENT_KEY_DOWN;
  shift.key.key = SDLK_LSHIFT;
  shift.key.mod = SDL_KMOD_LSHIFT;
  (void)shell.HandleEvent(shift);
  Expect(WorkspaceShellTestAccess::CachedCursorIsText(shell),
         "Shift down over the program shows the I-beam at once: Shift selects");
  shift.type = SDL_EVENT_KEY_UP;
  shift.key.mod = SDL_KMOD_NONE;
  (void)shell.HandleEvent(shift);
  Expect(WorkspaceShellTestAccess::CachedCursorIsPointer(shell), "and letting go gives the hand back");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]22;wait\x07");
  Expect(WorkspaceShellTestAccess::CursorKindAtIs(shell, plain.x, plain.y, microide::workspace::CursorKind::Wait) &&
             WorkspaceShellTestAccess::CursorKindAtIs(shell, digit.x, digit.y, microide::workspace::CursorKind::Wait),
         "the program's own pointer, everywhere on the pane");
  shift.type = SDL_EVENT_KEY_DOWN;
  shift.key.mod = SDL_KMOD_LSHIFT;
  (void)shell.HandleEvent(shift);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsText(shell, plain.x, plain.y), "Shift still selects");
}

void TestWorkspaceShellTerminalShiftDragSelectsPastAMouseOwner() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, temp_dir.path());
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1000h\x1b[?1006hselect me");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  const SDL_FPoint start = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  const SDL_FPoint end = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 6);

  SDL_Event shift{};
  shift.type = SDL_EVENT_KEY_DOWN;
  shift.key.key = SDLK_LSHIFT;
  shift.key.mod = SDL_KMOD_LSHIFT;
  (void)shell.HandleEvent(shift);
  Expect(SendMouseDown(shell, start.x, start.y, SDL_BUTTON_LEFT), "Shift+press starts a selection");
  Expect(SendMouseMotion(shell, end.x, end.y, SDL_BUTTON_LMASK), "the drag extends it");
  shift.type = SDL_EVENT_KEY_UP;
  shift.key.mod = SDL_KMOD_NONE;
  (void)shell.HandleEvent(shift);
  Expect(SendMouseUp(shell, end.x, end.y, SDL_BUTTON_LEFT), "the release ends it");
  Expect(WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell) == "select",
         "Shift+drag selected the text: [" + WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell) + "]");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "and the program was sent no mouse report for it: [" + TerminalSessionTestAccess::SentBytes(session) + "]");

  Expect(SendMouseDown(shell, start.x, start.y, SDL_BUTTON_LEFT), "a plain press");
  Expect(TerminalSessionTestAccess::SentBytes(session).find("\x1b[<0;1;1M") != std::string::npos,
         "without Shift the press is the program's: [" + TerminalSessionTestAccess::SentBytes(session) + "]");
}

void TestWorkspaceShellTerminalLinkUnderlinesOnHover() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, temp_dir.path());
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(
      session, "go to https://example.com/x now\r\nsee \x1b]8;;https://example.com/guide\x1b\\the docs\x1b]8;;\x1b\\ ok");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FPoint url = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 12);
  (void)SendMouseMotion(shell, url.x, url.y, 0);
  auto hovered = WorkspaceShellTestAccess::TerminalHoveredLink(shell);
  Expect(hovered.has_value() && hovered->row == 0 && hovered->begin_column == 6 && hovered->end_column == 27,
         "the URL under the pointer is underlined end to end");
  const SDL_FPoint docs = WorkspaceShellTestAccess::TerminalCellPoint(shell, 1, 9);
  (void)SendMouseMotion(shell, docs.x, docs.y, 0);
  hovered = WorkspaceShellTestAccess::TerminalHoveredLink(shell);
  Expect(hovered.has_value() && hovered->row == 1 && hovered->begin_column == 4 && hovered->end_column == 12,
         "an OSC 8 link's run of cells, not its target's length");
  const SDL_FPoint plain = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 1);
  (void)SendMouseMotion(shell, plain.x, plain.y, 0);
  Expect(!WorkspaceShellTestAccess::TerminalHoveredLink(shell).has_value(), "and none off a link");
}

void TestWorkspaceShellTerminalLinkUnderlineTakesTheLinksColour() {
  EnsureDummySdlVideo();
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, temp_dir.path());
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "see \x1b[34mhttps://example.com/x\x1b[0m now");
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const SDL_FPoint first = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 4);
  const SDL_FPoint last = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 24);
  const SDL_FPoint below = WorkspaceShellTestAccess::TerminalCellPoint(shell, 1, 4);
  const int x0 = static_cast<int>(first.x);
  const int x1 = static_cast<int>(last.x);
  const int y0 = static_cast<int>(first.y);
  const int y1 = static_cast<int>(below.y);
  std::vector<SDL_Color> before;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      before.push_back(canvas.PixelAt(x, y));
    }
  }
  (void)SendMouseMotion(shell, first.x + 30.0f, first.y, 0);
  Expect(WorkspaceShellTestAccess::TerminalHoveredLink(shell).has_value(), "the link is hovered");
  shell.Render(canvas.renderer(), kWidth, kHeight);
  const util::Rgba8 blue = util::BasicAnsiColor(4, false);
  std::size_t changed = 0;
  std::size_t index = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x, ++index) {
      const SDL_Color now = canvas.PixelAt(x, y);
      const SDL_Color was = before[index];
      if (now.r == was.r && now.g == was.g && now.b == was.b) {
        continue;
      }
      ++changed;
      if (now.r != blue.r || now.g != blue.g || now.b != blue.b) {
        Expect(false, "an underline pixel is not the link's blue at " + std::to_string(x) + "," +
                          std::to_string(y));
        return;
      }
    }
  }
  Expect(changed > 20, "the hover drew an underline (" + std::to_string(changed) + " pixels)");
}

void TestWorkspaceShellTerminalWrappedLinkIsOneLink() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  WriteFile(root / "src" / "main.cpp", "int x;\nint y;\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  const auto expect_span = [&](const std::optional<workspace::TerminalLinkSpan>& span, std::size_t row,
                               std::size_t begin, std::size_t end_row, std::size_t end,
                               const std::string& what) {
    Expect(span.has_value() && span->row == row && span->begin_column == begin &&
               span->end_row == end_row && span->end_column == end,
           what);
  };

  TerminalSessionTestAccess::Reset(session, 24, 20);
  TerminalSessionTestAccess::AppendOutput(session, "see https://example.com/abc/def end");
  for (const auto& [row, column] : {std::pair<std::size_t, std::size_t>{0, 6}, {1, 3}}) {
    const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, row, column);
    const auto link = WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, point.x, point.y);
    Expect(link.has_value() && link->target == "https://example.com/abc/def",
           "the wrapped URL resolves whole from row " + std::to_string(row));
    (void)SendMouseMotion(shell, point.x, point.y, 0);
    expect_span(WorkspaceShellTestAccess::TerminalHoveredLink(shell), 0, 4, 1, 11,
                "the underline runs from the first row onto the wrapped one");
  }
  const SDL_FPoint after = WorkspaceShellTestAccess::TerminalCellPoint(shell, 1, 13);
  Expect(!WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, after.x, after.y).has_value(),
         "the word after the wrapped URL is not part of it");

  TerminalSessionTestAccess::Reset(session, 24, 10);
  TerminalSessionTestAccess::AppendOutput(session, "at src/main.cpp:2 x");
  const SDL_FPoint file = WorkspaceShellTestAccess::TerminalCellPoint(shell, 1, 2);
  const auto file_link = WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, file.x, file.y);
  Expect(file_link.has_value() &&
             file_link->target == "file://" + (root / "src" / "main.cpp").lexically_normal().generic_string() + "#L2",
         "the wrapped file reference resolves whole");
  expect_span(file_link.has_value() ? std::optional(file_link->span) : std::nullopt, 0, 3, 1, 7,
              "and spans the wrap");

  TerminalSessionTestAccess::Reset(session, 24, 20);
  TerminalSessionTestAccess::AppendOutput(
      session, "\x1b]8;;https://example.com/g\x1b\\abcdefghijklmnopqrstuvwxy\x1b]8;;\x1b\\ z");
  const SDL_FPoint osc = WorkspaceShellTestAccess::TerminalCellPoint(shell, 1, 1);
  const auto osc_link = WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, osc.x, osc.y);
  Expect(osc_link.has_value() && osc_link->target == "https://example.com/g", "the OSC 8 run resolves");
  expect_span(osc_link.has_value() ? std::optional(osc_link->span) : std::nullopt, 0, 0, 1, 5,
              "the OSC 8 run's span crosses the wrap");
}

void TestWorkspaceShellTerminalWrappedLinkPastTheWindowIsWhole() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const std::string url = "https://example.com/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  TerminalSessionTestAccess::Reset(session, 24, 20);
  std::string output = url + "\r\n";
  for (int line = 0; line < 60; ++line) {
    output += "line\r\n";
  }
  TerminalSessionTestAccess::AppendOutput(session, output);
  WorkspaceShellTestAccess::SetActiveTerminalScrollRow(shell, 1);
  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, false);
  const SDL_FPoint below_top = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 3);
  const auto whole = WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, below_top.x, below_top.y);
  Expect(whole.has_value() && whole->target == url,
         "a link whose first row is above the window resolves whole");
  Expect(whole.has_value() && whole->span.row == 0 && whole->span.begin_column == 0 &&
             whole->span.end_row == 2 && whole->span.end_column == 10,
         "and its span starts above the window");

  const std::size_t visible = WorkspaceShellTestAccess::TerminalVisibleRowCount(shell);
  TerminalSessionTestAccess::Reset(session, 24, 20);
  output.clear();
  for (std::size_t line = 0; line + 2 < visible; ++line) {
    output += "line\r\n";
  }
  output += url + "\r\n";
  for (int line = 0; line < 60; ++line) {
    output += "line\r\n";
  }
  TerminalSessionTestAccess::AppendOutput(session, output);
  WorkspaceShellTestAccess::SetActiveTerminalScrollRow(shell, 0);
  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, false);
  const SDL_FPoint at_bottom = WorkspaceShellTestAccess::TerminalCellPoint(shell, visible - 2, 3);
  const auto tail = WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, at_bottom.x, at_bottom.y);
  Expect(tail.has_value() && tail->target == url,
         "a link whose last row is below the window resolves whole");
}

void TestWorkspaceShellTerminalOsc8LinkClickOpensItsTarget() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path source = temp_dir.path() / "notes.txt";
  WriteFile(source, "one\ntwo\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, temp_dir.path());
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(
      session, "see \x1b]8;;https://example.com/guide\x1b\\the docs\x1b]8;;\x1b\\ or \x1b]8;;file://" +
                   source.string() + "\x1b\\notes\x1b]8;;\x1b\\");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string opened_url;
  WorkspaceShellTestAccess::SetExternalUrlOpener(shell, [&](std::string_view url) {
    opened_url = std::string(url);
    return true;
  });
  const SDL_FPoint plain = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 1);
  Expect(!WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, plain.x, plain.y).has_value(),
         "text outside the link is not a link");
  const SDL_FPoint docs = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 6);
  Expect(SendMouseDown(shell, docs.x, docs.y, SDL_BUTTON_LEFT), "clicking the link text is handled");
  Expect(opened_url == "https://example.com/guide", "the link's target opens, not its text");

  opened_url.clear();
  const SDL_FPoint notes = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 18);
  Expect(SendMouseDown(shell, notes.x, notes.y, SDL_BUTTON_LEFT), "clicking the file link is handled");
  Expect(opened_url.empty(), "a file link is not handed to the external opener");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path().lexically_normal() ==
             source.lexically_normal(),
         "the file link opens in the editor");
}

void TestWorkspaceShellTerminalUrlHitTestHonorsMultibytePrefix() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\xc3\xa9 https://example.com/x");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string opened_url;
  WorkspaceShellTestAccess::SetExternalUrlOpener(shell, [&](std::string_view url) {
    opened_url = std::string(url);
    return true;
  });

  const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 2);
  Expect(SendMouseDown(shell, point.x, point.y, SDL_BUTTON_LEFT),
         "left-clicking a terminal URL after a multibyte glyph should be handled");
  Expect(opened_url == "https://example.com/x",
         "URL hit-testing must map the grid column to a byte offset before matching");
}

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

  const SDL_FPoint second = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 42);
  Expect(SendMouseDown(shell, second.x, second.y, SDL_BUTTON_LEFT), "clicking the second URL is handled");
  Expect(opened_url == "https://example.com/x",
         "the ) that closes the parenthesised link is stripped: " + opened_url);
}

void TestWorkspaceShellTerminalUrlHoverUsesRenderedSnapshot() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "see https://example.com/x done");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);

  const SDL_FPoint url_point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 4);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, url_point.x, url_point.y),
         "hovering a terminal URL cell resolves to the Pointer cursor via the cached snapshot");
  const SDL_FPoint text_point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsText(shell, text_point.x, text_point.y),
         "hovering a non-URL terminal cell resolves to the Text cursor");
}

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

  const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 3);
  Expect(SendMouseDown(shell, point.x, point.y, SDL_BUTTON_LEFT),
         "left-clicking an uppercase-scheme terminal URL should be handled");
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

void TestWorkspaceShellExitedTerminalTabIsRetained() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);
  TerminalSessionTestAccess::AppendOutput(session, "hi\r\n");

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

  microide::terminal::TerminalLine line;
  line.cells = {MakeAsciiCell('A'), MakeEmptyCell(), MakeEmptyCell(), MakeAsciiCell('B')};
  const std::vector<microide::terminal::TerminalLine> lines = {line};

  const std::string whole_line =
      TerminalLineSliceText(line, 0, line.cells.size(), false);
  Expect(whole_line == "A  B", "whole-line copy should render empty cells as spaces");

  const TerminalSelectionBounds selection{
      .start = TerminalSelectionPoint{.row = 0, .column = 0},
      .end = TerminalSelectionPoint{.row = 0, .column = line.cells.size()},
  };
  Expect(ExtractTerminalSelectionText(lines, selection) == whole_line,
         "selection copy should preserve internal spaces exactly like whole-line copy");
}

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
      padded_row("cd  ", 8, false),
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

  const TerminalSelectionBounds into_padding{
      .start = TerminalSelectionPoint{.row = 0, .column = 0},
      .end = TerminalSelectionPoint{.row = 0, .column = 6},
  };
  Expect(ExtractTerminalSelectionText(lines, into_padding) == "ab",
         "a selection that ends in the padding copies only the glyphs");
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

  microide::terminal::TerminalCell lead;
  lead.SetUtf8("\xE4\xB8\xAD");
  microide::terminal::TerminalCell trailing;
  trailing.SetAscii(' ');
  trailing.style.set(microide::terminal::cell_attr::kWideTrailing, true);

  microide::terminal::TerminalLine line;
  line.cells = {lead, trailing, MakeAsciiCell('X')};
  const std::vector<microide::terminal::TerminalLine> lines = {line};

  const std::string whole_line =
      TerminalLineSliceText(line, 0, line.cells.size(), false);
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

}


void TestWorkspaceShellTerminalCtrlFOpensFindBar() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "alpha needle beta\r\n");

  Expect(!WorkspaceShellTestAccess::TerminalFind(shell).visible(),
         "the find bar should start hidden");
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+F should be handled while the terminal has focus");
  auto& find = WorkspaceShellTestAccess::TerminalFind(shell);
  Expect(find.visible() && find.focused(),
         "Ctrl+F should open the find bar and give it the keyboard");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "Ctrl+F must not also reach the shell as a control byte");
}

void TestWorkspaceShellTerminalFindSelectsNewestMatchFirst() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  for (int index = 0; index < 5; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle line\r\n");
  }

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+F should open the find bar");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "needle"),
         "typing should route into the find query, not the terminal");
  auto& find = WorkspaceShellTestAccess::TerminalFind(shell);
  Expect(find.matches().size() == 5, "every matching row should be found");
  Expect(find.selected_index() == 4, "a fresh query should select the newest hit");
  Expect(find.count_text() == "5/5", "the counter should read selected-of-total");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "the query text must never reach the PTY");
}

void TestWorkspaceShellTerminalFindNavigationWrapsAndReveals() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 4, 80);
  for (int index = 0; index < 40; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle line\r\n");
  }
  WorkspaceShellTestAccess::SetActiveTerminalFollowTail(shell, true);

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+F should open the find bar");
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

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+F should open the find bar");
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

void TestWorkspaceShellTerminalFindEscapeReturnsKeyboardToTerminal() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "needle\r\n");

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+F should open the find bar");
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

void TestWorkspaceShellTerminalFindTracksStreamingOutput() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 4, 80);
  for (int index = 0; index < 30; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle line\r\n");
  }

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+F should open the find bar");
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

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+F should open the find bar");
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

void TestWorkspaceShellOutputReferenceClickUsesCharacterColumns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "\xc3\xa9=1\nsecond\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowOutputChannelWithEntries(shell, "lsp.references",
                                                         {"notes.txt:1:3"});
  const SDL_FPoint row = WorkspaceShellTestAccess::OutputPanelRowPoint(shell, 0);
  Expect(SendMouseDown(shell, row.x, row.y, SDL_BUTTON_LEFT),
         "clicking the reference row is handled");
  const auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.path().lexically_normal() == source.lexically_normal(),
         "the referenced file opens: " + editor.path().string());
  Expect(editor.cursor_line() == 0 && editor.cursor_column() == 3,
         "column 3 is the \"1\": byte offset 3 after the two-byte letter, not byte 2");
}

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

  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-close"),
         "term-close should reject when there is no terminal");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, WorkspaceShell::ActionId::TermClose),
         "Close Terminal should be disabled with no terminal open");
}

namespace {

struct SplitTerminalFixture {
  TemporaryDirectory temp_dir;
  std::filesystem::path root;
  WorkspaceShell shell;
  SoftwareCanvas canvas{1280, 720};

  SplitTerminalFixture() : root(temp_dir.path() / "project") {
    EnsureDummySdlVideo();
    WriteFile(root / "README.md", "root\n");
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::EnsureTerminalTab(shell);
    TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 24, 80);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  }

  void PaintFrame() { WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer()); }
};

void TestWorkspaceShellTerminalPaneFocusShowsAHiddenPanel() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  fixture.PaintFrame();
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalSplit, {}),
         "the fixture splits its terminal");
  WorkspaceShellTestAccess::HideBottomPanel(shell);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  Expect(!WorkspaceShellTestAccess::BottomPanelVisible(shell), "the panel is hidden");
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalFocusPaneLeft, {}),
         "there is a pane to the left");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 0, "the left pane is active");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell) && WorkspaceShellTestAccess::BottomPanelVisible(shell) &&
             WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "and the panel that now has the keyboard shows it");
}

void TestWorkspaceShellTerminalSplitAddsPaneRightOfTheActiveOne() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  fixture.PaintFrame();
  const std::size_t full_columns = WorkspaceShellTestAccess::ActiveTerminalSession(shell).columns();
  Expect(full_columns > 0, "the single pane is gridded to the panel body");
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalSplit, {}),
         "Split Terminal should be accepted with a terminal open");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1,
         "a split adds a pane to the tab, not a tab to the strip");
  Expect(WorkspaceShellTestAccess::TerminalPaneCount(shell) == 2,
         "the tab holds two panes after one split");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 1,
         "the new pane (to the right) takes the keyboard");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "splitting shows the terminal content");
  const std::vector<float> weights = WorkspaceShellTestAccess::TerminalPaneWeights(shell);
  Expect(weights.size() == 2 && std::abs(weights[0] - 0.5f) < 1e-5f, "the split pane is halved");

  const SDL_FRect body = WorkspaceShellTestAccess::BottomPanelContentRect(shell);
  const auto rects = WorkspaceShellTestAccess::TerminalPaneRects(shell);
  Expect(rects.panes.size() == 2 && rects.dividers.size() == 1, "two pane rects and one divider");
  Expect(rects.panes[0].x == body.x && rects.panes[1].x > rects.panes[0].x &&
             std::abs(rects.panes[1].x + rects.panes[1].w - (body.x + body.w)) < 1e-3f,
         "the panes tile the panel body left to right");

  TerminalSessionTestAccess::ClearSentBytes(WorkspaceShellTestAccess::TerminalPaneSession(shell, 0));
  TerminalSessionTestAccess::ClearSentBytes(WorkspaceShellTestAccess::TerminalPaneSession(shell, 1));
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter is handled by the terminal");
  Expect(TerminalSessionTestAccess::SentBytes(WorkspaceShellTestAccess::TerminalPaneSession(shell, 1)) ==
             "\r",
         "the active (new) pane receives the key");
  Expect(TerminalSessionTestAccess::SentBytes(WorkspaceShellTestAccess::TerminalPaneSession(shell, 0))
             .empty(),
         "the other pane receives nothing");

  fixture.PaintFrame();
  const std::size_t left_columns = WorkspaceShellTestAccess::TerminalPaneSession(shell, 0).columns();
  const std::size_t right_columns = WorkspaceShellTestAccess::TerminalPaneSession(shell, 1).columns();
  Expect(left_columns > 0 && right_columns > 0, "both panes get a grid");
  Expect(left_columns < full_columns && right_columns < full_columns &&
             left_columns + right_columns <= full_columns,
         "each pane's grid is a slice of the body the lone pane had");
  Expect(WorkspaceShellTestAccess::TerminalPaneSession(shell, 0).rows() ==
             WorkspaceShellTestAccess::TerminalPaneSession(shell, 1).rows(),
         "side-by-side panes share the panel's row count");
}

void TestWorkspaceShellTerminalPaneClickActivatesAndWheelScrollsUnderThePointer() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalSplit, {}),
         "split");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 1, "new pane active");
  const auto rects = WorkspaceShellTestAccess::TerminalPaneRects(shell);
  const SDL_FRect left = rects.panes[0];
  const SDL_FRect right = rects.panes[1];

  Expect(SendMouseDown(shell, left.x + left.w * 0.5f, left.y + left.h * 0.5f, SDL_BUTTON_LEFT),
         "a press in the left pane is handled");
  SendMouseUp(shell, left.x + left.w * 0.5f, left.y + left.h * 0.5f, SDL_BUTTON_LEFT);
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 0,
         "pressing in another pane makes it the active one");

  auto& right_session = WorkspaceShellTestAccess::TerminalPaneSession(shell, 1);
  TerminalSessionTestAccess::Reset(right_session, 8, 40);
  for (int i = 0; i < 60; ++i) {
    TerminalSessionTestAccess::AppendOutput(right_session, "line\r\n");
  }
  Expect(WorkspaceShellTestAccess::TerminalPaneFollowTail(shell, 1), "the pane follows its tail");
  const bool left_follow_before = WorkspaceShellTestAccess::TerminalPaneFollowTail(shell, 0);
  const int left_scroll_before = WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell);
  Expect(SendMouseWheel(shell, right.x + right.w * 0.5f, right.y + right.h * 0.5f, 1),
         "a wheel over the inactive pane is handled");
  Expect(!WorkspaceShellTestAccess::TerminalPaneFollowTail(shell, 1),
         "the wheel scrolls the pane under the pointer, active or not");
  Expect(WorkspaceShellTestAccess::TerminalPaneFollowTail(shell, 0) == left_follow_before &&
             WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell) == left_scroll_before,
         "the active pane did not scroll");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 0,
         "scrolling is not focusing");
}

void TestWorkspaceShellTerminalInactivePaneLinkHoversAndRightClickActivates() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalSplit, {}),
         "split");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 1, "new pane active");
  auto& left_session = WorkspaceShellTestAccess::TerminalPaneSession(shell, 0);
  TerminalSessionTestAccess::Reset(left_session, 24, 60);
  TerminalSessionTestAccess::AppendOutput(left_session, "go to https://example.com/x now");
  auto& right_session = WorkspaceShellTestAccess::TerminalPaneSession(shell, 1);
  TerminalSessionTestAccess::Reset(right_session, 24, 60);
  TerminalSessionTestAccess::AppendOutput(right_session, "plain text only");

  const SDL_FPoint url = WorkspaceShellTestAccess::TerminalPaneCellPoint(shell, 0, 0, 12);
  const auto link = WorkspaceShellTestAccess::TerminalLinkAtPoint(shell, url.x, url.y);
  Expect(link.has_value() && link->pane == 0 && link->target == "https://example.com/x",
         "the link in the inactive pane resolves from that pane's lines");
  (void)SendMouseMotion(shell, url.x, url.y, 0);
  const auto hovered = WorkspaceShellTestAccess::TerminalPaneHoveredLink(shell, 0);
  Expect(hovered.has_value() && hovered->begin_column == 6 && hovered->end_column == 27,
         "the inactive pane underlines its link");
  Expect(!WorkspaceShellTestAccess::TerminalPaneHoveredLink(shell, 1).has_value(),
         "the active pane underlines nothing");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 1, "hovering is not focusing");

  const SDL_FPoint plain = WorkspaceShellTestAccess::TerminalPaneCellPoint(shell, 1, 0, 2);
  (void)SendMouseMotion(shell, plain.x, plain.y, 0);
  Expect(!WorkspaceShellTestAccess::TerminalPaneHoveredLink(shell, 0).has_value(),
         "moving to another pane clears the underline");

  Expect(SendMouseDown(shell, url.x, url.y, SDL_BUTTON_RIGHT), "a right-click is handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 0,
         "a right-click activates the pane it lands on");
}

void TestWorkspaceShellTerminalPaneDividerDragResizesAndDoubleClickResets() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalSplit, {}),
         "split");
  const auto rects = WorkspaceShellTestAccess::TerminalPaneRects(shell);
  const SDL_FRect divider = rects.dividers[0].rect;
  const float grab_x = divider.x + divider.w * 0.5f;
  const float grab_y = divider.y + divider.h * 0.5f;

  Expect(SendMouseDown(shell, grab_x, grab_y, SDL_BUTTON_LEFT), "a press on the divider starts a drag");
  Expect(SendMouseMotion(shell, grab_x - 150.0f, grab_y, SDL_BUTTON_LMASK), "dragging is handled");
  Expect(SendMouseUp(shell, grab_x - 150.0f, grab_y, SDL_BUTTON_LEFT), "release ends the drag");
  const std::vector<float> dragged = WorkspaceShellTestAccess::TerminalPaneWeights(shell);
  Expect(dragged[0] < 0.5f - 0.05f && dragged[1] > 0.5f + 0.05f,
         "dragging the divider left shrinks the left pane and grows the right one");
  Expect(std::abs(dragged[0] + dragged[1] - 1.0f) < 1e-4f, "the shares still sum to one");
  const auto moved = WorkspaceShellTestAccess::TerminalPaneRects(shell);
  Expect(moved.dividers[0].rect.x < divider.x, "the divider rect follows the drag");

  const SDL_FRect moved_divider = moved.dividers[0].rect;
  Expect(SendMouseDown(shell, moved_divider.x + moved_divider.w * 0.5f,
                       moved_divider.y + moved_divider.h * 0.5f, SDL_BUTTON_LEFT, 2),
         "a double-click on the divider is handled");
  const std::vector<float> reset = WorkspaceShellTestAccess::TerminalPaneWeights(shell);
  Expect(std::abs(reset[0] - 0.5f) < 1e-5f, "double-click restores the even split");
}

void TestWorkspaceShellTerminalCloseTerminalClosesThePaneThenTheTab() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalSplit, {}),
         "split");
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TermClose, {}),
         "Close Terminal on a split tab is accepted");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1 &&
             WorkspaceShellTestAccess::TerminalPaneCount(shell) == 1,
         "closing the active pane keeps the tab with its other pane");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 0, "the survivor is active");
  Expect(WorkspaceShellTestAccess::TerminalPaneWeights(shell).size() == 1 &&
             std::abs(WorkspaceShellTestAccess::TerminalPaneWeights(shell)[0] - 1.0f) < 1e-5f,
         "the survivor owns the whole body");
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TermClose, {}),
         "Close Terminal on the last pane is accepted");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 0, "the tab goes with its last pane");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::None,
         "the panel hides with the last terminal");
}

void TestWorkspaceShellTerminalPaneFocusKeysFallThroughWithOnePane() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  constexpr auto kCtrlShift = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
  auto& only = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::ClearSentBytes(only);
  Expect(SendKeyDown(shell, SDLK_LEFT, kCtrlShift), "Ctrl+Shift+Left is handled");
  Expect(!TerminalSessionTestAccess::SentBytes(only).empty(),
         "with a single pane Ctrl+Shift+Left reaches the shell as an escape sequence");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalSplit, {}),
         "split");
  auto& left_pane = WorkspaceShellTestAccess::TerminalPaneSession(shell, 0);
  auto& right_pane = WorkspaceShellTestAccess::TerminalPaneSession(shell, 1);
  TerminalSessionTestAccess::ClearSentBytes(left_pane);
  TerminalSessionTestAccess::ClearSentBytes(right_pane);
  Expect(SendKeyDown(shell, SDLK_LEFT, SDL_KMOD_ALT), "Alt+Left is handled with two panes");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 1 &&
             TerminalSessionTestAccess::SentBytes(right_pane) == "\x1b[1;3D",
         "Alt+Left is the program's, not a pane move: [" + TerminalSessionTestAccess::SentBytes(right_pane) + "]");
  TerminalSessionTestAccess::ClearSentBytes(right_pane);
  Expect(SendKeyDown(shell, SDLK_LEFT, kCtrlShift), "Ctrl+Shift+Left is handled with two panes");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 0,
         "with two panes Ctrl+Shift+Left moves the keyboard to the left pane");
  Expect(TerminalSessionTestAccess::SentBytes(left_pane).empty() &&
             TerminalSessionTestAccess::SentBytes(right_pane).empty(),
         "the chord is consumed by the pane move and reaches no shell");
  Expect(SendKeyDown(shell, SDLK_LEFT, kCtrlShift), "Ctrl+Shift+Left at the leftmost pane is handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 0, "no wrap-around");
  Expect(!TerminalSessionTestAccess::SentBytes(left_pane).empty(),
         "at the edge the chord falls through to the active pane's shell");
  Expect(SendKeyDown(shell, SDLK_RIGHT, kCtrlShift), "Ctrl+Shift+Right is handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneIndex(shell) == 1, "Ctrl+Shift+Right moves right");
}

void TestWorkspaceShellPanelMaximizeTakesTheWindowAndRestoresOnEditorFocus() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  WorkspaceShellTestAccess::OpenFile(shell, fixture.root / "README.md");
  WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalToggle, {});
  const microide::workspace::WorkspaceLayout before = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(before.editor_area.h > 0.0f, "the editor column is laid out before maximizing");
  Expect(WorkspaceShellTestAccess::GroupEditorTabRect(shell, 0, 0).w > 0.0f,
         "the open file has a tab in the regular layout");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::PanelToggleMaximized,
                                                 {}),
         "Maximize Panel is accepted");
  Expect(WorkspaceShellTestAccess::PanelMaximized(shell), "the panel is maximized");
  const microide::workspace::WorkspaceLayout maximized = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(maximized.panel_maximized && maximized.editor_area.h == 0.0f && maximized.sidebar.h == 0.0f &&
             maximized.status_bar.h == 0.0f,
         "only the menu bar and project strip remain around the panel");
  Expect(maximized.bottom_panel.y == maximized.project_tab_strip.y + maximized.project_tab_strip.h &&
             maximized.bottom_panel.y + maximized.bottom_panel.h == before.full.h,
         "the panel fills everything below the project strip");
  Expect(WorkspaceShellTestAccess::BottomPanelHeight(shell) == 156.0f,
         "the stored panel height is untouched, so restoring is exact");
  const SDL_FRect maximized_tab = WorkspaceShellTestAccess::GroupEditorTabRect(shell, 0, 0);
  Expect(maximized_tab.w == 0.0f && maximized_tab.h == 0.0f,
         "a maximized panel leaves the editor strip with no tabs to paint or hit");
  fixture.PaintFrame();
  Expect(WorkspaceShellTestAccess::PanelMaximized(shell),
         "a frame with the panel focused keeps it maximized");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::PanelToggleMaximized,
                                                 {}),
         "Restore is accepted");
  Expect(!WorkspaceShellTestAccess::PanelMaximized(shell) &&
             WorkspaceShellTestAccess::CurrentLayout(shell) == before,
         "restoring returns the exact previous layout");

  WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::PanelToggleMaximized, {});
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  fixture.PaintFrame();
  Expect(WorkspaceShellTestAccess::PanelMaximized(shell) && WorkspaceShellTestAccess::FocusIsPanel(shell),
         "focus that arrived with the state is a baseline: still maximized, the keyboard on the panel");

  WorkspaceShellTestAccess::SetFocusEditor(shell);
  fixture.PaintFrame();
  Expect(!WorkspaceShellTestAccess::PanelMaximized(shell),
         "editor focus restores the maximized panel on the next frame");
  Expect(WorkspaceShellTestAccess::CurrentLayout(shell).editor_area.h > 0.0f,
         "the editor column is back");

  WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::PanelToggleMaximized, {});
  fixture.PaintFrame();
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "command-palette"), "the palette opens");
  fixture.PaintFrame();
  Expect(WorkspaceShellTestAccess::PanelMaximized(shell),
         "a centered quick-open modal leaves the panel maximized");
  (void)SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "search"), "Find opens");
  fixture.PaintFrame();
  Expect(!WorkspaceShellTestAccess::PanelMaximized(shell) &&
             WorkspaceShellTestAccess::CurrentLayout(shell).editor_surface.h > 0.0f,
         "the editor's find widget restores the panel, so it is drawn over its editor");
  (void)SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE);

  const SDL_FRect button = WorkspaceShellTestAccess::BottomPanelMaximizeButtonRect(shell);
  Expect(SendMouseDown(shell, button.x + button.w * 0.5f, button.y + button.h * 0.5f, SDL_BUTTON_LEFT),
         "the header button takes the press");
  SendMouseUp(shell, button.x + button.w * 0.5f, button.y + button.h * 0.5f, SDL_BUTTON_LEFT);
  Expect(WorkspaceShellTestAccess::PanelMaximized(shell), "the header button maximizes");
  const SDL_FRect restore = WorkspaceShellTestAccess::BottomPanelMaximizeButtonRect(shell);
  Expect(SendMouseDown(shell, restore.x + restore.w * 0.5f, restore.y + restore.h * 0.5f, SDL_BUTTON_LEFT),
         "the header button takes the press in the maximized layout too");
  SendMouseUp(shell, restore.x + restore.w * 0.5f, restore.y + restore.h * 0.5f, SDL_BUTTON_LEFT);
  Expect(!WorkspaceShellTestAccess::PanelMaximized(shell), "the header button restores");
}

void TestWorkspaceShellTerminalToggleAndPanelToggleKeys() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "fixture shows a terminal");

  Expect(SendKeyDown(shell, SDLK_GRAVE, SDL_KMOD_CTRL), "Ctrl+` is handled");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::None,
         "Ctrl+` on a focused terminal hides the panel");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1, "the terminal survives hidden");
  Expect(SendKeyDown(shell, SDLK_GRAVE, SDL_KMOD_CTRL), "Ctrl+` is handled again");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal &&
             WorkspaceShellTestAccess::TerminalTabCount(shell) == 1,
         "Ctrl+` shows the existing terminal instead of opening another");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  Expect(SendKeyDown(shell, SDLK_GRAVE, SDL_KMOD_CTRL), "Ctrl+` from the editor is handled");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "Ctrl+` from the editor keeps the panel up and focuses the terminal");
  TerminalSessionTestAccess::ClearSentBytes(WorkspaceShellTestAccess::ActiveTerminalSession(shell));
  SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE);
  Expect(TerminalSessionTestAccess::SentBytes(WorkspaceShellTestAccess::ActiveTerminalSession(shell)) ==
             "\r",
         "the terminal has the keyboard after Ctrl+`");

  TerminalSessionTestAccess::ClearSentBytes(WorkspaceShellTestAccess::ActiveTerminalSession(shell));
  Expect(SendKeyDown(shell, SDLK_J, SDL_KMOD_CTRL), "Ctrl+J in the terminal is handled");
  Expect(TerminalSessionTestAccess::SentBytes(WorkspaceShellTestAccess::ActiveTerminalSession(shell)) == "\n",
         "Ctrl+J reaches the program as LF");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "and the panel stays up");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  Expect(SendKeyDown(shell, SDLK_J, SDL_KMOD_CTRL), "Ctrl+J is handled");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::None,
         "Ctrl+J hides the panel");
  Expect(SendKeyDown(shell, SDLK_J, SDL_KMOD_CTRL), "Ctrl+J is handled again");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "Ctrl+J brings the terminal content back");
  WorkspaceShellTestAccess::SetFocusPanel(shell);

  Expect(SendKeyDown(shell, SDLK_GRAVE, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
         "Ctrl+Shift+` is handled");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 2, "Ctrl+Shift+` opens a terminal");

  Expect(WorkspaceShellTestAccess::ActiveTerminalTabIndex(shell) == 1, "the new tab is active");
  Expect(SendKeyDown(shell, SDLK_PAGEDOWN, SDL_KMOD_CTRL), "Ctrl+PageDown is handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalTabIndex(shell) == 0, "next wraps to the first tab");
  Expect(SendKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_CTRL), "Ctrl+PageUp is handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalTabIndex(shell) == 1, "previous wraps to the last");
}

void TestWorkspaceShellTerminalActivityDotMarksBackgroundOutput() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 24, 80);
  Expect(WorkspaceShellTestAccess::ActiveTerminalTabIndex(shell) == 1, "second tab active");

  auto& background = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
  auto& foreground = WorkspaceShellTestAccess::TerminalTabSession(shell, 1);
  TerminalSessionTestAccess::AppendOutput(background, "done\r\n");
  TerminalSessionTestAccess::MarkOutputArrived(background);
  TerminalSessionTestAccess::AppendOutput(foreground, "typing\r\n");
  TerminalSessionTestAccess::MarkOutputArrived(foreground);
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(WorkspaceShellTestAccess::TerminalTabHasUnseenOutput(shell, 0),
         "output in a tab that is not on screen lights its activity dot");
  Expect(!WorkspaceShellTestAccess::TerminalTabHasUnseenOutput(shell, 1),
         "output in the tab on screen is seen as it arrives");

  Expect(WorkspaceShellTestAccess::ActivateTerminalTab(shell, 0), "activate the background tab");
  fixture.PaintFrame();
  Expect(!WorkspaceShellTestAccess::TerminalTabHasUnseenOutput(shell, 0),
         "the dot clears once the tab is shown");

  SendKeyDown(shell, SDLK_GRAVE, SDL_KMOD_CTRL);
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::None,
         "panel hidden");
  TerminalSessionTestAccess::MarkOutputArrived(background);
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(WorkspaceShellTestAccess::TerminalTabHasUnseenOutput(shell, 0),
         "output behind a hidden panel is unseen");
}

void TestWorkspaceShellTerminalBellMarksABackgroundTab() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 24, 80);
  auto& background = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
  auto& foreground = WorkspaceShellTestAccess::TerminalTabSession(shell, 1);
  TerminalSessionTestAccess::AppendOutput(background, "build finished\x07");
  TerminalSessionTestAccess::AppendOutput(foreground, "\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(WorkspaceShellTestAccess::TerminalTabHasUnseenBell(shell, 0),
         "a bell in a background tab marks it");
  Expect(!WorkspaceShellTestAccess::TerminalTabHasUnseenBell(shell, 1),
         "a bell in the tab on screen is heard as it rings");

  Expect(WorkspaceShellTestAccess::ActivateTerminalTab(shell, 0), "activate the background tab");
  fixture.PaintFrame();
  Expect(!WorkspaceShellTestAccess::TerminalTabHasUnseenBell(shell, 0), "showing it clears the mark");
  Expect(WorkspaceShellTestAccess::ActivateTerminalTab(shell, 1), "back to the other tab");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(!WorkspaceShellTestAccess::TerminalTabHasUnseenBell(shell, 0) &&
             !WorkspaceShellTestAccess::TerminalTabHasUnseenBell(shell, 1),
         "no stale bell resurfaces");
}

void TestWorkspaceShellTerminalNotificationToastsFromABackgroundTab() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 24, 80);
  auto& background = WorkspaceShellTestAccess::TerminalTabSession(shell, 0);
  auto& foreground = WorkspaceShellTestAccess::TerminalTabSession(shell, 1);
  TerminalSessionTestAccess::AppendOutput(background, "\x1b]9;needs your permission\x07");
  TerminalSessionTestAccess::AppendOutput(foreground, "\x1b]9;seen already\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  bool background_toast = false;
  bool foreground_toast = false;
  for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    background_toast = background_toast || row.message.find("needs your permission") != std::string::npos;
    foreground_toast = foreground_toast || row.message.find("seen already") != std::string::npos;
  }
  Expect(background_toast, "a background terminal's notification is a toast");
  Expect(!foreground_toast, "the terminal on screen does not toast");
  Expect(WorkspaceShellTestAccess::TerminalTabHasUnseenBell(shell, 0), "and marks its tab");
}

void TestWorkspaceShellTerminalAlertReachesPastAnUnfocusedWindow() {
  SplitTerminalFixture fixture;
  WorkspaceShell& shell = fixture.shell;
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  const auto& interaction = WorkspaceShellTestAccess::InteractionState_(shell);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]9;focused\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(interaction.window_flags_requested == 0 && interaction.desktop_notifications_posted == 0,
         "a focused window is not flagged");

  SDL_Event focus_lost{};
  focus_lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  Expect(shell.HandleEvent(focus_lost).handled, "focus loss is handled");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]9;needs your input\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(interaction.window_flags_requested == 1, "an unfocused window is flagged");
  Expect(interaction.desktop_notifications_posted == 1, "and the notification is posted to the desktop");

  TerminalSessionTestAccess::AppendOutput(session, "\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(interaction.window_flags_requested == 2 && interaction.desktop_notifications_posted == 1,
         "a bell flags the window and posts nothing");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]9;again at once\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(interaction.window_flags_requested == 3 && interaction.desktop_notifications_posted == 1,
         "a second notification inside the spacing flags the window and is not posted");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "terminal.alert_when_unfocused", "false"),
         "the setting is registered");
  TerminalSessionTestAccess::AppendOutput(session, "\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(interaction.window_flags_requested == 3, "off, nothing leaves the window");
}

void TestWorkspaceShellTerminalNotificationToastsFromABackgroundProject() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path alpha = temp_dir.path() / "alpha";
  const std::filesystem::path beta = temp_dir.path() / "beta";
  WriteFile(alpha / "a.txt", "a\n");
  WriteFile(beta / "b.txt", "b\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, alpha), "project alpha opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& agent = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(agent, 24, 80);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, beta), "project beta opens in front");

  TerminalSessionTestAccess::AppendOutput(agent, "\x1b]9;needs your input\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  std::string toast;
  for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    if (row.message.find("needs your input") != std::string::npos) {
      toast = row.message;
    }
  }
  Expect(!toast.empty(), "a background project's terminal notification toasts at once");
  Expect(toast.find("alpha") != std::string::npos, "and names the project it came from: " + toast);
  Expect(WorkspaceShellTestAccess::ProjectTabDisplayTitle(shell, 0) == "alpha \xe2\x80\xa2",
         "alpha's project tab is marked: " + WorkspaceShellTestAccess::ProjectTabDisplayTitle(shell, 0));
  Expect(WorkspaceShellTestAccess::ProjectTabTooltipLabel(shell, 0).find("A terminal printed or rang") !=
             std::string::npos,
         "and its tooltip says why");
  Expect(WorkspaceShellTestAccess::ProjectTabDisplayTitle(shell, 1) == "beta", "the front project is not");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0), "back to alpha");
  Expect(WorkspaceShellTestAccess::ProjectTabDisplayTitle(shell, 0) == "alpha",
         "in front, its terminal tab carries the mark instead");
  Expect(WorkspaceShellTestAccess::TerminalTabHasUnseenBell(shell, 0),
         "its terminal tab carries the mark back to the front");
}

void TestWorkspaceShellNextTerminalAlertCrossesTabsAndProjects() {
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path alpha = temp_dir.path() / "alpha";
  const std::filesystem::path beta = temp_dir.path() / "beta";
  WriteFile(alpha / "a.txt", "a\n");
  WriteFile(beta / "b.txt", "b\n");
  WorkspaceShell shell;
  SoftwareCanvas canvas{1280, 720};
  const auto paint = [&] { WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer()); };
  const auto ring = [&](std::size_t tab) {
    TerminalSessionTestAccess::AppendOutput(WorkspaceShellTestAccess::TerminalTabSession(shell, tab), "\x07");
    WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  };
  const WorkspaceShell::ActionId next = WorkspaceShell::ActionId::TerminalNextAlert;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, alpha), "project alpha opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  for (std::size_t tab = 0; tab < 3; ++tab) {
    TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::TerminalTabSession(shell, tab), 24, 80);
  }
  Expect(WorkspaceShellTestAccess::ActivateTerminalTab(shell, 1), "alpha's middle terminal is in front");
  paint();
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, next) &&
             !WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-next-alert"),
         "with nothing waiting the command is dimmed and says so");

  TerminalSessionTestAccess::AppendOutput(WorkspaceShellTestAccess::TerminalTabSession(shell, 0), "working...\r\n");
  TerminalSessionTestAccess::MarkOutputArrived(WorkspaceShellTestAccess::TerminalTabSession(shell, 0));
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(WorkspaceShellTestAccess::TerminalTabHasUnseenOutput(shell, 0) &&
             !WorkspaceShellTestAccess::IsActionEnabled(shell, next),
         "a terminal that merely printed is not waiting for anyone");

  ring(0);
  ring(2);
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, next), "two terminals rang");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-next-alert") &&
             WorkspaceShellTestAccess::ActiveTerminalTabIndex(shell) == 2,
         "the next one after the active tab comes forward");
  paint();
  Expect(!WorkspaceShellTestAccess::TerminalTabHasUnseenBell(shell, 2), "and showing it clears its mark");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "term-next-alert") &&
             WorkspaceShellTestAccess::ActiveTerminalTabIndex(shell) == 0,
         "then the walk wraps to the earlier tab");
  paint();
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, next), "and nothing is left");

  auto& agent = WorkspaceShellTestAccess::TerminalTabSession(shell, 1);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, beta), "project beta opens in front");
  TerminalSessionTestAccess::AppendOutput(agent, "\x1b]9;needs your input\x07");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 1 &&
             WorkspaceShellTestAccess::IsActionEnabled(shell, next),
         "from beta, alpha's waiting terminal is offered");
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, next, {}), "Go to Next Terminal Alert");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 0 &&
             WorkspaceShellTestAccess::ActiveTerminalTabIndex(shell) == 1 &&
             WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "alpha comes forward with that terminal shown");
  paint();
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, next), "and its mark is cleared by being seen");
}

void TestWorkspaceShellFileDroppedOnTheTerminalTypesItsPath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");
  const std::filesystem::path shot = temp_dir.path() / "Screen shot.png";
  WriteFile(shot, "png");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  const SDL_FPoint cell = WorkspaceShellTestAccess::TerminalCellPoint(shell, 2, 4);
  SDL_Event drop{};
  drop.type = SDL_EVENT_DROP_FILE;
  drop.drop.x = cell.x;
  drop.drop.y = cell.y;
  const std::string path = shot.string();
  drop.drop.data = path.c_str();
  Expect(shell.HandleEvent(drop).handled, "the drop is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "'" + path + "' ",
         "the terminal gets the quoted path: [" + TerminalSessionTestAccess::SentBytes(session) + "]");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell), "and the keyboard, to go on typing");
}

void TestWorkspaceShellImagePastedIntoTheTerminalTypesItsPath() {
  TemporaryDirectory temp_dir;
  const ScopedEnvVar state_home("XDG_STATE_HOME", (temp_dir.path() / "state").string());
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const std::string png = std::string("\x89PNG\r\n\x1a\n") + "image bytes";
  std::optional<std::string> clipboard_text;
  std::optional<std::string> clipboard_png = png;
  WorkspaceShellTestAccess::SetClipboardTextReader(shell, [&]() { return clipboard_text; });
  WorkspaceShellTestAccess::SetClipboardPngReader(shell, [&]() { return clipboard_png; });

  Expect(WorkspaceShellTestAccess::PasteClipboardIntoTerminal(shell), "the paste is handled");
  std::string typed = TerminalSessionTestAccess::SentBytes(session);
  const std::string pasted_root = (temp_dir.path() / "state" / "microide" / "pasted").string();
  Expect(typed.starts_with(pasted_root) && typed.ends_with("/pasted-image.png "),
         "the terminal gets the saved image's path: [" + typed + "]");
  if (!typed.empty()) {
    typed.pop_back();
    Expect(ReadFile(typed) == png, "and the file holds the clipboard's bytes");
  }

  TerminalSessionTestAccess::ClearSentBytes(session);
  clipboard_text = "plain text";
  Expect(WorkspaceShellTestAccess::PasteClipboardIntoTerminal(shell), "a text paste is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "plain text",
         "text on the clipboard pastes as text even beside an image: [" +
             TerminalSessionTestAccess::SentBytes(session) + "]");

  TerminalSessionTestAccess::ClearSentBytes(session);
  clipboard_text.reset();
  clipboard_png = "<html>not an image</html>";
  Expect(WorkspaceShellTestAccess::PasteClipboardIntoTerminal(shell), "an unusable clipboard is handled");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(), "and types nothing");
}

void TestWorkspaceShellReferenceNamesTheCaretOrSelectionLines() {
  using ActionId = WorkspaceShell::ActionId;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::filesystem::path outside = temp_dir.path() / "elsewhere" / "stray.cpp";
  WriteFile(source, "int main() {\n  int value = 1;\n  return value;\n}\n");
  WriteFile(outside, "int stray;\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(shell, [&](std::string_view text) {
    clipboard_text = std::string(text);
    return true;
  });

  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::TerminalInsertReference),
         "with no terminal there is nowhere to insert a reference");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(2, 3);
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, ActionId::CopyReference, {}), "Copy Reference runs");
  Expect(clipboard_text == "src/main.cpp:3", "a caret is one line: " + clipboard_text);

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 2);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(3, 0, true);
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, ActionId::CopyReference, {}), "and for a selection");
  Expect(clipboard_text == "src/main.cpp:2-3",
         "a selection ending at a line's start does not count that line: " + clipboard_text);

  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, ActionId::TerminalInsertReference, {}),
         "Terminal: Insert Reference runs");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "src/main.cpp:2-3 ",
         "the terminal gets the reference and a space: [" + TerminalSessionTestAccess::SentBytes(session) + "]");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell), "and the keyboard, to go on with the prompt");

  WorkspaceShellTestAccess::OpenFile(shell, outside);
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, ActionId::CopyReference, {}), "outside the project");
  Expect(clipboard_text == outside.string() + ":1", "the file is named absolutely: " + clipboard_text);
}

void TestWorkspaceShellTerminalRelaunchRestartsAnExitedPane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenTerminalViaService(shell);
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1, "a terminal opened");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneLaunchDirectory(shell) == root.lexically_normal(),
         "the pane records the directory it was launched in");
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  Expect(session.running(), "a (placeholder) launched session reports running");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, WorkspaceShell::ActionId::TerminalRelaunch),
         "a running terminal has nothing to relaunch, so the verb greys out");
  TerminalSessionTestAccess::ClearSentBytes(session);
  SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE);
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\r",
         "Enter in a live pane goes to the shell");

  TerminalSessionTestAccess::AppendOutput(session, "connection closed\r\n");
  TerminalSessionTestAccess::EmitProcessExitMarker(session);
  TerminalSessionTestAccess::SetRunning(session, false);
  Expect(TerminalScreenText(session).find("[process exited]") != std::string::npos, "exit marker shown");

  SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE);
  Expect(session.running(), "Enter in an exited pane relaunches it in place");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) == 1 &&
             WorkspaceShellTestAccess::TerminalPaneCount(shell) == 1,
         "relaunching reuses the pane: no new tab, no new pane");
  Expect(TerminalScreenText(session).find("[process exited]") == std::string::npos,
         "the relaunched session starts on a fresh screen");
  Expect(WorkspaceShellTestAccess::ActiveTerminalPaneLaunchDirectory(shell) == root.lexically_normal(),
         "the relaunch keeps the recorded directory");

  TerminalSessionTestAccess::SetRunning(session, false);
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::TerminalRelaunch, {}),
         "Relaunch Terminal is accepted for an exited pane");
  Expect(session.running(), "the menu verb relaunches too");
}

void TestWorkspaceShellProjectOpensWithTheKeyboardInTheEditor() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "# readme\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project should open");
  Expect(WorkspaceShellTestAccess::TerminalTabCount(shell) >= 1, "the project's terminal opens");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell), "but the keyboard starts in the editor");
}

}

void RegisterWorkspaceShellTerminalTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/TerminalSplitAddsPaneRightOfTheActiveOne",
          TestWorkspaceShellTerminalSplitAddsPaneRightOfTheActiveOne);
  AddTest(tests, "WorkspaceShell/TerminalPaneFocusShowsAHiddenPanel",
          TestWorkspaceShellTerminalPaneFocusShowsAHiddenPanel);
  AddTest(tests, "WorkspaceShell/TerminalPaneClickActivatesAndWheelScrollsUnderThePointer",
          TestWorkspaceShellTerminalPaneClickActivatesAndWheelScrollsUnderThePointer);
  AddTest(tests, "WorkspaceShell/TerminalPaneDividerDragResizesAndDoubleClickResets",
          TestWorkspaceShellTerminalPaneDividerDragResizesAndDoubleClickResets);
  AddTest(tests, "WorkspaceShell/TerminalCloseTerminalClosesThePaneThenTheTab",
          TestWorkspaceShellTerminalCloseTerminalClosesThePaneThenTheTab);
  AddTest(tests, "WorkspaceShell/TerminalPaneFocusKeysFallThroughWithOnePane",
          TestWorkspaceShellTerminalPaneFocusKeysFallThroughWithOnePane);
  AddTest(tests, "WorkspaceShell/PanelMaximizeTakesTheWindowAndRestoresOnEditorFocus",
          TestWorkspaceShellPanelMaximizeTakesTheWindowAndRestoresOnEditorFocus);
  AddTest(tests, "WorkspaceShell/TerminalToggleAndPanelToggleKeys",
          TestWorkspaceShellTerminalToggleAndPanelToggleKeys);
  AddTest(tests, "WorkspaceShell/TerminalActivityDotMarksBackgroundOutput",
          TestWorkspaceShellTerminalActivityDotMarksBackgroundOutput);
  AddTest(tests, "WorkspaceShell/TerminalNotificationToastsFromABackgroundTab",
          TestWorkspaceShellTerminalNotificationToastsFromABackgroundTab);
  AddTest(tests, "WorkspaceShell/TerminalBellMarksABackgroundTab",
          TestWorkspaceShellTerminalBellMarksABackgroundTab);
  AddTest(tests, "WorkspaceShell/TerminalRelaunchRestartsAnExitedPane",
          TestWorkspaceShellTerminalRelaunchRestartsAnExitedPane);
  AddTest(tests, "WorkspaceShell/TerminalClosesFromTheCommandLine",
          TestWorkspaceShellTerminalClosesFromTheCommandLine);
  AddTest(tests, "WorkspaceShell/OutputReferenceClickUsesCharacterColumns",
          TestWorkspaceShellOutputReferenceClickUsesCharacterColumns);
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
  AddTest(tests, "WorkspaceShell/ProjectOpensWithTheKeyboardInTheEditor",
          TestWorkspaceShellProjectOpensWithTheKeyboardInTheEditor);
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
  AddTest(tests, "WorkspaceShell/TerminalCaretCoversAWideGlyphWhole",
          TestWorkspaceShellTerminalCaretCoversAWideGlyphWhole);
  AddTest(tests, "WorkspaceShell/TerminalKeysOweAFrame", TestWorkspaceShellTerminalKeysOweAFrame);
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
  AddTest(tests, "WorkspaceShell/TerminalShiftPageKeysScrollTheTranscript",
          TestWorkspaceShellTerminalShiftPageKeysScrollTheTranscript);
  AddTest(tests, "WorkspaceShell/BottomPanelWheelScrollsTranscript",
          TestWorkspaceShellBottomPanelWheelScrollsTranscript);
  AddTest(tests, "WorkspaceShell/TerminalDragAutoscrollsPastTheEdge",
          TestWorkspaceShellTerminalDragAutoscrollsPastTheEdge);
  AddTest(tests, "WorkspaceShell/TerminalAutoscrollNeverScrollsTheEditor",
          TestWorkspaceShellTerminalAutoscrollNeverScrollsTheEditor);
  AddTest(tests, "WorkspaceShell/TerminalCtrlShiftCNeverInterrupts",
          TestWorkspaceShellTerminalCtrlShiftCNeverInterrupts);
  AddTest(tests, "WorkspaceShell/TerminalOwnsGlobalControlChords",
          TestWorkspaceShellTerminalOwnsGlobalControlChords);
  AddTest(tests, "WorkspaceShell/TerminalInactivePaneLinkHoversAndRightClickActivates",
          TestWorkspaceShellTerminalInactivePaneLinkHoversAndRightClickActivates);
  AddTest(tests, "WorkspaceShell/TerminalSkipShellSettingEditsTheSet",
          TestWorkspaceShellTerminalSkipShellSettingEditsTheSet);
  AddTest(tests, "WorkspaceShell/TerminalCtrlSlashReachesTheShell",
          TestWorkspaceShellTerminalCtrlSlashReachesTheShell);
  AddTest(tests, "WorkspaceShell/TerminalDragSelectsTranscriptText",
          TestWorkspaceShellTerminalDragSelectsTranscriptText);
  AddTest(tests, "WorkspaceShell/TerminalGapOnScreenFillsAndTheSelectionFollows",
          TestWorkspaceShellTerminalGapOnScreenFillsAndTheSelectionFollows);
  AddTest(tests, "WorkspaceShell/UiTerminalGeometryResolvesCells",
          TestWorkspaceShellUiTerminalGeometryResolvesCells);
  AddTest(tests, "WorkspaceShell/TerminalDoubleClickSelectsWordTripleClickSelectsLine",
          TestWorkspaceShellTerminalDoubleClickSelectsWordTripleClickSelectsLine);
  AddTest(tests, "WorkspaceShell/TerminalSelectionWritesPrimaryBufferAndMiddleClickPastes",
          TestWorkspaceShellTerminalSelectionWritesPrimaryBufferAndMiddleClickPastes);
  AddTest(tests, "WorkspaceShell/TerminalLeftClickOpensUrls",
          TestWorkspaceShellTerminalLeftClickOpensUrls);
  AddTest(tests, "WorkspaceShell/TerminalLinkUnderlinesOnHover", TestWorkspaceShellTerminalLinkUnderlinesOnHover);
  AddTest(tests, "WorkspaceShell/TerminalLinkUnderlineTakesTheLinksColour",
          TestWorkspaceShellTerminalLinkUnderlineTakesTheLinksColour);
  AddTest(tests, "WorkspaceShell/FileDroppedOnTheTerminalTypesItsPath",
          TestWorkspaceShellFileDroppedOnTheTerminalTypesItsPath);
  AddTest(tests, "WorkspaceShell/ImagePastedIntoTheTerminalTypesItsPath",
          TestWorkspaceShellImagePastedIntoTheTerminalTypesItsPath);
  AddTest(tests, "WorkspaceShell/ReferenceNamesTheCaretOrSelectionLines",
          TestWorkspaceShellReferenceNamesTheCaretOrSelectionLines);
  AddTest(tests, "WorkspaceShell/TerminalNotificationToastsFromABackgroundProject",
          TestWorkspaceShellTerminalNotificationToastsFromABackgroundProject);
  AddTest(tests, "WorkspaceShell/NextTerminalAlertCrossesTabsAndProjects",
          TestWorkspaceShellNextTerminalAlertCrossesTabsAndProjects);
  AddTest(tests, "WorkspaceShell/TerminalAlertReachesPastAnUnfocusedWindow",
          TestWorkspaceShellTerminalAlertReachesPastAnUnfocusedWindow);
  AddTest(tests, "WorkspaceShell/TerminalMouseOwnerShowsAHandOnLitControls",
          TestWorkspaceShellTerminalMouseOwnerShowsAHandOnLitControls);
  AddTest(tests, "WorkspaceShell/TerminalShiftDragSelectsPastAMouseOwner",
          TestWorkspaceShellTerminalShiftDragSelectsPastAMouseOwner);
  AddTest(tests, "WorkspaceShell/TerminalWrappedLinkIsOneLink", TestWorkspaceShellTerminalWrappedLinkIsOneLink);
  AddTest(tests, "WorkspaceShell/TerminalReverseVideoPaintsThePane",
          TestWorkspaceShellTerminalReverseVideoPaintsThePane);
  AddTest(tests, "WorkspaceShell/TerminalSelectionWashesCellsAndLineBreaks",
          TestWorkspaceShellTerminalSelectionWashesCellsAndLineBreaks);
  AddTest(tests, "WorkspaceShell/TerminalAlternateScreenDropsTheSelection",
          TestWorkspaceShellTerminalAlternateScreenDropsTheSelection);
  AddTest(tests, "WorkspaceShell/TerminalCursorTakesTheProgramsShape",
          TestWorkspaceShellTerminalCursorTakesTheProgramsShape);
  AddTest(tests, "WorkspaceShell/TerminalMultiClickDragsAndShiftClickExtend",
          TestWorkspaceShellTerminalMultiClickDragsAndShiftClickExtend);
  AddTest(tests, "WorkspaceShell/TerminalSelectAllCopiesTheWholeTranscript",
          TestWorkspaceShellTerminalSelectAllCopiesTheWholeTranscript);
  AddTest(tests, "WorkspaceShell/TerminalWrappedLinkPastTheWindowIsWhole",
          TestWorkspaceShellTerminalWrappedLinkPastTheWindowIsWhole);
  AddTest(tests, "WorkspaceShell/TerminalOsc8LinkClickOpensItsTarget",
          TestWorkspaceShellTerminalOsc8LinkClickOpensItsTarget);
  AddTest(tests, "WorkspaceShellTerminal/UrlHitTestHonorsMultibytePrefix",
          TestWorkspaceShellTerminalUrlHitTestHonorsMultibytePrefix);
  AddTest(tests, "WorkspaceShellTerminal/FileReferenceClickOpensTheFile",
          TestWorkspaceShellTerminalFileReferenceClickOpensTheFile);
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

}
