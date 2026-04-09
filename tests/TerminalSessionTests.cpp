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
  AddTest(tests, "TerminalSession/ScrollUpSequenceUsesScrollRegion",
          TestTerminalSessionScrollUpSequenceUsesScrollRegion);
  AddTest(tests, "TerminalSession/ScrollDownSequenceUsesScrollRegion",
          TestTerminalSessionScrollDownSequenceUsesScrollRegion);
  AddTest(tests, "TerminalSession/DisableAutoWrapOverwritesLastColumn",
          TestTerminalSessionDisableAutoWrapOverwritesLastColumn);
}

}  // namespace microide::tests
