#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>

#include "platform/AppDirectories.h"
#include "util/StartupTrace.h"
#include "workspace/WorkspaceProjectPresentation.h"
namespace microide::workspace {

std::filesystem::path PersistenceCoordinator::WorkspaceSessionStatePath() const {
  const std::filesystem::path state_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  return state_root.empty() ? std::filesystem::path{} : state_root / "workspace-session";
}

bool PersistenceCoordinator::RestoreWorkspaceSession() {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::RestoreWorkspaceSession");
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty() || operations_.persistence_service == nullptr) {
    return false;
  }

  PersistedWorkspaceSessionState persisted_session;
  if (!operations_.persistence_service->LoadWorkspaceSession(session_path, &persisted_session)) {
    return false;
  }

  context_.project_catalog.entries.clear();
  context_.project_catalog.active_index = 0;
  context_.project_catalog.tab_scroll_index = 0;

  if (persisted_session.project_roots.empty()) {
    operations_.reset_project_catalog_to_welcome_state();
    return true;
  }

  // `active_project_index` indexes the ORIGINAL project_roots list. Roots that no
  // longer exist on disk are culled below, which shifts every surviving entry's
  // index down — so the saved active index must be remapped, not just clamped, or a
  // missing project positioned before the active one activates the wrong project.
  std::size_t restored_active_index = 0;
  for (std::size_t original_index = 0; original_index < persisted_session.project_roots.size();
       ++original_index) {
    const auto& root = persisted_session.project_roots[original_index];
    const std::filesystem::path normalized_root = operations_.resolve_project_root_input(root);
    std::error_code error;
    if (normalized_root.empty() || !std::filesystem::exists(normalized_root, error) || error ||
        !std::filesystem::is_directory(normalized_root, error)) {
      continue;
    }
    auto project_state = std::make_unique<ProjectWorkspaceState>();
    project_state->root = normalized_root;
    project_state->restore_persistence_on_activate = true;
    if (operations_.persistence_service != nullptr) {
      HydrateProjectBaseColorFromConfig(*project_state, *operations_.persistence_service);
    }
    // Track the surviving entry that best matches the saved active index: the one
    // with the greatest original index at or before it. If the active root itself
    // survived, this is its exact entry; if it was culled, we fall back to the
    // nearest surviving predecessor (or the first entry when none precede it).
    if (original_index <= persisted_session.active_project_index) {
      restored_active_index = context_.project_catalog.entries.size();
    }
    context_.project_catalog.entries.push_back(std::move(project_state));
  }

  if (context_.project_catalog.entries.empty()) {
    operations_.reset_project_catalog_to_welcome_state();
    return true;
  }

  if (!operations_.restore_project_catalog_after_removal(
          std::min(restored_active_index, context_.project_catalog.entries.size() - 1),
          true)) {
    return true;
  }
  operations_.ensure_active_project_visible();
  return true;
}

void PersistenceCoordinator::SaveWorkspaceSession() const {
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty() || operations_.persistence_service == nullptr) {
    return;
  }

  PersistedWorkspaceSessionState persisted_session;
  persisted_session.project_roots.reserve(context_.project_catalog.entries.size());
  // Emit active_project_index in the SAME (filtered) index space as project_roots:
  // empty-root entries are skipped below, so an entries-space index would diverge if
  // one ever preceded the active project. Record the filtered slot of the surviving
  // entry at or before the active entry (mirrors RestoreWorkspaceSession's remap).
  const std::size_t active_entry =
      context_.project_catalog.entries.empty()
          ? 0
          : std::min(context_.project_catalog.active_index,
                     context_.project_catalog.entries.size() - 1);
  std::size_t active_filtered_index = 0;
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    const auto* entry = context_.ProjectCatalogEntry(i);
    const std::filesystem::path project_root = entry != nullptr ? entry->root
                                                                : context_.ProjectCatalogRoot(i);
    if (project_root.empty()) {
      continue;
    }
    if (i <= active_entry) {
      active_filtered_index = persisted_session.project_roots.size();
    }
    persisted_session.project_roots.push_back(project_root.lexically_normal());
  }
  persisted_session.active_project_index = active_filtered_index;
  operations_.persistence_service->SaveWorkspaceSession(session_path, persisted_session);
}

}  // namespace microide::workspace
