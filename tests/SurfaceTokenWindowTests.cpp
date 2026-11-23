#include "TestSupport.h"

#include "editor/SyntaxHighlighter.h"
#include "workspace/state/SurfaceTokenWindow.h"

#if MICROIDE_PERF_HARNESS_BUILD
#include "perf/AllocationCounter.h"
#endif

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::SyntaxHighlighter;
using microide::editor::SyntaxState;
using microide::editor::SyntaxTokenKind;
using microide::workspace::SurfaceTokenWindow;

// A C++ pane with a block comment that spans many lines, so a row's tokens
// genuinely depend on the state left by every row above it. That dependence is
// what makes the window's per-row state table load-bearing rather than an
// optimization: get it wrong and a row re-tokenized after a scroll comes back
// as code instead of comment.
struct Pane {
  std::vector<std::string> lines;
  std::filesystem::path path{"/tmp/surface-token-window.cpp"};
};

Pane MakePane(std::size_t line_count) {
  Pane pane;
  pane.lines.reserve(line_count);
  for (std::size_t i = 0; i < line_count; ++i) {
    // A /* … */ region opened every 100 lines and closed 50 lines later, so
    // roughly half the file is inside a multi-line comment.
    const std::size_t phase = i % 100;
    if (phase == 0) {
      pane.lines.push_back("/* region opens here");
    } else if (phase == 50) {
      pane.lines.push_back("region closes here */");
    } else {
      pane.lines.push_back("int value_" + std::to_string(i) + " = 1;");
    }
  }
  return pane;
}

// The answer the window must agree with: tokenize the whole pane from the top,
// keeping everything. This is the implementation the window replaced.
std::vector<std::vector<SyntaxTokenKind>> TokenizeWholePane(const Pane& pane) {
  std::vector<std::vector<SyntaxTokenKind>> tokens(pane.lines.size());
  SyntaxState state = SyntaxHighlighter::InitialState(pane.path, pane.lines);
  for (std::size_t i = 0; i < pane.lines.size(); ++i) {
    state = SyntaxHighlighter::HighlightLineInto(pane.lines[i], pane.path, state, &tokens[i]);
  }
  return tokens;
}

void EnsureRows(SurfaceTokenWindow& window, const Pane& pane, std::size_t start,
                std::size_t end, std::size_t walk_budget) {
  window.EnsureWindow(start, end, pane.path, walk_budget,
                      [&](std::size_t row) -> std::string_view { return pane.lines[row]; });
}

// Walk the whole pane in viewport-sized steps and check every row against the
// keep-everything oracle. The window only holds a bounded band, so most of these
// rows have had their buffer retired and re-filled at least once.
void TestSurfaceTokenWindowMatchesTheWholePaneTokenization() {
  constexpr std::size_t kLines = 900;
  constexpr std::size_t kViewport = 40;
  const Pane pane = MakePane(kLines);
  const auto expected = TokenizeWholePane(pane);

  SurfaceTokenWindow window;
  window.Reset(kLines, SyntaxHighlighter::InitialState(pane.path, pane.lines));

  for (std::size_t top = 0; top + kViewport <= kLines; top += kViewport / 2) {
    // Budget high enough that the frontier keeps up; the budgeted case is its
    // own test below.
    EnsureRows(window, pane, top, top + kViewport, kLines);
    for (std::size_t row = top; row < top + kViewport; ++row) {
      Expect(window.Tokens(row) == expected[row],
             "a row in the requested range must tokenize to what the whole-pane "
             "walk produces, however far the window has travelled");
    }
  }
}

// Scrolling BACK is the case the per-row state table exists for: those rows'
// buffers were retired long ago, and re-tokenizing them needs the state their
// predecessor left rather than a re-walk from the top.
void TestSurfaceTokenWindowScrollBackRetokenizesCorrectly() {
  constexpr std::size_t kLines = 900;
  constexpr std::size_t kViewport = 40;
  const Pane pane = MakePane(kLines);
  const auto expected = TokenizeWholePane(pane);

  SurfaceTokenWindow window;
  window.Reset(kLines, SyntaxHighlighter::InitialState(pane.path, pane.lines));

  // Jump to the bottom, then walk back to the top a viewport at a time.
  EnsureRows(window, pane, kLines - kViewport, kLines, kLines);
  for (std::size_t top = kLines - kViewport; top > 0;
       top = top > kViewport ? top - kViewport : 0) {
    EnsureRows(window, pane, top, top + kViewport, kLines);
    for (std::size_t row = top; row < top + kViewport; ++row) {
      Expect(window.Tokens(row) == expected[row],
             "a row revisited after the window moved past it must come back with "
             "the same tokens, not with the region state reset");
    }
    if (top <= kViewport) {
      break;
    }
  }
  EnsureRows(window, pane, 0, kViewport, kLines);
  for (std::size_t row = 0; row < kViewport; ++row) {
    Expect(window.Tokens(row) == expected[row], "row 0's window must resume from the initial state");
  }
}

// The walk is budgeted so a jump to the bottom of a large file does not stall one
// frame. Rows past the frontier report no tokens rather than wrong ones, and
// repeated calls converge on the right answer.
void TestSurfaceTokenWindowBudgetedWalkConvergesWithoutWrongTokens() {
  constexpr std::size_t kLines = 900;
  constexpr std::size_t kViewport = 30;
  constexpr std::size_t kBudget = 64;
  const Pane pane = MakePane(kLines);
  const auto expected = TokenizeWholePane(pane);

  SurfaceTokenWindow window;
  window.Reset(kLines, SyntaxHighlighter::InitialState(pane.path, pane.lines));

  const std::size_t top = kLines - kViewport;
  bool reached = false;
  for (int pass = 0; pass < 100 && !reached; ++pass) {
    EnsureRows(window, pane, top, kLines, kBudget);
    // Whatever is populated must be RIGHT at every point, not just at the end.
    for (std::size_t row = top; row < kLines; ++row) {
      const auto& tokens = window.Tokens(row);
      Expect(tokens.empty() || tokens == expected[row],
             "a partially walked window must report no tokens for a row it has not "
             "reached, never tokens computed from an unfinished state");
    }
    reached = window.frontier() >= kLines;
  }
  Expect(reached, "a budgeted walk must reach the end of the pane in bounded passes");
  for (std::size_t row = top; row < kLines; ++row) {
    Expect(window.Tokens(row) == expected[row],
           "once the frontier arrives, the requested rows must be fully tokenized");
  }
}

// The point of the change: a steady scroll reuses buffers instead of allocating
// one per row. Without the pool this is one allocation per row entering the
// window, which over a large file was 24,196 of merge_large.scroll_burst's
// 24,392 (TD-2026-08-15-242).
void TestSurfaceTokenWindowSteadyScrollDoesNotAllocate() {
#if MICROIDE_PERF_HARNESS_BUILD
  namespace perf = microide::tests::perf;
  constexpr std::size_t kLines = 2000;
  constexpr std::size_t kViewport = 40;
  const Pane pane = MakePane(kLines);

  SurfaceTokenWindow window;
  window.Reset(kLines, SyntaxHighlighter::InitialState(pane.path, pane.lines));

  // Warm: fill the band once and let the pool reach its working size.
  for (std::size_t top = 0; top < 400; top += kViewport) {
    EnsureRows(window, pane, top, top + kViewport, kLines);
  }

  const auto before = perf::Allocations::Snapshot();
  for (std::size_t top = 400; top + kViewport <= kLines; top += kViewport) {
    EnsureRows(window, pane, top, top + kViewport, kLines);
  }
  const auto delta = perf::Allocations::DeltaSince(before);

  // 1,560 rows entered the window; one buffer each -- what this replaced -- is
  // 1,560 allocations. What is left is capacity convergence: the pool holds at
  // most a band's worth of buffers, and one takes a growth the first time it is
  // handed a line longer than any it has held. That is bounded by the number of
  // BUFFERS rather than by the number of rows, which is exactly the distinction
  // being asserted. Measured 168.
  Expect(delta.allocations < 400,
         "a steady scroll must recycle retired token buffers rather than allocate "
         "one per row it moves onto");
#endif
}

// Soak a64655101a5a: a diff scrolled so the only row on screen is a collapsed
// run reaching the end of the file asks for the empty range [end, end). The
// window returned before walking the state frontier, the compare render saw it
// still behind the visible rows and asked for another frame -- 80 a second, for
// as long as the app sat idle.
void TestSurfaceTokenWindowAnEmptyRangeStillWalksTheFrontierToIt() {
  const Pane pane = MakePane(300);
  SurfaceTokenWindow window;
  window.Reset(pane.lines.size(), SyntaxHighlighter::InitialState(pane.path, pane.lines));
  for (int frame = 0; frame < 4 && window.frontier() < pane.lines.size(); ++frame) {
    EnsureRows(window, pane, pane.lines.size(), pane.lines.size(), 256);
  }
  Expect(window.frontier() == pane.lines.size(),
         "the frontier reaches the end of an empty range: it stopped at " + std::to_string(window.frontier()));
}

// A walk with time left keeps going past its row step: the compare render walked
// 256 rows a frame and repainted its pane after each, so a diff scrolled to row
// 30,000 of a 60,000-line file took ~117 frames of a pane nobody saw change (soak
// a64655101a5a: 306 wakes a second, every one a repaint of the compare pane). One
// with no time left still takes its step, so a walk always progresses.
void TestSurfaceTokenWindowAWalkWithTimeLeftKeepsGoing() {
  const Pane pane = MakePane(5000);
  const std::size_t lines = pane.lines.size();
  const auto line_at = [&](std::size_t row) -> std::string_view { return pane.lines[row]; };

  SurfaceTokenWindow timed;
  timed.Reset(lines, SyntaxHighlighter::InitialState(pane.path, pane.lines));
  timed.EnsureWindow(lines - 40, lines, pane.path,
                     SurfaceTokenWindow::WalkBudget{256, std::chrono::steady_clock::now() + std::chrono::seconds(10)},
                     line_at);
  Expect(timed.frontier() == lines,
         "a walk with time left reaches the window in one step: it stopped at " + std::to_string(timed.frontier()));

  SurfaceTokenWindow spent;
  spent.Reset(lines, SyntaxHighlighter::InitialState(pane.path, pane.lines));
  spent.EnsureWindow(lines - 40, lines, pane.path,
                     SurfaceTokenWindow::WalkBudget{256, std::chrono::steady_clock::now()},
                     line_at);
  Expect(spent.frontier() == 256,
         "a walk out of time still takes its row step: it stopped at " + std::to_string(spent.frontier()));
}

// An edit re-points the window at the pane from its first changed line: the
// states and token buffers above it stand (a row's tokens follow from its text
// and the state it resumes from), and the walk restarts there. It restarted at
// the top: a keystroke at line 40,000 of a diff walked 40,000 rows again.
void TestSurfaceTokenWindowAnEditRestartsTheWalkAtItsLine() {
  constexpr std::size_t kLines = 900;
  constexpr std::size_t kTop = 600;
  constexpr std::size_t kBottom = 640;
  constexpr std::size_t kEdit = 620;
  Pane pane = MakePane(kLines);
  const SyntaxState initial = SyntaxHighlighter::InitialState(pane.path, pane.lines);
  SurfaceTokenWindow window;
  window.Reset(kLines, initial);
  EnsureRows(window, pane, kTop, kBottom, kLines);
  Expect(window.frontier() >= kBottom && !window.Tokens(kTop + 5).empty(), "the band is walked and filled");

  // A comment opened at the edit (every row below resumes from another state),
  // and a line added under it (every row below moves down one).
  pane.lines[kEdit] = "int edited = 1; /* opened by the edit";
  pane.lines.insert(pane.lines.begin() + static_cast<std::ptrdiff_t>(kEdit) + 1, "int added = 2;");
  const auto expected = TokenizeWholePane(pane);
  window.ResetFrom(kEdit, pane.lines.size(), initial);
  Expect(window.line_count() == kLines + 1, "the window has the pane's new line count");
  Expect(window.frontier() == kEdit, "the walk restarts at the edited line, not at the top: " +
                                         std::to_string(window.frontier()));
  for (std::size_t row = kTop; row < kEdit; ++row) {
    Expect(window.Tokens(row) == expected[row], "a row above the edit keeps its tokens");
  }
  for (std::size_t row = kEdit; row < kBottom; ++row) {
    Expect(window.Tokens(row).empty(), "a row from the edit on has handed its buffer back");
  }
  // One step of 40 rows reaches the bottom of the band: 20 rows were left to walk.
  EnsureRows(window, pane, kTop, kBottom, kBottom - kTop);
  Expect(window.frontier() == kBottom, "the walk from the edit reaches the band in one step: " +
                                           std::to_string(window.frontier()));
  for (std::size_t row = kTop; row < kBottom; ++row) {
    Expect(window.Tokens(row) == expected[row],
           "after the edit every row of the band tokenizes as a walk from the top does (row " +
               std::to_string(row) + ")");
  }

  // Nothing changed (the count alone): nothing is walked again.
  window.ResetFrom(pane.lines.size(), pane.lines.size(), initial);
  Expect(window.frontier() == kBottom && window.Tokens(kTop) == expected[kTop], "no change keeps everything");
  // The first line, or another initial state (the language changed): from the top.
  window.ResetFrom(0, pane.lines.size(), initial);
  Expect(window.frontier() == 0 && window.Tokens(kTop).empty(), "a change at the first line restarts at the top");
  EnsureRows(window, pane, kTop, kBottom, pane.lines.size());
  SyntaxState other = initial;
  other.definition_id += 1;
  window.ResetFrom(kEdit, pane.lines.size(), other);
  Expect(window.frontier() == 0 && window.Tokens(kTop).empty(), "another initial state restarts at the top");
}

// Typing: an edit and a re-walk of the band, over and over. Steady, it allocates
// nothing: the rows from the edit on hand their buffers to the pool and take
// them back, and the tables are resized within their capacity.
void TestSurfaceTokenWindowSteadyEditsDoNotAllocate() {
#if MICROIDE_PERF_HARNESS_BUILD
  namespace perf = microide::tests::perf;
  constexpr std::size_t kLines = 2000;
  constexpr std::size_t kTop = 1500;
  constexpr std::size_t kBottom = 1540;
  Pane pane = MakePane(kLines);
  const SyntaxState initial = SyntaxHighlighter::InitialState(pane.path, pane.lines);
  SurfaceTokenWindow window;
  window.Reset(kLines, initial);
  const auto edit = [&](std::size_t round) {
    // A line added and taken away in turn, at a line that moves through the band.
    const std::size_t line = kTop + 5 + round % 20;
    if (round % 2 == 0) {
      pane.lines.insert(pane.lines.begin() + static_cast<std::ptrdiff_t>(line), "int typed = 3;");
    } else {
      pane.lines.erase(pane.lines.begin() + static_cast<std::ptrdiff_t>(line));
    }
    window.ResetFrom(line, pane.lines.size(), initial);
    EnsureRows(window, pane, kTop, kBottom, kLines);
  };
  EnsureRows(window, pane, kTop, kBottom, kLines);
  for (std::size_t round = 0; round < 40; ++round) {
    edit(round);  // warm: the pool and the tables reach their working size
  }
  pane.lines.reserve(pane.lines.size() + 8);
  const auto before = perf::Allocations::Snapshot();
  for (std::size_t round = 40; round < 240; ++round) {
    edit(round);
  }
  const auto delta = perf::Allocations::DeltaSince(before);
  // The pane's own vector of strings is the test's (its inserted literal fits a
  // small string, its capacity is reserved); what is counted is the window's.
  Expect(delta.allocations == 0, "a steady run of edits must allocate nothing in the window: " +
                                     std::to_string(delta.allocations));
#endif
}

}  // namespace

void RegisterSurfaceTokenWindowTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SurfaceTokenWindow/AnEmptyRangeStillWalksTheFrontierToIt",
          TestSurfaceTokenWindowAnEmptyRangeStillWalksTheFrontierToIt);
  AddTest(tests, "SurfaceTokenWindow/MatchesTheWholePaneTokenization",
          TestSurfaceTokenWindowMatchesTheWholePaneTokenization);
  AddTest(tests, "SurfaceTokenWindow/ScrollBackRetokenizesCorrectly",
          TestSurfaceTokenWindowScrollBackRetokenizesCorrectly);
  AddTest(tests, "SurfaceTokenWindow/BudgetedWalkConvergesWithoutWrongTokens",
          TestSurfaceTokenWindowBudgetedWalkConvergesWithoutWrongTokens);
  AddTest(tests, "SurfaceTokenWindow/AWalkWithTimeLeftKeepsGoing", TestSurfaceTokenWindowAWalkWithTimeLeftKeepsGoing);
  AddTest(tests, "SurfaceTokenWindow/AnEditRestartsTheWalkAtItsLine",
          TestSurfaceTokenWindowAnEditRestartsTheWalkAtItsLine);
  AddTest(tests, "SurfaceTokenWindow/SteadyEditsDoNotAllocate", TestSurfaceTokenWindowSteadyEditsDoNotAllocate);
  AddTest(tests, "SurfaceTokenWindow/SteadyScrollDoesNotAllocate",
          TestSurfaceTokenWindowSteadyScrollDoesNotAllocate);
}

}  // namespace microide::tests
