#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "terminal/TerminalCell.h"
#include "util/TransparentStringHash.h"

namespace microide::terminal {

class TerminalCellExtraTable {
 public:
  static constexpr std::size_t kMaxUriBytes = 2083;
  static constexpr std::size_t kMaxEntries = 0xffff;

  std::uint16_t Intern(std::string_view uri, TerminalColor underline_color);
  std::string_view Uri(std::uint16_t id) const;
  TerminalColor UnderlineColor(std::uint16_t id) const;
  void Retain(const std::vector<bool>& live);
  void Clear();
  bool full() const { return free_.empty() && entries_.size() >= kMaxEntries; }
  std::size_t size() const { return ids_.size(); }
  bool has_underline_colors() const { return colored_entries_ != 0; }

 private:
  struct Entry {
    std::string key;
    TerminalColor underline_color;
  };
  static std::string KeyOf(std::string_view uri, TerminalColor underline_color);

  std::vector<Entry> entries_;
  std::unordered_map<std::string, std::uint16_t, util::TransparentStringHash, std::equal_to<>>
      ids_;
  std::vector<std::uint16_t> free_;
  std::size_t colored_entries_ = 0;
};

void CollectUnderlineColors(const TerminalCellExtraTable& table, const std::vector<TerminalLine>& lines,
                            std::vector<TerminalUnderlineColor>& out);

}
