#include "TestSupport.h"

#include "TerminalSessionTestAccess.h"

#include "terminal/TerminalBase64.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalMouseEncoder.h"
#include "util/PerformanceCounters.h"

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

void TestTerminalSessionAlternateScreenPreservesPrimaryScrollback() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  // Build a primary scrollback deeper than the viewport so the enter/exit path
  // exercises a non-trivial line buffer (this is the buffer the move-based swap
  // must hand back intact).
  TerminalSessionTestAccess::AppendOutput(session, "P0\nP1\nP2\nP3\nP4\nP5");
  const auto primary_before = session.SnapshotLines();
  Expect(primary_before.size() == 6,
         "primary screen should accumulate scrollback beyond the viewport");

  // Enter the alternate screen (mode 1049 clears it) and draw distinct content.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049h\x1b[HALT");
  ExpectLineText(session.SnapshotLines(), 0, "ALT",
                 "alternate screen should show its own content after entering");

  // Leaving the alternate screen must restore the primary scrollback verbatim.
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

  // Enter the alternate screen without clearing (mode 1047) and draw content.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1047h\x1b[HALTX");
  ExpectLineText(session.SnapshotLines(), 0, "ALTX",
                 "alternate screen should hold drawn content");

  // Leave and re-enter without clearing: prior alternate content must survive.
  // The move-based swap empties the backing store on restore, so re-entry must
  // not mistake that for an uninitialized screen and clear it.
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

  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[200~echo hi\n\x1b[201~",
         "terminal paste should wrap clipboard text when bracketed paste mode is enabled");
}

// A poisoned clipboard containing the bracketed-paste end marker must not be able
// to close paste mode early and inject the trailing bytes as typed input.
void TestTerminalSessionPasteStripsEmbeddedEndMarker() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");

  session.PasteText("safe\x1b[201~rm -rf ~\n");

  // The embedded end marker is dropped; the payload stays inside the paste guard.
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[200~saferm -rf ~\n\x1b[201~",
         "an embedded end marker must be stripped so paste mode can't be escaped");
}

// A single-pass deletion of the end marker is not closed under reconstitution:
// deleting the middle marker in `ESC[` + `ESC[201~` + `201~payload` splices the
// surrounding bytes into a fresh `ESC[201~`. The filter must reach a fixed point so
// deletion-created markers are also removed and cannot escape paste mode.
void TestTerminalSessionPasteNeutralizesReconstitutedEndMarker() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2004h");

  // Bytes: ESC [  <full end marker>  201~rm -rf ~\n
  session.PasteText("\x1b[\x1b[201~201~rm -rf ~\n");

  const std::string sent = TerminalSessionTestAccess::SentBytes(session);
  // The only end marker present must be the trailing guard we appended; the body
  // must contain no embedded ESC[201~ that could terminate paste mode early.
  Expect(sent == "\x1b[200~rm -rf ~\n\x1b[201~",
         "reconstituted end markers must be neutralized to a fixed point");
  const std::string_view body_and_tail(sent.data() + 6, sent.size() - 6);  // skip ESC[200~
  Expect(body_and_tail.find("\x1b[201~") == body_and_tail.size() - 6,
         "the pasted body must contain no embedded end marker before the final guard");
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

  // Regression: Meta+Ctrl must prefix the control byte with ESC so M-C-<key>
  // chords reach TUI apps; the ESC was previously dropped in legacy mode.
  KeyPress ctrl_alt_a;
  ctrl_alt_a.key = KeyPress::Key::Char;
  ctrl_alt_a.codepoint = 'a';
  ctrl_alt_a.ctrl = true;
  ctrl_alt_a.alt = true;
  Expect(sent_after(ctrl_alt_a) == std::string("\x1b\x01", 2),
         "Ctrl+Alt+A should send ESC + C0 control byte (Meta+Ctrl) in legacy mode");
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

  // SGR (1006) release must PRESERVE the button number in Pb and signal release
  // via the trailing 'm' — unlike the legacy encoding, it must not collapse to the
  // ambiguous button-3 code. Left release stays button 0; the 'm' distinguishes it.
  request.mouse_sgr_ext_mode = true;
  request.button = TerminalMouseButton::Left;
  request.pressed = false;
  request.motion = false;
  request.row = 4;
  request.column = 6;
  request.modifiers = SDL_KMOD_NONE;
  Expect(EncodeTerminalMouseEvent(request, bytes), "SGR mouse release should encode");
  Expect(bytes == "\x1b[<0;7;5m",
         "SGR mouse release must keep the real button (0) and use trailing m, not code 3");
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

// Resilience: ECH (`CSI Ps X`) with a large parameter must not grow the row far
// past the terminal width. Before the clamp, `CSI 65535 X` resized a row to
// ~65535 cells, and repeated across the scrollback that is a huge memory
// amplification driven purely by terminal output.
void TestTerminalSessionEraseCharsClampsToWidth() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // Home the cursor, then erase a pathologically large character count.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\x1b[65535X");
  const auto lines = session.SnapshotLines();
  Expect(!lines.empty(), "session should retain rows after ECH");
  Expect(lines[0].cells.size() <= 80,
         "CSI Ps X must clamp the erased range to the terminal width, not balloon the row");
}

// Resilience: ICH (`CSI Ps @`) inserts blank cells before the row is resized back
// to the terminal width, so a huge parameter must be clamped or it churns a
// 65535-cell insert/memmove per escape.
void TestTerminalSessionInsertCharsClampsToWidth() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\x1b[65535@");
  const auto lines = session.SnapshotLines();
  Expect(!lines.empty(), "session should retain rows after ICH");
  Expect(lines[0].cells.size() <= 80,
         "CSI Ps @ must clamp the inserted cell count to the terminal width");
}

// Resilience: CHT/CBT (`CSI Ps I` / `CSI Ps Z`) used to loop once per parameter
// even after the cursor had saturated at the terminal edge. Clamp the repeated
// tab-stop walk to the screen width.
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

// HT (`\t`) is pure cursor motion: it must move to the next tab stop WITHOUT
// blanking the cells it passes over. Regression for the fix that replaced the
// per-column `PutCharacterLocked(' ')` fill with a cursor move.
void TestTerminalSessionTabDoesNotOverwriteCells() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);
  // Default tab stops at 8,16,... . Fill cols 0..9, CR to col 0, tab to col 8,
  // then write 'X' at the tab stop. `ABCDEFGHIJ\r\tX` must yield `ABCDEFGHXJ`.
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

// ED 3 (`CSI 3J`, "Erase Saved Lines") must clear the scrollback and LEAVE the
// visible screen intact. Regression for the fix that stopped mode 3 from falling
// through to the ED-0 path, which erased from the cursor to the end of display.
void TestTerminalSessionEraseSavedLinesKeepsVisibleScreen() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);  // 3-row visible screen
  // Six CRLF-separated lines into a 3-row terminal builds three lines of
  // scrollback (L0..L2) above the visible screen (L3..L5).
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5");
  const auto before = session.SnapshotLines();
  Expect(before.size() > 3,
         "six lines into a 3-row terminal should accumulate scrollback");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3J");
  const auto after = session.SnapshotLines();
  Expect(after.size() == 3,
         "CSI 3J must trim the deque to the visible screen height (scrollback gone)");
  // The visible screen (L3..L5) must survive unharmed: 3J erases only scrollback.
  Expect(after.front().cells.size() >= 2 && after.front().cells[0].ascii_character() == 'L' &&
             after.front().cells[1].ascii_character() == '3',
         "3J must preserve the first visible line ('L3')");
  Expect(after.back().cells.size() >= 2 && after.back().cells[0].ascii_character() == 'L' &&
             after.back().cells[1].ascii_character() == '5',
         "3J must preserve the last visible line ('L5')");
}

// ED 3 front-trims the scrollback deque, so — exactly like the natural
// TrimScrollbackLocked path — it must add the trimmed count to ScrollbackTrimTotal().
// The workspace rebases its absolute scroll/selection mirrors purely from the delta of
// that counter; an untracked trim strands those rows `trim_count` too high after a
// modern `clear` (which emits ED2 then ED3). Regression for that desync.
void TestTerminalSessionEraseSavedLinesAccountsScrollbackTrim() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);  // 3-row visible screen
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5");
  Expect(session.ScrollbackTrimTotal() == 0,
         "no natural scrollback trim should have occurred within the default cap yet");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3J");
  // 6 deque rows minus the 3 visible rows == 3 trimmed off the front.
  Expect(session.ScrollbackTrimTotal() == 3,
         "ED3 must account its front-trim in ScrollbackTrimTotal so workspace mirrors rebase");
}

// The private DECXCPR reply (`CSI ? 6 n`) must report the row relative to the visible
// screen, mirroring the public CPR (`CSI 6 n`) path: on the primary buffer cursor_row_
// is an absolute deque index that includes scrollback, so a raw cursor_row_+1 grossly
// overstates the row once scrollback exists. Regression for the missed `?6n` variant.
void TestTerminalSessionPrivateCursorPositionReportIsScreenRelative() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);  // 3-row visible screen
  // Six lines into a 3-row terminal builds three scrollback rows; the cursor lands on
  // the last visible row (absolute row 5, screen-relative row 2 => reported row 3) at
  // column 2 after "L5" (reported column 3).
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5\x1b[?6n");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[?3;3R",
         "DECXCPR (?6n) must report the screen-relative row, not the absolute deque row");
}

// A hard newline (LF) that lands on a pre-existing row must not relabel that row's
// soft-wrap flag. Regression for AdvanceCursorRowLocked unconditionally stamping
// wrapped_from_previous=false onto an existing wrapped continuation reached via
// cursor-up + LF, which corrupted reflow / selection-by-logical-line.
void TestTerminalSessionHardNewlineOntoExistingRowKeepsWrapFlag() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 4);  // 4-column screen forces a soft wrap

  // "ABCDE" wraps: row0="ABCD", row1="E" (wrapped_from_previous=true), cursor at row1.
  // CUU moves the cursor up to row0; LF then advances it back down onto the existing
  // row1 — which must retain its wrapped continuation flag.
  TerminalSessionTestAccess::AppendOutput(session, "ABCDE\x1b[A\n");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() >= 2, "soft-wrap fixture should preserve the wrapped second row");
  Expect(lines[1].wrapped_from_previous,
         "a hard LF onto an existing wrapped row must not clear its soft-wrap flag");
}

// IL/DL outside the vertical scroll margins must be a no-op on the alternate
// screen (DEC STD 070). Regression for the fall-through that ran the primary-screen
// insert, growing the fixed-height alt grid past rows_ and eventually trimming real
// content off the top.
void TestTerminalSessionInsertLineOutsideRegionIsNoOpOnAltScreen() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);  // 4-row alt grid: A,B,C,D

  // Scroll region rows 2..3 (1-based) => 0-based [1,2]; put the cursor on row 0
  // (above the region), then issue IL. It must do nothing.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;3r\x1b[1;1H\x1b[L");
  auto lines = session.SnapshotLines();
  Expect(lines.size() == 4, "IL above the scroll margins must not grow the alt grid");
  ExpectLineText(lines, 0, "A", "IL outside the region must leave row 0 untouched");
  ExpectLineText(lines, 3, "D", "IL outside the region must leave the bottom row untouched");

  // Same for DL below the region: cursor on row 3 (below [1,2]) then DL.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[4;1H\x1b[M");
  lines = session.SnapshotLines();
  Expect(lines.size() == 4, "DL below the scroll margins must not shrink/resize the alt grid");
  ExpectLineText(lines, 0, "A", "DL outside the region must leave row 0 untouched");
  ExpectLineText(lines, 3, "D", "DL outside the region must leave the bottom row untouched");
}

// DSR/CPR (`CSI 6n`) at the pending-wrap (LCF) column, where cursor_column_ ==
// columns_, must report the last on-screen column, never one past the right edge.
void TestTerminalSessionReportsPendingWrapColumnClamped() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // Move to the last column (80, 1-based) and print a glyph; without autowrap
  // advancing the row this leaves the cursor in the pending-wrap state at col 80.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;80HX\x1b[6n");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[1;80R",
         "CPR at the pending-wrap column must report column 80, not 81");
}

// HTS-set custom tab stops must survive a resize (xterm behaviour); only the
// newly-exposed columns get the default every-8 stops. Regression for the resize
// path unconditionally rebuilding the tab-stop table.
void TestTerminalSessionResizePreservesCustomTabStops() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // Set a custom tab stop at column 3 (1-based col 4) via HTS (ESC H), which is not
  // a default every-8 stop.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;4H\x1bH");
  session.Resize(24, 100);
  // From column 0 the next tab must still land on the custom stop at column 3.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\t");
  Expect(session.cursor_column() == 3,
         "a resize must preserve custom HTS tab stops, not reset to every-8");
}

// Resilience: IL (`CSI Ps L`) inserts blank lines; a huge parameter must not
// transiently balloon the line deque to tens of thousands of entries before the
// scrollback trim.
void TestTerminalSessionInsertLinesClampsToHeight() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1H\x1b[65535L");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() <= 100,
         "CSI Ps L must clamp the inserted line count to the screen height, not 65535");
}

// A hostile program can emit additive cursor-down escapes (`CSI B` / `CSI E`) in
// the primary screen; without a clamp each `\x1b[65535B` grows `cursor_row_` and
// resizes the line deque to tens of millions of entries before the end-of-chunk
// scrollback trim — an OOM/crash on the reader thread. The post-append line count
// can't observe the transient balloon (trim collapses it), so assert on the
// scrollback-lines-allocated perf counter instead.
void TestTerminalSessionCursorDownClampsPrimaryScreen() {
  microide::util::ResetPerformanceCounters();
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[65535B");
  const std::uint64_t allocated = microide::util::ReadPerformanceCounter(
      microide::util::PerfCounterId::TerminalScrollbackLinesAllocated);
  // With the clamp, growth is bounded to ~max_scrollback_lines_ + rows_ (~2024);
  // without it, one escape would allocate 65536 lines. Use a generous ceiling.
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

// Regression: Background Color Erase. A whole-screen erase with a non-default
// background must paint the blanked rows with that background, matching the
// erase-in-line path. Previously EraseInDisplay reset rows to empty cells, so
// `\x1b[44m\x1b[2J` (blue background, clear screen) lost the background.
void TestTerminalSessionEraseDisplayAppliesBackgroundColorErase() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  // SGR 44 = blue background, then ED 2 (clear entire display).
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

// Regression: a scroll-region scroll (SU/SD/IL/DL) under a non-default background
// must paint the freed rows with that background, matching xterm/VTE and the
// ED/EL erase paths. Previously ScrollRegionUp/DownLocked reset freed rows to
// empty default cells, so `\x1b[44m\x1b[S` lost the background on the freed row.
void TestTerminalSessionScrollRegionAppliesBackgroundColorErase() {
  microide::terminal::TerminalSession session;
  ResetAlternateScreenFixture(session);

  // SGR 44 = blue background, then SU (scroll whole screen up one row).
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

// Regression: IND (ESC D) moves down one row PRESERVING the column; only NEL
// (ESC E) resets to column 0. IND is not subject to ONLCR, so a program using it
// to move down while holding its column must land at that column.
void TestTerminalSessionIndexPreservesColumn() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  TerminalSessionTestAccess::AppendOutput(session, "abc");  // row 0, column 3
  Expect(session.cursor_row() == 0 && session.cursor_column() == 3,
         "cursor should sit at column 3 after printing 'abc'");

  // NB: octal \033 (not \x1b) — a \x hex escape is greedy and would fold the
  // following 'D'/'E' hex digit into a single out-of-range escape.
  TerminalSessionTestAccess::AppendOutput(session, "\033D");  // IND
  Expect(session.cursor_row() == 1 && session.cursor_column() == 3,
         "IND (ESC D) must move down one row and preserve the column");

  TerminalSessionTestAccess::AppendOutput(session, "\033E");  // NEL
  Expect(session.cursor_row() == 2 && session.cursor_column() == 0,
         "NEL (ESC E) must move down and reset the column to 0");
}

// Regression: an ESC (not forming ST) inside an unterminated OSC string must
// terminate the string AND restart escape parsing, so a following real sequence
// runs instead of being swallowed into the OSC payload.
void TestTerminalSessionEscInsideOscRestartsSequence() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "HELLO");
  // OSC set-title WITHOUT a terminator, immediately followed by a clear-screen.
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

// Regression: restarting a session in place must reset all negotiated protocol
// state, so a reused session never inherits a prior shell's Kitty-keyboard flags,
// synchronized-output mode, cursor shape, reported cwd, or custom tab stops.
void TestTerminalSessionRestartResetsNegotiatedState() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 24, 80);

  // Negotiate protocol state a real shell might set.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[>5u");     // push Kitty flags = 5
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?2026h");  // synchronized output on
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3g");      // clear then set a tab stop
  TerminalSessionTestAccess::AppendOutput(session, "\x1bH");
  Expect(TerminalSessionTestAccess::KittyKeyboardFlags(session) == 5,
         "kitty keyboard flags should be negotiated before restart");
  Expect(TerminalSessionTestAccess::SynchronizedOutput(session),
         "synchronized output should be enabled before restart");

  // Restart in place (test seam routes through Stop()'s reset).
  session.StartPlaceholderForTesting(std::filesystem::path("/tmp"), "");

  Expect(TerminalSessionTestAccess::KittyKeyboardFlags(session) == 0,
         "restart must clear negotiated kitty keyboard flags");
  Expect(!TerminalSessionTestAccess::SynchronizedOutput(session),
         "restart must clear synchronized output mode");
  Expect(TerminalSessionTestAccess::TabStopCount(session) == 0,
         "restart must clear custom tab stops");
}

// Regression: an ESC appearing mid-CSI must cancel the current sequence and start
// a fresh one. `\x1b[\x1b[2J` must run the second CSI (clear screen), not swallow
// the ESC as a parameter and dispatch a corrupt sequence that prints "2J".
void TestTerminalSessionEscInsideCsiAbortsSequence() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);

  // Put some text on screen, then send an aborted CSI immediately followed by a
  // real clear-screen. The clear must take effect and no literal "2J" may appear.
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

// Overwriting either half of a double-width glyph must clear the orphaned half so
// the renderer never sees a lead without its spacer (overlap) or a spacer without
// its lead (stale gap).
void TestTerminalSessionOverwritingWideGlyphClearsStalePair() {
  {
    // Overwrite the LEAD: the trailing spacer must be blanked, not left dangling.
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 4, 8);
    TerminalSessionTestAccess::AppendOutput(session, "\xE6\x88\x91");  // wide lead + spacer
    TerminalSessionTestAccess::AppendOutput(session, "\rA");           // CR to col 0, write 'A'
    const auto lines = session.SnapshotLines();
    Expect(lines[0].cells[0].ascii_character() == 'A' && !lines[0].cells[0].style.wide_trailing(),
           "the overwriting narrow glyph should replace the wide lead");
    Expect(!lines[0].cells[1].style.wide_trailing() && lines[0].cells[1].ascii_character() == ' ',
           "the orphaned trailing spacer must be blanked, not left as a wide-trailing cell");
  }
  {
    // Overwrite the SPACER: the lead must be blanked so it no longer paints wide.
    microide::terminal::TerminalSession session;
    TerminalSessionTestAccess::Reset(session, 4, 8);
    TerminalSessionTestAccess::AppendOutput(session, "\xE6\x88\x91");   // wide lead at col 0
    TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;2HB");     // move to col 1, write 'B'
    const auto lines = session.SnapshotLines();
    Expect(lines[0].cells[1].ascii_character() == 'B',
           "the overwriting glyph should land on the former spacer column");
    Expect(!lines[0].cells[0].style.wide_trailing() && lines[0].cells[0].ascii_character() == ' ',
           "the orphaned wide lead must be blanked so it no longer paints across two columns");
  }
}

// DEL (0x7f) in the output stream is ignored (ECMA-48 / xterm / VTE); it must not
// perform a destructive delete-and-shift of the display.
void TestTerminalSessionIgnoresDelInOutputStream() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "abc\x7f");
  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "abc", "a stray DEL must leave the display unchanged");
  Expect(session.cursor_column() == 3, "DEL must not move the cursor");
}

// A tab at the pending-wrap column (cursor_column_ == columns_) must not move the
// cursor backward to the last column.
void TestTerminalSessionTabAtPendingWrapDoesNotMoveBackward() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "abcdefgh");  // fills all 8 cols -> pending wrap
  Expect(session.cursor_column() == 8, "precondition: cursor is at the pending-wrap column");
  TerminalSessionTestAccess::AppendOutput(session, "\t");
  Expect(session.cursor_column() == 8, "a tab at pending-wrap must not snap the cursor backward");
}

// EL 2 (CSI 2K) at the pending-wrap column must not grow the line one cell past
// the right margin.
void TestTerminalSessionEraseWholeLineDoesNotOvergrowAtPendingWrap() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "abcdefgh");  // pending wrap at column 8
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2K");
  const auto lines = session.SnapshotLines();
  Expect(lines[0].cells.size() == 8,
         "erase-whole-line must span exactly the terminal width, not columns_ + 1");
}

// ED 2 (`CSI 2J`) on the primary screen must erase the visible screen in place and
// PRESERVE scrollback — exactly what `clear`/`tput clear` (`ESC[H ESC[2J`) sends.
// Regression for the collapse that replaced the whole deque, silently destroying all
// history above the viewport.
void TestTerminalSessionClearPreservesScrollback() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);  // 3-row visible screen
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5");
  Expect(session.SnapshotLines().size() > 3, "six lines into a 3-row screen builds scrollback");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[H\x1b[2J");  // `clear`
  const auto after = session.SnapshotLines();
  Expect(after.size() >= 6, "2J must keep the scrollback deque, not collapse it to the screen");
  ExpectLineText(after, 0, "L0", "2J must preserve the first scrollback line");
  ExpectLineText(after, 1, "L1", "2J must preserve scrollback");
  ExpectLineText(after, 2, "L2", "2J must preserve the last scrollback line");
  Expect(LineText(after[after.size() - 1]).empty(), "2J must blank the visible screen");
  Expect(LineText(after[after.size() - 3]).empty(), "2J must blank the whole visible screen");
}

// CSI H (CUP home) on the primary screen must target the top-left of the VISIBLE
// screen, not absolute deque row 0 (which is scrollback). Regression for the
// viewport-relative primary-addressing rework that ED 2 depends on.
void TestTerminalSessionPrimaryCupIsViewportRelative() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 20);
  TerminalSessionTestAccess::AppendOutput(session, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5");

  TerminalSessionTestAccess::AppendOutput(session, "\x1b[HX");  // home, then write X
  const auto after = session.SnapshotLines();
  ExpectLineText(after, 0, "L0", "CUP home must not reach into scrollback");
  Expect(after.size() >= 3, "the deque must stay intact");
  ExpectLineText(after, after.size() - 3, "X3",
                 "CUP home targets the visible-screen top (overwrites L3's first cell -> X3)");
}

// EL 1 (`CSI 1K`, erase to start of line) at the pending-wrap column
// (cursor_column_ == columns_) must clamp its erase end to the width, mirroring the
// EL 2 fix, or it grows the row one cell past the right margin.
void TestTerminalSessionEraseToLineStartDoesNotOvergrowAtPendingWrap() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "abcdefgh");  // pending wrap at column 8
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1K");
  const auto lines = session.SnapshotLines();
  Expect(lines[0].cells.size() == 8,
         "erase-to-line-start must not grow the row to columns_ + 1 at the pending-wrap column");
}

// A scroll region that spanned the full screen must re-expand to the new height when
// the terminal grows, or LF-driven scrolling stays frozen at the old height.
void TestTerminalSessionScrollRegionReexpandsOnGrow() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049h");  // alt screen, full-screen region
  session.Resize(8, 8);
  // Fill all 8 rows; if the region were still capped at row 3, rows 5..8 would never
  // receive scrolled output.
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

}  // namespace

// Regression: CUU (`CSI A`) / CPL (`CSI F`) must clamp at the TOP OF THE VISIBLE
// SCREEN, not the top of the scrollback deque. On the primary buffer cursor_row_
// is absolute, so a bare `ESC[<N>A` ("go to top of screen") used to climb above
// the visible rows into history and overwrite it.
void TestTerminalSessionCursorUpClampsToVisibleScreenTop() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 8);
  // Six lines with a 3-row viewport => visible screen is L3/L4/L5, scrollback L0-L2.
  TerminalSessionTestAccess::AppendOutput(session, "L0\nL1\nL2\nL3\nL4\nL5");
  Expect(session.cursor_row() == 5, "cursor should sit on the last written primary row");

  // CR to column 0, then cursor up 4 and write X. The floor is the visible-screen
  // top (row 3), so X lands on L3 — never on the L1 scrollback line.
  TerminalSessionTestAccess::AppendOutput(session, "\r\x1b[4AX");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 6, "primary scrollback must be preserved across the cursor move");
  ExpectLineText(lines, 1, "L1", "CUU must not climb into scrollback and overwrite history");
  ExpectLineText(lines, 3, "X3", "CUU clamps at the visible-screen top");
}

// Regression: VT (0x0B) and FF (0x0C) perform an index (line feed), like xterm/VTE.
// They previously fell through the control switch and were dropped entirely.
void TestTerminalSessionVerticalTabAndFormFeedIndexLikeLineFeed() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  TerminalSessionTestAccess::AppendOutput(session, "a\x0b""b\x0c""c");

  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 3, "VT and FF should each open a new row like a line feed");
  ExpectLineText(lines, 0, "a", "content before VT stays on the first row");
  ExpectLineText(lines, 1, "b", "VT moves the cursor down one row like a line feed");
  ExpectLineText(lines, 2, "c", "FF moves the cursor down another row like a line feed");
}

// Regression: DECSC (ESC 7) saves the SGR graphic rendition and DECRC (ESC 8)
// restores it, not just the cursor position. Previously the style leaked past the
// restore.
void TestTerminalSessionSaveRestoreCursorRestoresGraphicRendition() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  // DECSC, set red foreground, print RED, DECRC, print X at the restored origin.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b""7\x1b[31mRED\x1b""8X");

  const auto lines = session.SnapshotLines();
  Expect(!lines.empty() && !lines[0].cells.empty(), "restored write should land on the first row");
  Expect(lines[0].cells[0].ascii_character() == 'X',
         "DECRC should restore the cursor to the pre-save position");
  Expect(!lines[0].cells[0].style.foreground.has_value(),
         "DECRC should restore the default graphic rendition saved by DECSC");
}

// Regression: toggling DECOM (origin mode, `CSI ?6h`/`CSI ?6l`) on the primary buffer
// homes the cursor to the VISIBLE-screen top, not absolute row 0 in scrollback. The
// old code passed a screen-relative 0 as an absolute deque index, so a bare
// `CSI ?6l` (which programs commonly emit) jumped into history and overwrote it.
void TestTerminalSessionOriginModeToggleHomesToVisibleScreenTop() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 3, 8);
  TerminalSessionTestAccess::AppendOutput(session, "L0\nL1\nL2\nL3\nL4\nL5");

  // Reset origin mode, then write X. X must land on the visible-screen top (L3),
  // never on the L0 scrollback line.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?6lX");
  const auto lines = session.SnapshotLines();
  Expect(lines.size() == 6, "primary scrollback must be preserved across the origin-mode toggle");
  ExpectLineText(lines, 0, "L0", "origin-mode home must not climb into scrollback");
  ExpectLineText(lines, 3, "X3", "origin-mode home lands at the visible-screen top");
}

// Regression: TrimScrollbackLocked exposes a monotonic count of front lines dropped so
// the workspace-side absolute-row mirrors (scroll position, selection, last-command row)
// can rebase and track the same content instead of jumping forward by the trim batch.
void TestTerminalSessionScrollbackTrimTotalTracksDroppedLines() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 4, 8);
  session.SetMaxScrollbackLines(200);  // clamped floor is 200
  Expect(session.ScrollbackTrimTotal() == 0, "no trim before the cap is exceeded");

  // Emit well past the coalesced trim high-watermark (cap + cap/4 == 251).
  std::string output;
  const int kLines = 300;
  for (int i = 0; i < kLines; ++i) {
    output += "line\n";
  }
  TerminalSessionTestAccess::AppendOutput(session, output);

  const std::uint64_t trimmed = session.ScrollbackTrimTotal();
  Expect(trimmed > 0, "exceeding the scrollback cap should trim front lines");
  // After trimming, the deque is capped and the trim count accounts for the difference
  // between everything written and what survives.
  Expect(session.LineCount() <= 251, "trimmed deque should collapse toward the cap");
  Expect(trimmed == static_cast<std::uint64_t>(kLines + 1) - session.LineCount(),
         "trim total should equal written lines minus surviving lines");
}

// DECSTBM (CSI r) requires the top margin to be strictly above the bottom.
// A one-line region (top == bottom) is invalid and must leave the region
// unchanged. We reveal the effective scroll-region top with origin mode: with
// origin mode on, CUP row 1 homes to the region top, so writing there shows
// where the top margin actually is.
void TestTerminalSessionDecstbmIgnoresEqualMargins() {
  microide::terminal::TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 6, 8);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049hA\nB\nC\nD\nE\nF");

  // CSI 5;5 r would set a one-line region at index 4; it must be ignored so the
  // region stays full-screen (top index 0).
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

  // CSI 8;3 r inverts the margins (top index 7 below bottom index 2); it must be
  // ignored, leaving the full-screen region with its top at row 0.
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

  // Establish a genuine narrower region, then the no-argument CSI r must reset it
  // to a valid full-screen region (top index 0 < bottom index 5).
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[3;5r\x1b[r\x1b[?6h\x1b[1;1HZ");

  const auto lines = session.SnapshotLines();
  ExpectLineText(lines, 0, "Z",
                 "full-screen CSI r should re-establish a valid region with the top margin at row 0");
  ExpectLineText(lines, 2, "C",
                 "full-screen CSI r must not leave the previous narrower region's top in effect");
}

void RegisterTerminalSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalSession/DecstbmIgnoresEqualMargins",
          TestTerminalSessionDecstbmIgnoresEqualMargins);
  AddTest(tests, "TerminalSession/DecstbmIgnoresInvertedMargins",
          TestTerminalSessionDecstbmIgnoresInvertedMargins);
  AddTest(tests, "TerminalSession/DecstbmFullScreenResetsRegion",
          TestTerminalSessionDecstbmFullScreenResetsRegion);
  AddTest(tests, "TerminalSession/ScrollbackTrimTotalTracksDroppedLines",
          TestTerminalSessionScrollbackTrimTotalTracksDroppedLines);
  AddTest(tests, "TerminalSession/OriginModeToggleHomesToVisibleScreenTop",
          TestTerminalSessionOriginModeToggleHomesToVisibleScreenTop);
  AddTest(tests, "TerminalSession/CursorUpClampsToVisibleScreenTop",
          TestTerminalSessionCursorUpClampsToVisibleScreenTop);
  AddTest(tests, "TerminalSession/VerticalTabAndFormFeedIndexLikeLineFeed",
          TestTerminalSessionVerticalTabAndFormFeedIndexLikeLineFeed);
  AddTest(tests, "TerminalSession/SaveRestoreCursorRestoresGraphicRendition",
          TestTerminalSessionSaveRestoreCursorRestoresGraphicRendition);
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
  AddTest(tests, "TerminalSession/EraseWholeLineDoesNotOvergrowAtPendingWrap",
          TestTerminalSessionEraseWholeLineDoesNotOvergrowAtPendingWrap);
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
  AddTest(tests, "TerminalSession/PasteStripsEmbeddedEndMarker",
          TestTerminalSessionPasteStripsEmbeddedEndMarker);
  AddTest(tests, "TerminalSession/PasteNeutralizesReconstitutedEndMarker",
          TestTerminalSessionPasteNeutralizesReconstitutedEndMarker);
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
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "TerminalSession/StopEscalatesToKillForStubbornChild",
          TestTerminalSessionStopEscalatesToKillForStubbornChild);
#endif
}

}  // namespace microide::tests
