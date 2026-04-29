#include "plugin/PluginContributionInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

namespace microide::plugin::contribution_interop {

bool RegisterFormatter(lua_State* state,
                       std::string_view plugin_id,
                       std::vector<PluginHost::ContributedFormatter>* formatters,
                       std::string* error_message) {
  if (formatters == nullptr) {
    return false;
  }
  registration_parsers::FormatterRegistration registration;
  if (!registration_parsers::ParseFormatterRegistration(state, std::string(plugin_id), &registration,
                                                        error_message)) {
    return false;
  }
  formatters->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterSaveParticipant(lua_State* state,
                             std::string_view plugin_id,
                             std::vector<PluginHost::ContributedSaveParticipant>* participants,
                             std::vector<runtime_types::SaveParticipantRuntime>* runtimes,
                             std::string* error_message) {
  if (participants == nullptr || runtimes == nullptr) {
    return false;
  }
  registration_parsers::SaveParticipantRegistration registration;
  if (!registration_parsers::ParseSaveParticipantRegistration(state, std::string(plugin_id),
                                                              &registration, error_message)) {
    return false;
  }
  participants->push_back(std::move(registration.contributed));
  runtimes->push_back(std::move(registration.runtime));
  return true;
}

bool RegisterCompletion(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedCompletion>* contributions,
                        std::vector<runtime_types::CompletionRuntime>* runtimes,
                        std::string* error_message) {
  if (contributions == nullptr || runtimes == nullptr) {
    return false;
  }
  registration_parsers::CompletionRegistration registration;
  if (!registration_parsers::ParseCompletionRegistration(state, std::string(plugin_id), &registration,
                                                         error_message)) {
    return false;
  }
  contributions->push_back(std::move(registration.contributed));
  if (registration.has_runtime) {
    runtimes->push_back(std::move(registration.runtime));
  }
  return true;
}

bool RegisterCodeAction(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedCodeAction>* contributions,
                        std::vector<runtime_types::CodeActionRuntime>* runtimes,
                        std::string* error_message) {
  if (contributions == nullptr || runtimes == nullptr) {
    return false;
  }
  registration_parsers::CodeActionRegistration registration;
  if (!registration_parsers::ParseCodeActionRegistration(state, std::string(plugin_id), &registration,
                                                         error_message)) {
    return false;
  }
  contributions->push_back(std::move(registration.contributed));
  if (registration.has_runtime) {
    runtimes->push_back(std::move(registration.runtime));
  }
  return true;
}

bool RegisterTask(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTask>* tasks,
                  std::string* error_message) {
  if (tasks == nullptr) {
    return false;
  }
  registration_parsers::TaskRegistration registration;
  if (!registration_parsers::ParseTaskRegistration(state, std::string(plugin_id), &registration,
                                                   error_message)) {
    return false;
  }
  tasks->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterLanguageServer(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedLanguageServer>* servers,
                            std::string* error_message) {
  if (servers == nullptr) {
    return false;
  }
  registration_parsers::LanguageServerRegistration registration;
  if (!registration_parsers::ParseLanguageServerRegistration(state, std::string(plugin_id),
                                                             &registration, error_message)) {
    return false;
  }
  servers->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterTool(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTool>* tools,
                  std::string* error_message) {
  if (tools == nullptr) {
    return false;
  }
  registration_parsers::ToolRegistration registration;
  if (!registration_parsers::ParseToolRegistration(state, std::string(plugin_id), &registration,
                                                   error_message)) {
    return false;
  }
  tools->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterDebugger(lua_State* state,
                      std::string_view plugin_id,
                      std::vector<PluginHost::ContributedDebugger>* debuggers,
                      std::string* error_message) {
  if (debuggers == nullptr) {
    return false;
  }
  registration_parsers::DebuggerRegistration registration;
  if (!registration_parsers::ParseDebuggerRegistration(state, std::string(plugin_id), &registration,
                                                       error_message)) {
    return false;
  }
  debuggers->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterTestProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedTestProvider>* providers,
                          std::vector<runtime_types::TestProviderRuntime>* runtimes,
                          std::string* error_message) {
  if (providers == nullptr || runtimes == nullptr) {
    return false;
  }
  registration_parsers::TestProviderRegistration registration;
  if (!registration_parsers::ParseTestProviderRegistration(state, std::string(plugin_id),
                                                           &registration, error_message)) {
    return false;
  }
  providers->push_back(std::move(registration.contributed));
  if (registration.has_runtime) {
    runtimes->push_back(std::move(registration.runtime));
  }
  return true;
}

}  // namespace microide::plugin::contribution_interop

#endif
