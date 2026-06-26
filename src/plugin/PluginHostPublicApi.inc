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
    impl_->PublishContributionSnapshot();
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

  PluginHostSnapshot snapshot = impl_->CaptureSnapshot();
  snapshot.project_root = next_project_root;
  auto load_body = [impl = impl_.get(), next_project_root, plugin_roots]() {
    impl->RunReloadLoad(next_project_root, plugin_roots);
  };
  if (needs_worker) {
    // Synchronous round-trip: the UI thread parks until the worker finishes. Direct
    // callers (and the test suite) depend on Reload's synchronous bool result.
    impl_->RunOnWorkerBlocking(snapshot, load_body);
  } else {
    impl_->ExecuteWithContext(&snapshot, /*direct=*/true, /*allow_registration=*/true, load_body);
  }
#endif

  impl_->SetReloadSummary();
  impl_->PublishContributionSnapshot();
  return impl_->errors.empty();
}

void PluginHost::ReloadAsync(const std::filesystem::path& project_root,
                             std::function<void(bool)> on_complete) {
  impl_->errors.clear();
  if (!impl_->enabled()) {
    lifecycle_reset_interop::ResetForDisabledRuntime(
        project_root, &impl_->current_project_root, &impl_->commands, &impl_->command_names,
        &impl_->sidebars, &impl_->sidebar_providers, &impl_->hovers, &impl_->hover_provider_order,
        &impl_->plugins);
    impl_->SetReloadSummary();
    impl_->PublishContributionSnapshot();
    on_complete(false);
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const std::filesystem::path next_project_root =
      project_root.empty() ? std::filesystem::path{} : project_root.lexically_normal();
  std::vector<std::filesystem::path> plugin_roots = impl_->DiscoverPluginRoots();
  const bool needs_worker =
      impl_->worker_ != nullptr &&
      (impl_->worker_->started() || !plugin_roots.empty() || !impl_->plugins.empty());

  PluginHostSnapshot snapshot = impl_->CaptureSnapshot();
  snapshot.project_root = next_project_root;
  if (!needs_worker) {
    // Nothing to run on a worker: stay inline and complete synchronously, exactly like
    // the no-plugin Reload path. Keeps no-worker callers (and most tests) synchronous.
    impl_->ExecuteWithContext(&snapshot, /*direct=*/true, /*allow_registration=*/true,
                              [&]() { impl_->RunReloadLoad(next_project_root, plugin_roots); });
    impl_->SetReloadSummary();
    impl_->PublishContributionSnapshot();
    on_complete(impl_->errors.empty());
    return;
  }
  // Detached: the worker runs the Lua load while the UI thread keeps painting; the
  // rebuilt snapshot is published and on_complete fires during the next mailbox drain.
  impl_->ReloadDetached(std::move(snapshot), std::move(plugin_roots), std::move(on_complete));
#else
  impl_->SetReloadSummary();
  impl_->PublishContributionSnapshot();
  on_complete(true);
#endif
}

void PluginHost::Shutdown() {
  if (!impl_->enabled()) {
    lifecycle_reset_interop::ShutdownForDisabledRuntime(
        &impl_->current_project_root, &impl_->commands, &impl_->command_names, &impl_->sidebars,
        &impl_->sidebar_providers, &impl_->hovers, &impl_->hover_provider_order, &impl_->plugins);
    impl_->SetReloadSummary();
    impl_->PublishContributionSnapshot();
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  impl_->TearDownPlugins();
#endif
  impl_->current_project_root.clear();
  impl_->SetReloadSummary();
  impl_->PublishContributionSnapshot();
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
  // Published (race-free) value; recomputed from the live `plugins` vector each time a
  // snapshot is built on the worker.
  return impl_->published_.editor_event_interests;
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

bool PluginHost::HasCommand(std::string_view name) const {
#if MICROIDE_HAS_LUA_PLUGINS
  if (!impl_->enabled()) {
    return false;
  }
  // Probe the published command-name view (sorted, UI-thread-owned) instead of the
  // live `commands` map the worker rebuilds on reload.
  return std::binary_search(impl_->published_.command_names.begin(),
                            impl_->published_.command_names.end(), name);
#else
  (void)name;
  return false;
#endif
}

void PluginHost::ExecuteCommandAsync(
    std::string name,
    std::vector<std::string> args,
    std::function<void(bool, std::string, std::string)> on_result) {
  if (!impl_->enabled()) {
    on_result(false, {}, {});
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  struct CommandResult {
    bool ran = false;
    std::string error;
    std::string feedback;
  };
  impl_->RunQueryAsync<CommandResult>(
      /*dedup_key=*/{},  // every command invocation must run; never supersede.
      [this, name = std::move(name), args = std::move(args)]() {
        CommandResult result;
        result.ran = provider_query_interop::ExecuteCommand(
            name, args, impl_->commands,
            [this](lua_State* state) { return impl_->FindPluginByState(state); },
            [this](lua_State* state) { impl_->PushPluginContext(state); }, &result.error,
            &result.feedback);
        return result;
      },
      [on_result = std::move(on_result)](CommandResult result) {
        on_result(result.ran, std::move(result.error), std::move(result.feedback));
      });
#else
  (void)name;
  (void)args;
  on_result(false, {}, {});
#endif
}

const std::vector<std::string>& PluginHost::CommandNames() const {
  // The sorted view is resolved once when the snapshot is published (registration
  // only flips the dirty bit); the UI reads the immutable published copy here.
  return impl_->published_.command_names;
}

const std::vector<PluginHost::SidebarProviderInfo>& PluginHost::SidebarProviders() const {
  return impl_->published_.sidebar_providers;
}

const PluginHost::SidebarProviderInfo* PluginHost::FindSidebarProvider(std::string_view id) const {
  // Search the published view (UI-thread-owned) rather than the live `sidebars` map
  // the worker rebuilds on reload. Providers are few, so a linear scan is cheap and
  // the returned pointer stays valid until the next publish (also on the UI thread).
  for (const auto& provider : impl_->published_.sidebar_providers) {
    if (provider.id == id) {
      return &provider;
    }
  }
  return nullptr;
}

bool PluginHost::SnapshotSidebar(std::string_view id,
                                 std::vector<SidebarItem>* items,
                                 std::string* error_message) {
  if (items != nullptr) {
    items->clear();
  }
  // Fast-reject unknown ids against the published view (no round-trip, race-free).
  if (FindSidebarProvider(id) == nullptr) {
    if (error_message != nullptr) {
      *error_message = "unknown plugin sidebar: " + std::string(id);
    }
    return false;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  // Resolve the live provider on the worker (the lua-ref-bearing map is worker-owned).
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    const auto it = impl_->sidebars.find(std::string(id));
    if (it == impl_->sidebars.end()) {
      if (error_message != nullptr) {
        *error_message = "unknown plugin sidebar: " + std::string(id);
      }
      ok = false;
      return;
    }
    ok = impl_->SnapshotSidebarProvider(it->second, items, error_message);
  });
  return ok;
#else
  if (error_message != nullptr) {
    *error_message = "Lua plugin runtime unavailable";
  }
  return false;
#endif
}

bool PluginHost::ConfirmSidebarItem(std::string_view id,
                                    const SidebarItem& item,
                                    std::string* error_message) {
  if (FindSidebarProvider(id) == nullptr) {
    if (error_message != nullptr) {
      *error_message = "unknown plugin sidebar: " + std::string(id);
    }
    return false;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    const auto it = impl_->sidebars.find(std::string(id));
    if (it == impl_->sidebars.end()) {
      if (error_message != nullptr) {
        *error_message = "unknown plugin sidebar: " + std::string(id);
      }
      ok = false;
      return;
    }
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
  if (FindSidebarProvider(id) == nullptr) {
    if (error_message != nullptr) {
      *error_message = "unknown plugin sidebar: " + std::string(id);
    }
    return false;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  bool ok = false;
  impl_->RunOnWorkerBlocking(impl_->CaptureSnapshot(), [&]() {
    const auto it = impl_->sidebars.find(std::string(id));
    if (it == impl_->sidebars.end()) {
      if (error_message != nullptr) {
        *error_message = "unknown plugin sidebar: " + std::string(id);
      }
      ok = false;
      return;
    }
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
      path_interop::ResolveRuntimePath(impl_->published_.project_root, path).lexically_normal();
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
  // Resolve against the published, enabled plugin roots (UI-thread-owned) rather than
  // the live `plugins` vector the worker rebuilds on reload.
  std::vector<std::filesystem::path> roots;
  roots.reserve(impl_->published_.loaded_plugins.size());
  for (const auto& plugin : impl_->published_.loaded_plugins) {
    if (plugin.enabled) {
      roots.push_back(plugin.root);
    }
  }
  return data_directory_interop::DataDirectories(subdirectory, roots);
}

const std::vector<PluginHost::ContributedMenuEntry>& PluginHost::ContributedMenuEntries() const {
  return impl_->published_.menu_entries;
}

const std::vector<PluginHost::ContributedKeybinding>& PluginHost::ContributedKeybindings() const {
  return impl_->published_.keybindings;
}

const std::vector<PluginHost::ContributedSettingSpec>& PluginHost::ContributedSettings() const {
  return impl_->published_.settings;
}

const std::vector<PluginHost::ContributedStatusItem>& PluginHost::ContributedStatusItems() const {
  return impl_->published_.status_item_order;
}

std::uint64_t PluginHost::StatusItemsRevision() const {
  return impl_->status_view_revision;
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
  // Pass a copy, not a move: on timeout the worker may still be transforming its
  // copy, so the caller's `*text` must stay intact to save untransformed.
  PluginHost::Impl::SaveParticipantResult result =
      impl_->RunSaveParticipantsBounded(path, *text);
  if (result.timed_out) {
    // Never wedge a save: proceed with the caller's untransformed `*text` (left
    // intact -- we passed a copy to the worker) and surface a toast.
    if (impl_->raw_callbacks.show_notification) {
      impl_->raw_callbacks.show_notification(
          "warning", "save participants timed out; saved without plugin transforms");
    }
    return true;
  }
  if (!result.ok) {
    if (error_message != nullptr) {
      *error_message = std::move(result.error);
    }
    return false;
  }
  *text = std::move(result.text);
  return true;
#else
  (void)path;
  (void)text;
  return true;
#endif
}

void PluginHost::SetSaveParticipantDeadlineForTesting(std::chrono::milliseconds deadline) {
  impl_->save_participant_deadline_ = deadline;
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

void PluginHost::QueryDocumentSymbolsAsync(
    std::string language_id,
    std::filesystem::path path,
    std::function<void(std::vector<DocumentSymbolNode>, std::string)> on_result) {
  if (!impl_->enabled()) {
    on_result({}, {});
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  struct QueryResult {
    std::vector<DocumentSymbolNode> items;
    std::string error;
  };
  impl_->RunQueryAsync<QueryResult>(
      "query:document_symbols",
      [this, language_id = std::move(language_id), path = std::move(path)]() {
        QueryResult result;
        result.items = impl_->QueryLanguageDocumentSymbols(language_id, path, &result.error);
        return result;
      },
      [on_result = std::move(on_result)](QueryResult result) {
        on_result(std::move(result.items), std::move(result.error));
      });
#else
  (void)language_id;
  (void)path;
  on_result({}, {});
#endif
}

void PluginHost::QueryHoverAsync(std::filesystem::path path,
                                 std::size_t line,
                                 std::size_t column,
                                 std::function<void(bool, HoverResult)> on_result) {
  if (!impl_->enabled() || path.empty() || line == 0 || column == 0) {
    on_result(false, HoverResult{});
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  const std::filesystem::path resolved_path =
      path_interop::ResolveRuntimePath(impl_->published_.project_root, path).lexically_normal();
  struct QueryResult {
    bool ok = false;
    HoverResult hover;
  };
  impl_->RunQueryAsync<QueryResult>(
      "query:hover",
      [this, resolved_path, line, column]() {
        QueryResult result;
        result.ok = hover_query_interop::QueryHover(
            impl_->hover_provider_order, impl_->hovers, resolved_path, line, column,
            [this](const runtime_types::HoverProvider& provider,
                   const std::filesystem::path& hover_path, std::size_t hover_line,
                   std::size_t hover_column, HoverResult* hover_result,
                   std::string* hover_error_message) {
              return impl_->QueryHoverProvider(provider, hover_path, hover_line, hover_column,
                                               hover_result, hover_error_message);
            },
            &result.hover, nullptr);
        return result;
      },
      [on_result = std::move(on_result)](QueryResult result) {
        on_result(result.ok, std::move(result.hover));
      });
#else
  (void)resolved_path;
  on_result(false, HoverResult{});
#endif
}

void PluginHost::SnapshotSidebarAsync(
    std::string id,
    std::function<void(bool, std::vector<SidebarItem>, std::string)> on_result) {
  // Fast-reject an unknown provider against the published view (race-free); the worker
  // closure below re-checks the live map. Nothing to run on the worker for an unknown id.
  if (FindSidebarProvider(id) == nullptr) {
    on_result(false, {}, "unknown plugin sidebar: " + id);
    return;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  struct QueryResult {
    bool ok = false;
    std::vector<SidebarItem> items;
    std::string error;
  };
  impl_->RunQueryAsync<QueryResult>(
      "query:sidebar_snapshot",
      [this, id = std::move(id)]() {
        QueryResult result;
        const auto it = impl_->sidebars.find(id);
        if (it == impl_->sidebars.end()) {
          result.error = "unknown plugin sidebar: " + id;
          return result;
        }
        result.ok = impl_->SnapshotSidebarProvider(it->second, &result.items, &result.error);
        return result;
      },
      [on_result = std::move(on_result)](QueryResult result) {
        on_result(result.ok, std::move(result.items), std::move(result.error));
      });
#else
  on_result(false, {}, "Lua plugin runtime unavailable");
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
  return impl_->published_.formatters;
}

const std::vector<PluginHost::ContributedSaveParticipant>& PluginHost::ContributedSaveParticipants()
    const {
  return impl_->published_.save_participants;
}

const std::vector<PluginHost::ContributedCompletion>& PluginHost::ContributedCompletions() const {
  return impl_->published_.completions;
}

const std::vector<PluginHost::ContributedCodeAction>& PluginHost::ContributedCodeActions() const {
  return impl_->published_.code_actions;
}

const std::vector<PluginHost::ContributedLanguageServer>&
PluginHost::ContributedLanguageServers() const {
  return impl_->published_.language_servers;
}

const std::vector<PluginHost::ContributedDebugAdapter>&
PluginHost::ContributedDebugAdapters() const {
  return impl_->published_.debug_adapters;
}

const std::vector<PluginHost::ContributedLaunchConfig>&
PluginHost::ContributedLaunchConfigs() const {
  return impl_->published_.launch_configs;
}

const std::vector<PluginHost::ContributedTask>& PluginHost::ContributedTasks() const {
  return impl_->published_.tasks;
}

const std::vector<PluginHost::ContributedTool>& PluginHost::ContributedTools() const {
  return impl_->published_.tools;
}

const std::vector<PluginHost::ContributedTestProvider>& PluginHost::ContributedTestProviders()
    const {
  return impl_->published_.test_providers;
}

const std::vector<PluginHost::ContributedScmProvider>& PluginHost::ContributedScmProviders()
    const {
  return impl_->published_.scm_providers;
}

const std::vector<PluginHost::ContributedAnnotationProvider>&
PluginHost::ContributedAnnotationProviders() const {
  return impl_->published_.annotation_providers;
}

const std::vector<PluginHost::ContributedAuthProvider>& PluginHost::ContributedAuthProviders()
    const {
  return impl_->published_.auth_providers;
}

const std::vector<PluginHost::ContributedBracketSet>& PluginHost::ContributedBrackets() const {
  return impl_->published_.bracket_sets;
}

const std::vector<PluginHost::ContributedCommentMarkers>& PluginHost::ContributedComments() const {
  return impl_->published_.comment_markers;
}

const std::vector<PluginHost::ContributedIndentRules>& PluginHost::ContributedIndents() const {
  return impl_->published_.indent_rules;
}

const std::vector<PluginHost::ContributedSnippet>& PluginHost::ContributedSnippets() const {
  return impl_->published_.snippets;
}

const std::vector<PluginHost::ContributedTheme>& PluginHost::ContributedThemes() const {
  return impl_->published_.themes;
}

const std::vector<PluginHost::ContributedFileIconTheme>& PluginHost::ContributedFileIconThemes()
    const {
  return impl_->published_.file_icon_themes;
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
  return impl_->published_.reload_summary;
}

std::size_t PluginHost::LoadedPluginCount() const {
  // Count the enabled plugins in the published view (UI-thread-owned); the live
  // `plugins` vector is worker-owned and rebuilt on reload. Disabled plugins appear in
  // loaded_plugins with enabled=false and are excluded, matching the live count.
  std::size_t count = 0;
  for (const auto& plugin : impl_->published_.loaded_plugins) {
    if (plugin.enabled) {
      ++count;
    }
  }
  return count;
}

void PluginHost::SetDisabledPlugins(std::vector<std::string> disabled_ids) {
  impl_->disabled_plugins = std::move(disabled_ids);
}

std::vector<PluginHost::LoadedPlugin> PluginHost::LoadedPlugins() const {
  return impl_->published_.loaded_plugins;
}

}  // namespace microide::plugin
