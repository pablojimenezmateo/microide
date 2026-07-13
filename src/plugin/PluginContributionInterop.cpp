#include "plugin/PluginContributionInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/PluginContributionLimits.h"

#include <algorithm>
#include <string>
#include <vector>

namespace microide::plugin::contribution_interop {

namespace {

// Reject a contribution whose non-empty `.id` already exists in the target
// vector. First-match consumers (task runner, tool/adapter/launch lookup) would
// otherwise behave order-dependently on a duplicate local id. Empty ids are left
// to the per-type parser's own validation.
template <typename T>
bool DuplicateContributionId(const std::vector<T>& items, const T& candidate,
                             std::string_view kind, std::string* error_message) {
  if (candidate.id.empty()) {
    return false;
  }
  const bool duplicate = std::any_of(
      items.begin(), items.end(), [&](const T& e) { return e.id == candidate.id; });
  if (duplicate && error_message != nullptr) {
    *error_message = "duplicate " + std::string(kind) + " id '" + candidate.id + "'";
  }
  return duplicate;
}

}  // namespace

bool RegisterFormatter(lua_State* state,
                       std::string_view plugin_id,
                       std::vector<PluginHost::ContributedFormatter>* formatters,
                       std::string* error_message) {
  if (formatters == nullptr) {
    return false;
  }
  if (ContributionLimitReached(formatters, error_message)) {
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
  if (ContributionLimitReached(participants, error_message)) {
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
  if (ContributionLimitReached(contributions, error_message)) {
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
  if (ContributionLimitReached(contributions, error_message)) {
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

bool RegisterLanguageQuery(lua_State* state,
                           std::string_view plugin_id,
                           runtime_types::LanguageQueryKind kind,
                           std::vector<runtime_types::LanguageQueryRuntime>* runtimes,
                           std::string* error_message) {
  if (runtimes == nullptr) {
    return false;
  }
  if (ContributionLimitReached(runtimes, error_message)) {
    return false;
  }
  registration_parsers::LanguageQueryRegistration registration;
  if (!registration_parsers::ParseLanguageQueryRegistration(state, std::string(plugin_id), kind,
                                                            &registration, error_message)) {
    return false;
  }
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
  if (ContributionLimitReached(tasks, error_message)) {
    return false;
  }
  registration_parsers::TaskRegistration registration;
  if (!registration_parsers::ParseTaskRegistration(state, std::string(plugin_id), &registration,
                                                   error_message)) {
    return false;
  }
  if (DuplicateContributionId(*tasks, registration.contributed, "task", error_message)) {
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
  if (ContributionLimitReached(servers, error_message)) {
    return false;
  }
  registration_parsers::LanguageServerRegistration registration;
  if (!registration_parsers::ParseLanguageServerRegistration(state, std::string(plugin_id),
                                                             &registration, error_message)) {
    return false;
  }
  if (DuplicateContributionId(*servers, registration.contributed, "language server",
                              error_message)) {
    return false;
  }
  servers->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterDebugAdapter(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedDebugAdapter>* adapters,
                          std::string* error_message) {
  if (adapters == nullptr) {
    return false;
  }
  if (ContributionLimitReached(adapters, error_message)) {
    return false;
  }
  registration_parsers::DebugAdapterRegistration registration;
  if (!registration_parsers::ParseDebugAdapterRegistration(state, std::string(plugin_id),
                                                            &registration, error_message)) {
    return false;
  }
  if (DuplicateContributionId(*adapters, registration.contributed, "debug adapter",
                              error_message)) {
    return false;
  }
  adapters->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterLaunchConfig(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedLaunchConfig>* configs,
                          std::string* error_message) {
  if (configs == nullptr) {
    return false;
  }
  if (ContributionLimitReached(configs, error_message)) {
    return false;
  }
  registration_parsers::LaunchConfigRegistration registration;
  if (!registration_parsers::ParseLaunchConfigRegistration(state, std::string(plugin_id),
                                                           &registration, error_message)) {
    return false;
  }
  if (DuplicateContributionId(*configs, registration.contributed, "launch config",
                              error_message)) {
    return false;
  }
  configs->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterTool(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTool>* tools,
                  std::string* error_message) {
  if (tools == nullptr) {
    return false;
  }
  if (ContributionLimitReached(tools, error_message)) {
    return false;
  }
  registration_parsers::ToolRegistration registration;
  if (!registration_parsers::ParseToolRegistration(state, std::string(plugin_id), &registration,
                                                   error_message)) {
    return false;
  }
  if (DuplicateContributionId(*tools, registration.contributed, "tool", error_message)) {
    return false;
  }
  tools->push_back(std::move(registration.contributed));
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
  if (ContributionLimitReached(providers, error_message)) {
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

bool RegisterScmProvider(lua_State* state,
                         std::string_view plugin_id,
                         std::vector<PluginHost::ContributedScmProvider>* providers,
                         std::vector<runtime_types::ScmProviderRuntime>* runtimes,
                         std::string* error_message) {
  if (providers == nullptr || runtimes == nullptr) {
    return false;
  }
  if (ContributionLimitReached(providers, error_message)) {
    return false;
  }
  registration_parsers::ScmProviderRegistration registration;
  if (!registration_parsers::ParseScmProviderRegistration(state, std::string(plugin_id),
                                                          &registration, error_message)) {
    return false;
  }
  providers->push_back(std::move(registration.contributed));
  if (registration.has_runtime) {
    runtimes->push_back(std::move(registration.runtime));
  }
  return true;
}

bool RegisterAnnotationProvider(
    lua_State* state,
    std::string_view plugin_id,
    std::vector<PluginHost::ContributedAnnotationProvider>* providers,
    std::vector<runtime_types::AnnotationProviderRuntime>* runtimes,
    std::string* error_message) {
  if (providers == nullptr || runtimes == nullptr) {
    return false;
  }
  if (ContributionLimitReached(providers, error_message)) {
    return false;
  }
  registration_parsers::AnnotationProviderRegistration registration;
  if (!registration_parsers::ParseAnnotationProviderRegistration(state, std::string(plugin_id),
                                                                 &registration, error_message)) {
    return false;
  }
  providers->push_back(std::move(registration.contributed));
  if (registration.has_runtime) {
    runtimes->push_back(std::move(registration.runtime));
  }
  return true;
}

bool RegisterAuthProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedAuthProvider>* providers,
                          std::vector<runtime_types::AuthProviderRuntime>* runtimes,
                          std::string* error_message) {
  if (providers == nullptr || runtimes == nullptr) {
    return false;
  }
  if (ContributionLimitReached(providers, error_message)) {
    return false;
  }
  registration_parsers::AuthProviderRegistration registration;
  if (!registration_parsers::ParseAuthProviderRegistration(state, std::string(plugin_id),
                                                           &registration, error_message)) {
    return false;
  }
  providers->push_back(std::move(registration.contributed));
  if (registration.has_runtime) {
    runtimes->push_back(std::move(registration.runtime));
  }
  return true;
}

bool RegisterAiProvider(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedAiProvider>* providers,
                        std::string* error_message) {
  if (providers == nullptr) {
    return false;
  }
  if (ContributionLimitReached(providers, error_message)) {
    return false;
  }
  registration_parsers::AiProviderRegistration registration;
  if (!registration_parsers::ParseAiProviderRegistration(state, std::string(plugin_id), &registration,
                                                         error_message)) {
    return false;
  }
  providers->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterExternalAgent(lua_State* state,
                           std::string_view plugin_id,
                           std::vector<PluginHost::ContributedExternalAgent>* agents,
                           std::string* error_message) {
  if (agents == nullptr) {
    return false;
  }
  if (ContributionLimitReached(agents, error_message)) {
    return false;
  }
  registration_parsers::ExternalAgentRegistration registration;
  if (!registration_parsers::ParseExternalAgentRegistration(state, std::string(plugin_id),
                                                            &registration, error_message)) {
    return false;
  }
  agents->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterBracketSet(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedBracketSet>* sets,
                        std::string* error_message) {
  if (sets == nullptr) {
    return false;
  }
  if (ContributionLimitReached(sets, error_message)) {
    return false;
  }
  registration_parsers::BracketSetRegistration registration;
  if (!registration_parsers::ParseBracketSetRegistration(state, std::string(plugin_id),
                                                         &registration, error_message)) {
    return false;
  }
  sets->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterCommentMarkers(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedCommentMarkers>* markers,
                            std::string* error_message) {
  if (markers == nullptr) {
    return false;
  }
  if (ContributionLimitReached(markers, error_message)) {
    return false;
  }
  registration_parsers::CommentMarkersRegistration registration;
  if (!registration_parsers::ParseCommentMarkersRegistration(state, std::string(plugin_id),
                                                             &registration, error_message)) {
    return false;
  }
  markers->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterIndentRules(lua_State* state,
                         std::string_view plugin_id,
                         std::vector<PluginHost::ContributedIndentRules>* rules,
                         std::string* error_message) {
  if (rules == nullptr) {
    return false;
  }
  if (ContributionLimitReached(rules, error_message)) {
    return false;
  }
  registration_parsers::IndentRulesRegistration registration;
  if (!registration_parsers::ParseIndentRulesRegistration(state, std::string(plugin_id),
                                                          &registration, error_message)) {
    return false;
  }
  rules->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterSnippet(lua_State* state,
                     std::string_view plugin_id,
                     std::vector<PluginHost::ContributedSnippet>* snippets,
                     std::string* error_message) {
  if (snippets == nullptr) {
    return false;
  }
  if (ContributionLimitReached(snippets, error_message)) {
    return false;
  }
  registration_parsers::SnippetRegistration registration;
  if (!registration_parsers::ParseSnippetRegistration(state, std::string(plugin_id),
                                                      &registration, error_message)) {
    return false;
  }
  snippets->push_back(std::move(registration.contributed));
  return true;
}

}  // namespace microide::plugin::contribution_interop

#endif
