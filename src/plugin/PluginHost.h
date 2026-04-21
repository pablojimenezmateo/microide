#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/DiagnosticsStore.h"

namespace microide::plugin {

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
  };

  struct HoverResult {
    std::string title;
    std::string content;

    bool operator==(const HoverResult&) const = default;
  };

  struct ContributedMenuEntry {
    std::string id;
    std::string menu;           // "file", "edit", "view", "search"
    std::string action;         // built-in command name or plugin command name
    std::string label;
    std::string accelerator;
    bool separator_before = false;
    std::string plugin_id;
  };

  struct ContributedKeybinding {
    std::string id;
    std::string action;         // command name
    std::string key_chord;      // "Ctrl+S" format
    std::string context;        // "global", "editor", "sidebar", "terminal"
    std::string plugin_id;
  };

  struct ContributedSettingSpec {
    std::string id;
    std::string label;
    std::string description;
    std::string type;           // "bool", "int", "float", "string", "enum"
    std::string scope;          // "user", "project"
    std::string default_value;  // string-serialised default
    std::vector<std::string> enum_values;
    std::string plugin_id;
  };

  struct ContributedStatusItem {
    std::string id;
    std::string text;
    std::string tooltip;
    std::string alignment;      // "left", "right"
    int priority = 0;
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

  struct ContributedDebugger {
    std::string id;
    std::string type;  // "lldb", "python", "node", etc.
    std::vector<std::string> command;
    std::string plugin_id;
  };

  struct ContributedTestProvider {
    std::string id;
    std::string language_id;
    std::string plugin_id;
  };

  struct ContributedScmProvider {
    std::string id;
    std::string label;
    std::string plugin_id;
  };

  struct ContributedAnnotationProvider {
    std::string id;
    std::string label;
    std::string type;  // "blame", "decoration", "margin"
    std::string language_id;
    std::string plugin_id;
  };

  struct ContributedAuthProvider {
    std::string id;
    std::string label;
    std::string plugin_id;
  };

  struct Callbacks {
    std::function<bool(std::string_view)> is_command_name_available;
    std::function<bool(const OpenFileRequest&)> open_file;
    std::function<std::optional<ActiveBuffer>()> active_buffer;
    std::function<bool(std::string_view)> show_sidebar;
    std::function<void(std::string_view,
                       const std::filesystem::path&,
                       std::vector<editor::Diagnostic>)>
        publish_diagnostics;
    std::function<void(std::string_view, const std::filesystem::path&)> clear_file_diagnostics;
    std::function<void(std::string_view)> clear_owner_diagnostics;
    std::function<void(const std::string&)> error_sink;
    std::function<void(const std::string&)> log_sink;
    std::function<std::optional<std::string>(std::string_view)> get_setting;
    std::function<void()> request_status_redraw;
  };

  PluginHost();
  ~PluginHost();
  PluginHost(const PluginHost&) = delete;
  PluginHost& operator=(const PluginHost&) = delete;
  PluginHost(PluginHost&&) noexcept;
  PluginHost& operator=(PluginHost&&) noexcept;

  void SetCallbacks(Callbacks callbacks);
  bool enabled() const;
  bool Reload(const std::filesystem::path& project_root);
  void Shutdown();
  void OnBufferOpen(const std::filesystem::path& path);
  void OnBufferSave(const std::filesystem::path& path);
  bool ExecuteCommand(std::string_view name,
                      const std::vector<std::string>& args,
                      std::string* error_message = nullptr);
  const std::vector<std::string>& CommandNames() const;
  const std::vector<SidebarProviderInfo>& SidebarProviders() const;
  const SidebarProviderInfo* FindSidebarProvider(std::string_view id) const;
  bool SnapshotSidebar(std::string_view id,
                       std::vector<SidebarItem>* items,
                       std::string* error_message = nullptr);
  bool ConfirmSidebarItem(std::string_view id,
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
  bool UpdateStatusItem(std::string_view id, std::string text, std::string tooltip = {});
  const std::vector<ContributedFormatter>& ContributedFormatters() const;
  const std::vector<ContributedSaveParticipant>& ContributedSaveParticipants() const;
  const std::vector<ContributedCompletion>& ContributedCompletions() const;
  const std::vector<ContributedCodeAction>& ContributedCodeActions() const;
  const std::vector<ContributedTask>& ContributedTasks() const;
  const std::vector<ContributedTool>& ContributedTools() const;
  const std::vector<ContributedDebugger>& ContributedDebuggers() const;
  const std::vector<ContributedTestProvider>& ContributedTestProviders() const;
  const std::vector<ContributedScmProvider>& ContributedScmProviders() const;
  const std::vector<ContributedAnnotationProvider>& ContributedAnnotationProviders() const;
  const std::vector<ContributedAuthProvider>& ContributedAuthProviders() const;
  const std::vector<std::string>& Messages() const;
  const std::vector<std::string>& Errors() const;
  void ClearMessages();
  std::string ReloadSummary() const;
  std::size_t LoadedPluginCount() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace microide::plugin
