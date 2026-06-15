#include "TestSupport.h"

#include "TerminalSessionTestAccess.h"

#include "terminal/TerminalBase64.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalMouseEncoder.h"

#include <chrono>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

bool ForkAvailableForTerminalTests() {
  errno = 0;
  const pid_t child_pid = fork();
  if (child_pid < 0) {
    return false;
  }
  if (child_pid == 0) {
    _exit(0);
  }
  int status = 0;
  (void)waitpid(child_pid, &status, 0);
  return true;
}
#endif

namespace microide::tests {

namespace {

std::string LineText(const microide::terminal::TerminalLine& line) {
  std::string text;
  text.reserve(line.cells.size());
  for (const auto& cell : line.cells) {
    text.append(cell.DisplayText());
  }
  return text;
}

void ExpectLineText(const std::vector<microide::terminal::TerminalLine>& lines,
                    std::size_t row,
                    std::string_view expected,
                    std::string_view message) {
  Expect(row < lines.size(), "terminal test should keep the expected row present");
  Expect(LineText(lines[row]) == expected, message);
}

void ResetAlternateScreenFixture(microide::terminal::TerminalSession& session) {
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049hA\nB\nC\nD");
}

void TestTerminalSessionScrollsBottomMarginOnLineFeed() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;4r\x1b[4;1H\nZ");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 4, "alternate-screen scroll should keep a fixed row count");
  ExpectLineText(lines, 0, "A", "line feed should preserve rows above the scroll region");
  ExpectLineText(lines, 1, "C", "line feed should scroll the scroll-region body upward");
  ExpectLineText(lines, 2, "D", "line feed should keep the previous bottom line visible");
  ExpectLineText(lines, 3, "Z", "line feed should leave a blank row for new output");
  Expect(session.cursor_row() == 3 && session.cursor_column() == 1,
         "line feed at the bottom margin should keep the cursor on the bottom row");
}

void TestTerminalSessionReverseIndexScrollsTopMargin() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;4r\x1b[2;1H\x1bMZ");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "A", "reverse index should preserve rows above the scroll region");
  ExpectLineText(lines, 1, "Z", "reverse index should open space at the top margin");
  ExpectLineText(lines, 2, "B", "reverse index should push region rows downward");
  ExpectLineText(lines, 3, "C", "reverse index should discard the previous bottom-margin row");
}

void TestTerminalSessionInsertLineRespectsScrollRegion() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;4r\x1b[3;1H\x1b[LZ");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "A", "insert line should preserve rows above the scroll region");
  ExpectLineText(lines, 1, "B", "insert line should preserve rows before the insertion point");
  ExpectLineText(lines, 2, "Z", "insert line should create a blank line at the cursor row");
  ExpectLineText(lines, 3, "C", "insert line should shift later rows down within the region");
}

void TestTerminalSessionDeleteLineRespectsScrollRegion() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;4r\x1b[2;1H\x1b[MZ");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "A", "delete line should preserve rows above the scroll region");
  ExpectLineText(lines, 1, "Z", "delete line should expose the next row at the cursor");
  ExpectLineText(lines, 2, "D", "delete line should shift later region rows upward");
  ExpectLineText(lines, 3, "", "delete line should blank-fill the freed bottom-margin row");
}

void TestTerminalSessionPasteUsesBracketedPasteWhenEnabled() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");

  session.PasteText("echo hi\n");

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[200~echo hi\n\x1b[201~",
         "terminal paste should wrap clipboard text when bracketed paste mode is enabled");
}

void TestTerminalSessionPasteFallsBackToRawBytesWhenDisabled() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  session.PasteText("echo hi\n");

  Expect(TerminalSessionTestAccess::SentBytes(session) == "echo hi\n",
         "terminal paste should send raw bytes when bracketed paste mode is disabled");
}

void TestTerminalSessionApplicationCursorKeysModeUsesSs3Sequences() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1h");

  session.SendKey(microide::terminal::TerminalSession::Key::Up);
  session.SendKey(microide::terminal::TerminalSession::Key::Home);
  session.SendKey(microide::terminal::TerminalSession::Key::End);

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1bOA\x1bOH\x1bOF",
         "application cursor-key mode should use SS3 sequences for arrows, Home, and End");
}

void TestTerminalSessionNormalCursorKeysUseCsiSequences() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1h\x1b[?1l");

  session.SendKey(microide::terminal::TerminalSession::Key::Up);
  session.SendKey(microide::terminal::TerminalSession::Key::Home);
  session.SendKey(microide::terminal::TerminalSession::Key::End);

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[A\x1b[H\x1b[F",
         "normal cursor-key mode should use CSI sequences after DECCKM is disabled");
}

void TestTerminalSessionFocusEventsUseCsiInAndOut() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1004h");

  session.SendFocusEvent(true);
  session.SendFocusEvent(false);

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[I\x1b[O",
         "focus-event mode should emit CSI I and CSI O notifications");
}

void TestTerminalSessionDisablingFocusEventsStopsNotifications() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1004h\x1b[?1004l");

  session.SendFocusEvent(true);

  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "disabled focus-event mode should suppress focus notifications");
}

void TestTerminalSessionScrollUpSequenceUsesScrollRegion() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;4r\x1b[S");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "A", "scroll-up should preserve rows above the scroll region");
  ExpectLineText(lines, 1, "C", "scroll-up should shift the region upward");
  ExpectLineText(lines, 2, "D", "scroll-up should keep later region rows visible");
  ExpectLineText(lines, 3, "", "scroll-up should blank-fill the freed bottom row");
}

void TestTerminalSessionScrollDownSequenceUsesScrollRegion() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;4r\x1b[T");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "A", "scroll-down should preserve rows above the scroll region");
  ExpectLineText(lines, 1, "", "scroll-down should blank-fill the freed top row");
  ExpectLineText(lines, 2, "B", "scroll-down should shift region rows downward");
  ExpectLineText(lines, 3, "C", "scroll-down should keep later region rows below the shift");
}

void TestTerminalSessionDisableAutoWrapOverwritesLastColumn() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 2, 4);

  TerminalSessionTestAccess::AppendOutput(session, "ABCD\x1b[?7lEF\x1b[?7hG");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() >= 2, "autowrap fixture should preserve the wrapped second row");
  ExpectLineText(lines, 0, "ABCF", "disabled autowrap should overwrite the last column");
  ExpectLineText(lines, 1, "G", "re-enabled autowrap should wrap subsequent characters");
}

void TestTerminalSessionTracksSoftWrappedRowsSeparatelyFromHardNewlines() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 4);

  TerminalSessionTestAccess::AppendOutput(session, "ABCD\nEFGHI");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() >= 3, "soft-wrap tracking fixture should preserve the wrapped third row");
  Expect(!lines[1].wrapped_from_previous,
         "rows reached by a hard newline should not be marked as wrapped continuations");
  Expect(lines[2].wrapped_from_previous,
         "rows reached by autowrap should be marked as wrapped continuations");
}

void TestTerminalSessionCachedSnapshotRangeRefreshesAfterOutput() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "ABCD\nEFGH");

  const auto& cached_before = session.SnapshotLineRangeCached(0, 2);
  Expect(cached_before.size() == 2, "cached snapshot should return the requested visible rows");
  ExpectLineText(cached_before, 1, "EFGH",
                 "cached snapshot should expose the current terminal content");

  TerminalSessionTestAccess::AppendOutput(session, "\nIJKL");
  Expect(cached_before.size() == 2,
         "cached snapshot references should remain stable when terminal output changes");
  ExpectLineText(cached_before, 1, "EFGH",
                 "terminal output invalidation should not mutate an outstanding cached snapshot");

  const auto& cached_after = session.SnapshotLineRangeCached(1, 2);
  Expect(cached_after.size() == 2,
         "cached snapshot should refresh after output changes with a new range request");
  ExpectLineText(cached_after, 0, "EFGH",
                 "cached snapshot should keep existing rows after output changes");
  ExpectLineText(cached_after, 1, "IJKL",
                 "cached snapshot should include newly appended rows after invalidation");
}

void TestTerminalSessionLineRangeSnapshotSkipsUnchangedGeneration() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "ABCD\nEFGH");

  microide::terminal::TerminalLineRangeSnapshot snapshot;
  Expect(session.SnapshotLineRangeIfChanged(0, 2, 0, &snapshot),
         "initial generation-aware snapshot should populate the visible range");
  Expect(snapshot.lines.size() == 2, "generation-aware snapshot should copy requested rows");
  ExpectLineText(snapshot.lines, 1, "EFGH",
                 "generation-aware snapshot should expose terminal content");

  const std::uint64_t stable_generation = snapshot.generation;
  Expect(!session.SnapshotLineRangeIfChanged(0, 2, stable_generation, &snapshot),
         "unchanged terminal generation should skip the visible-range copy");

  TerminalSessionTestAccess::AppendOutput(session, "\nIJKL");
  Expect(session.SnapshotLineRangeIfChanged(1, 2, stable_generation, &snapshot),
         "terminal output should advance the snapshot generation");
  Expect(snapshot.generation != stable_generation,
         "changed terminal content should publish a new generation");
  ExpectLineText(snapshot.lines, 0, "EFGH",
                 "changed snapshot should include the requested scrolled range");
  ExpectLineText(snapshot.lines, 1, "IJKL",
                 "changed snapshot should include newly appended rows");

  const std::uint64_t latest_generation = snapshot.generation;
  Expect(session.SnapshotLineRangeIfChanged(0, 2, 0, &snapshot),
         "callers should be able to force a copy when only the visible range changes");
  Expect(snapshot.generation == latest_generation,
         "range-only refresh should preserve the content generation");
  ExpectLineText(snapshot.lines, 0, "ABCD",
                 "forced range refresh should replace snapshot rows");
}

void TestTerminalSessionCursorSnapshotCapturesPositionAndVisibility() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3;4H\x1b[?25l");
  const microide::terminal::TerminalCursorSnapshot hidden = session.CursorSnapshot();
  Expect(hidden.row == 2 && hidden.column == 3,
         "cursor snapshot should capture the current cursor position under one lock");
  Expect(!hidden.visible, "cursor snapshot should capture hidden cursor state");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?25h");
  const microide::terminal::TerminalCursorSnapshot visible = session.CursorSnapshot();
  Expect(visible.row == 2 && visible.column == 3,
         "cursor snapshot should preserve position across visibility changes");
  Expect(visible.visible, "cursor snapshot should capture visible cursor state");
}

void TestTerminalSessionReportsCursorPositionQueries() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3;4H\x1b[6n\x1b[?6n");

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[3;4R\x1b[?3;4R",
         "cursor-position queries should report the current row and column");
}

void TestTerminalSessionReportsDeviceAttributesQueries() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[c\x1b[>c\x1bZ\x1b[5n");

  Expect(TerminalSessionTestAccess::SentBytes(session) ==
             "\x1b[?1;2c\x1b[>0;10;1c\x1b[?1;2c\x1b[0n",
         "device-attribute and status queries should emit terminal responses");
}

void TestTerminalSessionOriginModeMakesCupRelativeToScrollRegion() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;4r\x1b[?6h\x1b[1;1HZ");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "A", "origin mode should preserve rows above the scroll region");
  ExpectLineText(lines, 1, "Z", "origin mode should make CUP row 1 target the top margin");
  Expect(session.cursor_row() == 1 && session.cursor_column() == 1,
         "origin mode should keep the cursor relative to the scroll region");
}

void TestTerminalSessionDisableOriginModeRestoresAbsoluteCup() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;4r\x1b[?6h\x1b[?6l\x1b[1;1HZ");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "Z", "disabling origin mode should restore absolute CUP addressing");
  ExpectLineText(lines, 1, "B", "disabling origin mode should stop rebasing row 1 to the margin");
}

void TestTerminalSessionIgnoresCharsetDesignationEscapes() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "A\x1b(BC\x1b)0D\x1b*BE");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "ACDE",
                 "charset designation escapes should not leak trailing selector bytes");
}

void TestTerminalSessionOscTitleBellUpdatesLaunchLabel() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetLaunchLabel(session, "bash");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]2;repo shell\x07");

  Expect(session.LaunchLabel() == "repo shell",
         "OSC title sequences terminated by BEL should update the terminal label");
}

void TestTerminalSessionOscTitleStUpdatesLaunchLabel() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetLaunchLabel(session, "bash");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]0;git status\x1b\\");

  Expect(session.LaunchLabel() == "git status",
         "OSC title sequences terminated by ST should update the terminal label");
}

void TestTerminalSessionEmptyOscTitleRestoresLaunchLabel() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetLaunchLabel(session, "bash");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]2;temporary\x07\x1b]2;\x07");

  Expect(session.LaunchLabel() == "bash",
         "empty OSC titles should restore the default launch label");
}

void TestTerminalSessionOsc52ClipboardBellQueuesClipboardText() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]52;c;Y29waWVkIHRleHQ=\x07");

  const std::optional<std::string> clipboard = session.ConsumePendingClipboardText();
  Expect(clipboard.has_value() && *clipboard == "copied text",
         "OSC 52 sequences should decode and queue clipboard text");
  Expect(!session.ConsumePendingClipboardText().has_value(),
         "queued clipboard text should be consumed only once");
}

void TestTerminalSessionOsc52ClipboardStQueuesClipboardText() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]52;c;Zm9vCmJhcg==\x1b\\");

  const std::optional<std::string> clipboard = session.ConsumePendingClipboardText();
  Expect(clipboard.has_value() && *clipboard == "foo\nbar",
         "OSC 52 sequences terminated by ST should queue decoded clipboard text");
}

void TestTerminalSessionOsc52RejectsInvalidClipboardPayloads() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]52;c;@@@\x07\x1b]52;p;YQ==\x07");

  Expect(!session.ConsumePendingClipboardText().has_value(),
         "invalid or unsupported OSC 52 payloads should not queue clipboard text");
}

void TestTerminalSessionTracksCellForegroundAndBackgroundStyles() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[31;44mA\x1b[0mB");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 1 && lines[0].cells.size() >= 2,
         "styled terminal output should preserve at least the written cells");
  Expect(lines[0].cells[0].ascii_character() == 'A' && lines[0].cells[0].style.foreground.has_value() &&
             lines[0].cells[0].style.background.has_value(),
         "terminal cells should retain parsed foreground and background SGR styles");
  Expect(lines[0].cells[1].ascii_character() == 'B' && !lines[0].cells[1].style.foreground.has_value() &&
             !lines[0].cells[1].style.background.has_value(),
         "terminal style reset should stop coloring subsequent cells");
}

void TestTerminalSessionGroupsUtf8GlyphsIntoSingleCells() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  const std::string utf8_glyphs = "\xE2\x97\x8B \xE2\x9C\x93";

  TerminalSessionTestAccess::AppendOutput(session, utf8_glyphs);

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 1 && lines[0].cells.size() == 3,
         "utf-8 terminal symbols should consume one cell per glyph");
  Expect(LineText(lines[0]) == utf8_glyphs,
         "terminal lines should preserve pasted utf-8 symbols instead of splitting raw bytes");
}

void TestTerminalSessionSnapshotsLineRanges() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 5, 8);

  TerminalSessionTestAccess::AppendOutput(session, "A\nB\nC\nD\nE");

  const auto lines = session.SnapshotLineRange(1, 2);
  Expect(lines.size() == 2, "terminal range snapshots should return only the requested rows");
  ExpectLineText(lines, 0, "B", "terminal range snapshots should start at the requested row");
  ExpectLineText(lines, 1, "C", "terminal range snapshots should preserve row order");
}

void TestTerminalSessionTracksInverseVideoStyle() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[7mA\x1b[27mB");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 1 && lines[0].cells.size() >= 2,
         "inverse-video fixture should preserve the rendered cells");
  Expect(lines[0].cells[0].ascii_character() == 'A' && lines[0].cells[0].style.inverse(),
         "SGR 7 should mark terminal cells as inverse video");
  Expect(lines[0].cells[1].ascii_character() == 'B' && !lines[0].cells[1].style.inverse(),
         "SGR 27 should clear inverse video for later cells");
}

void TestTerminalSessionAllocatesTwoColumnsForWideGlyphs() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // U+6211 (CJK, double-width) followed by an ASCII letter.
  TerminalSessionTestAccess::AppendOutput(session, "\xE6\x88\x91" "A");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 1 && lines[0].cells.size() == 3,
         "a double-width glyph should occupy a lead cell plus a trailing spacer");
  Expect(lines[0].cells[0].DisplayText() == "\xE6\x88\x91" &&
             !lines[0].cells[0].style.wide_trailing(),
         "the wide glyph should live in the lead cell");
  Expect(lines[0].cells[1].style.wide_trailing() && lines[0].cells[1].length == 0,
         "the column after a wide glyph should be an empty trailing spacer");
  Expect(lines[0].cells[2].ascii_character() == 'A',
         "the following glyph should land two columns after the wide glyph");
  Expect(session.cursor_column() == 3,
         "the cursor should advance two columns across a wide glyph");
}

void TestTerminalSessionAttachesCombiningMarksToBaseCell() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // 'e' + U+0301 COMBINING ACUTE ACCENT (CC 81).
  TerminalSessionTestAccess::AppendOutput(session, "e\xCC\x81X");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 1 && lines[0].cells.size() == 2,
         "a combining mark should fold into the base cell, not consume a column");
  Expect(lines[0].cells[0].DisplayText() == "e\xCC\x81",
         "the base glyph and its combining mark should share one cell");
  Expect(lines[0].cells[1].ascii_character() == 'X',
         "the next glyph should follow the combined base cell directly");
}

void TestTerminalSessionReportsWorkingDirectoryAndColors() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session,
                                          "\x1b]7;file://host/home/user/proj%20x\x07");
  Expect(session.reported_working_directory() == std::filesystem::path("/home/user/proj x"),
         "OSC 7 should record the percent-decoded working directory");

  // OSC 11 background query must receive an rgb: reply terminated by ST.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]11;?\x1b\\");
  const std::string sent = TerminalSessionTestAccess::SentBytes(session);
  Expect(sent.rfind("\x1b]11;rgb:", 0) == 0 && sent.find("\x1b\\") != std::string::npos,
         "OSC 11 background query should be answered with an rgb: color reply");
}

void TestTerminalSessionEncodesModifiedAndFunctionKeys() {
  using KeyPress = microide::terminal::TerminalSession::KeyPress;
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  const auto sent_after = [&](const KeyPress& press) {
    session.SendKeyPress(press);
    std::string out = TerminalSessionTestAccess::SentBytes(session);
    TerminalSessionTestAccess::Reset(session, 24, 80);
    return out;
  };

  KeyPress up;
  up.key = KeyPress::Key::Up;
  Expect(sent_after(up) == "\x1b[A", "an unmodified Up arrow should send CSI A");

  KeyPress shift_up = up;
  shift_up.shift = true;
  Expect(sent_after(shift_up) == "\x1b[1;2A",
         "Shift+Up should send CSI 1;2 A with the shift modifier parameter");

  KeyPress ctrl_left;
  ctrl_left.key = KeyPress::Key::Left;
  ctrl_left.ctrl = true;
  Expect(sent_after(ctrl_left) == "\x1b[1;5D", "Ctrl+Left should send CSI 1;5 D");

  KeyPress f5;
  f5.key = KeyPress::Key::F5;
  Expect(sent_after(f5) == "\x1b[15~", "F5 should send CSI 15 ~");

  KeyPress shift_f5 = f5;
  shift_f5.shift = true;
  Expect(sent_after(shift_f5) == "\x1b[15;2~", "Shift+F5 should carry the modifier parameter");

  KeyPress shift_tab;
  shift_tab.key = KeyPress::Key::Tab;
  shift_tab.shift = true;
  Expect(sent_after(shift_tab) == "\x1b[Z", "Shift+Tab should send CBT (CSI Z)");

  KeyPress ctrl_a;
  ctrl_a.key = KeyPress::Key::Char;
  ctrl_a.codepoint = 'a';
  ctrl_a.ctrl = true;
  Expect(sent_after(ctrl_a) == std::string(1, '\x01'),
         "Ctrl+A should send the C0 control byte 0x01 in legacy mode");
}

void TestTerminalSessionAppliesKittyKeyboardProtocol() {
  using KeyPress = microide::terminal::TerminalSession::KeyPress;
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // Query before any negotiation reports flags = 0.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?u");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[?0u",
         "a Kitty keyboard query should report zero flags before negotiation");

  // Enable disambiguation (flag 1) via the set form, then Ctrl+A becomes CSI-u.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[=1;1u");
  KeyPress ctrl_a;
  ctrl_a.key = KeyPress::Key::Char;
  ctrl_a.codepoint = 'a';
  ctrl_a.ctrl = true;
  session.SendKeyPress(ctrl_a);
  Expect(TerminalSessionTestAccess::SentBytes(session).find("\x1b[97;5u") != std::string::npos,
         "with Kitty disambiguation, Ctrl+A should be CSI 97;5 u");

  // Shift+Enter is unrepresentable in legacy mode but disambiguated under Kitty.
  TerminalSessionTestAccess::SetRunning(session, false);
  microide::terminal::TerminalSession session2;
  TerminalSessionTestAccess::Reset(session2, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session2, "\x1b[>1u");  // push flags = 1
  KeyPress shift_enter;
  shift_enter.key = KeyPress::Key::Enter;
  shift_enter.shift = true;
  session2.SendKeyPress(shift_enter);
  Expect(TerminalSessionTestAccess::SentBytes(session2) == "\x1b[13;2u",
         "Shift+Enter under Kitty should send CSI 13;2 u");

  // Popping the stack returns to legacy mode (flags 0): Shift+Enter -> CR.
  TerminalSessionTestAccess::AppendOutput(session2, "\x1b[<1u");  // pop
  TerminalSessionTestAccess::AppendOutput(session2, "\x1b[?u");
  Expect(TerminalSessionTestAccess::SentBytes(session2).ends_with("\x1b[?0u"),
         "popping the Kitty stack should restore zero flags");
}

void TestTerminalSessionTracksSynchronizedOutputMode() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  Expect(!session.synchronized_output_active(),
         "synchronized output should start disabled");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2026h");
  Expect(session.synchronized_output_active(),
         "DEC mode 2026 set should open a synchronized-output frame");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2026l");
  Expect(!session.synchronized_output_active(),
         "DEC mode 2026 reset should close the synchronized-output frame");
}

void TestTerminalSessionAnswersDecrqmQueries() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");      // enable bracketed paste
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004$p");     // DECRQM query
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2026$p");     // DECRQM query (reset)
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?9999$p");     // unknown mode

  const std::string sent = TerminalSessionTestAccess::SentBytes(session);
  Expect(sent.find("\x1b[?2004;1$y") != std::string::npos,
         "DECRQM should report an enabled mode as set (1)");
  Expect(sent.find("\x1b[?2026;2$y") != std::string::npos,
         "DECRQM should report a disabled known mode as reset (2)");
  Expect(sent.find("\x1b[?9999;0$y") != std::string::npos,
         "DECRQM should report an unknown mode as not recognized (0)");
}

void TestTerminalSessionTracksCursorShape() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[5 q");  // steady... blinking bar
  Expect(session.cursor_shape() == microide::terminal::TerminalSession::CursorShape::Bar &&
             session.cursor_blinking(),
         "DECSCUSR 5 should select a blinking bar cursor");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2 q");  // steady block
  Expect(session.cursor_shape() == microide::terminal::TerminalSession::CursorShape::Block &&
             !session.cursor_blinking(),
         "DECSCUSR 2 should select a steady block cursor");
}

void TestTerminalSessionHonorsCustomTabStops() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  // Establish the default 8-column grid, then clear all and set a stop at col 3.
  session.Resize(24, 80);
  TerminalSessionTestAccess::SetCursorPosition(session, 0, 0);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3g");  // clear all tab stops
  TerminalSessionTestAccess::SetCursorPosition(session, 0, 3);
  TerminalSessionTestAccess::AppendOutput(session, "\x1bH");    // HTS at column 3
  TerminalSessionTestAccess::SetCursorPosition(session, 0, 0);
  TerminalSessionTestAccess::AppendOutput(session, "\t");       // tab from col 0
  Expect(session.cursor_column() == 3,
         "a tab should advance to the next custom tab stop");
}

void TestTerminalSessionTracksExtendedSgrAttributes() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // dim + italic + underline + strikethrough, then a reset clears them all.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;3;4;9mA\x1b[0mB");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 1 && lines[0].cells.size() >= 2,
         "extended-attribute fixture should preserve the rendered cells");
  const auto& a = lines[0].cells[0].style;
  Expect(a.dim() && a.italic() && a.underline() && a.strikethrough(),
         "SGR 2/3/4/9 should set dim, italic, underline, and strikethrough");
  const auto& b = lines[0].cells[1].style;
  Expect(!b.dim() && !b.italic() && !b.underline() && !b.strikethrough(),
         "SGR 0 should reset every extended attribute");
}

void TestTerminalSessionParsesColonTruecolorAndUnderlineStyles() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // Colon-form direct color (38:2:r:g:b), colon-form double underline (4:2),
  // and an underline color (58) that must be consumed without corrupting state.
  TerminalSessionTestAccess::AppendOutput(
      session, "\x1b[38:2:10:20:30;4:2;58:5:9mZ");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 1 && !lines[0].cells.empty(),
         "colon-SGR fixture should render a cell");
  const auto& cell = lines[0].cells[0];
  Expect(cell.ascii_character() == 'Z', "the glyph after colon-SGR should still render");
  Expect(cell.style.foreground.has_value() && cell.style.foreground->r == 10 &&
             cell.style.foreground->g == 20 && cell.style.foreground->b == 30,
         "colon-form 38:2:r:g:b should set a direct RGB foreground");
  Expect(cell.style.double_underline() && !cell.style.underline(),
         "colon-form 4:2 should select double underline");
}

void TestTerminalSessionCoalescesWakeEventsUntilConsumed() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  session.SetWakeEventType(SDL_EVENT_USER);

  Uint32 event_type = 0;
  Expect(TerminalSessionTestAccess::ReserveWakeEvent(session, event_type),
         "terminal sessions should reserve the first wake event");
  Expect(event_type == SDL_EVENT_USER,
         "terminal wake reservations should preserve the configured SDL event type");
  Expect(!TerminalSessionTestAccess::ReserveWakeEvent(session, event_type),
         "terminal sessions should coalesce repeated wake requests until the UI consumes one");
  Expect(session.ConsumeWakeEvent(),
         "consuming terminal wake events should clear the pending wake marker");
  Expect(TerminalSessionTestAccess::ReserveWakeEvent(session, event_type),
         "terminal sessions should allow another wake request after the UI consumes the prior one");
}

#if defined(__unix__) || defined(__APPLE__)
void TestTerminalSessionStopEscalatesToKillForStubbornChild() {
  // Cursor/agent sandboxes and some CI containers block fork(); skip instead of failing.
  if (!ForkAvailableForTerminalTests()) {
    return;
  }

  microide::terminal::TerminalSession session;

  const pid_t child_pid = fork();
  Expect(child_pid >= 0, "terminal stubborn-child fixture should fork successfully");
  if (child_pid == 0) {
    setsid();
    signal(SIGHUP, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    for (;;) {
      pause();
    }
  }

  TerminalSessionTestAccess::SetChildProcess(session, child_pid);

  const auto start = std::chrono::steady_clock::now();
  session.Stop();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            start);

  Expect(elapsed < std::chrono::milliseconds(1000),
         "terminal stop should not block for long when the child ignores hangup and terminate");
  Expect(!session.running(),
         "terminal stop should clear the running flag after forcing the child down");

  int status = 0;
  errno = 0;
  const pid_t waited = waitpid(child_pid, &status, WNOHANG);
  Expect(waited == -1 && errno == ECHILD,
         "terminal stop should reap the stubborn child process instead of leaving it behind");
}
#endif

void TestTerminalCsiParameterParsingEdgeCases() {
  using microide::terminal::CsiParamOrDefault;
  using microide::terminal::ParseCsiParameters;

  const std::vector<int> empty_default = ParseCsiParameters("");
  Expect(empty_default.empty(), "empty CSI bodies should produce no parameters");

  const std::vector<int> trailing_semicolon = ParseCsiParameters("1;2;");
  Expect(trailing_semicolon.size() == 3 && trailing_semicolon[0] == 1 && trailing_semicolon[1] == 2 &&
             trailing_semicolon[2] == 0,
         "trailing semicolons should preserve omitted trailing parameters as zero");

  const std::vector<int> prefixed = ParseCsiParameters("?25;1049");
  Expect(prefixed.size() == 2 && prefixed[0] == 25 && prefixed[1] == 1049,
         "leading non-digit prefixes should be ignored while numeric segments still parse");

  Expect(CsiParamOrDefault({0, -3, 5}, 0, 9) == 9,
         "zero CSI parameters should fall back to the default value");
  Expect(CsiParamOrDefault({0, -3, 5}, 2, 9) == 5,
         "positive CSI parameters should be returned as-is");
}

void TestTerminalBase64RejectsInvalidPayloads() {
  using microide::terminal::DecodeBase64;

  Expect(!DecodeBase64("@@@").has_value(), "invalid base64 characters should fail decoding");
  Expect(!DecodeBase64("YQ").has_value(), "non-multiple-of-four payloads should fail decoding");
  Expect(DecodeBase64("").has_value() && DecodeBase64("")->empty(),
         "empty payloads should decode to an empty string");
}

void TestTerminalMouseEncodingUsesExactByteSequences() {
  using microide::terminal::EncodeTerminalMouseEvent;
  using microide::terminal::TerminalMouseButton;
  using microide::terminal::TerminalMouseEncodeRequest;
  using microide::terminal::TerminalMouseTrackingMode;

  TerminalMouseEncodeRequest request{
      .tracking_mode = TerminalMouseTrackingMode::Normal,
      .mouse_sgr_ext_mode = false,
      .rows = 24,
      .columns = 80,
      .button = TerminalMouseButton::Left,
      .pressed = true,
      .motion = false,
      .row = 1,
      .column = 2,
      .modifiers = SDL_KMOD_NONE,
  };
  std::string bytes;
  Expect(EncodeTerminalMouseEvent(request, bytes),
         "normal mouse tracking should encode left-button presses");
  Expect(bytes == std::string("\x1b[M #\"", 6),
         "legacy mouse encoding should emit ESC [ M with 1-based coordinates");

  request.button = TerminalMouseButton::Left;
  request.pressed = false;
  request.motion = false;
  Expect(EncodeTerminalMouseEvent(request, bytes), "mouse release should still encode");
  Expect(bytes == std::string("\x1b[M##\"", 6),
         "legacy mouse release should use button code 3");

  request.mouse_sgr_ext_mode = true;
  request.button = TerminalMouseButton::Right;
  request.pressed = true;
  request.row = 4;
  request.column = 6;
  request.modifiers = SDL_KMOD_SHIFT;
  Expect(EncodeTerminalMouseEvent(request, bytes), "SGR mouse encoding should emit CSI sequences");
  Expect(bytes == "\x1b[<6;7;5M",
         "SGR mouse encoding should include button code, column, row, and trailing M");
}

void TestTerminalSessionMouseEncodingUsesExactByteSequences() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1000h");

  Expect(session.SendMouseButton(microide::terminal::TerminalSession::MouseButton::Left, true, 1, 2,
                                 SDL_KMOD_NONE),
         "terminal session should send encoded mouse button events in test mode");
  Expect(TerminalSessionTestAccess::SentBytes(session) == std::string("\x1b[M #\"", 6),
         "terminal session should preserve legacy mouse button byte sequences");
}

void TestTerminalSessionAltScreenResizeClampsCursorRows() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);
  TerminalSessionTestAccess::SetCursorPosition(session, 3, 7);

  session.Resize(2, 4);

  Expect(session.rows() == 2 && session.columns() == 4,
         "alternate-screen resize should apply the requested geometry");
  Expect(session.cursor_row() == 1 && session.cursor_column() == 3,
         "alternate-screen resize should clamp the live cursor inside the new bounds");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 2, "alternate-screen resize should keep a fixed row count");
}

void TestTerminalSessionResizeTrimScrollbackClampsSavedCursor() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  std::string bulk;
  bulk.reserve(2600 * 2);
  for (int i = 0; i < 2600; ++i) {
    bulk.append("x\n");
  }
  TerminalSessionTestAccess::AppendOutput(session, bulk);
  const std::size_t line_count_before_cursor_move = session.LineCount();
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1990;1H");
  session.Resize(24, 80);

  Expect(line_count_before_cursor_move > session.rows(),
         "bulk output should grow scrollback beyond the live viewport height");
  Expect(session.cursor_row() < session.LineCount(),
         "resize after scrollback trim should keep the cursor row addressable");
}

void TestTerminalSessionIncompleteOscChunkAcrossAppendCalls() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetLaunchLabel(session, "bash");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]2;partial");
  Expect(session.LaunchLabel() == "bash",
         "incomplete OSC title chunks should not update the launch label yet");
  TerminalSessionTestAccess::AppendOutput(session, " title\x07");
  Expect(session.LaunchLabel() == "partial title",
         "completed OSC title chunks should apply once the terminator arrives");
}

void TestTerminalSessionMalformedEscapeLeavesPlainText() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // 'g' is not a recognized escape final (unlike P/X/^/_ which start string
  // payloads), so it is consumed as a no-op and following text prints normally.
  TerminalSessionTestAccess::AppendOutput(session, "A\x1bgBC");

  ExpectLineText(session.SnapshotLines(), 0, "ABC",
                 "unknown single-byte escapes should not swallow following plain text");
}

void TestTerminalSessionStringPayloadsDoNotLeakToGrid() {
  // DCS (ESC P), SOS (ESC X), PM (ESC ^), and APC (ESC _) carry string payloads
  // terminated by ST (ESC \) or BEL. Their payloads must be discarded, not
  // printed. Real programs emit these for Sixel, Kitty graphics, tmux
  // passthrough, DECRQSS, etc.
  const std::array<std::string, 4> sequences = {
      std::string("A\x1bPq#0;2;0;0;0\x1b\\B"),   // DCS ... ST
      std::string("A\x1bX hidden sos \x1b\\B"),    // SOS ... ST
      std::string("A\x1b^ private message \x1b\\B"),  // PM ... ST
      std::string("A\x1b_Gi=1,a=q\x07""B"),        // APC ... BEL terminator
  };
  for (const std::string& sequence : sequences) {
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 24, 80);
    TerminalSessionTestAccess::AppendOutput(session, sequence);
    ExpectLineText(session.SnapshotLines(), 0, "AB",
                   "string-payload escapes must not print their payload to the grid");
  }
}

void TestTerminalSessionUnterminatedEscapeRecovers() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // An unterminated OSC longer than the internal cap must abandon the sequence
  // and let later text render rather than swallowing output forever.
  std::string output = "A\x1b]0;";
  output.append(20000, 'x');  // never sends ST/BEL; exceeds kMaxEscapeSequenceLength
  TerminalSessionTestAccess::AppendOutput(session, output);
  TerminalSessionTestAccess::AppendOutput(session, "DONE");

  const auto lines = session.SnapshotLines();
  Expect(!lines.empty(), "session should still have a line after a runaway escape");
  bool found_done = false;
  for (const microide::terminal::TerminalLine& line : lines) {
    if (LineText(line).find("DONE") != std::string::npos) {
      found_done = true;
      break;
    }
  }
  Expect(found_done, "text after an over-length unterminated escape should still render");
}

void TestTerminalSessionOverflowCsiParamDoesNotCrash() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // A parameter that overflows int must clamp instead of invoking std::atoi UB.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[99999999999;99999999999HX");
  const auto lines = session.SnapshotLines();
  // Cursor was clamped into range, so the 'X' lands somewhere on the grid.
  bool found_x = false;
  for (const microide::terminal::TerminalLine& line : lines) {
    if (LineText(line).find('X') != std::string::npos) {
      found_x = true;
      break;
    }
  }
  Expect(found_x, "an overflowing CSI parameter should clamp and still render following text");
}

void TestTerminalSessionMouseRoutingRequiresTrackingMode() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);

  Expect(!session.SendMouseButton(microide::terminal::TerminalSession::MouseButton::Left, true, 1,
                                  2, SDL_KMOD_NONE),
         "mouse routing should stay disabled until a tracking mode is enabled");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "disabled mouse routing should not emit bytes");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1000h");
  Expect(session.SendMouseButton(microide::terminal::TerminalSession::MouseButton::Left, true, 1, 2,
                                 SDL_KMOD_NONE),
         "mouse routing should encode events once normal tracking is enabled");
}

void TestTerminalSessionResizeClampsCursorAndPreservesBuffer() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "ABCDEFGH\nIJKL");
  TerminalSessionTestAccess::SetCursorPosition(session, 0, 7);

  session.Resize(4, 4);

  Expect(session.columns() == 4 && session.cursor_column() == 3,
         "resize should clamp the cursor column to the new width");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() >= 2, "resize should preserve existing terminal rows");
  ExpectLineText(lines, 0, "ABCDEFGH",
                 "resize should preserve buffered line content even after shrinking columns");
}

void TestTerminalSessionOutputParserIgnoresIncompleteEscapes() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "A\x1b[31");
  ExpectLineText(session.SnapshotLines(), 0, "A",
                 "incomplete CSI sequences should not consume trailing plain text yet");

  TerminalSessionTestAccess::AppendOutput(session, "mB");
  ExpectLineText(session.SnapshotLines(), 0, "AB",
                 "completed CSI sequences should apply styling without losing buffered text");
}

void TestTerminalCellIsTriviallyCopyableAndCompact() {
  // 2026-05-15 perf deep-dive round 2 Finding 8: TerminalCell must use inline UTF-8 storage so
  // scrollback snapshots/trims become bulk memcpys instead of per-cell std::string moves, and the
  // per-cell footprint stays small for large terminals.
  using microide::terminal::TerminalCell;
  static_assert(std::is_trivially_copyable_v<TerminalCell>,
                "TerminalCell must be trivially copyable to support bulk snapshot copies");
  // 4 bytes inline + 1 length byte + TerminalStyle (~24 bytes for two optional SDL_Color +
  // two bools). Cap at 64 to leave headroom for alignment.
  static_assert(sizeof(TerminalCell) <= 64,
                "TerminalCell footprint regressed; inline storage and trivial copyability are the "
                "point of Finding 8");

  // Functional invariants of the inline storage.
  TerminalCell ascii;
  ascii.SetAscii('Q');
  Expect(ascii.length == 1 && ascii.bytes[0] == 'Q',
         "SetAscii should record a single-byte glyph at bytes[0]");
  Expect(ascii.DisplayText() == "Q",
         "DisplayText must return the inline byte for an ASCII cell");
  Expect(ascii.ascii_character() == 'Q',
         "ascii_character must mirror the stored ASCII byte");

  TerminalCell utf8;
  utf8.SetUtf8("\xE2\x9C\x93");  // ✓ (3-byte UTF-8)
  Expect(utf8.length == 3,
         "SetUtf8 should record the full byte count of a multi-byte glyph");
  Expect(utf8.DisplayText() == std::string_view("\xE2\x9C\x93", 3),
         "DisplayText must return the inline UTF-8 sequence for non-ASCII cells");
  Expect(utf8.ascii_character() == '\0',
         "ascii_character must return NUL for multi-byte cells");

  TerminalCell empty;
  Expect(empty.length == 0 && empty.DisplayText().empty(),
         "default-constructed cell is empty and reports an empty DisplayText");
}

}  // namespace

void RegisterTerminalSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalSession/CellIsTriviallyCopyableAndCompact",
          TestTerminalCellIsTriviallyCopyableAndCompact);
  AddTest(tests, "TerminalSession/LineFeedScrollRegion",
          TestTerminalSessionScrollsBottomMarginOnLineFeed);
  AddTest(tests, "TerminalSession/ReverseIndexScrollRegion",
          TestTerminalSessionReverseIndexScrollsTopMargin);
  AddTest(tests, "TerminalSession/InsertLineScrollRegion",
          TestTerminalSessionInsertLineRespectsScrollRegion);
  AddTest(tests, "TerminalSession/DeleteLineScrollRegion",
          TestTerminalSessionDeleteLineRespectsScrollRegion);
  AddTest(tests, "TerminalSession/PasteBracketedMode",
          TestTerminalSessionPasteUsesBracketedPasteWhenEnabled);
  AddTest(tests, "TerminalSession/PasteRawMode",
          TestTerminalSessionPasteFallsBackToRawBytesWhenDisabled);
  AddTest(tests, "TerminalSession/ApplicationCursorKeysSs3Mode",
          TestTerminalSessionApplicationCursorKeysModeUsesSs3Sequences);
  AddTest(tests, "TerminalSession/NormalCursorKeysCsiMode",
          TestTerminalSessionNormalCursorKeysUseCsiSequences);
  AddTest(tests, "TerminalSession/FocusEventsUseCsiInAndOut",
          TestTerminalSessionFocusEventsUseCsiInAndOut);
  AddTest(tests, "TerminalSession/DisablingFocusEventsStopsNotifications",
          TestTerminalSessionDisablingFocusEventsStopsNotifications);
  AddTest(tests, "TerminalSession/ScrollUpSequenceUsesScrollRegion",
          TestTerminalSessionScrollUpSequenceUsesScrollRegion);
  AddTest(tests, "TerminalSession/ScrollDownSequenceUsesScrollRegion",
          TestTerminalSessionScrollDownSequenceUsesScrollRegion);
  AddTest(tests, "TerminalSession/DisableAutoWrapOverwritesLastColumn",
          TestTerminalSessionDisableAutoWrapOverwritesLastColumn);
  AddTest(tests, "TerminalSession/TracksSoftWrappedRowsSeparatelyFromHardNewlines",
          TestTerminalSessionTracksSoftWrappedRowsSeparatelyFromHardNewlines);
  AddTest(tests, "TerminalSession/CachedSnapshotRangeRefreshesAfterOutput",
          TestTerminalSessionCachedSnapshotRangeRefreshesAfterOutput);
  AddTest(tests, "TerminalSession/LineRangeSnapshotSkipsUnchangedGeneration",
          TestTerminalSessionLineRangeSnapshotSkipsUnchangedGeneration);
  AddTest(tests, "TerminalSession/CursorSnapshotCapturesPositionAndVisibility",
          TestTerminalSessionCursorSnapshotCapturesPositionAndVisibility);
  AddTest(tests, "TerminalSession/ReportsCursorPositionQueries",
          TestTerminalSessionReportsCursorPositionQueries);
  AddTest(tests, "TerminalSession/ReportsDeviceAttributesQueries",
          TestTerminalSessionReportsDeviceAttributesQueries);
  AddTest(tests, "TerminalSession/OriginModeMakesCupRelativeToScrollRegion",
          TestTerminalSessionOriginModeMakesCupRelativeToScrollRegion);
  AddTest(tests, "TerminalSession/DisableOriginModeRestoresAbsoluteCup",
          TestTerminalSessionDisableOriginModeRestoresAbsoluteCup);
  AddTest(tests, "TerminalSession/IgnoresCharsetDesignationEscapes",
          TestTerminalSessionIgnoresCharsetDesignationEscapes);
  AddTest(tests, "TerminalSession/OscTitleBellUpdatesLaunchLabel",
          TestTerminalSessionOscTitleBellUpdatesLaunchLabel);
  AddTest(tests, "TerminalSession/OscTitleStUpdatesLaunchLabel",
          TestTerminalSessionOscTitleStUpdatesLaunchLabel);
  AddTest(tests, "TerminalSession/EmptyOscTitleRestoresLaunchLabel",
          TestTerminalSessionEmptyOscTitleRestoresLaunchLabel);
  AddTest(tests, "TerminalSession/Osc52ClipboardBellQueuesClipboardText",
          TestTerminalSessionOsc52ClipboardBellQueuesClipboardText);
  AddTest(tests, "TerminalSession/Osc52ClipboardStQueuesClipboardText",
          TestTerminalSessionOsc52ClipboardStQueuesClipboardText);
  AddTest(tests, "TerminalSession/Osc52RejectsInvalidClipboardPayloads",
          TestTerminalSessionOsc52RejectsInvalidClipboardPayloads);
  AddTest(tests, "TerminalSession/TracksCellForegroundAndBackgroundStyles",
          TestTerminalSessionTracksCellForegroundAndBackgroundStyles);
  AddTest(tests, "TerminalSession/GroupsUtf8GlyphsIntoSingleCells",
          TestTerminalSessionGroupsUtf8GlyphsIntoSingleCells);
  AddTest(tests, "TerminalSession/SnapshotsLineRanges",
          TestTerminalSessionSnapshotsLineRanges);
  AddTest(tests, "TerminalSession/TracksInverseVideoStyle",
          TestTerminalSessionTracksInverseVideoStyle);
  AddTest(tests, "TerminalSession/ReportsWorkingDirectoryAndColors",
          TestTerminalSessionReportsWorkingDirectoryAndColors);
  AddTest(tests, "TerminalSession/EncodesModifiedAndFunctionKeys",
          TestTerminalSessionEncodesModifiedAndFunctionKeys);
  AddTest(tests, "TerminalSession/AppliesKittyKeyboardProtocol",
          TestTerminalSessionAppliesKittyKeyboardProtocol);
  AddTest(tests, "TerminalSession/TracksSynchronizedOutputMode",
          TestTerminalSessionTracksSynchronizedOutputMode);
  AddTest(tests, "TerminalSession/AnswersDecrqmQueries",
          TestTerminalSessionAnswersDecrqmQueries);
  AddTest(tests, "TerminalSession/TracksCursorShape", TestTerminalSessionTracksCursorShape);
  AddTest(tests, "TerminalSession/HonorsCustomTabStops",
          TestTerminalSessionHonorsCustomTabStops);
  AddTest(tests, "TerminalSession/AllocatesTwoColumnsForWideGlyphs",
          TestTerminalSessionAllocatesTwoColumnsForWideGlyphs);
  AddTest(tests, "TerminalSession/AttachesCombiningMarksToBaseCell",
          TestTerminalSessionAttachesCombiningMarksToBaseCell);
  AddTest(tests, "TerminalSession/TracksExtendedSgrAttributes",
          TestTerminalSessionTracksExtendedSgrAttributes);
  AddTest(tests, "TerminalSession/ParsesColonTruecolorAndUnderlineStyles",
          TestTerminalSessionParsesColonTruecolorAndUnderlineStyles);
  AddTest(tests, "TerminalSession/CoalescesWakeEventsUntilConsumed",
          TestTerminalSessionCoalescesWakeEventsUntilConsumed);
  AddTest(tests, "TerminalSession/CsiParameterParsingEdgeCases",
          TestTerminalCsiParameterParsingEdgeCases);
  AddTest(tests, "TerminalSession/Base64RejectsInvalidPayloads",
          TestTerminalBase64RejectsInvalidPayloads);
  AddTest(tests, "TerminalSession/MouseEncodingExactByteSequences",
          TestTerminalMouseEncodingUsesExactByteSequences);
  AddTest(tests, "TerminalSession/SessionMouseEncodingExactByteSequences",
          TestTerminalSessionMouseEncodingUsesExactByteSequences);
  AddTest(tests, "TerminalSession/AltScreenResizeClampsCursorRows",
          TestTerminalSessionAltScreenResizeClampsCursorRows);
  AddTest(tests, "TerminalSession/ResizeTrimScrollbackClampsSavedCursor",
          TestTerminalSessionResizeTrimScrollbackClampsSavedCursor);
  AddTest(tests, "TerminalSession/IncompleteOscChunkAcrossAppendCalls",
          TestTerminalSessionIncompleteOscChunkAcrossAppendCalls);
  AddTest(tests, "TerminalSession/MalformedEscapeLeavesPlainText",
          TestTerminalSessionMalformedEscapeLeavesPlainText);
  AddTest(tests, "TerminalSession/StringPayloadsDoNotLeakToGrid",
          TestTerminalSessionStringPayloadsDoNotLeakToGrid);
  AddTest(tests, "TerminalSession/UnterminatedEscapeRecovers",
          TestTerminalSessionUnterminatedEscapeRecovers);
  AddTest(tests, "TerminalSession/OverflowCsiParamDoesNotCrash",
          TestTerminalSessionOverflowCsiParamDoesNotCrash);
  AddTest(tests, "TerminalSession/MouseRoutingRequiresTrackingMode",
          TestTerminalSessionMouseRoutingRequiresTrackingMode);
  AddTest(tests, "TerminalSession/ResizeClampsCursorAndPreservesBuffer",
          TestTerminalSessionResizeClampsCursorAndPreservesBuffer);
  AddTest(tests, "TerminalSession/OutputParserIgnoresIncompleteEscapes",
          TestTerminalSessionOutputParserIgnoresIncompleteEscapes);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "TerminalSession/StopEscalatesToKillForStubbornChild",
          TestTerminalSessionStopEscalatesToKillForStubbornChild);
#endif
}

}  // namespace microide::tests
