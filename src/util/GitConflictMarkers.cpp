#include "util/GitConflictMarkers.h"

namespace microide::util {

bool ContainsCompleteConflictMarkers(std::string_view text) {
  return text.find("<<<<<<<") != std::string_view::npos &&
         text.find("=======") != std::string_view::npos &&
         text.find(">>>>>>>") != std::string_view::npos;
}

bool ContainsAnyConflictMarker(std::string_view text) {
  return text.find("<<<<<<<") != std::string_view::npos ||
         text.find("=======") != std::string_view::npos ||
         text.find(">>>>>>>") != std::string_view::npos;
}

std::optional<std::size_t> FirstConflictMarkerLine(std::span<const std::string> lines) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].starts_with("<<<<<<<")) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace microide::util
