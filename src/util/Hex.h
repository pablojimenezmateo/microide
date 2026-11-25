#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace microide::util {

constexpr int HexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return (c - 'a') + 10;
  if (c >= 'A' && c <= 'F') return (c - 'A') + 10;
  return -1;
}

constexpr std::optional<std::uint8_t> ParseHexByte(char hi, char lo) {
  const int high = HexDigitValue(hi);
  const int low = HexDigitValue(lo);
  if (high < 0 || low < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>((high << 4) | low);
}

constexpr std::optional<std::array<std::uint8_t, 3>> DecodeHexColor(std::string_view text) {
  if (text.size() != 7 || text.front() != '#') {
    return std::nullopt;
  }
  const auto red = ParseHexByte(text[1], text[2]);
  const auto green = ParseHexByte(text[3], text[4]);
  const auto blue = ParseHexByte(text[5], text[6]);
  if (!red || !green || !blue) {
    return std::nullopt;
  }
  return std::array<std::uint8_t, 3>{*red, *green, *blue};
}

inline void AppendHexByte(std::string& out, std::uint8_t byte) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  out.push_back(kDigits[byte >> 4]);
  out.push_back(kDigits[byte & 0x0F]);
}

inline std::string PercentDecode(std::string_view text) {
  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      if (const auto byte = ParseHexByte(text[i + 1], text[i + 2])) {
        decoded.push_back(static_cast<char>(*byte));
        i += 2;
        continue;
      }
    }
    decoded.push_back(text[i]);
  }
  return decoded;
}

inline std::optional<std::string> PercentDecodeStrict(std::string_view text) {
  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      decoded.push_back(text[i]);
      continue;
    }
    if (i + 2 >= text.size()) {
      return std::nullopt;
    }
    const auto byte = ParseHexByte(text[i + 1], text[i + 2]);
    if (!byte) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>(*byte));
    i += 2;
  }
  return decoded;
}

}
