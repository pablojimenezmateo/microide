#pragma once

// Line caps of a two-block hover card -- a plugin's hover, signature help --
// shared by the card's paint (WorkspaceShellHoverPopup.cpp) and the frame prep
// that lays the signature-help card out and accounts its damage
// (WorkspaceShellRenderFrame.cpp). The caps count the text's own lines
// (HoverTextWrap keeps them), not one run-on paragraph wrapped.

#include <cstddef>

namespace microide::workspace {

inline constexpr std::size_t kEditorHoverPopupMaxPluginTitleLines = 2;
inline constexpr std::size_t kEditorHoverPopupMaxPluginContentLines = 12;

}  // namespace microide::workspace
