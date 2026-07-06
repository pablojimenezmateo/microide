#pragma once

#include <cstddef>
#include <string_view>

// LSP position-encoding conversion. LSP `character` offsets are counted in code
// units of the server's negotiated `positionEncoding` (utf-16 by spec default,
// but this client advertises utf-8 first so clangd/rust-analyzer use utf-8). The
// editor stores columns as UTF-8 byte offsets, so every LSP position crossing a
// non-ASCII codepoint needs conversion. These helpers are the single seam for
// that; ASCII lines and the utf-8 encoding are pass-through (a byte == a unit).
namespace microide::workspace::lsp_encoding {

enum class PositionEncoding { Utf8, Utf16, Utf32 };

// Map a negotiated `positionEncoding` string ("utf-8"/"utf-16"/"utf-32") to the
// enum. Anything unrecognized (including the empty/unreported case) resolves to
// utf-16 — the LSP spec default and this client's initial value.
PositionEncoding ParsePositionEncoding(std::string_view negotiated);

// Convert an LSP `character` offset (in `encoding`'s code units, into `line`) to a
// UTF-8 byte column, clamped to `line.size()`. An offset landing inside a
// multi-unit codepoint snaps to that codepoint's start.
std::size_t LspCharacterToByteColumn(std::string_view line, std::size_t character,
                                     PositionEncoding encoding);

// Inverse: convert a UTF-8 byte column (into `line`) to an LSP `character` offset
// in `encoding`'s code units. `byte_column` is clamped to `line.size()`.
std::size_t ByteColumnToLspCharacter(std::string_view line, std::size_t byte_column,
                                     PositionEncoding encoding);

}  // namespace microide::workspace::lsp_encoding
