#include "plugin/PluginHoverQueryInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <string>
#include <unordered_map>

#include "plugin/PluginLuaInterop.h"

namespace microide::plugin::hover_query_interop {

bool QueryHover(
    const std::vector<std::string>& hover_provider_order,
    const std::unordered_map<std::string, runtime_types::HoverProvider>& hovers,
    const std::filesystem::path& resolved_path,
    std::size_t line,
    std::size_t column,
    const std::function<bool(const runtime_types::HoverProvider&,
                             const std::filesystem::path&,
                             std::size_t,
                             std::size_t,
                             PluginHost::HoverResult*,
                             std::string*)>& query_hover_provider,
    PluginHost::HoverResult* result,
    std::string* error_message) {
  bool had_failure = false;
  for (const std::string& provider_id : hover_provider_order) {
    const auto it = hovers.find(provider_id);
    if (it == hovers.end()) {
      continue;
    }
    // Reset before each provider so a failed provider's partial write never leaks into
    // the next provider's result inspection.
    result->title.clear();
    result->content.clear();
    std::string provider_error;
    if (!query_hover_provider(it->second, resolved_path, line, column, result, &provider_error)) {
      // A broken first provider must not mask every later ordered fallback
      // (TD-2026-07-17A-049): record the failure and keep scanning. A provider that
      // returns nil (no hover here) succeeds with an empty result and is skipped below,
      // which is distinct from this error path.
      lua_interop::AppendProviderFailure(error_message, "hover", provider_id, provider_error);
      had_failure = true;
      continue;
    }
    if (!result->title.empty() || !result->content.empty()) {
      if (error_message != nullptr) {
        error_message->clear();  // a real hover supersedes earlier provider failures
      }
      return true;
    }
  }

  result->title.clear();
  result->content.clear();
  // Keep the accumulated failures (if any) so a caller with a non-null sink can log them;
  // clear only when every provider was healthy-but-empty, preserving the prior contract.
  if (!had_failure && error_message != nullptr) {
    error_message->clear();
  }
  return false;
}

}  // namespace microide::plugin::hover_query_interop

#endif
