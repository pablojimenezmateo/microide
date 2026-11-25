#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

#include <algorithm>

#include "platform/AppDirectories.h"
#include "util/StartupTrace.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "util/InvokeIfSet.h"
namespace microide::workspace {

namespace {

bool SessionHoldsUnsavedWork(const PersistenceService& persistence, const std::filesystem::path& root) {
  PersistedProjectSessionState session;
  if (!persistence.LoadProjectSession(ProjectStateDirectory(root) / "session", &session)) {
    return false;
  }
  return std::any_of(session.groups.begin(), session.groups.end(), [](const PersistedEditorGroupState& group) {
    return std::any_of(group.tabs.begin(), group.tabs.end(),
                       [](const PersistedEditorTabState& tab) { return tab.dirty_snapshot; });
  });
}

}

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
  util::InvokeIfSet(operations_.restore_host_view_collapsed, persisted_session.host_view_collapsed);

  const bool from_ids = !persisted_session.projects.empty();
  const std::size_t project_count =
      from_ids ? persisted_session.projects.size() : persisted_session.project_roots.size();
  if (project_count == 0) {
    operations_.reset_project_catalog_to_welcome_state();
    return true;
  }

  std::size_t restored_active_index = 0;
  for (std::size_t original_index = 0; original_index < project_count; ++original_index) {
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
    bool root_missing = false;
    if (normalized_root.empty() || !std::filesystem::exists(normalized_root, error) || error ||
        !std::filesystem::is_directory(normalized_root, error)) {
      if (normalized_root.empty() || id.IsRemote() ||
          !SessionHoldsUnsavedWork(*operations_.persistence_service, normalized_root)) {
        continue;
      }
      root_missing = true;
    }
    auto project_state = std::make_unique<ProjectWorkspaceState>();
    project_state->root = normalized_root;
    project_state->root_missing = root_missing;
    project_state->id = std::move(id);
    project_state->restore_persistence_on_activate = true;
    if (operations_.persistence_service != nullptr) {
      HydrateProjectBaseColorFromConfig(*project_state, *operations_.persistence_service);
    }
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

}
