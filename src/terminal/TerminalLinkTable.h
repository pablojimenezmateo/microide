#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/TransparentStringHash.h"

namespace microide::terminal {

// The URIs of a session's OSC 8 hyperlinks, addressed by the 16-bit id a
// TerminalCell carries (0 = no link). Identical URIs share one id, so a link
// printed on every prompt costs one entry. Ids are recycled, not compacted: when
// the table is full the session marks the ids its buffers still reference and
// Retain() frees the rest — a cell's id never changes under it.
class TerminalLinkTable {
 public:
  // VTE's and xterm.js's bound; a longer URI is not a link the user can read.
  static constexpr std::size_t kMaxUriBytes = 2083;
  static constexpr std::size_t kMaxLinks = 0xffff;

  // 0 when the URI is empty, over-long, or the table is full.
  std::uint16_t Intern(std::string_view uri);
  // Empty for 0 or a freed id.
  std::string_view Uri(std::uint16_t id) const;
  // Frees every id whose `live[id]` is false (ids past the end of `live` too).
  void Retain(const std::vector<bool>& live);
  void Clear();
  bool full() const { return free_.empty() && uris_.size() >= kMaxLinks; }
  std::size_t size() const { return ids_.size(); }

 private:
  std::vector<std::string> uris_;  // uris_[id - 1]; empty when free
  std::unordered_map<std::string, std::uint16_t, util::TransparentStringHash, std::equal_to<>>
      ids_;
  std::vector<std::uint16_t> free_;
};

}  // namespace microide::terminal
