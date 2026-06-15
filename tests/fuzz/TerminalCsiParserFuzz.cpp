#include "terminal/TerminalCsiParser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// Fuzz the hand-written CSI / SGR parameter tokenizers. These feed cursor moves,
// erase/scroll counts, and SGR colour selectors straight from untrusted PTY
// output, so they must never crash or invoke undefined behaviour (e.g. the old
// std::atoi overflow) on arbitrary bytes.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  const std::string_view body(reinterpret_cast<const char*>(data), size);

  const std::vector<int> params = microide::terminal::ParseCsiParameters(body);
  for (std::size_t i = 0; i < params.size(); ++i) {
    (void)microide::terminal::CsiParamOrDefault(params, i, 1);
  }
  (void)microide::terminal::ParseSgrParameters(body);
  return 0;
}
