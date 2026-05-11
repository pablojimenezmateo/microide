#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace microide::workspace {

class LspManager;
class WorkspaceLanguageContract;
struct ProjectWorkspaceState;

// Coordinates debounced LSP `textDocument/documentSymbol` and regex fallback
// into `ProjectWorkspaceState::sidebar::outline`.
class WorkspaceOutlineService {
 public:
  using SettingGetter = std::function<std::optional<std::string>(std::string_view)>;

  WorkspaceOutlineService() = default;

  void ScheduleDebouncedRefresh();

  void Poll(uint32_t time_ms,
            bool outline_setting_enabled,
            ProjectWorkspaceState& project,
            LspManager& lsp,
            const WorkspaceLanguageContract& contracts,
            const SettingGetter& get_setting);

  void SetFixedClockMsForTesting(std::optional<uint32_t> ms) { fixed_clock_ms_ = ms; }
  std::size_t refresh_count_for_testing() const { return refresh_count_for_testing_; }
  std::size_t lsp_request_count_for_testing() const { return lsp_request_count_for_testing_; }
  void ResetCountsForTesting();

 private:
  uint32_t NowMs() const;
  void RefreshOutlineForActiveEditor(bool prefer_immediate_async,
                                     ProjectWorkspaceState& project,
                                     LspManager& lsp,
                                     const WorkspaceLanguageContract& contracts,
                                     const SettingGetter& get_setting);

  std::optional<uint32_t> debounce_deadline_ms_;
  bool debounce_pending_ = false;
  std::size_t tracked_tab_index_ = static_cast<std::size_t>(-1);
  std::string tracked_doc_key_;
  std::uint64_t lsp_callback_token_ = 0;

  std::optional<uint32_t> fixed_clock_ms_;
  std::size_t refresh_count_for_testing_ = 0;
  std::size_t lsp_request_count_for_testing_ = 0;
};

}  // namespace microide::workspace
