#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace microide::terminal {

std::optional<std::string> DecodeBase64(std::string_view text);
bool ClipboardPayloadIsText(std::string_view text);

}  // namespace microide::terminal
