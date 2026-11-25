#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

struct LaunchConfig;

class QuickPickService {
 public:
  struct Operations {
    std::function<void()> show_overlay;
    std::function<bool()> showing;
    std::function<void()> list_changed;
    std::function<void(std::size_t)> start_launch_config;
    std::function<void(std::string_view, bool persist)> apply_colorscheme;
    std::function<void(const std::filesystem::path&, std::size_t, std::size_t)> reveal;
    std::function<void(const QuickPickState::View&)> restore_view;
    std::function<void(QuickPickKind, const std::string& query)> requery;
    std::function<void(const std::filesystem::path&, std::size_t line, std::size_t character)>
        open_workspace_symbol;
    std::function<void(const std::string& language)> set_language;
    std::function<void(std::uint64_t stable_id)> activate_tab;
    std::function<void(const std::string& command_line)> run_command_line;
  };

  void Configure(QuickPickState& state, Operations operations);

  void Open(QuickPickState picker);
  std::uint64_t open_count() const { return open_count_; }
  void Refilter();
  void SelectionChanged();
  void Accept();
  void Cancel();
  bool DeliverItems(QuickPickKind kind, std::vector<QuickPickItem> items);
  void Fail(QuickPickKind kind, std::string reason);

  static QuickPickState LaunchConfigs(std::span<const LaunchConfig> configs);
  static QuickPickState ColorThemes(std::span<const std::string> names, std::string_view active);
  static QuickPickState DocumentSymbolsLoading(QuickPickState::View view);
  static std::vector<QuickPickItem> DocumentSymbolItems(
      std::span<const plugin::PluginHost::DocumentSymbolNode> symbols);
  static QuickPickState WorkspaceSymbols(std::string query);
  static QuickPickState RecentEditors(const EditorGroup& group, const std::filesystem::path& root, bool back);
  static QuickPickState CommandChoices(std::string title, std::string noun, std::vector<QuickPickItem> items);
  static QuickPickState LanguageModes(std::span<const std::string> languages, std::string_view chosen,
                                      std::string_view detected);

 private:
  void Preview();
  void ShowItemsAsTheyAre();

  QuickPickState* state_ = nullptr;
  Operations operations_;
  std::uint64_t open_count_ = 0;
};

}
