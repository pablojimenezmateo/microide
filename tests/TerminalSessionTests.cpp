#include "TestSupport.h"

#include "TerminalSessionTestAccess.h"

#include <string_view>
#include <vector>

namespace microide::tests {

namespace {

std::string LineText(const microide::terminal::TerminalLine& line) {
  std::string text;
  text.reserve(line.cells.size());
  for (const auto& cell : line.cells) {
    text.push_back(cell.character);
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

}  // namespace

void RegisterTerminalSessionTests(std::vector<TestCase>& tests) {
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
}

}  // namespace microide::tests
