#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/TerminalCell.h"
#include "terminal/TerminalInput.h"

namespace microide::terminal {

// The host terminal's wire model (dev-docs/design/remote-projects.md § 6.5): the
// pty and the terminal model run on the host; the client receives SCREEN STATE,
// never the byte stream, and sends SEMANTIC input the host encodes.
//
// Coordinates. The host's primary buffer is scrollback followed by the visible
// grid, and every line in it has an absolute index (the session's trim total plus
// its deque row) that never changes while the line exists. `screen_top` is the
// absolute index of the first visible line. Between two frames the screen moves
// down by `screen_top - previous_top` lines; the lines that moved out of view
// are the client's new scrollback, and the update says, per line, whether the
// client already holds it (Promote: its old screen row is still exact), needs
// it (Lines), or cannot have it (Gap: trimmed on the host, or withheld because
// the client's credit window was full — drawn as one counted rule).
//
// Screen rows likewise either Keep the client's old screen row at the same
// absolute index, or carry the line. A loud build's frame is therefore the lines
// that scrolled past (credit-windowed) plus the few rows that changed — not the
// whole screen per frame and never the raw output.
//
// The alternate screen has no scrollback: its frames carry no runs, and a frame
// that switches screens (or resizes) keeps nothing.

struct TerminalHostRun {
  enum class Kind : std::uint8_t { Promote, Lines, Gap };
  Kind kind = Kind::Lines;
  std::uint64_t count = 0;
};

// A cell extra a frame's lines reference (TerminalCell::extra): the host
// session's id, its OSC 8 URI and its SGR 58 underline colour. The client
// interns the pair in its own table and rewrites the ids, so host and client ids
// never need to agree.
struct TerminalHostCellExtra {
  std::uint16_t id = 0;
  std::string uri;
  TerminalColor underline_color;
};

// An OSC 9 / OSC 777 desktop notification the host program asked for.
struct TerminalHostNotification {
  std::string title;
  std::string body;
};

struct TerminalHostFrame {
  // Flag bits.
  static constexpr std::uint16_t kAlternateScreen = 1u << 0;
  static constexpr std::uint16_t kCursorVisible = 1u << 1;
  static constexpr std::uint16_t kCursorBlinking = 1u << 2;
  static constexpr std::uint16_t kMouseNormal = 1u << 3;
  static constexpr std::uint16_t kMouseDrag = 1u << 4;
  static constexpr std::uint16_t kMouseAny = 1u << 5;
  static constexpr std::uint16_t kFocusEvents = 1u << 6;
  static constexpr std::uint16_t kRunning = 1u << 7;
  static constexpr std::uint16_t kBell = 1u << 8;
  // The client must hold nothing of the previous screen (first frame, attach,
  // resize, screen switch): every Keep and Promote is invalid in this frame.
  static constexpr std::uint16_t kReset = 1u << 9;
  // DECSCNM: the screen is drawn in reverse video.
  static constexpr std::uint16_t kReverseVideo = 1u << 10;
  // Bits 11-15: the program's OSC 22 pointer (TerminalPointerShape, 0 = None). A
  // client older than it ignores them, so it needs no protocol version.
  static constexpr unsigned kPointerShapeShift = 11;
  static constexpr std::uint16_t kPointerShapeMask = 0x1fu << kPointerShapeShift;
  TerminalPointerShape pointer_shape() const {
    const auto value = static_cast<std::uint8_t>((flags & kPointerShapeMask) >> kPointerShapeShift);
    return value < kTerminalPointerShapeCount ? static_cast<TerminalPointerShape>(value) : TerminalPointerShape::None;
  }
  static std::uint16_t PointerShapeFlags(TerminalPointerShape shape) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(shape) << kPointerShapeShift) & kPointerShapeMask;
  }

  std::uint32_t rows = 24;
  std::uint32_t columns = 80;
  std::uint16_t flags = kCursorVisible | kCursorBlinking | kRunning;
  TerminalCursorShape cursor_shape = TerminalCursorShape::Block;
  // The latest input_seq the host has written to the pty AND whose effect this
  // frame can show (the pty answered, or 50 ms passed).
  std::uint64_t echo_ack = 0;
  std::uint64_t previous_top = 0;
  std::uint64_t screen_top = 0;
  std::uint32_t cursor_row = 0;  // relative to screen_top
  std::uint32_t cursor_column = 0;

  // Cover [previous_top, screen_top) in order; empty on the alternate screen.
  std::vector<TerminalHostRun> scrollback;
  std::vector<TerminalLine> scrollback_lines;  // the Lines runs' lines, concatenated

  // One entry per line from screen_top to the end of the buffer (at most `rows`;
  // fewer on a fresh terminal whose buffer has not filled the screen): true when
  // the client keeps its line at the same absolute index.
  std::vector<bool> screen_keep;
  std::vector<TerminalLine> screen_lines;  // the non-kept rows, in order

  // Outward signals, present only in the frame where they changed.
  std::optional<std::string> title;
  std::optional<std::string> working_directory;  // a HOST path (OSC 7)
  std::optional<std::string> clipboard;          // OSC 52, applied under local policy
  std::optional<TerminalHostNotification> notification;
  // A program on the host asked for this URL to be opened -- through the BROWSER
  // the server gives its terminals (TerminalTable::RequestOpenUrl), never through
  // the byte stream, so printed text cannot raise one. Sent to ONE client.
  std::optional<std::string> open_url;
  // Every non-zero TerminalCell::extra the frame's lines carry, sorted by id.
  std::vector<TerminalHostCellExtra> extras;

  bool has(std::uint16_t bit) const { return (flags & bit) != 0; }
};

// Hostile-input bounds the decoders enforce.
inline constexpr std::uint32_t kMaxHostTerminalDimension = 4096;
inline constexpr std::size_t kMaxHostTerminalLineCells = 65536;
inline constexpr std::size_t kMaxHostTerminalTextBytes = 64u << 20;
// The opener's own cap (platform::IsOpenableExternalUrl).
inline constexpr std::size_t kMaxHostOpenUrlBytes = 8192;

// What the receiving peer's wire version decodes beyond the floor's frame. The
// server builds one per connection from its hello; a field a peer cannot read
// is left out, so an older client keeps working (it draws those underlines in
// the text's colour).
struct TerminalHostWireOptions {
  // Version 4: the extras' SGR 58 underline colours, in their own section.
  bool underline_colors = true;
};

void EncodeTerminalLine(std::string& out, const TerminalLine& line);
void EncodeTerminalHostFrame(std::string& out, const TerminalHostFrame& frame,
                             TerminalHostWireOptions options = {});
// nullopt on any malformed, truncated or out-of-range field.
std::optional<TerminalHostFrame> DecodeTerminalHostFrame(std::string_view bytes);

// A run of lines (term/scrollback's page), and back; nullopt on malformed bytes or
// on more than `max_lines` lines (the page size the caller asked for).
void EncodeTerminalLines(std::string& out, const std::vector<TerminalLine>& lines);
std::optional<std::vector<TerminalLine>> DecodeTerminalLines(std::string_view bytes,
                                                             std::uint64_t max_lines);

// A batch of input events in one content frame.
void EncodeTerminalInputEvent(std::string& out, const TerminalInputEvent& event);
bool DecodeTerminalInputEvents(std::string_view bytes, std::vector<TerminalInputEvent>& out);

}  // namespace microide::terminal
