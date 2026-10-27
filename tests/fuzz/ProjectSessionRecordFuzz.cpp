#include "workspace/TerminalPaneLayout.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Fuzz the session decoders in WorkspacePersistenceBinaryFormatSessions: the
// per-project session (editor groups, split tree, sidebar and tree state, the
// terminal layout to revive, the closed editors and each editor's collapsed
// folds), and the workspace session (the open projects, as project-identity
// records and legacy root paths). Neither may crash on arbitrary bytes, and what
// they accept must stay inside the caps a restore trusts.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::byte>(data[i]);
  }
  microide::workspace::PersistedWorkspaceSessionState workspace;
  if (microide::workspace::DecodeWorkspaceSessionRecord(
          std::span<const std::byte>(bytes.data(), bytes.size()), &workspace)) {
    if (workspace.projects.size() > 256 || workspace.project_roots.size() > 256) {
      __builtin_trap();
    }
    for (const auto& id : workspace.projects) {
      // A decoded id is always usable: a path, or a host with an absolute root.
      const auto* remote = id.remote();
      if (remote != nullptr ? (remote->host.empty() || remote->host_root.empty() || remote->host_root.front() != '/')
                            : id.empty()) {
        __builtin_trap();
      }
    }
  }
  microide::workspace::PersistedProjectSessionState state;
  if (microide::workspace::DecodeProjectSessionRecord(
          std::span<const std::byte>(bytes.data(), bytes.size()), &state)) {
    if (state.terminal_layout.tabs.size() > microide::workspace::kMaxRevivedTerminalTabs) {
      __builtin_trap();
    }
    if (state.closed_editors.size() > microide::workspace::kMaxPersistedClosedEditors) {
      __builtin_trap();
    }
    for (const auto& closed : state.closed_editors) {
      if (closed.collapsed_fold_openers.size() > microide::workspace::kMaxPersistedCollapsedFolds) {
        __builtin_trap();
      }
    }
    for (const auto& group : state.groups) {
      for (const auto& tab : group.tabs) {
        if (tab.collapsed_fold_openers.size() > microide::workspace::kMaxPersistedCollapsedFolds) {
          __builtin_trap();
        }
      }
    }
    for (const auto& tab : state.terminal_layout.tabs) {
      if (tab.panes.empty() || tab.panes.size() > microide::workspace::kMaxTerminalPanes ||
          tab.weights.size() > microide::workspace::kMaxTerminalPanes) {
        __builtin_trap();
      }
      // A recorded host terminal is whole or absent: a reopen must never attach
      // a handle with no server to tell it by.
      for (const auto& pane : tab.panes) {
        if (!pane.host.empty() && (pane.host.epoch.empty() ||
                                   pane.host.epoch.size() > microide::workspace::kMaxRecordedHostEpochBytes)) {
          __builtin_trap();
        }
        if (pane.host.empty() && !pane.host.epoch.empty()) {
          __builtin_trap();
        }
      }
    }
  }
  return 0;
}
