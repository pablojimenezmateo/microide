bool PluginHost::enabled() const {
  return impl_->enabled();
}

void PluginHost::SetStartupPluginsEnabled(bool enabled) {
  impl_->startup_plugins_enabled = enabled;
}

bool PluginHost::Reload(const std::filesystem::path& project_root) {
  impl_->errors.clear();
  if (!impl_->enabled()) {
    lifecycle_reset_interop::ResetForDisabledRuntime(
        project_root, &impl_->current_project_root, &impl_->commands, &impl_->command_names,
        &impl_->sidebars, &impl_->sidebar_providers, &impl_->hovers, &impl_->hover_provider_order,
        &impl_->plugins);
    impl_->SetReloadSummary();
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const std::filesystem::path next_project_root =
      project_root.empty() ? std::filesystem::path{} : project_root.lexically_normal();
  // Discovery is pure filesystem work; keep it on the calling thread so a project
  // with no plugins never spawns the worker (zero-cost-when-unused).
  std::vector<std::filesystem::path> plugin_roots = impl_->DiscoverPluginRoots();
  // The whole load — teardown, state creation, setup, project-open — touches
  // lua_States, so it must run on the worker whenever one is (or becomes) live.
  // When there is genuinely nothing to run, stay inline and leave the worker unspawned.
  const bool needs_worker =
      impl_->worker_ != nullptr &&
      (impl_->worker_->started() || !plugin_roots.empty() || !impl_->plugins.empty());

  auto load_body = [impl = impl_.get(), next_project_root,
                    plugin_roots = std::move(plugin_roots)]() {
    impl->TearDownPlugins();
    impl->disabled_plugin_meta.clear();
    impl->current_project_root = next_project_root;

    for (const auto& plugin_root : plugin_roots) {
      std::string error_message;
      if (!impl->LoadPluginRoot(plugin_root, &error_message) && !error_message.empty()) {
        impl->RecordError(std::move(error_message));
      }
    }
    if (!impl->current_project_root.empty()) {
      project_lifecycle_interop::DispatchProjectOpenCallbacks(
          &impl->plugins, [impl](runtime_types::PluginInstance* plugin, int callback_ref,
                                 const char* callback_name) {
            impl->CallProjectCallback(plugin, callback_ref, callback_name);
          });
    }
  };

  PluginHostSnapshot snapshot = impl_->CaptureSnapshot();
  snapshot.project_root = next_project_root;
  if (needs_worker) {
    impl_->RunOnWorkerBlocking(snapshot, load_body);
  } else {
    impl_->ExecuteWithContext(&snapshot, /*direct=*/true, load_body);
  }
#endif

  impl_->SetReloadSummary();
  return impl_->errors.empty();
}

void PluginHost::Shutdown() {
  if (!impl_->enabled()) {
    lifecycle_reset_interop::ShutdownForDisabledRuntime(
        &impl_->current_project_root, &impl_->commands, &impl_->command_names, &impl_->sidebars,
        &impl_->sidebar_providers, &impl_->hovers, &impl_->hover_provider_order, &impl_->plugins);
    impl_->SetReloadSummary();
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  impl_->TearDownPlugins();
#endif
  impl_->current_project_root.clear();
  impl_->SetReloadSummary();
}

void PluginHost::OnBufferOpen(const std::filesystem::path& path) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerDetached(impl_->CaptureSnapshot(), [impl = impl_.get(), path]() {
    buffer_lifecycle_interop::DispatchBufferCallbacks(
        &impl->plugins, &runtime_types::PluginInstance::on_buffer_open_ref, "on_buffer_open", path,
        [impl](runtime_types::PluginInstance* plugin, int callback_ref, const char* callback_name,
               const std::filesystem::path& callback_path) {
          impl->CallBufferCallback(plugin, callback_ref, callback_name, callback_path);
        });
  });
#endif
}

void PluginHost::OnBufferSave(const std::filesystem::path& path) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerDetached(impl_->CaptureSnapshot(), [impl = impl_.get(), path]() {
    buffer_lifecycle_interop::DispatchBufferCallbacks(
        &impl->plugins, &runtime_types::PluginInstance::on_buffer_save_ref, "on_buffer_save", path,
        [impl](runtime_types::PluginInstance* plugin, int callback_ref, const char* callback_name,
               const std::filesystem::path& callback_path) {
          impl->CallBufferCallback(plugin, callback_ref, callback_name, callback_path);
        });
  });
#endif
}

PluginHost::EditorEventInterest PluginHost::EditorEventInterests() const {
  EditorEventInterest interest;
#if MICROIDE_HAS_LUA_PLUGINS
  for (const auto& plugin : impl_->plugins) {
    interest.buffer_change |= plugin.on_buffer_change_ref != LUA_NOREF;
    interest.cursor_move |= plugin.on_cursor_move_ref != LUA_NOREF;
    interest.selection_change |= plugin.on_selection_change_ref != LUA_NOREF;
    interest.buffer_close |= plugin.on_buffer_close_ref != LUA_NOREF;
  }
#endif
  return interest;
}

void PluginHost::OnBufferChange(const std::filesystem::path& path,
                                std::size_t start_line,
                                std::size_t end_line) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerDetached(impl_->CaptureSnapshot(),
                             [impl = impl_.get(), path, start_line, end_line]() {
    for (auto& plugin : impl->plugins) {
      impl->CallBufferEventCallback(
          &plugin, plugin.on_buffer_change_ref, "on_buffer_change", path,
          [start_line, end_line](lua_State* state) {
            lua_createtable(state, 0, 2);
            lua_pushinteger(state, static_cast<lua_Integer>(start_line));
            lua_setfield(state, -2, "start_line");
            lua_pushinteger(state, static_cast<lua_Integer>(end_line));
            lua_setfield(state, -2, "end_line");
          });
    }
  });
#else
  (void)start_line;
  (void)end_line;
#endif
}

void PluginHost::OnCursorMove(const std::filesystem::path& path,
                              std::size_t line,
                              std::size_t column) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerDetached(impl_->CaptureSnapshot(),
                             [impl = impl_.get(), path, line, column]() {
    for (auto& plugin : impl->plugins) {
      impl->CallBufferEventCallback(
          &plugin, plugin.on_cursor_move_ref, "on_cursor_move", path,
          [line, column](lua_State* state) {
            lua_createtable(state, 0, 2);
            lua_pushinteger(state, static_cast<lua_Integer>(line));
            lua_setfield(state, -2, "line");
            lua_pushinteger(state, static_cast<lua_Integer>(column));
            lua_setfield(state, -2, "col");
          });
    }
  });
#else
  (void)line;
  (void)column;
#endif
}

void PluginHost::OnSelectionChange(const std::filesystem::path& path,
                                   bool has_selection,
                                   std::size_t start_line,
                                   std::size_t start_column,
                                   std::size_t end_line,
                                   std::size_t end_column) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerDetached(
      impl_->CaptureSnapshot(),
      [impl = impl_.get(), path, has_selection, start_line, start_column, end_line, end_column]() {
        for (auto& plugin : impl->plugins) {
          impl->CallBufferEventCallback(
              &plugin, plugin.on_selection_change_ref, "on_selection_change", path,
              [has_selection, start_line, start_column, end_line, end_column](lua_State* state) {
                if (!has_selection) {
                  lua_pushnil(state);
                  return;
                }
                lua_createtable(state, 0, 4);
                lua_pushinteger(state, static_cast<lua_Integer>(start_line));
                lua_setfield(state, -2, "start_line");
                lua_pushinteger(state, static_cast<lua_Integer>(start_column));
                lua_setfield(state, -2, "start_col");
                lua_pushinteger(state, static_cast<lua_Integer>(end_line));
                lua_setfield(state, -2, "end_line");
                lua_pushinteger(state, static_cast<lua_Integer>(end_column));
                lua_setfield(state, -2, "end_col");
              });
        }
      });
#else
  (void)has_selection;
  (void)start_line;
  (void)start_column;
  (void)end_line;
  (void)end_column;
#endif
}

void PluginHost::OnBufferClose(const std::filesystem::path& path) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerDetached(impl_->CaptureSnapshot(), [impl = impl_.get(), path]() {
    buffer_lifecycle_interop::DispatchBufferCallbacks(
        &impl->plugins, &runtime_types::PluginInstance::on_buffer_close_ref, "on_buffer_close",
        path,
        [impl](runtime_types::PluginInstance* plugin, int callback_ref, const char* callback_name,
               const std::filesystem::path& callback_path) {
          impl->CallBufferCallback(plugin, callback_ref, callback_name, callback_path);
        });
  });
#endif
}

bool PluginHost::ExecuteCommand(std::string_view name,
                                const std::vector<std::string>& args,
                                std::string* error_message,
                                std::string* feedback) {
  if (feedback != nullptr) {
    feedback->clear();
  }
  if (!impl_->enabled()) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  bool executed = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    executed = provider_query_interop::ExecuteCommand(
        name, args, impl_->commands,
        [this](lua_State* state) { return impl_->FindPluginByState(state); },
        [this](lua_State* state) { impl_->PushPluginContext(state); }, error_message, feedback);
  });
  return executed;
#else
  (void)name;
  (void)args;
  if (error_message != nullptr) {
    error_message->clear();
  }
  return false;
#endif
}

const std::vector<std::string>& PluginHost::CommandNames() const {
#if MICROIDE_HAS_LUA_PLUGINS
  // Lazy rebuild: registration only marks the ordered view dirty, so the sort runs
  // at most once per batch of registrations, on the first read after a change.
  if (impl_->command_names_dirty) {
    registry_interop::RebuildCommandNames(impl_->commands, &impl_->command_names);
    impl_->command_names_dirty = false;
  }
#endif
  return impl_->command_names;
}

const std::vector<PluginHost::SidebarProviderInfo>& PluginHost::SidebarProviders() const {
#if MICROIDE_HAS_LUA_PLUGINS
  if (impl_->sidebar_providers_dirty) {
    registry_interop::RebuildSidebarProviders(impl_->sidebars, &impl_->sidebar_providers);
    impl_->sidebar_providers_dirty = false;
  }
#endif
  return impl_->sidebar_providers;
}

const PluginHost::SidebarProviderInfo* PluginHost::FindSidebarProvider(std::string_view id) const {
  const auto it = impl_->sidebars.find(std::string(id));
  return it == impl_->sidebars.end() ? nullptr : &it->second.info;
}

bool PluginHost::SnapshotSidebar(std::string_view id,
                                 std::vector<SidebarItem>* items,
                                 std::string* error_message) {
  const auto it = impl_->sidebars.find(std::string(id));
  if (it == impl_->sidebars.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown plugin sidebar: " + std::string(id);
    }
    if (items != nullptr) {
      items->clear();
    }
    return false;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = impl_->SnapshotSidebarProvider(it->second, items, error_message);
  });
  return ok;
#else
  if (items != nullptr) {
    items->clear();
  }
  if (error_message != nullptr) {
    *error_message = "Lua plugin runtime unavailable";
  }
  return false;
#endif
}

bool PluginHost::ConfirmSidebarItem(std::string_view id,
                                    const SidebarItem& item,
                                    std::string* error_message) {
  const auto it = impl_->sidebars.find(std::string(id));
  if (it == impl_->sidebars.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown plugin sidebar: " + std::string(id);
    }
    return false;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = impl_->ConfirmSidebarProviderItem(it->second, item, error_message);
  });
  return ok;
#else
  (void)item;
  if (error_message != nullptr) {
    *error_message = "Lua plugin runtime unavailable";
  }
  return false;
#endif
}

bool PluginHost::ToggleSidebarItem(std::string_view id,
                                   const SidebarItem& item,
                                   std::string* error_message) {
  const auto it = impl_->sidebars.find(std::string(id));
  if (it == impl_->sidebars.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown plugin sidebar: " + std::string(id);
    }
    return false;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = impl_->ToggleSidebarProviderItem(it->second, item, error_message);
  });
  return ok;
#else
  (void)item;
  if (error_message != nullptr) {
    error_message->clear();
  }
  return false;
#endif
}

bool PluginHost::QueryHover(const std::filesystem::path& path,
                            std::size_t line,
                            std::size_t column,
                            HoverResult* result,
                            std::string* error_message) const {
  if (result == nullptr) {
    if (error_message != nullptr) {
      *error_message = "hover result output pointer is required";
    }
    return false;
  }
  result->title.clear();
  result->content.clear();

  if (!impl_->enabled() || path.empty() || line == 0 || column == 0) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return false;
  }

  const std::filesystem::path resolved_path =
      path_interop::ResolveRuntimePath(impl_->current_project_root, path).lexically_normal();
#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = hover_query_interop::QueryHover(
        impl_->hover_provider_order, impl_->hovers, resolved_path, line, column,
        [this](const runtime_types::HoverProvider& provider,
               const std::filesystem::path& hover_path, std::size_t hover_line,
               std::size_t hover_column, HoverResult* hover_result,
               std::string* hover_error_message) {
          return impl_->QueryHoverProvider(provider, hover_path, hover_line, hover_column,
                                           hover_result, hover_error_message);
        },
        result, error_message);
  });
  return ok;
#else
  (void)resolved_path;
#endif

  if (error_message != nullptr) {
    error_message->clear();
  }
  return false;
}

std::vector<std::filesystem::path> PluginHost::DataDirectories(std::string_view subdirectory) const {
  return data_directory_interop::DataDirectories(subdirectory, impl_->plugins);
}

const std::vector<PluginHost::ContributedMenuEntry>& PluginHost::ContributedMenuEntries() const {
  return impl_->menu_entries;
}

const std::vector<PluginHost::ContributedKeybinding>& PluginHost::ContributedKeybindings() const {
  return impl_->keybindings;
}

const std::vector<PluginHost::ContributedSettingSpec>& PluginHost::ContributedSettings() const {
  return impl_->settings;
}

const std::vector<PluginHost::ContributedStatusItem>& PluginHost::ContributedStatusItems() const {
  return impl_->status_item_order;
}

std::uint64_t PluginHost::StatusItemsRevision() const {
  return impl_->status_items_revision;
}

bool PluginHost::UpdateStatusItem(std::string_view id, std::string text, std::string tooltip) {
  const bool changed = status_interop::UpdateStatusItem(
      id, std::move(text), std::move(tooltip), &impl_->status_items, &impl_->status_item_order,
      impl_->callbacks.request_status_redraw);
  if (changed) {
    ++impl_->status_items_revision;
  }
  return changed;
}

bool PluginHost::RunSaveParticipants(const std::filesystem::path& path,
                                     std::string* text,
                                     std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (text == nullptr) {
    if (error_message != nullptr) {
      *error_message = "save participants require mutable text";
    }
    return false;
  }
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = provider_query_interop::RunSaveParticipants(
        path, text, impl_->save_participant_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path, std::string_view value) {
          impl_->PushBufferContext(state, buffer_path, value);
        },
        error_message);
  });
  return ok;
#else
  (void)path;
  (void)text;
  return true;
#endif
}

std::vector<PluginHost::CompletionCandidate> PluginHost::QueryCompletions(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    std::string_view trigger_character,
    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }

  std::vector<CompletionCandidate> results;
  if (!impl_->enabled()) {
    return results;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    results = provider_query_interop::QueryCompletions(
        language_id, path, line, column, trigger_character, impl_->completion_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          impl_->PushBufferContext(state, buffer_path);
        },
        error_message);
  });
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
  (void)trigger_character;
#endif
  return results;
}

void PluginHost::QueryCompletionsAsync(
    std::string language_id,
    std::filesystem::path path,
    std::size_t line,
    std::size_t column,
    std::string trigger_character,
    std::function<void(std::vector<CompletionCandidate>, std::string)> on_result) {
  if (!impl_->enabled()) {
    on_result({}, {});
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  struct QueryResult {
    std::vector<CompletionCandidate> items;
    std::string error;
  };
  impl_->RunQueryAsync<QueryResult>(
      "query:completions",
      [this, language_id = std::move(language_id), path = std::move(path), line, column,
       trigger_character = std::move(trigger_character)]() {
        QueryResult result;
        result.items = provider_query_interop::QueryCompletions(
            language_id, path, line, column, trigger_character, impl_->completion_runtimes,
            [this](lua_State* state) { return impl_->FindPluginByState(state); },
            [this](lua_State* state, const std::filesystem::path& buffer_path) {
              impl_->PushBufferContext(state, buffer_path);
            },
            &result.error);
        return result;
      },
      [on_result = std::move(on_result)](QueryResult result) {
        on_result(std::move(result.items), std::move(result.error));
      });
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
  (void)trigger_character;
  on_result({}, {});
#endif
}

void PluginHost::QueryCodeActionsAsync(
    std::string language_id,
    std::filesystem::path path,
    std::size_t start_line,
    std::size_t start_column,
    std::size_t end_line,
    std::size_t end_column,
    std::function<void(std::vector<CodeActionCandidate>, std::string)> on_result) {
  if (!impl_->enabled()) {
    on_result({}, {});
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  struct QueryResult {
    std::vector<CodeActionCandidate> items;
    std::string error;
  };
  impl_->RunQueryAsync<QueryResult>(
      "query:code_actions",
      [this, language_id = std::move(language_id), path = std::move(path), start_line, start_column,
       end_line, end_column]() {
        QueryResult result;
        result.items = provider_query_interop::QueryCodeActions(
            language_id, path, start_line, start_column, end_line, end_column,
            impl_->code_action_runtimes,
            [this](lua_State* state) { return impl_->FindPluginByState(state); },
            [this](lua_State* state, const std::filesystem::path& buffer_path) {
              impl_->PushBufferContext(state, buffer_path);
            },
            &result.error);
        return result;
      },
      [on_result = std::move(on_result)](QueryResult result) {
        on_result(std::move(result.items), std::move(result.error));
      });
#else
  (void)language_id;
  (void)path;
  (void)start_line;
  (void)start_column;
  (void)end_line;
  (void)end_column;
  on_result({}, {});
#endif
}

void PluginHost::QueryDefinitionAsync(
    std::string language_id,
    std::filesystem::path path,
    std::size_t line,
    std::size_t column,
    std::function<void(std::vector<LocationResult>, std::string)> on_result) {
  if (!impl_->enabled()) {
    on_result({}, {});
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  struct QueryResult {
    std::vector<LocationResult> items;
    std::string error;
  };
  impl_->RunQueryAsync<QueryResult>(
      "query:definition",
      [this, language_id = std::move(language_id), path = std::move(path), line, column]() {
        QueryResult result;
        result.items = impl_->QueryLanguageLocations(runtime_types::LanguageQueryKind::Definition,
                                                     language_id, path, line, column, false,
                                                     &result.error);
        return result;
      },
      [on_result = std::move(on_result)](QueryResult result) {
        on_result(std::move(result.items), std::move(result.error));
      });
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
  on_result({}, {});
#endif
}

void PluginHost::QueryReferencesAsync(
    std::string language_id,
    std::filesystem::path path,
    std::size_t line,
    std::size_t column,
    bool include_declaration,
    std::function<void(std::vector<LocationResult>, std::string)> on_result) {
  if (!impl_->enabled()) {
    on_result({}, {});
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  struct QueryResult {
    std::vector<LocationResult> items;
    std::string error;
  };
  impl_->RunQueryAsync<QueryResult>(
      "query:references",
      [this, language_id = std::move(language_id), path = std::move(path), line, column,
       include_declaration]() {
        QueryResult result;
        result.items = impl_->QueryLanguageLocations(runtime_types::LanguageQueryKind::References,
                                                     language_id, path, line, column,
                                                     include_declaration, &result.error);
        return result;
      },
      [on_result = std::move(on_result)](QueryResult result) {
        on_result(std::move(result.items), std::move(result.error));
      });
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
  (void)include_declaration;
  on_result({}, {});
#endif
}

void PluginHost::QuerySignatureHelpAsync(
    std::string language_id,
    std::filesystem::path path,
    std::size_t line,
    std::size_t column,
    std::function<void(bool, SignatureHelpResult, std::string)> on_result) {
  if (!impl_->enabled()) {
    on_result(false, SignatureHelpResult{}, {});
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  struct QueryResult {
    bool resolved = false;
    SignatureHelpResult result;
    std::string error;
  };
  impl_->RunQueryAsync<QueryResult>(
      "query:signature_help",
      [this, language_id = std::move(language_id), path = std::move(path), line, column]() {
        QueryResult result;
        result.resolved = impl_->QueryLanguageSignatureHelp(language_id, path, line, column,
                                                            &result.result, &result.error);
        return result;
      },
      [on_result = std::move(on_result)](QueryResult result) {
        on_result(result.resolved, std::move(result.result), std::move(result.error));
      });
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
  on_result(false, SignatureHelpResult{}, {});
#endif
}

std::vector<PluginHost::CodeActionCandidate> PluginHost::QueryCodeActions(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t start_column,
    std::size_t end_line,
    std::size_t end_column,
    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }

  std::vector<CodeActionCandidate> results;
  if (!impl_->enabled()) {
    return results;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    results = provider_query_interop::QueryCodeActions(
        language_id, path, start_line, start_column, end_line, end_column,
        impl_->code_action_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          impl_->PushBufferContext(state, buffer_path);
        },
        error_message);
  });
#else
  (void)language_id;
  (void)path;
  (void)start_line;
  (void)start_column;
  (void)end_line;
  (void)end_column;
#endif
  return results;
}

std::vector<PluginHost::LocationResult> PluginHost::QueryDefinition(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  std::vector<LocationResult> results;
  if (!impl_->enabled()) {
    return results;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    results = impl_->QueryLanguageLocations(runtime_types::LanguageQueryKind::Definition,
                                            language_id, path, line, column, false, error_message);
  });
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
#endif
  return results;
}

std::vector<PluginHost::LocationResult> PluginHost::QueryReferences(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    bool include_declaration,
    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  std::vector<LocationResult> results;
  if (!impl_->enabled()) {
    return results;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    results = impl_->QueryLanguageLocations(runtime_types::LanguageQueryKind::References,
                                            language_id, path, line, column, include_declaration,
                                            error_message);
  });
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
  (void)include_declaration;
#endif
  return results;
}

bool PluginHost::QuerySignatureHelp(std::string_view language_id,
                                    const std::filesystem::path& path,
                                    std::size_t line,
                                    std::size_t column,
                                    SignatureHelpResult* result,
                                    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (result == nullptr || !impl_->enabled()) {
    return false;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = impl_->QueryLanguageSignatureHelp(language_id, path, line, column, result, error_message);
  });
  return ok;
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
  return false;
#endif
}

std::vector<PluginHost::DocumentSymbolNode> PluginHost::QueryDocumentSymbols(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  std::vector<DocumentSymbolNode> results;
  if (!impl_->enabled()) {
    return results;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    results = impl_->QueryLanguageDocumentSymbols(language_id, path, error_message);
  });
#else
  (void)language_id;
  (void)path;
#endif
  return results;
}

bool PluginHost::DiscoverTests(std::string_view provider_id,
                               const std::filesystem::path& path,
                               std::vector<TestCase>* tests,
                               std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (tests == nullptr) {
    if (error_message != nullptr) {
      *error_message = "test discovery requires an output vector";
    }
    return false;
  }
  tests->clear();
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = provider_query_interop::DiscoverTests(
        provider_id, path, impl_->current_project_root, impl_->test_provider_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          impl_->PushBufferContext(state, buffer_path);
        },
        [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
          return path_interop::ResolveRuntimePath(project_root, runtime_path);
        },
        tests, error_message);
  });
  return ok;
#else
  (void)provider_id;
  (void)path;
  return false;
#endif
}

bool PluginHost::RunTests(std::string_view provider_id,
                          const std::vector<std::string>& test_ids,
                          std::vector<TestRunResult>* results,
                          std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (results == nullptr) {
    if (error_message != nullptr) {
      *error_message = "test execution requires an output vector";
    }
    return false;
  }
  results->clear();
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = provider_query_interop::RunTests(
        provider_id, test_ids, impl_->test_provider_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); }, results,
        error_message);
  });
  return ok;
#else
  (void)provider_id;
  (void)test_ids;
  return false;
#endif
}

bool PluginHost::SnapshotScm(std::string_view provider_id,
                             ScmSnapshot* snapshot,
                             std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (snapshot == nullptr) {
    if (error_message != nullptr) {
      *error_message = "scm snapshot requires an output value";
    }
    return false;
  }
  *snapshot = ScmSnapshot{};
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = provider_query_interop::SnapshotScm(
        provider_id, impl_->current_project_root, impl_->scm_provider_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); },
        [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
          return path_interop::ResolveRuntimePath(project_root, runtime_path);
        },
        snapshot, error_message);
  });
  return ok;
#else
  (void)provider_id;
  return false;
#endif
}

std::vector<PluginHost::AnnotationLine> PluginHost::QueryAnnotations(
    std::string_view provider_id,
    const std::filesystem::path& path,
    std::string_view language_id,
    std::size_t visible_start_line,
    std::size_t visible_end_line,
    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }

  std::vector<AnnotationLine> lines;
  if (!impl_->enabled()) {
    return lines;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    lines = provider_query_interop::QueryAnnotations(
        provider_id, path, language_id, visible_start_line, visible_end_line,
        impl_->annotation_provider_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          impl_->PushBufferContext(state, buffer_path);
        },
        error_message);
  });
#else
  (void)provider_id;
  (void)path;
  (void)language_id;
  (void)visible_start_line;
  (void)visible_end_line;
#endif
  return lines;
}

bool PluginHost::LoginAuthProvider(std::string_view provider_id,
                                   const std::vector<std::string>& scopes,
                                   AuthSessionData* session,
                                   std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (session == nullptr) {
    if (error_message != nullptr) {
      *error_message = "auth login requires an output session";
    }
    return false;
  }
  *session = AuthSessionData{};
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = provider_query_interop::LoginAuthProvider(
        provider_id, scopes, impl_->auth_provider_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); }, session,
        error_message);
  });
  return ok;
#else
  (void)provider_id;
  (void)scopes;
  return false;
#endif
}

bool PluginHost::RefreshAuthSession(std::string_view provider_id,
                                    std::string_view session_id,
                                    AuthSessionData* session,
                                    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (session == nullptr) {
    if (error_message != nullptr) {
      *error_message = "auth refresh requires an output session";
    }
    return false;
  }
  *session = AuthSessionData{};
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = provider_query_interop::RefreshAuthSession(
        provider_id, session_id, impl_->auth_provider_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); }, session,
        error_message);
  });
  return ok;
#else
  (void)provider_id;
  (void)session_id;
  return false;
#endif
}

bool PluginHost::LogoutAuthSession(std::string_view provider_id,
                                   std::string_view session_id,
                                   std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    ok = provider_query_interop::LogoutAuthSession(
        provider_id, session_id, impl_->auth_provider_runtimes,
        [this](lua_State* state) { return impl_->FindPluginByState(state); }, error_message);
  });
  return ok;
#else
  (void)provider_id;
  (void)session_id;
  return false;
#endif
}

const std::vector<PluginHost::ContributedFormatter>& PluginHost::ContributedFormatters() const {
  return impl_->formatters;
}

const std::vector<PluginHost::ContributedSaveParticipant>& PluginHost::ContributedSaveParticipants()
    const {
  return impl_->save_participants;
}

const std::vector<PluginHost::ContributedCompletion>& PluginHost::ContributedCompletions() const {
  return impl_->completions;
}

const std::vector<PluginHost::ContributedCodeAction>& PluginHost::ContributedCodeActions() const {
  return impl_->code_actions;
}

const std::vector<PluginHost::ContributedLanguageServer>&
PluginHost::ContributedLanguageServers() const {
  return impl_->language_servers;
}

const std::vector<PluginHost::ContributedDebugAdapter>&
PluginHost::ContributedDebugAdapters() const {
  return impl_->debug_adapters;
}

const std::vector<PluginHost::ContributedLaunchConfig>&
PluginHost::ContributedLaunchConfigs() const {
  return impl_->launch_configs;
}

const std::vector<PluginHost::ContributedTask>& PluginHost::ContributedTasks() const {
  return impl_->tasks;
}

const std::vector<PluginHost::ContributedTool>& PluginHost::ContributedTools() const {
  return impl_->tools;
}

const std::vector<PluginHost::ContributedTestProvider>& PluginHost::ContributedTestProviders()
    const {
  return impl_->test_providers;
}

const std::vector<PluginHost::ContributedScmProvider>& PluginHost::ContributedScmProviders()
    const {
  return impl_->scm_providers;
}

const std::vector<PluginHost::ContributedAnnotationProvider>&
PluginHost::ContributedAnnotationProviders() const {
  return impl_->annotation_providers;
}

const std::vector<PluginHost::ContributedAuthProvider>& PluginHost::ContributedAuthProviders()
    const {
  return impl_->auth_providers;
}

const std::vector<PluginHost::ContributedBracketSet>& PluginHost::ContributedBrackets() const {
  return impl_->bracket_sets;
}

const std::vector<PluginHost::ContributedCommentMarkers>& PluginHost::ContributedComments() const {
  return impl_->comment_markers;
}

const std::vector<PluginHost::ContributedIndentRules>& PluginHost::ContributedIndents() const {
  return impl_->indent_rules;
}

const std::vector<PluginHost::ContributedSnippet>& PluginHost::ContributedSnippets() const {
  return impl_->snippets;
}

const std::vector<PluginHost::ContributedTheme>& PluginHost::ContributedThemes() const {
  return impl_->themes;
}

const std::vector<PluginHost::ContributedFileIconTheme>& PluginHost::ContributedFileIconThemes()
    const {
  return impl_->file_icon_themes;
}

const std::vector<std::string>& PluginHost::Messages() const {
  return impl_->messages;
}

const std::vector<std::string>& PluginHost::Errors() const {
  return impl_->errors;
}

void PluginHost::ClearMessages() {
  impl_->ClearMessages();
}

std::string PluginHost::ReloadSummary() const {
  return impl_->reload_summary;
}

std::size_t PluginHost::LoadedPluginCount() const {
  return impl_->plugins.size();
}

void PluginHost::SetDisabledPlugins(std::vector<std::string> disabled_ids) {
  impl_->disabled_plugins = std::move(disabled_ids);
}

std::vector<PluginHost::LoadedPlugin> PluginHost::LoadedPlugins() const {
  std::vector<LoadedPlugin> result;
  result.reserve(impl_->plugins.size() + impl_->disabled_plugin_meta.size());
  for (const auto& plugin : impl_->plugins) {
    result.push_back(LoadedPlugin{.id = plugin.id, .root = plugin.root, .enabled = true});
  }
  for (const auto& meta : impl_->disabled_plugin_meta) {
    result.push_back(meta);
  }
  std::sort(result.begin(), result.end(),
            [](const LoadedPlugin& a, const LoadedPlugin& b) { return a.id < b.id; });
  return result;
}

}  // namespace microide::plugin
