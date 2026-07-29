#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::workspace {

// One call-stack frame, host-side and adapter-agnostic. Display strings are
// prebuilt by DebugService when a `stopped` resolves, so render TUs (the Call
// Stack panel, the editor execution-line) never materialize strings in the hot
// path. `source_path` is normalized; empty when the frame carries no source.
struct DebugStackFrameView {
  int id = 0;                         // DAP frame id (used by scopes/evaluate in Phase 4/5)
  std::filesystem::path source_path;  // normalized; empty when the frame has no source
  // `source_path` as a normalized generic string, cached at frame-build time so the
  // per-pane execution-line marker and debug-value hover path-match do not
  // re-normalize and re-allocate it for every editor pane each frame. Empty when
  // the frame has no source.
  std::string source_path_normalized;
  std::size_t line = 0;               // 0-based buffer line (DAP line - 1)
  std::string display_primary;        // e.g. "main"        (prebuilt)
  std::string display_secondary;      // e.g. "main.c:42"   (prebuilt, muted)

  // Set `source_path` (normalizing) and its cached generic-string form together so
  // the two never desync. Takes the raw DAP source path; empty input clears both.
  void SetSource(const std::string& raw_path) {
    if (raw_path.empty()) {
      source_path.clear();
      source_path_normalized.clear();
      return;
    }
    source_path = std::filesystem::path(raw_path).lexically_normal();
    source_path_normalized = source_path.generic_string();
  }
};

// One entry in the thread selector (Phase 7 multi-thread). `display` is prebuilt
// (e.g. "Thread 1: MainThread") so the Call Stack render TU only draws.
struct DebugThreadView {
  int id = 0;
  std::string display;
};

// One entry in the session selector (Phase 8 multi-session). `display` is
// prebuilt (e.g. "server (paused)") so the Call Stack render TU only draws.
// `attention` marks a background session that paused but has not yet been viewed
// (rendered with an accent). Sourced from DapManager — survives resume so the
// switcher stays available while the active session runs.
struct DebugSessionView {
  int id = 0;
  std::string display;
  bool attention = false;
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
  // Thread selector (Phase 7 multi-thread). Populated a beat after the stop via
  // a `threads` request; `focused_thread_id` is the thread whose frames `frames`
  // currently holds. A single-thread target leaves `threads` size <= 1 (no
  // selector is shown). Preserved across a thread switch (frames are rebuilt).
  std::vector<DebugThreadView> threads;
  int focused_thread_id = 0;
  // Session selector (Phase 8 multi-session). Populated from DapManager whenever
  // the live-session set / active id / attention changes. Unlike the stop-scoped
  // fields below, these are NOT cleared on resume/stop (Clear() preserves them):
  // the switcher must stay available so the user can switch to a paused
  // background session while the active one runs.
  std::vector<DebugSessionView> sessions;
  int focused_session_id = 0;

  // Clear only the stop-scoped state (frames/threads/reason). The session
  // selector (`sessions`/`focused_session_id`) is owned by DapManager's
  // projection and intentionally survives so it stays visible while running.
  void Clear() {
    stopped = false;
    thread_id = 0;
    stop_reason.clear();
    frames.clear();
    focused_frame_index = 0;
    threads.clear();
    focused_thread_id = 0;
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

  // Cached normalized generic-string form of FocusedPath(), for per-pane path
  // comparison without re-normalizing and allocating on every editor pane.
  // Empty when the focused frame has no source.
  const std::string& FocusedPathNormalized() const {
    static const std::string kEmpty;
    const DebugStackFrameView* frame = FocusedFrame();
    return frame != nullptr ? frame->source_path_normalized : kEmpty;
  }

  std::size_t FocusedLine() const {
    const DebugStackFrameView* frame = FocusedFrame();
    return frame != nullptr ? frame->line : 0;
  }

  // The Call Stack panel lays out optional selectors above the frame list:
  // session rows (Phase 8), then thread rows (Phase 7), then frames. Each
  // selector shows only when it has more than one entry; with a single
  // session+thread the panel is byte-for-byte its pre-Phase-7 look. Render,
  // mouse, and scroll all derive their row geometry from these helpers so the
  // optional leading rows never desynchronize.
  bool HasSessionSelector() const { return sessions.size() > 1; }
  std::size_t SessionRowCount() const { return HasSessionSelector() ? sessions.size() : 0; }
  bool HasThreadSelector() const { return threads.size() > 1; }
  std::size_t ThreadRowCount() const { return HasThreadSelector() ? threads.size() : 0; }
  std::size_t PanelRowCount() const {
    return SessionRowCount() + ThreadRowCount() + frames.size();
  }

  struct PanelRowRef {
    enum class Kind { Session, Thread, Frame };
    Kind kind = Kind::Frame;
    std::size_t index = 0;  // into `sessions` / `threads` / `frames` per kind
  };
  // Map a flat panel row to a session, thread, or frame. Rows are laid out
  // [sessions)[threads)[frames) in that order.
  PanelRowRef PanelRowAt(std::size_t row) const {
    const std::size_t session_rows = SessionRowCount();
    if (row < session_rows) {
      return PanelRowRef{PanelRowRef::Kind::Session, row};
    }
    row -= session_rows;
    const std::size_t thread_rows = ThreadRowCount();
    if (row < thread_rows) {
      return PanelRowRef{PanelRowRef::Kind::Thread, row};
    }
    return PanelRowRef{PanelRowRef::Kind::Frame, row - thread_rows};
  }
  // Inverse of PanelRowAt for whatever is currently focused: the frame row when a
  // frame is focused, else the focused thread's or session's row. Keyboard
  // navigation walks the panel rows, so it needs to know which one the focus is
  // already sitting on; deriving it here keeps the row layout in one place.
  std::size_t FocusedPanelRow() const {
    const std::size_t session_rows = SessionRowCount();
    const std::size_t thread_rows = ThreadRowCount();
    if (!frames.empty()) {
      return session_rows + thread_rows + std::min(focused_frame_index, frames.size() - 1);
    }
    if (thread_rows > 0) {
      for (std::size_t i = 0; i < threads.size(); ++i) {
        if (threads[i].id == focused_thread_id) {
          return session_rows + i;
        }
      }
      return session_rows;
    }
    for (std::size_t i = 0; i < sessions.size(); ++i) {
      if (sessions[i].id == focused_session_id) {
        return i;
      }
    }
    return 0;
  }
};

// Transient hover-to-inspect cache (Phase 5). Holds the single in-flight / most
// recent `evaluate(context:"hover")` query, keyed by (frame_id, expression). The
// editor hover pipeline is synchronous but `evaluate` is async, so a cache *hit*
// is served immediately by the const hover resolver while a *miss* kicks off the
// async request and a later redraw re-resolves into a hit. Lives on
// ProjectWorkspaceState next to `debug_execution`; never persisted — cleared on
// resume/stop and on a focused-frame switch. `generation` is bumped on every
// Begin/Clear so a completion that lands after a frame switch or resume is dropped.
struct DebugHoverModel {
  enum class Status { Empty, Pending, Resolved, Failed };
  enum class Lookup { Miss, Pending, Hit, Failed };

  int frame_id = 0;
  std::string expression;
  Status status = Status::Empty;
  std::string value;  // prebuilt for display (DapEvaluateResult.result)
  std::string type;   //                       (DapEvaluateResult.type)
  std::uint64_t generation = 0;

  void Clear() {
    frame_id = 0;
    expression.clear();
    status = Status::Empty;
    value.clear();
    type.clear();
    ++generation;
  }

  // Begin a new query: marks Pending, bumps generation, returns the new generation
  // so the async callback can detect a stale completion.
  std::uint64_t Begin(int frame, std::string expr) {
    frame_id = frame;
    expression = std::move(expr);
    status = Status::Pending;
    value.clear();
    type.clear();
    return ++generation;
  }

  // Classify a query the resolver is about to serve against the cached one.
  Lookup Classify(int frame, std::string_view expr) const {
    if (status == Status::Empty || frame != frame_id || expr != expression) {
      return Lookup::Miss;
    }
    switch (status) {
      case Status::Pending:
        return Lookup::Pending;
      case Status::Resolved:
        return Lookup::Hit;
      case Status::Failed:
        return Lookup::Failed;
      case Status::Empty:
        break;
    }
    return Lookup::Miss;
  }

  void Resolve(std::uint64_t gen, std::string resolved_value, std::string resolved_type) {
    if (gen != generation || status != Status::Pending) {
      return;
    }
    value = std::move(resolved_value);
    type = std::move(resolved_type);
    status = Status::Resolved;
  }

  void Fail(std::uint64_t gen) {
    if (gen != generation || status != Status::Pending) {
      return;
    }
    status = Status::Failed;
  }
};

}  // namespace microide::workspace
