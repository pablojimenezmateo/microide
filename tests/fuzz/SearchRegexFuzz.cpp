#include "util/RegexUtil.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  const std::size_t split = size / 2;
  const std::string_view pattern = bytes.substr(0, split);
  const std::string_view text = bytes.substr(split);
  microide::util::CompiledRegex regex(pattern, PCRE2_UTF, "fuzz regex");
  if (!regex.valid()) {
    return 0;
  }
  auto match_data = regex.CreateMatchData();
  if (!match_data.valid()) {
    return 0;
  }
  (void)regex.Match(text, 0, match_data);
  microide::util::RegexMatchRange range;
  (void)regex.CaptureRange(match_data, text.size(), &range);
  return 0;
}
