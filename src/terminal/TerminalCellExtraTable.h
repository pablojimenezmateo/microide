#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "terminal/TerminalCell.h"
#include "util/TransparentStringHash.h"

namespace microide::terminal {

// A session's cell extended attributes -- an OSC 8 hyperlink's URI and an SGR 58
// underline colour -- addressed by the 16-bit id a TerminalCell carries in
// `extra` (0 = neither). Identical pairs share one id, so a link printed on
// every prompt, or a diagnostic colour on every error, costs one entry. Ids are
// recycled, not compacted: when the table is full the session marks the ids its
// buffers still reference and Retain() frees the rest -- a cell's id never
// changes under it.
class TerminalCellExtraTable {
 public:
  // VTE's and xterm.js's bound; a longer URI is not a link the user can read.
  static constexpr std::size_t kMaxUriBytes = 2083;
  static constexpr std::size_t kMaxEntries = 0xffff;

  // 0 when there is nothing to hold (no link and the default underline colour),
  // and when the table is full. An over-long URI is held as no link.
  std::uint16_t Intern(std::string_view uri, TerminalColor underline_color);
  // Empty for 0, a freed id, or an entry with no link.
  std::string_view Uri(std::uint16_t id) const;
  // The default (draw in the text colour) for 0, a freed id, or no colour.
  TerminalColor UnderlineColor(std::uint16_t id) const;
  // Frees every id whose `live[id]` is false (ids past the end of `live` too).
  void Retain(const std::vector<bool>& live);
  void Clear();
  bool full() const { return free_.empty() && entries_.size() >= kMaxEntries; }
  std::size_t size() const { return ids_.size(); }
  // Some live entry carries an underline colour. Lets a snapshot skip the walk
  // for the colours its cells name, which almost every terminal never prints.
  bool has_underline_colors() const { return colored_entries_ != 0; }

 private:
  struct Entry {
    std::string key;  // the interning key: 4 colour bytes, then the URI; empty when free
    TerminalColor underline_color;
  };
  static std::string KeyOf(std::string_view uri, TerminalColor underline_color);

  std::vector<Entry> entries_;  // entries_[id - 1]
  std::unordered_map<std::string, std::uint16_t, util::TransparentStringHash, std::equal_to<>>
      ids_;
  std::vector<std::uint16_t> free_;
  std::size_t colored_entries_ = 0;
};

// The underline colours `lines` name (underlined cells only), sorted by extra id:
// what a snapshot carries so paint can colour its underlines without the table.
void CollectUnderlineColors(const TerminalCellExtraTable& table, const std::vector<TerminalLine>& lines,
                            std::vector<TerminalUnderlineColor>& out);

}  // namespace microide::terminal
