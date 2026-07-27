#include "TestSupport.h"

#include "TerminalSessionTestAccess.h"

#include "terminal/TerminalSearch.h"
#include "terminal/TerminalSession.h"

#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::terminal::FindTerminalLineMatches;
using microide::terminal::MakeTerminalSearchQuery;
using microide::terminal::TerminalCell;
using microide::terminal::TerminalLine;
using microide::terminal::TerminalSearchMatch;
using microide::terminal::TerminalSearchScratch;
using microide::terminal::TerminalSession;

TerminalLine MakeLine(std::string_view ascii) {
  TerminalLine line;
  line.cells.reserve(ascii.size());
  for (const char character : ascii) {
    TerminalCell cell;
    if (character != '\0') {
      cell.SetAscii(character);
    }
    line.cells.push_back(cell);
  }
  return line;
}

std::vector<TerminalSearchMatch> MatchesIn(const TerminalLine& line,
                                           std::string_view query,
                                           bool case_sensitive = false,
                                           bool whole_word = false) {
  TerminalSearchScratch scratch;
  std::vector<TerminalSearchMatch> matches;
  FindTerminalLineMatches(line, MakeTerminalSearchQuery(query, case_sensitive, whole_word),
                          /*row=*/7, /*max_matches=*/64, scratch, matches);
  return matches;
}

void TestFindsLiteralMatchesWithGridColumns() {
  const TerminalLine line = MakeLine("error: no such file (error 2)");
  const std::vector<TerminalSearchMatch> matches = MatchesIn(line, "error");
  Expect(matches.size() == std::size_t{2}, "both occurrences should be reported");
  Expect(matches[0].row == std::size_t{7}, "the caller's row should be echoed back");
  Expect(matches[0].column == std::size_t{0}, "the first match starts at column 0");
  Expect(matches[0].length == std::size_t{5}, "the match spans its own cells");
  Expect(matches[1].column == std::size_t{21}, "the second match's column should be exact");

  Expect(MatchesIn(line, "ERROR").size() == 2, "search should fold case by default");
  Expect(MatchesIn(line, "ERROR", /*case_sensitive=*/true).empty(),
         "the case-sensitive toggle should reject a differently-cased query");
}

// Overlapping occurrences are distinct matches: "aa" appears twice in "aaa", and
// navigation must be able to land on both.
void TestReportsOverlappingMatches() {
  const std::vector<TerminalSearchMatch> matches = MatchesIn(MakeLine("aaa"), "aa");
  Expect(matches.size() == std::size_t{2}, "overlapping occurrences should both be reported");
  Expect(matches[0].column == std::size_t{0}, "the first overlap starts at 0");
  Expect(matches[1].column == std::size_t{1}, "the second overlap starts at 1");
}

void TestWholeWordRejectsSubstringHits() {
  const TerminalLine line = MakeLine("cat concatenate cat_x cat.");
  Expect(MatchesIn(line, "cat").size() == 4, "without the toggle every substring counts");
  const std::vector<TerminalSearchMatch> whole =
      MatchesIn(line, "cat", /*case_sensitive=*/false, /*whole_word=*/true);
  Expect(whole.size() == std::size_t{2}, "only the standalone words should survive");
  Expect(whole[0].column == std::size_t{0}, "the leading word should match");
  Expect(whole[1].column == std::size_t{22}, "a trailing '.' should still bound a word");
}

// Untouched cells paint as blanks, so they must read as spaces or a query
// containing a space would silently miss every gap on the row.
void TestEmptyCellsReadAsSpaces() {
  TerminalLine line = MakeLine("ab");
  line.cells.push_back(TerminalCell{});  // never written
  line.cells.push_back(TerminalCell{});
  TerminalCell trailing;
  trailing.SetAscii('c');
  line.cells.push_back(trailing);
  const std::vector<TerminalSearchMatch> matches = MatchesIn(line, "b  c");
  Expect(matches.size() == std::size_t{1}, "a query spanning blank cells should match");
  Expect(matches[0].column == std::size_t{1}, "the match should start at the 'b'");
  Expect(matches[0].length == std::size_t{4}, "the match should cover the blank cells");
}

// A double-width glyph occupies two columns, the second carrying no text. The
// reported span must cover both so the highlight has no hole in it, and the
// columns after it must not be shifted by the spacer.
void TestWideGlyphSpansBothColumns() {
  TerminalLine line;
  TerminalCell wide;
  wide.SetUtf8("\xE6\xBC\xA2");  // U+6F22, double width
  line.cells.push_back(wide);
  TerminalCell spacer;
  spacer.style.set(microide::terminal::cell_attr::kWideTrailing, true);
  line.cells.push_back(spacer);
  TerminalCell tail;
  tail.SetAscii('x');
  line.cells.push_back(tail);

  const std::vector<TerminalSearchMatch> wide_match = MatchesIn(line, "\xE6\xBC\xA2");
  Expect(wide_match.size() == std::size_t{1}, "a wide glyph should be findable");
  Expect(wide_match[0].column == std::size_t{0}, "the match starts at the lead cell");
  Expect(wide_match[0].length == std::size_t{2}, "the span should include the spacer cell");

  const std::vector<TerminalSearchMatch> tail_match = MatchesIn(line, "x");
  Expect(tail_match.size() == std::size_t{1}, "the cell after a wide glyph should be findable");
  Expect(tail_match[0].column == std::size_t{2}, "the spacer must not shift later columns in the reported match");
}

void TestFoldsNonAsciiCase() {
  TerminalLine line;
  TerminalCell cell;
  cell.SetUtf8("\xC3\x89");  // U+00C9 LATIN CAPITAL LETTER E WITH ACUTE
  line.cells.push_back(cell);
  Expect(MatchesIn(line, "\xC3\xA9").size() == 1, "case folding should reach non-ASCII letters");
  Expect(MatchesIn(line, "\xC3\xA9", /*case_sensitive=*/true).empty(),
         "the case-sensitive toggle should keep non-ASCII case distinct");
}

void TestScansScrollbackAndReportsStableBoundary() {
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, /*rows=*/4, /*columns=*/40);
  for (int index = 0; index < 20; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle line\r\n");
  }

  TerminalSearchScratch scratch;
  std::vector<TerminalSearchMatch> matches;
  const TerminalSession::SearchScan scan =
      session.FindMatches(MakeTerminalSearchQuery("needle", false, false), /*start_row=*/0,
                          session.ScrollbackTrimTotal(), /*max_matches=*/1000, scratch, matches);
  Expect(matches.size() == std::size_t{20}, "every scrollback row should be scanned");
  Expect(!scan.truncated, "a scan below the cap should not report truncation");
  Expect(scan.stable_row_end > 0 && scan.stable_row_end < scan.line_count,
         "the settled-scrollback boundary should sit above row 0 and below the tail");
  Expect(matches.back().row >= scan.stable_row_end,
         "the newest match should live in the still-mutable visible grid");
}

// A rescan after new output only restarts from the visible grid; the caller keeps
// the settled scrollback matches it already has. Feeding a fresh line must extend
// the set without duplicating or dropping anything.
void TestIncrementalRescanExtendsSettledMatches() {
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, /*rows=*/4, /*columns=*/40);
  for (int index = 0; index < 20; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle line\r\n");
  }

  TerminalSearchScratch scratch;
  std::vector<TerminalSearchMatch> matches;
  const auto query = MakeTerminalSearchQuery("needle", false, false);
  TerminalSession::SearchScan scan = session.FindMatches(
      query, 0, session.ScrollbackTrimTotal(), 1000, scratch, matches);
  const std::size_t stable_row_end = scan.stable_row_end;

  TerminalSessionTestAccess::AppendOutput(session, "needle again\r\n");
  std::erase_if(matches, [&](const TerminalSearchMatch& match) {
    return match.row >= stable_row_end;
  });
  const std::size_t settled = matches.size();
  scan = session.FindMatches(query, stable_row_end, session.ScrollbackTrimTotal(), 1000, scratch,
                             matches);
  Expect(!scan.full_rescan, "an up-to-date trim counter should not force a full rescan");
  Expect(matches.size() == std::size_t{21}, "the new row should extend the settled matches");
  Expect(settled < matches.size(), "the incremental pass should have appended rows");
  for (std::size_t index = 1; index < matches.size(); ++index) {
    Expect(matches[index - 1].row < matches[index].row, "matches should stay row-ordered");
  }
}

// Trimming happens on the reader thread, so a caller's rows can go stale between
// its rebase and the scan. Passing the counter it rebased against must make the
// scan restart cleanly rather than mix two coordinate spaces.
void TestStaleTrimCounterForcesFullRescan() {
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, /*rows=*/4, /*columns=*/40);
  session.SetMaxScrollbackLines(200);  // the session's floor
  for (int index = 0; index < 400; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle line\r\n");
  }
  Expect(session.ScrollbackTrimTotal() > 0, "the scrollback cap should have trimmed rows");

  TerminalSearchScratch scratch;
  std::vector<TerminalSearchMatch> matches{TerminalSearchMatch{.row = 999999}};
  const TerminalSession::SearchScan scan = session.FindMatches(
      MakeTerminalSearchQuery("needle", false, false), /*start_row=*/500,
      /*expected_trim_total=*/0, /*max_matches=*/1000, scratch, matches);
  Expect(scan.full_rescan, "a stale trim counter should be detected");
  Expect(!matches.empty(), "the restarted scan should find the retained rows");
  Expect(matches.front().row != 999999, "the caller's stale rows should have been discarded");
  for (const TerminalSearchMatch& match : matches) {
    Expect(match.row < scan.line_count, "every reported row should be in range");
  }
}

void TestHonorsMatchCap() {
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, /*rows=*/4, /*columns=*/40);
  for (int index = 0; index < 50; ++index) {
    TerminalSessionTestAccess::AppendOutput(session, "needle\r\n");
  }
  TerminalSearchScratch scratch;
  std::vector<TerminalSearchMatch> matches;
  const TerminalSession::SearchScan scan =
      session.FindMatches(MakeTerminalSearchQuery("needle", false, false), 0,
                          session.ScrollbackTrimTotal(), /*max_matches=*/10, scratch, matches);
  Expect(matches.size() == std::size_t{10}, "the cap should bound the reported matches");
  Expect(scan.truncated, "hitting the cap should be reported so the count can say so");
}

}  // namespace

void RegisterTerminalSearchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalSearch/LiteralMatchesWithGridColumns",
          TestFindsLiteralMatchesWithGridColumns);
  AddTest(tests, "TerminalSearch/OverlappingMatches", TestReportsOverlappingMatches);
  AddTest(tests, "TerminalSearch/WholeWordRejectsSubstrings", TestWholeWordRejectsSubstringHits);
  AddTest(tests, "TerminalSearch/EmptyCellsReadAsSpaces", TestEmptyCellsReadAsSpaces);
  AddTest(tests, "TerminalSearch/WideGlyphSpansBothColumns", TestWideGlyphSpansBothColumns);
  AddTest(tests, "TerminalSearch/FoldsNonAsciiCase", TestFoldsNonAsciiCase);
  AddTest(tests, "TerminalSearch/ScansScrollback", TestScansScrollbackAndReportsStableBoundary);
  AddTest(tests, "TerminalSearch/IncrementalRescan", TestIncrementalRescanExtendsSettledMatches);
  AddTest(tests, "TerminalSearch/StaleTrimForcesFullRescan", TestStaleTrimCounterForcesFullRescan);
  AddTest(tests, "TerminalSearch/HonorsMatchCap", TestHonorsMatchCap);
}

}  // namespace microide::tests
