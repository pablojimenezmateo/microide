#pragma once

#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

class LayoutModeService {
 public:
  LayoutModeService() = default;

  LayoutMode CurrentMode() const { return current_mode_; }
  void SetCurrentMode(LayoutMode mode) { current_mode_ = mode; }

  LayoutModeInputs::Override UserOverride() const { return user_override_; }
  void SetUserOverride(LayoutModeInputs::Override override) { user_override_ = override; }

  float CompactBreakpointPx() const { return compact_breakpoint_px_; }
  void SetCompactBreakpointPx(float px) { compact_breakpoint_px_ = px; }

  bool StatusBarVisible() const { return status_bar_visible_; }
  void SetStatusBarVisible(bool visible) { status_bar_visible_ = visible; }

  LayoutModeInputs SnapshotInputs() const {
    return LayoutModeInputs{
        .user_override = user_override_,
        .compact_breakpoint_px = compact_breakpoint_px_,
        .previous_mode = current_mode_,
    };
  }

 private:
  LayoutMode current_mode_ = LayoutMode::Regular;
  LayoutModeInputs::Override user_override_ = LayoutModeInputs::Override::Auto;
  float compact_breakpoint_px_ = kWorkspaceLayoutCompactBreakpointDefault;
  bool status_bar_visible_ = false;
};

}  // namespace microide::workspace
