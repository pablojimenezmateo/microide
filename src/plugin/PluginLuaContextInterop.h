#pragma once

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lua_context_interop {

#if MICROIDE_HAS_LUA_PLUGINS
struct ApiFns {
  lua_CFunction log;
  lua_CFunction notify;
  lua_CFunction commands_add;
  lua_CFunction workspace_project_root;
  lua_CFunction workspace_open_file;
  lua_CFunction workspace_active_buffer;
  lua_CFunction workspace_data_dir;
  lua_CFunction files_read_text;
  lua_CFunction files_write_text;
  lua_CFunction files_exists;
  lua_CFunction process_run;
  lua_CFunction process_run_async;
  lua_CFunction diagnostics_publish;
  lua_CFunction diagnostics_clear;
  lua_CFunction decorations_set;
  lua_CFunction decorations_clear;
  lua_CFunction sidebar_add;
  lua_CFunction sidebar_show;
  lua_CFunction hover_add;
  lua_CFunction menus_add;
  lua_CFunction keybindings_add;
  lua_CFunction settings_declare;
  lua_CFunction settings_get;
  lua_CFunction status_add;
  lua_CFunction status_update;
  lua_CFunction formatters_add;
  lua_CFunction save_participants_add;
  lua_CFunction completion_add;
  lua_CFunction code_action_add;
  lua_CFunction definition_add;
  lua_CFunction references_add;
  lua_CFunction signature_help_add;
  lua_CFunction document_symbols_add;
  lua_CFunction lsp_add;
  lua_CFunction debug_add;
  lua_CFunction debug_add_config;
  lua_CFunction task_add;
  lua_CFunction tool_add;
  lua_CFunction test_provider_add;
  lua_CFunction scm_provider_add;
  lua_CFunction annotation_provider_add;
  lua_CFunction auth_provider_add;
  lua_CFunction brackets_add;
  lua_CFunction comments_add;
  lua_CFunction indents_add;
  lua_CFunction snippets_add;
};

void PushPluginContext(lua_State* state, void* host_upvalue, const ApiFns& fns);
#endif

}  // namespace microide::plugin::lua_context_interop
