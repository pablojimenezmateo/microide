#include "terminal/TerminalSession.h"

#include "TerminalSessionTestAccess.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// Drive the full terminal output state machine (`AppendOutputLocked`) on
// arbitrary PTY bytes. Unlike TerminalCsiParserFuzz, which only exercises the
// CSI/SGR tokenizers in isolation, this target feeds raw output through the
// complete T1/T2 pipeline: the escape-mode state machine, UTF-8 decoder, glyph
// placement, scrollback management, alternate-screen swaps, and all escape /
// OSC / private-mode handlers. None of these may crash or invoke undefined
// behaviour on untrusted input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }

  microide::terminal::TerminalSession session;
  microide::tests::TerminalSessionTestAccess::Reset(session, 24, 80);

  const std::string_view output(reinterpret_cast<const char*>(data), size);
  microide::tests::TerminalSessionTestAccess::AppendOutput(session, output);

  // Exercise the read/snapshot path too, so grid invariants established during
  // parsing are observed by a consumer the way the render thread would.
  (void)session.SnapshotLines();

  return 0;
}
