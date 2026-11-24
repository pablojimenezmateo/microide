#include "terminal/TerminalSession.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalInternalConstants.h"
#include "terminal/TerminalMouseEncoder.h"
#include "terminal/TerminalOscClipboard.h"
#include "platform/TerminalBackend.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <unistd.h>

namespace microide::terminal {

namespace {

// Upper bound on a single CSI/OSC/string-payload escape sequence. A stream that
// opens an escape and never terminates it (a runaway or hostile program) would
// otherwise grow escape_sequence_buffer_ without limit and swallow all
// subsequent output. On overflow we abandon the sequence and resume normal text.
constexpr std::size_t kMaxEscapeSequenceLength = 8192;

}  // namespace

void TerminalSession::AbandonEscapeSequenceLocked() {
  util::AddPerformanceCounter(util::PerfCounterId::TerminalEscapeSequencesAborted);
  escape_sequence_buffer_.clear();
  escape_mode_ = EscapeMode::None;
  osc_escape_pending_ = false;
}

void TerminalSession::EmitProcessExitMarkerLocked() {
  // The child may have died mid-escape-sequence (e.g. an OSC/DCS opened but never
  // terminated). escape_mode_ persists across reads, so without resetting it the
  // marker below would be fed straight into the dangling parser and consumed as
  // sequence content — the marker would never display. Also drop any partial
  // UTF-8 lead bytes so the marker text is not prefixed by a stray glyph.
  AbandonEscapeSequenceLocked();
  pending_utf8_sequence_.clear();
  // If the child died while on the alternate screen (e.g. a full-screen editor
  // crashed), hand the primary buffer back first — matching xterm/VTE, which
  // restore the primary screen when the application exits. Otherwise the marker
  // append + TrimScrollbackLocked (capped at `rows_` on the alt grid) would erase
  // the still-visible alternate rows from the top.
  if (use_alternate_screen_) {
    SetAlternateScreenLocked(false, false);
  }
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }
  if (!lines_.back().cells.empty()) {
    lines_.push_back(TerminalLine{});
  }
  cursor_row_ = lines_.size() - 1;
  cursor_column_ = lines_.back().cells.size();
  AppendOutputLocked("[process exited]");
  lines_.push_back(TerminalLine{});
  TrimScrollbackLocked();
  AdvanceSnapshotGenerationLocked();
}

namespace {

// CAN (0x18) and SUB (0x1A) abort an escape sequence or control string.
constexpr bool IsCanOrSub(unsigned char byte) { return byte == 0x18 || byte == 0x1a; }

}  // namespace

void TerminalSession::ExecuteC0Locked(unsigned char byte) {
  switch (byte) {
    case '\r':
      cursor_column_ = 0;
      break;
    case '\n':
      AdvanceCursorRowLocked();
      break;
    case '\v':    // VT (0x0B)
    case '\f': {  // FF (0x0C)
      // ECMA-48 index: move down one row PRESERVING the column (like IND / ESC D),
      // NOT resetting to column 0 the way LF does. VT/FF are never subject to the
      // PTY's ONLCR translation, so `A\vB` must staircase (B under the char after
      // A), not land B at column 0. Mirrors the IND handler above.
      const std::size_t saved_column = cursor_column_;
      AdvanceCursorRowLocked();
      cursor_column_ = saved_column;
      break;
    }
    case '\b':
      // From the pending-wrap column a backspace lands on the second-to-last
      // column, as in xterm (see ClampPendingWrapColumnLocked).
      ClampPendingWrapColumn(cursor_column_, columns_);
      if (cursor_column_ > 0) {
        --cursor_column_;
      }
      break;
    case '\a':
      // BEL: no effect on the screen; the UI marks a background tab with it
      // (ConsumeBell), and a host session carries it in the next frame.
      pending_bell_ = true;
      pending_host_bell_ = true;
      break;
    case 0x0e:  // SO: G1 into GL
      charsets_.shifted_out = true;
      break;
    case 0x0f:  // SI: G0 back into GL
      charsets_.shifted_out = false;
      break;
    case '\t': {
      // HT is pure cursor motion to the next tab stop. It must NOT write
      // spaces over the cells it passes: real terminals (xterm/VTE) leave the
      // underlying glyphs intact, so `ABCDEFGHIJ\r\tX` yields `ABCDEFGHXJ`, not
      // `        XJ`. A later write past the current line length pads the gap
      // with default-style blanks via ResizeLineLocked, so no fill is needed here.
      // Not clamped: a tab at the pending-wrap column leaves the wrap pending,
      // as xterm.js does (its tab() returns before moving when x >= cols), so
      // the next glyph still wraps.
      cursor_column_ = NextTabStopLocked(cursor_column_);
      break;
    }
    default:
      // The remaining C0 controls have no effect on the screen.
      break;
  }
}

void TerminalSession::AppendOutputLocked(std::string_view data) {
  // The escape/UTF-8 parser, called per read chunk on the terminal reader
  // thread. Scoped per chunk, not per byte: the whole point of the ASCII bulk
  // fast path below is that per-byte work is the cost, so per-byte
  // instrumentation would be the same mistake.
  util::PerformanceTrace::Scope perf_scope("terminal::AppendOutput");
  util::AddPerformanceCounter(util::PerfCounterId::TerminalOutputBytesParsed, data.size());
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }

  for (std::size_t data_index = 0; data_index < data.size(); ++data_index) {
    const unsigned char byte = static_cast<unsigned char>(data[data_index]);
    switch (escape_mode_) {
      case EscapeMode::None:
        // Bulk fast path for the case that dominates every real command's output:
        // a run of plain printable ASCII while the parser is in ground state. The
        // per-byte path walks the escape modes and a switch, then re-does the line
        // lookup, the line resize, and the wide-pair break inside PutGlyphLocked
        // for EVERY byte. PutAsciiRunLocked does each of those once for the whole
        // run; it returns 0 when the run cannot be taken whole (a due wrap, a
        // control byte immediately) so the per-byte parser still owns every case
        // it owned before.
        if (byte >= 0x20 && byte < 0x7F && pending_utf8_sequence_.empty()) {
          if (const std::size_t consumed = PutAsciiRunLocked(data.substr(data_index));
              consumed > 0) {
            data_index += consumed - 1;  // the loop's ++ lands on the first unconsumed byte
            continue;
          }
        }
        ConsumeGroundByteLocked(byte);
        break;
      case EscapeMode::AfterEscape:
        ConsumeAfterEscapeByteLocked(byte);
        break;
      case EscapeMode::CharsetDesignate:
        ConsumeCharsetDesignateByteLocked(byte);
        break;
      case EscapeMode::Csi:
        ConsumeCsiByteLocked(byte);
        break;
      case EscapeMode::Osc:
      case EscapeMode::StringPayload:
        // An ESC not forming ST ends the string and begins a new escape: this
        // byte is then the first after that ESC, so the loop runs it again.
        if (ConsumeControlStringByteLocked(byte) == ControlStringByte::Reprocess) {
          --data_index;  // the unsigned wrap at 0 is undone by the loop's ++
        }
        break;
    }
  }

  TrimScrollbackLocked();
  AdvanceSnapshotGenerationLocked();
}

void TerminalSession::EnterEscapeModeLocked(EscapeMode mode) {
  escape_sequence_buffer_.clear();
  escape_mode_ = mode;
  osc_escape_pending_ = false;
}

void TerminalSession::ConsumeGroundByteLocked(unsigned char byte) {
  if (byte == '\x1b') {
    if (!pending_utf8_sequence_.empty()) {
      PutGlyphLocked(util::kUtf8ReplacementChar);
      pending_utf8_sequence_.clear();
    }
    EnterEscapeModeLocked(EscapeMode::AfterEscape);
    return;
  }
  if (!pending_utf8_sequence_.empty() && byte < 0x80) {
    PutGlyphLocked(util::kUtf8ReplacementChar);
    pending_utf8_sequence_.clear();
  }
  switch (byte) {
    case '\r':
    case '\n':
    case '\v':
    case '\f':
    case '\b':
    case '\a':
    case 0x0e:
    case 0x0f:
    case '\t':
      ExecuteC0Locked(byte);
      return;
    case 0x7f:
      // DEL received in the output stream is ignored (ECMA-48 / xterm / VTE).
      // The previous destructive delete-and-shift corrupted the display when a
      // stray 0x7f appeared in child output (e.g. `printf '\x7f'` after text).
      return;
    default:
      break;
  }
  if (byte < 0x20) {
    return;  // every remaining C0 control has no positional effect
  }
  if (byte < 0x80) {
    // Reached only when PutAsciiRunLocked declined the run (a wrap or a
    // no-autowrap overwrite is due at the right margin).
    const char glyph = static_cast<char>(byte);
    PutGlyphLocked(std::string_view(&glyph, 1));
    return;
  }
  ConsumeUtf8ByteLocked(byte);
}

void TerminalSession::ConsumeUtf8ByteLocked(unsigned char byte) {
  if (!pending_utf8_sequence_.empty() && !util::IsUtf8ContinuationByte(byte)) {
    // A lead byte cut the pending sequence short: it shows as one replacement,
    // and this byte starts afresh.
    PutGlyphLocked(util::kUtf8ReplacementChar);
    pending_utf8_sequence_.clear();
  }
  if (pending_utf8_sequence_.empty()) {
    const std::size_t sequence_length = util::Utf8SequenceLength(byte);
    if (sequence_length == 0) {
      PutGlyphLocked(util::kUtf8ReplacementChar);
      return;
    }
    pending_utf8_sequence_.push_back(static_cast<char>(byte));
    if (sequence_length == 1) {
      PutGlyphLocked(pending_utf8_sequence_);
      pending_utf8_sequence_.clear();
    }
    return;
  }
  pending_utf8_sequence_.push_back(static_cast<char>(byte));
  const std::size_t sequence_length =
      util::Utf8SequenceLength(static_cast<unsigned char>(pending_utf8_sequence_.front()));
  if (sequence_length != 0 && pending_utf8_sequence_.size() >= sequence_length) {
    PutGlyphLocked(pending_utf8_sequence_);
    pending_utf8_sequence_.clear();
  }
}

void TerminalSession::ConsumeAfterEscapeByteLocked(unsigned char byte) {
  switch (byte) {
    case '[':
      EnterEscapeModeLocked(EscapeMode::Csi);
      escape_sequence_buffer_.push_back('[');
      return;
    case ']':
      EnterEscapeModeLocked(EscapeMode::Osc);
      escape_sequence_buffer_.push_back(']');
      return;
    case '(':
    case ')':
    case '*':
    case '+':
    case '-':
    case '.':
    case '/':
      EnterEscapeModeLocked(EscapeMode::CharsetDesignate);
      escape_sequence_buffer_.push_back(static_cast<char>(byte));
      return;
    case 'P':
    case 'X':
    case '^':
    case '_':
      // DCS (P), SOS (X), PM (^), APC (_): consume the string payload up to its
      // String Terminator instead of letting it fall through and print as text.
      EnterEscapeModeLocked(EscapeMode::StringPayload);
      return;
    case '7':
      SaveCursorLocked();
      break;
    case '8':
      RestoreCursorLocked();
      break;
    case 'H':
      // HTS — set a horizontal tab stop at the cursor column.
      if (tab_stops_.empty()) {
        ResetTabStopsLocked();
      }
      if (cursor_column_ < tab_stops_.size()) {
        tab_stops_[cursor_column_] = true;
      }
      break;
    case 'c':
      FullResetLocked();
      break;
    case 'Z':
      SendBytesLocked("\x1b[?1;2c");
      break;
    case 'D':
    case 'E': {
      // IND (ESC D) moves down one row PRESERVING the column; only NEL (ESC E)
      // resets to column 0. AdvanceCursorRowLocked unconditionally zeroes the
      // column (correct for LF/wrap), so restore it for IND. Unlike a bare LF,
      // IND is not subject to the PTY's ONLCR translation, so a program using it
      // to move down while holding its column must land at that column.
      const std::size_t saved_column = cursor_column_;
      AdvanceCursorRowLocked();
      cursor_column_ = byte == 'D' ? saved_column : 0;
      break;
    }
    case 'M':
      ReverseIndexLocked();
      break;
    default:
      break;
  }
  EnterEscapeModeLocked(EscapeMode::None);
}

// RIS, terminfo's rs1, so the first thing `reset` sends. xterm's full reset: back
// to the primary screen, every mode and the tab stops to their defaults, the
// screen cleared and the cursor home. Scrollback, the title and the shell's
// reported directory stay -- not terminal state.
void TerminalSession::FullResetLocked() {
  SetAlternateScreenLocked(false, false);
  ResetTerminalModesLocked();
  tab_stops_.clear();
  ResetScrollRegionLocked();
  cursor_row_ = PrimaryScreenTopLocked();
  cursor_column_ = 0;
  EraseInDisplayLocked(2);
  AdvanceSnapshotGenerationLocked();
}

// RI: up one row; at the scroll region's top the region scrolls down instead.
void TerminalSession::ReverseIndexLocked() {
  if (use_alternate_screen_) {
    const std::size_t scroll_region_top = ActiveScrollRegionTopLocked();
    const std::size_t scroll_region_bottom = ActiveScrollRegionBottomLocked();
    if (cursor_row_ == scroll_region_top) {
      ScrollRegionDownLocked(scroll_region_top, scroll_region_bottom, 1);
    } else if (cursor_row_ > 0) {
      --cursor_row_;
    }
  } else if (HasCustomScrollRegionLocked()) {
    // Primary buffer with a custom DECSTBM region: RI at the region top
    // scrolls the region down (opens a blank line at the top margin), like
    // the alt-screen branch above; elsewhere it just moves up, floored at
    // the visible-screen top.
    const std::size_t screen_top = PrimaryScreenTopLocked();
    const std::size_t rel_top = ActiveScrollRegionTopLocked();
    const std::size_t rel_bottom = ActiveScrollRegionBottomLocked();
    if (cursor_row_ == screen_top + rel_top) {
      ScrollRegionDownLocked(rel_top, rel_bottom, 1);
    } else if (cursor_row_ > screen_top) {
      --cursor_row_;
    }
  } else {
    // Primary buffer: `cursor_row_` is an absolute index into scrollback,
    // so RI must floor at the visible-screen top (PrimaryScreenTopLocked),
    // not deque index 0 -- otherwise "move up one row" climbs above the
    // viewport into history and later glyphs overwrite it. Mirrors the
    // CUU/CPL/CUP/VPA clamps, which this branch previously missed.
    const std::size_t floor_row = PrimaryScreenTopLocked();
    cursor_row_ = cursor_row_ > floor_row ? cursor_row_ - 1 : floor_row;
  }
}

void TerminalSession::ConsumeCharsetDesignateByteLocked(unsigned char byte) {
  // ECMA-48: ESC/CAN/SUB appearing where the charset final byte is expected
  // cancel the designation rather than being consumed as the final. ESC begins
  // a fresh escape (`ESC ( ESC[2J` must run the CSI, not eat the ESC as the
  // designator), CAN/SUB abort to ground. Mirrors the CSI/OSC abort handling.
  if (byte == 0x1b) {
    EnterEscapeModeLocked(EscapeMode::AfterEscape);
    return;
  }
  if (IsCanOrSub(byte)) {
    EnterEscapeModeLocked(EscapeMode::None);
    return;
  }
  // ECMA-48: a designation is <designator> I... F — zero or more intermediate
  // bytes (0x20..0x2F) followed by one final (0x30..0x7E). Consuming exactly
  // one byte treated the first intermediate as the final, so a real multi-byte
  // designation such as `ESC ( " ?` (DEC Supplemental Graphic) left the `?` to
  // be printed as text (2026-07-10 pass). Capped like every other escape so a
  // runaway stream of intermediates cannot swallow all output.
  if (byte >= 0x20 && byte <= 0x2f) {
    if (escape_sequence_buffer_.size() > kMaxEscapeSequenceLength) {
      AbandonEscapeSequenceLocked();
      return;
    }
    escape_sequence_buffer_.push_back(static_cast<char>(byte));
    return;
  }
  // The final byte: G0 (`(`) or G1 (`)`) becomes DEC Special Graphics for
  // '0' and ASCII for anything else (G2/G3 and multi-byte sets are not drawn).
  if (escape_sequence_buffer_.size() == 1) {
    const bool graphics = byte == '0';
    if (escape_sequence_buffer_.front() == '(') {
      charsets_.g0_dec_graphics = graphics;
    } else if (escape_sequence_buffer_.front() == ')') {
      charsets_.g1_dec_graphics = graphics;
    }
  }
  EnterEscapeModeLocked(EscapeMode::None);
}

void TerminalSession::ConsumeCsiByteLocked(unsigned char byte) {
  // ECMA-48: ESC/CAN/SUB appearing mid-sequence cancel the control sequence.
  // ESC starts a fresh escape (e.g. `\x1b[\x1b[2J` must run the second CSI, not
  // dispatch a garbage sequence ending in the second '['); CAN/SUB abort to
  // ground. Without this the intervening byte was swallowed as a parameter and
  // the next final byte dispatched a corrupt sequence.
  if (byte == 0x1b) {
    EnterEscapeModeLocked(EscapeMode::AfterEscape);
    return;
  }
  if (IsCanOrSub(byte)) {
    EnterEscapeModeLocked(EscapeMode::None);
    return;
  }
  // ECMA-48: a C0 control other than ESC/CAN/SUB embedded mid-CSI is EXECUTED
  // immediately and the control sequence then continues. CSI parameter (0x20..
  // 0x3F), intermediate, and final (0x40..0x7E) bytes are all >= 0x20, so any
  // byte < 0x20 here is unambiguously a C0 to run, not sequence content. The
  // previous code buffered it as a parameter byte and corrupted the dispatch.
  if (byte < 0x20) {
    ExecuteC0Locked(byte);
    return;
  }
  escape_sequence_buffer_.push_back(static_cast<char>(byte));
  if (byte >= '@' && byte <= '~') {
    HandleEscapeSequenceLocked(escape_sequence_buffer_);
    EnterEscapeModeLocked(EscapeMode::None);
  } else if (escape_sequence_buffer_.size() > kMaxEscapeSequenceLength) {
    AbandonEscapeSequenceLocked();
  }
}

// OSC, and the DCS/SOS/PM/APC payloads we do not implement (Sixel, Kitty
// graphics, tmux passthrough, DECRQSS, ...): a string up to its String Terminator
// (ESC \) or BEL. An OSC is dispatched when it ends; the others are discarded.
TerminalSession::ControlStringByte TerminalSession::ConsumeControlStringByteLocked(
    unsigned char byte) {
  const bool osc = escape_mode_ == EscapeMode::Osc;
  const auto finish = [this, osc](EscapeMode next) {
    if (osc) {
      HandleOscSequenceLocked(escape_sequence_buffer_);
    }
    EnterEscapeModeLocked(next);
  };
  // CAN/SUB abort the control string (ESC is handled below as the ST lead-in).
  if (IsCanOrSub(byte)) {
    EnterEscapeModeLocked(EscapeMode::None);
    return ControlStringByte::Consumed;
  }
  if (osc_escape_pending_) {
    osc_escape_pending_ = false;
    if (byte == '\\') {
      finish(EscapeMode::None);
      return ControlStringByte::Consumed;
    }
    // ECMA-48: an ESC not forming ST (`ESC \`) terminates the control string
    // AND begins a new escape sequence -- otherwise a title followed by a real
    // sequence (`\e]0;t\e[0m`) would swallow the `\e[0m` into the OSC payload.
    finish(EscapeMode::AfterEscape);
    return ControlStringByte::Reprocess;
  }
  if (byte == '\a') {
    finish(EscapeMode::None);
    return ControlStringByte::Consumed;
  }
  if (byte == '\x1b') {
    osc_escape_pending_ = true;
    return ControlStringByte::Consumed;
  }
  // Discarded payload bytes are counted through the buffer too, so an
  // unterminated string cannot trap the parser forever.
  if (escape_sequence_buffer_.size() > kMaxEscapeSequenceLength) {
    // A too-large OSC that is an OSC 52 clipboard write is otherwise dropped
    // silently; flag it so the host can tell the user their clipboard write was
    // rejected for being too large (distinct from a policy denial).
    if (osc && std::string_view(escape_sequence_buffer_).starts_with("]52;")) {
      oversized_osc52_dropped_ = true;
    }
    AbandonEscapeSequenceLocked();
    return ControlStringByte::Consumed;
  }
  escape_sequence_buffer_.push_back(static_cast<char>(byte));
  return ControlStringByte::Consumed;
}

}  // namespace microide::terminal
