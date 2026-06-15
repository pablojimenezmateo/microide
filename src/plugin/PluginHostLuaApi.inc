#if MICROIDE_HAS_LUA_PLUGINS
  static int LuaMicroidePlugin(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    return 1;
  }

  static int LuaOpenMicroide(lua_State* state) {
    lua_createtable(state, 0, 2);
    lua_pushcfunction(state, &LuaMicroidePlugin);
    lua_setfield(state, -2, "plugin");
    lua_pushinteger(state, 1);
    lua_setfield(state, -2, "api_version");
    return 1;
  }

  static Impl* HostFromUpvalue(lua_State* state) {
    return static_cast<Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
  }

  // Long-lived empty fallbacks for the null-host case. Returning a reference (not
  // a value) lets the `host ? host->member : Fallback()` ternaries bind directly
  // without materializing a temporary in the wrapper frame. That matters because
  // the delegating interop call can raise a Lua error (a C longjmp), which would
  // skip the destructor of any wrapper-frame temporary. See src/plugin/LuaError.h.
  static const std::filesystem::path& EmptyProjectRoot() {
    static const std::filesystem::path kEmpty;
    return kEmpty;
  }
  static const PluginHost::Callbacks& EmptyCallbacks() {
    static const PluginHost::Callbacks kEmpty;
    return kEmpty;
  }

  template <typename RegisterFn>
  static int RegisterTableContribution(lua_State* state,
                                       const char* missing_plugin_message,
                                       const char* default_error_message,
                                       RegisterFn&& register_fn) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    if (plugin == nullptr) {
      lua_error_util::PushMessage(state, missing_plugin_message);
      return lua_error(state);
    }
    {
      std::string error_message;
      if (register_fn(host, plugin, &error_message)) {
        return 0;
      }
      lua_error_util::PushMessage(state, error_message, default_error_message);
    }
    return lua_error(state);
  }

  template <typename RegisterFn>
  static int RegisterHostTableContribution(lua_State* state,
                                           const char* default_error_message,
                                           RegisterFn&& register_fn) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    {
      std::string error_message;
      if (host != nullptr && register_fn(host, plugin, &error_message)) {
        return 0;
      }
      lua_error_util::PushMessage(state, error_message, default_error_message);
    }
    return lua_error(state);
  }

  PluginInstance* FindPluginByState(lua_State* state) {
    if (active_plugin != nullptr && active_plugin->state == state) {
      return active_plugin;
    }
    const auto it =
        std::find_if(plugins.begin(), plugins.end(), [state](const PluginInstance& plugin) {
          return plugin.state == state;
        });
    return it == plugins.end() ? nullptr : &(*it);
  }

  const PluginInstance* FindPluginByState(lua_State* state) const {
    if (active_plugin != nullptr && active_plugin->state == state) {
      return active_plugin;
    }
    const auto it =
        std::find_if(plugins.begin(), plugins.end(), [state](const PluginInstance& plugin) {
          return plugin.state == state;
        });
    return it == plugins.end() ? nullptr : &(*it);
  }

  std::string FormatPluginPrefix(const PluginInstance* plugin) const {
    return plugin == nullptr ? std::string("plugin")
                             : std::string("plugin ") + plugin->id;
  }

  static int LuaLog(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* message = luaL_checkstring(state, 1);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    if (host != nullptr) {
      std::string formatted = plugin != nullptr ? plugin->id + ": " + std::string(message)
                                                : std::string(message);
      host->RecordMessage(std::move(formatted));
    }
    return 0;
  }

  static int LuaNotify(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* level = luaL_checkstring(state, 1);
    const char* message = luaL_checkstring(state, 2);
    if (host != nullptr && host->callbacks.show_notification) {
      host->callbacks.show_notification(level, message);
    }
    return 0;
  }

  static int LuaCommandsAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* name = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    {
      std::string error_message;
      if (host != nullptr && registry_interop::RegisterCommand(
                                 state, host->FindPluginByState(state), host->callbacks, name, 2,
                                 &host->commands, &error_message)) {
        host->command_names_dirty = true;
        return 0;
      }
      lua_error_util::PushMessage(state, error_message, "failed to register plugin command");
    }
    return lua_error(state);
  }

  static int LuaSidebarAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    {
      std::string error_message;
      if (host != nullptr && registry_interop::RegisterSidebar(
                                 state, host->FindPluginByState(state), 1, &host->sidebars,
                                 &error_message)) {
        host->sidebar_providers_dirty = true;
        return 0;
      }
      lua_error_util::PushMessage(state, error_message, "failed to register plugin sidebar");
    }
    return lua_error(state);
  }

  static int LuaHoverAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    {
      std::string error_message;
      if (host != nullptr && registry_interop::RegisterHoverProvider(
                                 state, host->FindPluginByState(state), 1, &host->hovers,
                                 &host->hover_provider_order, &error_message)) {
        return 0;
      }
      lua_error_util::PushMessage(state, error_message, "failed to register hover provider");
    }
    return lua_error(state);
  }

  static int LuaMenusAdd(lua_State* state) {
    return RegisterHostTableContribution(
        state, "failed to register menu entry",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          registration_parsers::MenuEntryRegistration registration;
          if (!registration_parsers::ParseMenuEntryRegistration(
                  state, plugin != nullptr ? plugin->id : std::string{}, &registration,
                  error_message)) {
            return false;
          }
          return registry_interop::RegisterMenuEntry(
              plugin, std::move(registration.contributed), &host->menu_entries, error_message);
        });
  }

  static int LuaKeybindingsAdd(lua_State* state) {
    return RegisterHostTableContribution(
        state, "failed to register keybinding",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          registration_parsers::KeybindingRegistration registration;
          if (!registration_parsers::ParseKeybindingRegistration(
                  state, plugin != nullptr ? plugin->id : std::string{}, &registration,
                  error_message)) {
            return false;
          }
          return registry_interop::RegisterKeybinding(
              plugin, std::move(registration.contributed), &host->keybindings, error_message);
        });
  }

  static int LuaSettingsDeclare(lua_State* state) {
    return RegisterHostTableContribution(
        state, "failed to declare setting",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          registration_parsers::SettingRegistration registration;
          if (!registration_parsers::ParseSettingRegistration(
                  state, plugin != nullptr ? plugin->id : std::string{}, &registration,
                  error_message)) {
            return false;
          }
          return registry_interop::RegisterSetting(
              plugin, std::move(registration.contributed), &host->settings, error_message);
        });
  }

  static int LuaSettingsGet(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* id = luaL_checkstring(state, 1);
    if (host == nullptr || !host->callbacks.get_setting) {
      lua_pushnil(state);
      return 1;
    }
    const std::optional<std::string> value = host->callbacks.get_setting(id);
    if (!value.has_value()) {
      lua_pushnil(state);
      return 1;
    }
    lua_pushlstring(state, value->c_str(), value->size());
    return 1;
  }

  static int LuaStatusAdd(lua_State* state) {
    return RegisterHostTableContribution(
        state, "failed to register status item",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          registration_parsers::StatusItemRegistration registration;
          if (!registration_parsers::ParseStatusItemRegistration(
                  state, plugin != nullptr ? plugin->id : std::string{}, &registration,
                  error_message)) {
            return false;
          }
          return registry_interop::RegisterStatusItem(
              plugin, std::move(registration.contributed), &host->status_items,
              &host->status_item_order, error_message);
        });
  }

  static int LuaStatusUpdate(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* id = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    if (host == nullptr) {
      return 0;
    }
    if (registry_interop::UpdateStatusItem(state, host->FindPluginByState(state), id,
                                           &host->status_items, &host->status_item_order) &&
        host->callbacks.request_status_redraw) {
      host->callbacks.request_status_redraw();
    }
    return 0;
  }

  static int LuaFormattersAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "formatter registration requires an active plugin state",
        "failed to parse formatter registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterFormatter(
              state, plugin->id, &host->formatters, error_message);
        });
  }

  static int LuaSaveParticipantsAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "save participant registration requires an active plugin state",
        "failed to parse save participant registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterSaveParticipant(
              state, plugin->id, &host->save_participants, &host->save_participant_runtimes,
              error_message);
        });
  }

  static int LuaCompletionAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "completion registration requires an active plugin state",
        "failed to parse completion registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterCompletion(
              state, plugin->id, &host->completions, &host->completion_runtimes, error_message);
        });
  }

  static int LuaCodeActionAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "code action registration requires an active plugin state",
        "failed to parse code action registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterCodeAction(
              state, plugin->id, &host->code_actions, &host->code_action_runtimes, error_message);
        });
  }

  static int LuaTaskAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "task registration requires an active plugin state",
        "failed to parse task registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterTask(
              state, plugin->id, &host->tasks, error_message);
        });
  }

  static int LuaLspAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "language server registration requires an active plugin state",
        "failed to parse language server registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterLanguageServer(
              state, plugin->id, &host->language_servers, error_message);
        });
  }

  static int LuaToolAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "tool registration requires an active plugin state",
        "failed to parse tool registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterTool(
              state, plugin->id, &host->tools, error_message);
        });
  }

  static int LuaTestProviderAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "test provider registration requires an active plugin state",
        "failed to parse test provider registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterTestProvider(
              state, plugin->id, &host->test_providers, &host->test_provider_runtimes,
              error_message);
        });
  }

  static int LuaScmProviderAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "scm provider registration requires an active plugin state",
        "failed to parse scm provider registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          if (lua_istable(state, 1)) {
            return contribution_interop::RegisterScmProvider(
                state, plugin->id, &host->scm_providers, &host->scm_provider_runtimes,
                error_message);
          }
          // Shorthand form: register_scm_provider("id", "label").
          host->scm_providers.push_back(PluginHost::ContributedScmProvider{
              .id = plugin->id + "." + std::string(luaL_checkstring(state, 1)),
              .label = std::string(luaL_checkstring(state, 2)),
              .plugin_id = plugin->id,
          });
          return true;
        });
  }

  static int LuaAnnotationProviderAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "annotation provider registration requires an active plugin state",
        "failed to parse annotation provider registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterAnnotationProvider(
              state, plugin->id, &host->annotation_providers, &host->annotation_provider_runtimes,
              error_message);
        });
  }

  static int LuaAuthProviderAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "auth provider registration requires an active plugin state",
        "failed to parse auth provider registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          if (lua_istable(state, 1)) {
            return contribution_interop::RegisterAuthProvider(
                state, plugin->id, &host->auth_providers, &host->auth_provider_runtimes,
                error_message);
          }
          // Shorthand form: register_auth_provider("id", "label").
          host->auth_providers.push_back(PluginHost::ContributedAuthProvider{
              .id = plugin->id + "." + std::string(luaL_checkstring(state, 1)),
              .label = std::string(luaL_checkstring(state, 2)),
              .plugin_id = plugin->id,
          });
          return true;
        });
  }

  static int LuaBracketsAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "bracket set registration requires an active plugin state",
        "failed to parse bracket set registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterBracketSet(
              state, plugin->id, &host->bracket_sets, error_message);
        });
  }

  static int LuaCommentsAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "comment markers registration requires an active plugin state",
        "failed to parse comment markers registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterCommentMarkers(
              state, plugin->id, &host->comment_markers, error_message);
        });
  }

  static int LuaIndentsAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "indent rules registration requires an active plugin state",
        "failed to parse indent rules registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterIndentRules(
              state, plugin->id, &host->indent_rules, error_message);
        });
  }

  static int LuaSnippetsAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "snippet registration requires an active plugin state",
        "failed to parse snippet registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterSnippet(
              state, plugin->id, &host->snippets, error_message);
        });
  }

  static int LuaWorkspaceProjectRoot(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaWorkspaceProjectRoot(
        state, host != nullptr ? host->current_project_root : EmptyProjectRoot());
  }

  static int LuaWorkspaceOpenFile(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaWorkspaceOpenFile(
        state, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
  }

  static int LuaWorkspaceActiveBuffer(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaWorkspaceActiveBuffer(
        state, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
  }

  static int LuaFilesReadText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaFilesReadText(
        state, host != nullptr ? host->current_project_root : EmptyProjectRoot());
  }

  static int LuaFilesWriteText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaFilesWriteText(
        state, host != nullptr ? host->current_project_root : EmptyProjectRoot());
  }

  static int LuaFilesExists(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaFilesExists(
        state, host != nullptr ? host->current_project_root : EmptyProjectRoot());
  }

  // The process / diagnostics delegates return lua_error_util::kPendingError instead
  // of raising, having already pushed the message; we raise here, where the wrapper
  // frame holds no non-trivial locals to leak across the longjmp.
  static int LuaProcessRun(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const int rc = process_interop::LuaProcessRun(
        state, host != nullptr ? host->current_project_root : EmptyProjectRoot());
    return rc == lua_error_util::kPendingError ? lua_error(state) : rc;
  }

  static int LuaProcessRunAsync(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const int rc = process_interop::LuaProcessRunAsync(
        state, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->async_process_state : nullptr);
    return rc == lua_error_util::kPendingError ? lua_error(state) : rc;
  }

  static int LuaDiagnosticsPublish(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    const int rc = runtime_api_interop::LuaDiagnosticsPublish(
        state, plugin, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
    return rc == lua_error_util::kPendingError ? lua_error(state) : rc;
  }

  static int LuaDiagnosticsClear(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    int rc = 0;
    {
      std::optional<std::filesystem::path> path;
      if (lua_gettop(state) >= 1 && !lua_isnil(state, 1)) {
        const char* raw_path = luaL_checkstring(state, 1);
        path = path_interop::ResolveRuntimePath(
            host != nullptr ? host->current_project_root : EmptyProjectRoot(),
            std::filesystem::path(raw_path));
      }
      rc = runtime_api_interop::LuaDiagnosticsClear(
          state, plugin, path, host != nullptr ? host->callbacks : EmptyCallbacks());
    }
    return rc == lua_error_util::kPendingError ? lua_error(state) : rc;
  }

  static int LuaSidebarShow(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return runtime_api_interop::LuaSidebarShow(
        state, host != nullptr ? host->callbacks : EmptyCallbacks());
  }

  void PushPluginContext(lua_State* state) {
    lua_context_interop::PushPluginContext(
        state, this,
        lua_context_interop::ApiFns{
            .log = &LuaLog,
            .notify = &LuaNotify,
            .commands_add = &LuaCommandsAdd,
            .workspace_project_root = &LuaWorkspaceProjectRoot,
            .workspace_open_file = &LuaWorkspaceOpenFile,
            .workspace_active_buffer = &LuaWorkspaceActiveBuffer,
            .files_read_text = &LuaFilesReadText,
            .files_write_text = &LuaFilesWriteText,
            .files_exists = &LuaFilesExists,
            .process_run = &LuaProcessRun,
            .process_run_async = &LuaProcessRunAsync,
            .diagnostics_publish = &LuaDiagnosticsPublish,
            .diagnostics_clear = &LuaDiagnosticsClear,
            .sidebar_add = &LuaSidebarAdd,
            .sidebar_show = &LuaSidebarShow,
            .hover_add = &LuaHoverAdd,
            .menus_add = &LuaMenusAdd,
            .keybindings_add = &LuaKeybindingsAdd,
            .settings_declare = &LuaSettingsDeclare,
            .settings_get = &LuaSettingsGet,
            .status_add = &LuaStatusAdd,
            .status_update = &LuaStatusUpdate,
            .formatters_add = &LuaFormattersAdd,
            .save_participants_add = &LuaSaveParticipantsAdd,
            .completion_add = &LuaCompletionAdd,
            .code_action_add = &LuaCodeActionAdd,
            .lsp_add = &LuaLspAdd,
            .task_add = &LuaTaskAdd,
            .tool_add = &LuaToolAdd,
            .test_provider_add = &LuaTestProviderAdd,
            .scm_provider_add = &LuaScmProviderAdd,
            .annotation_provider_add = &LuaAnnotationProviderAdd,
            .auth_provider_add = &LuaAuthProviderAdd,
            .brackets_add = &LuaBracketsAdd,
            .comments_add = &LuaCommentsAdd,
            .indents_add = &LuaIndentsAdd,
            .snippets_add = &LuaSnippetsAdd,
        });
  }

  void PushProjectTable(lua_State* state, const std::filesystem::path& project_root) {
    lua_buffer_project_interop::PushProjectTable(state, project_root);
  }

  void PushBufferTable(lua_State* state, const std::filesystem::path& path) {
    lua_buffer_project_interop::PushBufferTable(
        state, path, [this](const std::filesystem::path& buffer_path) {
          return RelativePathString(buffer_path);
        });
  }

  bool SnapshotSidebarProvider(const SidebarProvider& provider,
                               std::vector<SidebarItem>* items,
                               std::string* error_message) {
    return sidebar_hover_interop::SnapshotSidebarProvider(
        provider, current_project_root,
        [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
          return path_interop::ResolveRuntimePath(project_root, runtime_path);
        },
        [this](lua_State* state) { return FindPluginByState(state); }, items, error_message);
  }

  bool ConfirmSidebarProviderItem(const SidebarProvider& provider,
                                  const SidebarItem& item,
                                  std::string* error_message) {
    return sidebar_hover_interop::ConfirmSidebarProviderItem(
        provider, item, current_project_root,
        [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
          return path_interop::ResolveRuntimePath(project_root, runtime_path);
        },
        [this](lua_State* state) { return FindPluginByState(state); }, callbacks.open_file,
        error_message);
  }

  bool QueryHoverProvider(const HoverProvider& provider,
                          const std::filesystem::path& path,
                          std::size_t line,
                          std::size_t column,
                          PluginHost::HoverResult* result,
                          std::string* error_message) {
    return sidebar_hover_interop::QueryHoverProvider(
        provider, path, line, column,
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          PushBufferTable(state, buffer_path);
        },
        [this](lua_State* state) { return FindPluginByState(state); }, result, error_message);
  }

  void ClearPluginDiagnostics(const PluginInstance* plugin) {
    state_teardown_interop::ClearPluginDiagnostics(
        plugin, [this](std::string_view owner) {
          if (callbacks.clear_owner_diagnostics) {
            callbacks.clear_owner_diagnostics(owner);
          }
        });
  }

  void DestroyPluginState(PluginInstance* plugin) {
    state_teardown_interop::DestroyPluginState(plugin);
  }

  void UnregisterContributionsForState(lua_State* state) {
    state_teardown_interop::UnregisterContributionsForState(
        state, FindPluginByState(state), &commands, &command_names, &sidebars, &sidebar_providers,
        &hovers, &hover_provider_order, &menu_entries, &keybindings, &settings, &status_items,
        &status_item_order, &formatters, &save_participants, &save_participant_runtimes,
        &completions, &completion_runtimes, &code_actions, &code_action_runtimes,
        &language_servers, &tasks, &tools, &test_providers, &test_provider_runtimes,
        &scm_providers, &scm_provider_runtimes, &annotation_providers,
        &annotation_provider_runtimes, &auth_providers, &auth_provider_runtimes,
        &bracket_sets, &comment_markers, &indent_rules, &snippets);
  }

  bool InitializeState(PluginInstance* plugin, std::string* error_message) {
    return lifecycle_load_interop::InitializeState(plugin, &LuaOpenMicroide, error_message);
  }

  bool LoadPluginDescriptor(PluginInstance* plugin, std::string* error_message) {
    return lifecycle_load_interop::LoadPluginDescriptor(plugin, error_message);
  }

  bool CallSetup(PluginInstance* plugin, std::string* error_message) {
    return lifecycle_callback_interop::CallSetup(
        plugin, &active_plugin,
        [this](lua_State* state) { PushPluginContext(state); },
        [this](const PluginInstance* plugin_instance) {
          return FormatPluginPrefix(plugin_instance);
        },
        error_message);
  }

  void CallProjectCallback(PluginInstance* plugin, int ref, const char* callback_name) {
    lifecycle_callback_interop::CallProjectCallback(
        plugin, ref, callback_name, !current_project_root.empty(),
        [this](lua_State* state) { PushPluginContext(state); },
        [this](lua_State* state) { PushProjectTable(state, current_project_root); },
        [this](std::string error) { RecordError(std::move(error)); },
        [this](const PluginInstance* plugin_instance) {
          return FormatPluginPrefix(plugin_instance);
        });
  }

  void CallBufferCallback(PluginInstance* plugin,
                          int ref,
                          const char* callback_name,
                          const std::filesystem::path& path) {
    lifecycle_callback_interop::CallBufferCallback(
        plugin, ref, callback_name, path,
        [this](lua_State* state) { PushPluginContext(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          PushBufferTable(state, buffer_path);
        },
        [this](std::string error) { RecordError(std::move(error)); },
        [this](const PluginInstance* plugin_instance) {
          return FormatPluginPrefix(plugin_instance);
        });
  }

  void CallShutdown(PluginInstance* plugin) {
    lifecycle_callback_interop::CallShutdown(
        plugin, [this](lua_State* state) { PushPluginContext(state); },
        [this](std::string error) { RecordError(std::move(error)); },
        [this](const PluginInstance* plugin_instance) {
          return FormatPluginPrefix(plugin_instance);
        });
  }

  void TearDownPlugins() {
    lifecycle_callback_interop::TearDownPlugins(
        &plugins, !current_project_root.empty(),
        [this](PluginInstance* plugin, int ref, const char* callback_name) {
          CallProjectCallback(plugin, ref, callback_name);
        },
        [this](PluginInstance* plugin) { CallShutdown(plugin); },
        [this](lua_State* state) { UnregisterContributionsForState(state); },
        [this](const PluginInstance* plugin) { ClearPluginDiagnostics(plugin); },
        [this](PluginInstance* plugin) { DestroyPluginState(plugin); });
  }

  bool LoadPluginRoot(const std::filesystem::path& plugin_root,
                      std::string* error_message) {
    return lifecycle_callback_interop::LoadPluginRoot(
        plugin_root, &plugins,
        [this](PluginInstance* plugin, std::string* init_error) {
          return InitializeState(plugin, init_error);
        },
        [this](PluginInstance* plugin, std::string* descriptor_error) {
          return LoadPluginDescriptor(plugin, descriptor_error);
        },
        [this](PluginInstance* plugin, std::string* setup_error) {
          return CallSetup(plugin, setup_error);
        },
        [this](lua_State* state) { UnregisterContributionsForState(state); },
        [this](const PluginInstance* plugin) { ClearPluginDiagnostics(plugin); },
        [this](PluginInstance* plugin) { DestroyPluginState(plugin); },
        [this](const std::string& id) {
          return std::find(disabled_plugins.begin(), disabled_plugins.end(), id) !=
                 disabled_plugins.end();
        },
        [this](const PluginInstance& plugin) {
          disabled_plugin_meta.push_back(
              PluginHost::LoadedPlugin{.id = plugin.id, .root = plugin.root, .enabled = false});
        },
        error_message);
  }
#endif
