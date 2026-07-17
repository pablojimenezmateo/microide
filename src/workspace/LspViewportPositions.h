#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>

#include "editor/TextViewport.h"
#include "workspace/LspPositionEncoding.h"
#include "workspace/WorkspaceLspClient.h"

// Viewport-aware LSP position conversion — the single home for "resolve the
// affected line in `viewport`, then map its column through the server's negotiated
// position encoding". This logic used to be re-implemented in AssistService,
// LspService, and WorkspaceShellPlugins; a subtle skew between the copies is a
// data-corruption bug on non-ASCII lines, so it lives here once.
//
// All line access goes through TextBuffer::LineView (zero-copy) rather than
// Snapshot() (which materializes the whole document) — the previous AssistService
// copies snapshotted the entire buffer just to read one line.
namespace microide::workspace {

// TD-2026-07-17-089: outbound LSP fields are protocol `int`. A pathological/generated
// buffer can have a line or column past INT_MAX; a raw static_cast<int> would wrap to
// a negative/wrong value and ask the server about the wrong location. Saturating to
// INT_MAX instead sends a large-but-valid position that every server clamps to EOF —
// never a wrapped negative. (Documents this large are outside the editor's normal byte
// caps; clamping is the safe outbound policy without threading an optional through every
// request caller.)
inline int SaturateToLspInt(std::size_t value) {
  constexpr std::size_t kMax = static_cast<std::size_t>(std::numeric_limits<int>::max());
  return static_cast<int>(std::min(value, kMax));
}

// Encoding the server negotiated for `client`, ready to feed the converters below.
inline lsp_encoding::PositionEncoding LspEncodingForClient(const LspClient& client) {
  return lsp_encoding::ParsePositionEncoding(client.ServerPositionEncoding());
}

// Zero-copy view of `line` in `viewport` (empty when out of range).
inline std::string_view LspLineView(const editor::TextViewport& viewport, std::size_t line) {
  return line < viewport.lines().size() ? viewport.lines().LineView(line) : std::string_view{};
}

// Editor byte column (on `line`) -> outbound LSP position in the server's encoding.
// Requests must be phrased in the server's units or they resolve at the wrong
// token on non-ASCII lines.
inline LspClient::Position ByteColumnToLspPosition(const editor::TextViewport& viewport,
                                                   std::size_t line, std::size_t byte_column,
                                                   lsp_encoding::PositionEncoding encoding) {
  const std::string_view text = LspLineView(viewport, line);
  return LspClient::Position{
      SaturateToLspInt(line),
      SaturateToLspInt(lsp_encoding::ByteColumnToLspCharacter(text, byte_column, encoding))};
}

// Inbound LSP `character` (server encoding) on `line` -> editor byte column, given
// a resolved viewport.
inline std::size_t LspPositionToByteColumn(const editor::TextViewport& viewport, std::size_t line,
                                           int character,
                                           lsp_encoding::PositionEncoding encoding) {
  const std::string_view text = LspLineView(viewport, line);
  return lsp_encoding::LspCharacterToByteColumn(
      text, static_cast<std::size_t>(std::max(0, character)), encoding);
}

// Inbound conversion with the `viewport == nullptr` / utf-8 fast path (the
// file-not-open case): both short-circuit to the raw byte offset, since utf-8 is
// pass-through and a missing buffer has no line text to map against.
inline std::size_t LspInboundColumn(const editor::TextViewport* viewport, std::size_t line,
                                    int character, lsp_encoding::PositionEncoding encoding) {
  const std::size_t raw = static_cast<std::size_t>(std::max(0, character));
  if (viewport == nullptr || encoding == lsp_encoding::PositionEncoding::Utf8) {
    return raw;
  }
  return LspPositionToByteColumn(*viewport, line, character, encoding);
}

// Full LSP range (0-based, server encoding) -> editor byte-column SelectionRange,
// clamped to the document.
inline editor::SelectionRange LspRangeToEditorRange(const editor::TextViewport& viewport,
                                                    const LspClient::Range& range,
                                                    lsp_encoding::PositionEncoding encoding) {
  const auto to_position = [&](const LspClient::Position& p) {
    const std::size_t line = static_cast<std::size_t>(std::max(0, p.line));
    return editor::TextPosition{line, LspPositionToByteColumn(viewport, line, p.character, encoding)};
  };
  return editor::SelectionRange{.start = to_position(range.start), .end = to_position(range.end)};
}

// Same as above but for a possibly-null viewport (the file-not-open publish case):
// each column short-circuits through LspInboundColumn (nullptr / utf-8 => raw byte
// offset). Lets the diagnostics/overlay publish paths share one converter.
inline editor::SelectionRange LspRangeToEditorRange(const editor::TextViewport* viewport,
                                                    const LspClient::Range& range,
                                                    lsp_encoding::PositionEncoding encoding) {
  const auto to_position = [&](const LspClient::Position& p) {
    const std::size_t line = static_cast<std::size_t>(std::max(0, p.line));
    return editor::TextPosition{line, LspInboundColumn(viewport, line, p.character, encoding)};
  };
  return editor::SelectionRange{.start = to_position(range.start), .end = to_position(range.end)};
}

}  // namespace microide::workspace
