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

struct HostActivityInput {
  std::string host;
  std::string connection_state;
  std::string server_release;
  const project::remote::ServerStatus* status = nullptr;
  std::vector<std::string> own_roots;
  const std::set<std::string, std::less<>>* collapsed = nullptr;
  std::string id_prefix;
  int depth = 0;
};
std::string HostRowPrefix(std::string_view host);
std::string HostGroupRowId(std::string_view host);
std::vector<plugin::PluginHost::SidebarItem> BuildHostActivityRows(const HostActivityInput& input);

struct HostRowId {
  enum class Kind { Group, Server, Workspace, Terminal, Process, Host };
  Kind kind = Kind::Group;
  std::uint64_t handle = 0;
  std::string_view name;
  std::string_view host;
};
std::optional<HostRowId> ParseHostRowId(std::string_view id);

}
