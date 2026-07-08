#include "terminal/TerminalAnsiColors.h"

#include "render/AnsiPalette.h"

namespace microide::terminal {

SDL_Color MakeTerminalRgbColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  return SDL_Color{r, g, b, a};
}

// The ANSI / xterm-256 palette is shared with the theme parser; see
// render/AnsiPalette.h.
SDL_Color BasicAnsiColor(int index, bool bright) {
  return render::BasicAnsiColor(index, bright);
}

SDL_Color Ansi256Color(int index) { return render::Ansi256Color(index); }

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
