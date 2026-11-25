#pragma once

#include <cstddef>
#include <string>
#include "util/ErrorOut.h"

namespace microide::plugin {

inline constexpr std::size_t kMaxPluginContributionsPerKind = 16384;

inline constexpr std::size_t kMaxPluginStatusItems = 1024;

template <typename Container>
bool ContributionLimitReachedAt(const Container* container,
                                std::size_t cap,
                                std::string* error_message) {
  if (container != nullptr && container->size() >= cap) {
    util::SetError(error_message, "plugin contribution limit reached");
    return true;
  }
  return false;
}

template <typename Container>
bool ContributionLimitReached(const Container* container, std::string* error_message) {
  return ContributionLimitReachedAt(container, kMaxPluginContributionsPerKind, error_message);
}

}
