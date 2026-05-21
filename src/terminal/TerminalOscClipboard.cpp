#include "terminal/TerminalOscClipboard.h"

#include "terminal/TerminalBase64.h"

#include <algorithm>
#include <string>

namespace microide::terminal {

std::string SanitizeOscTitle(std::string_view text) {
  std::string sanitized;
  sanitized.reserve(std::min<std::size_t>(text.size(), 256));
  for (unsigned char character : text) {
    if ((character < 0x20 && character != ' ') || character == 0x7f) {
      continue;
    }
    sanitized.push_back(static_cast<char>(character));
    if (sanitized.size() >= 256) {
      break;
    }
  }

  const std::size_t first = sanitized.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = sanitized.find_last_not_of(' ');
  return sanitized.substr(first, last - first + 1);
}

std::optional<std::string> DecodeOsc52ClipboardPayload(std::string_view osc_body) {
  if (osc_body.empty() || osc_body.front() != ']') {
    return std::nullopt;
  }

  const std::string_view body = osc_body.substr(1);
  const std::size_t separator = body.find(';');
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }

  const std::string_view command = body.substr(0, separator);
  if (command != "52") {
    return std::nullopt;
  }

  const std::string_view remainder = body.substr(separator + 1);
  const std::size_t second_separator = remainder.find(';');
  if (second_separator == std::string_view::npos) {
    return std::nullopt;
  }

  const std::string_view selection = remainder.substr(0, second_separator);
  const std::string_view encoded = remainder.substr(second_separator + 1);
  if ((!selection.empty() && selection != "c") || encoded == "?") {
    return std::nullopt;
  }

  const auto decoded = DecodeBase64(encoded);
  if (!decoded.has_value() || !ClipboardPayloadIsText(*decoded)) {
    return std::nullopt;
  }
  return *decoded;
}

}  // namespace microide::terminal
