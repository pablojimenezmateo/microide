#include "plugin/PluginLuaContextInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

namespace microide::plugin::lua_context_interop {
namespace {

void PushBoundFn(lua_State* state, void* host_upvalue, lua_CFunction fn, const char* name) {
  lua_pushlightuserdata(state, host_upvalue);
  lua_pushcclosure(state, fn, 1);
  lua_setfield(state, -2, name);
}

void PushAddOnlyModule(lua_State* state,
                       void* host_upvalue,
                       const char* module_name,
                       lua_CFunction add_fn) {
  lua_createtable(state, 0, 1);
  PushBoundFn(state, host_upvalue, add_fn, "add");
  lua_setfield(state, -2, module_name);
}

}  // namespace

void PushPluginContext(lua_State* state, void* host_upvalue, const ApiFns& fns) {
  lua_createtable(state, 0, 12);

  PushBoundFn(state, host_upvalue, fns.log, "log");
  PushBoundFn(state, host_upvalue, fns.notify, "notify");

  PushAddOnlyModule(state, host_upvalue, "commands", fns.commands_add);

  lua_createtable(state, 0, 4);
  PushBoundFn(state, host_upvalue, fns.workspace_project_root, "project_root");
  PushBoundFn(state, host_upvalue, fns.workspace_open_file, "open_file");
  PushBoundFn(state, host_upvalue, fns.workspace_active_buffer, "active_buffer");
  PushBoundFn(state, host_upvalue, fns.workspace_data_dir, "data_dir");
  lua_setfield(state, -2, "workspace");

  lua_createtable(state, 0, 3);
  PushBoundFn(state, host_upvalue, fns.files_read_text, "read_text");
  PushBoundFn(state, host_upvalue, fns.files_write_text, "write_text");
  PushBoundFn(state, host_upvalue, fns.files_exists, "exists");
  lua_setfield(state, -2, "files");

  lua_createtable(state, 0, 2);
  PushBoundFn(state, host_upvalue, fns.process_run, "run");
  PushBoundFn(state, host_upvalue, fns.process_run_async, "run_async");
  lua_setfield(state, -2, "process");

  lua_createtable(state, 0, 2);
  PushBoundFn(state, host_upvalue, fns.diagnostics_publish, "publish");
  PushBoundFn(state, host_upvalue, fns.diagnostics_clear, "clear");
  lua_setfield(state, -2, "diagnostics");

  lua_createtable(state, 0, 2);
  PushBoundFn(state, host_upvalue, fns.decorations_set, "set");
  PushBoundFn(state, host_upvalue, fns.decorations_clear, "clear");
  lua_setfield(state, -2, "decorations");

  lua_createtable(state, 0, 2);
  PushBoundFn(state, host_upvalue, fns.surface_set, "set");
  PushBoundFn(state, host_upvalue, fns.surface_clear, "clear");
  lua_setfield(state, -2, "surface");

  lua_createtable(state, 0, 3);
  PushBoundFn(state, host_upvalue, fns.editor_apply_edits, "apply_edits");
  PushBoundFn(state, host_upvalue, fns.editor_set_cursor, "set_cursor");
  PushBoundFn(state, host_upvalue, fns.editor_set_selection, "set_selection");
  lua_setfield(state, -2, "editor");

  lua_createtable(state, 0, 2);
  PushBoundFn(state, host_upvalue, fns.sidebar_add, "add");
  PushBoundFn(state, host_upvalue, fns.sidebar_show, "show");
  lua_setfield(state, -2, "sidebar");

  PushAddOnlyModule(state, host_upvalue, "hover", fns.hover_add);
  PushAddOnlyModule(state, host_upvalue, "menus", fns.menus_add);
  PushAddOnlyModule(state, host_upvalue, "keybindings", fns.keybindings_add);

  lua_createtable(state, 0, 2);
  PushBoundFn(state, host_upvalue, fns.settings_declare, "declare");
  PushBoundFn(state, host_upvalue, fns.settings_get, "get");
  lua_setfield(state, -2, "settings");

  lua_createtable(state, 0, 2);
  PushBoundFn(state, host_upvalue, fns.status_add, "add");
  PushBoundFn(state, host_upvalue, fns.status_update, "update");
  lua_setfield(state, -2, "status");

  PushAddOnlyModule(state, host_upvalue, "themes", fns.themes_register);
  PushAddOnlyModule(state, host_upvalue, "file_icons", fns.file_icons_register);

  PushAddOnlyModule(state, host_upvalue, "formatters", fns.formatters_add);
  PushAddOnlyModule(state, host_upvalue, "save_participants", fns.save_participants_add);
  PushAddOnlyModule(state, host_upvalue, "completion", fns.completion_add);
  PushAddOnlyModule(state, host_upvalue, "code_actions", fns.code_action_add);
  PushAddOnlyModule(state, host_upvalue, "definition", fns.definition_add);
  PushAddOnlyModule(state, host_upvalue, "references", fns.references_add);
  PushAddOnlyModule(state, host_upvalue, "signature_help", fns.signature_help_add);
  PushAddOnlyModule(state, host_upvalue, "document_symbols", fns.document_symbols_add);
  PushAddOnlyModule(state, host_upvalue, "lsp", fns.lsp_add);

  // ctx.debug exposes both adapter and launch-config contribution.
  lua_createtable(state, 0, 2);
  PushBoundFn(state, host_upvalue, fns.debug_add, "add");
  PushBoundFn(state, host_upvalue, fns.debug_add_config, "addConfig");
  lua_setfield(state, -2, "debug");

  PushAddOnlyModule(state, host_upvalue, "tasks", fns.task_add);
  PushAddOnlyModule(state, host_upvalue, "tools", fns.tool_add);
  PushAddOnlyModule(state, host_upvalue, "tests", fns.test_provider_add);
  PushAddOnlyModule(state, host_upvalue, "scm", fns.scm_provider_add);
  PushAddOnlyModule(state, host_upvalue, "annotations", fns.annotation_provider_add);
  PushAddOnlyModule(state, host_upvalue, "auth", fns.auth_provider_add);
  PushAddOnlyModule(state, host_upvalue, "brackets", fns.brackets_add);
  PushAddOnlyModule(state, host_upvalue, "comments", fns.comments_add);
  PushAddOnlyModule(state, host_upvalue, "indents", fns.indents_add);
  PushAddOnlyModule(state, host_upvalue, "snippets", fns.snippets_add);
}

}  // namespace microide::plugin::lua_context_interop

#endif
