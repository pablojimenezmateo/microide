#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>

#include "platform/AppDirectories.h"
#include "util/StartupTrace.h"
#include "util/TextFileIO.h"
#include "workspace/WorkspacePersistenceFormat.h"
namespace microide::workspace {

std::filesystem::path PersistenceCoordinator::WorkspaceSessionStatePath() const {
  const std::filesystem::path state_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  return state_root.empty() ? std::filesystem::path{} : state_root / "workspace-session";
}

bool PersistenceCoordinator::RestoreWorkspaceSession() {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::RestoreWorkspaceSession");
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty()) {
    return false;
  }
  const auto text = util::ReadTextFile(session_path);
  if (!text.has_value()) {
    return false;
  }

  PersistedWorkspaceSessionState persisted_session;
  if (!ParseWorkspaceSessionText(*text, &persisted_session)) {
    return false;
  }

  context_.project_catalog.entries.clear();
  context_.project_catalog.active_index = 0;
  context_.project_catalog.tab_scroll_index = 0;

  if (persisted_session.project_roots.empty()) {
    operations_.reset_project_catalog_to_welcome_state();
    return true;
  }

  for (const auto& root : persisted_session.project_roots) {
    const std::filesystem::path normalized_root = operations_.resolve_project_root_input(root);
    std::error_code error;
    if (normalized_root.empty() || !std::filesystem::exists(normalized_root, error) || error ||
        !std::filesystem::is_directory(normalized_root, error)) {
      continue;
    }
    auto project_state = std::make_unique<ProjectWorkspaceState>();
    project_state->root = normalized_root;
    project_state->restore_persistence_on_activate = true;
    context_.project_catalog.entries.push_back(std::move(project_state));
  }

  if (context_.project_catalog.entries.empty()) {
    operations_.reset_project_catalog_to_welcome_state();
    return true;
  }

  if (!operations_.restore_project_catalog_after_removal(
          std::min(persisted_session.active_project_index,
                   context_.project_catalog.entries.size() - 1),
          true)) {
    return true;
  }
  operations_.ensure_active_project_visible();
  return true;
}

void PersistenceCoordinator::SaveWorkspaceSession() const {
  const std::filesystem::path session_path = WorkspaceSessionStatePath();
  if (session_path.empty()) {
    return;
  }

  PersistedWorkspaceSessionState persisted_session;
  persisted_session.active_project_index =
      context_.project_catalog.entries.empty()
          ? 0
          : std::min(context_.project_catalog.active_index,
                     context_.project_catalog.entries.size() - 1);
  persisted_session.project_roots.reserve(context_.project_catalog.entries.size());
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    const auto* entry = context_.ProjectCatalogEntry(i);
    const std::filesystem::path project_root = entry != nullptr ? entry->root
                                                                : context_.ProjectCatalogRoot(i);
    if (project_root.empty()) {
      continue;
    }
    persisted_session.project_roots.push_back(project_root.lexically_normal());
  }
  util::WriteTextFileAtomically(session_path, SerializeWorkspaceSession(persisted_session));
}

}  // namespace microide::workspace
