#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace microide::tests {
struct TerminalSessionTestAccess;
}

namespace microide::terminal {

struct TerminalStyle {
  std::optional<SDL_Color> foreground;
  std::optional<SDL_Color> background;
  bool bold = false;
};

struct TerminalCell {
  char character = '\0';
  TerminalStyle style;
};

struct TerminalLine {
  std::vector<TerminalCell> cells;
};

class TerminalSession {
 public:
  enum class Key {
    Escape,
    Enter,
    Backspace,
    Tab,
    Up,
    Down,
    Right,
    Left,
    Home,
    End,
    PageUp,
    PageDown,
    Insert,
    Delete,
  };

  enum class MouseButton {
    Left,
    Middle,
    Right,
    None,
    WheelUp,
    WheelDown,
  };

  TerminalSession() = default;
  ~TerminalSession();

  TerminalSession(const TerminalSession&) = delete;
  TerminalSession& operator=(const TerminalSession&) = delete;

  void SetWakeEventType(Uint32 event_type);
  bool Start(const std::filesystem::path& working_directory, std::string_view command = {});
  void Stop();
  void Resize(std::size_t rows, std::size_t columns);
  void SendBytes(std::string_view bytes);
  void SendKey(Key key);
  bool running() const;
  std::size_t LineCount() const;
  std::vector<TerminalLine> SnapshotLines() const;
  std::string LaunchLabel() const;
  std::size_t rows() const;
  std::size_t columns() const;
  std::size_t cursor_row() const;
  std::size_t cursor_column() const;
  bool cursor_visible() const;
  bool WantsMouseCapture() const;
  bool WantsMouseMotionCapture(bool buttons_down) const;
  void PasteText(std::string_view text);
  bool SendMouseButton(MouseButton button,
                       bool pressed,
                       std::size_t row,
                       std::size_t column,
                       SDL_Keymod modifiers);
  bool SendMouseMotion(MouseButton button,
                       std::size_t row,
                       std::size_t column,
                       SDL_Keymod modifiers);

 private:
  struct ScreenState {
    std::vector<TerminalLine> lines = {TerminalLine{}};
    std::size_t cursor_row = 0;
    std::size_t cursor_column = 0;
    std::size_t saved_cursor_row = 0;
    std::size_t saved_cursor_column = 0;
    std::size_t scroll_region_top = 0;
    std::size_t scroll_region_bottom = 0;
  };

  enum class EscapeMode {
    None,
    AfterEscape,
    Csi,
    Osc,
  };

  enum class MouseTrackingMode {
    Disabled,
    Normal,
    Drag,
    Any,
  };

  void ReaderMain(int master_fd, int child_pid);
  void AppendOutputLocked(std::string_view data);
  void HandleEscapeSequenceLocked(std::string_view sequence);
  void HandlePrivateModeLocked(int mode, bool enabled);
  MouseTrackingMode CurrentMouseTrackingModeLocked() const;
  std::string FormatKeyBytesLocked(Key key) const;
  std::string FormatPasteBytesLocked(std::string_view text) const;
  bool EncodeMouseEventLocked(MouseButton button,
                              bool pressed,
                              bool motion,
                              std::size_t row,
                              std::size_t column,
                              SDL_Keymod modifiers,
                              std::string& out_bytes) const;
  void EnsureCursorLineExistsLocked();
  void AdvanceCursorRowLocked();
  void MoveCursorLocked(std::size_t row, std::size_t column);
  void PutCharacterLocked(char character);
  void ResizeLineLocked(TerminalLine& line, std::size_t size);
  void ClearLineRangeLocked(TerminalLine& line, std::size_t start, std::size_t end);
  void EraseInLineLocked(int mode);
  void EraseInDisplayLocked(int mode);
  void ResetScrollRegionLocked();
  void ClampScrollRegionLocked();
  std::size_t ActiveScrollRegionTopLocked() const;
  std::size_t ActiveScrollRegionBottomLocked() const;
  void ScrollRegionUpLocked(std::size_t top, std::size_t bottom, std::size_t count);
  void ScrollRegionDownLocked(std::size_t top, std::size_t bottom, std::size_t count);
  void SaveCursorLocked();
  void RestoreCursorLocked();
  void SaveActiveScreenLocked();
  void RestoreSavedScreenLocked();
  void ResetScreenLocked(bool fill_rows);
  void SetAlternateScreenLocked(bool enabled, bool clear);
  void TrimScrollbackLocked();
  void PushWakeEvent() const;

  mutable std::mutex mutex_;
  std::thread reader_thread_;
  std::vector<TerminalLine> lines_ = {TerminalLine{}};
  ScreenState primary_screen_;
  ScreenState alternate_screen_;
  std::filesystem::path working_directory_;
  std::string launch_label_;
  TerminalStyle current_style_;
  std::string escape_sequence_buffer_;
  Uint32 wake_event_type_ = 0;
  int master_fd_ = -1;
  int child_pid_ = -1;
  bool running_ = false;
  bool stop_requested_ = false;
  EscapeMode escape_mode_ = EscapeMode::None;
  bool osc_escape_pending_ = false;
  bool use_alternate_screen_ = false;
  bool mouse_tracking_normal_ = false;
  bool mouse_tracking_drag_ = false;
  bool mouse_tracking_any_ = false;
  bool mouse_sgr_ext_mode_ = false;
  bool application_cursor_keys_mode_ = false;
  bool bracketed_paste_mode_ = false;
  bool cursor_visible_ = true;
  std::size_t rows_ = 24;
  std::size_t columns_ = 80;
  std::size_t cursor_row_ = 0;
  std::size_t cursor_column_ = 0;
  std::size_t saved_cursor_row_ = 0;
  std::size_t saved_cursor_column_ = 0;
  std::size_t scroll_region_top_ = 0;
  std::size_t scroll_region_bottom_ = 23;

#ifdef MICROIDE_TESTING
  std::string test_sent_bytes_;
#endif

#ifdef MICROIDE_TESTING
  friend struct ::microide::tests::TerminalSessionTestAccess;
#endif
};

}  // namespace microide::terminal
