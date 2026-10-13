#include "workspace/TerminalPaneLayout.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Fuzz the per-project session decoder (editor groups, split tree, sidebar and
// tree state, the terminal layout to revive). DecodeProjectSessionRecord must
// never crash on arbitrary bytes, and what it accepts must stay inside the caps
// a revive trusts (tabs, panes per tab).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::byte>(data[i]);
  }
  microide::workspace::PersistedProjectSessionState state;
  if (microide::workspace::DecodeProjectSessionRecord(
          std::span<const std::byte>(bytes.data(), bytes.size()), &state)) {
    if (state.terminal_layout.tabs.size() > microide::workspace::kMaxRevivedTerminalTabs) {
      __builtin_trap();
    }
    for (const auto& tab : state.terminal_layout.tabs) {
      if (tab.panes.empty() || tab.panes.size() > microide::workspace::kMaxTerminalPanes ||
          tab.weights.size() > microide::workspace::kMaxTerminalPanes) {
        __builtin_trap();
      }
    }
  }
  return 0;
}
