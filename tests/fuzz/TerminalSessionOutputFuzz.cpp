#include "terminal/TerminalSession.h"

#include "TerminalSessionTestAccess.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }

  microide::terminal::TerminalSession session;
  microide::tests::TerminalSessionTestAccess::Reset(session, 24, 80);

  const std::string_view output(reinterpret_cast<const char*>(data), size);
  microide::tests::TerminalSessionTestAccess::AppendOutput(session, output);

  (void)session.SnapshotLines();

  return 0;
}
