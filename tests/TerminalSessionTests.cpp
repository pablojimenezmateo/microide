#include "TestSupport.h"

#include "terminal/TerminalSession.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace microide::tests {

struct TerminalSessionTestAccess {
  static void Reset(microide::terminal::TerminalSession& session,
                    std::size_t rows,
                    std::size_t columns) {
    std::scoped_lock lock(session.mutex_);
    session.lines_ = {microide::terminal::TerminalLine{}};
    session.primary_screen_ = microide::terminal::TerminalSession::ScreenState{};
    session.alternate_screen_ = microide::terminal::TerminalSession::ScreenState{};
    session.working_directory_.clear();
    session.launch_label_.clear();
    session.current_style_ = microide::terminal::TerminalStyle{};
    session.escape_sequence_buffer_.clear();
    session.wake_event_type_ = 0;
    session.master_fd_ = -1;
    session.child_pid_ = -1;
    session.running_ = false;
    session.stop_requested_ = false;
    session.escape_mode_ = microide::terminal::TerminalSession::EscapeMode::None;
    session.osc_escape_pending_ = false;
    session.use_alternate_screen_ = false;
    session.mouse_tracking_normal_ = false;
    session.mouse_tracking_drag_ = false;
    session.mouse_tracking_any_ = false;
    session.mouse_sgr_ext_mode_ = false;
    session.cursor_visible_ = true;
    session.rows_ = std::max<std::size_t>(1, rows);
    session.columns_ = std::max<std::size_t>(1, columns);
    session.cursor_row_ = 0;
    session.cursor_column_ = 0;
    session.saved_cursor_row_ = 0;
    session.saved_cursor_column_ = 0;
    session.ResetScrollRegionLocked();
  }

  static void AppendOutput(microide::terminal::TerminalSession& session, std::string_view data) {
    std::scoped_lock lock(session.mutex_);
    session.AppendOutputLocked(data);
  }
};

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
}

}  // namespace microide::tests
