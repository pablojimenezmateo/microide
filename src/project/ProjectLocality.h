#pragma once

#include "platform/ProcessLauncher.h"
#include "project/FileWriteGate.h"

namespace microide::project {

// Where a project lives: the launcher its processes run through and the gate its
// writes go through (dev-docs/design/remote-projects.md § 6.1). Local by default;
// a remote project's pair belongs to its host session.
//
// It is part of the project's IDENTITY, like its root: fixed when the project is
// opened and carried through every reset of the project's scoped state. A
// locality installed after the open is too late -- the open registers language
// servers, loads plugins and starts git with whatever the project had then.
//
// LIFETIME: both must outlive every project state that points at them -- free for
// the local pair (function-local statics), a real constraint for a remote one.
struct ProjectLocality {
  const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
  FileWriteGate* write_gate = &LocalFileWriteGate();
};

}  // namespace microide::project
