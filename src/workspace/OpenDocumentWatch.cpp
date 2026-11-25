#include "workspace/OpenDocumentWatch.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

#include "platform/FileIndexWatcher.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {
namespace {

template <typename Visit>
void ForEachOpenDocumentPath(const ProjectWorkspaceState& project, Visit&& visit) {
  for (const EditorGroup& group : project.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      const editor::TextViewport* view =
          tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()
              ? &tab.editor_state->viewport
          : tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() && tab.compare->right_editable
              ? &tab.compare->right_viewport
              : nullptr;
      if (view == nullptr) {
        continue;
      }
      const std::filesystem::path& path = view->path();
      if (!path.empty() && path.is_absolute()) {
        visit(path);
      }
    }
  }
}

}

void SyncOpenDocumentWatch(const ProjectWorkspaceState& project, platform::FileIndexWatcher* watcher) {
  platform::OpenFileWatcher* open_files = watcher != nullptr ? watcher->open_files() : nullptr;
  if (open_files == nullptr) {
    return;
  }
  std::uint64_t signature = 0xcbf29ce484222325ULL;
  ForEachOpenDocumentPath(project, [&signature](const std::filesystem::path& path) {
    signature ^= std::hash<std::string_view>{}(path.native());
    signature *= 0x100000001b3ULL;
  });
  signature |= 1;
  if (open_files->files_signature() == signature) {
    return;
  }
  std::vector<std::filesystem::path> paths;
  ForEachOpenDocumentPath(project, [&paths](const std::filesystem::path& path) { paths.push_back(path); });
  open_files->SetFiles(signature, paths);
}

}
