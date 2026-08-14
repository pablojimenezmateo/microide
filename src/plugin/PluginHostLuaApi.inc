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
  static const std::filesystem::path& EmptyDataDir() {
    static const std::filesystem::path kEmpty;
    return kEmpty;
  }
  // Capability set used when no calling plugin can be resolved (null host/plugin). Denying
  // everything is the safe posture: a real plugin always carries its own declared capabilities,
  // which default to project-scoped fs and no process/network.
  static const PluginCapabilities& DenyAllCaps() {
    static const PluginCapabilities kDeny{
        .fs_read = FsAccess::kNone,
        .fs_write = FsAccess::kNone,
        .process_exec = false,
        .process_allowlist = {},
        .network = false,
    };
    return kDeny;
  }

  // Builds the non-owning filesystem/process scope for the calling plugin. The result holds
  // only references (to host/plugin members or the static fallbacks above), so it is safe to
  // hand to a delegated interop call that may raise a Lua error (a C longjmp).
  static PluginFsContext MakeFsContext(Impl* host, const PluginInstance* plugin) {
    return PluginFsContext{
        host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        plugin != nullptr ? plugin->data_dir : EmptyDataDir(),
        plugin != nullptr ? plugin->capabilities : DenyAllCaps(),
    };
  }

  // Resolves the kernel-confinement descriptor for a process-spawning contribution from the
  // calling plugin's project root, data dir, and network capability. Mirrors the live
  // process.run sandbox so contributed servers/formatters are confined the same way.
  static platform::SubprocessSandbox MakeContributionSandbox(const std::filesystem::path& project_root,
                                                             const PluginInstance* plugin) {
    platform::SubprocessSandbox sandbox;
    sandbox.enabled = true;
    sandbox.allow_network = plugin != nullptr ? plugin->capabilities.network : false;
    if (!project_root.empty()) {
      sandbox.read_roots.push_back(project_root);
      sandbox.write_roots.push_back(project_root);
    }
    if (plugin != nullptr && !plugin->data_dir.empty()) {
      sandbox.read_roots.push_back(plugin->data_dir);
      sandbox.write_roots.push_back(plugin->data_dir);
    }
    return sandbox;
  }

  // Allowlist gate for a contributed argv (formatter / language server / task). Mirrors the
  // live process.run allowlist: an empty allowlist permits anything; otherwise command[0] must
  // match an entry by exact string or basename. An empty command is left to the registration
  // parser to reject and is not treated as an allowlist violation here.
  static bool CommandAllowed(const PluginCapabilities& caps,
                             const std::vector<std::string>& command) {
    if (caps.process_allowlist.empty() || command.empty()) {
      return true;
    }
    const std::string base = std::filesystem::path(command[0]).filename().string();
    for (const std::string& allowed : caps.process_allowlist) {
      if (allowed == command[0] || allowed == base) {
        return true;
      }
    }
    return false;
  }

  // Surfaces a denied/failed process call on the plugin's output channel. Fully destructs its
  // std::string before returning so the caller may immediately lua_error (longjmp) with nothing
  // live on the stack.
  static void RecordProcessPendingError(Impl* host,
                                        const PluginInstance* plugin,
                                        lua_State* state) {
    if (host == nullptr) {
      return;
    }
    const char* message = lua_tostring(state, -1);
    host->RecordMessage(host->FormatPluginPrefix(plugin) + ": " +
                        std::string(message != nullptr ? message : "process error"));
  }

  // Registration verbs only make sense during plugin setup, where the worker holds
  // exclusive host access (direct). A registration attempt from a reactive event
  // (detached on the worker) would mutate shared registries the UI thread reads, so
  // it is refused rather than raced. Returns true when the call must be rejected,
  // leaving the error message on the Lua stack for the caller to raise.
  static bool RejectRuntimeRegistration(lua_State* state, Impl* host) {
    if (host == nullptr || g_exec.allow_registration) {
      return false;
    }
    lua_error_util::PushMessage(
        state, "ctx registration verbs are only supported during plugin setup");
    return true;
  }

  template <typename RegisterFn>
  static int RegisterTableContribution(lua_State* state,
                                       const char* missing_plugin_message,
                                       const char* default_error_message,
                                       RegisterFn&& register_fn) {
    Impl* host = HostFromUpvalue(state);
    if (RejectRuntimeRegistration(state, host)) {
      return lua_error(state);
    }
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
    if (RejectRuntimeRegistration(state, host)) {
      return lua_error(state);
    }
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
    if (RejectRuntimeRegistration(state, host)) {
      return lua_error(state);
    }
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
    if (RejectRuntimeRegistration(state, host)) {
      return lua_error(state);
    }
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
    if (RejectRuntimeRegistration(state, host)) {
      return lua_error(state);
    }
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
          // Setup-time registration into the worker-owned registry; the bump to the
          // UI-visible revision happens when the reload publishes the snapshot.
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
    // Read the update off the Lua table here (worker), then defer the apply + redraw
    // so a status.update from a reactive event never races the UI thread's reads. The
    // closure mutates the published status view directly: ApplyHostMutation runs it
    // inline under exclusive access (setup / round-trip, UI parked) or marshals it to
    // the UI thread (detached event) -- published_ is UI-thread-owned in both cases.
    registry_interop::StatusItemUpdate update;
    if (!registry_interop::ExtractStatusItemUpdate(state, host->FindPluginByState(state), id,
                                                   &update)) {
      return 0;
    }
    if (g_exec.in_reload) {
      // Update issued from a setup / project-open handler: mutate the live order so the
      // reload's publish carries it (a publish to published_ here would be overwritten).
      registry_interop::ApplyStatusItemUpdate(update, &host->status_item_order,
                                              &host->status_item_index);
      return 0;
    }
    host->ApplyHostMutation([host, update = std::move(update)]() mutable {
      if (registry_interop::ApplyStatusItemUpdate(update, &host->published_.status_item_order,
                                                  &host->published_.status_item_index)) {
        ++host->status_view_revision;
        if (host->raw_callbacks.request_status_redraw) {
          host->raw_callbacks.request_status_redraw();
        }
      }
    });
    return 0;
  }

  static int LuaFormattersAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "formatter registration requires an active plugin state",
        "failed to parse formatter registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          if (!plugin->capabilities.process_exec) {
            *error_message = "formatter requires capabilities.process.exec";
            return false;
          }
          if (!contribution_interop::RegisterFormatter(
                  state, plugin->id, &host->formatters, error_message)) {
            return false;
          }
          if (!host->formatters.empty() &&
              !CommandAllowed(plugin->capabilities, host->formatters.back().command)) {
            host->formatters.pop_back();
            *error_message = "formatter program not in capabilities.process.allow";
            return false;
          }
          return true;
        });
  }

  static int LuaThemeRegister(lua_State* state) {
    return RegisterTableContribution(
        state, "theme registration requires an active plugin state",
        "failed to register theme",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return presentation_interop::RegisterTheme(state, plugin->id, &host->themes,
                                                     error_message);
        });
  }

  static int LuaFileIconThemeRegister(lua_State* state) {
    return RegisterTableContribution(
        state, "file icon theme registration requires an active plugin state",
        "failed to register file icon theme",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return presentation_interop::RegisterFileIconTheme(state, plugin->id,
                                                            &host->file_icon_themes, error_message);
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

  static int LuaVirtualDocumentsAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "virtual document registration requires an active plugin state",
        "failed to parse virtual document registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterVirtualDocument(
              state, plugin->id, &host->virtual_documents, error_message);
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

  static int LuaLanguageQueryAdd(lua_State* state, runtime_types::LanguageQueryKind kind) {
    return RegisterTableContribution(
        state, "language provider registration requires an active plugin state",
        "failed to parse language provider registration",
        [state, kind](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterLanguageQuery(
              state, plugin->id, kind, &host->language_query_runtimes, error_message);
        });
  }

  static int LuaDefinitionAdd(lua_State* state) {
    return LuaLanguageQueryAdd(state, runtime_types::LanguageQueryKind::Definition);
  }

  static int LuaReferencesAdd(lua_State* state) {
    return LuaLanguageQueryAdd(state, runtime_types::LanguageQueryKind::References);
  }

  static int LuaSignatureHelpAdd(lua_State* state) {
    return LuaLanguageQueryAdd(state, runtime_types::LanguageQueryKind::SignatureHelp);
  }

  static int LuaDocumentSymbolsAdd(lua_State* state) {
    return LuaLanguageQueryAdd(state, runtime_types::LanguageQueryKind::DocumentSymbol);
  }

  static int LuaTaskAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "task registration requires an active plugin state",
        "failed to parse task registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          if (!plugin->capabilities.process_exec) {
            *error_message = "task requires capabilities.process.exec";
            return false;
          }
          if (!contribution_interop::RegisterTask(
                  state, plugin->id, &host->tasks, &host->task_ids, error_message)) {
            return false;
          }
          if (!host->tasks.empty() &&
              !CommandAllowed(plugin->capabilities, host->tasks.back().command)) {
            // RegisterTask already recorded the id in task_ids; drop it back out so
            // the index stays a faithful mirror of the surviving vector.
            host->task_ids.erase(host->tasks.back().id);
            host->tasks.pop_back();
            *error_message = "task program not in capabilities.process.allow";
            return false;
          }
          return true;
        });
  }

  static int LuaLspAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "language server registration requires an active plugin state",
        "failed to parse language server registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          if (!plugin->capabilities.process_exec) {
            *error_message = "language server requires capabilities.process.exec";
            return false;
          }
          if (!contribution_interop::RegisterLanguageServer(
                  state, plugin->id, &host->language_servers, &host->language_server_ids,
                  error_message)) {
            return false;
          }
          if (!host->language_servers.empty() &&
              !CommandAllowed(plugin->capabilities, host->language_servers.back().command)) {
            host->language_server_ids.erase(host->language_servers.back().id);
            host->language_servers.pop_back();
            *error_message = "language server program not in capabilities.process.allow";
            return false;
          }
          if (!host->language_servers.empty()) {
            host->language_servers.back().sandbox =
                MakeContributionSandbox(host->current_project_root, plugin);
          }
          return true;
        });
  }

  static int LuaDebugAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "debug adapter registration requires an active plugin state",
        "failed to parse debug adapter registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          if (!plugin->capabilities.process_exec) {
            *error_message = "debug adapter requires capabilities.process.exec";
            return false;
          }
          if (!contribution_interop::RegisterDebugAdapter(
                  state, plugin->id, &host->debug_adapters, &host->debug_adapter_ids,
                  error_message)) {
            return false;
          }
          if (!host->debug_adapters.empty() &&
              !CommandAllowed(plugin->capabilities, host->debug_adapters.back().command)) {
            host->debug_adapter_ids.erase(host->debug_adapters.back().id);
            host->debug_adapters.pop_back();
            *error_message = "debug adapter program not in capabilities.process.allow";
            return false;
          }
          if (!host->debug_adapters.empty()) {
            host->debug_adapters.back().sandbox =
                MakeContributionSandbox(host->current_project_root, plugin);
          }
          return true;
        });
  }

  static int LuaDebugAddConfig(lua_State* state) {
    return RegisterTableContribution(
        state, "debug launch-config registration requires an active plugin state",
        "failed to parse debug launch-config registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          // A launch config only describes how to invoke an adapter; it spawns
          // nothing itself, so it shares the debug-adapter capability gate.
          if (!plugin->capabilities.process_exec) {
            *error_message = "debug launch config requires capabilities.process.exec";
            return false;
          }
          return contribution_interop::RegisterLaunchConfig(
              state, plugin->id, &host->launch_configs, &host->launch_config_ids, error_message);
        });
  }

  static int LuaToolAdd(lua_State* state) {
    return RegisterTableContribution(
        state, "tool registration requires an active plugin state",
        "failed to parse tool registration",
        [state](Impl* host, const PluginInstance* plugin, std::string* error_message) {
          return contribution_interop::RegisterTool(
              state, plugin->id, &host->tools, &host->tool_ids, error_message);
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
          // Shorthand form: register_scm_provider("id", "label"). Validate via a
          // non-raising check and route failures through error_message: raising
          // (luaL_checkstring) here would longjmp over the live C++ locals in this
          // frame and RegisterTableContribution's error_message.
          if (!lua_isstring(state, 1) || !lua_isstring(state, 2)) {
            *error_message = "register_scm_provider(id, label) requires string arguments";
            return false;
          }
          // Apply the shared per-kind contribution cap the table form enforces: the
          // shorthand path previously pushed directly into host->scm_providers, so a
          // plugin could loop it to grow the host vector past the memory backstop.
          if (microide::plugin::ContributionLimitReached(&host->scm_providers,
                                                             error_message)) {
            return false;
          }
          // NUL-reject the id so a truncated "a\0b" cannot collide with another provider
          // id (TD-2026-07-17A-080).
          std::optional<std::string> id = lua_interop::ToHostString(state, 1);
          std::optional<std::string> label = lua_interop::ToHostString(state, 2);
          if (!id || !label) {
            *error_message = "register_scm_provider(id, label) requires NUL-free strings";
            return false;
          }
          host->scm_providers.push_back(PluginHost::ContributedScmProvider{
              .id = plugin->id + "." + *id,
              .label = *label,
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
          // Shorthand form: register_auth_provider("id", "label"). See the scm
          // provider path above: validate without raising so a bad argument cannot
          // longjmp over the live C++ locals in this frame.
          if (!lua_isstring(state, 1) || !lua_isstring(state, 2)) {
            *error_message = "register_auth_provider(id, label) requires string arguments";
            return false;
          }
          // Same shared per-kind cap the table form enforces; the shorthand path
          // previously bypassed it and could grow host->auth_providers unbounded.
          if (microide::plugin::ContributionLimitReached(&host->auth_providers,
                                                             error_message)) {
            return false;
          }
          // NUL-reject the id (TD-2026-07-17A-080); see the scm shorthand above.
          std::optional<std::string> id = lua_interop::ToHostString(state, 1);
          std::optional<std::string> label = lua_interop::ToHostString(state, 2);
          if (!id || !label) {
            *error_message = "register_auth_provider(id, label) requires NUL-free strings";
            return false;
          }
          host->auth_providers.push_back(PluginHost::ContributedAuthProvider{
              .id = plugin->id + "." + *id,
              .label = *label,
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

  // Returns the calling plugin's writable scratch directory (created on demand), or nil when no
  // plugin can be resolved. Plugins that declare data-scope fs access use this for report files
  // and caches instead of writing to /tmp, which the sandbox denies. Building the path string and
  // creating the directory happen before anything that could raise, so no longjmp crosses them.
  static int LuaWorkspaceDataDir(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    if (plugin == nullptr || plugin->data_dir.empty()) {
      lua_pushnil(state);
      return 1;
    }
    std::error_code create_error;
    std::filesystem::create_directories(plugin->data_dir, create_error);
    const std::string path = plugin->data_dir.generic_string();
    lua_pushlstring(state, path.c_str(), path.size());
    return 1;
  }

  // Filesystem helpers resolve the calling plugin so MakeFsContext can carry its capabilities
  // and data dir. On a refused access the interop pushes the falsy result and sets `denied`,
  // letting us surface a diagnostic on the plugin's output channel without raising.
  static int LuaFilesReadText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    bool denied = false;
    const int rc = workspace_interop::LuaFilesReadText(state, MakeFsContext(host, plugin), &denied);
    if (denied && host != nullptr) {
      host->RecordMessage(host->FormatPluginPrefix(plugin) +
                          ": denied files.read_text (path outside fs scope or capabilities.fs.read)");
    }
    return rc;
  }

  static int LuaFilesWriteText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    bool denied = false;
    const int rc = workspace_interop::LuaFilesWriteText(state, MakeFsContext(host, plugin), &denied);
    if (denied && host != nullptr) {
      host->RecordMessage(host->FormatPluginPrefix(plugin) +
                          ": denied files.write_text (path outside fs scope or capabilities.fs.write)");
    }
    return rc;
  }

  static int LuaFilesExists(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    bool denied = false;
    const int rc = workspace_interop::LuaFilesExists(state, MakeFsContext(host, plugin), &denied);
    if (denied && host != nullptr) {
      host->RecordMessage(host->FormatPluginPrefix(plugin) +
                          ": denied files.exists (path outside fs scope or capabilities.fs.read)");
    }
    return rc;
  }

  // The process / diagnostics delegates return lua_error_util::kPendingError instead
  // of raising, having already pushed the message; we record the denial diagnostic and
  // raise here, where the wrapper frame holds no non-trivial locals to leak across the longjmp.
  static int LuaProcessRun(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    const int rc = process_interop::LuaProcessRun(state, MakeFsContext(host, plugin));
    if (rc == lua_error_util::kPendingError) {
      RecordProcessPendingError(host, plugin, state);
      return lua_error(state);
    }
    return rc;
  }

  static int LuaProcessRunAsync(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    const int rc = process_interop::LuaProcessRunAsync(state, MakeFsContext(host, plugin));
    if (rc == lua_error_util::kPendingError) {
      RecordProcessPendingError(host, plugin, state);
      return lua_error(state);
    }
    return rc;
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
    // Validate the optional path argument with non-raising checks BEFORE any
    // non-trivial C++ local exists. luaL_checkstring can longjmp on bad input
    // (e.g. clear({})), which would skip the ~std::optional/~path destructors.
    const bool has_path_arg = lua_gettop(state) >= 1 && !lua_isnil(state, 1);
    if (has_path_arg && lua_type(state, 1) != LUA_TSTRING) {
      lua_error_util::PushMessage(state, "diagnostics.clear requires a path string or nil");
      return lua_error(state);
    }
    int rc = 0;
    {
      std::optional<std::filesystem::path> path;
      if (has_path_arg) {
        const char* raw_path = lua_tostring(state, 1);
        path = path_interop::ResolveRuntimePath(
            host != nullptr ? host->current_project_root : EmptyProjectRoot(),
            std::filesystem::path(raw_path != nullptr ? raw_path : ""));
      }
      rc = runtime_api_interop::LuaDiagnosticsClear(
          state, plugin, path, host != nullptr ? host->callbacks : EmptyCallbacks());
    }
    return rc == lua_error_util::kPendingError ? lua_error(state) : rc;
  }

  static int LuaDecorationsSet(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    const int rc = runtime_api_interop::LuaDecorationsSet(
        state, plugin, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
    return rc == lua_error_util::kPendingError ? lua_error(state) : rc;
  }

  static int LuaDecorationsClear(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    // Validate the optional path argument with non-raising checks BEFORE any
    // non-trivial C++ local exists. luaL_checkstring can longjmp on bad input
    // (e.g. clear({})), which would skip the ~std::optional/~path destructors.
    const bool has_path_arg = lua_gettop(state) >= 1 && !lua_isnil(state, 1);
    if (has_path_arg && lua_type(state, 1) != LUA_TSTRING) {
      lua_error_util::PushMessage(state, "decorations.clear requires a path string or nil");
      return lua_error(state);
    }
    int rc = 0;
    {
      std::optional<std::filesystem::path> path;
      if (has_path_arg) {
        const char* raw_path = lua_tostring(state, 1);
        path = path_interop::ResolveRuntimePath(
            host != nullptr ? host->current_project_root : EmptyProjectRoot(),
            std::filesystem::path(raw_path != nullptr ? raw_path : ""));
      }
      rc = runtime_api_interop::LuaDecorationsClear(
          state, plugin, path, host != nullptr ? host->callbacks : EmptyCallbacks());
    }
    return rc == lua_error_util::kPendingError ? lua_error(state) : rc;
  }

  static int LuaSurfaceSet(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    const int rc = runtime_api_interop::LuaSurfaceSet(
        state, plugin, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
    return rc == lua_error_util::kPendingError ? lua_error(state) : rc;
  }

  static int LuaSurfaceClear(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    const int rc = runtime_api_interop::LuaSurfaceClear(
        state, plugin, host != nullptr ? host->callbacks : EmptyCallbacks());
    return rc == lua_error_util::kPendingError ? lua_error(state) : rc;
  }

  static int LuaSidebarShow(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return runtime_api_interop::LuaSidebarShow(
        state, host != nullptr ? host->callbacks : EmptyCallbacks());
  }

  // ctx.editor.* helpers never raise (they push (false, message) on failure), so
  // they need no kPendingError handling — the interop already pushed every result.
  static int LuaEditorApplyEdits(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    return runtime_api_interop::LuaEditorApplyEdits(
        state, plugin, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
  }

  static int LuaEditorSetCursor(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    return runtime_api_interop::LuaEditorSetCursor(
        state, plugin, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
  }

  static int LuaEditorSetSelection(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    return runtime_api_interop::LuaEditorSetSelection(
        state, plugin, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
  }

  static int LuaEditorSetGhostText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    return runtime_api_interop::LuaEditorSetGhostText(
        state, plugin, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
  }

  static int LuaEditorClearGhostText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    return runtime_api_interop::LuaEditorClearGhostText(
        state, plugin, host != nullptr ? host->current_project_root : EmptyProjectRoot(),
        host != nullptr ? host->callbacks : EmptyCallbacks());
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
            .workspace_data_dir = &LuaWorkspaceDataDir,
            .files_read_text = &LuaFilesReadText,
            .files_write_text = &LuaFilesWriteText,
            .files_exists = &LuaFilesExists,
            .process_run = &LuaProcessRun,
            .process_run_async = &LuaProcessRunAsync,
            .diagnostics_publish = &LuaDiagnosticsPublish,
            .diagnostics_clear = &LuaDiagnosticsClear,
            .decorations_set = &LuaDecorationsSet,
            .decorations_clear = &LuaDecorationsClear,
            .surface_set = &LuaSurfaceSet,
            .surface_clear = &LuaSurfaceClear,
            .editor_apply_edits = &LuaEditorApplyEdits,
            .editor_set_cursor = &LuaEditorSetCursor,
            .editor_set_selection = &LuaEditorSetSelection,
            .editor_set_ghost_text = &LuaEditorSetGhostText,
            .editor_clear_ghost_text = &LuaEditorClearGhostText,
            .sidebar_add = &LuaSidebarAdd,
            .sidebar_show = &LuaSidebarShow,
            .hover_add = &LuaHoverAdd,
            .menus_add = &LuaMenusAdd,
            .keybindings_add = &LuaKeybindingsAdd,
            .settings_declare = &LuaSettingsDeclare,
            .settings_get = &LuaSettingsGet,
            .status_add = &LuaStatusAdd,
            .status_update = &LuaStatusUpdate,
            .themes_register = &LuaThemeRegister,
            .file_icons_register = &LuaFileIconThemeRegister,
            .formatters_add = &LuaFormattersAdd,
            .save_participants_add = &LuaSaveParticipantsAdd,
            .virtual_documents_add = &LuaVirtualDocumentsAdd,
            .completion_add = &LuaCompletionAdd,
            .code_action_add = &LuaCodeActionAdd,
            .definition_add = &LuaDefinitionAdd,
            .references_add = &LuaReferencesAdd,
            .signature_help_add = &LuaSignatureHelpAdd,
            .document_symbols_add = &LuaDocumentSymbolsAdd,
            .lsp_add = &LuaLspAdd,
            .debug_add = &LuaDebugAdd,
            .debug_add_config = &LuaDebugAddConfig,
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

  bool ToggleSidebarProviderItem(const SidebarProvider& provider,
                                 const SidebarItem& item,
                                 std::string* error_message) {
    return sidebar_hover_interop::ToggleSidebarProviderItem(
        provider, item, current_project_root,
        [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
          return path_interop::ResolveRuntimePath(project_root, runtime_path);
        },
        [this](lua_State* state) { return FindPluginByState(state); }, error_message);
  }

  std::vector<PluginHost::LocationResult> QueryLanguageLocations(
      runtime_types::LanguageQueryKind kind,
      std::string_view language_id,
      const std::filesystem::path& path,
      std::size_t line,
      std::size_t column,
      bool include_declaration,
      std::string* error_message) {
    return language_provider_query_interop::QueryLocations(
        kind, language_id, path, line, column, include_declaration, current_project_root,
        language_query_runtimes,
        [this](lua_State* state) { return FindPluginByState(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          PushBufferContext(state, buffer_path);
        },
        [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
          return path_interop::ResolveRuntimePath(project_root, runtime_path);
        },
        error_message);
  }

  bool QueryLanguageSignatureHelp(std::string_view language_id,
                                  const std::filesystem::path& path,
                                  std::size_t line,
                                  std::size_t column,
                                  PluginHost::SignatureHelpResult* result,
                                  std::string* error_message) {
    return language_provider_query_interop::QuerySignatureHelp(
        language_id, path, line, column, language_query_runtimes,
        [this](lua_State* state) { return FindPluginByState(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          PushBufferContext(state, buffer_path);
        },
        result, error_message);
  }

  std::vector<PluginHost::DocumentSymbolNode> QueryLanguageDocumentSymbols(
      std::string_view language_id,
      const std::filesystem::path& path,
      std::string* error_message) {
    return language_provider_query_interop::QueryDocumentSymbols(
        language_id, path, language_query_runtimes,
        [this](lua_State* state) { return FindPluginByState(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          PushBufferContext(state, buffer_path);
        },
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

  // `plugin` is passed explicitly rather than resolved via FindPluginByState: during
  // setup-failure cleanup the failing plugin is not yet in `plugins` (and active_plugin
  // was reset), so FindPluginByState would return nullptr and the teardown would skip
  // every plugin_id-keyed container and runtime-provider vector — leaving them pointing
  // at the lua_State we are about to destroy (use-after-free on the next query).
  void UnregisterContributionsForState(lua_State* state, const PluginInstance* plugin) {
    state_teardown_interop::UnregisterContributionsForState(
        state, plugin, &commands, &command_names, &sidebars, &sidebar_providers,
        &hovers, &hover_provider_order, &menu_entries, &keybindings, &settings, &status_items,
        &status_item_order, &formatters, &save_participants, &virtual_documents,
        &save_participant_runtimes,
        &completions, &completion_runtimes, &code_actions, &code_action_runtimes,
        &language_query_runtimes,
        &language_servers, &debug_adapters, &launch_configs, &tasks, &tools, &test_providers,
        &test_provider_runtimes,
        &scm_providers, &scm_provider_runtimes, &annotation_providers,
        &annotation_provider_runtimes, &auth_providers, &auth_provider_runtimes,
        &bracket_sets, &comment_markers, &indent_rules, &snippets, &themes, &file_icon_themes);
    // The teardown remove_if above dropped one plugin's entries from the five
    // id-deduplicated vectors; re-sync their O(1) duplicate-id indexes from the
    // survivors so a later reload/registration sees exactly the live ids (see
    // TD-2026-07-17-077).
    RebuildContributionIdIndexes();
    // Teardown runs on the worker during reload; the publish at the end of the reload
    // copies the (now-empty or rebuilt) status_item_order into published_ and bumps
    // the UI-visible status_view_revision, so no separate invalidation is needed here.
  }

  // Rebuild the five contribution id indexes from their storage vectors. Cheap
  // (these kinds hold a handful of entries in practice) and the simplest way to
  // keep the indexes exact after a per-plugin remove_if teardown that does not
  // report which ids it removed. Empty ids are excluded, matching registration.
  void RebuildContributionIdIndexes() {
    const auto rebuild = [](std::unordered_set<std::string>* index, const auto& items) {
      index->clear();
      for (const auto& item : items) {
        if (!item.id.empty()) {
          index->insert(item.id);
        }
      }
    };
    rebuild(&language_server_ids, language_servers);
    rebuild(&debug_adapter_ids, debug_adapters);
    rebuild(&launch_config_ids, launch_configs);
    rebuild(&task_ids, tasks);
    rebuild(&tool_ids, tools);
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

  void CallBufferEventCallback(PluginInstance* plugin,
                               int ref,
                               const char* callback_name,
                               const std::filesystem::path& path,
                               const std::function<void(lua_State*)>& push_payload) {
    lifecycle_callback_interop::CallBufferEventCallback(
        plugin, ref, callback_name, path,
        [this](lua_State* state) { PushPluginContext(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          PushBufferTable(state, buffer_path);
        },
        push_payload, [this](std::string error) { RecordError(std::move(error)); },
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
        [this](lua_State* state) {
          UnregisterContributionsForState(state, FindPluginByState(state));
        },
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
        // Setup-failure cleanup: pass the live plugin directly so plugin_id-keyed
        // contributions and runtime-provider vectors are torn down before its
        // lua_State is destroyed (FindPluginByState can't see it yet).
        [this](const PluginInstance& plugin) {
          UnregisterContributionsForState(plugin.state, &plugin);
        },
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
