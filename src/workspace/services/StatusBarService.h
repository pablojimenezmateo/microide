#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <string>
#include <string_view>

#include "workspace/StatusTone.h"
#include "workspace/registries/MenuId.h"

namespace microide::workspace {

enum class StatusBarSegmentId : std::uint8_t {
  Project = 0,
  Branch,
  Worktrees,
  Language,
  Indent,
  Encoding,
  LineEnding,
  LineColumn,
  Problems,
  Lsp,
  LayoutMode,
  Remote,
  Count,
};

constexpr std::string_view StatusBarSegmentName(StatusBarSegmentId id) {
  switch (id) {
    case StatusBarSegmentId::Project: return "project";
    case StatusBarSegmentId::Branch: return "branch";
    case StatusBarSegmentId::Worktrees: return "worktrees";
    case StatusBarSegmentId::Language: return "language";
    case StatusBarSegmentId::Indent: return "indent";
    case StatusBarSegmentId::Encoding: return "encoding";
    case StatusBarSegmentId::LineEnding: return "line-ending";
    case StatusBarSegmentId::LineColumn: return "line-column";
    case StatusBarSegmentId::Problems: return "problems";
    case StatusBarSegmentId::Lsp: return "lsp";
    case StatusBarSegmentId::LayoutMode: return "layout-mode";
    case StatusBarSegmentId::Remote: return "remote";
    case StatusBarSegmentId::Count: break;
  }
  return "unknown";
}

struct StatusBarSegmentValue {
  std::string text;
  std::string tooltip;
  std::string command;
  std::string command_arg;
  MenuId menu = MenuId::None;
  bool visible = false;
  StatusTone tone = StatusTone::Default;

  bool clickable() const { return !command.empty() || menu != MenuId::None; }
};

class StatusBarService {
 public:
  StatusBarService() = default;

  bool SetSegment(StatusBarSegmentId id, StatusBarSegmentValue value) {
    StatusBarSegmentValue& slot = segments_[static_cast<std::size_t>(id)];
    const bool painted = slot.text != value.text || slot.visible != value.visible || slot.tone != value.tone;
    slot = std::move(value);
    return painted;
  }

  struct StatusBarSegmentUpdate {
    std::string_view text;
    std::string_view tooltip;
    std::string_view command;
    std::string_view command_arg;
    MenuId menu = MenuId::None;
    bool visible = false;
    StatusTone tone = StatusTone::Default;
  };

  void SetSegment(StatusBarSegmentId id, const StatusBarSegmentUpdate& update) {
    StatusBarSegmentValue& slot = segments_[static_cast<std::size_t>(id)];
    slot.text.assign(update.text);
    slot.tooltip.assign(update.tooltip);
    slot.command.assign(update.command);
    slot.command_arg.assign(update.command_arg);
    slot.menu = update.menu;
    slot.visible = update.visible;
    slot.tone = update.tone;
  }
  void ClearSegment(StatusBarSegmentId id) { SetSegment(id, StatusBarSegmentUpdate{}); }
  const StatusBarSegmentValue& Segment(StatusBarSegmentId id) const {
    return segments_[static_cast<std::size_t>(id)];
  }

  const std::array<StatusBarSegmentValue, static_cast<std::size_t>(StatusBarSegmentId::Count)>&
  Snapshot() const {
    return segments_;
  }

 private:
  std::array<StatusBarSegmentValue, static_cast<std::size_t>(StatusBarSegmentId::Count)>
      segments_{};
};

}
