#include "TestSupport.h"

#include <cstdint>
#include <optional>
#include <utility>
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
  std::string scope = "none";
  if (o.scope.has_value()) {
    scope = std::to_string(o.scope->start.line) + ":" + std::to_string(o.scope->start.column) + "-" +
            std::to_string(o.scope->end.line) + ":" + std::to_string(o.scope->end.column);
  }
  return " [seed=" + std::to_string(seed) + " query='" + std::string(query) +
         "' case_sensitive=" + (o.case_sensitive ? "1" : "0") +
         " whole_word=" + (o.whole_word ? "1" : "0") + " scope=" + scope + "]";
}

std::vector<std::optional<SelectionRange>> Scopes(Rng& rng, const std::vector<std::string>& lines) {
  const auto position = [&]() {
    const std::size_t line = rng.Below(lines.size());
    return microide::editor::TextPosition{line, rng.Below(lines[line].size() + 1)};
  };
  auto a = position();
  auto b = position();
  if (microide::editor::PositionBefore(b, a)) {
    std::swap(a, b);
  }
  return {std::nullopt, SelectionRange{a, b}};
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

void TestSnapshotAndBufferScannersAgree() {
  std::size_t total_matches = 0;
  for (std::uint64_t seed = 1; seed <= 500; ++seed) {
    Rng rng(seed);
    const std::vector<std::string> lines = BuildDocument(rng);
    const TextBuffer buffer(lines);
    for (const std::string& query : QueryPool()) {
      for (const bool case_sensitive : {false, true}) {
        for (const bool whole_word : {false, true}) {
          for (const auto& scope : Scopes(rng, lines)) {
            BufferSearchOptions options{.case_sensitive = case_sensitive, .whole_word = whole_word, .scope = scope};
            const auto from_lines = FindLiteralSearchMatches(lines, query, options);
            const auto from_buffer = FindLiteralSearchMatches(buffer, query, options);
            total_matches += from_lines.size();
            Expect(Same(from_lines, from_buffer),
                   "snapshot and buffer scanners must agree" + Describe(seed, query, options) +
                       "\n  lines:  " + Render(from_lines) + "\n  buffer: " + Render(from_buffer));
            for (const SelectionRange& match : from_buffer) {
              Expect(!scope.has_value() || microide::editor::RangeContains(*scope, match),
                     "a scoped match lies inside the scope" + Describe(seed, query, options));
            }
          }
        }
      }
    }
  }
  Expect(total_matches > 5000,
         "the corpus must actually produce matches, saw " + std::to_string(total_matches));
}

void TestIncrementalRefineEqualsAFreshScan() {
  for (std::uint64_t seed = 1; seed <= 500; ++seed) {
    Rng rng(seed ^ 0x8EF19Eull);
    const std::vector<std::string> lines = BuildDocument(rng);
    const TextBuffer buffer(lines);
    for (const std::string& query : QueryPool()) {
      for (const bool whole_word : {false, true}) {
       for (const auto& scope : Scopes(rng, lines)) {
        BufferSearchOptions options{.case_sensitive = false, .whole_word = whole_word, .scope = scope};
        std::vector<SelectionRange> previous;
        bool have_previous = false;
        for (std::size_t length = 1; length <= query.size(); ++length) {
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
}

void TestRegexOfAnEscapedLiteralFindsTheLiteralSpans() {
  std::size_t compared = 0;
  for (std::uint64_t seed = 1; seed <= 400; ++seed) {
    Rng rng(seed ^ 0xC0DEull);
    const std::vector<std::string> lines = BuildDocument(rng);
    const TextBuffer buffer(lines);
    for (const std::string& query : QueryPool()) {
      for (const bool case_sensitive : {false, true}) {
        for (const auto& scope : Scopes(rng, lines)) {
          BufferSearchOptions options{.case_sensitive = case_sensitive, .whole_word = false, .scope = scope};
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
  }
  Expect(compared > 1000, "every pattern in the pool should have compiled");
}

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

}

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

}
