// Randomized invariant sweep over the terminal emulator.
//
// The hand-written session tests drive one escape sequence at a time and check
// what it produced. This drives long pseudo-random streams -- escape sequences,
// text, resizes and scrollback trims interleaved the way a real program's output
// is -- and after every chunk asserts only the things that must ALWAYS hold. A
// sequence that leaves the cursor outside the screen, a line longer than the
// terminal is wide, or a snapshot range that disagrees with LineCount is a
// renderer read out of bounds, and none of those need a specific escape sequence
// to be worth catching.
//
// The generator is weighted toward the shapes that historically break emulators:
// the last column (pending wrap), wide and combining characters, scroll regions
// set right at the screen edges, resizes while the alternate screen is active,
// and trims that race the cursor.

#include "TestSupport.h"
#include "TerminalSessionTestAccess.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "terminal/TerminalSession.h"
#include "util/StringUtil.h"

namespace microide::tests {
namespace {

using microide::terminal::TerminalLine;
using microide::terminal::TerminalSession;

struct Rng {
  std::uint64_t state;
  explicit Rng(std::uint64_t seed) : state(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}
  std::uint64_t Next() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1Dull;
  }
  std::size_t Below(std::size_t bound) { return bound == 0 ? 0 : Next() % bound; }
  bool Chance(std::size_t percent) { return Below(100) < percent; }
};

// Text fragments, chosen so the stream keeps hitting the last column with a
// character that does not fit in one cell.
const std::vector<std::string>& TextPool() {
  static const std::vector<std::string> pool = {
      "a",      "hello",  "x",       "  ",    "\t",     "\n",     "\r",     "\r\n",
      "日本語", "🙂",     "é", "ÅÄÖ",   "\b",     "\x07",   "0123456789",
      "the quick brown fox jumps over the lazy dog",
  };
  return pool;
}

// Escape sequences spanning the families the session implements.
std::string RandomEscape(Rng& rng, std::size_t rows, std::size_t columns) {
  const std::size_t row = 1 + rng.Below(rows + 2);        // deliberately over-range sometimes
  const std::size_t column = 1 + rng.Below(columns + 2);
  const std::size_t n = rng.Below(6);
  switch (rng.Below(26)) {
    case 0: return "\x1b[" + std::to_string(row) + ";" + std::to_string(column) + "H";
    case 1: return "\x1b[" + std::to_string(n) + "A";
    case 2: return "\x1b[" + std::to_string(n) + "B";
    case 3: return "\x1b[" + std::to_string(n) + "C";
    case 4: return "\x1b[" + std::to_string(n) + "D";
    case 5: return "\x1b[" + std::to_string(n) + "J";
    case 6: return "\x1b[" + std::to_string(n) + "K";
    case 7: return "\x1b[" + std::to_string(n) + "L";
    case 8: return "\x1b[" + std::to_string(n) + "M";
    case 9: return "\x1b[" + std::to_string(n) + "P";
    case 10: return "\x1b[" + std::to_string(n) + "@";
    case 11: return "\x1b[" + std::to_string(n) + "X";
    case 12: return "\x1b[" + std::to_string(row) + ";" + std::to_string(row + n) + "r";
    case 13: return "\x1b[?1049h";
    case 14: return "\x1b[?1049l";
    case 15: return "\x1b[?25l";
    case 16: return "\x1b[?25h";
    case 17: return "\x1b[?7l";   // autowrap off
    case 18: return "\x1b[?7h";   // autowrap on
    case 19: return "\x1b[" + std::to_string(30 + rng.Below(10)) + "m";
    case 20: return "\x1b[0m";
    case 21: return "\x1bM";      // reverse index
    // Split literals: a hex escape is GREEDY, so "\x1bD" is the single value
    // 0x1BD (out of range, and not ESC D at all) -- these two cases fed the
    // sweep a malformed byte instead of the sequence they name.
    case 22: return "\x1b" "D";   // index
    case 23: return "\x1b" "E";   // next line
    case 24: return "\x1bH";      // set tab stop
    default: return "\x1b[" + std::to_string(n) + "S";
  }
}

std::string Describe(std::uint64_t seed, std::size_t step) {
  return " [seed=" + std::to_string(seed) + " step=" + std::to_string(step) + "]";
}

// `widest_ever` is the largest column count this session has ever had. A shrink
// deliberately does NOT truncate the lines it already holds -- widening again
// brings the text back, and the renderer clamps its own draw to the pane -- so
// the bound on a line is the widest the terminal has ever been, not its width
// right now. That is still a bound, and an unbounded one would be a leak.
void ExpectStructuralInvariants(const TerminalSession& session,
                                std::size_t widest_ever,
                                std::uint64_t seed,
                                std::size_t step) {
  const std::size_t rows = session.rows();
  const std::size_t columns = session.columns();
  Expect(rows >= 1 && columns >= 1, "geometry never collapses to zero" + Describe(seed, step));

  const std::size_t line_count = session.LineCount();
  Expect(line_count >= 1, "there is always at least one line" + Describe(seed, step));

  // The cursor addresses a cell that exists. A renderer draws the caret from
  // this pair without re-clamping.
  Expect(session.cursor_row() < line_count,
         "the cursor row must name a line that exists: row " +
             std::to_string(session.cursor_row()) + " of " + std::to_string(line_count) +
             Describe(seed, step));
  Expect(session.cursor_column() <= columns,
         "the cursor column must stay within the screen (one past is pending wrap): column " +
             std::to_string(session.cursor_column()) + " of " + std::to_string(columns) +
             Describe(seed, step));

  const std::vector<TerminalLine> lines = session.SnapshotLines();
  Expect(lines.size() == line_count,
         "SnapshotLines must agree with LineCount" + Describe(seed, step));
  for (std::size_t i = 0; i < lines.size(); ++i) {
    Expect(lines[i].cells.size() <= widest_ever,
           "a line must never be wider than the widest the terminal has ever been: line " +
               std::to_string(i) + " holds " + std::to_string(lines[i].cells.size()) +
               " cells against a high-water mark of " + std::to_string(widest_ever) +
               Describe(seed, step));
  }

  // Every windowed read must be a slice of the whole-screen read: this is the
  // accessor the renderer actually uses, once per visible row per frame.
  if (line_count > 0) {
    const std::size_t start = line_count / 2;
    const std::size_t want = 3;
    const std::vector<TerminalLine> range = session.SnapshotLineRange(start, want);
    Expect(range.size() == std::min(want, line_count - start),
           "a windowed snapshot must return exactly the rows in range" + Describe(seed, step));
    for (std::size_t i = 0; i < range.size(); ++i) {
      Expect(range[i].cells.size() == lines[start + i].cells.size(),
             "a windowed snapshot row must equal the full snapshot's row" +
                 Describe(seed, step));
    }
  }
  // Reading past the end is a clamp, not an out-of-bounds read.
  Expect(session.SnapshotLineRange(line_count + 5, 4).empty(),
         "a snapshot starting past the end is empty" + Describe(seed, step));
}

void TestTerminalSurvivesRandomStreamsWithInvariantsHeld() {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    Rng rng(seed);
    TerminalSession session;
    std::size_t rows = 2 + rng.Below(10);
    std::size_t columns = 2 + rng.Below(20);
    TerminalSessionTestAccess::Reset(session, rows, columns);
    session.SetMaxScrollbackLines(4 + rng.Below(40));
    std::size_t widest_ever = columns;

    for (std::size_t step = 0; step < 60; ++step) {
      std::string chunk;
      const std::size_t pieces = 1 + rng.Below(8);
      for (std::size_t i = 0; i < pieces; ++i) {
        if (rng.Chance(45)) {
          chunk += RandomEscape(rng, rows, columns);
        } else {
          const std::vector<std::string>& pool = TextPool();
          chunk += pool[rng.Below(pool.size())];
        }
      }
      TerminalSessionTestAccess::AppendOutput(session, chunk);
      ExpectStructuralInvariants(session, widest_ever, seed, step);

      if (rng.Chance(12)) {
        rows = 1 + rng.Below(12);
        columns = 1 + rng.Below(24);
        widest_ever = std::max(widest_ever, columns);
        session.Resize(rows, columns);
        ExpectStructuralInvariants(session, widest_ever, seed, step);
      }
      if (rng.Chance(6)) {
        session.SetMaxScrollbackLines(1 + rng.Below(30));
        ExpectStructuralInvariants(session, widest_ever, seed, step);
      }
    }
  }
}

// A byte stream split at every possible boundary must produce the same screen as
// the whole stream at once: the reader thread hands over whatever the pty gave
// it, so an escape sequence arrives in two halves routinely. A parser that keeps
// state in a local rather than the session shows up here and nowhere else.
void TestChunkBoundariesDoNotChangeTheResult() {
  const std::vector<std::string> streams = {
      "\x1b[31mred\x1b[0m plain",
      "\x1b[2;3Hpositioned\x1b[Kcleared",
      "abc\x1b[1;5rscroll\ndef\nghi\njkl",
      "wide 日本語 at the edge and a 🙂 too",
      "\x1b[?1049halt screen\x1b[?1049lback",
      // "\x07a" would extend the escape to 0x7AF; the BEL has to end the literal.
      "\x1b]0;a title\x07" "after the osc",
      "combining é and tab\there",
      "\x1b[10Ccolumns\x1b[4Dback",
  };
  for (const std::string& stream : streams) {
    TerminalSession whole;
    TerminalSessionTestAccess::Reset(whole, 6, 12);
    TerminalSessionTestAccess::AppendOutput(whole, stream);
    const std::vector<TerminalLine> expected = whole.SnapshotLines();

    for (std::size_t split = 1; split < stream.size(); ++split) {
      TerminalSession split_session;
      TerminalSessionTestAccess::Reset(split_session, 6, 12);
      TerminalSessionTestAccess::AppendOutput(split_session, stream.substr(0, split));
      TerminalSessionTestAccess::AppendOutput(split_session, stream.substr(split));
      const std::vector<TerminalLine> actual = split_session.SnapshotLines();

      Expect(actual.size() == expected.size(),
             "splitting the stream changed the line count at offset " + std::to_string(split) +
                 " of '" + stream + "'");
      for (std::size_t line = 0; line < expected.size(); ++line) {
        Expect(actual[line].cells.size() == expected[line].cells.size(),
               "splitting the stream changed line " + std::to_string(line) + " at offset " +
                   std::to_string(split) + " of '" + stream + "'");
        for (std::size_t cell = 0; cell < expected[line].cells.size(); ++cell) {
          Expect(actual[line].cells[cell].DisplayText() == expected[line].cells[cell].DisplayText(),
                 "splitting the stream changed the glyph at line " + std::to_string(line) +
                     " cell " + std::to_string(cell) + ", offset " + std::to_string(split) +
                     " of '" + stream + "'");
        }
      }
      Expect(split_session.cursor_row() == whole.cursor_row() &&
                 split_session.cursor_column() == whole.cursor_column(),
             "splitting the stream moved the cursor at offset " + std::to_string(split) +
                 " of '" + stream + "'");
    }
  }
}

// Scrollback is a declared cap, so it must hold no matter how much is written.
void TestScrollbackStaysInsideItsCap() {
  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    Rng rng(seed ^ 0x5CBull);
    TerminalSession session;
    const std::size_t rows = 2 + rng.Below(8);
    const std::size_t columns = 4 + rng.Below(16);
    TerminalSessionTestAccess::Reset(session, rows, columns);
    // SetMaxScrollbackLines clamps its argument to a 200-line floor, and the trim
    // coalesces: it fires once the buffer is 25 % above the target, then cuts all
    // the way back. So the true ceiling is the clamped cap plus the live screen,
    // plus that high-watermark slack.
    const std::size_t requested = 200 + rng.Below(400);
    session.SetMaxScrollbackLines(requested);
    const std::size_t target = requested + rows;
    const std::size_t ceiling = target + target / 4 + 2;

    for (std::size_t i = 0; i < 4 * requested; ++i) {
      TerminalSessionTestAccess::AppendOutput(session, "line " + std::to_string(i) + "\r\n");
      Expect(session.LineCount() <= ceiling,
             "scrollback must stay within its cap plus the live screen: " +
                 std::to_string(session.LineCount()) + " lines against a ceiling of " +
                 std::to_string(ceiling) + " [seed=" + std::to_string(seed) + "]");
    }
    ExpectStructuralInvariants(session, columns, seed, 0);
  }
}

}  // namespace

void RegisterTerminalInvariantSweepTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TerminalInvariantSweep/SurvivesRandomStreamsWithInvariantsHeld",
          TestTerminalSurvivesRandomStreamsWithInvariantsHeld);
  AddTest(tests, "TerminalInvariantSweep/ChunkBoundariesDoNotChangeTheResult",
          TestChunkBoundariesDoNotChangeTheResult);
  AddTest(tests, "TerminalInvariantSweep/ScrollbackStaysInsideItsCap",
          TestScrollbackStaysInsideItsCap);
}

}  // namespace microide::tests
