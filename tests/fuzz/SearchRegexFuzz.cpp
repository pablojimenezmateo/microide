#include "util/RegexUtil.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

  microide::util::CompiledRegex regex(pattern, options, "fuzz regex",
                                      microide::util::AsciiTwin::Build);
  if (!regex.valid()) {
    return 0;
  }
  auto match_data = regex.CreateMatchData();
  if (!match_data.valid()) {
    return 0;
  }

  (void)regex.Match(subject, 0, match_data);
  microide::util::RegexMatchRange range;
  (void)regex.CaptureRange(match_data, subject.size(), &range);

  const auto collect = [&](microide::util::RegexSubject kind) {
    std::vector<std::pair<std::size_t, std::size_t>> found;
    std::size_t search_from = 0;
    std::size_t match_start = 0;
    std::size_t match_end = 0;
    for (int iterations = 0; iterations < 4096; ++iterations) {
      if (!microide::util::FindNextRegexMatchInLine(regex, subject, &search_from, &match_data,
                                                    &match_start, &match_end, nullptr, kind)) {
        break;
      }
      found.emplace_back(match_start, match_end);
    }
    return found;
  };
  const auto utf_matches = collect(microide::util::RegexSubject::Unknown);
  if (regex.has_ascii_twin() && microide::util::IsAllAscii(subject) &&
      collect(microide::util::RegexSubject::Ascii) != utf_matches) {
    __builtin_trap();
  }

  std::string substituted;
  (void)regex.SubstituteInto(subject, replacement, substituted);
  (void)regex.ExpandMatchAt(subject, 0, replacement);

  return 0;
}
