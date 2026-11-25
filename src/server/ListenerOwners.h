#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <unordered_map>

#include "server/ListeningPorts.h"
#include "server/ProcessActivity.h"

namespace microide::server {

struct OwnerRoot {
  int pid = -1;
  std::string root;
};

std::map<std::uint16_t, std::string> AttributeListeners(std::span<const Listener> listeners,
                                                        std::span<const OwnerRoot> roots);

int OwningRootIndex(int pid, const std::unordered_map<int, int>& parents,
                    const std::unordered_map<int, std::size_t>& root_index);

}
