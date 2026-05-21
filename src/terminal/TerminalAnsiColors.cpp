#include "terminal/TerminalAnsiColors.h"

#include <algorithm>

namespace microide::terminal {

SDL_Color MakeTerminalRgbColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  return SDL_Color{r, g, b, a};
}

namespace {

SDL_Color MakeColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 0xff) {
  return MakeTerminalRgbColor(r, g, b, a);
}

}  // namespace

SDL_Color BasicAnsiColor(int index, bool bright) {
  static const std::array<SDL_Color, 8> kNormal = {
      MakeColor(0x1f, 0x24, 0x2c), MakeColor(0xc3, 0x4b, 0x59),
      MakeColor(0x8a, 0xb1, 0x66), MakeColor(0xd8, 0xb2, 0x5d),
      MakeColor(0x5a, 0x8c, 0xe6), MakeColor(0xb0, 0x72, 0xd1),
      MakeColor(0x56, 0xa8, 0xc9), MakeColor(0xb8, 0xc0, 0xcc),
  };
  static const std::array<SDL_Color, 8> kBright = {
      MakeColor(0x4a, 0x51, 0x5c), MakeColor(0xf0, 0x71, 0x78),
      MakeColor(0xa4, 0xc7, 0x6d), MakeColor(0xe7, 0xc5, 0x47),
      MakeColor(0x72, 0xa7, 0xff), MakeColor(0xcb, 0x8f, 0xf8),
      MakeColor(0x74, 0xc7, 0xec), MakeColor(0xf5, 0xf7, 0xfa),
  };
  const int clamped_index = std::clamp(index, 0, 7);
  return bright ? kBright[clamped_index] : kNormal[clamped_index];
}

SDL_Color Ansi256Color(int index) {
  if (index < 0) {
    return BasicAnsiColor(0, false);
  }
  if (index < 8) {
    return BasicAnsiColor(index, false);
  }
  if (index < 16) {
    return BasicAnsiColor(index - 8, true);
  }
  if (index < 232) {
    const int value = index - 16;
    const int red = value / 36;
    const int green = (value / 6) % 6;
    const int blue = value % 6;
    static constexpr std::array<Uint8, 6> kCube = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
    return MakeColor(kCube[red], kCube[green], kCube[blue]);
  }
  const Uint8 gray = static_cast<Uint8>(8 + (index - 232) * 10);
  return MakeColor(gray, gray, gray);
}

std::string DefaultShellPath() {
  if (const char* shell = std::getenv("SHELL"); shell != nullptr && shell[0] != '\0') {
    return shell;
  }
  return "/bin/sh";
}

std::string ShellProgramName(const std::string& shell_path) {
  const std::size_t slash = shell_path.find_last_of("/\\");
  return slash == std::string::npos ? shell_path : shell_path.substr(slash + 1);
}

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
