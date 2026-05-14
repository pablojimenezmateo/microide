#pragma once

#include <SDL3/SDL.h>

#include "platform/TerminalBackend.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
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
  bool inverse = false;
};

struct TerminalCell {
  char character = '\0';
  std::string text;
  TerminalStyle style;

  std::string_view DisplayText() const {
    if (!text.empty()) {
      return text;
    }
    return character == '\0' ? std::string_view{} : std::string_view(&character, 1);
  }
};

struct TerminalLine {
  std::vector<TerminalCell> cells;
  bool wrapped_from_previous = false;
};

struct TerminalCursorSnapshot {
  std::size_t row = 0;
  std::size_t column = 0;
  bool visible = true;
};

struct TerminalLineRangeSnapshot {
  std::uint64_t generation = 0;
  std::vector<TerminalLine> lines;
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
#ifdef MICROIDE_TESTING
  bool StartPlaceholderForTesting(const std::filesystem::path& working_directory,
                                  std::string_view command = {});
#endif
  void Resize(std::size_t rows, std::size_t columns);
  void SendBytes(std::string_view bytes);
  void SendKey(Key key);
  bool running() const;
  std::size_t LineCount() const;
  std::vector<TerminalLine> SnapshotLines() const;
  std::vector<TerminalLine> SnapshotLineRange(std::size_t start_row, std::size_t max_lines) const;
  const std::vector<TerminalLine>& SnapshotLineRangeCached(std::size_t start_row,
                                                           std::size_t max_lines) const;
  bool SnapshotLineRangeIfChanged(std::size_t start_row,
                                  std::size_t max_lines,
                                  std::uint64_t previous_generation,
                                  TerminalLineRangeSnapshot* snapshot) const;
  std::string LaunchLabel() const;
  std::size_t rows() const;
  std::size_t columns() const;
  std::size_t cursor_row() const;
  std::size_t cursor_column() const;
  bool cursor_visible() const;
  TerminalCursorSnapshot CursorSnapshot() const;
  bool using_alternate_screen() const;
  bool ConsumeWakeEvent();
  bool WantsMouseCapture() const;
  bool WantsMouseMotionCapture(bool buttons_down) const;
  bool WantsFocusEvents() const;
  std::optional<std::string> ConsumePendingClipboardText();
  void PasteText(std::string_view text);
  void SendFocusEvent(bool focused);
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
    CharsetDesignate,
  };

  enum class MouseTrackingMode {
    Disabled,
    Normal,
    Drag,
    Any,
  };

  void AppendOutputLocked(std::string_view data);
  void HandleEscapeSequenceLocked(std::string_view sequence);
  void HandleOscSequenceLocked(std::string_view sequence);
  void HandlePrivateModeLocked(int mode, bool enabled);
  void SendBytesLocked(std::string_view bytes);
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
  void AdvanceCursorRowLocked(bool wrapped_from_previous = false);
  void MoveCursorLocked(std::size_t row, std::size_t column);
  void PutCharacterLocked(char character);
  void PutGlyphLocked(std::string_view glyph);
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
  void AdvanceSnapshotGenerationLocked();
  bool ReserveWakeEvent(Uint32& event_type) const;
  void PushWakeEvent() const;

  mutable std::mutex mutex_;
  std::vector<TerminalLine> lines_ = {TerminalLine{}};
  ScreenState primary_screen_;
  ScreenState alternate_screen_;
  std::unique_ptr<platform::TerminalBackend> backend_;
  std::filesystem::path working_directory_;
  std::string default_launch_label_;
  std::string launch_label_;
  TerminalStyle current_style_;
  std::string escape_sequence_buffer_;
  Uint32 wake_event_type_ = 0;
  int child_pid_ = -1;
  bool running_ = false;
  bool stop_requested_ = false;
  mutable bool wake_event_pending_ = false;
  EscapeMode escape_mode_ = EscapeMode::None;
  bool osc_escape_pending_ = false;
  bool use_alternate_screen_ = false;
  bool mouse_tracking_normal_ = false;
  bool mouse_tracking_drag_ = false;
  bool mouse_tracking_any_ = false;
  bool mouse_sgr_ext_mode_ = false;
  bool application_cursor_keys_mode_ = false;
  bool origin_mode_ = false;
  bool auto_wrap_mode_ = true;
  bool bracketed_paste_mode_ = false;
  bool focus_event_mode_ = false;
  bool cursor_visible_ = true;
  std::optional<std::string> pending_clipboard_text_;
  std::string pending_utf8_sequence_;
  std::size_t rows_ = 24;
  std::size_t columns_ = 80;
  std::size_t cursor_row_ = 0;
  std::size_t cursor_column_ = 0;
  std::size_t saved_cursor_row_ = 0;
  std::size_t saved_cursor_column_ = 0;
  std::size_t scroll_region_top_ = 0;
  std::size_t scroll_region_bottom_ = 23;
  std::uint64_t snapshot_generation_ = 1;

#ifdef MICROIDE_TESTING
  std::string test_sent_bytes_;
#endif

#ifdef MICROIDE_TESTING
  friend struct ::microide::tests::TerminalSessionTestAccess;
#endif
};

}  // namespace microide::terminal
