#include "workspace/debug/DebugPaneRegistry.h"

#include <algorithm>
#include <array>

namespace microide::workspace {

std::span<const DebugPaneSurfaceSpec> BuiltinDebugPaneSurfaceSpecs() {
  // Display order: Variables first (the most-used surface while stepping), then
  // Breakpoints, Watch, Call Stack. The order is purely presentational; the
  // DebugPaneMode enum and the Ctrl+Shift+1..4 shortcuts map by mode, not index.
  static const auto kSpecs = std::to_array<DebugPaneSurfaceSpec>({
      DebugPaneSurfaceSpec{"variables", "Variables", DebugPaneMode::Variables},
      DebugPaneSurfaceSpec{"breakpoints", "Breakpoints", DebugPaneMode::Breakpoints},
      DebugPaneSurfaceSpec{"watch", "Watch", DebugPaneMode::Watch},
      DebugPaneSurfaceSpec{"callstack", "Call Stack", DebugPaneMode::CallStack},
  });
  return kSpecs;
}

const DebugPaneSurfaceSpec* FindDebugPaneSurface(std::string_view id) {
  const auto specs = BuiltinDebugPaneSurfaceSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [id](const DebugPaneSurfaceSpec& spec) { return spec.id == id; });
  return it == specs.end() ? nullptr : &(*it);
}

const DebugPaneSurfaceSpec* FindDebugPaneSurface(DebugPaneMode mode) {
  const auto specs = BuiltinDebugPaneSurfaceSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [mode](const DebugPaneSurfaceSpec& spec) { return spec.mode == mode; });
  return it == specs.end() ? nullptr : &(*it);
}

}  // namespace microide::workspace
