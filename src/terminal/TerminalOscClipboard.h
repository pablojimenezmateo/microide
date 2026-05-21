#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace microide::terminal {

std::string SanitizeOscTitle(std::string_view text);
std::optional<std::string> DecodeOsc52ClipboardPayload(std::string_view osc_body);

}  // namespace microide::terminal
