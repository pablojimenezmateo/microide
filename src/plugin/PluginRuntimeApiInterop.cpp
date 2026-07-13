#include "plugin/PluginRuntimeApiInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/LuaError.h"
#include "plugin/PluginDecorationInterop.h"
#include "plugin/PluginDiagnosticsInterop.h"
#include "plugin/PluginLuaInterop.h"
#include "plugin/PluginPathInterop.h"
#include "plugin/PluginSurfaceInterop.h"

#include <cmath>

namespace microide::plugin::runtime_api_interop {
namespace {

// Reads a 1-based position field; 0 means "absent" (callers treat 0 as unset).
// Read through `double` (lua_Number), not `float`: a 24-bit float mantissa rounds
// line/column indices at or above 2^24 to the wrong row, landing edits/cursors on
// the wrong line in very large buffers. Accepts both Lua integer and float subtypes.
std::size_t ReadIndexField(lua_State* state, int table_index, const char* field) {
  lua_interop::GetFieldProtected(state, table_index, field);
  const double value = lua_isnumber(state, -1) ? lua_tonumber(state, -1) : 0.0;
  lua_pop(state, 1);
  // Reject non-finite values (`inf`/NaN) and anything below 1 — casting a
  // non-finite double to size_t is undefined behavior. Clamp absurdly large
  // finite values so the double→size_t cast stays in range. A fractional value
  // truncates toward zero (its long-standing behavior).
  if (!std::isfinite(value) || value < 1.0) {
    return 0;
  }
  constexpr double kMaxIndex = 1e15;  // beyond any real document, safely < 2^53
  return static_cast<std::size_t>(value < kMaxIndex ? value : kMaxIndex);
}

// Resolves an optional `path` field on the spec table at index 1 against the
// project root. An absent/empty path leaves the request targeting the active
// editable buffer.
void ReadOptionalPathField(lua_State* state,
                           const std::filesystem::path& current_project_root,
                           std::filesystem::path* out_path) {
  std::optional<std::string> raw_path = lua_interop::ReadOptionalStringField(state, 1, "path");
  if (raw_path.has_value() && !raw_path->empty()) {
    *out_path =
        path_interop::ResolveRuntimePath(current_project_root, std::filesystem::path(*raw_path));
  }
}

void ReadOptionalPathField(lua_State* state,
                           const std::filesystem::path& current_project_root,
                           PluginHost::WorkspaceEditRequest* request) {
  ReadOptionalPathField(state, current_project_root, &request->path);
}

// Invokes the host edit callback and pushes the Lua result tuple. `request` must
// be destructed by the caller's enclosing scope before this returns to keep the
// (potentially OOM-raising) Lua pushes free of live C++ locals.
int PushEditResult(lua_State* state, bool applied, const char* fail_message) {
  if (applied) {
    lua_pushboolean(state, 1);
    return 1;
  }
  lua_pushboolean(state, 0);
  lua_pushstring(state, fail_message != nullptr ? fail_message : "editor edit failed");
  return 2;
}

}  // namespace

// These delegating functions are called by the thin wrappers in
// PluginHostLuaApi.inc. They never longjmp (no luaL_error / luaL_check): on any
// error they push the message and return lua_error_util::kPendingError, and the
// wrapper raises after its own locals destruct. See src/plugin/LuaError.h.

int LuaDiagnosticsPublish(lua_State* state,
                          const runtime_types::PluginInstance* plugin,
                          const std::filesystem::path& current_project_root,
                          const PluginHost::Callbacks& callbacks) {
  if (!lua_isstring(state, 1)) {
    lua_error_util::PushMessage(state, "diagnostics.publish requires a path string");
    return lua_error_util::kPendingError;
  }
  if (lua_type(state, 2) != LUA_TTABLE) {
    lua_error_util::PushMessage(state, "diagnostics.publish requires a diagnostics table");
    return lua_error_util::kPendingError;
  }
  const char* raw_path = lua_tostring(state, 1);
  {
    std::string error_message;
    if (plugin != nullptr &&
        diagnostics_interop::PublishDiagnostics(state, plugin->id, current_project_root, raw_path, 2,
                                                callbacks, &error_message)) {
      return 0;
    }
    lua_error_util::PushMessage(state, error_message, "failed to publish diagnostics");
  }
  return lua_error_util::kPendingError;
}

int LuaDiagnosticsClear(lua_State* state,
                        const runtime_types::PluginInstance* plugin,
                        const std::optional<std::filesystem::path>& path,
                        const PluginHost::Callbacks& callbacks) {
  {
    std::string error_message;
    if (plugin != nullptr &&
        diagnostics_interop::ClearDiagnostics(plugin->id, path, callbacks, &error_message)) {
      return 0;
    }
    lua_error_util::PushMessage(state, error_message, "failed to clear diagnostics");
  }
  return lua_error_util::kPendingError;
}

int LuaDecorationsSet(lua_State* state,
                      const runtime_types::PluginInstance* plugin,
                      const std::filesystem::path& current_project_root,
                      const PluginHost::Callbacks& callbacks) {
  if (!lua_isstring(state, 1)) {
    lua_error_util::PushMessage(state, "decorations.set requires a path string");
    return lua_error_util::kPendingError;
  }
  if (lua_type(state, 2) != LUA_TTABLE) {
    lua_error_util::PushMessage(state, "decorations.set requires a decoration table");
    return lua_error_util::kPendingError;
  }
  const char* raw_path = lua_tostring(state, 1);
  {
    std::string error_message;
    if (plugin != nullptr &&
        decoration_interop::PublishDecorations(state, plugin->id, current_project_root, raw_path, 2,
                                               callbacks, &error_message)) {
      return 0;
    }
    lua_error_util::PushMessage(state, error_message, "failed to set decorations");
  }
  return lua_error_util::kPendingError;
}

int LuaDecorationsClear(lua_State* state,
                        const runtime_types::PluginInstance* plugin,
                        const std::optional<std::filesystem::path>& path,
                        const PluginHost::Callbacks& callbacks) {
  {
    std::string error_message;
    if (plugin != nullptr &&
        decoration_interop::ClearDecorations(plugin->id, path, callbacks, &error_message)) {
      return 0;
    }
    lua_error_util::PushMessage(state, error_message, "failed to clear decorations");
  }
  return lua_error_util::kPendingError;
}

int LuaSurfaceSet(lua_State* state,
                  const runtime_types::PluginInstance* plugin,
                  const std::filesystem::path& current_project_root,
                  const PluginHost::Callbacks& callbacks) {
  if (!lua_isstring(state, 1)) {
    lua_error_util::PushMessage(state, "surface.set requires a surface id string");
    return lua_error_util::kPendingError;
  }
  if (lua_type(state, 2) != LUA_TTABLE) {
    lua_error_util::PushMessage(state, "surface.set requires a spec table");
    return lua_error_util::kPendingError;
  }
  const char* surface_id = lua_tostring(state, 1);
  {
    std::string error_message;
    if (plugin != nullptr &&
        surface_interop::PublishSurface(state, plugin->id, current_project_root, surface_id, 2,
                                        callbacks, &error_message)) {
      return 0;
    }
    lua_error_util::PushMessage(state, error_message, "failed to set surface");
  }
  return lua_error_util::kPendingError;
}

int LuaSurfaceClear(lua_State* state,
                    const runtime_types::PluginInstance* plugin,
                    const PluginHost::Callbacks& callbacks) {
  if (!lua_isstring(state, 1)) {
    lua_error_util::PushMessage(state, "surface.clear requires a surface id string");
    return lua_error_util::kPendingError;
  }
  const char* surface_id = lua_tostring(state, 1);
  {
    std::string error_message;
    if (plugin != nullptr &&
        surface_interop::ClearSurface(plugin->id, surface_id, callbacks, &error_message)) {
      return 0;
    }
    lua_error_util::PushMessage(state, error_message, "failed to clear surface");
  }
  return lua_error_util::kPendingError;
}

int LuaSidebarShow(lua_State* state, const PluginHost::Callbacks& callbacks) {
  const char* id = luaL_checkstring(state, 1);
  if (!callbacks.show_sidebar) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, callbacks.show_sidebar(id) ? 1 : 0);
  return 1;
}

int LuaEditorApplyEdits(lua_State* state,
                        const runtime_types::PluginInstance* plugin,
                        const std::filesystem::path& current_project_root,
                        const PluginHost::Callbacks& callbacks) {
  if (lua_type(state, 1) != LUA_TTABLE) {
    return PushEditResult(state, false, "editor.apply_edits requires a spec table");
  }
  if (plugin == nullptr || !callbacks.apply_workspace_edit) {
    return PushEditResult(state, false, "editor.apply_edits unavailable");
  }
  bool applied = false;
  const char* fail_message = "editor.apply_edits could not resolve a target buffer";
  {
    PluginHost::WorkspaceEditRequest request;
    ReadOptionalPathField(state, current_project_root, &request);

    lua_interop::GetFieldProtected(state, 1, "edits");
    if (lua_type(state, -1) == LUA_TTABLE) {
      // Cap the harvested edit count so a plugin returning a huge (or
      // sparse-border-overstated) array cannot grow an unbounded host vector on
      // this directly plugin-invokable runtime path (mirrors the provider-query
      // harvest clamps).
      constexpr lua_Integer kMaxApplyEdits = 100000;
      const lua_Integer raw_count = static_cast<lua_Integer>(lua_rawlen(state, -1));
      const lua_Integer count = raw_count < kMaxApplyEdits ? raw_count : kMaxApplyEdits;
      for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(state, -1, i);
        if (lua_type(state, -1) == LUA_TTABLE) {
          const int edit_index = lua_gettop(state);
          PluginHost::EditRequest edit;
          edit.start_line = ReadIndexField(state, edit_index, "start_line");
          edit.start_column = ReadIndexField(state, edit_index, "start_col");
          edit.end_line = ReadIndexField(state, edit_index, "end_line");
          edit.end_column = ReadIndexField(state, edit_index, "end_col");
          lua_interop::ReadStringField(state, edit_index, "text", &edit.text);
          // ReadIndexField yields 0 for any coordinate that was not a finite,
          // in-range 1-based index (fractional-below-1, negative, inf/NaN). Such
          // an edit is malformed; drop it rather than let a 0 clamp to a wild
          // insertion at the buffer start. If every edit is dropped the apply
          // resolves to "no edits" and is reported as failed to the plugin.
          if (edit.start_line == 0 || edit.start_column == 0 || edit.end_line == 0 ||
              edit.end_column == 0) {
            lua_pop(state, 1);
            continue;
          }
          request.edits.push_back(std::move(edit));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);  // edits

    lua_interop::GetFieldProtected(state, 1, "cursor");
    if (lua_type(state, -1) == LUA_TTABLE) {
      const int cursor_index = lua_gettop(state);
      request.cursor_line = ReadIndexField(state, cursor_index, "line");
      request.cursor_column = ReadIndexField(state, cursor_index, "col");
      request.has_cursor = request.cursor_line >= 1;
    }
    lua_pop(state, 1);  // cursor

    lua_interop::GetFieldProtected(state, 1, "selection");
    if (lua_type(state, -1) == LUA_TTABLE) {
      const int selection_index = lua_gettop(state);
      request.selection_start_line = ReadIndexField(state, selection_index, "start_line");
      request.selection_start_column = ReadIndexField(state, selection_index, "start_col");
      request.selection_end_line = ReadIndexField(state, selection_index, "end_line");
      request.selection_end_column = ReadIndexField(state, selection_index, "end_col");
      request.has_selection =
          request.selection_start_line >= 1 && request.selection_end_line >= 1;
    }
    lua_pop(state, 1);  // selection

    if (request.edits.empty() && !request.has_cursor && !request.has_selection) {
      fail_message = "editor.apply_edits requires edits, cursor, or selection";
    } else {
      applied = callbacks.apply_workspace_edit(plugin->id, request);
    }
  }
  return PushEditResult(state, applied, fail_message);
}

int LuaEditorSetCursor(lua_State* state,
                       const runtime_types::PluginInstance* plugin,
                       const std::filesystem::path& current_project_root,
                       const PluginHost::Callbacks& callbacks) {
  if (lua_type(state, 1) != LUA_TTABLE) {
    return PushEditResult(state, false, "editor.set_cursor requires a position table");
  }
  if (plugin == nullptr || !callbacks.apply_workspace_edit) {
    return PushEditResult(state, false, "editor.set_cursor unavailable");
  }
  bool applied = false;
  {
    PluginHost::WorkspaceEditRequest request;
    ReadOptionalPathField(state, current_project_root, &request);
    request.cursor_line = ReadIndexField(state, 1, "line");
    request.cursor_column = ReadIndexField(state, 1, "col");
    request.has_cursor = request.cursor_line >= 1;
    if (request.has_cursor) {
      applied = callbacks.apply_workspace_edit(plugin->id, request);
    }
  }
  return PushEditResult(state, applied, "editor.set_cursor requires a 1-based line");
}

int LuaEditorSetSelection(lua_State* state,
                          const runtime_types::PluginInstance* plugin,
                          const std::filesystem::path& current_project_root,
                          const PluginHost::Callbacks& callbacks) {
  if (lua_type(state, 1) != LUA_TTABLE) {
    return PushEditResult(state, false, "editor.set_selection requires a range table");
  }
  if (plugin == nullptr || !callbacks.apply_workspace_edit) {
    return PushEditResult(state, false, "editor.set_selection unavailable");
  }
  bool applied = false;
  {
    PluginHost::WorkspaceEditRequest request;
    ReadOptionalPathField(state, current_project_root, &request);
    request.selection_start_line = ReadIndexField(state, 1, "start_line");
    request.selection_start_column = ReadIndexField(state, 1, "start_col");
    request.selection_end_line = ReadIndexField(state, 1, "end_line");
    request.selection_end_column = ReadIndexField(state, 1, "end_col");
    request.has_selection =
        request.selection_start_line >= 1 && request.selection_end_line >= 1;
    if (request.has_selection) {
      applied = callbacks.apply_workspace_edit(plugin->id, request);
    }
  }
  return PushEditResult(state, applied, "editor.set_selection requires a 1-based range");
}

int LuaEditorSetGhostText(lua_State* state,
                          const runtime_types::PluginInstance* plugin,
                          const std::filesystem::path& current_project_root,
                          const PluginHost::Callbacks& callbacks) {
  if (lua_type(state, 1) != LUA_TTABLE) {
    return PushEditResult(state, false, "editor.set_ghost_text requires a spec table");
  }
  if (plugin == nullptr || !callbacks.publish_ghost_text) {
    return PushEditResult(state, false, "editor.set_ghost_text unavailable");
  }
  bool published = false;
  const char* fail_message = "editor.set_ghost_text requires non-empty text";
  {
    PluginHost::GhostTextRequest request;
    ReadOptionalPathField(state, current_project_root, &request.path);
    lua_interop::GetFieldProtected(state, 1, "anchor");
    if (lua_type(state, -1) == LUA_TTABLE) {
      const int anchor_index = lua_gettop(state);
      request.anchor_line = ReadIndexField(state, anchor_index, "line");
      request.anchor_column = ReadIndexField(state, anchor_index, "col");
    }
    lua_pop(state, 1);  // anchor
    lua_interop::ReadStringField(state, 1, "text", &request.text);
    if (!request.text.empty()) {
      callbacks.publish_ghost_text(plugin->id, request);
      published = true;
    }
  }
  return PushEditResult(state, published, fail_message);
}

int LuaEditorClearGhostText(lua_State* state,
                            const runtime_types::PluginInstance* plugin,
                            const std::filesystem::path& /*current_project_root*/,
                            const PluginHost::Callbacks& callbacks) {
  if (plugin == nullptr || !callbacks.clear_ghost_text) {
    return PushEditResult(state, false, "editor.clear_ghost_text unavailable");
  }
  callbacks.clear_ghost_text(plugin->id);
  return PushEditResult(state, true, nullptr);
}

}  // namespace microide::plugin::runtime_api_interop

#endif
