#include "terminal/TerminalHostWire.h"

#include "util/ByteCodec.h"

namespace microide::terminal {
namespace {

using util::ByteReader;
using util::PutBytes;
using util::PutVarint;

constexpr std::uint8_t kHasForeground = 1u << 0;
constexpr std::uint8_t kHasBackground = 1u << 1;
// Not a style bit: the run's OSC 8 link id follows the colours.
constexpr std::uint8_t kHasLink = 1u << 2;

// Optional-field presence bits.
constexpr std::uint8_t kHasTitle = 1u << 0;
constexpr std::uint8_t kHasWorkingDirectory = 1u << 1;
constexpr std::uint8_t kHasClipboard = 1u << 2;
constexpr std::uint8_t kHasNotification = 1u << 3;
constexpr std::uint8_t kHasLinks = 1u << 4;

void PutRgba(std::string& out, const util::Rgba8& color) {
  out.push_back(static_cast<char>(color.r));
  out.push_back(static_cast<char>(color.g));
  out.push_back(static_cast<char>(color.b));
  out.push_back(static_cast<char>(color.a));
}

util::Rgba8 GetRgba(ByteReader& in) {
  const std::uint64_t value = in.Le(4);
  return util::Rgba8{static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                     static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
}

// A run is one style AND one link: the link is per cell, not in the style, but
// it changes exactly where a run would.
void PutRunStyle(std::string& out, const TerminalCell& cell) {
  const TerminalStyle& style = cell.style;
  PutVarint(out, style.attrs);
  out.push_back(static_cast<char>((style.foreground ? kHasForeground : 0) |
                                  (style.background ? kHasBackground : 0) |
                                  (cell.link != 0 ? kHasLink : 0)));
  if (style.foreground) {
    PutRgba(out, *style.foreground);
  }
  if (style.background) {
    PutRgba(out, *style.background);
  }
  if (cell.link != 0) {
    PutVarint(out, cell.link);
  }
}

void GetRunStyle(ByteReader& in, TerminalCell& cell) {
  TerminalStyle& style = cell.style;
  style.attrs = static_cast<std::uint16_t>(in.Varint());
  const std::uint64_t colors = in.Le(1);
  if ((colors & kHasForeground) != 0) {
    style.foreground = GetRgba(in);
  }
  if ((colors & kHasBackground) != 0) {
    style.background = GetRgba(in);
  }
  if ((colors & kHasLink) != 0) {
    const std::uint64_t link = in.Varint();
    if (link == 0 || link > 0xffffu) {
      in.Fail();
      return;
    }
    cell.link = static_cast<std::uint16_t>(link);
  }
}

bool SameRun(const TerminalCell& a, const TerminalCell& b) {
  return a.link == b.link && a.style.attrs == b.style.attrs &&
         a.style.foreground == b.style.foreground && a.style.background == b.style.background;
}

// One cell's glyph: a printable ASCII byte stands for itself (the overwhelmingly
// common cell costs one byte); anything else is a length byte below 0x20 and then
// that many bytes, so the two forms cannot be confused.
void PutGlyph(std::string& out, const TerminalCell& cell) {
  if (cell.length == 1 && static_cast<unsigned char>(cell.bytes[0]) >= 0x20 &&
      static_cast<unsigned char>(cell.bytes[0]) < 0x7f) {
    out.push_back(cell.bytes[0]);
    return;
  }
  out.push_back(static_cast<char>(cell.length));
  out.append(cell.bytes.data(), cell.length);
}

void GetGlyph(ByteReader& in, TerminalCell& cell) {
  const auto lead = static_cast<std::uint8_t>(in.Le(1));
  if (lead >= 0x20) {
    cell.bytes[0] = static_cast<char>(lead);
    cell.length = 1;
    return;
  }
  if (lead > cell.bytes.size()) {
    in.Fail();
    return;
  }
  for (std::uint8_t i = 0; i < lead; ++i) {
    cell.bytes[i] = static_cast<char>(in.Le(1));
  }
  cell.length = lead;
}

bool GetLine(ByteReader& in, TerminalLine& line) {
  const std::uint64_t header = in.Varint();
  const std::uint64_t cells = header >> 1;
  line.wrapped_from_previous = (header & 1u) != 0;
  if (in.failed() || cells > kMaxHostTerminalLineCells || cells > in.remaining()) {
    in.Fail();
    return false;
  }
  line.cells.resize(static_cast<std::size_t>(cells));
  std::size_t at = 0;
  while (at < line.cells.size() && !in.failed()) {
    const std::uint64_t run = in.Varint();
    if (run == 0 || run > line.cells.size() - at) {
      in.Fail();
      break;
    }
    TerminalCell run_cell;
    GetRunStyle(in, run_cell);
    for (std::uint64_t i = 0; i < run && !in.failed(); ++i, ++at) {
      TerminalCell& cell = line.cells[at];
      cell = run_cell;
      GetGlyph(in, cell);
    }
  }
  return !in.failed();
}

bool GetLines(ByteReader& in, std::vector<TerminalLine>& lines) {
  const std::uint64_t count = in.Varint();
  if (in.failed() || count > in.remaining()) {
    return false;
  }
  lines.resize(static_cast<std::size_t>(count));
  for (TerminalLine& line : lines) {
    if (!GetLine(in, line)) {
      return false;
    }
  }
  return true;
}

void PutLines(std::string& out, const std::vector<TerminalLine>& lines) {
  PutVarint(out, lines.size());
  for (const TerminalLine& line : lines) {
    EncodeTerminalLine(out, line);
  }
}

}  // namespace

// Cells are grouped into runs of one style: a line of output is usually one or two
// styles across a hundred cells.
void EncodeTerminalLine(std::string& out, const TerminalLine& line) {
  PutVarint(out, (static_cast<std::uint64_t>(line.cells.size()) << 1) |
                     (line.wrapped_from_previous ? 1u : 0u));
  std::size_t at = 0;
  while (at < line.cells.size()) {
    std::size_t end = at + 1;
    while (end < line.cells.size() && SameRun(line.cells[end], line.cells[at])) {
      ++end;
    }
    PutVarint(out, end - at);
    PutRunStyle(out, line.cells[at]);
    for (std::size_t i = at; i < end; ++i) {
      PutGlyph(out, line.cells[i]);
    }
    at = end;
  }
}

void EncodeTerminalHostFrame(std::string& out, const TerminalHostFrame& frame) {
  PutVarint(out, frame.rows);
  PutVarint(out, frame.columns);
  PutVarint(out, frame.flags);
  out.push_back(static_cast<char>(frame.cursor_shape));
  PutVarint(out, frame.echo_ack);
  PutVarint(out, frame.previous_top);
  PutVarint(out, frame.screen_top);
  PutVarint(out, frame.cursor_row);
  PutVarint(out, frame.cursor_column);

  PutVarint(out, frame.scrollback.size());
  for (const TerminalHostRun& run : frame.scrollback) {
    out.push_back(static_cast<char>(run.kind));
    PutVarint(out, run.count);
  }
  PutLines(out, frame.scrollback_lines);

  PutVarint(out, frame.screen_keep.size());
  std::uint8_t bits = 0;
  for (std::size_t i = 0; i < frame.screen_keep.size(); ++i) {
    bits |= static_cast<std::uint8_t>(frame.screen_keep[i] ? 1u << (i % 8) : 0u);
    if (i % 8 == 7 || i + 1 == frame.screen_keep.size()) {
      out.push_back(static_cast<char>(bits));
      bits = 0;
    }
  }
  PutLines(out, frame.screen_lines);

  out.push_back(static_cast<char>((frame.title ? kHasTitle : 0) |
                                  (frame.working_directory ? kHasWorkingDirectory : 0) |
                                  (frame.clipboard ? kHasClipboard : 0) |
                                  (frame.notification ? kHasNotification : 0) |
                                  (frame.links.empty() ? 0 : kHasLinks)));
  for (const auto* field : {&frame.title, &frame.working_directory, &frame.clipboard}) {
    if (*field) {
      PutBytes(out, **field);
    }
  }
  if (frame.notification) {
    PutBytes(out, frame.notification->title);
    PutBytes(out, frame.notification->body);
  }
  if (!frame.links.empty()) {
    PutVarint(out, frame.links.size());
    for (const TerminalHostLink& link : frame.links) {
      PutVarint(out, link.id);
      PutBytes(out, link.uri);
    }
  }
}

std::optional<TerminalHostFrame> DecodeTerminalHostFrame(std::string_view bytes) {
  ByteReader in(bytes);
  TerminalHostFrame frame;
  const std::uint64_t rows = in.Varint();
  const std::uint64_t columns = in.Varint();
  const std::uint64_t flags = in.Varint();
  const std::uint64_t shape = in.Le(1);
  if (in.failed() || rows == 0 || columns == 0 || rows > kMaxHostTerminalDimension ||
      columns > kMaxHostTerminalDimension || flags > 0xffffu ||
      shape > static_cast<std::uint8_t>(TerminalCursorShape::Bar)) {
    return std::nullopt;
  }
  frame.rows = static_cast<std::uint32_t>(rows);
  frame.columns = static_cast<std::uint32_t>(columns);
  frame.flags = static_cast<std::uint16_t>(flags);
  frame.cursor_shape = static_cast<TerminalCursorShape>(shape);
  frame.echo_ack = in.Varint();
  frame.previous_top = in.Varint();
  frame.screen_top = in.Varint();
  const std::uint64_t cursor_row = in.Varint();
  const std::uint64_t cursor_column = in.Varint();
  if (in.failed() || cursor_row >= rows || cursor_column >= columns) {
    return std::nullopt;
  }
  frame.cursor_row = static_cast<std::uint32_t>(cursor_row);
  frame.cursor_column = static_cast<std::uint32_t>(cursor_column);

  // A screen that grew upward (screen_top < previous_top) moves no line into
  // scrollback, so it carries no runs.
  const std::uint64_t moved =
      frame.screen_top >= frame.previous_top ? frame.screen_top - frame.previous_top : 0;
  const std::uint64_t runs = in.Varint();
  if (in.failed() || runs > in.remaining()) {
    return std::nullopt;
  }
  frame.scrollback.resize(static_cast<std::size_t>(runs));
  std::uint64_t covered = 0;
  std::uint64_t line_runs = 0;
  for (TerminalHostRun& run : frame.scrollback) {
    const std::uint64_t kind = in.Le(1);
    run.count = in.Varint();
    if (in.failed() || kind > static_cast<std::uint8_t>(TerminalHostRun::Kind::Gap) ||
        run.count == 0 || run.count > moved - covered) {
      return std::nullopt;
    }
    run.kind = static_cast<TerminalHostRun::Kind>(kind);
    covered += run.count;
    line_runs += run.kind == TerminalHostRun::Kind::Lines ? run.count : 0;
  }
  if (covered != moved ||
      !GetLines(in, frame.scrollback_lines) || frame.scrollback_lines.size() != line_runs) {
    return std::nullopt;
  }

  const std::uint64_t screen = in.Varint();
  if (in.failed() || screen > rows) {
    return std::nullopt;
  }
  frame.screen_keep.resize(static_cast<std::size_t>(screen));
  std::size_t sent_rows = 0;
  for (std::size_t i = 0; i < frame.screen_keep.size(); i += 8) {
    const std::uint64_t bits = in.Le(1);
    for (std::size_t bit = 0; bit < 8 && i + bit < frame.screen_keep.size(); ++bit) {
      frame.screen_keep[i + bit] = (bits & (1u << bit)) != 0;
      sent_rows += frame.screen_keep[i + bit] ? 0 : 1;
    }
  }
  if (in.failed() || !GetLines(in, frame.screen_lines) || frame.screen_lines.size() != sent_rows) {
    return std::nullopt;
  }

  const std::uint64_t present = in.Le(1);
  for (auto [bit, field] : {std::pair{kHasTitle, &frame.title},
                            std::pair{kHasWorkingDirectory, &frame.working_directory},
                            std::pair{kHasClipboard, &frame.clipboard}}) {
    if ((present & bit) != 0) {
      *field = std::string(in.Bytes(kMaxHostTerminalTextBytes));
    }
  }
  if ((present & kHasNotification) != 0) {
    TerminalHostNotification notification;
    notification.title = std::string(in.Bytes(kMaxHostTerminalTextBytes));
    notification.body = std::string(in.Bytes(kMaxHostTerminalTextBytes));
    frame.notification = std::move(notification);
  }
  if ((present & kHasLinks) != 0) {
    const std::uint64_t count = in.Varint();
    if (in.failed() || count == 0 || count > 0xffffu || count > in.remaining()) {
      return std::nullopt;
    }
    frame.links.resize(static_cast<std::size_t>(count));
    for (TerminalHostLink& link : frame.links) {
      const std::uint64_t id = in.Varint();
      if (in.failed() || id == 0 || id > 0xffffu) {
        return std::nullopt;
      }
      link.id = static_cast<std::uint16_t>(id);
      link.uri = std::string(in.Bytes(kMaxHostTerminalTextBytes));
    }
  }
  if (in.failed() || !in.at_end()) {
    return std::nullopt;
  }
  return frame;
}

void EncodeTerminalInputEvent(std::string& out, const TerminalInputEvent& event) {
  out.push_back(static_cast<char>(event.kind));
  PutVarint(out, event.seq);
  switch (event.kind) {
    case TerminalInputEvent::Kind::Key:
      out.push_back(static_cast<char>(event.key.key));
      PutVarint(out, event.key.codepoint);
      out.push_back(static_cast<char>((event.key.shift ? 1 : 0) | (event.key.alt ? 2 : 0) |
                                      (event.key.ctrl ? 4 : 0) | (event.key.super ? 8 : 0)));
      break;
    case TerminalInputEvent::Kind::Paste:
    case TerminalInputEvent::Kind::Bytes:
      PutBytes(out, event.text);
      break;
    case TerminalInputEvent::Kind::MouseButton:
    case TerminalInputEvent::Kind::MouseMotion:
      out.push_back(static_cast<char>(event.button));
      out.push_back(static_cast<char>(event.pressed ? 1 : 0));
      PutVarint(out, event.row);
      PutVarint(out, event.column);
      PutVarint(out, event.modifiers);
      break;
    case TerminalInputEvent::Kind::Focus:
      out.push_back(static_cast<char>(event.pressed ? 1 : 0));
      break;
  }
}

bool DecodeTerminalInputEvents(std::string_view bytes, std::vector<TerminalInputEvent>& out) {
  ByteReader in(bytes);
  while (!in.at_end()) {
    TerminalInputEvent event;
    const std::uint64_t kind = in.Le(1);
    event.seq = in.Varint();
    if (in.failed() || kind > TerminalInputEvent::kLastKind) {
      return false;
    }
    event.kind = static_cast<TerminalInputEvent::Kind>(kind);
    switch (event.kind) {
      case TerminalInputEvent::Kind::Key: {
        const std::uint64_t key = in.Le(1);
        const std::uint64_t codepoint = in.Varint();
        const std::uint64_t mods = in.Le(1);
        if (key > TerminalKeyPress::kLastKey || codepoint > 0x10ffffu) {
          return false;
        }
        event.key.key = static_cast<TerminalKeyPress::Key>(key);
        event.key.codepoint = static_cast<char32_t>(codepoint);
        event.key.shift = (mods & 1) != 0;
        event.key.alt = (mods & 2) != 0;
        event.key.ctrl = (mods & 4) != 0;
        event.key.super = (mods & 8) != 0;
        break;
      }
      case TerminalInputEvent::Kind::Paste:
      case TerminalInputEvent::Kind::Bytes:
        event.text = std::string(in.Bytes(kMaxHostTerminalTextBytes));
        break;
      case TerminalInputEvent::Kind::MouseButton:
      case TerminalInputEvent::Kind::MouseMotion: {
        const std::uint64_t button = in.Le(1);
        event.pressed = in.Le(1) != 0;
        const std::uint64_t row = in.Varint();
        const std::uint64_t column = in.Varint();
        const std::uint64_t modifiers = in.Varint();
        if (button > static_cast<std::uint8_t>(TerminalMouseButton::WheelDown) ||
            row > kMaxHostTerminalDimension || column > kMaxHostTerminalDimension ||
            modifiers > 0xffffu) {
          return false;
        }
        event.button = static_cast<TerminalMouseButton>(button);
        event.row = static_cast<std::uint32_t>(row);
        event.column = static_cast<std::uint32_t>(column);
        event.modifiers = static_cast<util::KeyModifiers>(modifiers);
        break;
      }
      case TerminalInputEvent::Kind::Focus:
        event.pressed = in.Le(1) != 0;
        break;
    }
    if (in.failed()) {
      return false;
    }
    out.push_back(std::move(event));
  }
  return true;
}

}  // namespace microide::terminal
