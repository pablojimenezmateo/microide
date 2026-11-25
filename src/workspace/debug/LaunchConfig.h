#pragma once

#include <string>
#include <string_view>
#include <variant>

#include "util/JsonValue.h"

namespace microide::workspace {

struct LaunchConfig {
  std::string name;
  std::string type;
  std::string request = "launch";
  util::JsonValue arguments;

  bool IsAttach() const { return request == "attach"; }
};

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

}
