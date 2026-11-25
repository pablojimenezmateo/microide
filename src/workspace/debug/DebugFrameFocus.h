#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "workspace/debug/DapProtocol.h"

namespace microide::workspace {

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

}
