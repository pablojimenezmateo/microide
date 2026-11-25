#include "terminal/TerminalSession.h"

#include "terminal/TerminalInternalConstants.h"
#include "terminal/TerminalSessionInputEncoding.h"
#include "platform/TerminalBackend.h"
#include "util/StringUtil.h"

namespace microide::terminal {

void TerminalSession::SendBytesLocked(std::string_view bytes) {
  if (bytes.empty()) {
    return;
  }

  constexpr std::size_t kMaxPendingReplyBytes = 64 * 1024;
  if (pending_reply_.size() >= kMaxPendingReplyBytes) {
    return;
  }
  const std::size_t room = kMaxPendingReplyBytes - pending_reply_.size();
  pending_reply_.append(bytes.substr(0, room));
}

void TerminalSession::FlushPendingReply() {
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    if (pending_reply_.empty()) {
      return;
    }
    bytes.swap(pending_reply_);
  }
  SendBytes(bytes);
}

bool TerminalSession::SendKeyPress(const KeyPress& press) {
  if (SendToHost(TerminalInputEvent{.kind = TerminalInputEvent::Kind::Key, .key = press})) {
    return true;
  }
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    bytes = FormatTerminalKeyPress(application_cursor_keys_mode_, kitty_keyboard_flags_, press);
  }
  if (bytes.empty()) {
    return false;
  }
  SendBytes(bytes);
  return true;
}

void TerminalSession::PasteText(std::string_view text) {
  if (text.empty()) {
    return;
  }

  constexpr std::size_t kMaxTerminalPasteBytes = 64u << 20;
  if (text.size() > kMaxTerminalPasteBytes) {
    text = text.substr(0, util::PreviousUtf8Boundary(text, kMaxTerminalPasteBytes));
  }
  if (SendToHost(TerminalInputEvent{.kind = TerminalInputEvent::Kind::Paste,
                                    .text = std::string(text)})) {
    return;
  }

  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    bytes = FormatTerminalPasteBytes(bracketed_paste_mode_, text);
  }
  SendBytes(bytes);
}

bool TerminalSession::SendMouseToHost(TerminalInputEvent::Kind kind, MouseButton button,
                                      bool pressed, std::size_t row, std::size_t column,
                                      util::KeyModifiers modifiers) {
  {
    std::scoped_lock lock(mutex_);
    if (host_.channel == nullptr) {
      return false;
    }
    const TerminalMouseTrackingMode mode = CurrentTerminalMouseTrackingMode(
        mouse_.any, mouse_.drag, mouse_.normal);
    if (mode == TerminalMouseTrackingMode::Disabled ||
        (kind == TerminalInputEvent::Kind::MouseMotion && mode == TerminalMouseTrackingMode::Normal)) {
      return true;
    }
  }
  SendToHost(TerminalInputEvent{.kind = kind,
                                .button = button,
                                .pressed = pressed,
                                .row = static_cast<std::uint32_t>(row),
                                .column = static_cast<std::uint32_t>(column),
                                .modifiers = modifiers});
  return true;
}

bool TerminalSession::SendMouseButton(MouseButton button,
                                      bool pressed,
                                      std::size_t row,
                                      std::size_t column,
                                      util::KeyModifiers modifiers) {
  if (SendMouseToHost(TerminalInputEvent::Kind::MouseButton, button, pressed, row, column, modifiers)) {
    return true;
  }
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    const bool can_encode = (backend_ && backend_->running()) ||
                            (UsePlaceholderTerminalsForTesting() && running_);
    if (!EncodeTerminalSessionMouseEvent(mouse_.any, mouse_.drag,
                                         mouse_.normal, mouse_.sgr, rows_,
                                         columns_, button, pressed, false, row, column, modifiers,
                                         can_encode, bytes)) {
      return false;
    }
  }
  SendBytes(bytes);
  return true;
}

TerminalPointerShape TerminalSession::PointerShapeAt(std::size_t row, std::size_t column) const {
  std::scoped_lock lock(mutex_);
  if (mouse_.pointer_shape != TerminalPointerShape::None) {
    return mouse_.pointer_shape;
  }
  if (CurrentTerminalMouseTrackingMode(mouse_.any, mouse_.drag, mouse_.normal) == TerminalMouseTrackingMode::Disabled) {
    return TerminalPointerShape::None;
  }
  const std::size_t line = PrimaryScreenTopLocked() + row;
  return mouse_.any && row < rows_ && line < lines_.size() && pointer_.hover.Contains(row, column, lines_[line])
             ? TerminalPointerShape::Pointer
             : TerminalPointerShape::Default;
}

bool TerminalSession::SendMouseMotion(MouseButton button,
                                      std::size_t row,
                                      std::size_t column,
                                      util::KeyModifiers modifiers) {
  {
    std::scoped_lock lock(mutex_);
    const TerminalMotionCell cell{.row = row, .column = column, .button = button};
    if (pointer_.last_motion == cell) {
      return false;
    }
    pointer_.last_motion = cell;
    if (mouse_.any && button == MouseButton::None) {
      const std::size_t line = PrimaryScreenTopLocked() + row;
      if (row < rows_ && line < lines_.size()) {
        pointer_.hover.Probe(row, column, lines_[line]);
      }
    }
  }
  if (SendMouseToHost(TerminalInputEvent::Kind::MouseMotion, button, true, row, column, modifiers)) {
    return true;
  }
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    const bool can_encode = (backend_ && backend_->running()) ||
                            (UsePlaceholderTerminalsForTesting() && running_);
    if (!EncodeTerminalSessionMouseEvent(mouse_.any, mouse_.drag,
                                         mouse_.normal, mouse_.sgr, rows_,
                                         columns_, button, true, true, row, column, modifiers,
                                         can_encode, bytes)) {
      return false;
    }
  }
  SendBytes(bytes);
  return true;
}

}
