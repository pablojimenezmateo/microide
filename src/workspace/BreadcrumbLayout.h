#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace microide::workspace {

// The horizontal span a pane's breadcrumb text is drawn in: its band inset 12px
// at each end, then pushed clear of the status items contributed to the band --
// a left-aligned item moves the start right, a right-aligned one the end left.
// The paint and the click both resolve the text from this one rule, so a click
// lands on the segment drawn under it.
template <typename Items, typename IsLeft>
std::pair<float, float> BreadcrumbTextSpan(const SDL_FRect& band, const Items& status_items, IsLeft&& is_left) {
  float start = band.x + 12.0f;
  float end = band.x + band.w - 12.0f;
  for (const auto& item : status_items) {
    if (item.rect.x + item.rect.w <= band.x || item.rect.x >= band.x + band.w) {
      continue;  // window chrome laid out across the bar; not over this pane
    }
    if (is_left(item)) {
      start = std::max(start, item.rect.x + item.rect.w + 8.0f);
    } else {
      end = std::min(end, item.rect.x - 8.0f);
    }
  }
  return {start, std::max(start, end)};
}

// The folder a click at `x` names on a breadcrumb `path_label` ("src/ui/view.cpp",
// drawn from `text_x`, `width_of` measuring a prefix): the folder segment under
// it ("src/ui" for a click on "ui"), or the file's own folder for the last
// segment ("src/ui" again; "" when the file is at the top) -- VS Code's
// breadcrumb picker shows that folder's entries. nullopt off the text.
template <typename WidthOf>
std::optional<std::string> BreadcrumbFolderAt(std::string_view path_label, float text_x, float x, WidthOf&& width_of) {
  if (path_label.empty() || x < text_x || x > text_x + width_of(path_label)) {
    return std::nullopt;
  }
  std::size_t segment_end = 0;
  for (;;) {
    const std::size_t slash = path_label.find('/', segment_end);
    if (slash == std::string_view::npos) {
      // The last segment, the file: its folder.
      const std::size_t last = path_label.rfind('/');
      return std::string(last == std::string_view::npos ? std::string_view{} : path_label.substr(0, last));
    }
    if (x <= text_x + width_of(path_label.substr(0, slash))) {
      return std::string(path_label.substr(0, slash));
    }
    segment_end = slash + 1;
  }
}

}  // namespace microide::workspace
