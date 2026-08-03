#pragma once

#include <cmath>
#include <optional>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::app {

inline bool CanReuseCachedPresentationState(
    bool presentation_state_dirty,
    const std::optional<workspace::WorkspaceShell::WindowPresentationState>& cached_presentation,
    float last_presented_ui_scale,
    float current_ui_scale) {
  if (presentation_state_dirty || !cached_presentation.has_value()) {
    return false;
  }
  if (!std::isfinite(current_ui_scale) || current_ui_scale <= 0.0f) {
    return false;
  }
  return std::fabs(last_presented_ui_scale - current_ui_scale) < 0.001f;
}

}  // namespace microide::app
