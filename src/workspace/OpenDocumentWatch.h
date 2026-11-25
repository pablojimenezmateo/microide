#pragma once

namespace microide::platform {
class FileIndexWatcher;
}

namespace microide::workspace {

struct ProjectWorkspaceState;

void SyncOpenDocumentWatch(const ProjectWorkspaceState& project, platform::FileIndexWatcher* watcher);

}
