#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::workspace {

// One call-stack frame, host-side and adapter-agnostic. Display strings are
// prebuilt by DebugService when a `stopped` resolves, so render TUs (the Call
// Stack panel, the editor execution-line) never materialize strings in the hot
// path. `source_path` is normalized; empty when the frame carries no source.
struct DebugStackFrameView {
  int id = 0;                         // DAP frame id (used by scopes/evaluate in Phase 4/5)
  std::filesystem::path source_path;  // normalized; empty when the frame has no source
  std::size_t line = 0;               // 0-based buffer line (DAP line - 1)
  std::string display_primary;        // e.g. "main"        (prebuilt)
  std::string display_secondary;      // e.g. "main.c:42"   (prebuilt, muted)
};

// Transient execution state for the active debug session: the current stop, its
// call stack, and which frame the user is focused on. Lives on
// ProjectWorkspaceState (mirrors `breakpoint_store` / `diagnostics_store`) but is
// never persisted — it is rebuilt on every `stopped` and cleared on resume/stop.
struct DebugExecutionView {
  bool stopped = false;
  int thread_id = 0;
  std::string stop_reason;  // breakpoint / step / pause / exception / ...
  std::vector<DebugStackFrameView> frames;
  std::size_t focused_frame_index = 0;

  void Clear() {
    stopped = false;
    thread_id = 0;
    stop_reason.clear();
    frames.clear();
    focused_frame_index = 0;
  }

  const DebugStackFrameView* FocusedFrame() const {
    if (!stopped || focused_frame_index >= frames.size()) {
      return nullptr;
    }
    return &frames[focused_frame_index];
  }

  // True when the focused frame has a source location to highlight/navigate to.
  bool HasLocation() const {
    const DebugStackFrameView* frame = FocusedFrame();
    return frame != nullptr && !frame->source_path.empty();
  }

  const std::filesystem::path& FocusedPath() const {
    static const std::filesystem::path kEmpty;
    const DebugStackFrameView* frame = FocusedFrame();
    return frame != nullptr ? frame->source_path : kEmpty;
  }

  std::size_t FocusedLine() const {
    const DebugStackFrameView* frame = FocusedFrame();
    return frame != nullptr ? frame->line : 0;
  }
};

}  // namespace microide::workspace
