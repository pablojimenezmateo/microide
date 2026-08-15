#include "TestSupport.h"

#include "editor/SyntaxHighlighter.h"
#include "workspace/state/SurfaceTokenWindow.h"

#if MICROIDE_PERF_HARNESS_BUILD
#include "perf/AllocationCounter.h"
#endif

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

}  // namespace

void RegisterSurfaceTokenWindowTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SurfaceTokenWindow/MatchesTheWholePaneTokenization",
          TestSurfaceTokenWindowMatchesTheWholePaneTokenization);
  AddTest(tests, "SurfaceTokenWindow/ScrollBackRetokenizesCorrectly",
          TestSurfaceTokenWindowScrollBackRetokenizesCorrectly);
  AddTest(tests, "SurfaceTokenWindow/BudgetedWalkConvergesWithoutWrongTokens",
          TestSurfaceTokenWindowBudgetedWalkConvergesWithoutWrongTokens);
  AddTest(tests, "SurfaceTokenWindow/SteadyScrollDoesNotAllocate",
          TestSurfaceTokenWindowSteadyScrollDoesNotAllocate);
}

}  // namespace microide::tests
