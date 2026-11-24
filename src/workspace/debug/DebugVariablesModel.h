#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugValueTreePane.h"

namespace microide::workspace {

// The lazy variables tree for the focused stack frame. A thin wrapper over the
// shared DebugValueTree (which owns the node store, flatten, lazy-expand and
// inline-edit machinery shared with the Watch panel); this model adds only the
// scope/frame semantics. Lives on ProjectWorkspaceState next to
// `debug_execution`; transient — rebuilt on each `stopped`/frame focus, cleared
// on resume/stop. Never persisted.
//
// The model performs no I/O: DebugService drives `scopes`/`variables`/
// `setVariable` requests and feeds responses back through Apply*.
class DebugVariablesModel : public DebugValueTreePane {
 public:
  // The Locals scope is open by default each session (open-on-stop UX); the shared
  // tree handles the bounded re-expansion and respects an explicit collapse.
  DebugVariablesModel() { tree_.SetDefaultExpandedScope("Locals"); }

  // Called when a new debug session launches: forget the prior session's expansion
  // and re-arm the Locals-open-by-default one-shot for this session's first stop.
  void BeginSession() { tree_.ResetExpansionForNewSession(); }

  // Start a fresh tree for a focused frame. Clears everything; the service then
  // requests scopes for `frame_id`.
  void BeginFrame(int frame_id) {
    tree_.Clear();
    frame_id_ = frame_id;
  }
  int FrameId() const { return frame_id_; }

  // Install the focused frame's scopes as collapsed top-level rows, re-expanding
  // any the user had open before this stop. Returns the bounded fetches the
  // service must issue to repopulate those restored containers.
  std::vector<DebugValueTree::ChildFetch> ApplyScopes(
      const std::vector<dap_protocol::DapScope>& scopes);

  void Clear() {
    tree_.Clear();
    frame_id_ = 0;
  }

 private:
  int frame_id_ = 0;
};

}  // namespace microide::workspace
