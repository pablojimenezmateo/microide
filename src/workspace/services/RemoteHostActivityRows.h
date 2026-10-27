#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"
#include "project/remote/RemoteProtocol.h"

namespace microide::workspace {

// The Host view's rows (remote-host-activity D4), built once per status the host
// sends and painted as they are: four depth-0 groups -- Server, Workspaces,
// Terminals, Processes -- each with its totals as `detail`, and a depth-1 row per
// item with its figures. A row's `id` says what it is: "server", "workspace:<root>",
// "terminal:<handle>", "process:<handle>", and "group:<name>" for the groups.
struct HostActivityInput {
  std::string host;
  // The connection's state in the client's words ("ready", "reconnecting", ...)
  // and the server's release from its hello: the Server row when the server
  // reports nothing (an older one, or none yet).
  std::string connection_state;
  std::string server_release;
  // The last status the host sent; nullopt before the first.
  const project::remote::ServerStatus* status = nullptr;
  // The host roots this window has open as projects: their rows say "this window".
  std::vector<std::string> own_roots;
  // Group ids ("group:terminals") the user collapsed.
  const std::set<std::string, std::less<>>* collapsed = nullptr;
};
std::vector<plugin::PluginHost::SidebarItem> BuildHostActivityRows(const HostActivityInput& input);

// What a Host row's id names.
struct HostRowId {
  enum class Kind { Group, Server, Workspace, Terminal, Process };
  Kind kind = Kind::Group;
  std::uint64_t handle = 0;  // Terminal, Process
  std::string_view name;     // Group: its name; Workspace: its root
};
std::optional<HostRowId> ParseHostRowId(std::string_view id);

}  // namespace microide::workspace
