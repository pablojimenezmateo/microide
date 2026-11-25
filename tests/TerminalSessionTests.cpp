#include "TestSupport.h"
#include "util/Rgba8.h"

#include "TerminalSessionTestAccess.h"

#include "terminal/TerminalBase64.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalMouseEncoder.h"
#include "util/AnsiPalette.h"
#include "util/PerformanceCounters.h"
#include "util/Waker.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

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

void TestTerminalSessionSwallowsUnknownCsiFinals() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 16);

  TerminalSessionTestAccess::AppendOutput(session, "a\x1b[22;0tb\x1b[23;0tc");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "abc",
                 "an unhandled CSI final must be consumed, not printed into the grid");
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

void TestTerminalSessionShrinkKeepsCursorOnScreenAfterClear() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 40);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[H\x1b[2J\x1b[3Jdev$ ");
  Expect(session.LineCount() == 24, "clear should leave exactly one screen of lines");

  session.Resize(5, 40);

  const auto lines = session.SnapshotLines();
  const std::size_t visible_top = lines.size() > 5 ? lines.size() - 5 : 0;
  const auto cursor = session.CursorSnapshot();
  Expect(cursor.row >= visible_top,
         "a shrink must not leave the cursor above the visible window");

  bool prompt_visible = false;
  for (std::size_t row = visible_top; row < lines.size(); ++row) {
    if (LineText(lines[row]).find("dev$") != std::string::npos) {
      prompt_visible = true;
    }
  }
  Expect(prompt_visible, "the prompt written before the shrink should still be on screen");

  TerminalSessionTestAccess::AppendOutput(session, "echo hi\r\nhi\r\ndev$ ");
  const auto after = session.SnapshotLines();
  const std::size_t after_top = after.size() > 5 ? after.size() - 5 : 0;
  bool echoed_visible = false;
  for (std::size_t row = after_top; row < after.size(); ++row) {
    if (LineText(after[row]).find("hi") != std::string::npos) {
      echoed_visible = true;
    }
  }
  Expect(echoed_visible, "output written after a shrink should be visible, not off-screen");
}

void TestTerminalSessionPrimaryReverseIndexDoesNotClimbScrollback() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  TerminalSessionTestAccess::AppendOutput(session, "P0\nP1\nP2\nP3\nP4\nP5");
  const auto before = session.SnapshotLines();
  Expect(before.size() == 6, "primary buffer should accumulate scrollback beyond the viewport");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[H\x1bMX");

  const auto after = session.SnapshotLines();
  Expect(after.size() == 6, "reverse index at the top must not resize the buffer");
  Expect(LineText(after[1]) == "P1",
         "reverse index must not climb into and overwrite scrollback (was P1 -> X1)");
  Expect(LineText(after[2]) == "X2",
         "reverse index should pin the cursor at the visible top row");
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

void TestTerminalSessionAlternateScreenPreservesPrimaryScrollback() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  TerminalSessionTestAccess::AppendOutput(session, "P0\nP1\nP2\nP3\nP4\nP5");
  const auto primary_before = session.SnapshotLines();
  Expect(primary_before.size() == 6,
         "primary screen should accumulate scrollback beyond the viewport");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049h\x1b[HALT");
  ExpectLineText(session.SnapshotLines(), 0, "ALT",
                 "alternate screen should show its own content after entering");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049l");
  const auto primary_after = session.SnapshotLines();
  Expect(primary_after.size() == primary_before.size(),
         "exiting the alternate screen should restore the full primary scrollback");
  for (std::size_t row = 0; row < primary_before.size(); ++row) {
    Expect(LineText(primary_after[row]) == LineText(primary_before[row]),
           "exiting the alternate screen should restore primary content byte-for-byte");
  }
  Expect(session.cursor_row() == 5 && session.cursor_column() == 2,
         "mode 1049 should restore the saved primary cursor on exit");
}

void TestTerminalSessionAlternateScreenReentryPreservesAltContent() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "primary");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1047h\x1b[HALTX");
  ExpectLineText(session.SnapshotLines(), 0, "ALTX",
                 "alternate screen should hold drawn content");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1047l");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1047h");
  ExpectLineText(session.SnapshotLines(), 0, "ALTX",
                 "re-entering the alternate screen should restore prior content");
}

void TestTerminalSessionPasteUsesBracketedPasteWhenEnabled() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");

  session.PasteText("echo hi\n");

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[200~echo hi\r\x1b[201~",
         "terminal paste should wrap clipboard text when bracketed paste mode is enabled");
}

void TestTerminalSessionPasteSendsLineBreaksAsCarriageReturns() {
  {
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 24, 80);
    session.PasteText("one\ntwo\r\nthree\rfour\n\n");
    Expect(TerminalSessionTestAccess::SentBytes(session) == "one\rtwo\rthree\rfour\r\r",
           "a plain paste rewrites every line break to a carriage return");
  }
  {
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 24, 80);
    TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");
    session.PasteText("one\r\ntwo\n");
    Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[200~one\rtwo\r\x1b[201~",
           "a bracketed paste rewrites line breaks inside the guards");
  }
}

void TestTerminalSessionPasteStripsEmbeddedEndMarker() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");

  session.PasteText("safe\x1b[201~rm -rf ~\n");

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[200~saferm -rf ~\r\x1b[201~",
         "an embedded end marker must be stripped so paste mode can't be escaped");
}

void TestTerminalSessionPasteNeutralizesReconstitutedEndMarker() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");

  session.PasteText("\x1b[\x1b[201~201~rm -rf ~\n");

  const std::string sent = TerminalSessionTestAccess::SentBytes(session);
  Expect(sent == "\x1b[200~rm -rf ~\r\x1b[201~",
         "reconstituted end markers must be neutralized to a fixed point");
  const std::string_view body_and_tail(sent.data() + 6, sent.size() - 6);
  Expect(body_and_tail.find("\x1b[201~") == body_and_tail.size() - 6,
         "the pasted body must contain no embedded end marker before the final guard");
}

void TestTerminalSessionPasteFallsBackToRawBytesWhenDisabled() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  session.PasteText("echo hi\x1b[Ax\n");

  Expect(TerminalSessionTestAccess::SentBytes(session) == "echo hi\x1b[Ax\r",
         "terminal paste should send the bytes unguarded (line breaks as Enter) when "
         "bracketed paste mode is disabled");
}

using KeyPress = microide::terminal::TerminalSession::KeyPress;

void SendUnmodified(microide::terminal::TerminalSession& session, KeyPress::Key key) {
  KeyPress press;
  press.key = key;
  Expect(session.SendKeyPress(press), "an unmodified functional key should encode to bytes");
}

void TestTerminalSessionCtrlWithoutAControlMappingSendsTheKeysUtf8() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  KeyPress press;
  press.key = KeyPress::Key::Char;
  press.ctrl = true;
  press.codepoint = U'\u00e9';
  Expect(session.SendKeyPress(press), "Ctrl with an unmapped key still encodes to bytes");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\xc3\xa9",
         "an unmapped Ctrl key sends the key's own UTF-8, not its truncated low byte");

  TerminalSessionTestAccess::Reset(session, 24, 80);
  press.codepoint = U'a';
  Expect(session.SendKeyPress(press), "Ctrl+A encodes");
  press.codepoint = U'1';
  Expect(session.SendKeyPress(press), "Ctrl+1 encodes");
  Expect(TerminalSessionTestAccess::SentBytes(session) == std::string("\x01") + "1",
         "Ctrl+A stays SOH and Ctrl+1 stays '1'");
}

void TestTerminalSessionApplicationCursorKeysModeUsesSs3Sequences() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1h");

  SendUnmodified(session, KeyPress::Key::Up);
  SendUnmodified(session, KeyPress::Key::Home);
  SendUnmodified(session, KeyPress::Key::End);

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1bOA\x1bOH\x1bOF",
         "application cursor-key mode should use SS3 sequences for arrows, Home, and End");
}

void TestTerminalSessionNormalCursorKeysUseCsiSequences() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1h\x1b[?1l");

  SendUnmodified(session, KeyPress::Key::Up);
  SendUnmodified(session, KeyPress::Key::Home);
  SendUnmodified(session, KeyPress::Key::End);

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

void TestTerminalSnapshotRangeReusesDestinationCellBuffers() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 16);
  for (int i = 0; i < 8; ++i) {
    TerminalSessionTestAccess::AppendOutput(session, "row-content\r\n");
  }

  microide::terminal::TerminalLineRangeSnapshot snapshot;
  Expect(session.SnapshotLineRangeIfChanged(0, 4, 0, &snapshot),
         "the first snapshot of a range should report a change");
  Expect(snapshot.lines.size() == 4, "the snapshot should hold the requested rows");
  std::vector<const void*> first_pass;
  for (const auto& line : snapshot.lines) {
    first_pass.push_back(line.cells.data());
  }

  for (int round = 0; round < 4; ++round) {
    TerminalSessionTestAccess::AppendOutput(session, "more-content\r\n");
    Expect(session.SnapshotLineRangeIfChanged(0, 4, snapshot.generation - 1, &snapshot),
           "a changed generation should re-copy the range");
    Expect(snapshot.lines.size() == 4, "the row count is unchanged across rounds");
    for (std::size_t i = 0; i < snapshot.lines.size(); ++i) {
      Expect(snapshot.lines[i].cells.data() == first_pass[i],
             "re-snapshotting a same-sized range must reuse the destination cell buffers");
    }
  }

  session.SnapshotLineRangeInto(0, 4, snapshot.lines);
  Expect(snapshot.lines.size() == 4, "the reusing overload copies the same row count");
  for (const auto& line : snapshot.lines) {
    Expect(line.cells.size() <= 16, "reused rows must report the source row length, not the capacity");
  }
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

void TestTerminalSessionConsumesMultiByteCharsetDesignations() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "A\x1b(\"?B\x1b)%5C\x1b(BD");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "ABCD",
                 "a multi-byte charset designation must be consumed whole, final byte included");
}

void TestTerminalSessionCombiningMarkAttachesToAWideBaseGlyph() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  TerminalSessionTestAccess::AppendOutput(session, "\xe4\xb8\x80\xcc\x81" "e\xcc\x81");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "\xe4\xb8\x80\xcc\x81" "e\xcc\x81",
                 "a combining mark after a double-width glyph must attach to the lead cell");
  Expect(session.cursor_column() == 3,
         "the mark must not consume a column of its own");
}

void TestTerminalSessionDecstbmHomesToTheMarginUnderOriginMode() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?6h\x1b[2;4rZ");

  Expect(session.cursor_row() == 1,
         "DECSTBM under origin mode should home to the top margin, not to screen row 0");
  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "A", "the row above the margin must be untouched");
  ExpectLineText(lines, 1, "Z", "the homed write should land on the top margin row");
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

void TestTerminalSessionBellIsConsumedOnceAndNotFromAnOscTerminator() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]0;title\x07plain text");
  Expect(!session.ConsumeBell(), "an OSC terminated by BEL does not ring");

  TerminalSessionTestAccess::AppendOutput(session, "done\x07\x07");
  Expect(session.ConsumeBell(), "a bare BEL rings");
  Expect(!session.ConsumeBell(), "and is consumed once, however many arrived together");
}

void TestTerminalSessionOsc52ClipboardStQueuesClipboardText() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]52;c;Zm9vCmJhcg==\x1b\\");

  const std::optional<std::string> clipboard = session.ConsumePendingClipboardText();
  Expect(clipboard.has_value() && *clipboard == "foo\nbar",
         "OSC 52 sequences terminated by ST should queue decoded clipboard text");
}

void TestTerminalSessionOsc52OversizedPayloadIsFlagged() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  std::string huge = "\x1b]52;c;";
  huge.append(20000, 'A');
  TerminalSessionTestAccess::AppendOutput(session, huge);

  Expect(!session.ConsumePendingClipboardText().has_value(),
         "an oversized OSC 52 payload must not queue clipboard text");
  Expect(session.ConsumeOversizedOsc52Dropped(),
         "an oversized OSC 52 drop must be flagged for the host to surface");
  Expect(!session.ConsumeOversizedOsc52Dropped(),
         "the oversized-drop flag is consumed only once");
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
                                          "\x1b]7;file:///home/user/proj%20x\x07");
  Expect(session.reported_working_directory() == std::filesystem::path("/home/user/proj x"),
         "OSC 7 should record the percent-decoded working directory");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]7;file://localhost/tmp/local\x07");
  Expect(session.reported_working_directory() == std::filesystem::path("/tmp/local"),
         "OSC 7 with a localhost host should be treated as local");

  TerminalSessionTestAccess::AppendOutput(session,
                                          "\x1b]7;file://some-remote-box/remote/path\x07");
  Expect(session.reported_working_directory() == std::filesystem::path("/tmp/local"),
         "OSC 7 from a non-local host must be ignored, keeping the last local cwd");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]11;?\x1b\\");
  const std::string sent = TerminalSessionTestAccess::SentBytes(session);
  Expect(sent.rfind("\x1b]11;rgb:", 0) == 0 && sent.find("\x1b\\") != std::string::npos,
         "OSC 11 background query should be answered with an rgb: color reply");
}

void TestTerminalSessionAnswersModernTuiQueries() {
  using microide::terminal::TerminalSession;
  using microide::util::Rgba8;
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  const auto reply_to = [&](std::string_view query) {
    TerminalSessionTestAccess::ClearSentBytes(session);
    TerminalSessionTestAccess::AppendOutput(session, query);
    return TerminalSessionTestAccess::SentBytes(session);
  };

  const std::string version = reply_to("\x1b[>0q");
  Expect(version.rfind("\x1bP>|microide ", 0) == 0 &&
             version.size() > 2 && version.substr(version.size() - 2) == "\x1b\\",
         "XTVERSION is answered with DCS > | name version ST: " + version);
  Expect(reply_to("\x1b[>q").rfind("\x1bP>|microide ", 0) == 0, "with the parameter omitted too");

  session.SetDefaultColors(Rgba8{0x20, 0x20, 0x20, 0xff}, Rgba8{0xff, 0xff, 0xff, 0xff});
  Expect(reply_to("\x1b]11;?\x1b\\") == "\x1b]11;rgb:ffff/ffff/ffff\x1b\\",
         "OSC 11 reports the host's (light) background: " + reply_to("\x1b]11;?\x1b\\"));
  Expect(reply_to("\x1b]10;?\x07") == "\x1b]10;rgb:2020/2020/2020\x1b\\",
         "OSC 10 reports the host's foreground");

  Expect(reply_to("\x1b[?2031$p") == "\x1b[?2031;2$y", "2031 is a known mode, reset");
  Expect(reply_to("\x1b[?996n") == "\x1b[?997;2n", "a white background is light");
  Expect(reply_to("\x1b[?2031h").empty(), "enabling reports nothing by itself");
  Expect(reply_to("\x1b[?2031$p") == "\x1b[?2031;1$y", "and the mode now reads set");
  TerminalSessionTestAccess::ClearSentBytes(session);
  session.SetDefaultColors(Rgba8{0xee, 0xee, 0xee, 0xff}, Rgba8{0x10, 0x10, 0x10, 0xff});
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[?997;1n",
         "a switch to a dark theme is reported to a subscribed program");
  TerminalSessionTestAccess::ClearSentBytes(session);
  session.SetDefaultColors(Rgba8{0xef, 0xee, 0xee, 0xff}, Rgba8{0x11, 0x10, 0x10, 0xff});
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "a colour change that stays dark is not a scheme flip");
  Expect(reply_to("\x1b[?2031l").empty(), "unsubscribe");
  TerminalSessionTestAccess::ClearSentBytes(session);
  session.SetDefaultColors(Rgba8{0, 0, 0, 0xff}, Rgba8{0xff, 0xff, 0xff, 0xff});
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "and an unsubscribed program is not told");
}

void TestTerminalSessionQueuesDesktopNotifications() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]9;Claude needs your permission\x07");
  auto notification = session.ConsumeNotification();
  Expect(notification.has_value() && notification->title.empty() &&
             notification->body == "Claude needs your permission",
         "OSC 9 queues its body");
  Expect(!session.ConsumeNotification().has_value(), "once");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]9;4;1;50\x07");
  Expect(!session.ConsumeNotification().has_value(), "a ConEmu progress subcommand is not a message");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b]777;notify;Build;finished in 3s\x1b\\");
  notification = session.ConsumeNotification();
  Expect(notification.has_value() && notification->title == "Build" &&
             notification->body == "finished in 3s",
         "OSC 777 notify carries a title and a body");
  TerminalSessionTestAccess::AppendOutput(session, "plain text after");
  Expect(!session.ConsumeNotification().has_value(), "nothing else queues one");
}

void TestTerminalSessionStampsOsc8LinksOnGlyphs() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 40);
  TerminalSessionTestAccess::AppendOutput(
      session, "a \x1b]8;id=1;https://example.com\x1b\\link\x1b]8;;\x1b\\ b "
               "\x1b]8;;https://example.com\x07\xe4\xb8\xad\x1b]8;;\x07"
               "\r\n\x1b]8;;https://example.com\x07\x1b[41m\x1b[2K\x1b[m\x1b]8;;\x07");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() >= 2 && lines[0].cells.size() == 11 && lines[1].cells.size() == 40,
         "both lines were written");
  if (lines.size() < 2 || lines[0].cells.size() != 11 || lines[1].cells.size() != 40) {
    return;
  }
  const auto& cells = lines[0].cells;
  Expect(cells[0].extra == 0 && cells[1].extra == 0, "text before the link carries none");
  Expect(cells[2].extra != 0 && cells[2].extra == cells[5].extra &&
             session.LinkUri(cells[2].extra) == "https://example.com",
         "every glyph inside the link carries its URI");
  Expect(cells[6].extra == 0 && cells[8].extra == 0, "the closing OSC 8 ends it");
  Expect(cells[9].extra == cells[2].extra && cells[10].extra == cells[2].extra,
         "the same URI shares one id, wide spacer included");
  Expect(TerminalSessionTestAccess::ExtraCount(session) == 1, "one table entry");
  for (const auto& erased : lines[1].cells) {
    Expect(erased.extra == 0 && erased.style.background.has_value(),
           "an erased cell takes the background, not the open link");
  }

  TerminalSessionTestAccess::AppendOutput(
      session, "\r\n\x1b]8;;file://localhost/tmp/a%20b.txt\x07" "f\x1b]8;;\x07"
               "\x1b]8;;file://elsewhere.invalid/x\x07g\x1b]8;;\x07");
  const auto after = session.SnapshotLines();
  Expect(after.size() >= 3 && after[2].cells.size() >= 2,
         "the third line was written: " + std::to_string(after.size()) + " lines, " +
             std::to_string(after.size() > 2 ? after[2].cells.size() : 0) + " cells");
  if (after.size() < 3 || after[2].cells.size() < 2) {
    return;
  }
  Expect(session.LinkUri(after[2].cells[0].extra) == "file:///tmp/a b.txt",
         "a local file link keeps its decoded path");
  Expect(session.LinkUri(after[2].cells[1].extra) == "file://elsewhere.invalid/x",
         "another machine's file link is kept verbatim");
}

void TestTerminalSessionKeepsUnderlineShapesAndColours() {
  using microide::terminal::TerminalColor;
  using microide::terminal::TerminalUnderlineVariant;
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 40);
  TerminalSessionTestAccess::AppendOutput(
      session,
      "\x1b[4:3m\x1b[58;2;255;0;0mA\x1b[4:4mB\x1b[4:5m\x1b[58:5:2mC\x1b[59mD\x1b[24mE"
      "\x1b[4m\x1b[58;2;1;2;3mF\x1b[0mG"
      "\x1b]8;;https://x.example\x07\x1b[4m\x1b[58;2;1;2;3mH\x1b[59mI\x1b]8;;\x07"
      "\x1b[58;2;9;9;9m\x1b" "7\x1b[0mJ\x1b" "8K\x1b[21mL");
  const auto lines = session.SnapshotLines();
  Expect(!lines.empty() && lines[0].cells.size() == 11 && lines[0].cells[9].ascii_character() == 'K',
         "the line was written, K over J");
  if (lines.empty() || lines[0].cells.size() != 11) {
    return;
  }
  const auto& c = lines[0].cells;
  const auto color_of = [&](std::size_t i) { return TerminalSessionTestAccess::UnderlineColorOf(session, c[i].extra); };
  const TerminalColor red = microide::util::Rgba8{255, 0, 0, 255};
  const TerminalColor rgb123 = microide::util::Rgba8{1, 2, 3, 255};
  Expect(c[0].style.underline() && c[0].style.underline_variant() == TerminalUnderlineVariant::Curly &&
             color_of(0) == red,
         "4:3 is curly, and 58;2 colours it");
  Expect(c[1].style.underline_variant() == TerminalUnderlineVariant::Dotted && color_of(1) == red &&
             c[1].extra == c[0].extra,
         "4:4 is dotted, the colour stays on the pen and the cells share its entry");
  Expect(c[2].style.underline_variant() == TerminalUnderlineVariant::Dashed &&
             color_of(2) == TerminalColor(microide::util::Ansi256Color(2)),
         "4:5 is dashed; the colon form of 58 takes a palette index");
  Expect(c[3].style.underline() && c[3].extra == 0, "59 drops the colour");
  Expect(!c[4].style.underline() && c[4].style.underline_variant() == TerminalUnderlineVariant::Straight,
         "24 ends the underline and its shape");
  Expect(c[5].style.underline_variant() == TerminalUnderlineVariant::Straight && color_of(5) == rgb123,
         "a plain 4 is straight");
  Expect(!c[6].style.underline() && c[6].extra == 0, "0 resets the colour with the rest");
  Expect(session.LinkUri(c[7].extra) == "https://x.example" && color_of(7) == rgb123,
         "a link and a colour share one entry");
  Expect(session.LinkUri(c[8].extra) == "https://x.example" && !color_of(8).has_value() &&
             c[8].extra != c[7].extra,
         "dropping the colour keeps the link");
  Expect(color_of(9) == TerminalColor(microide::util::Rgba8{9, 9, 9, 255}),
         "DECRC restores the colour DECSC saved, past the SGR 0 between them");
  Expect(c[10].style.double_underline() && color_of(10) == color_of(9),
         "21 is double and keeps the colour");

  microide::terminal::TerminalLineRangeSnapshot snapshot;
  Expect(session.SnapshotLineRangeIfChanged(0, 4, 0, &snapshot), "a snapshot is taken");
  Expect(microide::terminal::FindUnderlineColor(snapshot.underline_colors, c[0].extra) == red &&
             microide::terminal::FindUnderlineColor(snapshot.underline_colors, c[7].extra) == rgb123 &&
             !microide::terminal::FindUnderlineColor(snapshot.underline_colors, c[8].extra).has_value(),
         "the snapshot resolves each underlined cell's colour by its extra");
}

void TestTerminalCellExtraTableRecyclesOnlyDeadIds() {
  using microide::terminal::TerminalCellExtraTable;
  using microide::terminal::TerminalColor;
  TerminalCellExtraTable table;
  Expect(table.Intern("", {}) == 0 && table.Intern(std::string(3000, 'x'), {}) == 0,
         "an empty or over-long URI with no colour holds nothing");
  const std::uint16_t a = table.Intern("https://a", {});
  const std::uint16_t b = table.Intern("https://b", {});
  Expect(a == 1 && b == 2 && table.Intern("https://a", {}) == a, "identical URIs share an id");
  const TerminalColor red = microide::util::Rgba8{255, 0, 0, 255};
  const std::uint16_t colored = table.Intern("https://a", red);
  Expect(colored != a && table.Uri(colored) == "https://a" && table.UnderlineColor(colored) == red,
         "the same link with an underline colour is its own entry");
  const std::uint16_t color_only = table.Intern("", red);
  Expect(color_only != 0 && table.Uri(color_only).empty() && table.UnderlineColor(color_only) == red &&
             table.Intern(std::string(3000, 'x'), red) == color_only,
         "a colour alone is an entry with no link (an over-long URI is dropped, not the colour)");
  Expect(table.has_underline_colors(), "and the table knows it holds colours");
  std::vector<bool> live(5, false);
  live[b] = true;
  table.Retain(live);
  Expect(table.Uri(a).empty() && table.Uri(b) == "https://b" && !table.UnderlineColor(colored).has_value(),
         "only the dead ids are freed");
  Expect(!table.has_underline_colors(), "freeing the coloured entries forgets the colours");
  Expect(table.Intern("https://c", {}) != b, "a freed id is reused");
  for (std::size_t i = table.size(); i < TerminalCellExtraTable::kMaxEntries; ++i) {
    table.Intern("https://n/" + std::to_string(i), {});
  }
  Expect(table.full() && table.Intern("https://overflow", {}) == 0, "a full table refuses");
}

void TestTerminalSessionReplaysClaudeCodeTrustDialog() {
  std::ifstream in(std::filesystem::path(MICROIDE_TEST_SOURCE_DIR) / "fixtures" / "terminal" /
                       "claude-code-2.1.294-trust-dialog.raw",
                   std::ios::binary);
  const std::string stream((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Expect(stream.size() > 1000, "the recording is present");
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 40, 120);
  TerminalSessionTestAccess::AppendOutput(session, stream);

  std::string screen;
  for (const auto& line : session.SnapshotLines()) {
    screen += LineText(line);
    screen += '\n';
  }
  const auto has = [&](std::string_view text) { return screen.find(text) != std::string::npos; };
  Expect(has(" Accessing workspace:") && has(" /tmp/project") &&
             has(" \xe2\x9d\xaf No, exit") && has("   Yes, I trust this folder") &&
             has(" Enter to confirm \xc2\xb7 Esc to cancel"),
         "the dialog lands where the program put it:\n" + screen);
  const std::string replies = TerminalSessionTestAccess::SentBytes(session);
  Expect(replies.find("\x1bP>|microide ") != std::string::npos, "XTVERSION answered: " + replies);
  Expect(replies.find("\x1b[?0u") != std::string::npos, "the kitty keyboard query answered");
  Expect(replies.find("\x1b[?1;2c") != std::string::npos, "DA1 answered");
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
  const auto ctrl_char = [&](char32_t cp) {
    KeyPress press;
    press.key = KeyPress::Key::Char;
    press.codepoint = cp;
    press.ctrl = true;
    return sent_after(press);
  };
  Expect(ctrl_char(U'/') == std::string(1, '\x1f'), "Ctrl+/ should send US (0x1f)");
  Expect(ctrl_char(U'2') == std::string(1, '\0'), "Ctrl+2 should send NUL");
  Expect(ctrl_char(U'6') == std::string(1, '\x1e'), "Ctrl+6 should send RS (0x1e)");
  Expect(ctrl_char(U'8') == std::string(1, '\x7f'), "Ctrl+8 should send DEL");

  KeyPress ctrl_alt_a;
  ctrl_alt_a.key = KeyPress::Key::Char;
  ctrl_alt_a.codepoint = 'a';
  ctrl_alt_a.ctrl = true;
  ctrl_alt_a.alt = true;
  Expect(sent_after(ctrl_alt_a) == std::string("\x1b\x01", 2),
         "Ctrl+Alt+A should send ESC + C0 control byte (Meta+Ctrl) in legacy mode");

  KeyPress ctrl_backspace;
  ctrl_backspace.key = KeyPress::Key::Backspace;
  ctrl_backspace.ctrl = true;
  Expect(sent_after(ctrl_backspace) == std::string(1, '\x08'),
         "Ctrl+Backspace should send BS (0x08) in legacy mode");

  KeyPress ctrl_alt_backspace = ctrl_backspace;
  ctrl_alt_backspace.alt = true;
  Expect(sent_after(ctrl_alt_backspace) == std::string("\x1b\x08", 2),
         "Ctrl+Alt+Backspace should send ESC + BS (Meta+Ctrl) in legacy mode");
}

void TestTerminalSessionAppliesKittyKeyboardProtocol() {
  using KeyPress = microide::terminal::TerminalSession::KeyPress;
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?u");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[?0u",
         "a Kitty keyboard query should report zero flags before negotiation");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[=1;1u");
  KeyPress ctrl_a;
  ctrl_a.key = KeyPress::Key::Char;
  ctrl_a.codepoint = 'a';
  ctrl_a.ctrl = true;
  session.SendKeyPress(ctrl_a);
  Expect(TerminalSessionTestAccess::SentBytes(session).find("\x1b[97;5u") != std::string::npos,
         "with Kitty disambiguation, Ctrl+A should be CSI 97;5 u");
  TerminalSessionTestAccess::ClearSentBytes(session);
  KeyPress escape;
  escape.key = KeyPress::Key::Escape;
  session.SendKeyPress(escape);
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[27u",
         "with Kitty disambiguation, a bare Esc should be CSI 27 u");
  TerminalSessionTestAccess::ClearSentBytes(session);
  KeyPress shift_a;
  shift_a.key = KeyPress::Key::Char;
  shift_a.codepoint = 'A';
  shift_a.shift = true;
  session.SendKeyPress(shift_a);
  Expect(TerminalSessionTestAccess::SentBytes(session) == "A",
         "with Kitty disambiguation, Shift+A should still send the text \"A\"");
  TerminalSessionTestAccess::ClearSentBytes(session);
  shift_a.alt = true;
  session.SendKeyPress(shift_a);
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[97;4u",
         "with Kitty disambiguation, Shift+Alt+A should be CSI 97;4 u");

  TerminalSessionTestAccess::SetRunning(session, false);
  microide::terminal::TerminalSession session2;
  TerminalSessionTestAccess::Reset(session2, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session2, "\x1b[>1u");
  KeyPress shift_enter;
  shift_enter.key = KeyPress::Key::Enter;
  shift_enter.shift = true;
  session2.SendKeyPress(shift_enter);
  Expect(TerminalSessionTestAccess::SentBytes(session2) == "\x1b[13;2u",
         "Shift+Enter under Kitty should send CSI 13;2 u");

  TerminalSessionTestAccess::AppendOutput(session2, "\x1b[<1u");
  TerminalSessionTestAccess::AppendOutput(session2, "\x1b[?u");
  Expect(TerminalSessionTestAccess::SentBytes(session2).ends_with("\x1b[?0u"),
         "popping the Kitty stack should restore zero flags");
}

void TestTerminalSessionKittyHugePopIsBoundedByStackDepth() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[>1u");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[>3u");
  Expect(TerminalSessionTestAccess::KittyKeyboardFlags(session) == 3,
         "two pushes leave the latest flags active");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[<65535u");
  Expect(TerminalSessionTestAccess::KittyKeyboardFlags(session) == 0,
         "a huge pop drains the whole stack and resets flags to 0");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[<65535u");
  Expect(TerminalSessionTestAccess::KittyKeyboardFlags(session) == 0,
         "popping an empty stack with a huge count stays at flags 0");
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

void TestTerminalSessionAFrameClosedInAReadWakesEvenWhenTheNextOneOpened() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 20);
  Expect(!TerminalSessionTestAccess::AppendOutputAndDecideWake(session, "\x1b[?2026hframe one"),
         "a frame still open does not wake");
  Expect(TerminalSessionTestAccess::AppendOutputAndDecideWake(session, " done\x1b[?2026l\x1b[?2026h\x1b[2;1Hnext"),
         "a read that closes a frame and opens the next wakes for the closed one");
  Expect(!TerminalSessionTestAccess::AppendOutputAndDecideWake(session, " still drawing"),
         "and the next frame, open, suppresses again");
  Expect(TerminalSessionTestAccess::AppendOutputAndDecideWake(session, "\x1b[?2026l"),
         "until it closes");
  Expect(TerminalSessionTestAccess::AppendOutputAndDecideWake(session, "plain"), "unsynchronized output wakes");
}

void TestTerminalSessionAnswersDecrqmQueries() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004$p");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2026$p");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?9999$p");

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

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[5 q");
  Expect(session.cursor_shape() == microide::terminal::TerminalSession::CursorShape::Bar &&
             session.cursor_blinking(),
         "DECSCUSR 5 should select a blinking bar cursor");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2 q");
  Expect(session.cursor_shape() == microide::terminal::TerminalSession::CursorShape::Block &&
             !session.cursor_blinking(),
         "DECSCUSR 2 should select a steady block cursor");
}

void TestTerminalSessionHonorsCustomTabStops() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  session.Resize(24, 80);
  TerminalSessionTestAccess::SetCursorPosition(session, 0, 0);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3g");
  TerminalSessionTestAccess::SetCursorPosition(session, 0, 3);
  TerminalSessionTestAccess::AppendOutput(session, "\x1bH");
  TerminalSessionTestAccess::SetCursorPosition(session, 0, 0);
  TerminalSessionTestAccess::AppendOutput(session, "\t");
  Expect(session.cursor_column() == 3,
         "a tab should advance to the next custom tab stop");
}

void TestTerminalSessionTracksExtendedSgrAttributes() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

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
  constexpr microide::util::WakeChannel kChannel = 4242;
  session.SetWakeChannel(kChannel);

  microide::util::WakeChannel channel = 0;
  Expect(TerminalSessionTestAccess::ReserveWakeChannel(session, channel),
         "terminal sessions should reserve the first wake event");
  Expect(channel == kChannel,
         "terminal wake reservations should preserve the configured wake channel");
  Expect(!TerminalSessionTestAccess::ReserveWakeChannel(session, channel),
         "terminal sessions should coalesce repeated wake requests until the UI consumes one");
  Expect(session.ConsumeWakeEvent(),
         "consuming terminal wake events should clear the pending wake marker");
  Expect(TerminalSessionTestAccess::ReserveWakeChannel(session, channel),
         "terminal sessions should allow another wake request after the UI consumes the prior one");
}

void TestTerminalSessionRejectedWakeLatchesOwedBit() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  session.SetWakeChannel(4242);

  microide::util::SetWakePusherForTesting([](microide::util::WakeChannel) { return false; });
  (void)microide::util::ConsumeOwedWake();

  TerminalSessionTestAccess::PushWakeEvent(session);

  const bool owed = microide::util::HasOwedWake();
  microide::util::SetWakePusherForTesting(nullptr);
  (void)microide::util::ConsumeOwedWake();

  Expect(owed, "a rejected terminal wake push must latch the process-wide owed-wake bit");
}

void TestTerminalSessionStopEscalatesToKillForStubbornChild() {
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
  const TimingWindow start_timing;
  session.Stop();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            start);

  Expect(start_timing.Within(elapsed, std::chrono::milliseconds(1000)),
         "terminal stop should not block for long when the child ignores hangup and terminate");
  Expect(!session.running(),
         "terminal stop should clear the running flag after forcing the child down");

  int status = 0;
  errno = 0;
  const pid_t waited = waitpid(child_pid, &status, WNOHANG);
  Expect(waited == -1 && errno == ECHILD,
         "terminal stop should reap the stubborn child process instead of leaving it behind");
}

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
      .modifiers = microide::util::kKeyModNone,
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
  request.modifiers = microide::util::kKeyModShift;
  Expect(EncodeTerminalMouseEvent(request, bytes), "SGR mouse encoding should emit CSI sequences");
  Expect(bytes == "\x1b[<6;7;5M",
         "SGR mouse encoding should include button code, column, row, and trailing M");

  request.mouse_sgr_ext_mode = true;
  request.button = TerminalMouseButton::Left;
  request.pressed = false;
  request.motion = false;
  request.row = 4;
  request.column = 6;
  request.modifiers = microide::util::kKeyModNone;
  Expect(EncodeTerminalMouseEvent(request, bytes), "SGR mouse release should encode");
  Expect(bytes == "\x1b[<0;7;5m",
         "SGR mouse release must keep the real button (0) and use trailing m, not code 3");

  {
    TerminalMouseEncodeRequest big{
        .tracking_mode = TerminalMouseTrackingMode::Normal,
        .mouse_sgr_ext_mode = false,
        .rows = 400,
        .columns = 400,
        .button = TerminalMouseButton::Left,
        .pressed = true,
        .motion = false,
        .row = 5,
        .column = 250,
        .modifiers = microide::util::kKeyModNone,
    };
    std::string dropped;
    Expect(!EncodeTerminalMouseEvent(big, dropped),
           "legacy mouse encoding should drop clicks beyond column 222");
    Expect(dropped.empty(), "dropped legacy mouse event must not emit bytes");

    big.column = 222;
    Expect(EncodeTerminalMouseEvent(big, dropped),
           "legacy mouse encoding should still encode column 222");
    big.column = 223;
    Expect(!EncodeTerminalMouseEvent(big, dropped),
           "legacy mouse encoding should drop column 223");
    big.mouse_sgr_ext_mode = true;
    big.column = 250;
    Expect(EncodeTerminalMouseEvent(big, dropped),
           "SGR mouse encoding should encode large coordinates");
  }
}

void TestTerminalSessionMouseEncodingUsesExactByteSequences() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1000h");

  Expect(session.SendMouseButton(microide::terminal::TerminalSession::MouseButton::Left, true, 1, 2,
                                 microide::util::kKeyModNone),
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

  TerminalSessionTestAccess::AppendOutput(session, "A\x1bgBC");

  ExpectLineText(session.SnapshotLines(), 0, "ABC",
                 "unknown single-byte escapes should not swallow following plain text");
}

void TestTerminalSessionStringPayloadsDoNotLeakToGrid() {
  const std::array<std::string, 4> sequences = {
      std::string("A\x1bPq#0;2;0;0;0\x1b\\B"),
      std::string("A\x1bX hidden sos \x1b\\B"),
      std::string("A\x1b^ private message \x1b\\B"),
      std::string("A\x1b_Gi=1,a=q\x07""B"),
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

  std::string output = "A\x1b]0;";
  output.append(20000, 'x');
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

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[99999999999;99999999999HX");
  const auto lines = session.SnapshotLines();
  bool found_x = false;
  for (const microide::terminal::TerminalLine& line : lines) {
    if (LineText(line).find('X') != std::string::npos) {
      found_x = true;
      break;
    }
  }
  Expect(found_x, "an overflowing CSI parameter should clamp and still render following text");
}

void TestTerminalSessionEraseCharsClampsToWidth() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\x1b[65535X");
  const auto lines = session.SnapshotLines();
  Expect(!lines.empty(), "session should retain rows after ECH");
  Expect(lines[0].cells.size() <= 80,
         "CSI Ps X must clamp the erased range to the terminal width, not balloon the row");
}

void TestTerminalSessionInsertCharsClampsToWidth() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\x1b[65535@");
  const auto lines = session.SnapshotLines();
  Expect(!lines.empty(), "session should retain rows after ICH");
  Expect(lines[0].cells.size() <= 80,
         "CSI Ps @ must clamp the inserted cell count to the terminal width");
}

void TestTerminalSessionTabulationClampsToWidth() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\x1b[65535I");
  Expect(session.cursor_column() == 79,
         "CSI Ps I should saturate at the last column without work proportional to Ps");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[65535Z");
  Expect(session.cursor_column() == 0,
         "CSI Ps Z should saturate at column zero without work proportional to Ps");
}

void TestTerminalSessionTabDoesNotOverwriteCells() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "ABCDEFGHIJ\r\tX");
  const auto lines = session.SnapshotLines();
  Expect(!lines.empty() && lines[0].cells.size() >= 10,
         "the row should still hold all ten written cells");
  const auto& cells = lines[0].cells;
  const char* const preserved = "ABCDEFGH";
  bool intact = true;
  for (int i = 0; i < 8; ++i) {
    if (cells[static_cast<std::size_t>(i)].ascii_character() != preserved[i]) {
      intact = false;
      break;
    }
  }
  Expect(intact, "HT must not overwrite the glyphs it tabs across with spaces");
  Expect(cells[8].ascii_character() == 'X',
         "the glyph written after the tab lands at the tab stop (col 8)");
  Expect(cells[9].ascii_character() == 'J',
         "the cell beyond the tab stop is untouched");
}

void TestTerminalSessionEraseSavedLinesKeepsVisibleScreen() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5");
  const auto before = session.SnapshotLines();
  Expect(before.size() > 3,
         "six lines into a 3-row terminal should accumulate scrollback");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3J");
  const auto after = session.SnapshotLines();
  Expect(after.size() == 3,
         "CSI 3J must trim the deque to the visible screen height (scrollback gone)");
  Expect(after.front().cells.size() >= 2 && after.front().cells[0].ascii_character() == 'L' &&
             after.front().cells[1].ascii_character() == '3',
         "3J must preserve the first visible line ('L3')");
  Expect(after.back().cells.size() >= 2 && after.back().cells[0].ascii_character() == 'L' &&
             after.back().cells[1].ascii_character() == '5',
         "3J must preserve the last visible line ('L5')");
}

void TestTerminalSessionEraseSavedLinesAccountsScrollbackTrim() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5");
  Expect(session.ScrollbackTrimTotal() == 0,
         "no natural scrollback trim should have occurred within the default cap yet");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3J");
  Expect(session.ScrollbackTrimTotal() == 3,
         "ED3 must account its front-trim in ScrollbackTrimTotal so workspace mirrors rebase");
}

void TestTerminalSessionPrivateCursorPositionReportIsScreenRelative() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5\x1b[?6n");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[?3;3R",
         "DECXCPR (?6n) must report the screen-relative row, not the absolute deque row");
}

void TestTerminalSessionHardNewlineOntoExistingRowKeepsWrapFlag() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 4);

  TerminalSessionTestAccess::AppendOutput(session, "ABCDE\x1b[A\n");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() >= 2, "soft-wrap fixture should preserve the wrapped second row");
  Expect(lines[1].wrapped_from_previous,
         "a hard LF onto an existing wrapped row must not clear its soft-wrap flag");
}

void TestTerminalSessionInsertLineOutsideRegionIsNoOpOnAltScreen() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;3r\x1b[1;1H\x1b[L");
  auto lines = session.SnapshotLines();
  Expect(lines.size() == 4, "IL above the scroll margins must not grow the alt grid");
  ExpectLineText(lines, 0, "A", "IL outside the region must leave row 0 untouched");
  ExpectLineText(lines, 3, "D", "IL outside the region must leave the bottom row untouched");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[4;1H\x1b[M");
  lines = session.SnapshotLines();
  Expect(lines.size() == 4, "DL below the scroll margins must not shrink/resize the alt grid");
  ExpectLineText(lines, 0, "A", "DL outside the region must leave row 0 untouched");
  ExpectLineText(lines, 3, "D", "DL outside the region must leave the bottom row untouched");
}

void TestTerminalSessionReportsPendingWrapColumnClamped() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;80HX\x1b[6n");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[1;80R",
         "CPR at the pending-wrap column must report column 80, not 81");
}

void TestTerminalSessionResizePreservesCustomTabStops() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;4H\x1bH");
  session.Resize(24, 100);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\t");
  Expect(session.cursor_column() == 3,
         "a resize must preserve custom HTS tab stops, not reset to every-8");
}

void TestTerminalSessionInsertLinesClampsToHeight() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\x1b[65535L");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() <= 100,
         "CSI Ps L must clamp the inserted line count to the screen height, not 65535");
}

void TestTerminalSessionCursorDownClampsPrimaryScreen() {
  microide::util::ResetPerformanceCounters();
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[65535B");
  const std::uint64_t allocated = microide::util::ReadPerformanceCounter(
      microide::util::PerfCounterId::TerminalScrollbackLinesAllocated);
  Expect(allocated < 10000,
         "CSI B cursor-down must clamp cursor_row_ to the scrollback ceiling, "
         "not balloon the line deque to the escape parameter");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() < 10000,
         "primary-screen line deque must stay bounded after a huge CSI B");
}

void TestTerminalSessionMouseRoutingRequiresTrackingMode() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::SetRunning(session, true);

  Expect(!session.SendMouseButton(microide::terminal::TerminalSession::MouseButton::Left, true, 1,
                                  2, microide::util::kKeyModNone),
         "mouse routing should stay disabled until a tracking mode is enabled");
  Expect(TerminalSessionTestAccess::SentBytes(session).empty(),
         "disabled mouse routing should not emit bytes");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1000h");
  Expect(session.SendMouseButton(microide::terminal::TerminalSession::MouseButton::Left, true, 1, 2,
                                 microide::util::kKeyModNone),
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

void TestTerminalSessionEraseDisplayAppliesBackgroundColorErase() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[44m\x1b[2J");

  const auto lines = session.SnapshotLines();
  bool any_blue = false;
  for (const auto& line : lines) {
    for (const auto& cell : line.cells) {
      if (cell.style.background.has_value()) {
        any_blue = true;
      }
    }
  }
  Expect(any_blue,
         "clearing the screen under a non-default background must retain that "
         "background on the erased rows (BCE)");
}

void TestTerminalSessionScrollRegionAppliesBackgroundColorErase() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[44m\x1b[S");

  const auto lines = session.SnapshotLines();
  Expect(!lines.empty(), "alt screen should have rows after scroll");
  const auto& freed = lines.back();
  bool freed_has_background = false;
  for (const auto& cell : freed.cells) {
    if (cell.style.background.has_value()) {
      freed_has_background = true;
    }
  }
  Expect(freed_has_background,
         "scrolling a region under a non-default background must paint the freed "
         "row with that background (BCE)");
}

void TestTerminalSessionIndexPreservesColumn() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  TerminalSessionTestAccess::AppendOutput(session, "abc");
  Expect(session.cursor_row() == 0 && session.cursor_column() == 3,
         "cursor should sit at column 3 after printing 'abc'");

  TerminalSessionTestAccess::AppendOutput(session, "\033D");
  Expect(session.cursor_row() == 1 && session.cursor_column() == 3,
         "IND (ESC D) must move down one row and preserve the column");

  TerminalSessionTestAccess::AppendOutput(session, "\033E");
  Expect(session.cursor_row() == 2 && session.cursor_column() == 0,
         "NEL (ESC E) must move down and reset the column to 0");
}

void TestTerminalSessionEscInsideOscRestartsSequence() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "HELLO");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]0;title\x1b[2J");

  const auto lines = session.SnapshotLines();
  std::string all;
  for (const auto& line : lines) {
    all += LineText(line);
  }
  Expect(all.find("HELLO") == std::string::npos,
         "the clear-screen after an unterminated OSC must run (ESC restarts parsing)");
  Expect(all.find("2J") == std::string::npos && all.find("title") == std::string::npos,
         "OSC payload and the following CSI must not leak as literal text");
}

void TestTerminalSessionRestartResetsNegotiatedState() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[>5u");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2026h");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3g");
  TerminalSessionTestAccess::AppendOutput(session, "\x1bH");
  Expect(TerminalSessionTestAccess::KittyKeyboardFlags(session) == 5,
         "kitty keyboard flags should be negotiated before restart");
  Expect(TerminalSessionTestAccess::SynchronizedOutput(session),
         "synchronized output should be enabled before restart");

  session.StartPlaceholderForTesting(std::filesystem::path("/tmp"), "");

  Expect(TerminalSessionTestAccess::KittyKeyboardFlags(session) == 0,
         "restart must clear negotiated kitty keyboard flags");
  Expect(!TerminalSessionTestAccess::SynchronizedOutput(session),
         "restart must clear synchronized output mode");
  Expect(TerminalSessionTestAccess::TabStopCount(session) == 0,
         "restart must clear custom tab stops");
}

void TestTerminalSessionEscInsideCsiAbortsSequence() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  TerminalSessionTestAccess::AppendOutput(session, "HELLO");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[\x1b[2J");

  const auto lines = session.SnapshotLines();
  std::string all;
  for (const auto& line : lines) {
    all += LineText(line);
  }
  Expect(all.find("2J") == std::string::npos,
         "an ESC-cancelled CSI must not leak its final bytes as literal text");
  Expect(all.find("HELLO") == std::string::npos,
         "the second CSI (clear screen) must actually run after the ESC cancel");
}

void TestTerminalCellIsTriviallyCopyableAndCompact() {
  using microide::terminal::TerminalCell;
  static_assert(std::is_trivially_copyable_v<TerminalCell>,
                "TerminalCell must be trivially copyable to support bulk snapshot copies");
  static_assert(sizeof(TerminalCell) <= 64,
                "TerminalCell footprint regressed; inline storage and trivial copyability are the "
                "point of Finding 8");
  static_assert(sizeof(TerminalCell) == 18,
                "TerminalCell grew past the free padding; widening inline storage now costs "
                "memory on every scrollback cell");

  TerminalCell ascii;
  ascii.SetAscii('Q');
  Expect(ascii.length == 1 && ascii.bytes[0] == 'Q',
         "SetAscii should record a single-byte glyph at bytes[0]");
  Expect(ascii.DisplayText() == "Q",
         "DisplayText must return the inline byte for an ASCII cell");
  Expect(ascii.ascii_character() == 'Q',
         "ascii_character must mirror the stored ASCII byte");

  TerminalCell utf8;
  utf8.SetUtf8("\xE2\x9C\x93");
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

void TestTerminalSessionOverwritingWideGlyphClearsStalePair() {
  {
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 4, 8);
    TerminalSessionTestAccess::AppendOutput(session, "\xE6\x88\x91");
    TerminalSessionTestAccess::AppendOutput(session, "\rA");
    const auto lines = session.SnapshotLines();
    Expect(lines[0].cells[0].ascii_character() == 'A' && !lines[0].cells[0].style.wide_trailing(),
           "the overwriting narrow glyph should replace the wide lead");
    Expect(!lines[0].cells[1].style.wide_trailing() && lines[0].cells[1].ascii_character() == ' ',
           "the orphaned trailing spacer must be blanked, not left as a wide-trailing cell");
  }
  {
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 4, 8);
    TerminalSessionTestAccess::AppendOutput(session, "\xE6\x88\x91");
    TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;2HB");
    const auto lines = session.SnapshotLines();
    Expect(lines[0].cells[1].ascii_character() == 'B',
           "the overwriting glyph should land on the former spacer column");
    Expect(!lines[0].cells[0].style.wide_trailing() && lines[0].cells[0].ascii_character() == ' ',
           "the orphaned wide lead must be blanked so it no longer paints across two columns");
  }
}

void TestTerminalSessionIgnoresDelInOutputStream() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "abc\x7f");
  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "abc", "a stray DEL must leave the display unchanged");
  Expect(session.cursor_column() == 3, "DEL must not move the cursor");
}

void TestTerminalSessionTabAtPendingWrapDoesNotMoveBackward() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "abcdefgh");
  Expect(session.cursor_column() == 8, "precondition: cursor is at the pending-wrap column");
  TerminalSessionTestAccess::AppendOutput(session, "\t");
  Expect(session.cursor_column() == 8, "a tab at pending-wrap must not snap the cursor backward");
}

void TestTerminalSessionPendingWrapColumnActsAsLastColumn() {
  const auto trimmed_row = [](const microide::terminal::TerminalSession& session) {
    std::string text = LineText(session.SnapshotLines()[0]);
    while (!text.empty() && text.back() == ' ') {
      text.pop_back();
    }
    return text;
  };
  const auto fresh = [](microide::terminal::TerminalSession& session) {
    TerminalSessionTestAccess::Reset(session, 4, 8);
    TerminalSessionTestAccess::AppendOutput(session, "abcdefgh");
    Expect(session.cursor_column() == 8, "precondition: the cursor is at the pending-wrap column");
  };
  {
    microide::terminal::TerminalSession session;
    fresh(session);
    TerminalSessionTestAccess::AppendOutput(session, "\x1b[DX");
    ExpectLineText(session.SnapshotLines(), 0, "abcdefXh",
                   "CUB 1 from pending wrap lands on the second-to-last column");
  }
  {
    microide::terminal::TerminalSession session;
    fresh(session);
    TerminalSessionTestAccess::AppendOutput(session, "\x1b[K");
    Expect(trimmed_row(session) == "abcdefg", "EL from pending wrap erases the last glyph");
    Expect(session.cursor_column() == 7, "and leaves the cursor on the last column");
  }
  {
    microide::terminal::TerminalSession session;
    fresh(session);
    TerminalSessionTestAccess::AppendOutput(session, "\x1b[P");
    Expect(trimmed_row(session) == "abcdefg", "DCH from pending wrap deletes the last glyph");
  }
  {
    microide::terminal::TerminalSession session;
    fresh(session);
    TerminalSessionTestAccess::AppendOutput(session, "\bX");
    ExpectLineText(session.SnapshotLines(), 0, "abcdefXh",
                   "backspace from pending wrap steps to the second-to-last column");
  }
  {
    microide::terminal::TerminalSession session;
    fresh(session);
    TerminalSessionTestAccess::AppendOutput(session, "\x1b" "7\r\x1b" "8X");
    ExpectLineText(session.SnapshotLines(), 0, "abcdefgX",
                   "a cursor saved at pending wrap restores onto the last column");
  }
  {
    microide::terminal::TerminalSession session;
    fresh(session);
    TerminalSessionTestAccess::AppendOutput(session, "\x1b[1mX");
    const auto lines = session.SnapshotLines();
    ExpectLineText(lines, 0, "abcdefgh", "SGR between the last glyph and the next keeps the wrap");
    ExpectLineText(lines, 1, "X", "so the next glyph still wraps to the next row");
  }
}

void TestTerminalSessionEraseWholeLineDoesNotOvergrowAtPendingWrap() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "abcdefgh");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2K");
  const auto lines = session.SnapshotLines();
  Expect(lines[0].cells.size() == 8,
         "erase-whole-line must span exactly the terminal width, not columns_ + 1");
}

void TestTerminalSessionClearPreservesScrollback() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5");
  Expect(session.SnapshotLines().size() > 3, "six lines into a 3-row screen builds scrollback");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[H\x1b[2J");
  const auto after = session.SnapshotLines();
  Expect(after.size() >= 6, "2J must keep the scrollback deque, not collapse it to the screen");
  ExpectLineText(after, 0, "L0", "2J must preserve the first scrollback line");
  ExpectLineText(after, 1, "L1", "2J must preserve scrollback");
  ExpectLineText(after, 2, "L2", "2J must preserve the last scrollback line");
  Expect(LineText(after[after.size() - 1]).empty(), "2J must blank the visible screen");
  Expect(LineText(after[after.size() - 3]).empty(), "2J must blank the whole visible screen");
}

void TestTerminalSessionPrimaryCupIsViewportRelative() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[HX");
  const auto after = session.SnapshotLines();
  ExpectLineText(after, 0, "L0", "CUP home must not reach into scrollback");
  Expect(after.size() >= 3, "the deque must stay intact");
  ExpectLineText(after, after.size() - 3, "X3",
                 "CUP home targets the visible-screen top (overwrites L3's first cell -> X3)");
}

void TestTerminalSessionEraseToLineStartDoesNotOvergrowAtPendingWrap() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "abcdefgh");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1K");
  const auto lines = session.SnapshotLines();
  Expect(lines[0].cells.size() == 8,
         "erase-to-line-start must not grow the row to columns_ + 1 at the pending-wrap column");
}

void TestTerminalSessionScrollRegionReexpandsOnGrow() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049h");
  session.Resize(8, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[H");
  for (int i = 0; i < 8; ++i) {
    TerminalSessionTestAccess::AppendOutput(session, i == 0 ? "r0" : "\r\nr");
  }
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[8;1Hbottom");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 8, "the alt grid must be the full new height");
  ExpectLineText(lines, 7, "bottom",
                 "CUP to the new last row must address a real row after the region re-expands");
}

void TestTerminalSessionSoftWrapOntoExistingRowMarksContinuation() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 4);
  TerminalSessionTestAccess::AppendOutput(session, "a\r\nb\r\nc");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1Hwxyz1");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() >= 2, "primary buffer should retain the wrapped rows");
  Expect(lines[1].wrapped_from_previous,
         "a soft wrap onto a pre-existing row must mark it as a continuation");
}

void TestTerminalSessionHardLineFeedDoesNotRelabelWrappedRow() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 2);
  TerminalSessionTestAccess::AppendOutput(session, "abc");
  {
    const auto lines = session.SnapshotLines();
    Expect(lines.size() >= 2 && lines[1].wrapped_from_previous,
           "the wrapped row should start as a continuation");
  }
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\n");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() >= 2 && lines[1].wrapped_from_previous,
         "a hard LF onto an existing wrapped row must not relabel it as a hard start");
}

void TestTerminalSessionInvalidUtf8StoredAsReplacementChar() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 2, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\xE0\x80\x80");
  {
    const auto lines = session.SnapshotLines();
    Expect(!lines.empty() && !lines[0].cells.empty(), "the glyph should occupy a cell");
    Expect(lines[0].cells[0].DisplayText() == "\xEF\xBF\xBD",
           "an invalid scalar must be stored as the replacement character, not raw bytes");
  }
  TerminalSessionTestAccess::Reset(session, 2, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\xC3\xA9");
  const auto lines = session.SnapshotLines();
  Expect(!lines.empty() && !lines[0].cells.empty() &&
             lines[0].cells[0].DisplayText() == "\xC3\xA9",
         "a valid multibyte glyph must be stored unchanged");
}

}

void TestTerminalSessionCursorUpClampsToVisibleScreenTop() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, "L0\nL1\nL2\nL3\nL4\nL5");
  Expect(session.cursor_row() == 5, "cursor should sit on the last written primary row");

  TerminalSessionTestAccess::AppendOutput(session, "\r\x1b[4AX");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 6, "primary scrollback must be preserved across the cursor move");
  ExpectLineText(lines, 1, "L1", "CUU must not climb into scrollback and overwrite history");
  ExpectLineText(lines, 3, "X3", "CUU clamps at the visible-screen top");
}

void TestTerminalSessionVerticalTabAndFormFeedIndexLikeLineFeed() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "a\x0b""b\x0c""c");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 3, "VT and FF should each open a new row like an index");
  ExpectLineText(lines, 0, "a", "content before VT stays on the first row");
  Expect(lines[1].cells.size() >= 2 && lines[1].cells[1].ascii_character() == 'b',
         "VT indexes down preserving the column (b staircases to column 1)");
  Expect(lines[1].cells[0].ascii_character() != 'b',
         "VT must not reset the column to 0");
  Expect(lines[2].cells.size() >= 3 && lines[2].cells[2].ascii_character() == 'c',
         "FF indexes down preserving the column (c staircases to column 2)");
}

void TestTerminalSessionSaveRestoreCursorRestoresGraphicRendition() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b""7\x1b[31mRED\x1b""8X");

  const auto lines = session.SnapshotLines();
  Expect(!lines.empty() && !lines[0].cells.empty(), "restored write should land on the first row");
  Expect(lines[0].cells[0].ascii_character() == 'X',
         "DECRC should restore the cursor to the pre-save position");
  Expect(!lines[0].cells[0].style.foreground.has_value(),
         "DECRC should restore the default graphic rendition saved by DECSC");
}

void TestTerminalSessionSavedCursorRowIsScreenRelative() {
  for (const auto& [save, restore] : {std::pair<std::string_view, std::string_view>{"\x1b""7", "\x1b""8"},
                                      {"\x1b[s", "\x1b[u"}}) {
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 3, 8);
    TerminalSessionTestAccess::AppendOutput(
        session, std::string(save) + "\x1b[31ma\r\nb\r\nc\r\nd\r\ne" + std::string(restore) + "X");
    const auto lines = session.SnapshotLines();
    Expect(lines.size() == 5, "five lines, two of them scrollback");
    Expect(lines.size() == 5 && lines[0].cells.size() > 0 && lines[0].cells[0].ascii_character() == 'a',
           "the scrollback line the cursor was saved on is untouched");
    Expect(lines.size() == 5 && lines[2].cells.size() > 0 && lines[2].cells[0].ascii_character() == 'X',
           "the restore lands on the screen's top row, where the save was");
    Expect(lines.size() == 5 && lines[2].cells.size() > 0 && !lines[2].cells[0].style.foreground.has_value(),
           "and restores the saved rendition");
  }
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 8);
  std::string output = "\r\n\r\n\x1b""7";
  for (int i = 0; i < 4000; ++i) {
    output += "\r\nline";
  }
  output += "\x1b""8X";
  TerminalSessionTestAccess::AppendOutput(session, output);
  const auto lines = session.SnapshotLines();
  Expect(!lines.empty() && !lines.back().cells.empty() && lines.back().cells[0].ascii_character() == 'X',
         "after the scrollback was trimmed the restore is still the screen's bottom row");
}

void TestTerminalSessionEraseAndDeleteTakeAWideGlyphWhole() {
  microide::terminal::TerminalSession session;
  const std::string text = "ab\xe4\xb8\xad" "cd";
  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, text + "\x1b[1;4H\x1b[K");
  Expect(LineText(session.SnapshotLines()[0]).rfind("ab ", 0) == 0 &&
             LineText(session.SnapshotLines()[0]).find("\xe4\xb8\xad") == std::string::npos,
         "EL from the spacer erases the lead too");
  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, text + "\x1b[1;3H\x1b[X");
  ExpectLineText(session.SnapshotLines(), 0, "ab  cd", "ECH of the lead erases its spacer");
  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, text + "\x1b[1;4H\x1b[P");
  ExpectLineText(session.SnapshotLines(), 0, "ab cd", "DCH of the spacer blanks the lead");

  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[44mabcdefgh\x1b[1;2H\x1b[2P");
  const auto lines = session.SnapshotLines();
  Expect(lines[0].cells.size() == 8 && lines[0].cells[6].style.background.has_value() &&
             lines[0].cells[7].style.background.has_value(),
         "the two vacated cells take the current background");
  ExpectLineText(lines, 0, "adefgh  ", "and the rest shifts left");
}

void TestTerminalSessionDecSpecialGraphicsDrawsBoxes() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 12);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b(0lqqk\x1b(Bok\r\n\x1b(0x\x1b(B x");
  auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "\u250c\u2500\u2500\u2510ok", "the box's top draws, and ESC ( B is ASCII again");
  ExpectLineText(lines, 1, "\u2502 x", "a side");

  TerminalSessionTestAccess::Reset(session, 4, 12);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b)0a\x0eq\x0fq");
  ExpectLineText(session.SnapshotLines(), 0, "a\u2500q", "SO shifts G1 in, SI shifts it out");

  TerminalSessionTestAccess::Reset(session, 4, 12);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b(0\x1b""7\x1b(Bq\x1b""8q");
  ExpectLineText(session.SnapshotLines(), 0, "\u2500", "DECRC restores the set DECSC saved");

  TerminalSessionTestAccess::Reset(session, 4, 12);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b(0ABC\x1b(B");
  ExpectLineText(session.SnapshotLines(), 0, "ABC", "bytes outside the graphics range stay as they are");
}

void TestTerminalSessionPrimaryScrollsAndSoftReset() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, "a\r\nb\r\nc\x1b[SX");
  auto lines = session.SnapshotLines();
  Expect(lines.size() == 4, "SU scrolls the top row into scrollback");
  ExpectLineText(lines, 0, "a", "the row SU scrolled off is history");
  Expect(lines.size() == 4 && lines[3].cells.size() == 2 && lines[3].cells[1].ascii_character() == 'X',
         "and the cursor keeps its screen row and column");

  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, "a\r\nb\r\nc\x1b[T");
  lines = session.SnapshotLines();
  Expect(lines.size() == 3 && lines[0].cells.empty(), "SD opens a blank row at the screen's top");
  ExpectLineText(lines, 1, "a", "and moves the screen down");
  ExpectLineText(lines, 2, "b", "dropping the bottom row");

  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session,
                                          "abc\x1b[4h\x1b[2;3r\x1b[?6h\x1b(0\x1b[31m\x1b[!p\x1b[1;1Hq");
  lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "qbc", "after DECSTR: no insert mode, no origin mode, ASCII");
  Expect(!lines[0].cells.empty() && !lines[0].cells[0].style.foreground.has_value(), "and the default rendition");
  Expect(TerminalSessionTestAccess::ScrollRegionBottom(session) == 3, "and the full-screen margins");
}

void TestTerminalSessionFullResetAndReverseVideo() {
  microide::terminal::TerminalSession session;
  const auto reply_to = [&](std::string_view query) {
    TerminalSessionTestAccess::ClearSentBytes(session);
    TerminalSessionTestAccess::AppendOutput(session, query);
    return TerminalSessionTestAccess::SentBytes(session);
  };
  TerminalSessionTestAccess::Reset(session, 4, 10);
  Expect(reply_to("\x1b[?5$p") == "\x1b[?5;2$y", "DECSCNM is a known mode, reset at start");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?5h");
  Expect(session.screen_reverse_video(), "CSI ? 5 h reverses the screen");
  Expect(reply_to("\x1b[?5$p") == "\x1b[?5;1$y", "and DECRQM reads it set");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?5l");
  Expect(!session.screen_reverse_video(), "CSI ? 5 l restores it");

  TerminalSessionTestAccess::Reset(session, 4, 10);
  TerminalSessionTestAccess::AppendOutput(
      session, "shell$\r\n\x1b[?1049h\x1b[4h\x1b[?1h\x1b[?2004h\x1b[?5h\x1b[?25l\x1b[2;3r\x1b(0\x1b[31mvim");
  Expect(session.using_alternate_screen(), "the program is on the alternate screen");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b" "c" "qx");
  Expect(!session.using_alternate_screen(), "RIS returns to the primary screen");
  Expect(!session.screen_reverse_video() && session.CursorSnapshot().visible,
         "with reverse video off and the cursor shown");
  Expect(reply_to("\x1b[?2004$p") == "\x1b[?2004;2$y" && reply_to("\x1b[?1$p") == "\x1b[?1;2$y",
         "bracketed paste and application cursor keys are off");
  Expect(TerminalSessionTestAccess::ScrollRegionBottom(session) == 3, "the margins are the screen's");
  const auto lines = session.SnapshotLines();
  const std::size_t top = lines.size() >= 4 ? lines.size() - 4 : 0;
  ExpectLineText(lines, top, "qx", "the screen is cleared, the cursor home, no insert mode, ASCII");
  Expect(lines.size() > top && !lines[top].cells.empty() &&
             !lines[top].cells[0].style.foreground.has_value(),
         "and the rendition is the default");
}

void TestTerminalSessionRepeatAndInsertModeFromTheAdvertisedTerminfo() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 12);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[5bab\x1b[3bc");
  ExpectLineText(session.SnapshotLines(), 0, "abbbbc", "REP repeats the last glyph; before any glyph it is ignored");

  TerminalSessionTestAccess::Reset(session, 3, 12);
  TerminalSessionTestAccess::AppendOutput(session, "\xe2\x94\x80\x1b[4b|");
  ExpectLineText(session.SnapshotLines(), 0, "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80|",
                 "a box rule repeats too");

  TerminalSessionTestAccess::Reset(session, 3, 4);
  TerminalSessionTestAccess::AppendOutput(session, "x\x1b[2147483647b");
  Expect(session.SnapshotLines().size() <= 4, "a huge count is bounded by the screen");

  TerminalSessionTestAccess::Reset(session, 4, 4);
  TerminalSessionTestAccess::AppendOutput(session, "ab\x1b[8bZ");
  auto wrapped = session.SnapshotLines();
  ExpectLineText(wrapped, 0, "abbb", "the repeat fills the first row");
  ExpectLineText(wrapped, 1, "bbbb", "wraps onto the next");
  ExpectLineText(wrapped, 2, "bbZ", "and the glyph after it follows the last repeat");
  Expect(wrapped.size() > 1 && wrapped[1].wrapped_from_previous, "the wrap is a soft wrap");

  TerminalSessionTestAccess::Reset(session, 3, 10);
  TerminalSessionTestAccess::AppendOutput(session, "abcdef\r\x1b[4hXY\x1b[4lZ");
  ExpectLineText(session.SnapshotLines(), 0, "XYZbcdef", "IRM inserts; after rmir the next glyph overwrites");

  TerminalSessionTestAccess::Reset(session, 3, 6);
  TerminalSessionTestAccess::AppendOutput(session, "abcdef\r\x1b[4hX");
  ExpectLineText(session.SnapshotLines(), 0, "Xabcde", "what an insert pushes past the margin is lost");

  TerminalSessionTestAccess::Reset(session, 3, 6);
  TerminalSessionTestAccess::AppendOutput(session, "abcd\xe4\xb8\xad\r\x1b[@");
  ExpectLineText(session.SnapshotLines(), 0, " abcd ", "the wide glyph whose spacer left is blanked");
}

void TestTerminalSessionOriginModeAppliesOnThePrimaryScreen() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 5, 8);
  TerminalSessionTestAccess::AppendOutput(session, "s0\r\ns1\r\n0\r\n1\r\n2\r\n3\r\n4");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;4r\x1b[?6hA\x1b[9;1HB");
  auto lines = session.SnapshotLines();
  ExpectLineText(lines, 3, "A", "DECOM homes to the region's top (screen row 2)");
  ExpectLineText(lines, 5, "B", "a CUP past the region stops at its bottom (screen row 4)");
  ExpectLineText(lines, 0, "s0", "scrollback is untouched");
  TerminalSessionTestAccess::ClearSentBytes(session);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[6n");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[3;2R",
         "CPR reports relative to the region: " + TerminalSessionTestAccess::SentBytes(session));
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?6l\x1b[1;1HC");
  lines = session.SnapshotLines();
  ExpectLineText(lines, 2, "C", "with DECOM reset CUP counts from the screen's top again");
}

void TestTerminalSessionCursorMovesStopAtTheScreensBottomRow() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 8);
  std::string output;
  for (int i = 0; i < 4000; ++i) {
    output += "line\r\n";
  }
  output += "\x1b[HX";
  TerminalSessionTestAccess::AppendOutput(session, output);
  auto lines = session.SnapshotLines();
  Expect(lines.size() >= 3 && !lines[lines.size() - 3].cells.empty() &&
             lines[lines.size() - 3].cells[0].ascii_character() == 'X',
         "CUP in an over-long chunk lands on the screen's top row");

  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, "a\r\nb\r\nc\x1b[5BX");
  lines = session.SnapshotLines();
  Expect(lines.size() == 3, "CUD on the bottom row does not scroll");
  Expect(lines.size() == 3 && lines[2].cells.size() > 1 && lines[2].cells[1].ascii_character() == 'X',
         "it stays on the bottom row");
}

void TestTerminalSessionOriginModeToggleHomesToVisibleScreenTop() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, "L0\nL1\nL2\nL3\nL4\nL5");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?6lX");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 6, "primary scrollback must be preserved across the origin-mode toggle");
  ExpectLineText(lines, 0, "L0", "origin-mode home must not climb into scrollback");
  ExpectLineText(lines, 3, "X3", "origin-mode home lands at the visible-screen top");
}

void TestTerminalSessionScrollbackTrimTotalTracksDroppedLines() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  session.SetMaxScrollbackLines(200);
  Expect(session.ScrollbackTrimTotal() == 0, "no trim before the cap is exceeded");

  std::string output;
  const int kLines = 300;
  for (int i = 0; i < kLines; ++i) {
    output += "line\n";
  }
  TerminalSessionTestAccess::AppendOutput(session, output);

  const std::uint64_t trimmed = session.ScrollbackTrimTotal();
  Expect(trimmed > 0, "exceeding the scrollback cap should trim front lines");
  Expect(session.LineCount() <= 251, "trimmed deque should collapse toward the cap");
  Expect(trimmed == static_cast<std::uint64_t>(kLines + 1) - session.LineCount(),
         "trim total should equal written lines minus surviving lines");
}

void TestTerminalSessionRecyclesTrimmedLineBuffers() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 80);
  session.SetMaxScrollbackLines(200);

  const auto feed_chunk = [&](int first, int count) {
    std::string output;
    for (int i = first; i < first + count; ++i) {
      output += "recycled scrollback line ";
      output += std::to_string(i);
      output += "\r\n";
    }
    TerminalSessionTestAccess::AppendOutput(session, output);
  };

  feed_chunk(0, 300);
  Expect(session.ScrollbackTrimTotal() > 0, "the first chunk trims");
  const std::size_t pooled = TerminalSessionTestAccess::LineBufferPoolSize(session);
  Expect(pooled > 0, "a trim hands its lines' cell buffers to the pool");

  const int kSecondChunkLines = 20;
  feed_chunk(300, kSecondChunkLines);
  Expect(session.LineCount() < 251, "the second chunk stays under the trim watermark");
  Expect(TerminalSessionTestAccess::LineBufferPoolSize(session) ==
             pooled - static_cast<std::size_t>(kSecondChunkLines),
         "the lines created after a trim drain the pool rather than allocating");

  const std::vector<microide::terminal::TerminalLine> lines = session.SnapshotLines();
  Expect(!lines.empty(), "the session still has lines");
  const auto text_of = [](const microide::terminal::TerminalLine& line) {
    std::string text;
    for (const microide::terminal::TerminalCell& cell : line.cells) {
      text.append(cell.bytes.data(), cell.length);
    }
    while (!text.empty() && text.back() == ' ') {
      text.pop_back();
    }
    return text;
  };
  bool found_last = false;
  for (const microide::terminal::TerminalLine& line : lines) {
    if (text_of(line) == "recycled scrollback line 319") {
      found_last = true;
    }
    const std::string text = text_of(line);
    Expect(text.empty() || text.rfind("recycled scrollback line ", 0) == 0,
           "a recycled buffer never leaks the previous line's cells");
  }
  Expect(found_last, "the most recent line is present and intact");
}

void TestTerminalSessionDecstbmIgnoresEqualMargins() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 6, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049hA\nB\nC\nD\nE\nF");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[5;5r\x1b[?6h\x1b[1;1HZ");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "Z",
                 "equal DECSTBM margins should be ignored, leaving the top margin at row 0");
  ExpectLineText(lines, 4, "E",
                 "an ignored one-line region must not become the effective scroll region");
}

void TestTerminalSessionDecstbmIgnoresInvertedMargins() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 8, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049hA\nB\nC\nD\nE\nF\nG\nH");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[8;3r\x1b[?6h\x1b[1;1HZ");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "Z",
                 "inverted DECSTBM margins should be ignored, leaving the top margin at row 0");
  ExpectLineText(lines, 2, "C",
                 "an ignored inverted region must not repurpose the bottom row as the top margin");
}

void TestTerminalSessionDecstbmFullScreenResetsRegion() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 6, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049hA\nB\nC\nD\nE\nF");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3;5r\x1b[r\x1b[?6h\x1b[1;1HZ");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "Z",
                 "full-screen CSI r should re-establish a valid region with the top margin at row 0");
  ExpectLineText(lines, 2, "C",
                 "full-screen CSI r must not leave the previous narrower region's top in effect");
}

void TestTerminalSessionCharsetDesignationAbortsOnEmbeddedEscape() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "X\x1b(\x1b[31mY");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "XY",
                 "the CSI after an aborted charset designation must execute, not print as text");
  Expect(!lines.empty() && lines[0].cells.size() >= 2 &&
             lines[0].cells[1].ascii_character() == 'Y' &&
             lines[0].cells[1].style.foreground.has_value(),
         "the SGR after the aborted designation applied (Y is colored)");
  Expect(!lines[0].cells[0].style.foreground.has_value(),
         "text before the designation keeps the default color");
}

void TestTerminalSessionAltScreenLineFeedBelowRegionDoesNotScroll() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;2r\x1b[4;1HZ\n");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 4, "alt-screen row count stays fixed");
  ExpectLineText(lines, 0, "A", "LF below the region must not scroll the top of the screen");
  ExpectLineText(lines, 1, "B", "region rows above are untouched");
  ExpectLineText(lines, 2, "C", "rows between the region and the status line are untouched");
  ExpectLineText(lines, 3, "Z", "the status row stays put; LF at the bottom does not scroll");
}

void TestTerminalSessionProcessExitMarkerLeavesAlternateScreen() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);
  TerminalSessionTestAccess::EmitProcessExitMarker(session);

  const auto lines = session.SnapshotLines();
  std::string joined;
  for (const auto& line : lines) {
    joined += LineText(line);
  }
  Expect(joined.find("[process exited]") != std::string::npos,
         "the process-exit marker must be shown");
  Expect(joined.find('D') == std::string::npos,
         "exiting on the alternate screen restores the primary buffer instead of "
         "appending the marker onto (and evicting) the alt grid");
}

void TestTerminalSessionBasicForegroundBrightnessTracksBold() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(
      session, "\x1b[1m\x1b[31mA\x1b[0m\x1b[31m\x1b[1mB\x1b[0m\x1b[1;31m\x1b[22mC\x1b[0m\x1b[31mD");

  const auto lines = session.SnapshotLines();
  Expect(!lines.empty() && lines[0].cells.size() >= 4, "A,B,C,D land on the first row");
  const auto color_of = [&](std::size_t col) { return lines[0].cells[col].style.foreground; };
  const auto same = [](microide::terminal::TerminalColor lhs,
                       microide::terminal::TerminalColor rhs) {
    return lhs.has_value() && rhs.has_value() && lhs->r == rhs->r && lhs->g == rhs->g &&
           lhs->b == rhs->b;
  };
  Expect(color_of(0).has_value() && color_of(3).has_value(), "foreground colors were applied");
  Expect(same(color_of(0), color_of(1)),
         "bold-then-color and color-then-bold yield the same bright red");
  Expect(same(color_of(2), color_of(3)),
         "SGR 22 reverts a bold-brightened basic foreground to the plain (dark) shade");
  Expect(!same(color_of(0), color_of(3)), "bright red must differ from dark red");
  Expect(color_of(3) == microide::util::BasicAnsiColor(1, false),
         "a plain SGR 31 foreground is exactly the shared palette's dark red");
  Expect(color_of(0) == microide::util::BasicAnsiColor(1, true),
         "a bold SGR 31 foreground is exactly the shared palette's bright red");
}

void TestTerminalSessionPrimaryScreenScrollRegionScrollsOnlyRegion() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "L0\nL1\nL2\nL3");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;3r\x1b[3;1H\n");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 4, "primary scroll region must not grow the screen");
  ExpectLineText(lines, 0, "L0", "rows above the region are untouched");
  ExpectLineText(lines, 1, "L2", "the region body scrolled up within the margins");
  ExpectLineText(lines, 2, "", "a blank line was exposed at the region bottom");
  ExpectLineText(lines, 3, "L3", "rows below the region are untouched");
}

void TestTerminalSessionDecstbmClampsOutOfRangeBottom() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;99r\x1b[4;1H\n");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "A", "an out-of-range bottom margin clamps, keeping the top row fixed");
  ExpectLineText(lines, 1, "C", "the clamped region scrolled its body up");
}

void TestTerminalSessionC0ControlExecutesMidCsi() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "A\x1b[\nmB");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 2, "the mid-CSI line feed opened a new row");
  ExpectLineText(lines, 0, "A", "content before the CSI stays on the first row");
  ExpectLineText(lines, 1, "B", "the mid-CSI line feed moved the cursor down before B printed");
}

void TestTerminalSessionMidCsiC0MatchesGroundState() {
  const auto text_after = [](std::string_view output) {
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 4, 8);
    TerminalSessionTestAccess::AppendOutput(session, output);
    const auto lines = session.SnapshotLines();
    return lines.empty() ? std::string() : LineText(lines.front());
  };
  Expect(text_after("ABCDEFGH\bX") == "ABCDEFXH", "ground: a backspace from the pending wrap clamps first");
  Expect(text_after("ABCDEFGH\x1b[\bmX") == "ABCDEFXH", "mid-CSI: the same backspace clamps the same way");
  const std::string ground = text_after("\x1b)0\x0eq");
  Expect(ground != "q", "ground: SO shifts G1's line drawing in");
  Expect(text_after("\x1b)0\x1b[\x0emq") == ground, "mid-CSI: SO shifts it in as well");
}

void TestTerminalSessionRestartResetsScrollRegionToDefaultHeight() {
  microide::terminal::TerminalSession session;
  session.StartPlaceholderForTesting(std::filesystem::path("/tmp"), "");
  session.Resize(50, 100);
  Expect(TerminalSessionTestAccess::ScrollRegionBottom(session) == 49,
         "the resized session's region spans its full 50-row height");

  session.StartPlaceholderForTesting(std::filesystem::path("/tmp"), "");
  Expect(session.rows() == 24, "restarting reseeds the 24-row default");
  Expect(TerminalSessionTestAccess::ScrollRegionBottom(session) == 23,
         "the restarted session's region follows the reseeded height, not the old one");
}

void TestTerminalSessionResizeReexpandsSavedScrollRegion() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049h");
  session.Resize(6, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049l");
  Expect(TerminalSessionTestAccess::ScrollRegionBottom(session) == 5,
         "the restored primary region re-expands to the new full height (row 5)");
}

void TestTerminalSessionRestoreClampsOversizedCursorRow() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  session.SetMaxScrollbackLines(2000);

  const std::size_t restored_line_count =
      TerminalSessionTestAccess::RestoreWithSavedPrimaryCursorRow(session, 50'000'000);

  Expect(restored_line_count <= 2000 + 24 + 1,
         "restore of an oversized cursor row must stay within the scrollback+visible budget");
  Expect(session.cursor_row() <= 2000 + 24,
         "the restored cursor row must be clamped to the scrollback ceiling");
}

void TestTerminalSessionAsciiRunFastPathMatchesPerByte() {
  microide::terminal::TerminalSession session;

  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "ABCDEFGH");
  auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "ABCDEFGH", "a run filling the line stays on that line");
  Expect(session.cursor_row() == 0, "a run filling the line must not wrap yet");
  Expect(session.cursor_column() == 8, "the cursor rests at the right margin");

  TerminalSessionTestAccess::AppendOutput(session, "IJ");
  lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "ABCDEFGH", "the filled line is untouched by the wrap");
  ExpectLineText(lines, 1, "IJ", "the deferred wrap continues on the next row");

  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "ABCDEFGHIJKLMNOPQRST");
  lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "ABCDEFGH", "an over-long run wraps at the margin");
  ExpectLineText(lines, 1, "IJKLMNOP", "an over-long run keeps wrapping");
  ExpectLineText(lines, 2, "QRST", "the remainder lands on the third row");

  TerminalSessionTestAccess::Reset(session, 4, 20);
  TerminalSessionTestAccess::AppendOutput(session, "ab\tcd\r\nef\x1b[1mgh");
  lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "ab      cd", "a tab inside a run still moves to the tab stop");
  ExpectLineText(lines, 1, "efgh", "an SGR escape splits a run without losing bytes");

  TerminalSessionTestAccess::Reset(session, 4, 20);
  TerminalSessionTestAccess::AppendOutput(session, "\xE6\x97\xA5\xE6\x9C\xAC ok");
  TerminalSessionTestAccess::AppendOutput(session, "\r");
  TerminalSessionTestAccess::AppendOutput(session, "xy");
  lines = session.SnapshotLines();
  Expect(LineText(lines[0]).rfind("xy", 0) == 0,
         "a run overwriting a wide glyph writes its own bytes at the start");
  Expect(LineText(lines[0]).find('\xE6') == std::string::npos ||
             LineText(lines[0]).find("\xE6\x9C\xAC") != std::string::npos,
         "the untouched second wide glyph survives; the overwritten one leaves no half-pair");

  TerminalSessionTestAccess::Reset(session, 4, 20);
  TerminalSessionTestAccess::AppendOutput(session, "ab\x7f" "cd");
  lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "abcd", "DEL inside a run is ignored, not printed");

  TerminalSessionTestAccess::Reset(session, 4, 20);
  for (const char c : std::string("chunked output")) {
    TerminalSessionTestAccess::AppendOutput(session, std::string_view(&c, 1));
  }
  lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "chunked output",
                 "byte-at-a-time delivery must produce the same row as one bulk write");
}

void TestTerminalSessionScreenAgreesWithPyteReference() {
  const std::string fixture = ReadFile(FixturePath("terminal/pyte-reference.txt"));
  Expect(!fixture.empty(), "the pyte reference fixture should be present");
  struct Case {
    std::string id;
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::string input;
    std::vector<std::string> screen;
    std::size_t cursor_row = 0;
    std::size_t cursor_col = 0;
  };
  std::vector<Case> cases;
  {
    std::size_t start = 0;
    while (start < fixture.size()) {
      std::size_t end = fixture.find('\n', start);
      if (end == std::string::npos) end = fixture.size();
      const std::string_view line(fixture.data() + start, end - start);
      start = end + 1;
      if (line.starts_with("case ")) {
        Case c;
        const auto parts = microide::util::SplitAsciiWhitespace(line);
        c.id = std::string(parts[1]);
        c.rows = static_cast<std::size_t>(std::stoul(std::string(parts[2].substr(5))));
        c.cols = static_cast<std::size_t>(std::stoul(std::string(parts[3].substr(5))));
        cases.push_back(std::move(c));
      } else if (line.starts_with("in ")) {
        const std::string_view hex = line.substr(3);
        std::string bytes;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
          bytes.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
        }
        cases.back().input = std::move(bytes);
      } else if (line.starts_with("| ")) {
        cases.back().screen.emplace_back(line.substr(2));
      } else if (line.starts_with("cursor ")) {
        const auto parts = microide::util::SplitAsciiWhitespace(line);
        cases.back().cursor_row = static_cast<std::size_t>(std::stoul(std::string(parts[1])));
        cases.back().cursor_col = static_cast<std::size_t>(std::stoul(std::string(parts[2])));
      }
    }
  }
  Expect(cases.size() > 100, "the fixture should carry a few hundred cases");

  const auto rtrim = [](std::string text) {
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
  };
  std::size_t failures = 0;
  std::string report;
  for (const Case& c : cases) {
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, c.rows, c.cols);
    TerminalSessionTestAccess::AppendOutput(session, c.input);
    const auto lines = session.SnapshotLines();
    const std::size_t first_visible = lines.size() > c.rows ? lines.size() - c.rows : 0;
    std::vector<std::string> got_screen(c.rows);
    for (std::size_t r = 0; r < c.rows; ++r) {
      const std::size_t index = first_visible + r;
      std::string text;
      if (index < lines.size()) {
        for (std::size_t col = 0; col < c.cols; ++col) {
          if (col < lines[index].cells.size() && lines[index].cells[col].length > 0) {
            text += std::string(lines[index].cells[col].DisplayText());
          } else {
            text += ' ';
          }
        }
      }
      got_screen[r] = rtrim(text);
    }
    const std::size_t got_row =
        session.cursor_row() >= first_visible ? session.cursor_row() - first_visible : 0;
    const std::size_t got_col = std::min(session.cursor_column(), c.cols - 1);
    bool same = got_row == c.cursor_row && got_col == c.cursor_col;
    for (std::size_t r = 0; r < c.rows && same; ++r) {
      same = got_screen[r] == rtrim(c.screen[r]);
    }
    if (same) continue;
    ++failures;
    if (failures <= 12) {
      report += "\ncase " + c.id + " rows=" + std::to_string(c.rows) +
                " cols=" + std::to_string(c.cols) + " cursor want " +
                std::to_string(c.cursor_row) + "," + std::to_string(c.cursor_col) + " got " +
                std::to_string(got_row) + "," + std::to_string(got_col) + "\n  in ";
      for (const unsigned char b : c.input) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x", b);
        report += buf;
      }
      for (std::size_t r = 0; r < c.rows; ++r) {
        report += "\n  want |" + rtrim(c.screen[r]) + "|  got |" + got_screen[r] + "|";
      }
    }
  }
  Expect(failures == 0,
         "screens agree with the pyte reference; " + std::to_string(failures) + " of " +
             std::to_string(cases.size()) + " cases differ:" + report);
}

void TestWideGlyphOnANarrowScreenStaysInsideTheMargin() {
  for (const std::size_t columns : {std::size_t{1}, std::size_t{2}}) {
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 3, columns);
    TerminalSessionTestAccess::AppendOutput(session, "\xe6\x97\xa5");

    Expect(session.cursor_column() <= columns,
           "the cursor must stay within the screen on a " + std::to_string(columns) +
               "-column terminal, got column " + std::to_string(session.cursor_column()));
    const auto lines = session.SnapshotLines();
    for (const auto& line : lines) {
      Expect(line.cells.size() <= columns,
             "no line may exceed the screen width on a " + std::to_string(columns) +
                 "-column terminal, got " + std::to_string(line.cells.size()) + " cells");
    }
    Expect(session.cursor_row() < session.LineCount(),
           "the cursor row must name a line that exists");
  }

  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 1);
  for (int i = 0; i < 8; ++i) {
    TerminalSessionTestAccess::AppendOutput(session, "\xe6\x97\xa5");
    Expect(session.cursor_column() <= 1,
           "repeated wide glyphs must not walk the cursor past the margin, got column " +
               std::to_string(session.cursor_column()) + " after " + std::to_string(i + 1) +
               " glyphs");
  }
}

void RegisterTerminalSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalSession/StampsOsc8LinksOnGlyphs", TestTerminalSessionStampsOsc8LinksOnGlyphs);
  AddTest(tests, "TerminalSession/KeepsUnderlineShapesAndColours",
          TestTerminalSessionKeepsUnderlineShapesAndColours);
  AddTest(tests, "TerminalSession/CellExtraTableRecyclesOnlyDeadIds",
          TestTerminalCellExtraTableRecyclesOnlyDeadIds);
  AddTest(tests, "TerminalSession/WideGlyphOnANarrowScreenStaysInsideTheMargin",
          TestWideGlyphOnANarrowScreenStaysInsideTheMargin);
  AddTest(tests, "TerminalSession/ScreenAgreesWithPyteReference",
          TestTerminalSessionScreenAgreesWithPyteReference);
  AddTest(tests, "TerminalSession/SwallowsUnknownCsiFinals",
          TestTerminalSessionSwallowsUnknownCsiFinals);
  AddTest(tests, "TerminalSession/AsciiRunFastPathMatchesPerByte",
          TestTerminalSessionAsciiRunFastPathMatchesPerByte);
  AddTest(tests, "TerminalSession/RestoreClampsOversizedCursorRow",
          TestTerminalSessionRestoreClampsOversizedCursorRow);
  AddTest(tests, "TerminalSession/RejectedWakeLatchesOwedBit",
          TestTerminalSessionRejectedWakeLatchesOwedBit);
  AddTest(tests, "TerminalSession/PrimaryScreenScrollRegionScrollsOnlyRegion",
          TestTerminalSessionPrimaryScreenScrollRegionScrollsOnlyRegion);
  AddTest(tests, "TerminalSession/DecstbmClampsOutOfRangeBottom",
          TestTerminalSessionDecstbmClampsOutOfRangeBottom);
  AddTest(tests, "TerminalSession/C0ControlExecutesMidCsi",
          TestTerminalSessionC0ControlExecutesMidCsi);
  AddTest(tests, "TerminalSession/MidCsiC0MatchesGroundState", TestTerminalSessionMidCsiC0MatchesGroundState);
  AddTest(tests, "TerminalSession/ResizeReexpandsSavedScrollRegion",
          TestTerminalSessionResizeReexpandsSavedScrollRegion);
  AddTest(tests, "TerminalSession/RestartResetsScrollRegionToDefaultHeight",
          TestTerminalSessionRestartResetsScrollRegionToDefaultHeight);
  AddTest(tests, "TerminalSession/CharsetDesignationAbortsOnEmbeddedEscape",
          TestTerminalSessionCharsetDesignationAbortsOnEmbeddedEscape);
  AddTest(tests, "TerminalSession/AltScreenLineFeedBelowRegionDoesNotScroll",
          TestTerminalSessionAltScreenLineFeedBelowRegionDoesNotScroll);
  AddTest(tests, "TerminalSession/ProcessExitMarkerLeavesAlternateScreen",
          TestTerminalSessionProcessExitMarkerLeavesAlternateScreen);
  AddTest(tests, "TerminalSession/BasicForegroundBrightnessTracksBold",
          TestTerminalSessionBasicForegroundBrightnessTracksBold);
  AddTest(tests, "TerminalSession/DecstbmIgnoresEqualMargins",
          TestTerminalSessionDecstbmIgnoresEqualMargins);
  AddTest(tests, "TerminalSession/DecstbmIgnoresInvertedMargins",
          TestTerminalSessionDecstbmIgnoresInvertedMargins);
  AddTest(tests, "TerminalSession/DecstbmFullScreenResetsRegion",
          TestTerminalSessionDecstbmFullScreenResetsRegion);
  AddTest(tests, "TerminalSession/ScrollbackTrimTotalTracksDroppedLines",
          TestTerminalSessionScrollbackTrimTotalTracksDroppedLines);
  AddTest(tests, "TerminalSession/RecyclesTrimmedLineBuffers",
          TestTerminalSessionRecyclesTrimmedLineBuffers);
  AddTest(tests, "TerminalSession/OriginModeToggleHomesToVisibleScreenTop",
          TestTerminalSessionOriginModeToggleHomesToVisibleScreenTop);
  AddTest(tests, "TerminalSession/CursorUpClampsToVisibleScreenTop",
          TestTerminalSessionCursorUpClampsToVisibleScreenTop);
  AddTest(tests, "TerminalSession/VerticalTabAndFormFeedIndexLikeLineFeed",
          TestTerminalSessionVerticalTabAndFormFeedIndexLikeLineFeed);
  AddTest(tests, "TerminalSession/SaveRestoreCursorRestoresGraphicRendition",
          TestTerminalSessionSaveRestoreCursorRestoresGraphicRendition);
  AddTest(tests, "TerminalSession/SavedCursorRowIsScreenRelative",
          TestTerminalSessionSavedCursorRowIsScreenRelative);
  AddTest(tests, "TerminalSession/CursorMovesStopAtTheScreensBottomRow",
          TestTerminalSessionCursorMovesStopAtTheScreensBottomRow);
  AddTest(tests, "TerminalSession/RepeatAndInsertModeFromTheAdvertisedTerminfo",
          TestTerminalSessionRepeatAndInsertModeFromTheAdvertisedTerminfo);
  AddTest(tests, "TerminalSession/OriginModeAppliesOnThePrimaryScreen",
          TestTerminalSessionOriginModeAppliesOnThePrimaryScreen);
  AddTest(tests, "TerminalSession/EraseAndDeleteTakeAWideGlyphWhole",
          TestTerminalSessionEraseAndDeleteTakeAWideGlyphWhole);
  AddTest(tests, "TerminalSession/DecSpecialGraphicsDrawsBoxes",
          TestTerminalSessionDecSpecialGraphicsDrawsBoxes);
  AddTest(tests, "TerminalSession/FullResetAndReverseVideo", TestTerminalSessionFullResetAndReverseVideo);
  AddTest(tests, "TerminalSession/PrimaryScrollsAndSoftReset",
          TestTerminalSessionPrimaryScrollsAndSoftReset);
  AddTest(tests, "TerminalSession/ClearPreservesScrollback",
          TestTerminalSessionClearPreservesScrollback);
  AddTest(tests, "TerminalSession/PrimaryCupIsViewportRelative",
          TestTerminalSessionPrimaryCupIsViewportRelative);
  AddTest(tests, "TerminalSession/EraseToLineStartDoesNotOvergrowAtPendingWrap",
          TestTerminalSessionEraseToLineStartDoesNotOvergrowAtPendingWrap);
  AddTest(tests, "TerminalSession/ScrollRegionReexpandsOnGrow",
          TestTerminalSessionScrollRegionReexpandsOnGrow);
  AddTest(tests, "TerminalSession/OverwritingWideGlyphClearsStalePair",
          TestTerminalSessionOverwritingWideGlyphClearsStalePair);
  AddTest(tests, "TerminalSession/IgnoresDelInOutputStream",
          TestTerminalSessionIgnoresDelInOutputStream);
  AddTest(tests, "TerminalSession/TabAtPendingWrapDoesNotMoveBackward",
          TestTerminalSessionTabAtPendingWrapDoesNotMoveBackward);
  AddTest(tests, "TerminalSession/PendingWrapColumnActsAsLastColumn",
          TestTerminalSessionPendingWrapColumnActsAsLastColumn);
  AddTest(tests, "TerminalSession/EraseWholeLineDoesNotOvergrowAtPendingWrap",
          TestTerminalSessionEraseWholeLineDoesNotOvergrowAtPendingWrap);
  AddTest(tests, "TerminalSession/CellIsTriviallyCopyableAndCompact",
          TestTerminalCellIsTriviallyCopyableAndCompact);
  AddTest(tests, "TerminalSession/LineFeedScrollRegion",
          TestTerminalSessionScrollsBottomMarginOnLineFeed);
  AddTest(tests, "TerminalSession/ReverseIndexScrollRegion",
          TestTerminalSessionReverseIndexScrollsTopMargin);
  AddTest(tests, "TerminalSession/PrimaryReverseIndexDoesNotClimbScrollback",
          TestTerminalSessionPrimaryReverseIndexDoesNotClimbScrollback);
  AddTest(tests, "TerminalSession/InsertLineScrollRegion",
          TestTerminalSessionInsertLineRespectsScrollRegion);
  AddTest(tests, "TerminalSession/DeleteLineScrollRegion",
          TestTerminalSessionDeleteLineRespectsScrollRegion);
  AddTest(tests, "TerminalSession/PasteBracketedMode",
          TestTerminalSessionPasteUsesBracketedPasteWhenEnabled);
  AddTest(tests, "TerminalSession/PasteSendsLineBreaksAsCarriageReturns",
          TestTerminalSessionPasteSendsLineBreaksAsCarriageReturns);
  AddTest(tests, "TerminalSession/PasteStripsEmbeddedEndMarker",
          TestTerminalSessionPasteStripsEmbeddedEndMarker);
  AddTest(tests, "TerminalSession/PasteNeutralizesReconstitutedEndMarker",
          TestTerminalSessionPasteNeutralizesReconstitutedEndMarker);
  AddTest(tests, "TerminalSession/PasteRawMode",
          TestTerminalSessionPasteFallsBackToRawBytesWhenDisabled);
  AddTest(tests, "TerminalSession/ApplicationCursorKeysSs3Mode",
          TestTerminalSessionApplicationCursorKeysModeUsesSs3Sequences);
  AddTest(tests, "TerminalSession/CtrlWithoutAControlMappingSendsTheKeysUtf8",
          TestTerminalSessionCtrlWithoutAControlMappingSendsTheKeysUtf8);
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
  AddTest(tests, "TerminalSession/SnapshotRangeReusesDestinationCellBuffers",
          TestTerminalSnapshotRangeReusesDestinationCellBuffers);
  AddTest(tests, "TerminalSession/LineRangeSnapshotSkipsUnchangedGeneration",
          TestTerminalSessionLineRangeSnapshotSkipsUnchangedGeneration);
  AddTest(tests, "TerminalSession/CursorSnapshotCapturesPositionAndVisibility",
          TestTerminalSessionCursorSnapshotCapturesPositionAndVisibility);
  AddTest(tests, "TerminalSession/ReportsCursorPositionQueries",
          TestTerminalSessionReportsCursorPositionQueries);
  AddTest(tests, "TerminalSession/PrivateCursorPositionReportIsScreenRelative",
          TestTerminalSessionPrivateCursorPositionReportIsScreenRelative);
  AddTest(tests, "TerminalSession/EraseSavedLinesAccountsScrollbackTrim",
          TestTerminalSessionEraseSavedLinesAccountsScrollbackTrim);
  AddTest(tests, "TerminalSession/HardNewlineOntoExistingRowKeepsWrapFlag",
          TestTerminalSessionHardNewlineOntoExistingRowKeepsWrapFlag);
  AddTest(tests, "TerminalSession/ReportsDeviceAttributesQueries",
          TestTerminalSessionReportsDeviceAttributesQueries);
  AddTest(tests, "TerminalSession/OriginModeMakesCupRelativeToScrollRegion",
          TestTerminalSessionOriginModeMakesCupRelativeToScrollRegion);
  AddTest(tests, "TerminalSession/DisableOriginModeRestoresAbsoluteCup",
          TestTerminalSessionDisableOriginModeRestoresAbsoluteCup);
  AddTest(tests, "TerminalSession/IgnoresCharsetDesignationEscapes",
          TestTerminalSessionIgnoresCharsetDesignationEscapes);
  AddTest(tests, "TerminalSession/ConsumesMultiByteCharsetDesignations",
          TestTerminalSessionConsumesMultiByteCharsetDesignations);
  AddTest(tests, "TerminalSession/CombiningMarkAttachesToAWideBaseGlyph",
          TestTerminalSessionCombiningMarkAttachesToAWideBaseGlyph);
  AddTest(tests, "TerminalSession/DecstbmHomesToTheMarginUnderOriginMode",
          TestTerminalSessionDecstbmHomesToTheMarginUnderOriginMode);
  AddTest(tests, "TerminalSession/OscTitleBellUpdatesLaunchLabel",
          TestTerminalSessionOscTitleBellUpdatesLaunchLabel);
  AddTest(tests, "TerminalSession/OscTitleStUpdatesLaunchLabel",
          TestTerminalSessionOscTitleStUpdatesLaunchLabel);
  AddTest(tests, "TerminalSession/EmptyOscTitleRestoresLaunchLabel",
          TestTerminalSessionEmptyOscTitleRestoresLaunchLabel);
  AddTest(tests, "TerminalSession/Osc52ClipboardBellQueuesClipboardText",
          TestTerminalSessionOsc52ClipboardBellQueuesClipboardText);
  AddTest(tests, "TerminalSession/BellIsConsumedOnceAndNotFromAnOscTerminator",
          TestTerminalSessionBellIsConsumedOnceAndNotFromAnOscTerminator);
  AddTest(tests, "TerminalSession/Osc52ClipboardStQueuesClipboardText",
          TestTerminalSessionOsc52ClipboardStQueuesClipboardText);
  AddTest(tests, "TerminalSession/Osc52RejectsInvalidClipboardPayloads",
          TestTerminalSessionOsc52RejectsInvalidClipboardPayloads);
  AddTest(tests, "TerminalSession/Osc52OversizedPayloadIsFlagged",
          TestTerminalSessionOsc52OversizedPayloadIsFlagged);
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
  AddTest(tests, "TerminalSession/QueuesDesktopNotifications",
          TestTerminalSessionQueuesDesktopNotifications);
  AddTest(tests, "TerminalSession/ReplaysClaudeCodeTrustDialog",
          TestTerminalSessionReplaysClaudeCodeTrustDialog);
  AddTest(tests, "TerminalSession/AnswersModernTuiQueries",
          TestTerminalSessionAnswersModernTuiQueries);
  AddTest(tests, "TerminalSession/EncodesModifiedAndFunctionKeys",
          TestTerminalSessionEncodesModifiedAndFunctionKeys);
  AddTest(tests, "TerminalSession/KittyHugePopIsBoundedByStackDepth",
          TestTerminalSessionKittyHugePopIsBoundedByStackDepth);
  AddTest(tests, "TerminalSession/AppliesKittyKeyboardProtocol",
          TestTerminalSessionAppliesKittyKeyboardProtocol);
  AddTest(tests, "TerminalSession/TracksSynchronizedOutputMode",
          TestTerminalSessionTracksSynchronizedOutputMode);
  AddTest(tests, "TerminalSession/AFrameClosedInAReadWakesEvenWhenTheNextOneOpened",
          TestTerminalSessionAFrameClosedInAReadWakesEvenWhenTheNextOneOpened);
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
  AddTest(tests, "TerminalSession/AlternateScreenPreservesPrimaryScrollback",
          TestTerminalSessionAlternateScreenPreservesPrimaryScrollback);
  AddTest(tests, "TerminalSession/AlternateScreenReentryPreservesAltContent",
          TestTerminalSessionAlternateScreenReentryPreservesAltContent);
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
  AddTest(tests, "TerminalSession/EraseCharsClampsToWidth",
          TestTerminalSessionEraseCharsClampsToWidth);
  AddTest(tests, "TerminalSession/InsertCharsClampsToWidth",
          TestTerminalSessionInsertCharsClampsToWidth);
  AddTest(tests, "TerminalSession/TabulationClampsToWidth",
          TestTerminalSessionTabulationClampsToWidth);
  AddTest(tests, "TerminalSession/TabDoesNotOverwriteCells",
          TestTerminalSessionTabDoesNotOverwriteCells);
  AddTest(tests, "TerminalSession/EraseSavedLinesKeepsVisibleScreen",
          TestTerminalSessionEraseSavedLinesKeepsVisibleScreen);
  AddTest(tests, "TerminalSession/InsertLinesClampsToHeight",
          TestTerminalSessionInsertLinesClampsToHeight);
  AddTest(tests, "TerminalSession/InsertLineOutsideRegionIsNoOpOnAltScreen",
          TestTerminalSessionInsertLineOutsideRegionIsNoOpOnAltScreen);
  AddTest(tests, "TerminalSession/ReportsPendingWrapColumnClamped",
          TestTerminalSessionReportsPendingWrapColumnClamped);
  AddTest(tests, "TerminalSession/ResizePreservesCustomTabStops",
          TestTerminalSessionResizePreservesCustomTabStops);
  AddTest(tests, "TerminalSession/CursorDownClampsPrimaryScreen",
          TestTerminalSessionCursorDownClampsPrimaryScreen);
  AddTest(tests, "TerminalSession/MouseRoutingRequiresTrackingMode",
          TestTerminalSessionMouseRoutingRequiresTrackingMode);
  AddTest(tests, "TerminalSession/ResizeClampsCursorAndPreservesBuffer",
          TestTerminalSessionResizeClampsCursorAndPreservesBuffer);
  AddTest(tests, "TerminalSession/OutputParserIgnoresIncompleteEscapes",
          TestTerminalSessionOutputParserIgnoresIncompleteEscapes);
  AddTest(tests, "TerminalSession/EraseDisplayAppliesBackgroundColorErase",
          TestTerminalSessionEraseDisplayAppliesBackgroundColorErase);
  AddTest(tests, "TerminalSession/ScrollRegionAppliesBackgroundColorErase",
          TestTerminalSessionScrollRegionAppliesBackgroundColorErase);
  AddTest(tests, "TerminalSession/EscInsideCsiAbortsSequence",
          TestTerminalSessionEscInsideCsiAbortsSequence);
  AddTest(tests, "TerminalSession/EscInsideOscRestartsSequence",
          TestTerminalSessionEscInsideOscRestartsSequence);
  AddTest(tests, "TerminalSession/IndexPreservesColumn",
          TestTerminalSessionIndexPreservesColumn);
  AddTest(tests, "TerminalSession/RestartResetsNegotiatedState",
          TestTerminalSessionRestartResetsNegotiatedState);
  AddTest(tests, "TerminalSession/SoftWrapOntoExistingRowMarksContinuation",
          TestTerminalSessionSoftWrapOntoExistingRowMarksContinuation);
  AddTest(tests, "TerminalSession/HardLineFeedDoesNotRelabelWrappedRow",
          TestTerminalSessionHardLineFeedDoesNotRelabelWrappedRow);
  AddTest(tests, "TerminalSession/InvalidUtf8StoredAsReplacementChar",
          TestTerminalSessionInvalidUtf8StoredAsReplacementChar);
  AddTest(tests, "TerminalSession/ShrinkKeepsCursorOnScreenAfterClear",
          TestTerminalSessionShrinkKeepsCursorOnScreenAfterClear);
  AddTest(tests, "TerminalSession/StopEscalatesToKillForStubbornChild",
          TestTerminalSessionStopEscalatesToKillForStubbornChild);
}

}
