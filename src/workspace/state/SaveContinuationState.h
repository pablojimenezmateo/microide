#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "workspace/state/WorkspacePromptState.h"

namespace microide::workspace {

// Work that has to wait for deferred saves to land: close a tab once its write is
// on disk, rename/delete a path once the buffers under it are written, close a
// project, quit. Format-on-save runs off the shell thread (TD-2026-09-28-304), so
// none of these may block on it; each registers here instead and runs when the
// last tab it waits on has finished its save.
//
// A continuation is DATA, not a closure. Every one of them is run by the shell's
// dirty-buffer flows (DirtyPromptCoordinator::SettleSave), which re-resolve what
// they act on when they run; a closure would have captured a coordinator built for
// one call, which is exactly the dangling-`this` bug this codebase has paid for.
//
// Tabs are named by their stable id, never an index: tabs close in whatever order
// their formatters finish, so an index captured at registration addresses a
// different tab by the time it is used.
struct SaveContinuation {
  enum class Kind {
    CloseTab,      // close `tab_id` once it is written
    PathMutation,  // replay `path_mutation` (the rename/delete prompt) once written
    CloseProject,  // close `project_root`, then return to `return_root`
    Quit,          // save `remaining_roots` in turn, return to `return_root`, quit
  };

  std::uint64_t id = 0;  // assigned by the queue
  Kind kind = Kind::CloseTab;
  // Stable ids of the tabs whose deferred saves are still running.
  std::vector<std::uint64_t> waiting_tab_ids;
  // How many tabs this step waited on in total, for the progress row.
  std::size_t total_tab_count = 0;
  // The project those tabs live in. A continuation never runs against another
  // project: by the time it runs the user may have switched away or closed it.
  std::filesystem::path project_root;

  std::uint64_t tab_id = 0;                          // CloseTab
  std::optional<PromptSurfaceState> path_mutation;   // PathMutation
  std::filesystem::path return_root;                 // CloseProject, Quit
  bool switched_to_project = false;                  // CloseProject: return afterwards
  std::vector<std::filesystem::path> remaining_roots;  // Quit: projects still to save
};

class SaveContinuationQueue {
 public:
  // What a finished save settled.
  struct Settled {
    std::vector<SaveContinuation> ready;      // every tab it waited on is written
    std::vector<SaveContinuation> cancelled;  // one of them was not
  };

  // Register; returns the id it was given. A continuation with nothing to wait on
  // is the caller's to run at once, so registering one is a programming error and
  // returns 0 without storing it.
  std::uint64_t Add(SaveContinuation continuation);

  // The deferred save of `tab_id` finished. A successful write removes the tab
  // from every continuation waiting on it and hands back those with nothing left
  // to wait for; a failed or refused one cancels every continuation waiting on it.
  Settled Complete(std::uint64_t tab_id, bool saved);

  // Drop one (its Cancel button); nullopt when it already ran or was cancelled.
  std::optional<SaveContinuation> Remove(std::uint64_t id);

  const SaveContinuation* Find(std::uint64_t id) const;
  bool IsWaitingOn(std::uint64_t tab_id) const;
  bool Empty() const { return continuations_.empty(); }
  const std::vector<SaveContinuation>& All() const { return continuations_; }

 private:
  std::vector<SaveContinuation> continuations_;
  std::uint64_t next_id_ = 1;
};

}  // namespace microide::workspace
