#include "workspace/TerminalPaneLayout.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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
    if (workspace.host_view_collapsed.size() > 1024) {
      __builtin_trap();
    }
    for (const auto& id : workspace.host_view_collapsed) {
      if (id.size() > 4096) {
        __builtin_trap();
      }
    }
    for (const auto& id : workspace.projects) {
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
        if (tab.merge_conflict_spans.size() > microide::workspace::kMaxPersistedMergeConflictSpans) {
          __builtin_trap();
        }
      }
    }
    for (const auto& tab : state.terminal_layout.tabs) {
      if (tab.panes.empty() || tab.panes.size() > microide::workspace::kMaxTerminalPanes ||
          tab.weights.size() > microide::workspace::kMaxTerminalPanes) {
        __builtin_trap();
      }
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
