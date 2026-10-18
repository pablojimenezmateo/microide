#pragma once

#include <string>
#include <string_view>
#include <variant>

#include "util/JsonValue.h"

namespace microide::workspace {

// A single debug launch/attach configuration. Native format only — there is no
// `.vscode/launch.json` import (banned by the persistence invariants); configs
// persist through PersistenceService + PersistedRecord* in a later phase.
//
// `type` selects the contributed debug adapter (mirrors an LSP `language_id`
// selecting a server). `request` is the DAP request verb ("launch"/"attach").
// `arguments` is forwarded as the launch/attach request body (with
// `${workspaceFolder}` expanded, see ExpandLaunchVariables), so it can carry
// adapter-specific keys (program, args, cwd, stopOnEntry, ...) without the host
// needing to model each one.
struct LaunchConfig {
  std::string name;             // human label shown in pickers / status
  std::string type;             // adapter type id, e.g. "debugpy", "lldb"
  std::string request = "launch";  // "launch" or "attach"
  util::JsonValue arguments;    // forwarded as the launch/attach request body

  bool IsAttach() const { return request == "attach"; }
};

// VS Code's `${workspaceFolder}` in launch arguments: every occurrence in every
// string value (nested arrays and objects included) becomes the project root. A
// plugin-contributed config is global and cannot know the root itself, so without
// this it could not name the program it launches.
inline void ExpandLaunchVariables(util::JsonValue& value, std::string_view workspace_folder) {
  constexpr std::string_view kWorkspaceFolder = "${workspaceFolder}";
  if (auto* text = std::get_if<std::string>(&value.v)) {
    for (std::size_t at = text->find(kWorkspaceFolder); at != std::string::npos;
         at = text->find(kWorkspaceFolder, at + workspace_folder.size())) {
      text->replace(at, kWorkspaceFolder.size(), workspace_folder);
    }
  } else if (auto* array = std::get_if<util::JsonArray>(&value.v)) {
    for (util::JsonValue& item : *array) {
      ExpandLaunchVariables(item, workspace_folder);
    }
  } else if (auto* object = std::get_if<util::JsonObject>(&value.v)) {
    for (util::JsonObjectEntry& entry : *object) {
      ExpandLaunchVariables(entry.value, workspace_folder);
    }
  }
}

}  // namespace microide::workspace
