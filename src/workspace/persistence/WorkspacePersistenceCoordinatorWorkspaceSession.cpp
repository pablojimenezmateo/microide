#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

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
  if (operations_.restore_host_view_collapsed) {
    operations_.restore_host_view_collapsed(persisted_session.host_view_collapsed);
  }

  // The identity records when the file has them; a file written before them names
  // its projects by path alone, and each is named here as a typed path would be.
  const bool from_ids = !persisted_session.projects.empty();
  const std::size_t project_count =
      from_ids ? persisted_session.projects.size() : persisted_session.project_roots.size();
  if (project_count == 0) {
    operations_.reset_project_catalog_to_welcome_state();
    return true;
  }

  // `active_project_index` indexes the ORIGINAL list. Roots that no longer exist on
  // disk are culled below, which shifts every surviving entry's index down — so the
  // saved active index must be remapped, not just clamped, or a missing project
  // positioned before the active one activates the wrong project.
  std::size_t restored_active_index = 0;
  for (std::size_t original_index = 0; original_index < project_count; ++original_index) {
    // The catalog entry is created here, so its id is fixed here: a remote project
    // is remote (and labelled as one) before it is ever activated.
    project::ProjectId id;
    std::filesystem::path normalized_root;
    if (!from_ids) {
      normalized_root =
          operations_.resolve_project_root_input(persisted_session.project_roots[original_index]);
      id = project::ResolveProjectId(normalized_root);
    } else if (persisted_session.projects[original_index].IsRemote()) {
      id = persisted_session.projects[original_index];
      normalized_root = id.IoRoot();
    } else {
      normalized_root = operations_.resolve_project_root_input(
          persisted_session.projects[original_index].local()->root);
      id = project::ProjectId::ForLocal(normalized_root);
    }
    std::error_code error;
    if (normalized_root.empty() || !std::filesystem::exists(normalized_root, error) || error ||
        !std::filesystem::is_directory(normalized_root, error)) {
      continue;
    }
    auto project_state = std::make_unique<ProjectWorkspaceState>();
    project_state->root = normalized_root;
    project_state->id = std::move(id);
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
  persisted_session.projects.reserve(context_.project_catalog.entries.size());
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
    // Both lists, parallel: the identity, and its root for an older build.
    // An entry with a root and no id is a plain folder (ProjectWorkspaceState's
    // default): written as one, so the two lists never differ in length.
    const project::ProjectId& id = context_.ProjectCatalogId(i);
    persisted_session.projects.push_back(
        id.empty() ? project::ProjectId::ForLocal(project_root.lexically_normal()) : id);
    persisted_session.project_roots.push_back(project_root.lexically_normal());
  }
  persisted_session.active_project_index = active_filtered_index;
  if (operations_.host_view_collapsed) {
    persisted_session.host_view_collapsed = operations_.host_view_collapsed();
  }
  operations_.persistence_service->SaveWorkspaceSession(session_path, persisted_session);
}

}  // namespace microide::workspace
