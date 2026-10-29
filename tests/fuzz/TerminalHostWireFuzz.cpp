// The host terminal's wire decoders (src/terminal/TerminalHostWire.h). A frame and
// a scrollback page come from the host's server and an input batch from the
// client, so all three are hostile-input surfaces (dev-docs/design/remote-projects.md
// § 6.9). The first input byte picks the decoder.
//
// Invariants: a successful decode re-encodes to bytes that decode again to the same
// encoding (nothing dropped, invented or altered on a second pass); a frame never
// holds more screen lines than rows, nor a cursor outside them, nor more promoted
// lines than a screen holds (the client loops over them); a scrollback page
// never holds more lines than its caller allowed. Run with `-malloc_limit_mb=512`:
// a count the decoder trusts before checking it shows up as an allocation far
// larger than the input, which no output invariant can see.
#include "terminal/TerminalHostWire.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace microide::terminal;

constexpr std::uint64_t kPageLines = 64;

void Frame(std::string_view bytes) {
  const std::optional<TerminalHostFrame> frame = DecodeTerminalHostFrame(bytes);
  if (!frame.has_value()) {
    return;
  }
  if (frame->screen_keep.size() > frame->rows || frame->screen_lines.size() > frame->rows ||
      frame->cursor_row >= frame->rows || frame->cursor_column >= frame->columns) {
    std::abort();
  }
  // The client appends a line per promoted line: never more than one screen.
  std::uint64_t promoted = 0;
  for (const TerminalHostRun& run : frame->scrollback) {
    promoted += run.kind == TerminalHostRun::Kind::Promote ? run.count : 0;
  }
  if (promoted > kMaxHostTerminalDimension) {
    std::abort();
  }
  std::string once;
  EncodeTerminalHostFrame(once, *frame);
  const std::optional<TerminalHostFrame> again = DecodeTerminalHostFrame(once);
  if (!again.has_value()) {
    std::abort();
  }
  std::string twice;
  EncodeTerminalHostFrame(twice, *again);
  if (once != twice) {
    std::abort();
  }
}

void Lines(std::string_view bytes) {
  const std::optional<std::vector<TerminalLine>> lines = DecodeTerminalLines(bytes, kPageLines);
  if (!lines.has_value()) {
    return;
  }
  if (lines->size() > kPageLines) {
    std::abort();
  }
  std::string once;
  EncodeTerminalLines(once, *lines);
  const std::optional<std::vector<TerminalLine>> again = DecodeTerminalLines(once, kPageLines);
  if (!again.has_value() || again->size() != lines->size()) {
    std::abort();
  }
  for (std::size_t i = 0; i < lines->size(); ++i) {
    if (!SameLine((*lines)[i], (*again)[i])) {
      std::abort();
    }
  }
}

void Input(std::string_view bytes) {
  std::vector<TerminalInputEvent> events;
  if (!DecodeTerminalInputEvents(bytes, events)) {
    return;
  }
  std::string once;
  for (const TerminalInputEvent& event : events) {
    EncodeTerminalInputEvent(once, event);
  }
  std::vector<TerminalInputEvent> again;
  if (!DecodeTerminalInputEvents(once, again) || again.size() != events.size()) {
    std::abort();
  }
  std::string twice;
  for (const TerminalInputEvent& event : again) {
    EncodeTerminalInputEvent(twice, event);
  }
  if (once != twice) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }
  const std::string_view bytes(reinterpret_cast<const char*>(data) + 1, size - 1);
  switch (data[0] % 3) {
    case 0:
      Frame(bytes);
      break;
    case 1:
      Lines(bytes);
      break;
    default:
      Input(bytes);
      break;
  }
  return 0;
}
