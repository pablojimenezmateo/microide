#pragma once

#include <array>
#include <string_view>

namespace microide::workspace {

inline constexpr std::array<std::string_view, 3> kSidebarToolNames = {
    "git",
    "search",
    "tree",
};

inline constexpr std::array<std::string_view, 3> kFocusTargetNames = {
    "editor",
    "panel",
    "sidebar",
};

inline constexpr std::array<std::string_view, 2> kToggleValues = {
    "off",
    "on",
};

inline constexpr std::array<std::string_view, 3> kUiScaleCommands = {
    "down",
    "reset",
    "up",
};

}  // namespace microide::workspace
