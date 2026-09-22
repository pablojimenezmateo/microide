#include "terminal/TerminalAnsiColors.h"

namespace microide::terminal {

TerminalCell MakeAsciiTerminalCell(char character, const TerminalStyle& style) {
  TerminalCell cell;
  cell.SetAscii(character);
  cell.style = style;
  return cell;
}

TerminalCell MakeUtf8TerminalCell(std::string_view glyph, const TerminalStyle& style) {
  if (glyph.size() == 1 && static_cast<unsigned char>(glyph.front()) < 0x80) {
    return MakeAsciiTerminalCell(glyph.front(), style);
  }
  TerminalCell cell;
  cell.SetUtf8(glyph);
  cell.style = style;
  return cell;
}

}  // namespace microide::terminal
