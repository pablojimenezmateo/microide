bool PluginHost::enabled() const {
  return impl_->enabled();
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
  impl_->DrainAsyncProcessWorkers();
  impl_->TearDownPlugins();
  impl_->current_project_root = next_project_root;

  for (const auto& plugin_root : impl_->DiscoverPluginRoots()) {
    std::string error_message;
    if (!impl_->LoadPluginRoot(plugin_root, &error_message) &&
        !error_message.empty()) {
      impl_->RecordError(std::move(error_message));
    }
  }
  if (!impl_->current_project_root.empty()) {
    project_lifecycle_interop::DispatchProjectOpenCallbacks(
        &impl_->plugins, [this](runtime_types::PluginInstance* plugin,
                                int callback_ref,
                                const char* callback_name) {
          impl_->CallProjectCallback(plugin, callback_ref, callback_name);
        });
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
  impl_->DrainAsyncProcessWorkers();
  impl_->TearDownPlugins();
#endif
  impl_->current_project_root.clear();
  impl_->SetReloadSummary();
}

void PluginHost::SetAsyncProcessEventType(std::uint32_t type) {
  async_state_interop::SetEventType(*impl_->async_process_state, type);
}

int PluginHost::ConsumeAsyncProcessCallbacks() {
#if MICROIDE_HAS_LUA_PLUGINS
  return async_callback_interop::ConsumeCallbacks(
      *impl_->async_process_state,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](std::string error) {
        if (impl_->callbacks.error_sink) {
          impl_->callbacks.error_sink(std::move(error));
        }
      });
#endif
  return 0;
}

int PluginHost::PendingAsyncProcessCount() const {
  return async_state_interop::PendingCount(*impl_->async_process_state);
}

void PluginHost::OnBufferOpen(const std::filesystem::path& path) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  buffer_lifecycle_interop::DispatchBufferCallbacks(
      &impl_->plugins, &runtime_types::PluginInstance::on_buffer_open_ref, "on_buffer_open", path,
      [this](runtime_types::PluginInstance* plugin,
             int callback_ref,
             const char* callback_name,
             const std::filesystem::path& callback_path) {
        impl_->CallBufferCallback(plugin, callback_ref, callback_name, callback_path);
      });
#endif
}

void PluginHost::OnBufferSave(const std::filesystem::path& path) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  buffer_lifecycle_interop::DispatchBufferCallbacks(
      &impl_->plugins, &runtime_types::PluginInstance::on_buffer_save_ref, "on_buffer_save", path,
      [this](runtime_types::PluginInstance* plugin,
             int callback_ref,
             const char* callback_name,
             const std::filesystem::path& callback_path) {
        impl_->CallBufferCallback(plugin, callback_ref, callback_name, callback_path);
      });
#endif
}

bool PluginHost::ExecuteCommand(std::string_view name,
                                const std::vector<std::string>& args,
                                std::string* error_message) {
  if (!impl_->enabled()) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  return provider_query_interop::ExecuteCommand(
      name, args, impl_->commands,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state) { impl_->PushPluginContext(state); }, error_message);
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
  return impl_->command_names;
}

const std::vector<PluginHost::SidebarProviderInfo>& PluginHost::SidebarProviders() const {
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
  return impl_->SnapshotSidebarProvider(it->second, items, error_message);
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
  return impl_->ConfirmSidebarProviderItem(it->second, item, error_message);
#else
  (void)item;
  if (error_message != nullptr) {
    *error_message = "Lua plugin runtime unavailable";
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
  return hover_query_interop::QueryHover(
      impl_->hover_provider_order, impl_->hovers, resolved_path, line, column,
      [this](const runtime_types::HoverProvider& provider,
             const std::filesystem::path& hover_path,
             std::size_t hover_line,
             std::size_t hover_column,
             HoverResult* hover_result,
             std::string* hover_error_message) {
        return impl_->QueryHoverProvider(provider, hover_path, hover_line, hover_column,
                                         hover_result, hover_error_message);
      },
      result, error_message);
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

bool PluginHost::UpdateStatusItem(std::string_view id, std::string text, std::string tooltip) {
  return status_interop::UpdateStatusItem(
      id, std::move(text), std::move(tooltip), &impl_->status_items, &impl_->status_item_order,
      impl_->callbacks.request_status_redraw);
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
  return provider_query_interop::RunSaveParticipants(
      path, text, impl_->save_participant_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path, std::string_view value) {
        impl_->PushBufferContext(state, buffer_path, value);
      },
      error_message);
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
  results = provider_query_interop::QueryCompletions(
      language_id, path, line, column, trigger_character, impl_->completion_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path) {
        impl_->PushBufferContext(state, buffer_path);
      },
      error_message);
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
  (void)trigger_character;
#endif
  return results;
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
  results = provider_query_interop::QueryCodeActions(
      language_id, path, start_line, start_column, end_line, end_column,
      impl_->code_action_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path) {
        impl_->PushBufferContext(state, buffer_path);
      },
      error_message);
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
  return provider_query_interop::DiscoverTests(
      provider_id, path, impl_->current_project_root, impl_->test_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path) {
        impl_->PushBufferContext(state, buffer_path);
      },
      [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
        return path_interop::ResolveRuntimePath(project_root, runtime_path);
      },
      tests, error_message);
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
  return provider_query_interop::RunTests(
      provider_id, test_ids, impl_->test_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); }, results,
      error_message);
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
  return provider_query_interop::SnapshotScm(
      provider_id, impl_->current_project_root, impl_->scm_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
        return path_interop::ResolveRuntimePath(project_root, runtime_path);
      },
      snapshot, error_message);
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
  lines = provider_query_interop::QueryAnnotations(
      provider_id, path, language_id, visible_start_line, visible_end_line,
      impl_->annotation_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path) {
        impl_->PushBufferContext(state, buffer_path);
      },
      error_message);
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
  return provider_query_interop::LoginAuthProvider(
      provider_id, scopes, impl_->auth_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); }, session,
      error_message);
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
  return provider_query_interop::RefreshAuthSession(
      provider_id, session_id, impl_->auth_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); }, session,
      error_message);
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
  return provider_query_interop::LogoutAuthSession(
      provider_id, session_id, impl_->auth_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); }, error_message);
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

}  // namespace microide::plugin
