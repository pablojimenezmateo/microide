#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::workspace {

struct DebugStackFrameView {
  int id = 0;
  std::filesystem::path source_path;
  std::string source_path_normalized;
  std::size_t line = 0;
  std::string display_primary;
  std::string display_secondary;

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

struct DebugThreadView {
  int id = 0;
  std::string display;
};

struct DebugSessionView {
  int id = 0;
  std::string display;
  bool attention = false;
};

struct DebugExecutionView {
  bool stopped = false;
  int thread_id = 0;
  std::string stop_reason;
  std::vector<DebugStackFrameView> frames;
  std::size_t focused_frame_index = 0;
  std::vector<DebugThreadView> threads;
  int focused_thread_id = 0;
  std::vector<DebugSessionView> sessions;
  int focused_session_id = 0;

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

  bool HasLocation() const {
    const DebugStackFrameView* frame = FocusedFrame();
    return frame != nullptr && !frame->source_path.empty();
  }

  const std::filesystem::path& FocusedPath() const {
    static const std::filesystem::path kEmpty;
    const DebugStackFrameView* frame = FocusedFrame();
    return frame != nullptr ? frame->source_path : kEmpty;
  }

  const std::string& FocusedPathNormalized() const {
    static const std::string kEmpty;
    const DebugStackFrameView* frame = FocusedFrame();
    return frame != nullptr ? frame->source_path_normalized : kEmpty;
  }

  std::size_t FocusedLine() const {
    const DebugStackFrameView* frame = FocusedFrame();
    return frame != nullptr ? frame->line : 0;
  }

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
    std::size_t index = 0;
  };
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

struct DebugHoverModel {
  enum class Status { Empty, Pending, Resolved, Failed };
  enum class Lookup { Miss, Pending, Hit, Failed };

  int frame_id = 0;
  std::string expression;
  Status status = Status::Empty;
  std::string value;
  std::string type;
  std::uint64_t generation = 0;

  void Clear() {
    frame_id = 0;
    expression.clear();
    status = Status::Empty;
    value.clear();
    type.clear();
    ++generation;
  }

  std::uint64_t Begin(int frame, std::string expr) {
    frame_id = frame;
    expression = std::move(expr);
    status = Status::Pending;
    value.clear();
    type.clear();
    return ++generation;
  }

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

}
