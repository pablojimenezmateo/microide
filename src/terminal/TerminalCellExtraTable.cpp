#include "terminal/TerminalCellExtraTable.h"

#include <algorithm>

namespace microide::terminal {

namespace {
constexpr std::size_t kColorBytes = 4;
}  // namespace

// The colour leads at a fixed width, so no URI can be read as another pair.
std::string TerminalCellExtraTable::KeyOf(std::string_view uri, TerminalColor underline_color) {
  const util::Rgba8 color = underline_color.value_or(util::Rgba8{0, 0, 0, 0});
  std::string key;
  key.reserve(kColorBytes + uri.size());
  key.push_back(static_cast<char>(color.r));
  key.push_back(static_cast<char>(color.g));
  key.push_back(static_cast<char>(color.b));
  key.push_back(static_cast<char>(underline_color.has_value() ? 1 : 0));
  key.append(uri);
  return key;
}

std::uint16_t TerminalCellExtraTable::Intern(std::string_view uri, TerminalColor underline_color) {
  if (uri.size() > kMaxUriBytes) {
    uri = {};
  }
  if (uri.empty() && !underline_color.has_value()) {
    return 0;
  }
  std::string key = KeyOf(uri, underline_color);
  if (const auto found = ids_.find(key); found != ids_.end()) {
    return found->second;
  }
  std::uint16_t id = 0;
  if (!free_.empty()) {
    id = free_.back();
    free_.pop_back();
  } else if (entries_.size() < kMaxEntries) {
    entries_.emplace_back();
    id = static_cast<std::uint16_t>(entries_.size());
  } else {
    return 0;
  }
  Entry& entry = entries_[id - 1];
  entry.key = std::move(key);
  entry.underline_color = underline_color;
  if (underline_color.has_value()) {
    ++colored_entries_;
  }
  ids_.emplace(entry.key, id);
  return id;
}

std::string_view TerminalCellExtraTable::Uri(std::uint16_t id) const {
  if (id == 0 || id > entries_.size() || entries_[id - 1].key.empty()) {
    return {};
  }
  return std::string_view(entries_[id - 1].key).substr(kColorBytes);
}

TerminalColor TerminalCellExtraTable::UnderlineColor(std::uint16_t id) const {
  if (id == 0 || id > entries_.size()) {
    return {};
  }
  return entries_[id - 1].underline_color;
}

void TerminalCellExtraTable::Retain(const std::vector<bool>& live) {
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    const std::size_t id = index + 1;
    Entry& entry = entries_[index];
    if (entry.key.empty() || (id < live.size() && live[id])) {
      continue;
    }
    ids_.erase(entry.key);
    if (entry.underline_color.has_value()) {
      --colored_entries_;
    }
    entry.key.clear();
    entry.key.shrink_to_fit();  // a long URI's buffer goes back, not just its length
    entry.underline_color.reset();
    free_.push_back(static_cast<std::uint16_t>(id));
  }
}

void TerminalCellExtraTable::Clear() {
  entries_.clear();
  ids_.clear();
  free_.clear();
  colored_entries_ = 0;
}

void CollectUnderlineColors(const TerminalCellExtraTable& table, const std::vector<TerminalLine>& lines,
                            std::vector<TerminalUnderlineColor>& out) {
  out.clear();
  if (!table.has_underline_colors()) {
    return;  // nearly every terminal: no walk at all
  }
  for (const TerminalLine& line : lines) {
    for (const TerminalCell& cell : line.cells) {
      if (cell.extra == 0 || (!cell.style.underline() && !cell.style.double_underline())) {
        continue;
      }
      const auto at = std::lower_bound(
          out.begin(), out.end(), cell.extra,
          [](const TerminalUnderlineColor& entry, std::uint16_t id) { return entry.extra < id; });
      if (at != out.end() && at->extra == cell.extra) {
        continue;
      }
      if (const TerminalColor color = table.UnderlineColor(cell.extra); color.has_value()) {
        out.insert(at, TerminalUnderlineColor{.extra = cell.extra, .color = color});
      }
    }
  }
}

}  // namespace microide::terminal
