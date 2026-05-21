#include "terminal/TerminalBase64.h"

#include <optional>
#include <string>

namespace microide::terminal {

namespace {

std::optional<int> Base64Value(unsigned char character) {
  if (character >= 'A' && character <= 'Z') {
    return static_cast<int>(character - 'A');
  }
  if (character >= 'a' && character <= 'z') {
    return static_cast<int>(character - 'a' + 26);
  }
  if (character >= '0' && character <= '9') {
    return static_cast<int>(character - '0' + 52);
  }
  if (character == '+') {
    return 62;
  }
  if (character == '/') {
    return 63;
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> DecodeBase64(std::string_view text) {
  std::string compact;
  compact.reserve(text.size());
  for (unsigned char character : text) {
    if (character == '\r' || character == '\n' || character == '\t' || character == ' ') {
      continue;
    }
    compact.push_back(static_cast<char>(character));
  }

  if (compact.empty()) {
    return std::string{};
  }
  if ((compact.size() % 4) != 0) {
    return std::nullopt;
  }

  std::string decoded;
  decoded.reserve((compact.size() / 4) * 3);
  for (std::size_t index = 0; index < compact.size(); index += 4) {
    const char a = compact[index];
    const char b = compact[index + 1];
    const char c = compact[index + 2];
    const char d = compact[index + 3];

    const auto a_value = Base64Value(static_cast<unsigned char>(a));
    const auto b_value = Base64Value(static_cast<unsigned char>(b));
    if (!a_value.has_value() || !b_value.has_value()) {
      return std::nullopt;
    }
    if ((c == '=' && d != '=') || (index + 4 != compact.size() && (c == '=' || d == '='))) {
      return std::nullopt;
    }

    const int sextet0 = *a_value;
    const int sextet1 = *b_value;
    const int sextet2 =
        c == '=' ? 0 : Base64Value(static_cast<unsigned char>(c)).value_or(-1);
    const int sextet3 =
        d == '=' ? 0 : Base64Value(static_cast<unsigned char>(d)).value_or(-1);
    if (sextet2 < 0 || sextet3 < 0) {
      return std::nullopt;
    }

    decoded.push_back(static_cast<char>((sextet0 << 2) | (sextet1 >> 4)));
    if (c != '=') {
      decoded.push_back(static_cast<char>(((sextet1 & 0x0f) << 4) | (sextet2 >> 2)));
    }
    if (d != '=') {
      decoded.push_back(static_cast<char>(((sextet2 & 0x03) << 6) | sextet3));
    }
  }

  return decoded;
}

bool ClipboardPayloadIsText(std::string_view text) {
  for (unsigned char character : text) {
    if (character == '\0') {
      return false;
    }
  }
  return true;
}

}  // namespace microide::terminal
