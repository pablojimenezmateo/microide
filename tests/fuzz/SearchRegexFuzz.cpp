#include "util/RegexUtil.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Fuzzes the SEARCH regex surface as the product actually uses it.
//
// The original harness compiled with a hardcoded PCRE2_UTF and called Match()
// exactly once, which left the interesting code unfuzzed:
//
//   * util::SearchRegexCompileOptions — picks the option set from the query, and
//     turns UTF/UCP/MATCH_INVALID_UTF on only for a case-insensitive non-ASCII
//     query. That branch changes how PCRE2 treats malformed subject bytes, which
//     is exactly the property a fuzzer should be attacking.
//   * util::FindNextRegexMatchInLine — the shared engine both project-wide and
//     in-file search iterate with. It carries the subtle logic: the empty-match
//     advance, the anchored retry that recovers a non-empty alternative at the
//     same offset, and the UTF-safe byte advance.
//   * SubstituteInto / ExpandMatchAt — the replace-all and replace-one paths,
//     including PCRE2's extended replacement syntax (`$1`, `\U`, `${n:-default}`).
//
// Input layout: first byte selects the case sensitivity the compile options are
// derived from; the remainder is split into pattern / subject / replacement.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size < 2) {
    return 0;
  }
  const std::uint8_t control = data[0];
  const std::string_view bytes(reinterpret_cast<const char*>(data) + 1, size - 1);

  const std::size_t third = bytes.size() / 3;
  const std::string_view pattern = bytes.substr(0, third);
  const std::string_view subject = bytes.substr(third, third);
  const std::string_view replacement = bytes.substr(2 * third);

  const bool case_sensitive = (control & 1u) != 0;
  const std::uint32_t options =
      microide::util::SearchRegexCompileOptions(pattern, case_sensitive);

  microide::util::CompiledRegex regex(pattern, options, "fuzz regex");
  if (!regex.valid()) {
    return 0;
  }
  auto match_data = regex.CreateMatchData();
  if (!match_data.valid()) {
    return 0;
  }

  // Single-shot match + capture extraction (the original coverage).
  (void)regex.Match(subject, 0, match_data);
  microide::util::RegexMatchRange range;
  (void)regex.CaptureRange(match_data, subject.size(), &range);

  // Drive the shared engine to exhaustion, exactly as a search over one line
  // does. The iteration guard bounds a pathological pattern's run time; the
  // engine itself is required to always advance, so hitting the guard on a small
  // input would itself be a finding.
  std::size_t search_from = 0;
  std::size_t match_start = 0;
  std::size_t match_end = 0;
  for (int iterations = 0; iterations < 4096; ++iterations) {
    if (!microide::util::FindNextRegexMatchInLine(regex, subject, &search_from, &match_data,
                                                  &match_start, &match_end)) {
      break;
    }
  }

  // Replace-all and replace-one, with the fuzzer-supplied replacement driving
  // PCRE2's extended substitution syntax.
  std::string substituted;
  (void)regex.SubstituteInto(subject, replacement, substituted);
  (void)regex.ExpandMatchAt(subject, 0, replacement);

  return 0;
}
