#pragma once

#include <string>

#include "util/JsonValue.h"

namespace microide::workspace {

// A single debug launch/attach configuration. Native format only — there is no
// `.vscode/launch.json` import (banned by the persistence invariants); configs
// persist through PersistenceService + PersistedRecord* in a later phase.
//
// `type` selects the contributed debug adapter (mirrors an LSP `language_id`
// selecting a server). `request` is the DAP request verb ("launch"/"attach").
// `arguments` is forwarded verbatim as the launch/attach request body, so it can
// carry adapter-specific keys (program, args, cwd, stopOnEntry, ...) without the
// host needing to model each one.
struct LaunchConfig {
  std::string name;             // human label shown in pickers / status
  std::string type;             // adapter type id, e.g. "debugpy", "lldb"
  std::string request = "launch";  // "launch" or "attach"
  util::JsonValue arguments;    // forwarded as the launch/attach request body

  bool IsAttach() const { return request == "attach"; }
};

}  // namespace microide::workspace
