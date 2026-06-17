#include "workspace/DebugPaneRegistry.h"

#include <algorithm>
#include <array>

namespace microide::workspace {

std::span<const DebugPaneSurfaceSpec> BuiltinDebugPaneSurfaceSpecs() {
  static const auto kSpecs = std::to_array<DebugPaneSurfaceSpec>({
      DebugPaneSurfaceSpec{"callstack", "Call Stack", DebugPaneMode::CallStack},
      DebugPaneSurfaceSpec{"variables", "Variables", DebugPaneMode::Variables},
      DebugPaneSurfaceSpec{"watch", "Watch", DebugPaneMode::Watch},
      DebugPaneSurfaceSpec{"breakpoints", "Breakpoints", DebugPaneMode::Breakpoints},
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
