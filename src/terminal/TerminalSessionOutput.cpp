#include "terminal/TerminalSession.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalCsiParser.h"
#include "terminal/TerminalInternalConstants.h"
#include "terminal/TerminalMouseEncoder.h"
#include "terminal/TerminalOscClipboard.h"
#include "terminal/TerminalProcessControl.h"
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

void TerminalSession::AppendOutputLocked(std::string_view data) {
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }

  for (const unsigned char byte : data) {
    if (escape_mode_ == EscapeMode::AfterEscape) {
      if (byte == '[') {
        escape_sequence_buffer_.assign(1, '[');
        escape_mode_ = EscapeMode::Csi;
        continue;
      }
      if (byte == ']') {
        escape_sequence_buffer_.assign(1, ']');
        escape_mode_ = EscapeMode::Osc;
        osc_escape_pending_ = false;
        continue;
      }
      if (byte == '(' || byte == ')' || byte == '*' || byte == '+' || byte == '-' ||
          byte == '.' || byte == '/') {
        escape_sequence_buffer_.assign(1, static_cast<char>(byte));
        escape_mode_ = EscapeMode::CharsetDesignate;
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
      escape_sequence_buffer_.push_back(static_cast<char>(byte));
      continue;
    }

    if (byte == '\x1b') {
      if (!pending_utf8_sequence_.empty()) {
        PutGlyphLocked("\xEF\xBF\xBD");
        pending_utf8_sequence_.clear();
      }
      escape_mode_ = EscapeMode::AfterEscape;
      escape_sequence_buffer_.clear();
      osc_escape_pending_ = false;
      continue;
    }

    if (!pending_utf8_sequence_.empty() && byte < 0x80) {
      PutGlyphLocked("\xEF\xBF\xBD");
      pending_utf8_sequence_.clear();
    }

    switch (byte) {
      case '\r':
        cursor_column_ = 0;
        break;
      case '\n':
        AdvanceCursorRowLocked();
        break;
      case '\b':
        if (cursor_column_ > 0) {
          --cursor_column_;
        }
        break;
      case 0x7f: {
        if (cursor_column_ > 0) {
          --cursor_column_;
        }
        EnsureCursorLineExistsLocked();
        auto& line = lines_[cursor_row_];
        if (cursor_column_ < line.cells.size()) {
          line.cells.erase(line.cells.begin() +
                           static_cast<std::ptrdiff_t>(cursor_column_));
        }
        break;
      }
      case '\t': {
        const std::size_t target = NextTabStopLocked(cursor_column_);
        while (cursor_column_ < target) {
          PutCharacterLocked(' ');
        }
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
                PutGlyphLocked("\xEF\xBF\xBD");
                break;
              }
              pending_utf8_sequence_.assign(1, static_cast<char>(byte));
              if (sequence_length == 1) {
                PutGlyphLocked(pending_utf8_sequence_);
                pending_utf8_sequence_.clear();
              }
              break;
            }

            if (!util::IsUtf8ContinuationByte(byte)) {
              PutGlyphLocked("\xEF\xBF\xBD");
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
