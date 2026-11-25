#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugValueTreePane.h"

namespace microide::workspace {

class DebugVariablesModel : public DebugValueTreePane {
 public:
  DebugVariablesModel() { tree_.SetDefaultExpandedScope("Locals"); }

  void BeginSession() { tree_.ResetExpansionForNewSession(); }

  void BeginFrame(int frame_id) {
    tree_.Clear();
    frame_id_ = frame_id;
  }
  int FrameId() const { return frame_id_; }

  std::vector<DebugValueTree::ChildFetch> ApplyScopes(
      const std::vector<dap_protocol::DapScope>& scopes);

  void Clear() {
    tree_.Clear();
    frame_id_ = 0;
  }

 private:
  int frame_id_ = 0;
};

}
