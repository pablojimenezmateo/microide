#pragma once

#include <string>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::registration_parsers {

struct CompletionRegistration {
  PluginHost::ContributedCompletion contributed;
  runtime_types::CompletionRuntime runtime;
  bool has_runtime = false;
};

struct CodeActionRegistration {
  PluginHost::ContributedCodeAction contributed;
  runtime_types::CodeActionRuntime runtime;
  bool has_runtime = false;
};

struct LanguageQueryRegistration {
  runtime_types::LanguageQueryRuntime runtime;
  bool has_runtime = false;
};

struct TestProviderRegistration {
  PluginHost::ContributedTestProvider contributed;
  runtime_types::TestProviderRuntime runtime;
  bool has_runtime = false;
};

struct TaskRegistration {
  PluginHost::ContributedTask contributed;
};

struct MenuEntryRegistration {
  PluginHost::ContributedMenuEntry contributed;
};

struct KeybindingRegistration {
  PluginHost::ContributedKeybinding contributed;
};

struct SettingRegistration {
  PluginHost::ContributedSettingSpec contributed;
};

struct StatusItemRegistration {
  PluginHost::ContributedStatusItem contributed;
};

struct FormatterRegistration {
  PluginHost::ContributedFormatter contributed;
};

struct SaveParticipantRegistration {
  PluginHost::ContributedSaveParticipant contributed;
  runtime_types::SaveParticipantRuntime runtime;
};

struct LanguageServerRegistration {
  PluginHost::ContributedLanguageServer contributed;
};

struct DebugAdapterRegistration {
  PluginHost::ContributedDebugAdapter contributed;
};

struct LaunchConfigRegistration {
  PluginHost::ContributedLaunchConfig contributed;
};

struct ToolRegistration {
  PluginHost::ContributedTool contributed;
};

struct ScmProviderRegistration {
  PluginHost::ContributedScmProvider contributed;
  runtime_types::ScmProviderRuntime runtime;
  bool has_runtime = false;
};

struct AnnotationProviderRegistration {
  PluginHost::ContributedAnnotationProvider contributed;
  runtime_types::AnnotationProviderRuntime runtime;
  bool has_runtime = false;
};

struct AuthProviderRegistration {
  PluginHost::ContributedAuthProvider contributed;
  runtime_types::AuthProviderRuntime runtime;
  bool has_runtime = false;
};

struct AiProviderRegistration {
  PluginHost::ContributedAiProvider contributed;
};

struct ExternalAgentRegistration {
  PluginHost::ContributedExternalAgent contributed;
};

struct McpToolRegistration {
  PluginHost::ContributedMcpTool contributed;
  runtime_types::McpToolRuntime runtime;
  bool has_runtime = false;
};

struct BracketSetRegistration {
  PluginHost::ContributedBracketSet contributed;
};

struct CommentMarkersRegistration {
  PluginHost::ContributedCommentMarkers contributed;
};

struct IndentRulesRegistration {
  PluginHost::ContributedIndentRules contributed;
};

struct SnippetRegistration {
  PluginHost::ContributedSnippet contributed;
};

#if MICROIDE_HAS_LUA_PLUGINS
bool ParseCompletionRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 CompletionRegistration* out,
                                 std::string* error_message);

bool ParseCodeActionRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 CodeActionRegistration* out,
                                 std::string* error_message);

bool ParseLanguageQueryRegistration(lua_State* state,
                                    const std::string& plugin_id,
                                    runtime_types::LanguageQueryKind kind,
                                    LanguageQueryRegistration* out,
                                    std::string* error_message);

bool ParseTestProviderRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   TestProviderRegistration* out,
                                   std::string* error_message);

bool ParseTaskRegistration(lua_State* state,
                           const std::string& plugin_id,
                           TaskRegistration* out,
                           std::string* error_message);

bool ParseMenuEntryRegistration(lua_State* state,
                                const std::string& plugin_id,
                                MenuEntryRegistration* out,
                                std::string* error_message);

bool ParseKeybindingRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 KeybindingRegistration* out,
                                 std::string* error_message);

bool ParseSettingRegistration(lua_State* state,
                              const std::string& plugin_id,
                              SettingRegistration* out,
                              std::string* error_message);

bool ParseStatusItemRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 StatusItemRegistration* out,
                                 std::string* error_message);

bool ParseFormatterRegistration(lua_State* state,
                                const std::string& plugin_id,
                                FormatterRegistration* out,
                                std::string* error_message);

bool ParseSaveParticipantRegistration(lua_State* state,
                                      const std::string& plugin_id,
                                      SaveParticipantRegistration* out,
                                      std::string* error_message);

bool ParseLanguageServerRegistration(lua_State* state,
                                     const std::string& plugin_id,
                                     LanguageServerRegistration* out,
                                     std::string* error_message);

bool ParseDebugAdapterRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   DebugAdapterRegistration* out,
                                   std::string* error_message);

bool ParseLaunchConfigRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   LaunchConfigRegistration* out,
                                   std::string* error_message);

bool ParseToolRegistration(lua_State* state,
                           const std::string& plugin_id,
                           ToolRegistration* out,
                           std::string* error_message);

bool ParseScmProviderRegistration(lua_State* state,
                                  const std::string& plugin_id,
                                  ScmProviderRegistration* out,
                                  std::string* error_message);

bool ParseAnnotationProviderRegistration(lua_State* state,
                                         const std::string& plugin_id,
                                         AnnotationProviderRegistration* out,
                                         std::string* error_message);

bool ParseAuthProviderRegistration(lua_State* state,
                                   const std::string& plugin_id,
                                   AuthProviderRegistration* out,
                                   std::string* error_message);

bool ParseAiProviderRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 AiProviderRegistration* out,
                                 std::string* error_message);

bool ParseExternalAgentRegistration(lua_State* state,
                                    const std::string& plugin_id,
                                    ExternalAgentRegistration* out,
                                    std::string* error_message);

bool ParseMcpToolRegistration(lua_State* state,
                              const std::string& plugin_id,
                              McpToolRegistration* out,
                              std::string* error_message);

bool ParseBracketSetRegistration(lua_State* state,
                                 const std::string& plugin_id,
                                 BracketSetRegistration* out,
                                 std::string* error_message);

bool ParseCommentMarkersRegistration(lua_State* state,
                                     const std::string& plugin_id,
                                     CommentMarkersRegistration* out,
                                     std::string* error_message);

bool ParseIndentRulesRegistration(lua_State* state,
                                  const std::string& plugin_id,
                                  IndentRulesRegistration* out,
                                  std::string* error_message);

bool ParseSnippetRegistration(lua_State* state,
                              const std::string& plugin_id,
                              SnippetRegistration* out,
                              std::string* error_message);
#endif

}  // namespace microide::plugin::registration_parsers
