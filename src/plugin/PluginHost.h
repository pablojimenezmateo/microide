#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microide::plugin {

class PluginHost {
 public:
  struct Callbacks {
    std::function<bool(std::string_view)> is_command_name_available;
    std::function<bool(const std::filesystem::path&)> open_file;
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
