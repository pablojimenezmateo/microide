#pragma once

#include "architecture/ArchitectureFileScanner.h"

#include <cstddef>
#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests::architecture {

struct Violation {
  std::filesystem::path path;
  std::size_t line = 0;
  std::string message;
};

struct RuleResult {
  std::string label;
  bool hard_fail = false;
  std::vector<Violation> violations;
  // Files the rule expected to scan but did not find. Kept apart from
  // `violations` because the two answer different questions: a violation means
  // the code broke the rule, a missing target means the RULE broke — it now
  // scans nothing and would report green forever. The real-repo run fails on
  // either; the synthetic-root fixtures assert on `violations` alone, since they
  // deliberately materialize only the file under test.
  std::vector<Violation> missing_targets;
};

// A single architecture rule paired with a stable name. Exposing rule groups as
// lists of NamedRule lets the test layer register one ctest case per rule (so
// sharding runs them in parallel) while the aggregating Run*ArchitectureRules
// functions iterate the same list — one source of truth, no drift.
using ArchitectureRuleFn = RuleResult (*)(const std::filesystem::path&);
struct NamedRule {
  std::string_view name;
  ArchitectureRuleFn fn;
};

void ReportRule(const RuleResult& result);
void AppendViolations(RuleResult& result,
                      const std::filesystem::path& path,
                      const std::string& text,
                      const std::regex& pattern,
                      std::string_view message);
void AppendCodeMaskRegexViolations(RuleResult& result,
                                   const std::filesystem::path& path,
                                   const std::string& text,
                                   const std::regex& pattern,
                                   std::string_view message);
// Like AppendCodeMaskRegexViolations, but only requires the LAST character of
// the match to be code. Use for patterns that intentionally anchor on a
// string-literal quote (e.g. detecting `"prefix" + ident` concatenations):
// BuildCodeMask marks the opening quote of every string literal as non-code, so
// the all-characters-in-code predicate in AppendCodeMaskRegexViolations can
// never match a quote-anchored pattern. The trailing identifier is real code
// iff the construct is real source -- a match inside a comment or string
// literal ends on a masked character and is correctly skipped.
void AppendTrailingCodeRegexViolations(RuleResult& result,
                                       const std::filesystem::path& path,
                                       const std::string& text,
                                       const std::regex& pattern,
                                       std::string_view message);
RuleResult CheckShellFileSize(const std::filesystem::path& repo_root,
                              std::string_view relative_path,
                              std::size_t limit);

// Records a missing target when a rule's file is not on disk, and returns whether
// it is. Every rule below scans named files, so a rule that simply returns (or
// `continue`s past) a missing target keeps passing forever after the file is
// renamed or split — the rule is structurally dead but reports green, which is
// how several rules in this suite went silently blind. Gate every target read on
// this instead.
bool RequireRuleTarget(RuleResult& result, const std::filesystem::path& path);

}  // namespace microide::tests::architecture
