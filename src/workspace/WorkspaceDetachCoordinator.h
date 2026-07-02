#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include "workspace/WorkspacePersistenceFormat.h"

namespace microide::workspace {

struct ControlInstanceDescriptor;

// A resolved reattach drop target: another running window whose published bounds
// contain the drop point.
struct TabDropTarget {
  int pid = 0;
  std::filesystem::path socket;
};

// Pure hit-test for a cross-window tab drop. Returns the first instance (other
// than `self_pid`) whose published global window bounds contain the point, or
// nullopt when the point is over no other window (→ detach into a new window).
// Instances with unpublished (all-zero) geometry are skipped. Unit-tested.
std::optional<TabDropTarget> ResolveDropTarget(
    int global_x, int global_y, int self_pid,
    const std::vector<ControlInstanceDescriptor>& instances);

// Orchestrates detaching a tab or a project into its own microide window. Because
// the shell is strictly single-window, "detach" launches a second microide
// process seeded from a handoff session file (an ordinary
// PersistedProjectSessionState, so dirty buffers and compare/merge state ride
// along). Lives off the shell (service-style Operations seam, never a
// WorkspaceShell& reference) so the shell stays a thin orchestrator and this
// file does not count against the WorkspaceShell*.cpp companion cap.
class WorkspaceDetachCoordinator {
 public:
  struct Operations {
    // Build a handoff payload capturing the focused group's active tab (one
    // group, one tab). Groups are empty when the active tab cannot be detached
    // (e.g. a transient/non-persistable compare/merge tab or an empty buffer).
    std::function<PersistedProjectSessionState()> build_active_tab_handoff;
    // Build a whole-project handoff (all groups/tabs) for the active project.
    std::function<PersistedProjectSessionState()> build_project_handoff;
    // Persist a handoff payload through the PersistedRecord writer. Returns false
    // on write failure.
    std::function<bool(const std::filesystem::path&, const PersistedProjectSessionState&)>
        write_handoff;
    // Root of the active project (the detached window opens the same project).
    std::function<std::filesystem::path()> project_root;
    // Active project index in the catalog (closed on a successful project detach).
    std::function<std::size_t()> active_project_index;
    std::function<void(std::size_t)> request_close_project;
    // Close the focused group's active tab WITHOUT the dirty prompt -- its content
    // already moved into the handoff, so there is nothing to lose.
    std::function<void()> close_active_tab_discarding_dirty;
    // Make this window discoverable as a reattach drop target (starts its control
    // channel + publishes geometry). Called before a detach so the tab can later
    // be dragged back into this window.
    std::function<void()> ensure_discoverable;
  };

  explicit WorkspaceDetachCoordinator(Operations operations);

  // Detach the focused group's active tab into a new window. Returns true when the
  // detached window was launched and the source tab was closed. False (no state
  // change) when the tab is not detachable or the spawn/write failed.
  bool DetachActiveTab();
  // Detach the active project into a new window. Returns true when launched (the
  // source project is then closed in the catalog).
  bool DetachActiveProject();
  // Drop the focused group's active tab at a global (desktop-space) point: if the
  // point lands over another microide window, hand the tab off to it (reattach);
  // otherwise detach into a new window. Returns true when the tab was moved.
  bool DropActiveTabAtGlobal(int global_x, int global_y);

 private:
  // Write `handoff` to a scratch file, then launch `<self> <root> --detach-handoff
  // <file> [--detach-owns-session]`. Returns true on launch; deletes the scratch
  // file and returns false on any failure so the caller leaves the source intact.
  bool SpawnHandoffWindow(const PersistedProjectSessionState& handoff,
                          const std::filesystem::path& project_root,
                          bool child_owns_session);

  Operations operations_;
};

}  // namespace microide::workspace
