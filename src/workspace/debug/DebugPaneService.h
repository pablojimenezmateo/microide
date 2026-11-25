#pragma once

#include <functional>

#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

class DebugPaneService {
 public:
  struct Operations {
    std::function<void()> request_frame;
    std::function<void()> note_layout_inputs_changed;
  };

  DebugPaneService(ProjectWorkspaceState& state, Operations operations);

  void ShowMode(DebugPaneMode mode);

  void Close();
  void Toggle();

  void OpenOnStop();

 private:
  void Activate(DebugPaneMode mode);

  ProjectWorkspaceState& state_;
  Operations operations_;
};

}
