#include "plugin/PluginProjectLifecycleInterop.h"

namespace microide::plugin::project_lifecycle_interop {

#if MICROIDE_HAS_LUA_PLUGINS
void DispatchProjectOpenCallbacks(
    std::vector<runtime_types::PluginInstance>* plugins,
    const std::function<void(runtime_types::PluginInstance*, int, const char*)>&
        call_project_callback) {
  for (auto& plugin : *plugins) {
    call_project_callback(&plugin, plugin.on_project_open_ref, "on_project_open");
  }
}
#endif

}  // namespace microide::plugin::project_lifecycle_interop
