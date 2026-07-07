#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "editor/TextViewport.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/LanguageDetection.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceTextSearch.h"
#include "workspace/WorkspaceUiText.h"

namespace microide::workspace {

namespace {

void ClampSelectionToItemCount(std::size_t item_count, std::size_t* selected_index) {
  if (selected_index == nullptr) {
    return;
  }
  *selected_index = item_count == 0 ? 0 : std::min(*selected_index, item_count - 1);
}

// Depth-first flatten of the plugin document-symbol tree into the flat
// SidebarItem row set the outline view renders. `depth` drives host-drawn
// indentation; outline rows are not collapsible (fully expanded first pass).
void FlattenDocumentSymbols(const std::vector<plugin::PluginHost::DocumentSymbolNode>& nodes,
                            const std::filesystem::path& path,
                            int depth,
                            std::vector<plugin::PluginHost::SidebarItem>* out) {
  for (const auto& node : nodes) {
    out->push_back(plugin::PluginHost::SidebarItem{
        .label = node.name,
        .detail = node.detail.empty() ? node.kind : node.detail,
        .path = path,
        .line = node.line,
        .column = node.column,
        .id = node.name,
        .depth = depth,
        .collapsible = false,
        .collapsed = false,
    });
    FlattenDocumentSymbols(node.children, path, depth + 1, out);
  }
}

void ApplyGitRefreshSnapshot(GitSidebarState& git_state,
                             const GitSidebarState::RefreshSnapshot& snapshot,
                             const std::filesystem::path& project_root) {
  git_state.entries.clear();
  git_state.entries.reserve(snapshot.entries.size());
  for (const auto& entry : snapshot.entries) {
    GitSidebarEntry git_entry{
        .section = entry.section,
        .path = (project_root / entry.relative_path).lexically_normal(),
        .relative_path = entry.relative_path,
        .status = entry.conflicted ? project::GitFileStatus::Conflicted : entry.status,
        .conflicted = entry.conflicted,
        .staged = entry.staged,
        .provider_id = {},
        .provider_label = {},
        .supports_stage = false,
        .supports_discard = false,
    };
    const GitSidebarActionAvailability availability = GitSidebarActionAvailabilityForEntry(
        git_entry, snapshot.repo_available, git_state.supports_mutations);
    git_entry.supports_stage = availability.stage;
    git_entry.supports_discard = availability.discard;
    git_state.entries.push_back(std::move(git_entry));
  }
  git_state.repo_available = snapshot.repo_available;
  git_state.branch_label = snapshot.branch_label;
  git_state.upstream_label = snapshot.upstream_label;
  git_state.ahead = snapshot.ahead;
  git_state.behind = snapshot.behind;
  git_state.base_ref = snapshot.base_ref;
  git_state.base_label = snapshot.base_label;
  git_state.snapshot_stale = snapshot.snapshot_stale;
  git_state.refresh_error = snapshot.refresh_error;
  git_state.snapshot_generation = snapshot.generation;
}

}  // namespace

void SidebarCoordinator::RefreshGit() {
  if (state_.sidebar.git.refreshing &&
      operations_.consume_git_refresh_snapshot != nullptr) {
    GitSidebarState::RefreshSnapshot pending_snapshot;
    if (!operations_.consume_git_refresh_snapshot(&pending_snapshot)) {
      if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git) {
        operations_.request_sidebar_redraw();
      }
      return;
    }
    if (pending_snapshot.includes_tree_git_statuses) {
      state_.directory_tree.ApplyGitStatuses(std::move(pending_snapshot.tree_git_statuses));
    }

    const std::filesystem::path previous_path =
        state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
            ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].path
            : std::filesystem::path{};
    const GitSidebarEntry::Section previous_section =
        state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
            ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].section
            : GitSidebarEntry::Section::Changed;

    state_.sidebar.git.selected_index = 0;
    ApplyGitRefreshSnapshot(state_.sidebar.git, pending_snapshot, project_root_);
    state_.sidebar.git.refreshing = false;

    for (std::size_t i = 0; i < state_.sidebar.git.entries.size(); ++i) {
      if (state_.sidebar.git.entries[i].path == previous_path &&
          state_.sidebar.git.entries[i].section == previous_section) {
        state_.sidebar.git.selected_index = i;
        RevealSelectedGitLine();
        if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git) {
          operations_.request_sidebar_redraw();
        }
        return;
      }
    }

    RevealSelectedGitLine();
    operations_.request_window_redraw();
    if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git) {
      operations_.request_sidebar_redraw();
    }
    return;
  }

  const std::filesystem::path previous_path =
      state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
          ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].path
          : std::filesystem::path{};
  const GitSidebarEntry::Section previous_section =
      state_.sidebar.git.selected_index < state_.sidebar.git.entries.size()
          ? state_.sidebar.git.entries[state_.sidebar.git.selected_index].section
          : GitSidebarEntry::Section::Changed;

  state_.sidebar.git.selected_index = 0;
  if (project_root_.empty()) {
    state_.sidebar.git.entries.clear();
    return;
  }

  GitSidebarState::RefreshSnapshot snapshot;
  bool has_snapshot =
      operations_.consume_git_refresh_snapshot != nullptr &&
      operations_.consume_git_refresh_snapshot(&snapshot);
  if (!has_snapshot) {
    if (!state_.sidebar.git.refreshing && state_.sidebar.visible &&
        ActiveSidebarMode() == SidebarMode::Git &&
        operations_.request_git_refresh != nullptr) {
      operations_.request_git_refresh();
    }
    operations_.request_sidebar_redraw();
    return;
  }
  if (snapshot.includes_tree_git_statuses) {
    state_.directory_tree.ApplyGitStatuses(std::move(snapshot.tree_git_statuses));
  }

  ApplyGitRefreshSnapshot(state_.sidebar.git, snapshot, project_root_);
  // Preserve refresh-in-flight state when data was rendered from a synchronous
  // fallback while an async refresh request is still pending.
  state_.sidebar.git.refreshing = false;

  for (std::size_t i = 0; i < state_.sidebar.git.entries.size(); ++i) {
    if (state_.sidebar.git.entries[i].path == previous_path &&
        state_.sidebar.git.entries[i].section == previous_section) {
      state_.sidebar.git.selected_index = i;
      RevealSelectedGitLine();
      if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git) {
        operations_.request_sidebar_redraw();
      }
      return;
    }
  }

  RevealSelectedGitLine();
  operations_.request_window_redraw();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git) {
    operations_.request_sidebar_redraw();
  }
}

bool SidebarCoordinator::RefreshProblems() {
  std::optional<editor::PublishedDiagnostic> previous_diagnostic;
  if (state_.sidebar.problems.selected_index < state_.sidebar.problems.entries.size()) {
    previous_diagnostic =
        state_.sidebar.problems.entries[state_.sidebar.problems.selected_index].diagnostic;
  }

  state_.sidebar.problems.entries.clear();
  state_.sidebar.problems.selected_index = 0;
  for (const auto& diagnostic : state_.diagnostics_store.SnapshotAll()) {
    ProblemsSidebarEntry entry;
    const std::string collapsed_message = CollapseWhitespace(diagnostic.message);
    entry.diagnostic = diagnostic;
    entry.primary_label = collapsed_message.empty() ? "Diagnostic" : collapsed_message;
    entry.detail_label = RelativePathLabel(project_root_, diagnostic.path) + ":" +
                         std::to_string(diagnostic.range.start.line + 1) + ":" +
                         std::to_string(diagnostic.range.start.column + 1);
    if (!diagnostic.owner.empty()) {
      entry.detail_label += " | " + diagnostic.owner;
    }
    state_.sidebar.problems.entries.push_back(std::move(entry));
  }

  if (previous_diagnostic.has_value()) {
    for (std::size_t i = 0; i < state_.sidebar.problems.entries.size(); ++i) {
      if (state_.sidebar.problems.entries[i].diagnostic == *previous_diagnostic) {
        state_.sidebar.problems.selected_index = i;
        break;
      }
    }
  }

  RevealSelectedProblemsLine();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Problems) {
    operations_.request_sidebar_redraw();
  }
  return !state_.sidebar.problems.entries.empty();
}

bool SidebarCoordinator::RefreshTests() {
  const bool populated =
      operations_.refresh_tests_sidebar_state ? operations_.refresh_tests_sidebar_state() : false;
  ClampSelectionToItemCount(state_.sidebar.tests.entries.size(),
                            &state_.sidebar.tests.selected_index);
  RevealSelectedTestsLine();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Tests) {
    operations_.request_sidebar_redraw();
  }
  return populated;
}

bool SidebarCoordinator::RefreshPlugin() {
  // The outline view is a builtin that shares this item-tree storage but is
  // populated from the host's document-symbol query rather than a plugin snapshot.
  if (state_.sidebar.view_id == "outline") {
    return RefreshOutline();
  }
  state_.sidebar.plugin.items.clear();
  state_.sidebar.plugin.error.clear();
  state_.sidebar.plugin.placeholder.clear();
  state_.sidebar.plugin.placeholder_is_error = false;
  state_.sidebar.plugin.selected_index = 0;
  if (state_.sidebar.view_id.empty() || FindBuiltinSidebarView(state_.sidebar.view_id) != nullptr) {
    return false;
  }
  if (plugin_runtime_.Host().FindSidebarProvider(state_.sidebar.view_id) == nullptr) {
    state_.sidebar.view_id = "tree";
    if (state_.sidebar.visible) {
      operations_.request_sidebar_redraw();
    }
    return false;
  }

  // Show the (empty) loading state immediately; the snapshot runs on the plugin
  // worker and fills items in on the drain without blocking the UI.
  const std::string request_view_id = state_.sidebar.view_id;
  RecomputePluginSidebarPlaceholder();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Plugin) {
    operations_.request_sidebar_redraw();
  }

  plugin_runtime_.Host().SnapshotSidebarAsync(
      request_view_id,
      [this, request_view_id](bool ok, std::vector<plugin::PluginHost::SidebarItem> items,
                              std::string error_message) {
        if (state_.sidebar.view_id != request_view_id) {
          return;  // superseded: the active sidebar view changed
        }
        state_.sidebar.plugin.items.clear();
        state_.sidebar.plugin.error.clear();
        if (!ok) {
          state_.sidebar.plugin.error = std::move(error_message);
        } else {
          state_.sidebar.plugin.items = std::move(items);
        }
        ClampSelectionToItemCount(state_.sidebar.plugin.items.size(),
                                  &state_.sidebar.plugin.selected_index);
        RecomputePluginSidebarPlaceholder();
        if (ok) {
          RevealSelectedPluginLine();
        }
        if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Plugin) {
          operations_.request_sidebar_redraw();
        }
      });
  return true;
}

bool SidebarCoordinator::RefreshOutline() {
  state_.sidebar.plugin.items.clear();
  state_.sidebar.plugin.error.clear();
  state_.sidebar.plugin.placeholder.clear();
  state_.sidebar.plugin.placeholder_is_error = false;
  state_.sidebar.plugin.selected_index = 0;

  editor::TextViewport* viewport =
      operations_.active_editor_viewport ? operations_.active_editor_viewport() : nullptr;
  if (viewport == nullptr || viewport->path().empty()) {
    RecomputePluginSidebarPlaceholder();
    if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Outline) {
      operations_.request_sidebar_redraw();
    }
    return false;
  }

  const std::string language_id = DetectViewportLanguageId(*viewport);
  const std::filesystem::path request_path = viewport->path();
  // Show the loading state immediately; document symbols are queried on the worker
  // and flattened in on the drain without blocking the UI.
  RecomputePluginSidebarPlaceholder();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Outline) {
    operations_.request_sidebar_redraw();
  }

  // The document-symbol query completes on a later main-thread drain, by which time
  // this coordinator (a stack temporary) is destroyed. So the callback must NOT
  // capture `this`: it routes the result back through the shell, which re-creates a
  // fresh coordinator to apply it. `apply` holds the shell, not the coordinator.
  auto apply = operations_.apply_plugin_outline_result;
  plugin_runtime_.Host().QueryDocumentSymbolsAsync(
      language_id, request_path,
      [apply = std::move(apply), request_path](
          std::vector<plugin::PluginHost::DocumentSymbolNode> symbols, std::string error_message) {
        if (apply) {
          apply(request_path, std::move(symbols), std::move(error_message));
        }
      });
  return true;
}

void SidebarCoordinator::ApplyPluginOutlineResult(
    const std::filesystem::path& request_path,
    std::vector<plugin::PluginHost::DocumentSymbolNode> symbols, std::string plugin_error) {
  if (state_.sidebar.view_id != "outline") {
    return;
  }
  editor::TextViewport* current =
      operations_.active_editor_viewport ? operations_.active_editor_viewport() : nullptr;
  if (current == nullptr || current->path() != request_path) {
    return;
  }
  // No plugin symbols: fall back to the language server's documentSymbol so the
  // outline works for any LSP-backed language even without a plugin provider (the
  // shell issues that async request and applies its result through its own fresh
  // coordinator). Otherwise flatten the plugin symbols directly.
  if (symbols.empty() && operations_.query_lsp_document_symbols) {
    operations_.query_lsp_document_symbols(*current, request_path, std::move(plugin_error));
    return;
  }
  // Same flatten/placeholder/reveal path the LSP result uses.
  ApplyLspOutlineResult(request_path, plugin_error, symbols);
}

void SidebarCoordinator::ApplyLspOutlineResult(
    const std::filesystem::path& request_path, const std::string& plugin_error,
    const std::vector<plugin::PluginHost::DocumentSymbolNode>& lsp_symbols) {
  // Superseded if the user switched away from the outline or changed buffer while the
  // request was in flight.
  if (state_.sidebar.view_id != "outline") {
    return;
  }
  editor::TextViewport* current =
      operations_.active_editor_viewport ? operations_.active_editor_viewport() : nullptr;
  if (current == nullptr || current->path() != request_path) {
    return;
  }
  state_.sidebar.plugin.items.clear();
  state_.sidebar.plugin.error.clear();
  // Surface the plugin provider's "no provider" message only when LSP also came back
  // empty, so a working LSP fallback never shows a misleading plugin error.
  if (lsp_symbols.empty() && !plugin_error.empty()) {
    state_.sidebar.plugin.error = plugin_error;
  }
  FlattenDocumentSymbols(lsp_symbols, request_path, 0, &state_.sidebar.plugin.items);
  ClampSelectionToItemCount(state_.sidebar.plugin.items.size(),
                            &state_.sidebar.plugin.selected_index);
  RecomputePluginSidebarPlaceholder();
  RevealSelectedPluginLine();
  if (state_.sidebar.visible && ActiveSidebarMode() == SidebarMode::Outline) {
    operations_.request_sidebar_redraw();
  }
}

void SidebarCoordinator::RecomputePluginSidebarPlaceholder() {
  PluginSidebarState& plugin = state_.sidebar.plugin;
  if (!plugin.error.empty()) {
    plugin.placeholder_is_error = true;
    plugin.placeholder.assign("Error: ").append(plugin.error);
    return;
  }
  plugin.placeholder_is_error = false;
  if (plugin.items.empty()) {
    // Outline shares this storage but reports symbols, not generic items.
    plugin.placeholder =
        FormatEmptyState(ActiveSidebarMode() == SidebarMode::Outline ? "symbols" : "items");
  } else {
    plugin.placeholder.clear();
  }
}

void SidebarCoordinator::RevealListSelection(
    std::size_t count, std::size_t selected_index,
    const std::function<ScrollableListLayout(const SDL_FRect&, std::size_t)>& compute_layout) {
  if (count == 0 || selected_index >= count) {
    return;
  }
  const auto layout_state = operations_.current_workspace_layout();
  if (!layout_state.has_value() || layout_state->sidebar.h <= 0.0f) {
    return;
  }
  const ScrollableListLayout list_layout = compute_layout(layout_state->sidebar, count);
  state_.sidebar.scroll_row =
      RevealScrollableListIndex(list_layout, static_cast<int>(selected_index));
}

void SidebarCoordinator::RevealSelectedTreeLine() {
  RevealListSelection(state_.directory_tree.entries().size(),
                      state_.directory_tree.selected_index(),
                      operations_.compute_tree_sidebar_list_layout);
}

void SidebarCoordinator::RevealSelectedGitLine() {
  const auto selected_line = operations_.selected_git_sidebar_line_index();
  if (!selected_line.has_value()) {
    return;
  }
  // Git rows are derived from a built line model, not a flat entries vector.
  RevealListSelection(operations_.build_git_sidebar_lines().size(), *selected_line,
                      operations_.compute_git_sidebar_list_layout);
}

void SidebarCoordinator::RevealSelectedProblemsLine() {
  RevealListSelection(state_.sidebar.problems.entries.size(),
                      state_.sidebar.problems.selected_index,
                      operations_.compute_problems_sidebar_list_layout);
}

void SidebarCoordinator::RevealSelectedTestsLine() {
  RevealListSelection(state_.sidebar.tests.entries.size(), state_.sidebar.tests.selected_index,
                      operations_.compute_tests_sidebar_list_layout);
}

void SidebarCoordinator::RevealSelectedPluginLine() {
  RevealListSelection(state_.sidebar.plugin.items.size(), state_.sidebar.plugin.selected_index,
                      operations_.compute_plugin_sidebar_list_layout);
}

}  // namespace microide::workspace
