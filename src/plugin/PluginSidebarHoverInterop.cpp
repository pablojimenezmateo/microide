#include "plugin/PluginSidebarHoverInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/PluginLuaInterop.h"

namespace microide::plugin::sidebar_hover_interop {
namespace {

bool ReadHoverResultTable(lua_State* state,
                          int table_index,
                          PluginHost::HoverResult* result,
                          std::string_view provider_id,
                          std::string* error_message) {
  if (result == nullptr) {
    if (error_message != nullptr) {
      *error_message = "hover result output pointer is required";
    }
    return false;
  }

  const int absolute_index = lua_absindex(state, table_index);
  result->title.clear();
  result->content.clear();

  lua_getfield(state, absolute_index, "title");
  if (lua_isstring(state, -1)) {
    result->title = lua_tostring(state, -1);
  } else if (!lua_isnil(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "plugin hover '" + std::string(provider_id) +
                       "' title must be a string when present";
    }
    lua_pop(state, 1);
    return false;
  }
  lua_pop(state, 1);

  lua_getfield(state, absolute_index, "content");
  if (lua_isstring(state, -1)) {
    result->content = lua_tostring(state, -1);
  } else if (!lua_isnil(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "plugin hover '" + std::string(provider_id) +
                       "' content must be a string when present";
    }
    lua_pop(state, 1);
    return false;
  }
  lua_pop(state, 1);

  if (result->title.empty() && result->content.empty()) {
    if (error_message != nullptr) {
      *error_message = "plugin hover '" + std::string(provider_id) +
                       "' must return a title or content";
    }
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

void PushSidebarItemTable(lua_State* state,
                          const PluginHost::SidebarItem& item,
                          const std::filesystem::path& current_project_root,
                          const std::function<std::filesystem::path(const std::filesystem::path&,
                                                                    const std::filesystem::path&)>&
                              resolve_runtime_path) {
  lua_createtable(state, 0, 5);
  lua_pushstring(state, item.label.c_str());
  lua_setfield(state, -2, "label");
  if (!item.detail.empty()) {
    lua_pushstring(state, item.detail.c_str());
    lua_setfield(state, -2, "detail");
  }
  if (!item.path.empty()) {
    const std::filesystem::path resolved_path = resolve_runtime_path(current_project_root, item.path);
    lua_pushstring(state, resolved_path.generic_string().c_str());
    lua_setfield(state, -2, "path");
  }
  if (item.line > 0) {
    lua_pushinteger(state, static_cast<lua_Integer>(item.line));
    lua_setfield(state, -2, "line");
  }
  if (item.column > 0) {
    lua_pushinteger(state, static_cast<lua_Integer>(item.column));
    lua_setfield(state, -2, "column");
  }
}

}  // namespace

bool SnapshotSidebarProvider(
    const runtime_types::SidebarProvider& provider,
    const std::filesystem::path& current_project_root,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>& resolve_runtime_path,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    std::vector<PluginHost::SidebarItem>* items,
    std::string* error_message) {
  if (provider.state == nullptr || provider.snapshot_ref == LUA_NOREF || items == nullptr) {
    if (error_message != nullptr) {
      *error_message = "plugin sidebar is unavailable";
    }
    return false;
  }

  items->clear();
  const lua_interop::StackResetGuard stack_guard(provider.state);
  lua_rawgeti(provider.state, LUA_REGISTRYINDEX, provider.snapshot_ref);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(provider.state);
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime ||
      !plugin->runtime->PCall(0, 1, &call_error)) {
    if (error_message != nullptr) {
      *error_message =
          "plugin sidebar '" + provider.info.id + "' snapshot failed: " + call_error;
    }
    return false;
  }
  if (!lua_istable(provider.state, -1)) {
    if (error_message != nullptr) {
      *error_message = "plugin sidebar '" + provider.info.id +
                       "' snapshot must return an array table";
    }
    lua_pop(provider.state, 1);
    return false;
  }

  const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(provider.state, -1));
  items->reserve(static_cast<std::size_t>(count));
  for (lua_Integer i = 1; i <= count; ++i) {
    lua_rawgeti(provider.state, -1, i);
    if (!lua_istable(provider.state, -1)) {
      if (error_message != nullptr) {
        *error_message = "plugin sidebar '" + provider.info.id +
                         "' snapshot items must be tables";
      }
      lua_pop(provider.state, 2);
      items->clear();
      return false;
    }
    PluginHost::SidebarItem item = lua_interop::ReadSidebarItem(provider.state, -1);
    if (item.label.empty()) {
      if (error_message != nullptr) {
        *error_message = "plugin sidebar '" + provider.info.id +
                         "' items require a label";
      }
      lua_pop(provider.state, 2);
      items->clear();
      return false;
    }
    if (!item.path.empty()) {
      item.path = resolve_runtime_path(current_project_root, item.path);
    }
    items->push_back(std::move(item));
    lua_pop(provider.state, 1);
  }

  lua_pop(provider.state, 1);
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ConfirmSidebarProviderItem(
    const runtime_types::SidebarProvider& provider,
    const PluginHost::SidebarItem& item,
    const std::filesystem::path& current_project_root,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>& resolve_runtime_path,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<bool(const PluginHost::OpenFileRequest&)>& open_file,
    std::string* error_message) {
  if (provider.confirm_ref == LUA_NOREF || provider.state == nullptr) {
    if (!item.path.empty() && open_file) {
      const bool opened = open_file(PluginHost::OpenFileRequest{
          .path = item.path,
          .line = item.line,
          .column = item.column,
      });
      if (error_message != nullptr) {
        error_message->clear();
      }
      return opened;
    }
    if (error_message != nullptr) {
      *error_message = "plugin sidebar '" + provider.info.id + "' has no confirm action";
    }
    return false;
  }

  const lua_interop::StackResetGuard stack_guard(provider.state);
  lua_rawgeti(provider.state, LUA_REGISTRYINDEX, provider.confirm_ref);
  PushSidebarItemTable(provider.state, item, current_project_root, resolve_runtime_path);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(provider.state);
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime ||
      !plugin->runtime->PCall(1, 0, &call_error)) {
    if (error_message != nullptr) {
      *error_message =
          "plugin sidebar '" + provider.info.id + "' confirm failed: " + call_error;
    }
    return false;
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool QueryHoverProvider(
    const runtime_types::HoverProvider& provider,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_table,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    PluginHost::HoverResult* result,
    std::string* error_message) {
  if (provider.state == nullptr || provider.provide_ref == LUA_NOREF || result == nullptr) {
    if (error_message != nullptr) {
      *error_message = "plugin hover provider is unavailable";
    }
    return false;
  }

  const lua_interop::StackResetGuard stack_guard(provider.state);
  lua_rawgeti(provider.state, LUA_REGISTRYINDEX, provider.provide_ref);
  push_buffer_table(provider.state, path);
  lua_interop::PushHoverPosition(provider.state, line, column);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(provider.state);
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime ||
      !plugin->runtime->PCall(2, 1, &call_error)) {
    if (error_message != nullptr) {
      *error_message = "plugin hover '" + provider.id + "' failed: " + call_error;
    }
    return false;
  }

  if (lua_isnil(provider.state, -1)) {
    lua_pop(provider.state, 1);
    result->title.clear();
    result->content.clear();
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }

  if (!lua_istable(provider.state, -1)) {
    if (error_message != nullptr) {
      *error_message = "plugin hover '" + provider.id + "' must return a table or nil";
    }
    lua_pop(provider.state, 1);
    return false;
  }

  const bool ok = ReadHoverResultTable(provider.state, -1, result, provider.id, error_message);
  lua_pop(provider.state, 1);
  return ok;
}

}  // namespace microide::plugin::sidebar_hover_interop

#endif
