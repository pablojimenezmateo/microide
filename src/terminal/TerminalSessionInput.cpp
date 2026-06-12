#include "terminal/TerminalSession.h"

#include "terminal/TerminalInternalConstants.h"
#include "terminal/TerminalSessionInputEncoding.h"
#include "platform/TerminalBackend.h"

namespace microide::terminal {

void TerminalSession::SendBytesLocked(std::string_view bytes) {
  if (bytes.empty()) {
    return;
  }

#ifdef MICROIDE_TESTING
  if (!backend_ || !backend_->running()) {
    test_sent_bytes_.append(bytes);
    return;
  }
#endif

  if (!backend_) {
    return;
  }
  backend_->Write(bytes);
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
#ifdef MICROIDE_TESTING
    const bool can_encode = (backend_ && backend_->running()) || running_;
#else
    const bool can_encode = backend_ && backend_->running();
#endif
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
#ifdef MICROIDE_TESTING
    const bool can_encode = (backend_ && backend_->running()) || running_;
#else
    const bool can_encode = backend_ && backend_->running();
#endif
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
