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

struct Pane {
  std::vector<std::string> lines;
  std::filesystem::path path{"/tmp/surface-token-window.cpp"};
};

Pane MakePane(std::size_t line_count) {
  Pane pane;
  pane.lines.reserve(line_count);
  for (std::size_t i = 0; i < line_count; ++i) {
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

void TestSurfaceTokenWindowMatchesTheWholePaneTokenization() {
  constexpr std::size_t kLines = 900;
  constexpr std::size_t kViewport = 40;
  const Pane pane = MakePane(kLines);
  const auto expected = TokenizeWholePane(pane);

  SurfaceTokenWindow window;
  window.Reset(kLines, SyntaxHighlighter::InitialState(pane.path, pane.lines));

  for (std::size_t top = 0; top + kViewport <= kLines; top += kViewport / 2) {
    EnsureRows(window, pane, top, top + kViewport, kLines);
    for (std::size_t row = top; row < top + kViewport; ++row) {
      Expect(window.Tokens(row) == expected[row],
             "a row in the requested range must tokenize to what the whole-pane "
             "walk produces, however far the window has travelled");
    }
  }
}

void TestSurfaceTokenWindowScrollBackRetokenizesCorrectly() {
  constexpr std::size_t kLines = 900;
  constexpr std::size_t kViewport = 40;
  const Pane pane = MakePane(kLines);
  const auto expected = TokenizeWholePane(pane);

  SurfaceTokenWindow window;
  window.Reset(kLines, SyntaxHighlighter::InitialState(pane.path, pane.lines));

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

void TestSurfaceTokenWindowSteadyScrollDoesNotAllocate() {
#if MICROIDE_PERF_HARNESS_BUILD
  namespace perf = microide::tests::perf;
  constexpr std::size_t kLines = 2000;
  constexpr std::size_t kViewport = 40;
  const Pane pane = MakePane(kLines);

  SurfaceTokenWindow window;
  window.Reset(kLines, SyntaxHighlighter::InitialState(pane.path, pane.lines));

  for (std::size_t top = 0; top < 400; top += kViewport) {
    EnsureRows(window, pane, top, top + kViewport, kLines);
  }

  const auto before = perf::Allocations::Snapshot();
  for (std::size_t top = 400; top + kViewport <= kLines; top += kViewport) {
    EnsureRows(window, pane, top, top + kViewport, kLines);
  }
  const auto delta = perf::Allocations::DeltaSince(before);

  Expect(delta.allocations < 400,
         "a steady scroll must recycle retired token buffers rather than allocate "
         "one per row it moves onto");
#endif
}

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
  EnsureRows(window, pane, kTop, kBottom, kBottom - kTop);
  Expect(window.frontier() == kBottom, "the walk from the edit reaches the band in one step: " +
                                           std::to_string(window.frontier()));
  for (std::size_t row = kTop; row < kBottom; ++row) {
    Expect(window.Tokens(row) == expected[row],
           "after the edit every row of the band tokenizes as a walk from the top does (row " +
               std::to_string(row) + ")");
  }

  window.ResetFrom(pane.lines.size(), pane.lines.size(), initial);
  Expect(window.frontier() == kBottom && window.Tokens(kTop) == expected[kTop], "no change keeps everything");
  window.ResetFrom(0, pane.lines.size(), initial);
  Expect(window.frontier() == 0 && window.Tokens(kTop).empty(), "a change at the first line restarts at the top");
  EnsureRows(window, pane, kTop, kBottom, pane.lines.size());
  SyntaxState other = initial;
  other.definition_id += 1;
  window.ResetFrom(kEdit, pane.lines.size(), other);
  Expect(window.frontier() == 0 && window.Tokens(kTop).empty(), "another initial state restarts at the top");
}

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
    edit(round);
  }
  pane.lines.reserve(pane.lines.size() + 8);
  const auto before = perf::Allocations::Snapshot();
  for (std::size_t round = 40; round < 240; ++round) {
    edit(round);
  }
  const auto delta = perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0, "a steady run of edits must allocate nothing in the window: " +
                                     std::to_string(delta.allocations));
#endif
}

}

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

}
