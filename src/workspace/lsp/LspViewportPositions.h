#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>

#include "editor/TextViewport.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/WorkspaceLspClient.h"

namespace microide::workspace {

inline int SaturateToLspInt(std::size_t value) {
  constexpr std::size_t kMax = static_cast<std::size_t>(std::numeric_limits<int>::max());
  return static_cast<int>(std::min(value, kMax));
}

inline lsp_encoding::PositionEncoding LspEncodingForClient(const LspClient& client) {
  return lsp_encoding::ParsePositionEncoding(client.ServerPositionEncoding());
}

inline std::string_view LspLineView(const editor::TextViewport& viewport, std::size_t line) {
  return line < viewport.lines().size() ? viewport.lines().LineView(line) : std::string_view{};
}

inline LspClient::Position ByteColumnToLspPosition(const editor::TextViewport& viewport,
                                                   std::size_t line, std::size_t byte_column,
                                                   lsp_encoding::PositionEncoding encoding) {
  const std::string_view text = LspLineView(viewport, line);
  return LspClient::Position{
      SaturateToLspInt(line),
      SaturateToLspInt(lsp_encoding::ByteColumnToLspCharacter(text, byte_column, encoding))};
}

inline std::size_t LspPositionToByteColumn(const editor::TextViewport& viewport, std::size_t line,
                                           int character,
                                           lsp_encoding::PositionEncoding encoding) {
  const std::string_view text = LspLineView(viewport, line);
  return lsp_encoding::LspCharacterToByteColumn(
      text, static_cast<std::size_t>(std::max(0, character)), encoding);
}

inline std::size_t LspInboundColumn(const editor::TextViewport* viewport, std::size_t line,
                                    int character, lsp_encoding::PositionEncoding encoding) {
  const std::size_t raw = static_cast<std::size_t>(std::max(0, character));
  if (viewport == nullptr || encoding == lsp_encoding::PositionEncoding::Utf8) {
    return raw;
  }
  return LspPositionToByteColumn(*viewport, line, character, encoding);
}

inline editor::SelectionRange LspRangeToEditorRange(const editor::TextViewport& viewport,
                                                    const LspClient::Range& range,
                                                    lsp_encoding::PositionEncoding encoding) {
  const auto to_position = [&](const LspClient::Position& p) {
    const std::size_t line = static_cast<std::size_t>(std::max(0, p.line));
    return editor::TextPosition{line, LspPositionToByteColumn(viewport, line, p.character, encoding)};
  };
  return editor::SelectionRange{.start = to_position(range.start), .end = to_position(range.end)};
}

inline editor::SelectionRange LspRangeToEditorRange(const editor::TextViewport* viewport,
                                                    const LspClient::Range& range,
                                                    lsp_encoding::PositionEncoding encoding) {
  const auto to_position = [&](const LspClient::Position& p) {
    const std::size_t line = static_cast<std::size_t>(std::max(0, p.line));
    return editor::TextPosition{line, LspInboundColumn(viewport, line, p.character, encoding)};
  };
  return editor::SelectionRange{.start = to_position(range.start), .end = to_position(range.end)};
}

}
