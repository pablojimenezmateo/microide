#pragma once

#include <cstddef>
#include <string_view>

namespace microide::workspace::lsp_encoding {

enum class PositionEncoding { Utf8, Utf16, Utf32 };

PositionEncoding ParsePositionEncoding(std::string_view negotiated);

std::size_t LspCharacterToByteColumn(std::string_view line, std::size_t character,
                                     PositionEncoding encoding);

std::size_t ByteColumnToLspCharacter(std::string_view line, std::size_t byte_column,
                                     PositionEncoding encoding);

}
