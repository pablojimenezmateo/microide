#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <string>

namespace microide::workspace {

enum class StatusBarSegmentId : std::uint8_t {
  Project = 0,
  Branch,
  Language,
  Indent,
  Encoding,
  LineColumn,
  Problems,
  Lsp,
  AiProvider,
  LayoutMode,
  Count,
};

struct StatusBarSegmentValue {
  std::string text;
  std::string tooltip;
  bool clickable = false;
  bool visible = false;
};

class StatusBarService {
 public:
  StatusBarService() = default;

  void SetSegment(StatusBarSegmentId id, StatusBarSegmentValue value) {
    segments_[static_cast<std::size_t>(id)] = std::move(value);
  }
  const StatusBarSegmentValue& Segment(StatusBarSegmentId id) const {
    return segments_[static_cast<std::size_t>(id)];
  }

  std::array<StatusBarSegmentValue, static_cast<std::size_t>(StatusBarSegmentId::Count)>
  Snapshot() const {
    return segments_;
  }

 private:
  std::array<StatusBarSegmentValue, static_cast<std::size_t>(StatusBarSegmentId::Count)>
      segments_{};
};

}  // namespace microide::workspace
