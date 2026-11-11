#pragma once

namespace microide::platform {
class FileIndexWatcher;
}

namespace microide::workspace {

struct ProjectWorkspaceState;

// Hands the project's open documents to its watcher's open-file watch
// (FileIndexWatcher::WatchOpenFiles), whenever the set differs from what it last
// handed over. Called once per prepared frame: a fingerprint of the open paths
// costs no allocation, and the list is built only when that fingerprint moves --
// a tab opened, closed or saved under another name -- or after a watched
// directory went away and its watch with it.
void SyncOpenDocumentWatch(const ProjectWorkspaceState& project, platform::FileIndexWatcher* watcher);

}  // namespace microide::workspace
