#include "TestSupport.h"

#include "TerminalSessionTestAccess.h"
#include "WorkspaceShellTestAccess.h"

#include <optional>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using microide::workspace::WorkspaceShellTestAccess;

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
             "\x1b[200~printf 'hi'\n\x1b[201~",
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
  Expect(TerminalSessionTestAccess::SentBytes(session) == "git status\n",
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

  Expect(clipboard_text == "copied from term",
         "workspace terminal updates should route OSC 52 clipboard text into the clipboard writer");
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
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_TAB, SDL_KMOD_CTRL),
         "Ctrl+Tab should move focus away from the terminal panel");
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_TAB, SDL_KMOD_CTRL),
         "Ctrl+Tab should keep cycling focus targets");
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_TAB, SDL_KMOD_CTRL),
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

  Expect(WorkspaceShellTestAccess::HandleWindowFocusEvent(shell, false),
         "window focus loss should be handled");
  Expect(WorkspaceShellTestAccess::HandleWindowFocusEvent(shell, true),
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

void TestWorkspaceShellTerminalCaretDirtyRectTracksVisibleCursor() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "prompt");

  const std::optional<SDL_FRect> caret_rect = WorkspaceShellTestAccess::CurrentCaretDirtyRect(shell);
  Expect(caret_rect.has_value(),
         "focused terminal panels should expose a caret dirty rect for partial redraws");
  Expect(caret_rect->w > 0.0f && caret_rect->h > 0.0f,
         "terminal caret dirty rects should have a visible size");
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

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
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

  Expect(shell.HandleEvent(event), "right-clicking a terminal tab should be handled");
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell),
         "right-clicking a terminal tab should open a popup menu");
  Expect(WorkspaceShellTestAccess::TerminalTabContextMenuOpen(shell),
         "right-clicking a terminal tab should open the terminal tab context menu");
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

  Expect(shell.HandleEvent(event), "right-clicking the terminal panel should be handled");
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
      WorkspaceShellTestAccess::ActiveEditor(shell).lines();
  Expect(WorkspaceShellTestAccess::ExecutePasteClipboard(shell),
         "paste should execute while the terminal owns focus");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "pwd",
         "terminal-focused paste should send clipboard text to the terminal");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines() == editor_before,
         "terminal-focused paste should not modify the editor buffer");
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
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, source_rect.x + source_rect.w * 0.5f, source_rect.y + source_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "dragging should start from a terminal tab press");

  const SDL_FRect first_rect = WorkspaceShellTestAccess::TerminalTabRect(shell, 0);
  const float drop_x = first_rect.x - 8.0f;
  const float drop_y = first_rect.y + first_rect.h * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK),
         "dragging across terminal tabs should be handled");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT),
         "releasing a dragged terminal tab should be handled");

  Expect(WorkspaceShellTestAccess::TerminalLaunchLabels(shell) ==
             std::vector<std::string>{"three", "one", "two"},
         "dragging a terminal tab to the start should reorder the terminal strip");
  Expect(WorkspaceShellTestAccess::ActiveTerminalTabIndex(shell) == 0,
         "dragged terminal tab should stay active after reordering");
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

  const SDL_FRect panel_rect = WorkspaceShellTestAccess::BottomPanelContentRect(shell);
  Expect(WorkspaceShellTestAccess::HandleMouseWheel(
             shell, panel_rect.x + 12.0f, panel_rect.y + 12.0f, -3),
         "mouse wheel over the bottom panel should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTerminalScrollRow(shell) == 3,
         "mouse wheel over the bottom panel should advance the transcript scroll row");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "mouse wheel over the bottom panel should keep panel focus");
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
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, start.x, start.y,
                                                         SDL_BUTTON_LEFT),
         "pressing inside the terminal panel should start transcript selection");
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, end.x, end.y, SDL_BUTTON_LMASK),
         "dragging inside the terminal panel should update transcript selection");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonUp(shell, end.x, end.y,
                                                       SDL_BUTTON_LEFT),
         "releasing inside the terminal panel should end transcript selection");

  Expect(WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "dragging across terminal cells should create a selection");
  Expect(WorkspaceShellTestAccess::ActiveTerminalSelectedText(shell) == "select",
         "terminal drag selection should capture the selected transcript text");
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
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, start.x, start.y,
                                                         SDL_BUTTON_LEFT),
         "pressing inside the terminal panel should start transcript selection");
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, end.x, end.y, SDL_BUTTON_LMASK),
         "dragging inside the terminal panel should update transcript selection");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonUp(shell, end.x, end.y,
                                                       SDL_BUTTON_LEFT),
         "releasing inside the terminal panel should end transcript selection");
  Expect(primary_selection == "select",
         "terminal drag selection should update the primary selection buffer");

  const SDL_FPoint paste_point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 8);
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, paste_point.x, paste_point.y,
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
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, point.x, point.y,
                                                         SDL_BUTTON_LEFT),
         "left-clicking a terminal URL should be handled");
  Expect(opened_url == "https://example.com/path?a=1",
         "left-clicking a terminal URL should open the detected link target");
  Expect(!WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "opening a terminal URL should not start a text selection");
}

void TestWorkspaceShellTerminalMouseCaptureSendsButtonEvents() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetMouseTracking(session, true, false, false);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FPoint point = WorkspaceShellTestAccess::TerminalCellPoint(shell, 0, 0);
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, point.x, point.y,
                                                         SDL_BUTTON_LEFT),
         "mouse presses should be handled when the terminal requests mouse capture");
  Expect(!WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "mouse-captured presses should not create a transcript selection");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "mouse-captured presses should keep panel focus");

  Expect(WorkspaceShellTestAccess::HandleMouseButtonUp(shell, point.x, point.y,
                                                       SDL_BUTTON_LEFT),
         "mouse releases should be handled when the terminal requests mouse capture");
  Expect(!WorkspaceShellTestAccess::TerminalHasSelection(shell),
         "mouse-captured releases should leave transcript selection disabled");
}

}  // namespace

void RegisterWorkspaceShellTerminalTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "WorkspaceShell/TerminalCaretDirtyRectTracksVisibleCursor",
          TestWorkspaceShellTerminalCaretDirtyRectTracksVisibleCursor);
  AddTest(tests, "WorkspaceShell/TypingReenablesTerminalTailFollow",
          TestWorkspaceShellTypingReenablesTerminalTailFollow);
  AddTest(tests, "WorkspaceShell/HandleEventPassesEscapeToTerminal",
          TestWorkspaceShellHandleEventPassesEscapeToTerminal);
  AddTest(tests, "WorkspaceShell/CopyLastTerminalCommandIncludesOutput",
          TestWorkspaceShellCopyLastTerminalCommandIncludesOutput);
  AddTest(tests, "WorkspaceShell/CopyLastTerminalCommandFallsBackDuringAlternateScreen",
          TestWorkspaceShellCopyLastTerminalCommandFallsBackDuringAlternateScreen);
  AddTest(tests, "WorkspaceShell/CopyLastTerminalCommandIgnoresPrecedingFullWidthOutput",
          TestWorkspaceShellCopyLastTerminalCommandIgnoresPrecedingFullWidthOutput);
  AddTest(tests, "WorkspaceShell/CopyLastTerminalCommandPreservesSoftWrappedInvocation",
          TestWorkspaceShellCopyLastTerminalCommandPreservesSoftWrappedInvocation);
  AddTest(tests, "WorkspaceShell/TerminalTabRightClickOpensContextMenu",
          TestWorkspaceShellTerminalTabRightClickOpensContextMenu);
  AddTest(tests, "WorkspaceShell/TerminalPanelRightClickOpensContextMenu",
          TestWorkspaceShellTerminalPanelRightClickOpensContextMenu);
  AddTest(tests, "WorkspaceShell/TerminalPasteActionTargetsPanelFocus",
          TestWorkspaceShellTerminalPasteActionTargetsPanelFocus);
  AddTest(tests, "WorkspaceShell/TerminalTabsDragReorderToStart",
          TestWorkspaceShellTerminalTabsDragReorderToStart);
  AddTest(tests, "WorkspaceShell/BottomPanelWheelScrollsTranscript",
          TestWorkspaceShellBottomPanelWheelScrollsTranscript);
  AddTest(tests, "WorkspaceShell/TerminalDragSelectsTranscriptText",
          TestWorkspaceShellTerminalDragSelectsTranscriptText);
  AddTest(tests, "WorkspaceShell/TerminalSelectionWritesPrimaryBufferAndMiddleClickPastes",
          TestWorkspaceShellTerminalSelectionWritesPrimaryBufferAndMiddleClickPastes);
  AddTest(tests, "WorkspaceShell/TerminalLeftClickOpensUrls",
          TestWorkspaceShellTerminalLeftClickOpensUrls);
  AddTest(tests, "WorkspaceShell/TerminalMouseCaptureSendsButtonEvents",
          TestWorkspaceShellTerminalMouseCaptureSendsButtonEvents);
}

}  // namespace microide::tests
