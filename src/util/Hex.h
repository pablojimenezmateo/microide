#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Shared hexadecimal and percent-decoding primitives. These were previously
// reimplemented across the JSON parser, file-URI decoder, terminal OSC handler,
// and theme/project colour parsers; route every caller through here so the
// digit tables and bounds logic live in exactly one place.
namespace microide::util {

// Value of a single hex digit, or -1 when `c` is not `[0-9a-fA-F]`.
constexpr int HexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return (c - 'a') + 10;
  if (c >= 'A' && c <= 'F') return (c - 'A') + 10;
  return -1;
}

// Combine two hex digits (high nibble, low nibble) into a byte. Returns nullopt
// if either character is not a hex digit.
constexpr std::optional<std::uint8_t> ParseHexByte(char hi, char lo) {
  const int high = HexDigitValue(hi);
  const int low = HexDigitValue(lo);
  if (high < 0 || low < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>((high << 4) | low);
}

// Decode a `#rrggbb` colour string into its three byte components. Requires the
// leading '#' and exactly six hex digits; returns nullopt otherwise.
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

// Append the two uppercase, zero-padded hex digits of `byte` to `out`. The
// inverse of ParseHexByte; used by percent-encoders so they need not reach for
// std::ostringstream + iomanip manipulators on hot paths.
inline void AppendHexByte(std::string& out, std::uint8_t byte) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  out.push_back(kDigits[byte >> 4]);
  out.push_back(kDigits[byte & 0x0F]);
}

// Percent-decode `text`, replacing `%XX` escapes with the decoded byte. A `%`
// without two following hex digits is left verbatim, matching prior callers.
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

// Strict percent-decode: every `%` MUST introduce exactly two hex digits, or the
// whole input is rejected (nullopt). Unlike PercentDecode this does not pass a
// malformed `%zz` / trailing `%` through verbatim, so callers parsing untrusted
// URIs (e.g. file:// from a language server) cannot be tricked into materializing
// a path containing a stray `%` that a stricter peer would have refused.
inline std::optional<std::string> PercentDecodeStrict(std::string_view text) {
  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      decoded.push_back(text[i]);
      continue;
    }
    if (i + 2 >= text.size()) {
      return std::nullopt;  // '%' with fewer than two following characters
    }
    const auto byte = ParseHexByte(text[i + 1], text[i + 2]);
    if (!byte) {
      return std::nullopt;  // '%' not followed by two hex digits
    }
    decoded.push_back(static_cast<char>(*byte));
    i += 2;
  }
  return decoded;
}

}  // namespace microide::util
