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

constexpr std::size_t kMaxEscapeSequenceLength = 8192;

}

void TerminalSession::AbandonEscapeSequenceLocked() {
  util::AddPerformanceCounter(util::PerfCounterId::TerminalEscapeSequencesAborted);
  escape_sequence_buffer_.clear();
  escape_mode_ = EscapeMode::None;
  osc_escape_pending_ = false;
}

void TerminalSession::EmitProcessExitMarkerLocked() {
  AbandonEscapeSequenceLocked();
  pending_utf8_sequence_.clear();
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

constexpr bool IsCanOrSub(unsigned char byte) { return byte == 0x18 || byte == 0x1a; }

}

void TerminalSession::ExecuteC0Locked(unsigned char byte) {
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
      ClampPendingWrapColumn(cursor_column_, columns_);
      if (cursor_column_ > 0) {
        --cursor_column_;
      }
      break;
    case '\a':
      pending_bell_ = true;
      pending_host_bell_ = true;
      break;
    case 0x0e:
      charsets_.shifted_out = true;
      break;
    case 0x0f:
      charsets_.shifted_out = false;
      break;
    case '\t': {
      cursor_column_ = NextTabStopLocked(cursor_column_);
      break;
    }
    default:
      break;
  }
}

void TerminalSession::AppendOutputLocked(std::string_view data) {
  util::PerformanceTrace::Scope perf_scope("terminal::AppendOutput");
  util::AddPerformanceCounter(util::PerfCounterId::TerminalOutputBytesParsed, data.size());
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }

  for (std::size_t data_index = 0; data_index < data.size(); ++data_index) {
    const unsigned char byte = static_cast<unsigned char>(data[data_index]);
    switch (escape_mode_) {
      case EscapeMode::None:
        if (byte >= 0x20 && byte < 0x7F && pending_utf8_sequence_.empty()) {
          if (const std::size_t consumed = PutAsciiRunLocked(data.substr(data_index));
              consumed > 0) {
            data_index += consumed - 1;
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
        if (ConsumeControlStringByteLocked(byte) == ControlStringByte::Reprocess) {
          --data_index;
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
      return;
    default:
      break;
  }
  if (byte < 0x20) {
    return;
  }
  if (byte < 0x80) {
    const char glyph = static_cast<char>(byte);
    PutGlyphLocked(std::string_view(&glyph, 1));
    return;
  }
  ConsumeUtf8ByteLocked(byte);
}

void TerminalSession::ConsumeUtf8ByteLocked(unsigned char byte) {
  if (!pending_utf8_sequence_.empty() && !util::IsUtf8ContinuationByte(byte)) {
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
      EnterEscapeModeLocked(EscapeMode::StringPayload);
      return;
    case '7':
      SaveCursorLocked();
      break;
    case '8':
      RestoreCursorLocked();
      break;
    case 'H':
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
    const std::size_t screen_top = PrimaryScreenTopLocked();
    const std::size_t rel_top = ActiveScrollRegionTopLocked();
    const std::size_t rel_bottom = ActiveScrollRegionBottomLocked();
    if (cursor_row_ == screen_top + rel_top) {
      ScrollRegionDownLocked(rel_top, rel_bottom, 1);
    } else if (cursor_row_ > screen_top) {
      --cursor_row_;
    }
  } else {
    const std::size_t floor_row = PrimaryScreenTopLocked();
    cursor_row_ = cursor_row_ > floor_row ? cursor_row_ - 1 : floor_row;
  }
}

void TerminalSession::ConsumeCharsetDesignateByteLocked(unsigned char byte) {
  if (byte == 0x1b) {
    EnterEscapeModeLocked(EscapeMode::AfterEscape);
    return;
  }
  if (IsCanOrSub(byte)) {
    EnterEscapeModeLocked(EscapeMode::None);
    return;
  }
  if (byte >= 0x20 && byte <= 0x2f) {
    if (escape_sequence_buffer_.size() > kMaxEscapeSequenceLength) {
      AbandonEscapeSequenceLocked();
      return;
    }
    escape_sequence_buffer_.push_back(static_cast<char>(byte));
    return;
  }
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
  if (byte == 0x1b) {
    EnterEscapeModeLocked(EscapeMode::AfterEscape);
    return;
  }
  if (IsCanOrSub(byte)) {
    EnterEscapeModeLocked(EscapeMode::None);
    return;
  }
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

TerminalSession::ControlStringByte TerminalSession::ConsumeControlStringByteLocked(
    unsigned char byte) {
  const bool osc = escape_mode_ == EscapeMode::Osc;
  const auto finish = [this, osc](EscapeMode next) {
    if (osc) {
      HandleOscSequenceLocked(escape_sequence_buffer_);
    }
    EnterEscapeModeLocked(next);
  };
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
  if (escape_sequence_buffer_.size() > kMaxEscapeSequenceLength) {
    if (osc && std::string_view(escape_sequence_buffer_).starts_with("]52;")) {
      oversized_osc52_dropped_ = true;
    }
    AbandonEscapeSequenceLocked();
    return ControlStringByte::Consumed;
  }
  escape_sequence_buffer_.push_back(static_cast<char>(byte));
  return ControlStringByte::Consumed;
}

}
