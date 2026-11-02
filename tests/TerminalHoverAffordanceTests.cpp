#include "TestSupport.h"

#include "TerminalSessionTestAccess.h"

#include "terminal/TerminalSession.h"

#include <string_view>
#include <vector>

// What a program that owns the mouse lights under a resting pointer
// (TerminalHoverAffordance): the bytes below are Claude Code 2.1.316's own
// answers to a pointer move, recorded through a pty tap -- its collapsed
// "Listed 1 directory" tool group brightens from grey to the default colour on
// hover and goes back to grey when the pointer leaves.

namespace microide::tests {
namespace {

// The hand over a control the program lit (PointerShapeAt's affordance answer).
bool Lit(const terminal::TerminalSession& session, std::size_t row, std::size_t column) {
  return session.PointerShapeAt(row, column) == terminal::TerminalPointerShape::Pointer;
}


using microide::terminal::TerminalSession;
using Button = TerminalSession::MouseButton;

constexpr std::string_view kAnyMotion = "\x1b[?1003h\x1b[?1006h";
// Row 2 (1-based), column 3: the grey group, then its hovered and unhovered paints.
constexpr std::string_view kGrey = "\x1b[2;3H\x1b[38;2;153;153;153mListed \x1b[1m1\x1b[22m directory\x1b[39m";
constexpr std::string_view kLit = "\x1b[2;3HListed \x1b[1m1\x1b[22m directory\x1b[K";

void Fixture(TerminalSession& session) {
  TerminalSessionTestAccess::Reset(session, 6, 40);
  TerminalSessionTestAccess::AppendOutput(session, kAnyMotion);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[1;1Hplain text here");
  TerminalSessionTestAccess::AppendOutput(session, kGrey);
}

void Hover(TerminalSession& session, std::size_t row, std::size_t column) {
  (void)session.SendMouseMotion(Button::None, row, column, {});
}

// OSC 22 (TD-2026-11-02-428 item 1): a program names its own pointer, and the
// pane shows it -- with or without the mouse, kitty's and foot's rule. CSS keywords
// and xterm's X cursor names both; comma fallbacks; empty resets; a query changes
// nothing; RIS (`reset`) forgets it.
void TestAProgramNamesItsPointer() {
  using Shape = terminal::TerminalPointerShape;
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 6, 40);
  Expect(session.PointerShapeAt(1, 1) == Shape::None, "a plain shell names none: the panel decides");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]22;pointer\x1b\\");
  Expect(session.PointerShapeAt(1, 1) == Shape::Pointer && session.PointerShapeAt(5, 39) == Shape::Pointer,
         "a CSS name, over the whole pane, without mouse reporting");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]22;watch\x07");
  Expect(session.PointerShapeAt(1, 1) == Shape::Wait, "an X cursor name (xterm's)");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]22;no-such,crosshair\x07");
  Expect(session.PointerShapeAt(1, 1) == Shape::Crosshair, "the first known of the fallbacks");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]22;nonsense\x07");
  Expect(session.PointerShapeAt(1, 1) == Shape::Crosshair, "an unknown name changes nothing");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]22;?pointer\x07");
  Expect(session.PointerShapeAt(1, 1) == Shape::Crosshair, "nor does a query");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]22;>text\x07");
  Expect(session.PointerShapeAt(1, 1) == Shape::Text, "kitty's push sets it");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b]22;\x07");
  Expect(session.PointerShapeAt(1, 1) == Shape::None, "empty resets it");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1003h\x1b]22;move\x07");
  Expect(session.PointerShapeAt(1, 1) == Shape::Move, "it wins over the mouse owner's arrow");
  TerminalSessionTestAccess::AppendOutput(session, "\x1b" "c");
  Expect(session.PointerShapeAt(1, 1) == Shape::None, "RIS forgets it with the rest of the modes");
}

void TestLitControlIsAHoverTarget() {
  TerminalSession session;
  Fixture(session);
  Hover(session, 1, 4);
  Expect(!Lit(session, 1, 4), "before the program answers, nothing is lit");
  TerminalSessionTestAccess::AppendOutput(session, kLit);
  Expect(Lit(session, 1, 4), "the restyled cell under the pointer is a hover target");
  // The whole run the repaint restyled, its bold "1" included, is the target.
  Expect(Lit(session, 1, 2), "the run's first cell is part of it");
  Expect(Lit(session, 1, 9), "the bold digit is part of the same run");
  Expect(Lit(session, 1, 19), "the run's last cell is part of it");
  Expect(!Lit(session, 1, 25), "a blank cell past the run is not");
  Expect(!Lit(session, 0, 4), "another row is not");
}

void TestMovingWithinTheTargetKeepsIt() {
  TerminalSession session;
  Fixture(session);
  Hover(session, 1, 4);
  TerminalSessionTestAccess::AppendOutput(session, kLit);
  // The program does not repaint for a move inside the control it already lit.
  Hover(session, 1, 12);
  Expect(Lit(session, 1, 12), "a move within the lit control stays a hover target");
}

void TestUnlitControlIsNotATarget() {
  TerminalSession session;
  Fixture(session);
  Hover(session, 1, 4);
  TerminalSessionTestAccess::AppendOutput(session, kLit);
  Hover(session, 0, 2);  // onto plain text: the program unlights the group
  TerminalSessionTestAccess::AppendOutput(session, kGrey);
  Expect(!Lit(session, 0, 2), "plain text the program did not restyle is no target");
  Expect(!Lit(session, 1, 4), "the control the pointer left is no target");
}

void TestTextChangeIsNotHover() {
  TerminalSession session;
  Fixture(session);
  Hover(session, 1, 4);
  // A redraw for another reason (a spinner, a scroll) changes the text under
  // the pointer, which is not a hover answer, whatever its style.
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[2;3H\x1b[1mSpinning 1 directory");
  Expect(!Lit(session, 1, 4), "changed text is not a lit control");
}

void TestNeedsAnyMotionTracking() {
  TerminalSession session;
  Fixture(session);
  Hover(session, 1, 4);
  TerminalSessionTestAccess::AppendOutput(session, kLit);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1003l");
  Expect(!Lit(session, 1, 4),
         "without any-motion tracking the program is sent no hover, so nothing it lights is one");
  // A program with only button tracking never sees a resting pointer.
  TerminalSession buttons;
  TerminalSessionTestAccess::Reset(buttons, 6, 40);
  TerminalSessionTestAccess::AppendOutput(buttons, "\x1b[?1000h");
  TerminalSessionTestAccess::AppendOutput(buttons, kGrey);
  Hover(buttons, 1, 4);
  TerminalSessionTestAccess::AppendOutput(buttons, kLit);
  Expect(!Lit(buttons, 1, 4), "a move under button tracking probes nothing");
}

void TestScreenSwitchForgetsTheProbe() {
  TerminalSession session;
  Fixture(session);
  Hover(session, 1, 4);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1049h");
  TerminalSessionTestAccess::AppendOutput(session, kLit);
  Expect(!Lit(session, 1, 4),
         "a probe of the primary screen says nothing about the alternate one");
}

// A move inside one cell is not reported, as in xterm: under any-motion tracking
// every pixel of travel was a report -- PTY bytes, a redraw in the program, a
// wire message for a host terminal. A tracking mode set again reports afresh.
void TestMotionIsReportedOncePerCell() {
  TerminalSession session;
  TerminalSessionTestAccess::Reset(session, 6, 40);
  TerminalSessionTestAccess::SetRunning(session, true);
  TerminalSessionTestAccess::AppendOutput(session, kAnyMotion);
  Expect(session.SendMouseMotion(Button::None, 1, 4, {}), "the first move into a cell is reported");
  Expect(!session.SendMouseMotion(Button::None, 1, 4, {}), "a move within it is not");
  Expect(TerminalSessionTestAccess::SentBytes(session) == "\x1b[<35;5;2M",
         "one report for the cell: [" + TerminalSessionTestAccess::SentBytes(session) + "]");
  Expect(session.SendMouseMotion(Button::None, 1, 5, {}), "the next cell is");
  Expect(session.SendMouseMotion(Button::Left, 1, 5, {}), "and the same cell with a button held is");
  TerminalSessionTestAccess::ClearSentBytes(session);
  TerminalSessionTestAccess::AppendOutput(session, "\x1b[?1003h");
  Expect(session.SendMouseMotion(Button::Left, 1, 5, {}), "re-enabled tracking hears the pointer again");
}

}  // namespace

void RegisterTerminalHoverAffordanceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalHoverAffordance/AProgramNamesItsPointer", TestAProgramNamesItsPointer);
  AddTest(tests, "TerminalHoverAffordance/LitControlIsAHoverTarget", TestLitControlIsAHoverTarget);
  AddTest(tests, "TerminalHoverAffordance/MovingWithinTheTargetKeepsIt",
          TestMovingWithinTheTargetKeepsIt);
  AddTest(tests, "TerminalHoverAffordance/UnlitControlIsNotATarget", TestUnlitControlIsNotATarget);
  AddTest(tests, "TerminalHoverAffordance/TextChangeIsNotHover", TestTextChangeIsNotHover);
  AddTest(tests, "TerminalHoverAffordance/NeedsAnyMotionTracking", TestNeedsAnyMotionTracking);
  AddTest(tests, "TerminalHoverAffordance/ScreenSwitchForgetsTheProbe",
          TestScreenSwitchForgetsTheProbe);
  AddTest(tests, "TerminalHoverAffordance/MotionIsReportedOncePerCell", TestMotionIsReportedOncePerCell);
}

}  // namespace microide::tests
