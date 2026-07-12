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

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

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

void TerminalSession::AppendOutputLocked(std::string_view data) {
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }

  // Index-based so a byte can be reprocessed (see the OSC/StringPayload ESC
  // restart below): `--data_index; continue;` re-runs the current byte after the
  // parser state has changed. The unsigned wrap at index 0 is well-defined and the
  // subsequent `++data_index` returns it to 0.
  for (std::size_t data_index = 0; data_index < data.size(); ++data_index) {
    const unsigned char byte = static_cast<unsigned char>(data[data_index]);
    if (escape_mode_ == EscapeMode::AfterEscape) {
      if (byte == '[') {
        escape_sequence_buffer_.push_back('[');
        escape_mode_ = EscapeMode::Csi;
        continue;
      }
      if (byte == ']') {
        escape_sequence_buffer_.push_back(']');
        escape_mode_ = EscapeMode::Osc;
        osc_escape_pending_ = false;
        continue;
      }
      if (byte == '(' || byte == ')' || byte == '*' || byte == '+' || byte == '-' ||
          byte == '.' || byte == '/') {
        escape_sequence_buffer_.push_back(static_cast<char>(byte));
        escape_mode_ = EscapeMode::CharsetDesignate;
        continue;
      }
      // DCS (P), SOS (X), PM (^), APC (_): consume the string payload up to its
      // String Terminator instead of letting it fall through and print as text.
      if (byte == 'P' || byte == 'X' || byte == '^' || byte == '_') {
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::StringPayload;
        osc_escape_pending_ = false;
        continue;
      }
      if (byte == '7') {
        SaveCursorLocked();
      } else if (byte == '8') {
        RestoreCursorLocked();
      } else if (byte == 'H') {
        // HTS — set a horizontal tab stop at the cursor column.
        if (tab_stops_.empty()) {
          ResetTabStopsLocked();
        }
        if (cursor_column_ < tab_stops_.size()) {
          tab_stops_[cursor_column_] = true;
        }
      } else if (byte == 'Z') {
        SendBytesLocked("\x1b[?1;2c");
      } else if (byte == 'D' || byte == 'E') {
        // IND (ESC D) moves down one row PRESERVING the column; only NEL (ESC E)
        // resets to column 0. AdvanceCursorRowLocked unconditionally zeroes the
        // column (correct for LF/wrap), so restore it for IND. Unlike a bare LF,
        // IND is not subject to the PTY's ONLCR translation, so a program using it
        // to move down while holding its column must land at that column.
        const std::size_t saved_column = cursor_column_;
        AdvanceCursorRowLocked();
        if (byte == 'D') {
          cursor_column_ = saved_column;
        } else {
          cursor_column_ = 0;
        }
      } else if (byte == 'M') {
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
      escape_sequence_buffer_.clear();
      escape_mode_ = EscapeMode::None;
      continue;
    }

    if (escape_mode_ == EscapeMode::CharsetDesignate) {
      // ECMA-48: ESC/CAN/SUB appearing where the charset final byte is expected
      // cancel the designation rather than being consumed as the final. ESC begins
      // a fresh escape (`ESC ( ESC[2J` must run the CSI, not eat the ESC as the
      // designator), CAN/SUB abort to ground. Mirrors the CSI/OSC abort handling.
      if (byte == 0x1b) {
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::AfterEscape;
        osc_escape_pending_ = false;
        continue;
      }
      if (byte == 0x18 || byte == 0x1a) {
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::None;
        continue;
      }
      escape_sequence_buffer_.clear();
      escape_mode_ = EscapeMode::None;
      continue;
    }

    if (escape_mode_ == EscapeMode::Csi) {
      // ECMA-48: ESC/CAN/SUB appearing mid-sequence cancel the control sequence.
      // ESC starts a fresh escape (e.g. `\x1b[\x1b[2J` must run the second CSI, not
      // dispatch a garbage sequence ending in the second '['); CAN/SUB abort to
      // ground. Without this the intervening byte was swallowed as a parameter and
      // the next final byte dispatched a corrupt sequence.
      if (byte == 0x1b) {
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::AfterEscape;
        osc_escape_pending_ = false;
        continue;
      }
      if (byte == 0x18 || byte == 0x1a) {
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::None;
        continue;
      }
      // ECMA-48: a C0 control other than ESC/CAN/SUB embedded mid-CSI is EXECUTED
      // immediately and the control sequence then continues. CSI parameter (0x20..
      // 0x3F), intermediate, and final (0x40..0x7E) bytes are all >= 0x20, so any
      // byte < 0x20 here is unambiguously a C0 to run, not sequence content. The
      // previous code buffered it as a parameter byte and corrupted the dispatch.
      if (byte < 0x20) {
        switch (byte) {
          case '\r':
            cursor_column_ = 0;
            break;
          case '\n':
            AdvanceCursorRowLocked();
            break;
          case '\v':
          case '\f': {
            const std::size_t saved_column = cursor_column_;
            AdvanceCursorRowLocked();
            cursor_column_ = saved_column;
            break;
          }
          case '\b':
            if (cursor_column_ > 0) {
              --cursor_column_;
            }
            break;
          case '\t':
            cursor_column_ = NextTabStopLocked(cursor_column_);
            break;
          default:
            // BEL and the remaining C0 controls have no positional effect here.
            break;
        }
        continue;
      }
      escape_sequence_buffer_.push_back(static_cast<char>(byte));
      if (byte >= '@' && byte <= '~') {
        HandleEscapeSequenceLocked(escape_sequence_buffer_);
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::None;
      } else if (escape_sequence_buffer_.size() > kMaxEscapeSequenceLength) {
        AbandonEscapeSequenceLocked();
      }
      continue;
    }

    if (escape_mode_ == EscapeMode::Osc) {
      // CAN/SUB abort the control string (ESC is handled below as the ST lead-in).
      if (byte == 0x18 || byte == 0x1a) {
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::None;
        osc_escape_pending_ = false;
        continue;
      }
      if (osc_escape_pending_) {
        osc_escape_pending_ = false;
        if (byte == '\\') {
          HandleOscSequenceLocked(escape_sequence_buffer_);
          escape_sequence_buffer_.clear();
          escape_mode_ = EscapeMode::None;
          continue;
        }
        // ECMA-48: an ESC not forming ST (`ESC \`) terminates the control string
        // AND begins a new escape sequence. Dispatch the OSC, then reprocess this
        // byte as the first byte after ESC — otherwise a title followed by a real
        // sequence (`\e]0;t\e[0m`) would swallow the `\e[0m` into the OSC payload.
        HandleOscSequenceLocked(escape_sequence_buffer_);
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::AfterEscape;
        --data_index;
        continue;
      }
      if (byte == '\a') {
        HandleOscSequenceLocked(escape_sequence_buffer_);
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::None;
        continue;
      }
      if (byte == '\x1b') {
        osc_escape_pending_ = true;
        continue;
      }
      if (escape_sequence_buffer_.size() > kMaxEscapeSequenceLength) {
        AbandonEscapeSequenceLocked();
        continue;
      }
      escape_sequence_buffer_.push_back(static_cast<char>(byte));
      continue;
    }

    if (escape_mode_ == EscapeMode::StringPayload) {
      // DCS/SOS/PM/APC payload: discard everything up to the ST (ESC \) or BEL.
      // CAN/SUB abort the control string early (ESC is the ST lead-in, below).
      if (byte == 0x18 || byte == 0x1a) {
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::None;
        osc_escape_pending_ = false;
        continue;
      }
      if (osc_escape_pending_) {
        osc_escape_pending_ = false;
        if (byte == '\\') {
          escape_sequence_buffer_.clear();
          escape_mode_ = EscapeMode::None;
          continue;
        }
        // ESC not forming ST terminates the payload AND begins a new escape:
        // drop the (discarded) payload and reprocess this byte after ESC.
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::AfterEscape;
        --data_index;
        continue;
      }
      if (byte == '\a') {
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::None;
        continue;
      }
      if (byte == '\x1b') {
        osc_escape_pending_ = true;
        continue;
      }
      // Count discarded bytes via the buffer so an unterminated payload can't
      // trap the parser forever.
      if (escape_sequence_buffer_.size() > kMaxEscapeSequenceLength) {
        AbandonEscapeSequenceLocked();
        continue;
      }
      escape_sequence_buffer_.push_back(static_cast<char>(byte));
      continue;
    }

    if (byte == '\x1b') {
      if (!pending_utf8_sequence_.empty()) {
        PutGlyphLocked(util::kUtf8ReplacementChar);
        pending_utf8_sequence_.clear();
      }
      escape_mode_ = EscapeMode::AfterEscape;
      escape_sequence_buffer_.clear();
      osc_escape_pending_ = false;
      continue;
    }

    if (!pending_utf8_sequence_.empty() && byte < 0x80) {
      PutGlyphLocked(util::kUtf8ReplacementChar);
      pending_utf8_sequence_.clear();
    }

    switch (byte) {
      case '\r':
        cursor_column_ = 0;
        break;
      case '\n':
        AdvanceCursorRowLocked();
        break;
      case '\v':  // VT (0x0B)
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
        if (cursor_column_ > 0) {
          --cursor_column_;
        }
        break;
      case 0x7f:
        // DEL received in the output stream is ignored (ECMA-48 / xterm / VTE).
        // The previous destructive delete-and-shift corrupted the display when a
        // stray 0x7f appeared in child output (e.g. `printf '\x7f'` after text).
        break;
      case '\t': {
        // HT is pure cursor motion to the next tab stop. It must NOT write
        // spaces over the cells it passes: real terminals (xterm/VTE) leave the
        // underlying glyphs intact, so `ABCDEFGHIJ\r\tX` yields `ABCDEFGHXJ`, not
        // `        XJ`. A later write past the current line length pads the gap
        // with default-style blanks via ResizeLineLocked, so no fill is needed here.
        cursor_column_ = NextTabStopLocked(cursor_column_);
        break;
      }
      default:
        if (byte >= 32 || byte >= 0x80) {
          if (byte < 0x80) {
            PutCharacterLocked(static_cast<char>(byte));
            break;
          }

          while (true) {
            if (pending_utf8_sequence_.empty()) {
              const std::size_t sequence_length = util::Utf8SequenceLength(byte);
              if (sequence_length == 0) {
                PutGlyphLocked(util::kUtf8ReplacementChar);
                break;
              }
              pending_utf8_sequence_.push_back(static_cast<char>(byte));
              if (sequence_length == 1) {
                PutGlyphLocked(pending_utf8_sequence_);
                pending_utf8_sequence_.clear();
              }
              break;
            }

            if (!util::IsUtf8ContinuationByte(byte)) {
              PutGlyphLocked(util::kUtf8ReplacementChar);
              pending_utf8_sequence_.clear();
              continue;
            }

            pending_utf8_sequence_.push_back(static_cast<char>(byte));
            const std::size_t sequence_length =
                util::Utf8SequenceLength(
                    static_cast<unsigned char>(pending_utf8_sequence_.front()));
            if (sequence_length != 0 && pending_utf8_sequence_.size() >= sequence_length) {
              PutGlyphLocked(pending_utf8_sequence_);
              pending_utf8_sequence_.clear();
            }
            break;
          }
        }
        break;
    }
  }

  TrimScrollbackLocked();
  AdvanceSnapshotGenerationLocked();
}
}  // namespace microide::terminal
