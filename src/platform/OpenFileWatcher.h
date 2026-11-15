#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace microide::platform {

// Watches a set of individual files -- the documents open in the editor -- by
// their directories, wherever they are. The project watcher covers the project's
// tracked tree only: it prunes `.git`, ignored and excluded directories, it does
// not follow a symlink back into the tree, and it knows nothing outside the root.
// A clean buffer of a file in any of those places was never told its file changed
// (soak: `.git/HEAD` after a branch switch, `.git/index`, a file opened through a
// symlinked directory), kept the old text, and a save would have written it back.
//
// A file is watched through its directory, so an atomic replace (write a temp,
// rename it over) is seen like an in-place write; a symlinked file is watched at
// its target as well, and reported under the path it is open as.
//
// A file whose directory does not exist (yet, or any more: a stash took the
// folder, a branch switch has not brought it back) is waited for from the
// nearest directory that does; when its directory is back the watcher arms it
// on its own and reports the file, which may have been written before the watch
// existed. Nothing has to call SetFiles again for that.
//
// Threading contract: the callback fires on the watcher's own thread with the
// paths that changed (each once per wake); SetFiles is called from one thread.
class OpenFileWatcher {
 public:
  using Callback = std::function<void(std::vector<std::filesystem::path>)>;

  explicit OpenFileWatcher(Callback callback);
  ~OpenFileWatcher();

  OpenFileWatcher(const OpenFileWatcher&) = delete;
  OpenFileWatcher& operator=(const OpenFileWatcher&) = delete;

  // What the caller last named its file set with: SetFiles only needs calling
  // when the caller's own fingerprint of the set differs. Reads 0 again after a
  // watched directory went away (deleted or moved), so the next call re-arms
  // what has come back in its place.
  std::uint64_t files_signature() const { return signature_.load(std::memory_order_acquire); }

  // Replaces the watched set. `signature` is the caller's fingerprint of it.
  void SetFiles(std::uint64_t signature, const std::vector<std::filesystem::path>& files);

 private:
  struct Entry {
    std::string name;                // the directory entry's name
    std::filesystem::path reported;  // the path the document is open as
  };
  // A file whose directory is missing, waited for from an existing ancestor.
  struct Waiting {
    std::filesystem::path file;      // the file to watch, through its directory
    std::filesystem::path reported;  // the path the document is open as
  };
  using EntriesByWatch = std::unordered_map<int, std::vector<Entry>>;
  using WaitingByWatch = std::unordered_map<int, std::vector<Waiting>>;
  enum class Armed { Watching, Waiting, Unwatchable };

  bool StartLocked();
  void WorkerMain();
  // Watches `file`'s directory into `entries`, or, when that directory is
  // missing, its nearest existing ancestor into `waiting`. Under mutex_.
  Armed ArmLocked(const std::filesystem::path& file, const std::filesystem::path& reported,
                  EntriesByWatch& entries, WaitingByWatch& waiting);
  // Something appeared under a waited-on ancestor: arms again every waiting
  // file, reporting into `changed` each one whose directory is now watched.
  void RearmWaitingLocked(std::vector<std::filesystem::path>& changed);
  // Removes the watches in `before` that neither map uses any more.
  void DropUnusedWatchesLocked(const EntriesByWatch& before_entries,
                               const WaitingByWatch& before_waiting);

  Callback callback_;
  std::atomic<std::uint64_t> signature_{0};
  std::mutex mutex_;
  int inotify_fd_ = -1;
  int control_pipe_[2] = {-1, -1};
  EntriesByWatch entries_by_watch_;
  WaitingByWatch waiting_by_watch_;
  std::thread worker_;
};

}  // namespace microide::platform
