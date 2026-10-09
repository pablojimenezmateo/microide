#include "terminal/TerminalLinkTable.h"

namespace microide::terminal {

std::uint16_t TerminalLinkTable::Intern(std::string_view uri) {
  if (uri.empty() || uri.size() > kMaxUriBytes) {
    return 0;
  }
  if (const auto found = ids_.find(uri); found != ids_.end()) {
    return found->second;
  }
  std::uint16_t id = 0;
  if (!free_.empty()) {
    id = free_.back();
    free_.pop_back();
    uris_[id - 1] = std::string(uri);
  } else if (uris_.size() < kMaxLinks) {
    uris_.emplace_back(uri);
    id = static_cast<std::uint16_t>(uris_.size());
  } else {
    return 0;
  }
  ids_.emplace(uris_[id - 1], id);
  return id;
}

std::string_view TerminalLinkTable::Uri(std::uint16_t id) const {
  if (id == 0 || id > uris_.size()) {
    return {};
  }
  return uris_[id - 1];
}

void TerminalLinkTable::Retain(const std::vector<bool>& live) {
  for (std::size_t index = 0; index < uris_.size(); ++index) {
    const std::size_t id = index + 1;
    if (uris_[index].empty() || (id < live.size() && live[id])) {
      continue;
    }
    ids_.erase(uris_[index]);
    uris_[index].clear();
    uris_[index].shrink_to_fit();
    free_.push_back(static_cast<std::uint16_t>(id));
  }
}

void TerminalLinkTable::Clear() {
  uris_.clear();
  ids_.clear();
  free_.clear();
}

}  // namespace microide::terminal
