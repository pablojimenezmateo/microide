#pragma once

#include "platform/ProcessLauncher.h"
#include "project/FileWriteGate.h"

namespace microide::project {

struct ProjectLocality {
  const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
  FileWriteGate* write_gate = &LocalFileWriteGate();
};

}
