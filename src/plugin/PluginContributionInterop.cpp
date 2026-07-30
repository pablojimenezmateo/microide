#include "plugin/PluginContributionInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/PluginContributionLimits.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace microide::plugin::contribution_interop {

namespace {

// Reject a contribution whose non-empty `.id` already exists among the kind's
// registered contributions. First-match consumers (task runner, tool/adapter/
// launch lookup) would otherwise behave order-dependently on a duplicate local
// id. `id_index` mirrors the storage vector's non-empty ids, so the membership
// test is O(1) instead of an O(n) scan (registering N ids was O(N^2)). Empty ids
// are left to the per-type parser's own validation and never enter the index.
template <typename T>
bool DuplicateContributionId(const std::unordered_set<std::string>* id_index, const T& candidate,
                             std::string_view kind, std::string* error_message) {
  if (candidate.id.empty() || id_index == nullptr) {
    return false;
  }
  if (id_index->find(candidate.id) == id_index->end()) {
    return false;
  }
  if (error_message != nullptr) {
    *error_message = "duplicate " + std::string(kind) + " id '" + candidate.id + "'";
  }
  return true;
}

// Record a just-accepted contribution's non-empty id in the kind's index so a
// later duplicate is detected in O(1). Keeps the index in sync with push_back.
template <typename T>
void NoteContributionId(std::unordered_set<std::string>* id_index, const T& candidate) {
  if (id_index != nullptr && !candidate.id.empty()) {
    id_index->insert(candidate.id);
  }
}

// Register a contribution kind that needs no id-uniqueness check: null-guard the
// storage, refuse past the per-kind cap, parse, append.
//
// The four id-less kinds (bracket sets, comment markers, indent rules, snippets)
// each wrote that out; only the registration type and its parser differed. The
// kinds that DO carry an id keep their own bodies — they additionally run
// DuplicateContributionId / NoteContributionId, and that ordering (check cap,
// parse, check duplicate, append, note) is worth reading in full at each site.
template <typename Contributed, typename Registration, typename Parse>
bool RegisterSimpleContribution(lua_State* state,
                                std::string_view plugin_id,
                                std::vector<Contributed>* storage,
                                std::string* error_message,
                                Parse&& parse) {
  if (storage == nullptr) {
    return false;
  }
  if (ContributionLimitReached(storage, error_message)) {
    return false;
  }
  Registration registration;
  if (!parse(state, std::string(plugin_id), &registration, error_message)) {
    return false;
  }
  storage->push_back(std::move(registration.contributed));
  return true;
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
                  std::unordered_set<std::string>* id_index,
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
  if (DuplicateContributionId(id_index, registration.contributed, "task", error_message)) {
    return false;
  }
  NoteContributionId(id_index, registration.contributed);
  tasks->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterLanguageServer(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedLanguageServer>* servers,
                            std::unordered_set<std::string>* id_index,
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
  if (DuplicateContributionId(id_index, registration.contributed, "language server",
                              error_message)) {
    return false;
  }
  NoteContributionId(id_index, registration.contributed);
  servers->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterDebugAdapter(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedDebugAdapter>* adapters,
                          std::unordered_set<std::string>* id_index,
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
  if (DuplicateContributionId(id_index, registration.contributed, "debug adapter",
                              error_message)) {
    return false;
  }
  NoteContributionId(id_index, registration.contributed);
  adapters->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterLaunchConfig(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedLaunchConfig>* configs,
                          std::unordered_set<std::string>* id_index,
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
  if (DuplicateContributionId(id_index, registration.contributed, "launch config",
                              error_message)) {
    return false;
  }
  NoteContributionId(id_index, registration.contributed);
  configs->push_back(std::move(registration.contributed));
  return true;
}

bool RegisterTool(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTool>* tools,
                  std::unordered_set<std::string>* id_index,
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
  if (DuplicateContributionId(id_index, registration.contributed, "tool", error_message)) {
    return false;
  }
  NoteContributionId(id_index, registration.contributed);
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

bool RegisterBracketSet(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedBracketSet>* sets,
                        std::string* error_message) {
  return RegisterSimpleContribution<PluginHost::ContributedBracketSet,
                                    registration_parsers::BracketSetRegistration>(
      state, plugin_id, sets, error_message,
      registration_parsers::ParseBracketSetRegistration);
}

bool RegisterCommentMarkers(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedCommentMarkers>* markers,
                            std::string* error_message) {
  return RegisterSimpleContribution<PluginHost::ContributedCommentMarkers,
                                    registration_parsers::CommentMarkersRegistration>(
      state, plugin_id, markers, error_message,
      registration_parsers::ParseCommentMarkersRegistration);
}

bool RegisterIndentRules(lua_State* state,
                         std::string_view plugin_id,
                         std::vector<PluginHost::ContributedIndentRules>* rules,
                         std::string* error_message) {
  return RegisterSimpleContribution<PluginHost::ContributedIndentRules,
                                    registration_parsers::IndentRulesRegistration>(
      state, plugin_id, rules, error_message,
      registration_parsers::ParseIndentRulesRegistration);
}

bool RegisterSnippet(lua_State* state,
                     std::string_view plugin_id,
                     std::vector<PluginHost::ContributedSnippet>* snippets,
                     std::string* error_message) {
  return RegisterSimpleContribution<PluginHost::ContributedSnippet,
                                    registration_parsers::SnippetRegistration>(
      state, plugin_id, snippets, error_message,
      registration_parsers::ParseSnippetRegistration);
}

}  // namespace microide::plugin::contribution_interop

#endif
