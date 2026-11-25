#include "util/JsonValue.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto parsed = microide::util::ParseJson(text);
  if (parsed.has_value()) {
    volatile std::size_t sink = microide::util::SerializeJson(*parsed).size();
    (void)sink;
  }
  return 0;
}
