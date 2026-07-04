#include "terminal/TerminalSession.h"

#include "terminal/TerminalInternalConstants.h"
#include "terminal/TerminalSessionInputEncoding.h"
#include "platform/TerminalBackend.h"

namespace microide::terminal {

void TerminalSession::SendBytesLocked(std::string_view bytes) {
  if (bytes.empty()) {
    return;
  }

  // Buffer the reply rather than writing it here. SendBytesLocked runs under
  // mutex_ — on the reader thread via AppendOutputLocked for query replies
  // (DSR/DA/DECRQM/color/kitty), and on the UI thread for focus events. A direct
  // backend_->Write is a blocking write() to the PTY master, so a child that
  // floods queries (e.g. `\033[6n` in a loop) without draining its own stdin
  // would fill the input buffer and park the reader thread inside write() while
  // holding mutex_, freezing every UI-thread snapshot/cursor call. Instead
  // accumulate here and flush via FlushPendingReply() once the lock is released.
  // Cap the buffer so the flood cannot grow it without bound (also bounds the
  // DECRQM one-reply-per-mode amplification).
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
  // SendBytes performs the blocking PTY write() without holding mutex_ (and
  // routes to test_sent_bytes_ in placeholder test mode).
  SendBytes(bytes);
}

void TerminalSession::SendKey(Key key) {
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    bytes = FormatTerminalKeyBytes(application_cursor_keys_mode_, key);
  }
  SendBytes(bytes);
}

bool TerminalSession::SendKeyPress(const KeyPress& press) {
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

  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    bytes = FormatTerminalPasteBytes(bracketed_paste_mode_, text);
  }
  SendBytes(bytes);
}

bool TerminalSession::SendMouseButton(MouseButton button,
                                      bool pressed,
                                      std::size_t row,
                                      std::size_t column,
                                      SDL_Keymod modifiers) {
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    // Placeholder test mode has no backend, so it falls back to running_; in
    // production (flag off) this is exactly `backend_ && backend_->running()`.
    const bool can_encode = (backend_ && backend_->running()) ||
                            (UsePlaceholderTerminalsForTesting() && running_);
    if (!EncodeTerminalSessionMouseEvent(mouse_tracking_any_, mouse_tracking_drag_,
                                         mouse_tracking_normal_, mouse_sgr_ext_mode_, rows_,
                                         columns_, button, pressed, false, row, column, modifiers,
                                         can_encode, bytes)) {
      return false;
    }
  }
  SendBytes(bytes);
  return true;
}

bool TerminalSession::SendMouseMotion(MouseButton button,
                                      std::size_t row,
                                      std::size_t column,
                                      SDL_Keymod modifiers) {
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    // Placeholder test mode has no backend, so it falls back to running_; in
    // production (flag off) this is exactly `backend_ && backend_->running()`.
    const bool can_encode = (backend_ && backend_->running()) ||
                            (UsePlaceholderTerminalsForTesting() && running_);
    if (!EncodeTerminalSessionMouseEvent(mouse_tracking_any_, mouse_tracking_drag_,
                                         mouse_tracking_normal_, mouse_sgr_ext_mode_, rows_,
                                         columns_, button, true, true, row, column, modifiers,
                                         can_encode, bytes)) {
      return false;
    }
  }
  SendBytes(bytes);
  return true;
}

}  // namespace microide::terminal
