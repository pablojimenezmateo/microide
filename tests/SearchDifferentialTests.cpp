// Differential tests over the four implementations of "where does this query
// match".
//
// The in-file find widget answers that question four different ways, each one a
// speed optimisation of the one before it:
//
//   1. FindLiteralSearchMatches(lines, ...)   -- over a document snapshot
//   2. FindLiteralSearchMatches(buffer, ...)  -- zero-copy over the piece tree
//   3. RefineLiteralSearchMatches(...)        -- per-keystroke incremental scan
//   4. FindRegexSearchMatches(...)            -- PCRE2, over the joined buffer
//
// (3)'s header states it equals (2) "by construction"; (4) with an escaped
// literal pattern must find the same spans as (1). All four are exercised on
// every keystroke of a find session, so a divergence desynchronises the match
// count, next/previous navigation and replace-all against what is highlighted.
// The existing suite pins them on fixed queries; this asserts the agreement over
// a generated corpus, with whole-word and case-sensitivity toggled.

#include "TestSupport.h"

#include <cstdint>
#include <string>
#include <vector>

#include "editor/TextBuffer.h"
#include "util/RegexUtil.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::tests {
namespace {

using microide::editor::SelectionRange;
using microide::editor::TextBuffer;
using microide::workspace::BufferSearchOptions;
using microide::workspace::FindLiteralSearchMatches;
using microide::workspace::FindRegexSearchMatches;
using microide::workspace::RefineLiteralSearchMatches;

struct Rng {
  std::uint64_t state;
  explicit Rng(std::uint64_t seed) : state(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}
  std::uint64_t Next() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1Dull;
  }
  std::size_t Below(std::size_t bound) { return bound == 0 ? 0 : Next() % bound; }
};

// Deliberately full of self-overlapping runs, case variation, word boundaries
// and multi-byte characters -- the four things that separate these scanners.
const std::vector<std::string>& LinePool() {
  static const std::vector<std::string> pool = {
      "aaaa",
      "aaa aa a",
      "Alpha alphabet ALPHA",
      "alpha-alpha_alpha",
      "no match here",
      "aLpHaBeT",
      "banana ananas",
      "",
      " ",
      "café CAFÉ Café",
      "δέλτα ΔΕΛΤΑ",
      "naïve NAÏVE",
      "日本語 日本 語 日本語",
      "x.y.z",
      "aXaa",
      "end with alpha",
      "alpha",
  };
  return pool;
}

const std::vector<std::string>& QueryPool() {
  static const std::vector<std::string> pool = {
      "a",   "aa",   "alpha", "ALPHA", "Alpha", "alph", "an",     "ana",
      "na",  "café", "CAFÉ",  "é",     "δ",     "Δ",    "日本",   "日本語",
      "ï",   " ",    "x.y",   "z",     "b",     "t",
  };
  return pool;
}

std::string Describe(std::uint64_t seed, std::string_view query, const BufferSearchOptions& o) {
  return " [seed=" + std::to_string(seed) + " query='" + std::string(query) +
         "' case_sensitive=" + (o.case_sensitive ? "1" : "0") +
         " whole_word=" + (o.whole_word ? "1" : "0") + "]";
}

std::string Render(const std::vector<SelectionRange>& matches) {
  std::string out;
  for (const SelectionRange& match : matches) {
    out += "(" + std::to_string(match.start.line) + ":" + std::to_string(match.start.column) +
           "-" + std::to_string(match.end.line) + ":" + std::to_string(match.end.column) + ")";
  }
  return out.empty() ? "<none>" : out;
}

bool Same(const std::vector<SelectionRange>& a, const std::vector<SelectionRange>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].start.line != b[i].start.line || a[i].start.column != b[i].start.column ||
        a[i].end.line != b[i].end.line || a[i].end.column != b[i].end.column) {
      return false;
    }
  }
  return true;
}

// Escape a literal so PCRE2 reads it as the same bytes. \Q..\E would do, but
// escaping each metacharacter keeps the pattern free of any quoting subtlety of
// its own.
std::string EscapeForRegex(std::string_view literal) {
  static constexpr std::string_view kMeta = R"(\^$.[]|()?*+{}-#&~)";
  std::string out;
  out.reserve(literal.size() * 2);
  for (const char c : literal) {
    if (kMeta.find(c) != std::string_view::npos) out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

std::vector<std::string> BuildDocument(Rng& rng) {
  const std::vector<std::string>& pool = LinePool();
  std::vector<std::string> lines;
  const std::size_t count = 1 + rng.Below(8);
  lines.reserve(count);
  for (std::size_t i = 0; i < count; ++i) lines.push_back(pool[rng.Below(pool.size())]);
  return lines;
}

// (1) vs (2): the snapshot scanner and the zero-copy buffer scanner.
void TestSnapshotAndBufferScannersAgree() {
  std::size_t total_matches = 0;
  for (std::uint64_t seed = 1; seed <= 500; ++seed) {
    Rng rng(seed);
    const std::vector<std::string> lines = BuildDocument(rng);
    const TextBuffer buffer(lines);
    for (const std::string& query : QueryPool()) {
      for (const bool case_sensitive : {false, true}) {
        for (const bool whole_word : {false, true}) {
          BufferSearchOptions options{.case_sensitive = case_sensitive, .whole_word = whole_word};
          const auto from_lines = FindLiteralSearchMatches(lines, query, options);
          const auto from_buffer = FindLiteralSearchMatches(buffer, query, options);
          total_matches += from_lines.size();
          Expect(Same(from_lines, from_buffer),
                 "snapshot and buffer scanners must agree" + Describe(seed, query, options) +
                     "\n  lines:  " + Render(from_lines) + "\n  buffer: " + Render(from_buffer));
        }
      }
    }
  }
  Expect(total_matches > 5000,
         "the corpus must actually produce matches, saw " + std::to_string(total_matches));
}

// (2) vs (3): the per-keystroke incremental refine against a fresh full scan, at
// every prefix length of every query.
void TestIncrementalRefineEqualsAFreshScan() {
  for (std::uint64_t seed = 1; seed <= 500; ++seed) {
    Rng rng(seed ^ 0x8EF19Eull);
    const std::vector<std::string> lines = BuildDocument(rng);
    const TextBuffer buffer(lines);
    for (const std::string& query : QueryPool()) {
      for (const bool whole_word : {false, true}) {
        // Refine is the case-insensitive find-as-you-type path; it is only valid
        // where the shorter query is a case-insensitive prefix of the longer.
        BufferSearchOptions options{.case_sensitive = false, .whole_word = whole_word};
        std::vector<SelectionRange> previous;
        bool have_previous = false;
        for (std::size_t length = 1; length <= query.size(); ++length) {
          // Step one whole UTF-8 character at a time; a partial sequence is not a
          // query a keystroke can produce.
          if (length < query.size() &&
              (static_cast<unsigned char>(query[length]) & 0xC0) == 0x80) {
            continue;
          }
          const std::string prefix = query.substr(0, length);
          const auto fresh = FindLiteralSearchMatches(buffer, prefix, options);
          if (have_previous) {
            const auto refined = RefineLiteralSearchMatches(buffer, prefix, previous, options);
            Expect(Same(refined, fresh),
                   "incremental refine must equal a fresh scan" +
                       Describe(seed, prefix, options) + "\n  refined: " + Render(refined) +
                       "\n  fresh:   " + Render(fresh));
          }
          previous = fresh;
          have_previous = true;
        }
      }
    }
  }
}

// (1) vs (4): an escaped literal pattern must find exactly the literal spans.
// This is the cross-check the compile-option comment in RegexUtil.h is about --
// the regex path once folded ASCII only, so the same query matched
// case-insensitively as a literal and case-sensitively as a regex.
void TestRegexOfAnEscapedLiteralFindsTheLiteralSpans() {
  std::size_t compared = 0;
  for (std::uint64_t seed = 1; seed <= 400; ++seed) {
    Rng rng(seed ^ 0xC0DEull);
    const std::vector<std::string> lines = BuildDocument(rng);
    const TextBuffer buffer(lines);
    for (const std::string& query : QueryPool()) {
      for (const bool case_sensitive : {false, true}) {
        BufferSearchOptions options{.case_sensitive = case_sensitive, .whole_word = false};
        const util::CompiledRegex pattern(
            EscapeForRegex(query), util::SearchRegexCompileOptions(query, case_sensitive));
        if (!pattern.valid()) continue;
        const auto literal = FindLiteralSearchMatches(lines, query, options);
        const auto regex = FindRegexSearchMatches(buffer, pattern, options);
        ++compared;
        Expect(Same(literal, regex),
               "an escaped literal pattern must find the literal spans" +
                   Describe(seed, query, options) + "\n  literal: " + Render(literal) +
                   "\n  regex:   " + Render(regex));
      }
    }
  }
  Expect(compared > 1000, "every pattern in the pool should have compiled");
}

// Whatever the scanner, a match set must be sorted, non-empty, non-overlapping
// and inside the buffer. Overlapping spans desynchronise next/previous and make
// replace-all double-edit.
void TestMatchSetsAreOrderedNonOverlappingAndInBounds() {
  for (std::uint64_t seed = 1; seed <= 500; ++seed) {
    Rng rng(seed ^ 0x08DE8ull);
    const std::vector<std::string> lines = BuildDocument(rng);
    const TextBuffer buffer(lines);
    for (const std::string& query : QueryPool()) {
      for (const bool case_sensitive : {false, true}) {
        for (const bool whole_word : {false, true}) {
          BufferSearchOptions options{.case_sensitive = case_sensitive, .whole_word = whole_word};
          const auto matches = FindLiteralSearchMatches(lines, query, options);
          std::size_t previous_line = 0;
          std::size_t previous_column = 0;
          bool first = true;
          for (const SelectionRange& match : matches) {
            Expect(match.start.line < lines.size() && match.end.line < lines.size(),
                   "a match must name a line the document has" +
                       Describe(seed, query, options));
            Expect(match.end.column <= lines[match.end.line].size(),
                   "a match must end inside its line" + Describe(seed, query, options));
            Expect(match.start.line < match.end.line ||
                       (match.start.line == match.end.line &&
                        match.start.column < match.end.column),
                   "a literal match is never empty or inverted" +
                       Describe(seed, query, options));
            if (!first) {
              const bool after_previous =
                  match.start.line > previous_line ||
                  (match.start.line == previous_line && match.start.column >= previous_column);
              Expect(after_previous,
                     "matches must be sorted and non-overlapping" +
                         Describe(seed, query, options) + " " + Render(matches));
            }
            first = false;
            previous_line = match.end.line;
            previous_column = match.end.column;
          }
        }
      }
    }
  }
}

}  // namespace

void RegisterSearchDifferentialTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SearchDifferential/SnapshotAndBufferScannersAgree",
          TestSnapshotAndBufferScannersAgree);
  AddTest(tests, "SearchDifferential/IncrementalRefineEqualsAFreshScan",
          TestIncrementalRefineEqualsAFreshScan);
  AddTest(tests, "SearchDifferential/RegexOfAnEscapedLiteralFindsTheLiteralSpans",
          TestRegexOfAnEscapedLiteralFindsTheLiteralSpans);
  AddTest(tests, "SearchDifferential/MatchSetsAreOrderedNonOverlappingAndInBounds",
          TestMatchSetsAreOrderedNonOverlappingAndInBounds);
}

}  // namespace microide::tests
