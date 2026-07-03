// Fuzzes the JSON parser that backs every control-channel sink (--control-spec
// files, socket request lines, instance descriptor files) plus DAP/LSP message
// decoding. The parser must never crash on arbitrary bytes — in particular the
// recursion depth guard must keep a deeply nested payload from overflowing the
// stack. When a payload parses, we serialize it back to exercise the writer's
// matching depth bound too.
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
