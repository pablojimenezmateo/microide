#include "plugin/PluginContributionInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/PluginContributionLimits.h"

#include <string>
#include <unordered_set>
#include <vector>
#include "util/ErrorOut.h"

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
  util::SetError(error_message, "duplicate " + std::string(kind) + " id '" + candidate.id + "'");
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

// The registration shapes the contribution kinds share. Each null-guards its
// storage, refuses past the per-kind cap, then parses; they differ in what a
// parsed registration adds besides its contribution.

// A kind with nothing more: append the contribution.
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

// A kind whose contributions carry an id that must be unique among the kind's:
// a duplicate is refused (see DuplicateContributionId), an accepted id indexed.
template <typename Contributed, typename Registration, typename Parse>
bool RegisterUniqueContribution(lua_State* state,
                                std::string_view plugin_id,
                                std::vector<Contributed>* storage,
                                std::unordered_set<std::string>* id_index,
                                std::string_view kind,
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
  if (DuplicateContributionId(id_index, registration.contributed, kind, error_message)) {
    return false;
  }
  NoteContributionId(id_index, registration.contributed);
  storage->push_back(std::move(registration.contributed));
  return true;
}

// A kind whose registration may also carry the Lua callbacks that run it: the
// runtime goes into its own list when the registration has one.
template <typename Contributed, typename Runtime, typename Registration, typename Parse>
bool RegisterContributionWithRuntime(lua_State* state,
                                     std::string_view plugin_id,
                                     std::vector<Contributed>* storage,
                                     std::vector<Runtime>* runtimes,
                                     std::string* error_message,
                                     Parse&& parse) {
  if (storage == nullptr || runtimes == nullptr) {
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
  if (registration.has_runtime) {
    runtimes->push_back(std::move(registration.runtime));
  }
  return true;
}

}  // namespace

bool RegisterFormatter(lua_State* state,
                       std::string_view plugin_id,
                       std::vector<PluginHost::ContributedFormatter>* formatters,
                       std::string* error_message) {
  return RegisterSimpleContribution<PluginHost::ContributedFormatter,
                                    registration_parsers::FormatterRegistration>(
      state, plugin_id, formatters, error_message,
      registration_parsers::ParseFormatterRegistration);
}

bool RegisterVirtualDocument(lua_State* state,
                             std::string_view plugin_id,
                             std::vector<PluginHost::ContributedVirtualDocument>* documents,
                             std::string* error_message) {
  if (documents == nullptr) {
    return false;
  }
  if (ContributionLimitReached(documents, error_message)) {
    return false;
  }
  registration_parsers::VirtualDocumentRegistration registration;
  if (!registration_parsers::ParseVirtualDocumentRegistration(state, std::string(plugin_id),
                                                              &registration, error_message)) {
    return false;
  }
  // Last registration of a URI wins, so a plugin can republish a document's
  // content by re-registering it rather than accumulating duplicates the
  // registry would then have to disambiguate.
  for (PluginHost::ContributedVirtualDocument& existing : *documents) {
    if (existing.uri == registration.contributed.uri) {
      existing = std::move(registration.contributed);
      return true;
    }
  }
  documents->push_back(std::move(registration.contributed));
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
  return RegisterContributionWithRuntime<PluginHost::ContributedCompletion,
                                         runtime_types::CompletionRuntime,
                                         registration_parsers::CompletionRegistration>(
      state, plugin_id, contributions, runtimes, error_message,
      registration_parsers::ParseCompletionRegistration);
}

bool RegisterCodeAction(lua_State* state,
                        std::string_view plugin_id,
                        std::vector<PluginHost::ContributedCodeAction>* contributions,
                        std::vector<runtime_types::CodeActionRuntime>* runtimes,
                        std::string* error_message) {
  return RegisterContributionWithRuntime<PluginHost::ContributedCodeAction,
                                         runtime_types::CodeActionRuntime,
                                         registration_parsers::CodeActionRegistration>(
      state, plugin_id, contributions, runtimes, error_message,
      registration_parsers::ParseCodeActionRegistration);
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
  return RegisterUniqueContribution<PluginHost::ContributedTask,
                                    registration_parsers::TaskRegistration>(
      state, plugin_id, tasks, id_index, "task", error_message,
      registration_parsers::ParseTaskRegistration);
}

bool RegisterLanguageServer(lua_State* state,
                            std::string_view plugin_id,
                            std::vector<PluginHost::ContributedLanguageServer>* servers,
                            std::unordered_set<std::string>* id_index,
                            std::string* error_message) {
  return RegisterUniqueContribution<PluginHost::ContributedLanguageServer,
                                    registration_parsers::LanguageServerRegistration>(
      state, plugin_id, servers, id_index, "language server", error_message,
      registration_parsers::ParseLanguageServerRegistration);
}

bool RegisterDebugAdapter(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedDebugAdapter>* adapters,
                          std::unordered_set<std::string>* id_index,
                          std::string* error_message) {
  return RegisterUniqueContribution<PluginHost::ContributedDebugAdapter,
                                    registration_parsers::DebugAdapterRegistration>(
      state, plugin_id, adapters, id_index, "debug adapter", error_message,
      registration_parsers::ParseDebugAdapterRegistration);
}

bool RegisterLaunchConfig(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedLaunchConfig>* configs,
                          std::unordered_set<std::string>* id_index,
                          std::string* error_message) {
  return RegisterUniqueContribution<PluginHost::ContributedLaunchConfig,
                                    registration_parsers::LaunchConfigRegistration>(
      state, plugin_id, configs, id_index, "launch config", error_message,
      registration_parsers::ParseLaunchConfigRegistration);
}

bool RegisterTool(lua_State* state,
                  std::string_view plugin_id,
                  std::vector<PluginHost::ContributedTool>* tools,
                  std::unordered_set<std::string>* id_index,
                  std::string* error_message) {
  return RegisterUniqueContribution<PluginHost::ContributedTool,
                                    registration_parsers::ToolRegistration>(
      state, plugin_id, tools, id_index, "tool", error_message,
      registration_parsers::ParseToolRegistration);
}

bool RegisterTestProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedTestProvider>* providers,
                          std::vector<runtime_types::TestProviderRuntime>* runtimes,
                          std::string* error_message) {
  return RegisterContributionWithRuntime<PluginHost::ContributedTestProvider,
                                         runtime_types::TestProviderRuntime,
                                         registration_parsers::TestProviderRegistration>(
      state, plugin_id, providers, runtimes, error_message,
      registration_parsers::ParseTestProviderRegistration);
}

bool RegisterScmProvider(lua_State* state,
                         std::string_view plugin_id,
                         std::vector<PluginHost::ContributedScmProvider>* providers,
                         std::vector<runtime_types::ScmProviderRuntime>* runtimes,
                         std::string* error_message) {
  return RegisterContributionWithRuntime<PluginHost::ContributedScmProvider,
                                         runtime_types::ScmProviderRuntime,
                                         registration_parsers::ScmProviderRegistration>(
      state, plugin_id, providers, runtimes, error_message,
      registration_parsers::ParseScmProviderRegistration);
}

bool RegisterAnnotationProvider(
    lua_State* state,
    std::string_view plugin_id,
    std::vector<PluginHost::ContributedAnnotationProvider>* providers,
    std::vector<runtime_types::AnnotationProviderRuntime>* runtimes,
    std::string* error_message) {
  return RegisterContributionWithRuntime<PluginHost::ContributedAnnotationProvider,
                                         runtime_types::AnnotationProviderRuntime,
                                         registration_parsers::AnnotationProviderRegistration>(
      state, plugin_id, providers, runtimes, error_message,
      registration_parsers::ParseAnnotationProviderRegistration);
}

bool RegisterAuthProvider(lua_State* state,
                          std::string_view plugin_id,
                          std::vector<PluginHost::ContributedAuthProvider>* providers,
                          std::vector<runtime_types::AuthProviderRuntime>* runtimes,
                          std::string* error_message) {
  return RegisterContributionWithRuntime<PluginHost::ContributedAuthProvider,
                                         runtime_types::AuthProviderRuntime,
                                         registration_parsers::AuthProviderRegistration>(
      state, plugin_id, providers, runtimes, error_message,
      registration_parsers::ParseAuthProviderRegistration);
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
