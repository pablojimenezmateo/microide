#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "platform/ProcessLauncher.h"
#include "platform/SubprocessSandbox.h"
#include "project/FileWriteGate.h"
#include "project/ProjectLocality.h"
#include "util/JsonValue.h"

namespace microide::plugin {

class PluginThread;

class PluginHost {
 public:
  struct OpenFileRequest {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
  };

  struct ActiveBuffer {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
    std::uint64_t content_revision = 0;
  };

  struct SidebarProviderInfo {
    std::string id;
    std::string label;
    std::string plugin_id;
  };

  struct SidebarItem {
    std::string label;
    std::string detail;
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
    std::string id;
    int depth = 0;
    bool collapsible = false;
    bool collapsed = false;
  };

  struct HoverResult {
    std::string title;
    std::string content;

    bool operator==(const HoverResult&) const = default;
  };

  struct ContributedMenuEntry {
    std::string id;
    std::string menu;
    std::string action;
    std::string label;
    std::string accelerator;
    bool separator_before = false;
    std::string plugin_id;
  };

  struct ContributedKeybinding {
    std::string id;
    std::string action;
    std::string key_chord;
    std::string context;
    std::string plugin_id;
    bool skips_terminal = false;
  };

  struct ContributedSettingSpec {
    std::string id;
    std::string label;
    std::string description;
    std::string type;
    std::string scope;
    std::string default_value;
    std::vector<std::string> enum_values;
    std::string plugin_id;
  };

  struct ContributedStatusItem {
    std::string id;
    std::string text;
    std::string tooltip;
    std::string alignment;
    int priority = 0;
    std::string icon;
    std::string tone;
    std::string command;
    float progress = -1.0f;
    std::string plugin_id;
  };

  struct ContributedFormatter {
    std::string id;
    std::string language_id;
    std::string label;
    std::vector<std::string> command;
    std::string plugin_id;
  };

  struct ContributedSaveParticipant {
    std::string id;
    std::string plugin_id;
  };

  struct ContributedVirtualDocument {
    std::string uri;
    std::string language_id;
    std::string content;
    bool editable = false;
    std::string plugin_id;
  };

  struct ContributedCompletion {
    std::string id;
    std::string language_id;
    std::string trigger_characters;
    std::string plugin_id;
  };

  struct ContributedCodeAction {
    std::string id;
    std::string language_id;
    std::string plugin_id;
  };

  struct ContributedLanguageProvider {
    enum class Kind { Definition, References, SignatureHelp, DocumentSymbol };
    Kind kind = Kind::Definition;
    std::string id;
    std::string language_id;
    std::string plugin_id;
    std::string trigger_characters;
  };

  struct ContributedLanguageServer {
    std::string id;
    std::vector<std::string> language_ids;
    std::vector<std::string> command;
    std::string plugin_id;
    util::JsonValue initialization_options;
    util::JsonValue settings;
    bool eager_start = false;
    platform::SubprocessSandbox sandbox;
  };

  struct ContributedDebugAdapter {
    std::string id;
    std::string type;
    std::vector<std::string> command;
    std::string plugin_id;
    platform::SubprocessSandbox sandbox;
  };

  struct ContributedLaunchConfig {
    std::string id;
    std::string name;
    std::string type;
    std::string request;
    std::string arguments_json;
    std::string plugin_id;
  };

  struct ContributedTask {
    std::string id;
    std::string label;
    std::string group;
    std::vector<std::string> command;
    std::string cwd;
    bool run_in_shell = false;
    std::string plugin_id;
  };

  struct ContributedTool {
    std::string id;
    std::string label;
    std::string platform;
    std::string download_url;
    std::string sha256;
    std::string install_dir;
    std::string plugin_id;
  };

  struct ContributedTestProvider {
    std::string id;
    std::string language_id;
    std::string plugin_id;
  };

  struct CompletionCandidate {
    std::string label;
    std::string detail;
    std::string documentation;
    std::string insert_text;
    bool is_snippet = false;
  };

  struct CodeActionCandidate {
    std::string title;
    std::string command;
    std::vector<std::string> arguments;
  };

  struct EditRequest {
    std::size_t start_line = 0;
    std::size_t start_column = 0;
    std::size_t end_line = 0;
    std::size_t end_column = 0;
    std::string text;
  };

  struct WorkspaceEditRequest {
    std::filesystem::path path;
    std::vector<EditRequest> edits;
    bool has_cursor = false;
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
    bool has_selection = false;
    std::size_t selection_start_line = 0;
    std::size_t selection_start_column = 0;
    std::size_t selection_end_line = 0;
    std::size_t selection_end_column = 0;
    bool has_staleness_guard = false;
    std::filesystem::path guard_path;
    std::uint64_t captured_content_revision = 0;
  };

  struct GhostTextRequest {
    std::filesystem::path path;
    std::size_t anchor_line = 0;
    std::size_t anchor_column = 0;
    std::string text;
  };

  struct LocationResult {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
  };

  struct SignatureParameter {
    std::string label;
    std::string documentation;
  };

  struct SignatureInfo {
    std::string label;
    std::string documentation;
    std::vector<SignatureParameter> parameters;
    int active_parameter = -1;
  };

  struct SignatureHelpResult {
    std::vector<SignatureInfo> signatures;
    int active_signature = 0;
  };

  struct DocumentSymbolNode {
    std::string name;
    std::string detail;
    std::string kind;
    std::size_t line = 0;
    std::size_t column = 0;
    std::vector<DocumentSymbolNode> children;
  };

  struct TestCase {
    std::string id;
    std::string label;
    std::filesystem::path file;
    int line = 0;
    std::string parent_id;
  };

  struct TestRunResult {
    std::string test_id;
    std::string state;
    std::string message;
    int duration_ms = 0;
  };

  struct ScmEntry {
    std::filesystem::path path;
    std::filesystem::path relative_path;
    std::string status;
    bool conflicted = false;
    bool staged = false;
    bool supports_stage = false;
    bool supports_discard = false;
  };

  struct ScmSnapshot {
    std::string base_ref;
    std::string base_label;
    std::vector<ScmEntry> entries;
    bool supports_mutations = false;
  };

  struct ContributedScmProvider {
    std::string id;
    std::string label;
    std::string plugin_id;
  };

  struct AnnotationLine {
    std::size_t line = 0;
    std::string text;
    std::string author;
    std::string summary;
    std::string date;
  };

  struct ContributedAnnotationProvider {
    std::string id;
    std::string label;
    std::string type;
    std::string language_id;
    std::string plugin_id;
  };

  struct ContributedAuthProvider {
    std::string id;
    std::string label;
    std::string plugin_id;
  };

  struct AuthSessionData {
    std::string id;
    std::string account;
    std::string access_token;
    std::vector<std::string> scopes;
  };

  struct ContributedBracketSet {
    std::string language_id;
    std::vector<std::pair<std::string, std::string>> bracket_pairs;
    std::vector<std::pair<std::string, std::string>> auto_close_pairs;
    std::vector<std::pair<std::string, std::string>> surround_pairs;
    std::string plugin_id;
  };

  struct ContributedCommentMarkers {
    std::string language_id;
    std::string line_comment;
    std::string block_comment_open;
    std::string block_comment_close;
    std::string plugin_id;
  };

  struct ContributedIndentRules {
    std::string language_id;
    std::vector<std::string> indent_after_open_patterns;
    std::vector<std::string> dedent_on_close_chars;
    std::string plugin_id;
  };

  struct ContributedSnippet {
    std::string id;
    std::string language_id;
    std::string prefix;
    std::string label;
    std::string body;
    std::string plugin_id;
  };

  struct ContributedThemeStyle {
    std::string group;
    std::optional<SDL_Color> foreground;
    std::optional<SDL_Color> background;
    bool reverse = false;
  };

  struct ContributedTheme {
    std::string id;
    std::string label;
    std::vector<ContributedThemeStyle> styles;
    std::string plugin_id;
  };

  struct ContributedFileIconRule {
    std::string matcher;
    bool match_filename = false;
    std::string icon;
    SDL_Color color{};
  };

  struct ContributedFileIconTheme {
    std::string id;
    std::string label;
    std::vector<ContributedFileIconRule> rules;
    std::string plugin_id;
  };

  struct Callbacks {
    std::function<bool(std::string_view)> is_command_name_available;
    std::function<bool(const OpenFileRequest&)> open_file;
    std::function<std::optional<ActiveBuffer>()> active_buffer;
    std::function<std::shared_ptr<const std::string>(const std::filesystem::path&)>
        live_buffer_text;
    std::function<bool(std::string_view)> show_sidebar;
    std::function<void(std::string_view,
                       const std::filesystem::path&,
                       std::vector<editor::Diagnostic>)>
        publish_diagnostics;
    std::function<void(std::string_view, const std::filesystem::path&)> clear_file_diagnostics;
    std::function<void(std::string_view)> clear_owner_diagnostics;
    std::function<void(std::string_view,
                       const std::filesystem::path&,
                       editor::PluginDecorationData)>
        publish_decorations;
    std::function<void(std::string_view, const std::filesystem::path&)> clear_file_decorations;
    std::function<void(std::string_view)> clear_owner_decorations;
    std::function<void(std::string_view, std::string_view, editor::SurfaceContent)>
        publish_surface;
    std::function<void(std::string_view, std::string_view)> clear_surface;
    std::function<void(std::string_view)> clear_owner_surfaces;
    std::function<void(std::uint64_t, int, std::vector<std::byte>, int, int)> decode_raster;
    std::function<bool(std::string_view owner, const WorkspaceEditRequest&)> apply_workspace_edit;
    std::function<void(std::string_view owner, const GhostTextRequest&)> publish_ghost_text;
    std::function<void(std::string_view owner)> clear_ghost_text;
    std::function<void(const std::string&)> error_sink;
    std::function<void(const std::string&)> log_sink;
    std::function<std::optional<std::string>(std::string_view)> get_setting;
    std::function<std::uint64_t()> settings_revision;
    std::function<void()> request_status_redraw;
    std::function<void(const std::string& level, const std::string& message)> show_notification;
  };

  struct LoadedPlugin {
    std::string id;
    std::filesystem::path root;
    bool enabled = true;
  };

  PluginHost();
  ~PluginHost();
  PluginHost(const PluginHost&) = delete;
  PluginHost& operator=(const PluginHost&) = delete;
  PluginHost(PluginHost&&) noexcept;
  PluginHost& operator=(PluginHost&&) noexcept;

  void SetCallbacks(Callbacks callbacks);
  void SetWorker(PluginThread* worker);
  bool enabled() const;
  void SetStartupPluginsEnabled(bool enabled);
  void SetDisabledPlugins(std::vector<std::string> disabled_ids);
  using ProjectLocality = project::ProjectLocality;
  bool Reload(const std::filesystem::path& project_root, ProjectLocality locality);
  bool Reload(const std::filesystem::path& project_root) {
    return Reload(project_root, ProjectLocality{});
  }
  void ReloadAsync(const std::filesystem::path& project_root, ProjectLocality locality,
                   std::function<void(bool)> on_complete);
  void ReloadAsync(const std::filesystem::path& project_root,
                   std::function<void(bool)> on_complete) {
    ReloadAsync(project_root, ProjectLocality{}, std::move(on_complete));
  }
  void Shutdown();
  void OnBufferOpen(const std::filesystem::path& path);
  void OnBufferSave(const std::filesystem::path& path);
  struct EditorEventInterest {
    bool buffer_change = false;
    bool cursor_move = false;
    bool selection_change = false;
    bool buffer_close = false;
    bool buffer_open = false;
    bool buffer_save = false;
    bool any() const {
      return buffer_change || cursor_move || selection_change || buffer_close;
    }
  };
  EditorEventInterest EditorEventInterests() const;
  void OnBufferChange(const std::filesystem::path& path,
                      std::size_t start_line,
                      std::size_t end_line);
  void OnCursorMove(const std::filesystem::path& path, std::size_t line, std::size_t column);
  void OnSelectionChange(const std::filesystem::path& path,
                         bool has_selection,
                         std::size_t start_line,
                         std::size_t start_column,
                         std::size_t end_line,
                         std::size_t end_column);
  void OnBufferClose(const std::filesystem::path& path);
  bool ExecuteCommand(std::string_view name,
                      const std::vector<std::string>& args,
                      std::string* error_message = nullptr,
                      std::string* feedback = nullptr);
  bool HasCommand(std::string_view name) const;
  void ExecuteCommandAsync(std::string name,
                           std::vector<std::string> args,
                           std::function<void(bool, std::string, std::string)> on_result);
  const std::vector<std::string>& CommandNames() const;
  const std::vector<SidebarProviderInfo>& SidebarProviders() const;
  const SidebarProviderInfo* FindSidebarProvider(std::string_view id) const;
  bool SnapshotSidebar(std::string_view id,
                       std::vector<SidebarItem>* items,
                       std::string* error_message = nullptr);
  bool ConfirmSidebarItem(std::string_view id,
                          const SidebarItem& item,
                          std::string* error_message = nullptr);
  bool ToggleSidebarItem(std::string_view id,
                         const SidebarItem& item,
                         std::string* error_message = nullptr);
  bool QueryHover(const std::filesystem::path& path,
                  std::size_t line,
                  std::size_t column,
                  HoverResult* result,
                  std::string* error_message = nullptr) const;
  std::vector<std::filesystem::path> DataDirectories(std::string_view subdirectory) const;
  const std::vector<ContributedMenuEntry>& ContributedMenuEntries() const;
  const std::vector<ContributedKeybinding>& ContributedKeybindings() const;
  const std::vector<ContributedSettingSpec>& ContributedSettings() const;
  const std::vector<ContributedStatusItem>& ContributedStatusItems() const;
  std::uint64_t StatusItemsRevision() const;
  bool RunSaveParticipants(const std::filesystem::path& path,
                           std::string* text,
                           std::string* error_message = nullptr) const;
  struct SaveParticipantOutcome {
    bool ok = true;
    bool timed_out = false;
    std::string text;
    std::string error;
  };
  std::function<SaveParticipantOutcome()> PrepareSaveParticipants(const std::filesystem::path& path,
                                                                  std::string text) const;
  void SetSaveParticipantDeadlineForTesting(std::chrono::milliseconds deadline);
  std::vector<CompletionCandidate> QueryCompletions(std::string_view language_id,
                                                    const std::filesystem::path& path,
                                                    std::size_t line,
                                                    std::size_t column,
                                                    std::string_view trigger_character = {},
                                                    std::string* error_message = nullptr) const;
  void QueryCompletionsAsync(
      std::string language_id,
      std::filesystem::path path,
      std::size_t line,
      std::size_t column,
      std::string trigger_character,
      std::function<void(std::vector<CompletionCandidate>, std::string)> on_result);
  void QueryCodeActionsAsync(
      std::string language_id,
      std::filesystem::path path,
      std::size_t start_line,
      std::size_t start_column,
      std::size_t end_line,
      std::size_t end_column,
      std::function<void(std::vector<CodeActionCandidate>, std::string)> on_result);
  void QueryDefinitionAsync(
      std::string language_id,
      std::filesystem::path path,
      std::size_t line,
      std::size_t column,
      std::function<void(std::vector<LocationResult>, std::string)> on_result);
  void QueryReferencesAsync(
      std::string language_id,
      std::filesystem::path path,
      std::size_t line,
      std::size_t column,
      bool include_declaration,
      std::function<void(std::vector<LocationResult>, std::string)> on_result);
  void QuerySignatureHelpAsync(
      std::string language_id,
      std::filesystem::path path,
      std::size_t line,
      std::size_t column,
      std::function<void(bool, SignatureHelpResult, std::string)> on_result);
  void QueryDocumentSymbolsAsync(
      std::string language_id,
      std::filesystem::path path,
      std::function<void(std::vector<DocumentSymbolNode>, std::string)> on_result);
  void QueryHoverAsync(const std::filesystem::path& path,
                       std::size_t line,
                       std::size_t column,
                       std::function<void(bool, HoverResult)> on_result);
  void SnapshotSidebarAsync(
      std::string id,
      std::function<void(bool, std::vector<SidebarItem>, std::string)> on_result);
  std::vector<CodeActionCandidate> QueryCodeActions(std::string_view language_id,
                                                    const std::filesystem::path& path,
                                                    std::size_t start_line,
                                                    std::size_t start_column,
                                                    std::size_t end_line,
                                                    std::size_t end_column,
                                                    std::string* error_message = nullptr) const;
  std::vector<LocationResult> QueryDefinition(std::string_view language_id,
                                              const std::filesystem::path& path,
                                              std::size_t line,
                                              std::size_t column,
                                              std::string* error_message = nullptr) const;
  std::vector<LocationResult> QueryReferences(std::string_view language_id,
                                              const std::filesystem::path& path,
                                              std::size_t line,
                                              std::size_t column,
                                              bool include_declaration,
                                              std::string* error_message = nullptr) const;
  bool QuerySignatureHelp(std::string_view language_id,
                          const std::filesystem::path& path,
                          std::size_t line,
                          std::size_t column,
                          SignatureHelpResult* result,
                          std::string* error_message = nullptr) const;
  std::vector<DocumentSymbolNode> QueryDocumentSymbols(
      std::string_view language_id,
      const std::filesystem::path& path,
      std::string* error_message = nullptr) const;
  bool DiscoverTests(std::string_view provider_id,
                     const std::filesystem::path& path,
                     std::vector<TestCase>* tests,
                     std::string* error_message = nullptr) const;
  bool RunTests(std::string_view provider_id,
                const std::vector<std::string>& test_ids,
                std::vector<TestRunResult>* results,
                std::string* error_message = nullptr) const;
  bool SnapshotScm(std::string_view provider_id,
                   ScmSnapshot* snapshot,
                   std::string* error_message = nullptr) const;
  std::vector<AnnotationLine> QueryAnnotations(std::string_view provider_id,
                                               const std::filesystem::path& path,
                                               std::string_view language_id,
                                               std::size_t visible_start_line,
                                               std::size_t visible_end_line,
                                               std::string* error_message = nullptr) const;
  bool LoginAuthProvider(std::string_view provider_id,
                         const std::vector<std::string>& scopes,
                         AuthSessionData* session,
                         std::string* error_message = nullptr) const;
  bool RefreshAuthSession(std::string_view provider_id,
                          std::string_view session_id,
                          AuthSessionData* session,
                          std::string* error_message = nullptr) const;
  bool LogoutAuthSession(std::string_view provider_id,
                         std::string_view session_id,
                         std::string* error_message = nullptr) const;
  const std::vector<ContributedFormatter>& ContributedFormatters() const;
  const std::vector<ContributedSaveParticipant>& ContributedSaveParticipants() const;
  const std::vector<ContributedVirtualDocument>& ContributedVirtualDocuments() const;
  const std::vector<ContributedCompletion>& ContributedCompletions() const;
  const std::vector<ContributedCodeAction>& ContributedCodeActions() const;
  const std::vector<ContributedLanguageProvider>& ContributedLanguageProviders() const;
  const ContributedLanguageProvider* FindLanguageProvider(ContributedLanguageProvider::Kind kind,
                                                          std::string_view language_id) const;
  bool HasLanguageProvider(ContributedLanguageProvider::Kind kind,
                           std::string_view language_id) const {
    return FindLanguageProvider(kind, language_id) != nullptr;
  }
  const std::vector<ContributedLanguageServer>& ContributedLanguageServers() const;
  const std::vector<ContributedDebugAdapter>& ContributedDebugAdapters() const;
  const std::vector<ContributedLaunchConfig>& ContributedLaunchConfigs() const;
  const std::vector<ContributedTask>& ContributedTasks() const;
  const std::vector<ContributedTool>& ContributedTools() const;
  const std::vector<ContributedTestProvider>& ContributedTestProviders() const;
  const std::vector<ContributedScmProvider>& ContributedScmProviders() const;
  const std::vector<ContributedAnnotationProvider>& ContributedAnnotationProviders() const;
  const std::vector<ContributedAuthProvider>& ContributedAuthProviders() const;
  const std::vector<ContributedBracketSet>& ContributedBrackets() const;
  const std::vector<ContributedCommentMarkers>& ContributedComments() const;
  const std::vector<ContributedIndentRules>& ContributedIndents() const;
  const std::vector<ContributedSnippet>& ContributedSnippets() const;
  const std::vector<ContributedTheme>& ContributedThemes() const;
  const std::vector<ContributedFileIconTheme>& ContributedFileIconThemes() const;
  static constexpr std::size_t kMaxRecordedLogEntries = 2048;
  const std::vector<std::string>& Messages() const;
  const std::vector<std::string>& Errors() const;
  void ClearMessages();
  std::string ReloadSummary() const;
  std::size_t LoadedPluginCount() const;
  std::vector<LoadedPlugin> LoadedPlugins() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  template <typename WorkerOp>
  bool DispatchSidebarQuery(std::string_view id,
                            std::string* error_message,
                            const char* unavailable_message,
                            WorkerOp worker_op);
};

}
