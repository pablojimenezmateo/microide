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
    std::function<void(const std::string&)> log_sink;
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
