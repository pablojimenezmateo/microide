// The multi-caret remap batch, against the reference it claims to reproduce.
//
// `ResolveMultiCaretRemapSites` folds every lower caret's edit into every higher
// caret's recorded position. It has TWO paths -- an O(sites) forward pass for
// the common shape (single-line removed ranges, no anchors) and the exact
// O(sites^2) per-edit remap for everything else -- and the header states they
// "yield identical results". Nothing checked that. The fast path is an
// optimisation guarded by an eligibility test, which is the shape where a wrong
// answer is invisible: the slow path keeps working, so every test that happens
// to carry an anchor passes while the fast path is quietly wrong.
//
// So: random eligible site sets, resolved by the function, compared against the
// exact per-edit fold written out here. And `ComputeReplacementShape`, which the
// remap reads, checked against the line-ending normalizer it says it must
// mirror -- the header records what goes wrong if it does not ("higher carets
// land on the wrong line").

#include "TestSupport.h"

#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/TextViewportInternal.h"
#include "util/StringUtil.h"

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::TextPosition;
using microide::editor::detail::ComputeReplacementShape;
using microide::editor::detail::MultiCaretRemapSite;
using microide::editor::detail::RemapPositionAfterReplace;
using microide::editor::detail::ReplacementShape;
using microide::editor::detail::ResolveMultiCaretRemapSites;

std::string Show(const TextPosition& p) {
  return "(" + std::to_string(p.line) + "," + std::to_string(p.column) + ")";
}

// The exact reference the fast path claims to reproduce: site i is remapped by
// edits i-1, i-2, ..., 0 in that order.
void ResolveExactly(std::vector<MultiCaretRemapSite>& sites) {
  for (std::size_t i = 0; i < sites.size(); ++i) {
    for (std::size_t k = i; k-- > 0;) {
      if (!sites[k].has_edit) {
        continue;
      }
      const SelectionRange& removed = sites[k].removed;
      sites[i].landed =
          RemapPositionAfterReplace(sites[i].landed, removed.start, removed.end, sites[k].shape);
      if (sites[i].anchor.has_value()) {
        sites[i].anchor = RemapPositionAfterReplace(*sites[i].anchor, removed.start, removed.end,
                                                    sites[k].shape);
      }
    }
  }
}

void TestFastRemapMatchesTheExactFold() {
  std::mt19937 rng(20260925u);
  auto pick = [&](std::size_t n) {
    return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
  };
  std::size_t multi_site_runs = 0;
  std::size_t same_line_pairs = 0;
  std::size_t newline_inserts = 0;

  for (int iteration = 0; iteration < 6000; ++iteration) {
    // Sites in ascending caret order with single-line removed ranges and no
    // anchors -- the shape the fast path is eligible for. Several sites share a
    // line on purpose: that is where the column accumulator has to be right.
    std::vector<MultiCaretRemapSite> sites;
    const std::size_t count = 1 + pick(5);
    std::size_t line = pick(3);
    std::size_t column = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (pick(3) == 0) {
        line += 1 + pick(2);
        column = 0;
      }
      const std::size_t start = column + pick(3);
      const std::size_t end = start + pick(3);
      column = end + 1;

      MultiCaretRemapSite site;
      site.has_edit = pick(6) != 0;  // some sites produce no edit at all
      site.removed = SelectionRange{TextPosition{line, start}, TextPosition{line, end}};
      site.shape.inserted_newlines = pick(4) == 0 ? 1 + pick(2) : 0;
      site.shape.last_segment_cols = pick(4);
      if (site.shape.inserted_newlines > 0) {
        ++newline_inserts;
      }
      // Where the walk records the caret after applying this site's own edit.
      site.landed = site.shape.inserted_newlines == 0
                        ? TextPosition{line, start + site.shape.last_segment_cols}
                        : TextPosition{line + site.shape.inserted_newlines,
                                       site.shape.last_segment_cols};
      if (!site.has_edit) {
        site.landed = TextPosition{line, start};
      }
      if (i > 0 && sites.back().removed.start.line == line) {
        ++same_line_pairs;
      }
      sites.push_back(site);
    }
    if (count > 1) {
      ++multi_site_runs;
    }

    std::vector<MultiCaretRemapSite> fast = sites;
    std::vector<MultiCaretRemapSite> exact = sites;
    ResolveMultiCaretRemapSites(fast);
    ResolveExactly(exact);

    Expect(fast.size() == exact.size(), "the resolve keeps every site");
    for (std::size_t i = 0; i < fast.size(); ++i) {
      Expect(fast[i].landed == exact[i].landed,
             "site " + std::to_string(i) + ": the fast fold gave " + Show(fast[i].landed) +
                 " and the exact fold " + Show(exact[i].landed));
    }
  }

  Expect(multi_site_runs > 2000,
         "the sweep barely built multi-site batches, where the fold does anything: " +
             std::to_string(multi_site_runs));
  Expect(same_line_pairs > 500,
         "the sweep barely put two sites on one line, which is what the column accumulator "
         "is for: " + std::to_string(same_line_pairs));
  Expect(newline_inserts > 500,
         "the sweep barely inserted a newline, which is what rebases the accumulator: " +
             std::to_string(newline_inserts));
}

// ComputeReplacementShape must agree with the normalizer every edit path applies
// before splitting the replacement into lines.
void TestReplacementShapeMirrorsLineEndingNormalization() {
  const std::vector<std::string> bodies = {
      "",
      "abc",
      "a\nb",
      "a\r\nb",
      "a\rb",
      "\r\n",
      "\n",
      "\r",
      "a\r\n\r\nb",
      "a\n\rb",
      "a\r\rb",
      "trailing\n",
      "trailing\r\n",
      "\r\nleading",
      "mixed\r\nand\rand\nend",
  };
  for (const std::string& body : bodies) {
    const ReplacementShape shape = ComputeReplacementShape(body);

    // Oracle: normalize exactly as the edit paths do, then count.
    const std::string normalized = microide::util::NormalizeLineEndings(body);
    std::size_t newlines = 0;
    std::size_t last_segment = 0;
    for (const char ch : normalized) {
      if (ch == '\n') {
        ++newlines;
        last_segment = 0;
      } else {
        ++last_segment;
      }
    }
    std::string escaped;
    for (const char ch : body) {
      if (ch == '\n') escaped += "\\n";
      else if (ch == '\r') escaped += "\\r";
      else escaped.push_back(ch);
    }
    const std::string where = "body \"" + escaped + "\"";
    Expect(shape.inserted_newlines == newlines,
           where + ": newline count " + std::to_string(shape.inserted_newlines) +
               " against the normalizer's " + std::to_string(newlines));
    Expect(shape.last_segment_cols == last_segment,
           where + ": last segment " + std::to_string(shape.last_segment_cols) +
               " against the normalizer's " + std::to_string(last_segment));
  }
}

}  // namespace

void RegisterMultiCaretRemapTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MultiCaretRemap/FastRemapMatchesTheExactFold", TestFastRemapMatchesTheExactFold);
  AddTest(tests, "MultiCaretRemap/ReplacementShapeMirrorsLineEndingNormalization",
          TestReplacementShapeMirrorsLineEndingNormalization);
}

}  // namespace microide::tests
