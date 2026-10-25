#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "workspace/debug/DapProtocol.h"

namespace microide::workspace {

// The frame a stop shows and the editor opens: the topmost one whose source can
// be opened, as VS Code's getTopStackFrame picks it. A pause usually lands inside
// a library call (a sleep, a read) whose frames name sources that are not on this
// machine (`nptl/cancellation.c`); showing those left the editor where it was and
// the Call Stack pointing past the program. Frames the adapter marks `label` (a
// separator) or `subtle` (runtime internals) are skipped too. With no such frame,
// the top one, as before.
template <typename SourceAvailable>
std::size_t TopFrameWithSource(const std::vector<dap_protocol::DapStackFrame>& frames,
                               SourceAvailable&& available) {
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const dap_protocol::DapStackFrame& frame = frames[i];
    if (frame.presentation_hint == "label" || frame.presentation_hint == "subtle" ||
        frame.source.path.empty()) {
      continue;
    }
    if (available(frame.source.path)) {
      return i;
    }
  }
  return 0;
}

}  // namespace microide::workspace
