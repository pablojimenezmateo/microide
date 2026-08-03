#include "workspace/lsp/LspPositionEncoding.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microide::workspace::lsp_encoding {

namespace {

// Advance one UTF-8 codepoint at `byte` in `line`. Returns its byte length (>=1,
// lenient: a malformed/truncated lead byte counts as a single byte) and the
// codepoint value via `out_cp`.
std::size_t StepCodepoint(std::string_view line, std::size_t byte, char32_t& out_cp) {
  std::size_t len = util::Utf8SequenceLength(line, byte);
  if (len == 0 || byte + len > line.size()) {
    len = 1;
  }
  out_cp = util::DecodeUtf8Codepoint(line.substr(byte, len));
  return len;
}

// UTF-16 code units for a scalar value: 2 for astral (>= U+10000), else 1.
std::size_t Utf16Units(char32_t cp) { return cp >= 0x10000 ? 2 : 1; }

}  // namespace

PositionEncoding ParsePositionEncoding(std::string_view negotiated) {
  if (negotiated == "utf-8") {
    return PositionEncoding::Utf8;
  }
  if (negotiated == "utf-32") {
    return PositionEncoding::Utf32;
  }
  return PositionEncoding::Utf16;
}

std::size_t LspCharacterToByteColumn(std::string_view line, std::size_t character,
                                     PositionEncoding encoding) {
  if (encoding == PositionEncoding::Utf8) {
    // The server already counts bytes; a byte offset is a valid boundary in
    // well-formed input, so just clamp to the line length.
    return std::min(character, line.size());
  }
  if (encoding == PositionEncoding::Utf32) {
    return util::Utf8ByteOffsetForCodepointCount(line, character);
  }
  // utf-16: walk codepoints, accumulating code units until reaching `character`.
  std::size_t byte = 0;
  std::size_t units = 0;
  while (byte < line.size() && units < character) {
    char32_t cp = 0;
    const std::size_t len = StepCodepoint(line, byte, cp);
    const std::size_t cp_units = Utf16Units(cp);
    if (units + cp_units > character) {
      break;  // `character` points into a surrogate pair -> snap to this cp's start.
    }
    units += cp_units;
    byte += len;
  }
  return byte;
}

std::size_t ByteColumnToLspCharacter(std::string_view line, std::size_t byte_column,
                                     PositionEncoding encoding) {
  const std::size_t clamped = std::min(byte_column, line.size());
  if (encoding == PositionEncoding::Utf8) {
    return clamped;
  }
  if (encoding == PositionEncoding::Utf32) {
    return util::Utf8CodepointCount(line.substr(0, clamped));
  }
  // utf-16: sum code units of every codepoint up to the byte column.
  std::size_t byte = 0;
  std::size_t units = 0;
  while (byte < clamped) {
    char32_t cp = 0;
    const std::size_t len = StepCodepoint(line, byte, cp);
    if (byte + len > clamped) {
      break;  // byte_column landed mid-codepoint; count it as reached.
    }
    units += Utf16Units(cp);
    byte += len;
  }
  return units;
}

}  // namespace microide::workspace::lsp_encoding
