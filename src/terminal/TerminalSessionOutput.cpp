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

  for (const unsigned char byte : data) {
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
        AdvanceCursorRowLocked();
        if (byte == 'E') {
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
        } else if (cursor_row_ > 0) {
          --cursor_row_;
        }
      }
      escape_sequence_buffer_.clear();
      escape_mode_ = EscapeMode::None;
      continue;
    }

    if (escape_mode_ == EscapeMode::CharsetDesignate) {
      escape_sequence_buffer_.clear();
      escape_mode_ = EscapeMode::None;
      continue;
    }

    if (escape_mode_ == EscapeMode::Csi) {
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
      if (osc_escape_pending_) {
        if (byte == '\\') {
          HandleOscSequenceLocked(escape_sequence_buffer_);
          escape_sequence_buffer_.clear();
          escape_mode_ = EscapeMode::None;
          osc_escape_pending_ = false;
          continue;
        }
        osc_escape_pending_ = false;
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
      if (osc_escape_pending_) {
        if (byte == '\\') {
          escape_sequence_buffer_.clear();
          escape_mode_ = EscapeMode::None;
          osc_escape_pending_ = false;
          continue;
        }
        osc_escape_pending_ = false;
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
      case '\v':  // VT (0x0B): ECMA-48 treats it as index (line feed), like xterm/VTE.
      case '\f':  // FF (0x0C): likewise index — a bare form feed moves down one row.
        AdvanceCursorRowLocked();
        break;
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
