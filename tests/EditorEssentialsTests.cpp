#include "TestSupport.h"

#include "editor/RowDecorationBuilder.h"

#include <set>

#include "editor/BracketScanner.h"
#include "editor/FoldingModel.h"
#include "editor/IndentDetect.h"
#include "editor/IndentGuides.h"
#include "editor/LineSpan.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextBuffer.h"
#include "editor/LanguageContractView.h"
#include "editor/SaveNormalization.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"
#include "plugin/PluginHost.h"
#include "workspace/WorkspaceFoldingRefresh.h"
#include "workspace/WorkspaceIndentDetectApply.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/state/WorkspaceTabState.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/WorkspaceContext.h"
#include "util/PerformanceCounters.h"

#include <algorithm>
#include <optional>
#include <cstdint>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::TextPosition;
using microide::editor::TextViewport;

void TestBracketScannerForwardMatch() {
  TextViewport viewport;
  viewport.LoadContent("if (a) {\n  return 1;\n}\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 7);  // caret right after '{' opener (line 0, col 7 = '{')
  auto match = microide::editor::FindBracketMatch(viewport, viewport.cursor_line(),
                                                  viewport.cursor_column());
  Expect(match.has_value(), "expected a bracket match for '{' on line 0");
  Expect(match->open_line == 0 && match->open_column == 7,
         "open should be at line 0 col 7");
  Expect(match->close_line == 2 && match->close_column == 0,
         "close should be at line 2 col 0");
}

void TestBracketScannerBackwardMatch() {
  TextViewport viewport;
  viewport.LoadContent("{\n  x;\n}\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(2, 1);  // caret right after '}'
  auto match = microide::editor::FindBracketMatch(viewport, 2, 0);
  Expect(match.has_value(), "expected a backward match for '}'");
  Expect(match->open_line == 0 && match->open_column == 0,
         "backward match should land on line 0 col 0");
}

void TestBracketScannerNoMatchOnUnbalanced() {
  TextViewport viewport;
  viewport.LoadContent("{ no closer\n", "/tmp/sample.cpp");
  auto match = microide::editor::FindBracketMatch(viewport, 0, 0);
  Expect(!match.has_value(), "unbalanced opener should not match");
}

void TestBracketScannerSkipsStringClosers() {
  TextViewport viewport;
  viewport.LoadContent(
      "void f() {\n"
      "  const char* s = \"}\";\n"
      "}\n",
      "/tmp/bracket-skip-str.cpp");
  viewport.MoveCursorTo(0, 9);  // on '{' after "void f() "
  auto match = microide::editor::FindBracketMatch(viewport, viewport.cursor_line(),
                                                  viewport.cursor_column());
  Expect(match.has_value(), "expected match past string literal closer");
  Expect(match->open_line == 0 && match->open_column == 9, "open brace line 0 col 9");
  Expect(match->close_line == 2 && match->close_column == 0,
         "real closer should be final brace, not quoted }");
}

void TestBracketScannerSkipsCommentBraces() {
  TextViewport viewport;
  viewport.LoadContent(
      "void f() {\n"
      "  /* } */\n"
      "  x;\n"
      "}\n",
      "/tmp/bracket-skip-comment.cpp");
  viewport.MoveCursorTo(0, 9);
  auto match = microide::editor::FindBracketMatch(viewport, 0, 9);
  Expect(match.has_value(), "expected match skipping commented braces");
  Expect(match->close_line == 3 && match->close_column == 0,
         "closer should be the real function terminator");
}

void TestBracketScannerNoMatchWhenAnchorInsideString() {
  TextViewport viewport;
  viewport.LoadContent("auto s = \"{\";\n", "/tmp/bracket-in-str.cpp");
  viewport.MoveCursorTo(0, 10);  // on '{' inside string literal
  auto match = microide::editor::FindBracketMatch(viewport, 0, 10);
  Expect(!match.has_value(), "bracket inside string should not participate in matching");
}

// Exercises the windowed scan (base > 0): with a caret several lines down and a
// small per-side window, FindBracketMatch materializes only [caret-max, caret+max]
// and indexes it through WindowLines. The returned pair must still be in absolute
// coordinates.
void TestBracketScannerWindowedDeepCaret() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\nd\n{\n  x;\n}\ne\n", "/tmp/bracket-window.cpp");
  // Opener at line 4 col 0, closer at line 6 col 0. Caret just past the opener,
  // a per-side window of 3 -> window base is line 1 (absolute), not 0.
  auto forward = microide::editor::FindBracketMatch(viewport, 4, 1, 3);
  Expect(forward.has_value(), "windowed forward scan should find the pair");
  Expect(forward->open_line == 4 && forward->open_column == 0, "absolute opener at line 4 col 0");
  Expect(forward->close_line == 6 && forward->close_column == 0, "absolute closer at line 6 col 0");

  // Backward from the closer, window base line 3 (absolute).
  auto backward = microide::editor::FindBracketMatch(viewport, 6, 1, 3);
  Expect(backward.has_value(), "windowed backward scan should find the pair");
  Expect(backward->open_line == 4 && backward->open_column == 0,
         "backward absolute opener at line 4 col 0");
  Expect(backward->close_line == 6 && backward->close_column == 0,
         "backward absolute closer at line 6 col 0");
}

// Suppression is a per-LINE question. The matchers walk a line byte by byte, and
// each byte used to re-resolve that line's token vector through
// `TextViewport::HighlightedLineTokens` — so a scan cost one highlight-cache probe
// per BYTE. On a file with no line breaks in it that is one probe per byte of the
// document, per caret move: 12.6 million of them on the 2 MiB perf fixture.
//
// Pinned through the viewport's own highlight-query counter, because the answer is
// identical either way and only the counter can tell the two apart.
void TestBracketScannerProbesTokensOncePerLineNotPerByte() {
  constexpr std::size_t kLineBytes = 64u * 1024u;
  std::string line = "(";
  line.append(kLineBytes, 'x');
  line.push_back(')');
  TextViewport viewport;
  viewport.LoadContent(line + "\n", "/tmp/bracket-long-line.cpp");

  // Warm the highlight cache for the line so the count below is probes, not the
  // one-time highlight behind the first probe.
  (void)viewport.HighlightedLineTokens(0);
  viewport.ResetCacheStats();

  const auto match = microide::editor::FindBracketMatch(viewport, 0, 1);
  Expect(match.has_value(), "the pair spanning the long line should still match");
  Expect(match->open_column == 0 && match->close_column == kLineBytes + 1,
         "the matched columns must be the line's own brackets");

  const std::size_t probes = viewport.CacheStats().highlight_queries;
  // One line was scanned, so a handful of probes is right and anything
  // proportional to the line's bytes is the regression.
  Expect(probes <= 8,
         "a bracket scan must probe the token cache per line, not per byte (probed " +
             std::to_string(probes) + " times over " + std::to_string(kLineBytes) + " bytes)");
}

// `max_lines_each_side` bounds the scan in lines, which bounds work only if lines
// are bounded. `kMaxBracketMatchScanBytes` is the bound in the unit that actually
// costs; past it the match is reported not-found, exactly as an unbalanced bracket
// inside the line window already is.
void TestBracketScannerStopsAtTheScanByteCap() {
  // The byte budget spans the whole scan window, and since TD-2026-08-05-133 a
  // caret line past the tokenization cap is refused outright -- so the budget can
  // only be reached across MANY lines now, and a single over-long line no longer
  // exercises it. Building this from one line would pass for the other reason.
  const auto document_of = [](std::size_t body_lines) {
    std::string text = "(\n";
    for (std::size_t i = 0; i < body_lines; ++i) {
      text.append(1000, 'x');
      text.push_back('\n');
    }
    text += ")\n";
    return text;
  };
  const std::size_t over_budget_lines = microide::editor::kMaxBracketMatchScanBytes / 1000 + 64;

  TextViewport viewport;
  viewport.LoadContent(document_of(over_budget_lines), "/tmp/bracket-over-cap.cpp");
  Expect(!microide::editor::FindBracketMatch(viewport, 0, 1).has_value(),
         "a forward scan past the byte cap reports no match");
  Expect(!microide::editor::FindBracketMatch(viewport, over_budget_lines + 1, 1).has_value(),
         "a backward scan past the byte cap reports no match");

  // Half the budget still matches, so the cap bounds work rather than breaking
  // bracket matching on merely-large windows.
  TextViewport under_viewport;
  under_viewport.LoadContent(document_of(over_budget_lines / 4), "/tmp/bracket-under-cap.cpp");
  Expect(microide::editor::FindBracketMatch(under_viewport, 0, 1).has_value(),
         "a scan comfortably inside the byte cap still matches");
}

// TD-2026-08-05-133: a caret line past `runtime_syntax::kMaxHighlightLineBytes`
// matches no bracket at all. That is the argument FoldingModel::kMaxBracketScanLineBytes
// already makes about the same bracket on the same line -- past the tokenization
// cap the line has no tokens, so a brace inside a string literal is
// indistinguishable from a real one and the pair would be arbitrary rather than
// approximate. It is also what keeps the scan from reading the line: on a
// piece-tree source the window build takes a LineView per line, which copies any
// line that spans pieces.
void TestBracketScannerRefusesALineTooLongToTokenize() {
  const std::size_t cap = microide::editor::runtime_syntax::kMaxHighlightLineBytes;
  const auto line_of = [](std::size_t filler) {
    std::string line = "()";
    line.append(filler, 'x');
    return line;
  };

  // Adjacent pair at column 0 -- the cheapest possible match, so a refusal here is
  // the cap and nothing else.
  TextViewport under;
  under.LoadContent(line_of(cap - 8) + "\n", "/tmp/bracket-under-line-cap.cpp");
  Expect(microide::editor::FindBracketMatch(under, 0, 1).has_value(),
         "a line just under the tokenization cap still matches its brackets");

  TextViewport over;
  over.LoadContent(line_of(cap + 8) + "\n", "/tmp/bracket-over-line-cap.cpp");
  Expect(!microide::editor::FindBracketMatch(over, 0, 1).has_value(),
         "one byte past the cap the line contributes no bracket match");

  // The work claim: refusing must not read the line. Split it first, so reading it
  // whole could only be served by a copy.
  TextViewport spanning;
  spanning.LoadContent(line_of(2 * cap) + "\n", "/tmp/bracket-spanning.cpp");
  spanning.InsertText("Z");
  spanning.Backspace();
  util::ResetPerformanceCounters();
  Expect(!microide::editor::FindBracketMatch(spanning, 0, 1).has_value(),
         "the long spanning line is refused");
  const std::uint64_t copied =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes);
  Expect(copied == 0, "a refused caret line must not be read (copied " + std::to_string(copied) +
                          " bytes)");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorBracketMatchLineTooLong) > 0,
         "the refusal counter must have moved -- otherwise the zero above proves nothing");
  util::ResetPerformanceCounters();
}

// A line operation over an N-line selection is ONE splice, not N.
//
// `ApplyEntryToBuffer` used to write a same-count replacement line by line, which
// was the cheap form when the document was a `std::vector<std::string>`. Against
// the piece tree every `SetLine` is a full `ReplaceLineRange` -- its own joined
// replacement string, delete+insert, revision bump and line-cache wipe -- so a
// 1,000-line Toggle Comment cost 1,000 of them (TD-2026-08-06-159). Pinned with
// the document-edit counter because the buffer contents are identical either way:
// only the cost tells the two apart.
void TestShapingLineOpAppliesOneDocumentSplice() {
  constexpr std::size_t kLines = 200;
  std::string content;
  for (std::size_t i = 0; i < kLines; ++i) {
    content += "    int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/line-op-one-splice.cpp");
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(kLines - 1, viewport.lines().LineView(kLines - 1).size(), true);

  util::ResetPerformanceCounters();
  Expect(microide::editor::ToggleLineComment(viewport, "//"),
         "toggling the whole selection must change the buffer");
  const std::uint64_t edits =
      util::ReadPerformanceCounter(util::PerfCounterId::DocumentEdits);
  Expect(edits == 1, "a " + std::to_string(kLines) +
                         "-line toggle must be one document splice, not one per line (was " +
                         std::to_string(edits) + ")");
  Expect(viewport.lines().LineView(0) == "    // int value_0 = 0;",
         "the comment marker goes in at the common indent");

  // The uncomment branch is the one that was rebuilt to a single exact-sized
  // buffer; round-tripping proves it still strips exactly what it inserted.
  util::ResetPerformanceCounters();
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(kLines - 1, viewport.lines().LineView(kLines - 1).size(), true);
  Expect(microide::editor::ToggleLineComment(viewport, "//"), "untoggling must change the buffer");
  const std::uint64_t undo_edits =
      util::ReadPerformanceCounter(util::PerfCounterId::DocumentEdits);
  Expect(undo_edits == 1, "the uncomment pass is one splice too (was " +
                              std::to_string(undo_edits) + ")");
  for (std::size_t i = 0; i < kLines; ++i) {
    Expect(viewport.lines().LineView(i) ==
               "    int value_" + std::to_string(i) + " = " + std::to_string(i) + ";",
           "line " + std::to_string(i) + " round-trips through comment/uncomment");
  }
  util::ResetPerformanceCounters();
}

void TestShapingMoveLineDown() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::MoveLineDown(viewport),
         "MoveLineDown on a non-last line should change buffer");
  Expect(viewport.lines().size() >= 3, "lines preserved");
  Expect(viewport.lines()[0] == "b" && viewport.lines()[1] == "a" &&
             viewport.lines()[2] == "c",
         "first two lines should swap on MoveLineDown");
}

// Regression: a whole-line selection ends at column 0 of the line AFTER the block,
// so ResolveLineRange excludes that trailing line and its end.line is range_last+1.
// MoveLineDown must carry the selection with the moved block instead of dropping it
// to the single-caret fallback.
void TestShapingMoveLineDownKeepsWholeLineSelection() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\nd", "/tmp/moveline-sel.txt");
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(2, 0, /*extend_selection=*/true);  // whole-line selection of a,b
  Expect(microide::editor::MoveLineDown(viewport), "MoveLineDown on the a,b block should succeed");
  Expect(viewport.lines()[0] == "c" && viewport.lines()[1] == "a" && viewport.lines()[2] == "b" &&
             viewport.lines()[3] == "d",
         "block [a,b] moves below c");
  const auto selection = viewport.selection_range();
  Expect(selection.has_value(), "the whole-line selection must survive the move, not be dropped");
  Expect(selection->start == microide::editor::TextPosition{1, 0} &&
             selection->end == microide::editor::TextPosition{3, 0},
         "the selection follows the moved block (now lines 1..2, exclusive end at line 3)");
}

// TD-2026-08-07-160: the fidelity claim. `ResolveLineRanges` used to widen to
// min..max over every caret, so a shaping op rewrote every line BETWEEN the
// carets — Ctrl+/ with carets on lines 10 and 100 commented all 91. VSCode
// comments two.
void TestShapingMultiCaretOpsDoNotTouchLinesBetweenCarets() {
  using microide::editor::SelectionRange;
  using microide::editor::TextPosition;

  {
    TextViewport viewport;
    viewport.LoadContent("l0\nl1\nl2\nl3\nl4\nl5", "/tmp/shaping-gap-comment.cpp");
    viewport.MoveCursorTo(1, 0);
    viewport.AddSecondaryCaret(4, 0);
    Expect(microide::editor::ToggleLineComment(viewport, "//"),
           "ToggleLineComment across two carets should succeed");
    Expect(viewport.lines()[1] == "// l1" && viewport.lines()[4] == "// l4",
           "both caret lines are commented");
    Expect(viewport.lines()[0] == "l0" && viewport.lines()[2] == "l2" &&
               viewport.lines()[3] == "l3" && viewport.lines()[5] == "l5",
           "every line that has no caret is untouched");
    Expect(viewport.Undo(), "the whole multi-region toggle is one undo step");
    Expect(viewport.lines()[1] == "l1" && viewport.lines()[4] == "l4",
           "undo restores both regions at once");
  }

  {
    // Adjacent carets merge into ONE region, so the op is not applied to a line
    // twice and the two lines are one edit rather than two abutting ones.
    TextViewport viewport;
    viewport.LoadContent("a\nb\nc\nd", "/tmp/shaping-adjacent.cpp");
    viewport.MoveCursorTo(1, 0);
    viewport.AddSecondaryCaret(2, 0);
    Expect(microide::editor::ToggleLineComment(viewport, "//"), "adjacent carets should toggle");
    Expect(viewport.lines()[1] == "// b" && viewport.lines()[2] == "// c",
           "both adjacent lines are commented exactly once");
    Expect(viewport.lines()[0] == "a" && viewport.lines()[3] == "d", "neighbours are untouched");
  }

  {
    // Two carets on the SAME line collapse to one region — an op applied twice to
    // one line would double-comment it.
    TextViewport viewport;
    viewport.LoadContent("aaaa\nbbbb", "/tmp/shaping-same-line.cpp");
    viewport.MoveCursorTo(0, 0);
    viewport.AddSecondaryCaret(0, 3);
    Expect(microide::editor::ToggleLineComment(viewport, "//"), "same-line carets should toggle");
    Expect(viewport.lines()[0] == "// aaaa", "the shared line is commented exactly once");
  }

  {
    // Each region decides for itself whether it is commenting or uncommenting,
    // and at what column — one LineCommentCommand per selection, as VSCode does.
    TextViewport viewport;
    viewport.LoadContent("// x\ngap\n    y", "/tmp/shaping-per-region.cpp");
    viewport.MoveCursorTo(0, 0);
    viewport.AddSecondaryCaret(2, 4);
    Expect(microide::editor::ToggleLineComment(viewport, "//"),
           "a mixed commented/uncommented caret set should still toggle");
    Expect(viewport.lines()[0] == "x", "the already-commented region uncomments");
    Expect(viewport.lines()[2] == "    // y",
           "the uncommented region comments AT ITS OWN INDENT, not at the other region's");
    Expect(viewport.lines()[1] == "gap", "the line between is untouched");
  }

  {
    // Outdent: only the caret regions lose indentation.
    TextViewport viewport;
    viewport.LoadContent("    a\n    b\n    c", "/tmp/shaping-gap-outdent.txt");
    viewport.SetSoftTabs(true);
    viewport.SetIndentWidth(4);
    viewport.MoveCursorTo(0, 4);
    viewport.AddSecondaryCaret(2, 4);
    Expect(microide::editor::OutdentSelection(viewport), "OutdentSelection should succeed");
    Expect(viewport.lines()[0] == "a" && viewport.lines()[2] == "c",
           "both caret regions are outdented");
    Expect(viewport.lines()[1] == "    b", "the line between keeps its indentation");
    Expect(viewport.cursor_column() == 0, "the primary caret's column follows its region's delta");
    Expect(viewport.Undo(), "the multi-region outdent is one undo step");
    Expect(viewport.lines()[0] == "    a" && viewport.lines()[1] == "    b" &&
               viewport.lines()[2] == "    c",
           "undo restores every region");
  }

  {
    // Sort: two ranged selections sort independently instead of being merged into
    // one span whose sort would interleave them.
    TextViewport viewport;
    viewport.LoadContent("b\na\nKEEP\nz\ny", "/tmp/shaping-gap-sort.txt");
    viewport.MoveCursorTo(0, 0);
    viewport.MoveCursorTo(1, 1, /*extend_selection=*/true);
    viewport.AddSecondaryCaretWithRange(SelectionRange{TextPosition{3, 0}, TextPosition{4, 1}});
    Expect(microide::editor::SortLines(viewport, /*ascending=*/true), "SortLines should succeed");
    Expect(viewport.lines()[0] == "a" && viewport.lines()[1] == "b",
           "the first selection sorts within itself");
    Expect(viewport.lines()[2] == "KEEP", "the line between the selections stays put");
    Expect(viewport.lines()[3] == "y" && viewport.lines()[4] == "z",
           "the second selection sorts within itself");
  }
}

// A move region pinned against the edge of the buffer no-ops on its own; the
// other regions still move. VSCode gives each selection its own MoveLinesCommand
// and a pinned one returns without an edit.
void TestShapingMoveLineSkipsOnlyThePinnedRegion() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\nd", "/tmp/shaping-pinned.txt");
  viewport.MoveCursorTo(0, 0);
  viewport.AddSecondaryCaret(3, 0);
  Expect(microide::editor::MoveLineUp(viewport),
         "MoveLineUp should succeed because one of the two regions can move");
  Expect(viewport.lines()[0] == "a" && viewport.lines()[1] == "b",
         "the region at the top of the buffer stays put");
  Expect(viewport.lines()[2] == "d" && viewport.lines()[3] == "c",
         "the region that can move still swaps with the line above it");
  Expect(viewport.cursor_line() == 0, "the pinned caret does not shift");

  TextViewport top_only;
  top_only.LoadContent("a\nb", "/tmp/shaping-pinned-only.txt");
  top_only.MoveCursorTo(0, 0);
  Expect(!microide::editor::MoveLineUp(top_only),
         "with nothing left that can move, the op is a no-op");
}

// TD-2026-07-17A-120: shaping actions must not drop a ranged secondary caret's
// selection anchor, and line-range resolution must cover lines spanned only by
// such an anchor.
void TestShapingPreservesRangedSecondaryCaretAnchors() {
  using microide::editor::SelectionRange;
  using microide::editor::TextPosition;

  {
    // A ranged secondary caret (anchor on a different line than its cursor) must
    // keep its anchor across a line move, not collapse to a bare caret.
    TextViewport viewport;
    viewport.LoadContent("aaa\nbbb\nccc\nddd", "/tmp/shaping-anchor-move.txt");
    viewport.MoveCursorTo(0, 1);
    viewport.AddSecondaryCaretWithRange(SelectionRange{TextPosition{2, 0}, TextPosition{2, 3}});
    Expect(microide::editor::MoveLineDown(viewport), "MoveLineDown should succeed");
    const auto ranges = viewport.secondary_caret_ranges();
    Expect(ranges.size() == 1, "the ranged secondary caret survives the move");
    Expect(ranges[0].position == TextPosition{3, 3},
           "the secondary caret's cursor follows the moved block (+1 line)");
    Expect(ranges[0].selection_anchor.has_value(),
           "the secondary caret's selection anchor is preserved, not dropped");
    Expect(ranges[0].selection_anchor.value() == TextPosition{3, 0},
           "the preserved anchor shifts with the moved block too");
  }

  {
    // ResolveLineRange must include a line spanned only by a secondary caret's
    // anchor. Primary caret has no selection on line 3; a ranged secondary caret
    // spans lines 0..1 (anchor on line 0, cursor on line 1). Indent must reach
    // line 0 — with the old positions-only resolution it was missed.
    TextViewport viewport;
    viewport.LoadContent("aa\nbb\ncc\ndd", "/tmp/shaping-anchor-indent.txt");
    viewport.SetSoftTabs(true);
    viewport.SetIndentWidth(2);
    viewport.MoveCursorTo(3, 0);
    viewport.AddSecondaryCaretWithRange(SelectionRange{TextPosition{0, 0}, TextPosition{1, 1}});
    Expect(microide::editor::IndentSelection(viewport), "IndentSelection should succeed");
    Expect(viewport.lines()[0] == "  aa",
           "the line covered only by the secondary anchor must be indented (A-120)");
    Expect(viewport.lines()[1] == "  bb", "the secondary caret's own line is indented");
    Expect(viewport.lines()[3] == "  dd", "the primary caret line is indented");
    Expect(viewport.lines()[2] == "cc",
           "the line BETWEEN the two regions is untouched (TD-2026-08-07-160)");
  }
}

// TD-2026-08-07-160: two carets are two regions, not one span between them. Each
// caret's line moves past its own neighbour — which is what VSCode does, one
// MoveLinesCommand per selection — and the whole thing is still ONE undo step.
void TestShapingMoveLineDownMultiCaretSingleUndoStep() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\nc\nd", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 0);
  viewport.AddSecondaryCaret(2, 0);
  Expect(microide::editor::MoveLineDown(viewport),
         "MoveLineDown on two disjoint caret lines should succeed");
  Expect(viewport.lines().size() == 4,
         "MoveLineDown should preserve line count when moving block before tail line");
  Expect(viewport.lines()[0] == "b" && viewport.lines()[1] == "a" && viewport.lines()[2] == "d" &&
             viewport.lines()[3] == "c",
         "each caret line swaps with the line below it; line 'b' between them is NOT dragged");
  Expect(viewport.Undo(),
         "undo after multi-caret MoveLineDown should succeed (single history entry)");
  Expect(viewport.lines()[0] == "a" && viewport.lines()[1] == "b" && viewport.lines()[2] == "c" &&
             viewport.lines()[3] == "d",
         "undo should restore the original line order atomically");
}

void TestShapingMoveLineUpMultiCaretSingleUndoStep() {
  TextViewport viewport;
  viewport.LoadContent("top\na\nb\nc", "/tmp/sample.txt");
  viewport.MoveCursorTo(1, 0);
  viewport.AddSecondaryCaret(3, 0);
  Expect(microide::editor::MoveLineUp(viewport),
         "MoveLineUp on two disjoint caret lines should succeed");
  Expect(viewport.lines().size() == 4,
         "MoveLineUp should preserve line count when moving block above prior line");
  Expect(viewport.lines()[0] == "a" && viewport.lines()[1] == "top" &&
             viewport.lines()[2] == "c" && viewport.lines()[3] == "b",
         "each caret line swaps with the line above it; line 'b' between them is NOT dragged");
  Expect(viewport.Undo(),
         "undo after multi-caret MoveLineUp should succeed (single history entry)");
  Expect(viewport.lines()[0] == "top" && viewport.lines()[1] == "a" && viewport.lines()[2] == "b" &&
             viewport.lines()[3] == "c",
         "undo should restore the original line order atomically");
}

void TestShapingMoveLineDownRedoPreservesMultiCaret() {
  TextViewport viewport;
  viewport.LoadContent("aaa\nbbb\nccc\nddd", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 2);
  viewport.AddSecondaryCaret(2, 2);
  Expect(microide::editor::MoveLineDown(viewport), "MoveLineDown should succeed");
  // Each caret line swaps with the one below it; both carets follow (+1 line,
  // same column).
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "primary caret should follow the moved block and keep its column");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets()[0].line == 3 &&
             viewport.secondary_carets()[0].column == 2,
         "secondary caret should follow the moved block");

  Expect(viewport.Undo(), "undo should succeed");
  Expect(viewport.Redo(), "redo should succeed");
  Expect(viewport.lines()[0] == "bbb" && viewport.lines()[1] == "aaa" &&
             viewport.lines()[2] == "ddd" && viewport.lines()[3] == "ccc",
         "redo should re-apply BOTH region moves");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "redo should restore the primary caret line AND column (regression: snapped to col 0)");
  Expect(viewport.secondary_carets().size() == 1 &&
             viewport.secondary_carets()[0].line == 3 &&
             viewport.secondary_carets()[0].column == 2,
         "redo should restore the secondary caret (regression: secondaries were dropped)");
}

void TestShapingMoveLineUpAtTopIsNoop() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 0);
  Expect(!microide::editor::MoveLineUp(viewport),
         "MoveLineUp at top should be a no-op and return false");
}

void TestShapingMoveLineDownMovesCursor() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 3);
  Expect(microide::editor::MoveLineDown(viewport),
         "MoveLineDown should succeed");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 3,
         "cursor should follow the moved line down one row");
}

void TestShapingMoveLineUpMovesCursor() {
  TextViewport viewport;
  viewport.LoadContent("alpha\nbeta\ngamma\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(2, 4);
  Expect(microide::editor::MoveLineUp(viewport),
         "MoveLineUp should succeed");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 4,
         "cursor should follow the moved line up one row");
}

void TestShapingMoveLineUpKeepsSecondaryCaretAndSelection() {
  TextViewport viewport;
  viewport.LoadContent("zero\none\ntwo\nthree\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(2, 2);
  viewport.AddSecondaryCaret(3, 1);
  Expect(microide::editor::MoveLineUp(viewport),
         "MoveLineUp with secondary caret should succeed");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "primary cursor should follow the block up by one row");
  const auto secondaries = viewport.secondary_carets();
  Expect(secondaries.size() == 1 && secondaries.front().line == 2 &&
             secondaries.front().column == 1,
         "secondary caret on a moved line should also shift up by one row");
}

void TestShapingDuplicateLine() {
  TextViewport viewport;
  viewport.LoadContent("hello\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::CopyLines(viewport, true),
         "CopyLines should produce a change");
  Expect(viewport.lines().size() >= 2, "duplicate should add a line");
  Expect(viewport.lines()[0] == "hello" && viewport.lines()[1] == "hello",
         "first two lines should both be 'hello'");
}

// Copy-lines is line-granular in both directions (VS Code copyLinesUp/Down): a
// PARTIAL selection duplicates the lines it touches, not the selected text. The
// two directions produce identical text and differ only in which copy the caret
// lands on.
void TestShapingCopyLinesDirections() {
  TextViewport viewport;
  viewport.LoadContent("one\ntwo\nthree\n", "/tmp/sample.txt");

  viewport.MoveCursorTo(1, 1);
  Expect(microide::editor::CopyLines(viewport, /*downward=*/true), "copy down should change");
  Expect(viewport.lines()[1] == "two" && viewport.lines()[2] == "two",
         "copy down should leave two adjacent copies");
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 1,
         "copy down should put the caret on the LOWER copy");

  TextViewport up;
  up.LoadContent("one\ntwo\nthree\n", "/tmp/sample.txt");
  up.MoveCursorTo(1, 1);
  Expect(microide::editor::CopyLines(up, /*downward=*/false), "copy up should change");
  Expect(up.lines()[1] == "two" && up.lines()[2] == "two",
         "copy up should produce the same two copies");
  Expect(up.cursor_line() == 1 && up.cursor_column() == 1,
         "copy up should leave the caret on the UPPER copy");

  // A selection of three characters inside one line still copies the WHOLE line.
  TextViewport partial;
  partial.LoadContent("hello world\n", "/tmp/sample.txt");
  partial.MoveCursorTo(0, 1);
  partial.MoveCursorTo(0, 4, /*extend_selection=*/true);
  Expect(microide::editor::CopyLines(partial, /*downward=*/true), "partial copy should change");
  Expect(partial.lines().size() >= 2 && partial.lines()[0] == "hello world" &&
             partial.lines()[1] == "hello world",
         "a partial selection copies its whole line, not the selected fragment");
}

// Ctrl+Enter / Ctrl+Shift+Enter open a line regardless of where in the line the
// caret sits -- that is the whole point of them over pressing Enter.
void TestShapingInsertLineBelowAndAbove() {
  TextViewport viewport;
  viewport.LoadContent("    body;\nnext;\n", "/tmp/sample.cpp");
  viewport.SetIndentWidth(2);
  viewport.SetSoftTabs(true);

  // Caret in the MIDDLE of the line: the line must not be split.
  viewport.MoveCursorTo(0, 6);
  Expect(microide::editor::InsertLineBelow(viewport), "insert-below should change");
  Expect(viewport.lines()[0] == "    body;", "the source line stays intact");
  Expect(viewport.cursor_line() == 1, "the caret moves to the new line");
  Expect(viewport.lines()[1] == "    ", "the new line carries the source line's indent");
  Expect(viewport.cursor_column() == 4, "the caret sits after that indent");

  TextViewport above;
  above.LoadContent("    body;\nnext;\n", "/tmp/sample.cpp");
  above.MoveCursorTo(0, 6);
  Expect(microide::editor::InsertLineAbove(above), "insert-above should change");
  Expect(above.lines()[1] == "    body;", "the source line is pushed down intact");
  Expect(above.cursor_line() == 0 && above.cursor_column() == 4,
         "the caret sits on the new line above, at the same indent");
  Expect(above.lines()[0] == "    ", "the new line takes the pushed-down line's indent");
}

void TestShapingIndentSelection() {
  TextViewport viewport;
  viewport.LoadContent("a\nb\n", "/tmp/sample.txt");
  viewport.SetIndentWidth(2);
  viewport.SetSoftTabs(true);
  viewport.MoveCursorTo(0, 0);
  // Build a 2-line selection by setting selection through anchor + cursor
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(1, 1, true);
  Expect(microide::editor::IndentSelection(viewport),
         "IndentSelection should change the buffer");
  Expect(viewport.lines()[0].rfind("  ", 0) == 0,
         "first line should now start with two-space indent");
  Expect(viewport.lines()[1].rfind("  ", 0) == 0,
         "second line should now start with two-space indent");
}

void TestShapingToggleLineComment() {
  TextViewport viewport;
  viewport.LoadContent("foo\nbar\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::ToggleLineComment(viewport, "//"),
         "first toggle should comment a non-blank line");
  Expect(viewport.lines()[0].rfind("// ", 0) == 0,
         "line should start with the // marker after toggle");
  // Toggle again to uncomment.
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::ToggleLineComment(viewport, "//"),
         "second toggle should uncomment");
  Expect(viewport.lines()[0] == "foo",
         "second toggle should restore the original text");
}

void TestShapingToggleBlockCommentRoundTrips() {
  TextViewport viewport;
  viewport.LoadContent("alpha\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"),
         "first toggle should wrap the line in a block comment");
  Expect(viewport.lines()[0] == "/* alpha */",
         "line should be wrapped exactly once, padded as VS Code writes it");
  // Toggling again must STRIP, not nest to `/* /*alpha*/ */`.
  viewport.MoveCursorTo(0, 0);
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"),
         "second toggle should strip the block comment");
  Expect(viewport.lines()[0] == "alpha",
         "second toggle should restore the original text (no nesting)");
}

void TestShapingToggleBlockCommentSelectionStripsWithWhitespace() {
  TextViewport viewport;
  viewport.LoadContent("  /* boxed */  \n", "/tmp/sample.cpp");
  // Select the whole line's text (including surrounding whitespace).
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(0, viewport.lines()[0].size(), true);
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"),
         "toggle should recognise an existing wrap despite surrounding whitespace");
  Expect(viewport.lines()[0] == "  boxed  ",
         "strip should keep the whitespace around the markers and drop their padding");
}

// Found by driving the real binary: a selection toggle collapsed the selection
// after wrapping, so the second toggle inserted an empty `/**/` at the caret
// instead of unwrapping. VS Code keeps the wrapped text selected and the next
// toggle finds the markers just outside the selection.
void TestShapingToggleBlockCommentSelectionIsAToggle() {
  TextViewport viewport;
  viewport.LoadContent("int x;\nint y;\n", "/tmp/sample.c");
  viewport.SelectAll();
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"), "the wrap applies");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>{"/* int x;", "int y;", " */"},
         "the selection is wrapped once, padded (got <" + viewport.lines()[0] + "|" +
             viewport.lines()[viewport.line_count() - 1] + ">)");
  const auto kept = viewport.selection_range();
  Expect(kept.has_value() && kept->start == TextPosition{0, 3} && kept->end == TextPosition{2, 0},
         "the wrapped text stays selected, markers outside");
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/"), "the second toggle applies");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>{"int x;", "int y;", ""},
         "the second toggle unwraps (got <" + viewport.lines()[0] + "|" +
             viewport.lines()[viewport.line_count() - 1] + ">)");
  const auto after = viewport.selection_range();
  Expect(after.has_value() && after->start == TextPosition{0, 0} && after->end == TextPosition{2, 0},
         "the unwrapped text stays selected");

  // A single-line selection: wrap, unwrap, and a selection over the markers.
  viewport.LoadContent("a + b\n", "/tmp/sample.c");
  viewport.MoveCursorTo(0, 2, false);
  viewport.MoveCursorTo(0, 3, true);
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/") && viewport.lines()[0] == "a /* + */ b",
         "an inner selection wraps with padding");
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/") && viewport.lines()[0] == "a + b",
         "and the next toggle unwraps it");
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(0, 5, true);
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/") && viewport.lines()[0] == "/* a + b */",
         "a whole-line selection wraps");
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(0, 11, true);
  Expect(microide::editor::ToggleBlockComment(viewport, "/*", "*/") && viewport.lines()[0] == "a + b",
         "a selection covering the markers strips them");
}

void TestShapingSortLinesAscending() {
  TextViewport viewport;
  viewport.LoadContent("c\nb\na\n", "/tmp/sample.txt");
  viewport.MoveCursorTo(0, 0, false);
  viewport.MoveCursorTo(2, 1, true);
  Expect(microide::editor::SortLines(viewport, /*ascending=*/true),
         "SortLines on a multi-line range should change buffer");
  Expect(viewport.lines()[0] == "a" && viewport.lines()[1] == "b" &&
             viewport.lines()[2] == "c",
         "sort ascending should reorder lines");
}

void TestTextViewportSaveAppliesNormalization() {
  // Setup: write a file with trailing whitespace and no final newline.
  TemporaryDirectory tmp;
  std::filesystem::path path = tmp.path() / "sample.txt";
  WriteFile(path, "foo  \nbar\t\nbaz");
  TextViewport viewport;
  Expect(viewport.OpenFile(path), "OpenFile should succeed");
  viewport.SetSaveTrimTrailingWhitespace(true);
  viewport.SetSaveEnsureFinalNewline(true);
  Expect(viewport.Save(), "Save should succeed");
  std::string after = ReadFile(path);
  Expect(after == "foo\nbar\nbaz\n",
         "saved file should be trimmed and end with a single newline");
}

void TestTextViewportSaveNormalizationPreservesCaret() {
  // Regression: a normalizing save mirrored the trimmed content via a whole-document
  // ReplaceLines, snapping the caret/selection/scroll to (0,0). VS Code preserves the
  // caret across format-on-save; the caret must stay where the user left it.
  {
    TemporaryDirectory tmp;
    std::filesystem::path path = tmp.path() / "caret.txt";
    WriteFile(path, "alpha\nbeta  \ngamma  \ndelta\nepsilon");
    TextViewport viewport;
    Expect(viewport.OpenFile(path), "OpenFile should succeed");
    viewport.SetSaveTrimTrailingWhitespace(true);
    viewport.SetSaveEnsureFinalNewline(true);
    viewport.MoveCursorTo(3, 4);  // inside "delta", a line unaffected by trimming
    Expect(viewport.Save(), "Save should succeed");
    Expect(viewport.cursor_line() == 3 && viewport.cursor_column() == 4,
           "the caret must be preserved across a normalizing save, not reset to (0,0)");
  }
  {
    // A caret sitting in now-trimmed trailing whitespace is clamped to the new end.
    TemporaryDirectory tmp;
    std::filesystem::path path = tmp.path() / "caret-trim.txt";
    WriteFile(path, "alpha\nbeta  \ngamma");
    TextViewport viewport;
    Expect(viewport.OpenFile(path), "OpenFile should succeed");
    viewport.SetSaveTrimTrailingWhitespace(true);
    viewport.MoveCursorTo(1, 6);  // end of "beta  " (col 6, inside the trailing spaces)
    Expect(viewport.Save(), "Save should succeed");
    Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 4,
           "a caret in trimmed trailing whitespace clamps to the shortened line end");
  }
}

void TestTextViewportDetectsDiskConflict() {
  using DiskConflict = TextViewport::DiskConflict;
  TemporaryDirectory tmp;
  std::filesystem::path path = tmp.path() / "conflict.txt";
  WriteFile(path, "one\ntwo\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "OpenFile should succeed");
  Expect(viewport.DetectDiskConflict() == DiskConflict::None,
         "a freshly opened file should report no conflict");

  // External rewrite changes size, so mtime+size diverges from the load record.
  WriteFile(path, "one\ntwo\nthree\n");
  Expect(viewport.DetectDiskConflict() == DiskConflict::Changed,
         "an external rewrite should be detected as a disk conflict");

  // Saving recaptures the signature, clearing the conflict.
  Expect(viewport.Save(), "Save should succeed");
  Expect(viewport.DetectDiskConflict() == DiskConflict::None,
         "saving should re-baseline the on-disk signature");

  // Deleting the file is a Vanished conflict.
  std::filesystem::remove(path);
  Expect(viewport.DetectDiskConflict() == DiskConflict::Vanished,
         "a deleted file should be detected as vanished");
}

void TestTextViewportUntitledHasNoDiskConflict() {
  TextViewport viewport;
  viewport.SetUntitledBuffer();
  viewport.InsertText("hello");
  Expect(viewport.DetectDiskConflict() == TextViewport::DiskConflict::None,
         "an untitled buffer should never report a disk conflict");
}

void TestSaveNormalizationTrim() {
  std::vector<std::string> lines = {"foo  ", "bar\t", "baz"};
  Expect(microide::editor::TrimTrailingWhitespace(lines),
         "trim should report change");
  Expect(lines[0] == "foo" && lines[1] == "bar" && lines[2] == "baz",
         "all trailing whitespace removed");
}

void TestSaveNormalizationEnsureFinalNewline() {
  std::vector<std::string> lines = {"foo"};
  Expect(microide::editor::EnsureSingleFinalNewline(lines),
         "single content line without final newline should be normalized");
  Expect(lines.size() == 2 && lines[1].empty(),
         "buffer should end with one empty trailing line");

  // Multiple trailing newlines collapse to one.
  lines = {"foo", "", "", ""};
  Expect(microide::editor::EnsureSingleFinalNewline(lines),
         "multiple trailing newlines should be collapsed");
  Expect(lines.size() == 2 && lines[0] == "foo" && lines[1].empty(),
         "trailing newlines should reduce to a single one");

  // Already correct: no change.
  lines = {"foo", ""};
  Expect(!microide::editor::EnsureSingleFinalNewline(lines),
         "already-normalized buffer should be a no-op");
}

void TestIndentDetectSpacesMajority() {
  std::vector<std::string> lines = {
      "x = 1",
      "  if x:",
      "    y = 2",
      "    z = 3",
      "  else:",
      "    return",
  };
  auto detected = microide::editor::DetectIndent(lines);
  Expect(detected.detected(), "indent should be detected");
  Expect(detected.soft_tabs == true, "majority spaces should yield soft_tabs=true");
  Expect(detected.indent_width == 2,
         "indent step should be detected as 2");
  Expect(detected.non_blank_lines_inspected == 6, "short buffers inspect every non-blank line");
}

void TestIndentDetectTabsMajority() {
  std::vector<std::string> lines = {"\tfoo", "\tbar", "\tbaz"};
  auto detected = microide::editor::DetectIndent(lines);
  Expect(detected.detected(), "tabs should be detected");
  Expect(detected.soft_tabs == false, "majority tabs should yield soft_tabs=false");
  Expect(!detected.indent_width.has_value(),
         "a tab-indented buffer cannot say how wide a tab is: the width stays the caller's");
  Expect(detected.non_blank_lines_inspected == 3, "tab-majority sample uses three non-blank lines");
}

// Regression: a leading whitespace run that has spaces before a tab (`"  \tcode"`,
// a common Makefile/Python mixed style) is tab-indented in effect. Detection must
// classify it by the presence of a tab, not by the first byte only — otherwise a
// tab-indented file whose lines start with alignment spaces is misdetected as
// space-indented.
void TestIndentDetectMixedLeadingTabsCountAsTabs() {
  std::vector<std::string> lines = {
      "def f():",
      "  \tfoo",   // spaces then a tab
      "  \tbar",
      "\tbaz",
  };
  auto detected = microide::editor::DetectIndent(lines);
  Expect(detected.detected(), "mixed leading tabs should still detect indentation");
  Expect(detected.soft_tabs == false,
         "leading whitespace containing a tab must count as tab indentation, not spaces");
}

// The detector used to count only indentation increases, so a 2-space file whose
// wrapped calls continue at +4 (clang-format's default) was read as 4-space: 141
// of this repository's own 1068 C++ files came back as 4-space, `Theme.cpp` with
// twice as many +4 continuations as +2 block steps among them. VS Code counts the
// step in both directions and lets 2 win over 4 when 2-steps are at least half
// as common, which is what a 2-space file with continuations looks like.
void TestIndentDetectTwoSpaceFileWithFourSpaceContinuations() {
  std::vector<std::string> lines;
  for (int i = 0; i < 10; ++i) {
    lines.push_back("void f() {");
    lines.push_back("  call(alpha,");
    lines.push_back("      beta);");  // aligned under `alpha`: no evidence
    lines.push_back("  other_call(");
    lines.push_back("      gamma, delta);");  // +4 continuation
    lines.push_back("  if (x) {");
    lines.push_back("    y();");
    lines.push_back("  }");
    lines.push_back("}");
  }
  auto detected = microide::editor::DetectIndent(lines);
  Expect(detected.soft_tabs == true, "the file is space-indented");
  Expect(detected.indent_width == 2, "continuation lines must not turn a 2-space file into 4");

  // A genuine 4-space file with dedents still reads as 4.
  std::vector<std::string> four = {"a", "    b", "        c", "    d", "a", "    e"};
  Expect(microide::editor::DetectIndent(four).indent_width == 4, "a 4-space file reads as 4");
  // Dedents are evidence too: a file that opens mid-block and only ever dedents.
  std::vector<std::string> dedent_only = {"      a", "    b", "  c", "d"};
  Expect(microide::editor::DetectIndent(dedent_only).indent_width == 2,
         "a step is a step in either direction");
}

// Tabs and spaces tie (or nothing is indented): the style is unknown and stays
// whatever the preferences chose, while a space step can still be reported.
void TestIndentDetectTieLeavesStyleUndetermined() {
  std::vector<std::string> tie = {"a", "\tb", "a", "  c"};
  auto detected = microide::editor::DetectIndent(tie);
  Expect(!detected.soft_tabs.has_value(), "a tab/space tie must not pick a style");
  std::vector<std::string> flat = {"a", "b", "c"};
  Expect(!microide::editor::DetectIndent(flat).detected(), "an unindented buffer detects nothing");
}

void TestIndentDetectMaxInspectBoundsNonBlankCount() {
  std::vector<std::string> lines;
  lines.reserve(400);
  for (int i = 0; i < 400; ++i) {
    lines.push_back(std::string("  line ") + std::to_string(i));
  }
  auto detected = microide::editor::DetectIndent(lines, /*max_inspect_lines=*/40);
  Expect(detected.non_blank_lines_inspected == 40,
         "detection should stop after the non-blank line budget");
}

void TestApplyDetectedIndentOnOpenUpdatesViewport() {
  editor::TextViewport viewport;
  viewport.LoadContent(
      "root\n"
      "    child\n"
      "        grand\n",
      "/tmp/sample.cpp");
  viewport.SetSoftTabs(false);
  viewport.SetIndentWidth(8);
  viewport.SetTabSize(8);

  const auto get_on = [](std::string_view id) -> std::optional<std::string> {
    if (id == "editor.indent.detect_on_open") {
      return std::string{"true"};
    }
    return std::nullopt;
  };
  microide::workspace::ApplyDetectedIndentAfterPreferences(viewport, get_on);
  Expect(viewport.soft_tabs(), "four-space sample should switch to soft tabs");
  Expect(viewport.indent_width() == 4 && viewport.tab_size() == 4,
         "indent and tab size should follow detection");
}

void TestApplyDetectedIndentOnOpenSkippedWhenDisabled() {
  editor::TextViewport viewport;
  viewport.LoadContent("\tone\n\t\ttwo\n", "/tmp/sample.cpp");
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(3);
  viewport.SetTabSize(3);

  const auto get_off = [](std::string_view id) -> std::optional<std::string> {
    if (id == "editor.indent.detect_on_open") {
      return std::string{"false"};
    }
    return std::nullopt;
  };
  microide::workspace::ApplyDetectedIndentAfterPreferences(viewport, get_off);
  Expect(viewport.soft_tabs() && viewport.indent_width() == 3,
         "disabled detection should leave prior viewport settings");
}

void TestApplyDetectedIndentOnOpenSkippedWhenPathEmpty() {
  editor::TextViewport viewport;
  viewport.LoadContent("    lone\n");
  viewport.SetIndentWidth(2);
  const auto get_on = [](std::string_view) -> std::optional<std::string> {
    return std::string{"true"};
  };
  microide::workspace::ApplyDetectedIndentAfterPreferences(viewport, get_on);
  Expect(viewport.indent_width() == 2, "untitled buffer should skip detection");
}

void TestLanguageContractDefaults() {
  microide::workspace::WorkspaceLanguageContract registry;
  const auto* cpp = registry.Find("cpp");
  Expect(cpp != nullptr, "cpp contract should exist by default");
  Expect(cpp->line_comment == "//",
         "cpp line comment should be '//'");
  Expect(!cpp->bracket_pairs.empty(),
         "cpp bracket pairs should be populated");

  // Markdown is prose: parens/brackets routinely span lines, so bracket-based
  // folding would emit bogus fold markers on ordinary paragraphs. The contract
  // intentionally ships no fold-driving bracket pairs, but still auto-closes.
  const auto* md = registry.Find("markdown");
  Expect(md != nullptr, "markdown contract should exist by default");
  Expect(md->bracket_pairs.empty(),
         "markdown should not fold on brackets (prose parens span lines)");
  Expect(!md->auto_close_pairs.empty(),
         "markdown should still auto-close brackets despite no fold pairs");

  const auto* py = registry.Find("python");
  Expect(py != nullptr, "python contract should exist by default");
  Expect(py->line_comment == "#",
         "python line comment should be '#'");
  Expect(py->indent_after_open_patterns.size() > 0,
         "python should have indent-after-open patterns including ':'");
}

void TestLanguageContractMissingLanguage() {
  microide::workspace::WorkspaceLanguageContract registry;
  Expect(registry.Find("totally-fictional") == nullptr,
         "unknown language id should resolve to nullptr");
  auto view = registry.ResolveView("totally-fictional");
  Expect(view.contract == nullptr,
         "unknown language id view should have null contract");
}

void TestLanguageContractAliasesCppFiletypeName() {
  // RuntimeSyntaxRegistry::DetectFiletype returns "c++" for .cpp / .hpp /
  // .cxx files; the language-contract defaults are keyed under "cpp".
  // Without the alias, auto-close / surround / smart-indent silently
  // no-op on every C++ buffer.
  microide::workspace::WorkspaceLanguageContract registry;
  const auto* cpp_contract = registry.Find("c++");
  Expect(cpp_contract != nullptr,
         "'c++' (the runtime-syntax detected name) should resolve to the C-style contract");
  Expect(!cpp_contract->auto_close_pairs.empty(),
         "aliased c++ contract should carry C-style auto_close_pairs");
  const auto* canonical = registry.Find("cpp");
  Expect(canonical != nullptr && canonical->language_id == cpp_contract->language_id,
         "c++ should alias to the same contract instance as cpp");
}

void TestIndentGuidesEmitsRunsAtNestedDepths() {
  using microide::editor::ComputeIndentGuides;
  using microide::editor::IndentGuideRun;

  std::vector<std::string> lines = {
      "",                  // 0
      "if (x) {",          // 1
      "    a();",          // 2 — depth 1
      "    if (y) {",      // 3 — depth 1
      "        b();",      // 4 — depth 2
      "    }",             // 5 — depth 1
      "}",                 // 6
  };
  std::vector<std::size_t> visible_rows{0, 1, 2, 3, 4, 5, 6};
  std::vector<IndentGuideRun> runs;
  ComputeIndentGuides(lines, visible_rows, /*tab_size=*/4, /*indent_width=*/4,
                      /*caret_line=*/SIZE_MAX, /*caret_leading=*/0, &runs);

  // 2026-05-15 perf deep-dive round 2 Finding 14: guides are now stored as
  // multi-row runs `[start_row, end_row]` to coalesce the per-row segments
  // the renderer used to walk. A guide "covers" a row when start_row<=row<=end_row.
  const auto covers = [&](std::size_t column, std::size_t row) {
    return std::any_of(runs.begin(), runs.end(), [&](const IndentGuideRun& r) {
      return r.column == column && r.start_row <= row && row <= r.end_row;
    });
  };
  Expect(covers(4, 2), "depth-1 line should be covered by a guide at column 4");
  Expect(covers(4, 4) && covers(8, 4),
         "depth-2 line should be covered by guides at columns 4 and 8");
  Expect(!covers(4, 6), "closing brace at column 0 should not be covered by a column-4 guide");
  Expect(!covers(8, 2), "depth-1 line should not be covered by a column-8 guide");

  // Coalescing: the column-4 guide should span the contiguous run rows 2..5 as
  // a single segment instead of four per-row entries.
  const auto col4_run = std::find_if(runs.begin(), runs.end(), [](const IndentGuideRun& r) {
    return r.column == 4 && r.start_row == 2;
  });
  Expect(col4_run != runs.end(), "column-4 guide should start at row 2");
  Expect(col4_run->end_row == 5,
         "column-4 guide should coalesce rows 2..5 into one run (depth-1 contiguous block)");
}

// ComputeIndentGuides emits vertical runs by sweeping one guide column at a time.
// It used to emit one entry per (row, column) and std::sort the lot so a
// coalescing pass could merge neighbours -- thousands of entries per frame on
// deeply indented content, and 92% of the step's cost.
//
// The runs are a compressed encoding of a coverage set, so check the encoding
// against the set: every (column, row) a naive per-row computation would mark
// must be covered by exactly one run, and no run may cover anything else. Uses
// tabs, mixed depths, blank lines, dedents and an out-of-range row, since those
// are where a sweep and a sort can disagree.
// TD-2026-08-13-206 filed compare indent guides as blocked on the row->line
// mapping: a diff's visible rows are not contiguous line indices, because the
// blank padding rows of an added/deleted hunk have no line at all. They do not
// have to be — the compare renderer hands the visible window over as its OWN
// document, one view per visible row, empty for a padding row. This pins the
// property that makes that legal: an empty row measures no indent, so it breaks
// the run rather than continuing it through the filler.
void TestIndentGuidesBreakAcrossABlankDiffFillerRow() {
  using microide::editor::ComputeIndentGuides;
  using microide::editor::IndentGuideRun;

  // Visible window of a diff pane: two indented rows, a blank filler row where
  // the other side had an inserted line, then two more indented rows.
  const std::vector<std::string_view> window = {
      "        deep one", "        deep two", "", "        deep three", "        deep four",
  };
  std::vector<std::size_t> rows(window.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    rows[i] = i;
  }

  std::vector<IndentGuideRun> runs;
  ComputeIndentGuides(microide::editor::LineSpan(window), rows, /*tab_size=*/4,
                      /*indent_width=*/4, /*caret_line=*/SIZE_MAX, 0, &runs);

  Expect(!runs.empty(), "an indented window emits guides");
  for (const IndentGuideRun& run : runs) {
    Expect(!(run.start_row <= 2 && run.end_row >= 2),
           "no guide run spans the blank filler row");
  }
  bool covers_before = false;
  bool covers_after = false;
  for (const IndentGuideRun& run : runs) {
    covers_before = covers_before || (run.start_row <= 1 && run.end_row >= 1);
    covers_after = covers_after || (run.start_row <= 3 && run.end_row >= 3);
  }
  Expect(covers_before && covers_after,
         "the indented rows on either side of the filler still get their guides");
}

void TestIndentGuidesRunsMatchNaiveCoverage() {
  using microide::editor::ComputeIndentGuides;
  using microide::editor::IndentGuideRun;
  using microide::editor::LeadingVisualIndent;

  const std::vector<std::string> lines = {
      "top();",              // 0: no indent
      "    a();",            // 1: depth 1
      "        b();",        // 2: depth 2
      "",                    // 3: blank
      "            c();",    // 4: depth 3
      "        d();",        // 5: back to depth 2
      "\te();",              // 6: a tab, which is a full stop
      "\t\tf();",            // 7: two tabs
      "   odd();",           // 8: indent that is not a multiple of the width
      "}",                   // 9
  };
  constexpr std::size_t kTabSize = 4;
  constexpr std::size_t kIndentWidth = 4;

  for (std::size_t caret_line : {std::size_t{2}, std::size_t{7}, SIZE_MAX}) {
    // Include a row index past the end of the buffer: it must contribute nothing.
    std::vector<std::size_t> visible_rows;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      visible_rows.push_back(i);
    }
    visible_rows.push_back(lines.size() + 3);

    const std::size_t caret_lead =
        caret_line < lines.size() ? LeadingVisualIndent(lines[caret_line], kTabSize) : 0;
    std::vector<IndentGuideRun> runs;
    ComputeIndentGuides(lines, visible_rows, kTabSize, kIndentWidth, caret_line, caret_lead,
                        &runs);

    // Naive coverage: a guide sits at every multiple of the indent width up to
    // and including the row's leading visual indent.
    std::set<std::pair<std::size_t, std::size_t>> expected;  // (column, row)
    for (std::size_t row = 0; row < visible_rows.size(); ++row) {
      const std::size_t line_index = visible_rows[row];
      if (line_index >= lines.size()) {
        continue;
      }
      const std::size_t leading = LeadingVisualIndent(lines[line_index], kTabSize);
      for (std::size_t column = kIndentWidth; column <= leading; column += kIndentWidth) {
        expected.insert({column, row});
      }
    }

    std::set<std::pair<std::size_t, std::size_t>> covered;
    for (const IndentGuideRun& run : runs) {
      Expect(run.start_row <= run.end_row, "a run must not be empty");
      for (std::size_t row = run.start_row; row <= run.end_row; ++row) {
        const bool fresh = covered.insert({run.column, row}).second;
        Expect(fresh, "runs must not overlap — each (column, row) belongs to exactly one run");
      }
    }
    Expect(covered == expected,
           "the emitted runs must cover exactly the (column, row) pairs a naive per-row "
           "computation marks");
  }
}

void TestIndentGuidesMarksActiveAtParentColumn() {
  using microide::editor::ComputeIndentGuides;
  using microide::editor::IndentGuideRun;

  std::vector<std::string> lines = {
      "if (x) {",          // 0
      "    if (y) {",      // 1
      "        b();",      // 2 — caret here, depth 2 → parent at column 4
      "    }",             // 3
      "}",                 // 4
  };
  std::vector<std::size_t> visible_rows{0, 1, 2, 3, 4};
  std::vector<IndentGuideRun> runs;
  ComputeIndentGuides(lines, visible_rows, /*tab_size=*/4, /*indent_width=*/4,
                      /*caret_line=*/2, /*caret_leading=*/8, &runs);

  const auto active_for = [&](std::size_t column, std::size_t row) {
    const auto it = std::find_if(runs.begin(), runs.end(), [&](const IndentGuideRun& r) {
      return r.column == column && r.start_row == row;
    });
    return it != runs.end() && it->active;
  };
  Expect(active_for(4, 2),
         "the caret's parent indent column should be flagged active on the caret's line");
  Expect(!active_for(8, 2),
         "the caret's own indent column should not be marked active on the caret's line");
}

void TestIndentGuidesFoldModelEmphasisOnInnerCloser() {
  using microide::editor::ComputeIndentGuides;
  using microide::editor::FoldingModel;
  using microide::editor::IndentGuideRun;

  std::vector<std::string> lines = {
      "if (x) {",        // 0
      "    if (y) {",    // 1 — four leading spaces
      "        b();",    // 2
      "    }",           // 3 — caret on inner closer, same leading as inner opener
      "}",
  };
  FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = false;
  options.tab_size = 4;
  FoldingModel model;
  Expect(model.Compute(lines, options), "fold model should compute bracket ranges");

  std::vector<std::size_t> visible_rows{0, 1, 2, 3, 4};
  std::vector<IndentGuideRun> with_fold;
  std::vector<IndentGuideRun> without_fold;
  const std::size_t caret_line = 3;
  const std::size_t caret_lead = microide::editor::LeadingVisualIndent(lines[caret_line], 4);
  ComputeIndentGuides(lines, visible_rows, 4, 4, caret_line, caret_lead, &with_fold, &model);
  ComputeIndentGuides(lines, visible_rows, 4, 4, caret_line, caret_lead, &without_fold,
                        nullptr);

  const auto active_columns_on_caret_line = [&](const std::vector<IndentGuideRun>& runs) {
    std::vector<std::size_t> cols;
    for (const auto& r : runs) {
      if (r.active) {
        cols.push_back(r.column);
      }
    }
    return cols;
  };
  const auto fold_cols = active_columns_on_caret_line(with_fold);
  Expect(!fold_cols.empty() && fold_cols[0] == 4,
         "inner fold opener indent should drive active emphasis on the inner closer line");
  const auto scan_cols = active_columns_on_caret_line(without_fold);
  bool scan_has_four = false;
  for (std::size_t c : scan_cols) {
    if (c == 4) scan_has_four = true;
  }
  Expect(!scan_has_four,
         "leading-indent-only scan should not emphasize column 4 on the inner closer line");
}

void TestRenderViewModelBuilderWhitespaceGlyphRunsToggle() {
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  microide::editor::TextViewport viewport;
  viewport.LoadContent("a b\tc\n", "/tmp/ws-glyphs.txt");
  viewport.SetTabSize(4);
  viewport.SetScrollLine(0);
  viewport.SetViewportSize(5, 80);
  const auto off =
      builder.BuildEditorViewModel(viewport, 5, nullptr, false, false, false, 3, false);
  Expect(off.whitespace_glyph_runs.empty(), "runs should stay empty when render-whitespace is off");
  const auto on =
      builder.BuildEditorViewModel(viewport, 5, nullptr, false, false, false, 3, true);
  Expect(on.whitespace_glyph_runs.size() >= 2, "viewport should expose at least two glyph runs");
  bool saw_dot = false;
  bool saw_tab = false;
  for (const auto& g : on.whitespace_glyph_runs) {
    if (!g.is_tab_rule) {
      saw_dot = true;
    } else {
      saw_tab = true;
    }
  }
  Expect(saw_dot && saw_tab, "visible whitespace should surface both dots and tab rules");
}

// TD-2026-08-14-210: after the walk was shared, TWO implementations of
// render-whitespace remain — the walk (editor fallback + both compare panes) and
// the view model's precomputed glyph-run table, which is the editor's fast path.
// `dev-docs/project/validation-traps.md` names the trap they form: a parity test
// only holds on fixtures small enough to hide the difference. So this compares
// them on the cases where they could actually diverge — a tab that expands across
// cells, a multi-byte glyph before whitespace, and a window that starts mid-line.
void TestWhitespaceGlyphRunsAgreeWithTheSharedWalk() {
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  microide::editor::TextViewport viewport;
  // "é" before a space (byte offset != visual column), a tab at a column that
  // expands across several cells, and trailing spaces.
  const std::string line = "a\xc3\xa9 b\tc  d";
  viewport.LoadContent(line + "\n", "/tmp/ws-parity.txt");
  viewport.SetTabSize(4);
  viewport.SetScrollLine(0);
  viewport.SetViewportSize(5, 80);

  const auto vm = builder.BuildEditorViewModel(viewport, 5, nullptr, false, false, false, 3, true);

  constexpr float kCharWidth = 10.0f;
  constexpr float kTextX = 100.0f;
  constexpr float kY = 50.0f;
  constexpr float kLineHeight = 16.0f;
  const SDL_Color color{1, 2, 3, 255};

  // What the run table would paint, in the same order the row loop emits it.
  std::vector<microide::editor::DecoratedTextFill> from_runs;
  for (const auto& glyph : vm.whitespace_glyph_runs) {
    microide::editor::PushWhitespaceMarker(
        from_runs, glyph.is_tab_rule,
        kTextX + static_cast<float>(glyph.cell_visual_start) * kCharWidth, kCharWidth,
        static_cast<float>(glyph.cell_visual_extent) * kCharWidth, kY, kLineHeight, color);
  }

  std::vector<microide::editor::DecoratedTextFill> from_walk;
  microide::editor::AppendWhitespaceMarkers(from_walk, line, /*tab_size=*/4,
                                            /*row_visual_start=*/0, /*row_visual_end=*/80, kTextX,
                                            kCharWidth, kY, kLineHeight, color);

  Expect(!from_walk.empty(), "the fixture actually contains whitespace to mark");
  Expect(from_runs.size() == from_walk.size(),
         "the run table and the walk mark the same number of cells");
  for (std::size_t i = 0; i < from_runs.size() && i < from_walk.size(); ++i) {
    Expect(from_runs[i].rect.x == from_walk[i].rect.x &&
               from_runs[i].rect.w == from_walk[i].rect.w &&
               from_runs[i].rect.h == from_walk[i].rect.h,
           "the run table and the walk place the same marker rect");
  }
}

void TestLanguageContractAppliesUserOverrides() {
  microide::workspace::WorkspaceLanguageContract registry;
  microide::plugin::PluginHost host;
  const auto getter = [](std::string_view id) -> std::optional<std::string> {
    if (id == "editor.brackets.user_pairs") return std::string("<|>");
    if (id == "editor.brackets.user_disabled") return std::string("[|]");
    if (id == "editor.comments.user_line") return std::string(";;");
    if (id == "editor.indents.user_open_patterns") return std::string("=>,then");
    return std::nullopt;
  };
  registry.Refresh(host, getter);

  const auto* cpp = registry.Find("cpp");
  Expect(cpp != nullptr, "cpp contract should still exist after override-driven refresh");

  const bool added_pair = std::any_of(
      cpp->bracket_pairs.begin(), cpp->bracket_pairs.end(),
      [](const auto& p) { return p.open == "<" && p.close == ">"; });
  Expect(added_pair, "user_pairs override should append <|> to the bracket set");

  const bool removed_pair = std::any_of(
      cpp->bracket_pairs.begin(), cpp->bracket_pairs.end(),
      [](const auto& p) { return p.open == "[" && p.close == "]"; });
  Expect(!removed_pair, "user_disabled override should remove [|] from the bracket set");

  Expect(cpp->line_comment == ";;",
         "user_line override should replace the line-comment marker globally");

  const bool added_pattern = std::find(cpp->indent_after_open_patterns.begin(),
                                       cpp->indent_after_open_patterns.end(),
                                       "=>") != cpp->indent_after_open_patterns.end();
  Expect(added_pattern, "user_open_patterns override should add custom indent suffixes");
}

// A Refresh with no plugin contribution and no user override reproduces the
// defaults exactly, so it must neither rebuild the table (~5,000 allocations)
// nor bump the revision — a bump drops the shared editor-view cache and
// re-applies preferences to every open tab. Every project switch runs this path
// (TD-2026-08-06-159).
void TestLanguageContractSkipsNoOpRefresh() {
  microide::workspace::WorkspaceLanguageContract registry;
  microide::plugin::PluginHost host;

  const std::size_t initial_revision = registry.revision();
  const auto* cpp_before = registry.Find("cpp");
  Expect(cpp_before != nullptr, "cpp contract should exist by default");

  registry.Refresh(host);
  Expect(registry.revision() == initial_revision,
         "a refresh that changes nothing should not bump the contract revision");
  Expect(registry.Find("cpp") == cpp_before,
         "a no-op refresh should leave the existing contract storage in place");

  // A user override still takes effect on the very next refresh...
  const auto getter = [](std::string_view id) -> std::optional<std::string> {
    if (id == "editor.comments.user_line") return std::string(";;");
    return std::nullopt;
  };
  registry.Refresh(host, getter);
  Expect(registry.revision() > initial_revision,
         "a refresh that layers an override on top must bump the revision");
  const auto* cpp_overridden = registry.Find("cpp");
  Expect(cpp_overridden != nullptr && cpp_overridden->line_comment == ";;",
         "the override should be applied after the skip-eligible refresh");

  // ...and dropping it must rebuild rather than leave the override standing.
  const std::size_t overridden_revision = registry.revision();
  registry.Refresh(host);
  Expect(registry.revision() > overridden_revision,
         "dropping the override must rebuild the table, not skip");
  const auto* cpp_restored = registry.Find("cpp");
  Expect(cpp_restored != nullptr && cpp_restored->line_comment == "//",
         "the defaults should be restored once the override is gone");
}

microide::editor::LanguageContractView MakeCStyleContractView() {
  microide::editor::LanguageContractView view;
  view.auto_close_pairs = {
      {"(", ")"},
      {"[", "]"},
      {"{", "}"},
      {"\"", "\""},
      {"'", "'"},
  };
  view.surround_pairs = view.auto_close_pairs;
  view.indent_after_open_patterns = {"{", "(", "[", ":"};
  view.dedent_on_close_chars = {"}", ")", "]"};
  view.line_comment = "//";
  view.auto_close_enabled = true;
  view.surround_enabled = true;
  view.smart_indent_enabled = true;
  return view;
}

void TestAutoClosePairInsertsClose() {
  TextViewport viewport;
  viewport.LoadContent("\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "()",
         "typing '(' should auto-insert ')'");
  Expect(viewport.cursor_column() == 1,
         "caret should sit between open and close");
}

void TestAutoCloseSkipOverClose() {
  TextViewport viewport;
  viewport.LoadContent("\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.InsertCharacter('(');
  viewport.InsertCharacter(')');
  Expect(viewport.lines()[0] == "()",
         "typing the matching close should advance over the existing close");
  Expect(viewport.cursor_column() == 2,
         "caret should be after the close character");
}

void TestAutoCloseDisabledByDefault() {
  TextViewport viewport;
  viewport.LoadContent("\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "(",
         "without contract view, auto-close must not fire");
  Expect(viewport.cursor_column() == 1,
         "caret should advance past the inserted '('");
}

void TestAutoCloseViaInsertTextSingleCharacter() {
  TextViewport viewport;
  viewport.LoadContent("\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.InsertText("(");
  Expect(viewport.lines()[0] == "()",
         "single-character InsertText should use the same pair path as InsertCharacter");
  Expect(viewport.cursor_column() == 1,
         "InsertText pair path should leave caret between open and close");
}

void TestSurroundSelection() {
  TextViewport viewport;
  viewport.LoadContent("hello\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);
  viewport.InsertCharacter('"');
  Expect(viewport.lines()[0] == "\"hello\"",
         "surround should wrap the selection with the matching pair");
  Expect(viewport.has_selection(),
         "surround should preserve the inner selection");
  Expect(viewport.cursor_column() == 6,
         "caret should sit at the end of the inner text");
}

void TestSurroundMultiLineSelection() {
  TextViewport viewport;
  viewport.LoadContent("aa\nbb\ncc\n", "/tmp/multi-line-surround.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(1, 2, /*extend_selection=*/true);
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "(aa",
         "multi-line surround should insert the opener on the first selected line");
  Expect(viewport.lines()[1] == "bb)",
         "multi-line surround should insert the closer on the last selected line");
  Expect(viewport.lines()[2] == "cc",
         "multi-line surround should leave lines outside the selection unchanged");
  Expect(viewport.has_selection(),
         "multi-line surround should preserve the inner selection");
}

void TestMultiCaretSurroundMultiLineSelections() {
  TextViewport viewport;
  viewport.LoadContent("aa\nbb\ncc\ndd\n", "/tmp/multi-caret-multi-line-surround.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(1, 2, /*extend_selection=*/true);
  viewport.AddSecondaryCaretWithRange(
      microide::editor::SelectionRange{{2, 0}, {3, 2}});
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "(aa" && viewport.lines()[1] == "bb)" &&
             viewport.lines()[2] == "(cc" && viewport.lines()[3] == "dd)",
         "multi-caret surround should wrap every selected range, including multi-line spans");
  Expect(viewport.has_multiple_carets(),
         "multi-caret multi-line surround should keep the caret set active");
  Expect(viewport.has_selection(),
         "multi-caret multi-line surround should preserve the primary inner selection");
  Expect(viewport.Undo(),
         "multi-caret multi-line surround should record one undo step");
  Expect(viewport.lines()[0] == "aa" && viewport.lines()[1] == "bb" &&
             viewport.lines()[2] == "cc" && viewport.lines()[3] == "dd",
         "undo should restore every multi-line surround atomically");
}

// The multi-caret aggregate undo entry spans first..last affected caret line, so
// with far-apart carets it captures the whole gap between them. That capture moved
// from a per-line `lines[i]` walk to one SliceLines call, and the slices now move
// into the entry instead of being copied into it — the untouched interior lines
// must survive the edit, the undo and the redo unchanged.
void TestMultiCaretSurroundAcrossADistantGap() {
  std::string content;
  for (int i = 0; i < 400; ++i) {
    content += "line" + std::to_string(i) + "\n";
  }
  TextViewport viewport;
  viewport.LoadContent(content, "/tmp/multi-caret-distant-surround.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(10, 0);
  viewport.MoveCursorTo(10, 6, /*extend_selection=*/true);
  viewport.AddSecondaryCaretWithRange(microide::editor::SelectionRange{{390, 0}, {390, 7}});
  viewport.InsertCharacter('(');

  Expect(viewport.lines().size() == 401, "the surround must not change the line count");
  Expect(viewport.lines()[10] == "(line10)" && viewport.lines()[390] == "(line390)",
         "both distant carets should wrap their selection");
  const auto interior_intact = [&]() {
    for (std::size_t i = 11; i < 390; ++i) {
      if (viewport.lines()[i] != "line" + std::to_string(i)) {
        return false;
      }
    }
    return true;
  };
  Expect(interior_intact(), "the lines between the carets must be untouched by the edit");

  Expect(viewport.Undo(), "the distant multi-caret surround should be one undo step");
  Expect(viewport.lines()[10] == "line10" && viewport.lines()[390] == "line390",
         "undo should restore both wrapped lines");
  Expect(interior_intact(), "undo must not disturb the lines between the carets");

  Expect(viewport.Redo(), "the distant multi-caret surround should redo");
  Expect(viewport.lines()[10] == "(line10)" && viewport.lines()[390] == "(line390)",
         "redo should re-apply both wrapped lines");
  Expect(interior_intact(), "redo must not disturb the lines between the carets");
}

// TD-2026-07-17A-021: surround wraps by touching only the boundary lines (no
// whole-selection materialization). Verify the inner selection endpoints and a
// single atomic undo for a multi-line selection that ends mid-line.
void TestSurroundBoundaryWrapInnerSelectionAndUndo() {
  {
    // Multi-line selection ending mid-line: opener on the first line, closer at
    // the mid-line end column, middle untouched, inner selection preserved.
    TextViewport viewport;
    viewport.LoadContent("aaaa\nbbbb\ncccc\n", "/tmp/surround-boundary.cpp");
    viewport.SetLanguageContractView(MakeCStyleContractView());
    viewport.MoveCursorTo(0, 1);
    viewport.MoveCursorTo(2, 2, /*extend_selection=*/true);
    viewport.InsertCharacter('(');
    Expect(viewport.lines()[0] == "a(aaa", "opener inserted at the selection start column");
    Expect(viewport.lines()[1] == "bbbb", "an interior line stays untouched");
    Expect(viewport.lines()[2] == "cc)cc", "closer inserted at the selection end column");
    Expect(viewport.has_selection(), "the inner selection is preserved");
    Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 2,
           "the caret sits at the end of the inner content on the last line");

    Expect(viewport.Undo(), "surround records exactly one undo step");
    Expect(viewport.lines()[0] == "aaaa" && viewport.lines()[1] == "bbbb" &&
               viewport.lines()[2] == "cccc",
           "a single undo restores the whole selection exactly");
    Expect(viewport.Redo() && viewport.lines()[0] == "a(aaa" && viewport.lines()[2] == "cc)cc",
           "redo reapplies the boundary wrap");
  }
  {
    // Single-line selection: both delimiters land on the same line and the caret
    // ends past the inner text.
    TextViewport viewport;
    viewport.LoadContent("xhellox\n", "/tmp/surround-single.cpp");
    viewport.SetLanguageContractView(MakeCStyleContractView());
    viewport.MoveCursorTo(0, 1);
    viewport.MoveCursorTo(0, 6, /*extend_selection=*/true);
    viewport.InsertCharacter('"');
    Expect(viewport.lines()[0] == "x\"hello\"x",
           "single-line surround wraps only the selected span");
    Expect(viewport.cursor_column() == 7, "caret sits just past the inner content");
    Expect(viewport.Undo() && viewport.lines()[0] == "xhellox",
           "single-line surround undoes atomically");
  }
}

void TestSurroundDisabledFallsBackToLiteralInsert() {
  TextViewport viewport;
  auto view = MakeCStyleContractView();
  view.surround_enabled = false;
  viewport.SetLanguageContractView(std::move(view));
  viewport.LoadContent("hello\n", "/tmp/sample.cpp");
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, /*extend_selection=*/true);
  viewport.InsertCharacter('"');
  Expect(viewport.lines()[0] == "\"",
         "with surround disabled, typing quote over selection should do literal replacement");
}

void TestAutoCloseInhibitedInsideString() {
  TextViewport viewport;
  viewport.LoadContent("\"abc\"\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 2);  // inside string literal
  viewport.InsertCharacter('"');
  Expect(viewport.lines()[0] == "\"a\"bc\"",
         "auto-close should be inhibited inside string scopes");
}

void TestAutoCloseInhibitedInsideComment() {
  TextViewport viewport;
  viewport.LoadContent("// comment\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "// comment(",
         "auto-close should be inhibited inside comment scopes");
}

void TestAutoCloseMultiCaretInsertsPairs() {
  TextViewport viewport;
  viewport.LoadContent("\n\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 0);
  viewport.AddSecondaryCaret(1, 0);
  viewport.InsertCharacter('(');
  Expect(viewport.lines()[0] == "()" && viewport.lines()[1] == "()",
         "multi-caret pair insert should apply to every caret");
  const auto carets = viewport.secondary_carets();
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 1,
         "primary caret should land between inserted pair");
  Expect(carets.size() == 1 && carets[0].line == 1 && carets[0].column == 1,
         "secondary caret should also land between inserted pair");
}

void TestAutoCloseMultiCaretSkipOverClose() {
  TextViewport viewport;
  viewport.LoadContent("()\n()\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.MoveCursorTo(0, 1);
  viewport.AddSecondaryCaret(1, 1);
  viewport.InsertCharacter(')');
  Expect(viewport.lines()[0] == "()" && viewport.lines()[1] == "()",
         "skip-over should not duplicate close characters for multi-caret");
  const auto carets = viewport.secondary_carets();
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 2,
         "primary caret should advance over existing close");
  Expect(carets.size() == 1 && carets[0].line == 1 && carets[0].column == 2,
         "secondary caret should advance over existing close");
}

void TestSmartIndentAfterOpenBrace() {
  TextViewport viewport;
  viewport.LoadContent("if (x) {\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, 8);  // end of line right after '{'
  viewport.InsertNewline();
  // Split-braces should NOT trigger here because there is no closer at caret;
  // smart-indent path inserts a single newline plus indent.
  Expect(viewport.lines().size() >= 2,
         "newline must produce a new line below");
  Expect(viewport.lines()[1] == "  ",
         "smart indent should insert one indent unit after '{'");
}

void TestSmartIndentDisabledFallsBackToBaseIndent() {
  TextViewport viewport;
  auto view = MakeCStyleContractView();
  view.smart_indent_enabled = false;
  viewport.SetLanguageContractView(std::move(view));
  viewport.LoadContent("  if (x) {\n", "/tmp/sample.cpp");
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertNewline();
  Expect(viewport.lines().size() >= 2, "newline should add another line");
  Expect(viewport.lines()[1] == "  ",
         "smart-indent disabled should preserve only base leading whitespace");
}

void TestSplitBracesInsertsThreeLines() {
  TextViewport viewport;
  viewport.LoadContent("foo() {}", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(0, 7);  // between '{' and '}'
  viewport.InsertNewline();
  Expect(viewport.lines().size() == 3,
         "split-braces should produce three lines");
  Expect(viewport.lines()[0] == "foo() {", "first line keeps the opener");
  Expect(viewport.lines()[1] == "  ", "middle line is the indented body");
  Expect(viewport.lines()[2] == "}", "third line carries the closer");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 2,
         "caret should land on the indented middle line");
}

void TestDedentOnCloseBrace() {
  TextViewport viewport;
  viewport.LoadContent("if (x) {\n    \n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  viewport.MoveCursorTo(1, 4);
  // Type '}' on an indent-only line; dedent-on-close should drop one indent
  // unit BEFORE the auto-close path runs.
  viewport.InsertCharacter('}');
  Expect(viewport.lines()[1] == "  }" || viewport.lines()[1] == "  }}",
         "dedent-on-close should reduce indent before inserting the close");
}

void TestSmartIndentAfterArbitraryPattern() {
  TextViewport viewport;
  auto view = MakeCStyleContractView();
  view.indent_after_open_patterns = {":"};
  view.dedent_on_close_chars = {};
  viewport.LoadContent("if cond:", "/tmp/sample.py");
  viewport.SetLanguageContractView(std::move(view));
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(0, viewport.lines()[0].size());
  viewport.InsertNewline();
  Expect(viewport.lines().size() >= 2,
         "newline should add a new line after arbitrary indent pattern");
  Expect(viewport.lines()[1] == "    ",
         "smart indent should add one indent unit after ':' opener");
}

void TestDedentOnCloseSkippedOnNonIndentOnlyLine() {
  TextViewport viewport;
  viewport.LoadContent("if (x) {\n    foo()\n", "/tmp/sample.cpp");
  viewport.SetLanguageContractView(MakeCStyleContractView());
  viewport.SetSoftTabs(true);
  viewport.SetIndentWidth(2);
  // Position caret at the end of "    foo()" (line index 1).
  viewport.MoveCursorTo(1, viewport.lines()[1].size());
  viewport.InsertCharacter('}');
  Expect(viewport.lines()[1].rfind("    foo()", 0) == 0,
         "dedent-on-close must not trigger on lines with non-whitespace content");
}

microide::workspace::TabEntry::EditorTabState MakeFoldingTab() {
  return microide::workspace::TabEntry::EditorTabState{};
}

microide::workspace::LanguageContract MakeCStyleFoldContract() {
  microide::workspace::LanguageContract contract;
  contract.language_id = "cpp";
  contract.bracket_pairs.push_back({"{", "}"});
  contract.bracket_pairs.push_back({"(", ")"});
  contract.bracket_pairs.push_back({"[", "]"});
  return contract;
}

void TestFoldingRefreshComputesBracketRanges() {
  auto tab = MakeFoldingTab();
  auto& viewport = tab.viewport;
  viewport.LoadContent("void f() {\n  body;\n}\n", "/tmp/sample.cpp");
  viewport.SetViewportSize(/*visible_lines=*/12, /*visible_columns=*/80);
  const auto contract = MakeCStyleFoldContract();
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract,
                                               /*tab_size=*/4,
                                               /*fold_enabled=*/true,
                                               /*visible_rows=*/viewport.visible_lines());
  Expect(!tab.folding_model->resolved_ranges().empty(),
         "fold model should have at least one range after refresh");
  Expect(tab.folding_model->resolved_ranges().front().opener_line == 0,
         "first fold opener must be on line 0");
  Expect(tab.folding_model->resolved_ranges().front().closer_line == 2,
         "first fold closer must be on line 2");
}

void TestFoldingRefreshFingerprintReusedAcrossCalls() {
  auto tab = MakeFoldingTab();
  auto& viewport = tab.viewport;
  viewport.LoadContent("void f() {\n  body;\n}\n", "/tmp/sample.cpp");
  viewport.SetViewportSize(/*visible_lines=*/12, /*visible_columns=*/80);
  const auto contract = MakeCStyleFoldContract();
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract, 4, true,
                                               viewport.visible_lines());
  const auto& fp_first = tab.folding_model->fingerprint();
  // A subsequent call with the same fingerprint must not change the stored
  // fingerprint or the ranges; the model exposes IsFresh as the canonical
  // freshness check, so we verify that.
  Expect(tab.folding_model->IsFresh(fp_first),
         "model must report itself fresh after a successful compute");
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract, 4, true,
                                               viewport.visible_lines());
  Expect(tab.folding_model->IsFresh(fp_first),
         "model must remain fresh on a repeat refresh with the same inputs");
}

void TestFoldingRefreshDisabledExpandsAndClears() {
  auto tab = MakeFoldingTab();
  auto& viewport = tab.viewport;
  viewport.LoadContent("void f() {\n  body;\n}\nvoid g() {\n  body;\n}\n", "/tmp/sample.cpp");
  viewport.SetViewportSize(/*visible_lines=*/12, /*visible_columns=*/80);
  const auto contract = MakeCStyleFoldContract();
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract, 4, true,
                                               viewport.visible_lines());
  Expect(tab.folding_model->resolved_ranges().size() >= 1,
         "fold model should have ranges after enable");
  Expect(tab.folding_model->Collapse(0), "expected to collapse fold at line 0");
  Expect(tab.folding_model->IsCollapsedAtOpener(0),
         "fold at line 0 should be collapsed before disable");
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &contract, 4,
                                               /*fold_enabled=*/false,
                                               /*visible_rows=*/viewport.visible_lines());
  Expect(tab.folding_model->resolved_ranges().empty(),
         "disabling fold must clear stored ranges");
  Expect(!tab.folding_model->has_any_collapsed_fold(),
         "disabling fold must drop collapsed state so no rows are hidden");
}

void TestFoldingRefreshLanguageChangeRebuilds() {
  auto tab = MakeFoldingTab();
  auto& viewport = tab.viewport;
  viewport.LoadContent("void f() {\n  body;\n}\n", "/tmp/sample.cpp");
  viewport.SetViewportSize(/*visible_lines=*/12, /*visible_columns=*/80);
  const auto cpp = MakeCStyleFoldContract();
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &cpp, 4, true,
                                               viewport.visible_lines());
  const auto cpp_fp = tab.folding_model->fingerprint();
  microide::workspace::LanguageContract other = cpp;
  other.language_id = "rust";
  microide::workspace::EnsureFoldingModelFresh(tab, viewport, &other, 4, true,
                                               viewport.visible_lines());
  Expect(!tab.folding_model->IsFresh(cpp_fp),
         "language id change must invalidate the prior fingerprint");
  Expect(tab.folding_model->fingerprint().language_id == "rust",
         "post-refresh fingerprint must reflect the new language id");
}

// TD-2026-08-05-133: three places asked "how wide is this line's leading
// whitespace" by materializing the whole line -- the fold indent source, the
// indent guides, and the caret's active-guide column. They share one chunked scan
// now. Two claims, and only the first is about the answer:
//   * the LineSpan form equals the whole-line form on every shape, including
//     indents that cross the 256-byte chunk boundary with tabs on both sides, and
//   * asking a piece-tree line that spans pieces reads none of it past the indent.
void TestLeadingIndentScanIsBoundedAndMatchesTheWholeLineForm() {
  using microide::editor::LeadingVisualIndent;
  constexpr std::size_t kTabSize = 3;  // does not divide the chunk, so the
                                       // carried column across a boundary matters

  std::vector<std::string> shapes = {
      "", " ", "\t", "code", "  code", "\t\tcode", " \t \tcode", "   ",
      std::string(255, ' ') + "\tx",  std::string(256, ' ') + "\tx",
      std::string(257, ' ') + "\tx",  std::string(600, ' ') + "\t\t\tx",
      std::string(300, ' '),           std::string(300, '\t') + "x",
  };
  for (const std::string& shape : shapes) {
    microide::editor::TextBuffer buffer;
    buffer.ResetFromText(shape + "\n");
    // Fragment the line without changing it: insert a byte mid-line and delete it
    // again. The tree never re-merges pieces, so the line now spans them and
    // reading it whole can only be served by a copy. A zero-length splice would
    // not do -- it is a no-op and leaves the line contiguous, which is how a
    // "fragmented" fixture quietly stops testing the path it names.
    if (shape.size() >= 2) {
      const std::size_t mid = shape.size() / 2;
      buffer.ReplaceTextRange(0, mid, 0, mid, "Z");
      buffer.ReplaceTextRange(0, mid, 0, mid + 1, "");
    }
    const microide::editor::LineSpan span(buffer);
    Expect(LeadingVisualIndent(span, 0, kTabSize) == LeadingVisualIndent(shape, kTabSize),
           "the chunked indent scan must equal the whole-line one (" +
               std::to_string(shape.size()) + " bytes)");
  }

  // The work claim, on a line long enough that reading it would show up.
  microide::editor::TextBuffer wide;
  wide.ResetFromText(std::string(8, ' ') + std::string(512 * 1024, 'x') + "\n");
  wide.ReplaceTextRange(0, 100000, 0, 100000, "Z");  // split it into pieces...
  wide.ReplaceTextRange(0, 100000, 0, 100001, "");   // ...and leave them split
  util::ResetPerformanceCounters();
  Expect(LeadingVisualIndent(microide::editor::LineSpan(wide), 0, kTabSize) == 8,
         "the indent of a long line is still measured correctly");
  const std::uint64_t copied =
      util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes);
  Expect(copied == 0, "measuring an indent must not read the line (copied " +
                          std::to_string(copied) + " bytes)");
  // Reachability control: the same line read whole does copy it.
  (void)wide.LineView(0);
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes) > 512 * 1024,
         "reading the spanning line whole still copies it (control)");
  util::ResetPerformanceCounters();
}

}  // namespace


// A plain stack over the text is the definition of bracket matching (no
// syntax viewport, so strings and comments are not exempt): for every bracket
// in random bracket soup the scanner must return the stack's partner, and the
// partner's partner must be the original. Random multi-line soup with all
// three bracket kinds nested and mismatched, with long and empty lines.
void TestBracketScannerAgreesWithStackReference() {
  using microide::editor::FindBracketMatchInLines;
  std::uint64_t state = 0xA5A5A5A55A5A5A5Aull;
  const auto next = [&state](std::uint64_t bound) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return bound == 0 ? 0 : static_cast<std::size_t>(state % bound);
  };
  static constexpr const char* kAtoms[] = {"(", ")", "[", "]", "{", "}", "a", " ", "é", "\n"};
  std::size_t checked = 0;
  for (int round = 0; round < 60; ++round) {
    std::string text;
    const std::size_t atoms = 1 + next(40);
    for (std::size_t i = 0; i < atoms; ++i) {
      text += kAtoms[next(std::size(kAtoms))];
    }
    std::vector<std::string> owned;
    {
      std::size_t start = 0;
      while (true) {
        const std::size_t newline = text.find('\n', start);
        owned.push_back(text.substr(start, newline == std::string::npos ? std::string::npos
                                                                        : newline - start));
        if (newline == std::string::npos) break;
        start = newline + 1;
      }
    }
    std::vector<std::string_view> lines(owned.begin(), owned.end());
    struct Pos { std::size_t line, column; };
    std::vector<std::pair<Pos, Pos>> pairs;
    std::vector<std::pair<Pos, char>> stack;
    for (std::size_t l = 0; l < lines.size(); ++l) {
      for (std::size_t c = 0; c < lines[l].size(); ++c) {
        const char ch = lines[l][c];
        if (ch == '(' || ch == '[' || ch == '{') {
          stack.push_back({{l, c}, ch});
        } else if (ch == ')' || ch == ']' || ch == '}') {
          const char open = ch == ')' ? '(' : ch == ']' ? '[' : '{';
          if (!stack.empty() && stack.back().second == open) {
            pairs.push_back({stack.back().first, {l, c}});
            stack.pop_back();
          } else {
            stack.clear();  // a mismatched closer abandons the openers it could not close
          }
        }
      }
    }
    const auto expected_partner = [&](std::size_t l, std::size_t c) -> std::optional<Pos> {
      for (const auto& [open, close] : pairs) {
        if (open.line == l && open.column == c) return close;
        if (close.line == l && close.column == c) return open;
      }
      return std::nullopt;
    };
    // The scanner tries the bracket AT the caret first and the one before it
    // second, so the bracket it used is read back from the pair it returns.
    for (std::size_t l = 0; l < lines.size(); ++l) {
      for (std::size_t c = 0; c <= lines[l].size(); ++c) {
        const auto got = FindBracketMatchInLines(lines, l, c, 2000, nullptr);
        const std::string label = "round " + std::to_string(round) + " text=[" + text +
                                  "] caret " + std::to_string(l) + ":" + std::to_string(c);
        if (!got.has_value()) {
          // Nothing found: neither candidate may have a reference partner.
          for (const std::size_t cand : {c, c > 0 ? c - 1 : c}) {
            if (cand < lines[l].size()) {
              Expect(!expected_partner(l, cand).has_value(),
                     (label + ": no match, so candidate " + std::to_string(cand) +
                      " has no reference partner")
                         .c_str());
            }
          }
          ++checked;
          continue;
        }
        const std::size_t used_line = got->caret_at_opener ? got->open_line : got->close_line;
        const std::size_t used_col = got->caret_at_opener ? got->open_column : got->close_column;
        const std::size_t partner_line = got->caret_at_opener ? got->close_line : got->open_line;
        const std::size_t partner_col = got->caret_at_opener ? got->close_column : got->open_column;
        Expect(used_line == l && (used_col == c || used_col + 1 == c),
               (label + ": the matched bracket is at or just before the caret").c_str());
        const char ch = lines[used_line][used_col];
        const char partner = lines[partner_line][partner_col];
        const char expected_partner_char = ch == '(' ? ')' : ch == ')' ? '(' : ch == '[' ? ']'
                                           : ch == ']' ? '[' : ch == '{' ? '}' : '{';
        Expect(partner == expected_partner_char,
               (label + ": the partner is the matching bracket kind").c_str());
        if (const auto want = expected_partner(used_line, used_col); want.has_value()) {
          Expect(partner_line == want->line && partner_col == want->column,
                 (label + ": the partner is the stack's partner (got " +
                  std::to_string(partner_line) + ":" + std::to_string(partner_col) + " want " +
                  std::to_string(want->line) + ":" + std::to_string(want->column) + ")")
                     .c_str());
        }
        // Symmetric: the partner's own match (the bracket AT the partner is
        // tried first) is this pair.
        const auto back = FindBracketMatchInLines(lines, partner_line, partner_col, 2000, nullptr);
        Expect(back.has_value() && back->open_line == got->open_line &&
                   back->open_column == got->open_column && back->close_line == got->close_line &&
                   back->close_column == got->close_column,
               (label + ": the partner's match is the same pair").c_str());
        ++checked;
      }
    }
  }
  Expect(checked >= 300, "enough brackets were checked to mean anything");
}

void RegisterEditorEssentialsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorEssentials/IndentGuides/BracketScannerAgreesWithStackReference",
          TestBracketScannerAgreesWithStackReference);
  AddTest(tests, "EditorEssentials/IndentGuides/LeadingIndentScanIsBoundedAndMatchesTheWholeLineForm",
          TestLeadingIndentScanIsBoundedAndMatchesTheWholeLineForm);
  AddTest(tests, "EditorEssentials/BracketScanner/ForwardMatch",
          TestBracketScannerForwardMatch);
  AddTest(tests, "EditorEssentials/BracketScanner/BackwardMatch",
          TestBracketScannerBackwardMatch);
  AddTest(tests, "EditorEssentials/BracketScanner/NoMatchOnUnbalanced",
          TestBracketScannerNoMatchOnUnbalanced);
  AddTest(tests, "EditorEssentials/BracketScanner/SkipsStringClosers",
          TestBracketScannerSkipsStringClosers);
  AddTest(tests, "EditorEssentials/BracketScanner/SkipsCommentBraces",
          TestBracketScannerSkipsCommentBraces);
  AddTest(tests, "EditorEssentials/BracketScanner/NoMatchWhenAnchorInsideString",
          TestBracketScannerNoMatchWhenAnchorInsideString);
  AddTest(tests, "EditorEssentials/BracketScanner/ProbesTokensOncePerLineNotPerByte",
          TestBracketScannerProbesTokensOncePerLineNotPerByte);
  AddTest(tests, "EditorEssentials/BracketScanner/StopsAtTheScanByteCap",
          TestBracketScannerStopsAtTheScanByteCap);
  AddTest(tests, "EditorEssentials/BracketScanner/RefusesALineTooLongToTokenize",
          TestBracketScannerRefusesALineTooLongToTokenize);
  AddTest(tests, "EditorEssentials/BracketScanner/WindowedDeepCaret",
          TestBracketScannerWindowedDeepCaret);
  AddTest(tests, "EditorEssentials/Shaping/LineOpAppliesOneDocumentSplice",
          TestShapingLineOpAppliesOneDocumentSplice);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineDown",
          TestShapingMoveLineDown);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineDownKeepsWholeLineSelection",
          TestShapingMoveLineDownKeepsWholeLineSelection);
  AddTest(tests, "EditorEssentials/Shaping/MultiCaretOpsDoNotTouchLinesBetweenCarets",
          TestShapingMultiCaretOpsDoNotTouchLinesBetweenCarets);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineSkipsOnlyThePinnedRegion",
          TestShapingMoveLineSkipsOnlyThePinnedRegion);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineDownMultiCaretSingleUndoStep",
          TestShapingMoveLineDownMultiCaretSingleUndoStep);
  AddTest(tests, "EditorEssentials/Shaping/PreservesRangedSecondaryCaretAnchors",
          TestShapingPreservesRangedSecondaryCaretAnchors);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineDownRedoPreservesMultiCaret",
          TestShapingMoveLineDownRedoPreservesMultiCaret);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineUpMultiCaretSingleUndoStep",
          TestShapingMoveLineUpMultiCaretSingleUndoStep);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineUpAtTop",
          TestShapingMoveLineUpAtTopIsNoop);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineDownMovesCursor",
          TestShapingMoveLineDownMovesCursor);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineUpMovesCursor",
          TestShapingMoveLineUpMovesCursor);
  AddTest(tests, "EditorEssentials/Shaping/MoveLineUpKeepsSecondaryCaretAndSelection",
          TestShapingMoveLineUpKeepsSecondaryCaretAndSelection);
  AddTest(tests, "EditorEssentials/Shaping/DuplicateLine",
          TestShapingDuplicateLine);
  AddTest(tests, "EditorEssentials/Shaping/CopyLinesDirections",
          TestShapingCopyLinesDirections);
  AddTest(tests, "EditorEssentials/Shaping/InsertLineBelowAndAbove",
          TestShapingInsertLineBelowAndAbove);
  AddTest(tests, "EditorEssentials/Shaping/IndentSelection",
          TestShapingIndentSelection);
  AddTest(tests, "EditorEssentials/Shaping/ToggleLineComment",
          TestShapingToggleLineComment);
  AddTest(tests, "EditorEssentials/Shaping/ToggleBlockCommentRoundTrips",
          TestShapingToggleBlockCommentRoundTrips);
  AddTest(tests, "EditorEssentials/Shaping/ToggleBlockCommentSelectionIsAToggle",
          TestShapingToggleBlockCommentSelectionIsAToggle);
  AddTest(tests, "EditorEssentials/Shaping/ToggleBlockCommentSelectionStripsWithWhitespace",
          TestShapingToggleBlockCommentSelectionStripsWithWhitespace);
  AddTest(tests, "EditorEssentials/Shaping/SortLinesAscending",
          TestShapingSortLinesAscending);
  AddTest(tests, "EditorEssentials/Save/TrimTrailingWhitespace",
          TestSaveNormalizationTrim);
  AddTest(tests, "EditorEssentials/Save/TextViewportAppliesNormalization",
          TestTextViewportSaveAppliesNormalization);
  AddTest(tests, "EditorEssentials/Save/NormalizationPreservesCaret",
          TestTextViewportSaveNormalizationPreservesCaret);
  AddTest(tests, "EditorEssentials/Save/EnsureFinalNewline",
          TestSaveNormalizationEnsureFinalNewline);
  AddTest(tests, "EditorEssentials/Save/DetectsDiskConflict",
          TestTextViewportDetectsDiskConflict);
  AddTest(tests, "EditorEssentials/Save/UntitledHasNoDiskConflict",
          TestTextViewportUntitledHasNoDiskConflict);
  AddTest(tests, "EditorEssentials/IndentDetect/SpacesMajority",
          TestIndentDetectSpacesMajority);
  AddTest(tests, "EditorEssentials/IndentDetect/TabsMajority",
          TestIndentDetectTabsMajority);
  AddTest(tests, "EditorEssentials/IndentDetect/MixedLeadingTabsCountAsTabs",
          TestIndentDetectMixedLeadingTabsCountAsTabs);
  AddTest(tests, "EditorEssentials/IndentDetect/TwoSpaceFileWithFourSpaceContinuations",
          TestIndentDetectTwoSpaceFileWithFourSpaceContinuations);
  AddTest(tests, "EditorEssentials/IndentDetect/TieLeavesStyleUndetermined",
          TestIndentDetectTieLeavesStyleUndetermined);
  AddTest(tests, "EditorEssentials/IndentDetect/MaxInspectBoundsNonBlankCount",
          TestIndentDetectMaxInspectBoundsNonBlankCount);
  AddTest(tests, "EditorEssentials/IndentDetect/ApplyOnOpenUpdatesViewport",
          TestApplyDetectedIndentOnOpenUpdatesViewport);
  AddTest(tests, "EditorEssentials/IndentDetect/ApplyOnOpenSkippedWhenDisabled",
          TestApplyDetectedIndentOnOpenSkippedWhenDisabled);
  AddTest(tests, "EditorEssentials/IndentDetect/ApplyOnOpenSkippedWhenPathEmpty",
          TestApplyDetectedIndentOnOpenSkippedWhenPathEmpty);
  AddTest(tests, "EditorEssentials/LanguageContract/Defaults",
          TestLanguageContractDefaults);
  AddTest(tests, "EditorEssentials/LanguageContract/AliasesCppFiletypeName",
          TestLanguageContractAliasesCppFiletypeName);
  AddTest(tests, "EditorEssentials/LanguageContract/Missing",
          TestLanguageContractMissingLanguage);
  AddTest(tests, "EditorEssentials/LanguageContract/AppliesUserOverrides",
          TestLanguageContractAppliesUserOverrides);
  AddTest(tests, "EditorEssentials/LanguageContract/SkipsNoOpRefresh",
          TestLanguageContractSkipsNoOpRefresh);
  AddTest(tests, "EditorEssentials/IndentGuides/EmitsRunsAtNestedDepths",
          TestIndentGuidesEmitsRunsAtNestedDepths);
  AddTest(tests, "EditorEssentials/IndentGuides/RunsMatchNaiveCoverage",
          TestIndentGuidesRunsMatchNaiveCoverage);
  AddTest(tests, "EditorEssentials/IndentGuides/MarksActiveAtParentColumn",
          TestIndentGuidesMarksActiveAtParentColumn);
  AddTest(tests, "EditorEssentials/IndentGuides/FoldModelEmphasisOnInnerCloser",
          TestIndentGuidesFoldModelEmphasisOnInnerCloser);
  AddTest(tests, "EditorEssentials/RenderViewModel/WhitespaceGlyphRunsToggle",
          TestRenderViewModelBuilderWhitespaceGlyphRunsToggle);
  AddTest(tests, "EditorEssentials/IndentGuidesBreakAcrossABlankDiffFillerRow",
          TestIndentGuidesBreakAcrossABlankDiffFillerRow);
  AddTest(tests, "EditorEssentials/WhitespaceGlyphRunsAgreeWithTheSharedWalk",
          TestWhitespaceGlyphRunsAgreeWithTheSharedWalk);
  AddTest(tests, "EditorEssentials/AutoClose/InsertsClose",
          TestAutoClosePairInsertsClose);
  AddTest(tests, "EditorEssentials/AutoClose/SkipOverClose",
          TestAutoCloseSkipOverClose);
  AddTest(tests, "EditorEssentials/AutoClose/DisabledByDefault",
          TestAutoCloseDisabledByDefault);
  AddTest(tests, "EditorEssentials/AutoClose/InsertTextSingleCharUsesPairPath",
          TestAutoCloseViaInsertTextSingleCharacter);
  AddTest(tests, "EditorEssentials/AutoClose/InhibitInsideString",
          TestAutoCloseInhibitedInsideString);
  AddTest(tests, "EditorEssentials/AutoClose/InhibitInsideComment",
          TestAutoCloseInhibitedInsideComment);
  AddTest(tests, "EditorEssentials/AutoClose/MultiCaretInsertsPairs",
          TestAutoCloseMultiCaretInsertsPairs);
  AddTest(tests, "EditorEssentials/AutoClose/MultiCaretSkipOverClose",
          TestAutoCloseMultiCaretSkipOverClose);
  AddTest(tests, "EditorEssentials/Surround/SingleLineSelection",
          TestSurroundSelection);
  AddTest(tests, "EditorEssentials/Surround/MultiLineSelection",
          TestSurroundMultiLineSelection);
  AddTest(tests, "EditorEssentials/Surround/MultiCaretMultiLineSelections",
          TestMultiCaretSurroundMultiLineSelections);
  AddTest(tests, "EditorEssentials/Surround/MultiCaretAcrossADistantGap",
          TestMultiCaretSurroundAcrossADistantGap);
  AddTest(tests, "EditorEssentials/Surround/BoundaryWrapInnerSelectionAndUndo",
          TestSurroundBoundaryWrapInnerSelectionAndUndo);
  AddTest(tests, "EditorEssentials/Surround/DisabledFallsBackLiteral",
          TestSurroundDisabledFallsBackToLiteralInsert);
  AddTest(tests, "EditorEssentials/SmartIndent/AfterOpenBrace",
          TestSmartIndentAfterOpenBrace);
  AddTest(tests, "EditorEssentials/SmartIndent/AfterArbitraryPattern",
          TestSmartIndentAfterArbitraryPattern);
  AddTest(tests, "EditorEssentials/DedentOnClose/SkippedOnNonIndentOnlyLine",
          TestDedentOnCloseSkippedOnNonIndentOnlyLine);
  AddTest(tests, "EditorEssentials/SmartIndent/DisabledFallsBackToBaseIndent",
          TestSmartIndentDisabledFallsBackToBaseIndent);
  AddTest(tests, "EditorEssentials/SplitBraces/InsertsThreeLines",
          TestSplitBracesInsertsThreeLines);
  AddTest(tests, "EditorEssentials/DedentOnClose/Brace",
          TestDedentOnCloseBrace);
  AddTest(tests, "EditorEssentials/Folding/RefreshComputesBracketRanges",
          TestFoldingRefreshComputesBracketRanges);
  AddTest(tests, "EditorEssentials/Folding/RefreshFingerprintReusedAcrossCalls",
          TestFoldingRefreshFingerprintReusedAcrossCalls);
  AddTest(tests, "EditorEssentials/Folding/RefreshDisabledExpandsAndClears",
          TestFoldingRefreshDisabledExpandsAndClears);
  AddTest(tests, "EditorEssentials/Folding/RefreshLanguageChangeRebuilds",
          TestFoldingRefreshLanguageChangeRebuilds);
}

}  // namespace microide::tests
