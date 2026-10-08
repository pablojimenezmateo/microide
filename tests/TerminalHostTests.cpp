#include "TestSupport.h"
#include "TerminalSessionTestAccess.h"

#include "terminal/TerminalHostFrameBuilder.h"
#include "terminal/TerminalHostWire.h"
#include "terminal/TerminalSession.h"

#include <memory>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using terminal::DecodeTerminalHostFrame;
using terminal::DecodeTerminalInputEvents;
using terminal::EncodeTerminalHostFrame;
using terminal::EncodeTerminalInputEvent;
using terminal::TerminalHostFrame;
using terminal::TerminalHostFrameBuilder;
using terminal::TerminalHostRun;
using terminal::TerminalInputEvent;
using terminal::TerminalLine;
using terminal::TerminalSession;

std::string LineText(const TerminalLine& line) {
  std::string text;
  for (const auto& cell : line.cells) {
    text += cell.DisplayText().empty() ? std::string(" ") : std::string(cell.DisplayText());
  }
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

class RecordingChannel final : public terminal::TerminalHostChannel {
 public:
  void Send(const TerminalInputEvent& event) override { events.push_back(event); }
  void Resize(std::size_t rows, std::size_t columns) override {
    resized_rows = rows;
    resized_columns = columns;
  }
  void Close() override { closed = true; }

  std::vector<TerminalInputEvent> events;
  std::size_t resized_rows = 0;
  std::size_t resized_columns = 0;
  bool closed = false;
};

// A host session, a client session in host mode, and the builder between them;
// every Sync goes through the real encoder and decoder.
struct Mirror {
  TerminalSession host;
  TerminalSession client;
  std::shared_ptr<RecordingChannel> channel = std::make_shared<RecordingChannel>();
  TerminalHostFrameBuilder builder;
  TerminalSession::HostCapture capture;
  TerminalHostFrame frame;
  std::size_t last_frame_bytes = 0;

  Mirror(std::size_t rows, std::size_t columns) {
    TerminalSessionTestAccess::Reset(host, rows, columns);
    TerminalSessionTestAccess::Reset(client, rows, columns);
    TerminalSessionTestAccess::SetRunning(host, true);
    TerminalSessionTestAccess::EnterHostMode(client, channel);
  }
  ~Mirror() { client.Stop(); }

  void Write(std::string_view bytes) { TerminalSessionTestAccess::AppendOutput(host, bytes); }

  bool Sync(std::size_t budget = 1u << 30, std::uint64_t echo_ack = 0) {
    host.CaptureForHost(builder.capture_from(), builder.capture_lines_before_screen(500), capture);
    builder.Build(capture, budget, frame);
    frame.echo_ack = echo_ack;
    std::string wire;
    EncodeTerminalHostFrame(wire, frame);
    last_frame_bytes = wire.size();
    std::optional<TerminalHostFrame> decoded = DecodeTerminalHostFrame(wire);
    Expect(decoded.has_value(), "a built frame decodes");
    return decoded.has_value() && client.ApplyHostFrame(std::move(*decoded));
  }

  // The client shows exactly what the host does: every line, the cursor and the modes.
  void ExpectSame(const std::string& what) {
    const std::vector<TerminalLine> host_lines = host.SnapshotLines();
    const std::vector<TerminalLine> client_lines = client.SnapshotLines();
    Expect(host_lines.size() == client_lines.size(),
           what + ": line count " + std::to_string(client_lines.size()) + " vs host " +
               std::to_string(host_lines.size()));
    for (std::size_t i = 0; i < std::min(host_lines.size(), client_lines.size()); ++i) {
      Expect(terminal::SameLine(host_lines[i], client_lines[i]),
             what + ": line " + std::to_string(i) + " is '" + LineText(client_lines[i]) +
                 "', host '" + LineText(host_lines[i]) + "'");
    }
    Expect(host.cursor_row() == client.cursor_row() && host.cursor_column() == client.cursor_column(),
           what + ": cursor " + std::to_string(client.cursor_row()) + "," +
               std::to_string(client.cursor_column()) + " vs host " +
               std::to_string(host.cursor_row()) + "," + std::to_string(host.cursor_column()));
    Expect(host.using_alternate_screen() == client.using_alternate_screen(),
           what + ": the same screen");
  }
};

// A bell on the host reaches the client once, in the next frame — not in every
// frame after it.
void TestHostBellReachesTheClientOnce() {
  Mirror mirror(4, 20);
  mirror.Write("build done\x07");
  Expect(mirror.Sync(), "the frame applies");
  Expect(mirror.frame.has(TerminalHostFrame::kBell), "the frame after a BEL carries it");
  Expect(mirror.client.ConsumeBell(), "and the client rings");
  mirror.Write("more");
  Expect(mirror.Sync(), "the next frame applies");
  Expect(!mirror.frame.has(TerminalHostFrame::kBell), "the next frame does not repeat it");
  Expect(!mirror.client.ConsumeBell(), "so the client rang once");
}

void TestHostFrameRoundTripsEveryField() {
  TerminalHostFrame frame;
  frame.rows = 3;
  frame.columns = 10;
  frame.flags = TerminalHostFrame::kMouseAny | TerminalHostFrame::kFocusEvents |
                TerminalHostFrame::kCursorVisible;
  frame.cursor_shape = terminal::TerminalCursorShape::Bar;
  frame.echo_ack = 77;
  frame.previous_top = 5;
  frame.screen_top = 9;
  frame.cursor_row = 2;
  frame.cursor_column = 9;
  frame.scrollback = {{TerminalHostRun::Kind::Promote, 1},
                      {TerminalHostRun::Kind::Lines, 2},
                      {TerminalHostRun::Kind::Gap, 1}};
  TerminalLine styled;
  for (const char* glyph : {"a", "\xc3\xa9", "\xe4\xb8\xad", "\x01"}) {
    terminal::TerminalCell cell;
    cell.SetUtf8(glyph);
    cell.style.foreground = util::Rgba8{1, 2, 3, 4};
    styled.cells.push_back(cell);
  }
  styled.cells[3].style.attrs = terminal::cell_attr::kBold;
  styled.wrapped_from_previous = true;
  frame.scrollback_lines = {styled, TerminalLine{}};
  frame.screen_keep = {true, false, true};
  frame.screen_lines = {styled};
  frame.title = "vim";
  frame.working_directory = "/home/u/project";
  frame.clipboard = std::string("clip\0board", 10);

  std::string wire;
  EncodeTerminalHostFrame(wire, frame);
  const std::optional<TerminalHostFrame> back = DecodeTerminalHostFrame(wire);
  Expect(back.has_value(), "the frame decodes");
  if (!back) {
    return;
  }
  Expect(back->rows == 3 && back->columns == 10 && back->flags == frame.flags &&
             back->cursor_shape == frame.cursor_shape && back->echo_ack == 77 &&
             back->previous_top == 5 && back->screen_top == 9 && back->cursor_row == 2 &&
             back->cursor_column == 9,
         "the header round-trips");
  Expect(back->scrollback.size() == 3 && back->scrollback[1].kind == TerminalHostRun::Kind::Lines &&
             back->scrollback[1].count == 2,
         "the runs round-trip");
  Expect(back->scrollback_lines.size() == 2 && terminal::SameLine(back->scrollback_lines[0], styled),
         "styled UTF-8 cells and the wrap flag round-trip");
  Expect(back->screen_keep == frame.screen_keep && back->screen_lines.size() == 1,
         "the keep bits round-trip");
  Expect(back->title == frame.title && back->working_directory == frame.working_directory &&
             back->clipboard == frame.clipboard,
         "the outward signals round-trip");

  // Every strict prefix is truncated input and must be refused, never half-applied.
  for (std::size_t length = 0; length < wire.size(); ++length) {
    Expect(!DecodeTerminalHostFrame(std::string_view(wire).substr(0, length)).has_value(),
           "a truncated frame is refused at " + std::to_string(length));
  }
  Expect(!DecodeTerminalHostFrame(wire + "x").has_value(), "trailing bytes are refused");
}

void TestHostFrameDecoderRefusesInconsistentFrames() {
  const auto encode = [](const TerminalHostFrame& frame) {
    std::string wire;
    EncodeTerminalHostFrame(wire, frame);
    return wire;
  };
  TerminalHostFrame base;
  base.screen_top = 4;
  base.previous_top = 2;
  base.scrollback = {{TerminalHostRun::Kind::Gap, 2}};
  Expect(DecodeTerminalHostFrame(encode(base)).has_value(), "the base frame is valid");

  TerminalHostFrame short_runs = base;
  short_runs.scrollback = {{TerminalHostRun::Kind::Gap, 1}};
  Expect(!DecodeTerminalHostFrame(encode(short_runs)).has_value(),
         "runs that do not cover the moved lines are refused");

  TerminalHostFrame missing_lines = base;
  missing_lines.scrollback = {{TerminalHostRun::Kind::Lines, 2}};
  Expect(!DecodeTerminalHostFrame(encode(missing_lines)).has_value(),
         "a Lines run without its lines is refused");

  TerminalHostFrame cursor_off_screen = base;
  cursor_off_screen.cursor_row = base.rows;
  Expect(!DecodeTerminalHostFrame(encode(cursor_off_screen)).has_value(),
         "a cursor outside the screen is refused");

  TerminalHostFrame too_many_rows = base;
  too_many_rows.rows = 2;
  too_many_rows.screen_keep = {false, false, false};
  too_many_rows.screen_lines = {TerminalLine{}, TerminalLine{}, TerminalLine{}};
  Expect(!DecodeTerminalHostFrame(encode(too_many_rows)).has_value(),
         "more screen lines than rows are refused");

  TerminalHostFrame grew_with_runs = base;
  grew_with_runs.screen_top = 1;
  Expect(!DecodeTerminalHostFrame(encode(grew_with_runs)).has_value(),
         "a screen that grew upward carries no runs");
}

void TestInputEventsRoundTrip() {
  std::vector<TerminalInputEvent> events;
  TerminalInputEvent key{.kind = TerminalInputEvent::Kind::Key, .seq = 1};
  key.key.key = terminal::TerminalKeyPress::Key::Char;
  key.key.codepoint = U'é';
  key.key.ctrl = true;
  events.push_back(key);
  events.push_back(TerminalInputEvent{.kind = TerminalInputEvent::Kind::Paste, .seq = 2,
                                      .text = "multi\nline"});
  events.push_back(TerminalInputEvent{.kind = TerminalInputEvent::Kind::MouseButton,
                                      .seq = 3,
                                      .button = terminal::TerminalMouseButton::Right,
                                      .pressed = true,
                                      .row = 4,
                                      .column = 70,
                                      .modifiers = util::kKeyModShift});
  events.push_back(TerminalInputEvent{.kind = TerminalInputEvent::Kind::Focus, .seq = 4,
                                      .pressed = false});
  std::string wire;
  for (const auto& event : events) {
    EncodeTerminalInputEvent(wire, event);
  }
  std::vector<TerminalInputEvent> back;
  Expect(DecodeTerminalInputEvents(wire, back) && back.size() == 4, "the batch decodes");
  if (back.size() != 4) {
    return;
  }
  Expect(back[0].key.codepoint == U'é' && back[0].key.ctrl && !back[0].key.alt,
         "a key keeps its codepoint and modifiers");
  Expect(back[1].text == "multi\nline" && back[1].seq == 2, "a paste keeps its text");
  Expect(back[2].button == terminal::TerminalMouseButton::Right && back[2].pressed &&
             back[2].row == 4 && back[2].column == 70 && back[2].modifiers == util::kKeyModShift,
         "a mouse event keeps its button, cell and modifiers");
  Expect(back[3].kind == TerminalInputEvent::Kind::Focus && !back[3].pressed, "focus out");
  std::vector<TerminalInputEvent> partial;
  Expect(!DecodeTerminalInputEvents(std::string_view(wire).substr(0, wire.size() - 1), partial),
         "a truncated batch is refused");
}

void TestClientMirrorsTheHostAsItScrolls() {
  Mirror mirror(5, 20);
  mirror.Write("$ ");
  Expect(mirror.Sync(), "the first frame applies");
  mirror.ExpectSame("a fresh prompt");

  for (int i = 0; i < 40; ++i) {
    mirror.Write("line " + std::to_string(i) + "\r\n");
    if (i % 7 == 0) {
      Expect(mirror.Sync(), "a frame mid-scroll applies");
      mirror.ExpectSame("mid-scroll at " + std::to_string(i));
    }
  }
  mirror.Write("$ ");
  Expect(mirror.Sync(), "the frame after the scroll applies");
  mirror.ExpectSame("after 40 lines of output");

  // Typing one character changes one row: the frame says "keep" for the others.
  mirror.Write("l");
  Expect(mirror.Sync(), "the keystroke frame applies");
  Expect(mirror.frame.scrollback.empty() && mirror.frame.screen_lines.size() == 1,
         "an echoed keystroke ships one row, not the screen");
  mirror.ExpectSame("after a keystroke");

  // Nothing changed: nothing to ship.
  mirror.host.CaptureForHost(mirror.builder.capture_from(),
                             mirror.builder.capture_lines_before_screen(500), mirror.capture);
  Expect(mirror.builder.UpToDate(mirror.capture), "an unchanged terminal is up to date");
}

void TestClientMirrorsClearsAndRewrites() {
  Mirror mirror(4, 12);
  mirror.Write("one\r\ntwo\r\nthree\r\nfour\r\nfive\r\n");
  Expect(mirror.Sync(), "sync");
  mirror.Write("\x1b[H\x1b[2J" "top\x1b[3;5Hmid");
  Expect(mirror.Sync(), "sync after a clear and a cursor move");
  mirror.ExpectSame("after clear + CUP");
  mirror.Write("\x1b[1;1H\x1b[2K" "rewritten");
  Expect(mirror.Sync(), "sync after an in-place rewrite");
  mirror.ExpectSame("after rewriting a row in place");
}

void TestClientMirrorsTheAlternateScreen() {
  Mirror mirror(4, 16);
  mirror.Write("shell 1\r\nshell 2\r\nshell 3\r\nshell 4\r\nshell 5\r\n$ vim");
  Expect(mirror.Sync(), "sync");
  mirror.Write("\x1b[?1049h\x1b[H\x1b[2Jvim screen\x1b[2;1H~");
  Expect(mirror.Sync(), "sync into the alternate screen");
  mirror.ExpectSame("on the alternate screen");
  mirror.Write("\x1b[3;1H~ more");
  Expect(mirror.Sync(), "sync on the alternate screen");
  mirror.ExpectSame("editing on the alternate screen");
  mirror.Write("\x1b[?1049l\r\n$ ");
  Expect(mirror.Sync(), "sync back to the primary screen");
  mirror.ExpectSame("back on the primary screen, scrollback intact");
}

void TestClientMirrorsAResize() {
  Mirror mirror(6, 20);
  for (int i = 0; i < 20; ++i) {
    mirror.Write("row " + std::to_string(i) + "\r\n");
  }
  Expect(mirror.Sync(), "sync");
  mirror.host.Resize(3, 20);
  Expect(mirror.Sync(), "sync after a shrink");
  mirror.ExpectSame("after shrinking the host");
  mirror.host.Resize(9, 30);
  Expect(mirror.Sync(), "sync after a grow");
  mirror.ExpectSame("after growing the host back over its scrollback");
  mirror.Write("tail\r\n");
  Expect(mirror.Sync(), "sync after more output");
  mirror.ExpectSame("output after the resizes");
}

void TestCreditWindowWithholdsScrollbackAsOneCountedGap() {
  Mirror mirror(3, 20);
  Expect(mirror.Sync(), "sync");
  for (int i = 0; i < 30; ++i) {
    mirror.Write("flood " + std::to_string(i) + "\r\n");
  }
  Expect(mirror.Sync(/*budget=*/0), "a frame with no credit applies");
  std::uint64_t gap = 0;
  for (const auto& run : mirror.frame.scrollback) {
    Expect(run.kind != TerminalHostRun::Kind::Lines, "no scrollback line is sent without credit");
    gap += run.kind == TerminalHostRun::Kind::Gap ? run.count : 0;
  }
  Expect(gap > 0, "the withheld lines are counted");
  const std::vector<TerminalLine> lines = mirror.client.SnapshotLines();
  bool saw_rule = false;
  for (const auto& line : lines) {
    saw_rule = saw_rule || LineText(line).find("lines not transferred") != std::string::npos;
  }
  Expect(saw_rule, "the gap is drawn as one counted rule");
  // The screen itself is never withheld.
  const std::vector<TerminalLine> host_lines = mirror.host.SnapshotLines();
  Expect(LineText(lines[lines.size() - 2]) == LineText(host_lines[host_lines.size() - 2]),
         "the visible screen arrives even with no credit");
}

void TestHostModeInputBecomesSemanticEvents() {
  Mirror mirror(4, 20);
  TerminalSession::KeyPress up;
  up.key = TerminalSession::KeyPress::Key::Up;
  Expect(mirror.client.SendKeyPress(up), "a key press is accepted");
  mirror.client.PasteText("pasted");
  mirror.client.SendBytes("raw");
  Expect(!mirror.client.SendMouseButton(TerminalSession::MouseButton::Left, true, 1, 1, 0) ||
             mirror.channel->events.size() == 3,
         "a click with no mouse tracking on the last frame is not forwarded");
  mirror.Write("\x1b[?1000h");
  Expect(mirror.Sync(), "sync the mouse mode");
  Expect(mirror.client.SendMouseButton(TerminalSession::MouseButton::Left, true, 1, 2, 0),
         "a click is forwarded once the program asked for the mouse");
  const auto& events = mirror.channel->events;
  Expect(events.size() == 4, "four events reached the channel, got " + std::to_string(events.size()));
  if (events.size() == 4) {
    Expect(events[0].kind == TerminalInputEvent::Kind::Key &&
               events[0].key.key == TerminalSession::KeyPress::Key::Up,
           "the key travels as a key, not as bytes encoded against stale modes");
    Expect(events[1].kind == TerminalInputEvent::Kind::Paste && events[1].text == "pasted",
           "a paste travels unbracketed; the host brackets it");
    Expect(events[2].kind == TerminalInputEvent::Kind::Bytes && events[2].text == "raw", "bytes");
    Expect(events[3].kind == TerminalInputEvent::Kind::MouseButton && events[3].column == 2,
           "the click");
    Expect(events[0].seq == 1 && events[1].seq == 2 && events[2].seq == 3 && events[3].seq == 4,
           "input_seq increases by one per event");
  }
  mirror.client.Resize(10, 40);
  Expect(mirror.channel->resized_rows == 10 && mirror.channel->resized_columns == 40,
         "a resize goes to the host");
  mirror.client.Stop();
  Expect(mirror.channel->closed, "stopping a host terminal closes it on the host");
}

void Type(TerminalSession& session, char32_t codepoint) {
  TerminalSession::KeyPress press;
  press.key = TerminalSession::KeyPress::Key::Char;
  press.codepoint = codepoint;
  (void)session.SendKeyPress(press);
}

std::string CursorLineText(const TerminalSession& session) {
  const std::vector<TerminalLine> lines = session.SnapshotLines();
  return session.cursor_row() < lines.size() ? LineText(lines[session.cursor_row()]) : "";
}

bool UnderlinedAt(const TerminalSession& session, std::size_t column) {
  const std::vector<TerminalLine> lines = session.SnapshotLines();
  if (session.cursor_row() >= lines.size() || column >= lines[session.cursor_row()].cells.size()) {
    return false;
  }
  return lines[session.cursor_row()].cells[column].style.underline();
}

// A typed glyph is drawn, underlined, before any frame; the frame that confirms it
// replaces it with exactly the host's cell — nothing left over.
void TestPredictionIsDrawnThenConfirmed() {
  Mirror mirror(4, 20);
  mirror.client.SetPredictionMode(terminal::TerminalPredictionOverlay::Mode::Always);
  mirror.Write("$ ");
  Expect(mirror.Sync(), "sync");
  Type(mirror.client, U'a');
  Type(mirror.client, U'b');
  Expect(CursorLineText(mirror.client) == "$ ab", "both glyphs are drawn in the keystroke's frame");
  Expect(UnderlinedAt(mirror.client, 2) && UnderlinedAt(mirror.client, 3), "underlined while pending");
  Expect(mirror.client.cursor_column() == 4, "the cursor moves with the prediction");
  mirror.Write("a");
  Expect(mirror.Sync(1u << 30, /*echo_ack=*/1), "the first echo arrives");
  Expect(CursorLineText(mirror.client) == "$ ab", "the second is still drawn after the first is confirmed");
  Expect(!UnderlinedAt(mirror.client, 2) && UnderlinedAt(mirror.client, 3),
         "the confirmed glyph is the host's, the pending one still underlined");
  mirror.Write("b");
  Expect(mirror.Sync(1u << 30, 2), "the second echo arrives");
  mirror.ExpectSame("after both echoes, the screen is exactly the host's");
  Expect(mirror.client.PendingPredictions() == 0, "nothing is pending");
}

// A password prompt does not echo: the glyph is gone with the frame that covers
// it, and nothing more is drawn until a prediction is confirmed again.
void TestContradictedPredictionIsGoneAndSuppressesTheNext() {
  Mirror mirror(4, 20);
  mirror.client.SetPredictionMode(terminal::TerminalPredictionOverlay::Mode::Always);
  mirror.Write("Password: ");
  Expect(mirror.Sync(), "sync");
  Type(mirror.client, U's');
  Expect(CursorLineText(mirror.client) == "Password: s", "drawn before the host answers");
  Expect(mirror.Sync(1u << 30, 1), "the frame covering it arrives, unchanged");
  mirror.ExpectSame("the contradicted glyph is gone within the round trip");
  Type(mirror.client, U'e');
  mirror.ExpectSame("after a contradiction nothing is drawn");
  // The program starts echoing again (the password was accepted, a shell prompt).
  mirror.Write("\r\n$ x");
  Expect(mirror.Sync(1u << 30, 2), "a frame covering the undrawn 'e'");
  mirror.ExpectSame("still nothing drawn: 'e' was never echoed");
  Type(mirror.client, U'y');
  mirror.ExpectSame("'y' is tracked but not drawn");
  mirror.Write("y");
  Expect(mirror.Sync(1u << 30, 3), "'y' echoes where it was predicted");
  Type(mirror.client, U'z');
  Expect(CursorLineText(mirror.client) == "$ xyz", "a confirmed prediction lifts the suppression");
}

void TestNothingIsPredictedWhereThePositionIsUnknown() {
  {
    Mirror mirror(3, 6);
    mirror.client.SetPredictionMode(terminal::TerminalPredictionOverlay::Mode::Always);
    mirror.Write("abcde");
    Expect(mirror.Sync(), "sync");
    Type(mirror.client, U'f');
    mirror.ExpectSame("nothing at the right margin");
  }
  {
    Mirror mirror(3, 20);
    mirror.client.SetPredictionMode(terminal::TerminalPredictionOverlay::Mode::Always);
    mirror.Write("$ \x1b[?25l");
    Expect(mirror.Sync(), "sync");
    Type(mirror.client, U'a');
    mirror.ExpectSame("nothing while the cursor is hidden");
  }
  {
    Mirror mirror(3, 20);
    mirror.client.SetPredictionMode(terminal::TerminalPredictionOverlay::Mode::Always);
    mirror.Write("\x1b[?1049h~");
    Expect(mirror.Sync(), "sync");
    Type(mirror.client, U'a');
    mirror.ExpectSame("nothing on the alternate screen");
  }
  {
    Mirror mirror(3, 20);
    mirror.client.SetPredictionMode(terminal::TerminalPredictionOverlay::Mode::Always);
    mirror.Write("$ ");
    Expect(mirror.Sync(), "sync");
    TerminalSession::KeyPress enter;
    enter.key = TerminalSession::KeyPress::Key::Enter;
    (void)mirror.client.SendKeyPress(enter);
    Type(mirror.client, U'a');
    mirror.ExpectSame("nothing after a control key until the host has answered it");
    mirror.Write("\r\n$ ");
    Expect(mirror.Sync(1u << 30, 1), "the host answers the Enter");
    Type(mirror.client, U'b');
    Expect(CursorLineText(mirror.client) == "$ b", "predicting resumes once it has");
  }
  {
    Mirror mirror(3, 20);  // the default: adaptive, and no round trip measured
    mirror.Write("$ ");
    Expect(mirror.Sync(), "sync");
    Type(mirror.client, U'a');
    mirror.ExpectSame("adaptive draws nothing without a slow round trip");
  }
}

// The host enters the alternate screen between two predicted keystrokes: the
// confirmed frame is applied byte for byte, with no predicted cell left over.
void TestModeChangeDuringABurstLeavesTheConfirmedScreenExact() {
  Mirror mirror(4, 20);
  mirror.client.SetPredictionMode(terminal::TerminalPredictionOverlay::Mode::Always);
  mirror.Write("$ ");
  Expect(mirror.Sync(), "sync");
  Type(mirror.client, U'v');
  Type(mirror.client, U'i');
  mirror.Write("v\x1b[?1049h\x1b[H\x1b[2J~\r\n~");
  Expect(mirror.Sync(1u << 30, 2), "the frame that switched screens");
  mirror.ExpectSame("the alternate screen exactly as the host has it");
  mirror.Write("\x1b[?1049l");
  Expect(mirror.Sync(1u << 30, 2), "back to the primary screen");
  mirror.ExpectSame("the primary screen exactly as the host has it");
}

}  // namespace

void RegisterTerminalHostTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalHost/FrameRoundTripsEveryField", TestHostFrameRoundTripsEveryField);
  AddTest(tests, "TerminalHost/DecoderRefusesInconsistentFrames",
          TestHostFrameDecoderRefusesInconsistentFrames);
  AddTest(tests, "TerminalHost/InputEventsRoundTrip", TestInputEventsRoundTrip);
  AddTest(tests, "TerminalHost/ClientMirrorsTheHostAsItScrolls", TestClientMirrorsTheHostAsItScrolls);
  AddTest(tests, "TerminalHost/ClientMirrorsClearsAndRewrites", TestClientMirrorsClearsAndRewrites);
  AddTest(tests, "TerminalHost/ClientMirrorsTheAlternateScreen", TestClientMirrorsTheAlternateScreen);
  AddTest(tests, "TerminalHost/ClientMirrorsAResize", TestClientMirrorsAResize);
  AddTest(tests, "TerminalHost/CreditWindowWithholdsScrollbackAsOneCountedGap",
          TestCreditWindowWithholdsScrollbackAsOneCountedGap);
  AddTest(tests, "TerminalHost/HostModeInputBecomesSemanticEvents",
          TestHostModeInputBecomesSemanticEvents);
  AddTest(tests, "TerminalHost/PredictionIsDrawnThenConfirmed", TestPredictionIsDrawnThenConfirmed);
  AddTest(tests, "TerminalHost/ContradictedPredictionIsGoneAndSuppressesTheNext",
          TestContradictedPredictionIsGoneAndSuppressesTheNext);
  AddTest(tests, "TerminalHost/NothingIsPredictedWhereThePositionIsUnknown",
          TestNothingIsPredictedWhereThePositionIsUnknown);
  AddTest(tests, "TerminalHost/BellReachesTheClientOnce", TestHostBellReachesTheClientOnce);
  AddTest(tests, "TerminalHost/ModeChangeDuringABurstLeavesTheConfirmedScreenExact",
          TestModeChangeDuringABurstLeavesTheConfirmedScreenExact);
}

}  // namespace microide::tests
