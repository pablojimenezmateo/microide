#pragma once

#include <SDL3/SDL.h>

#include "platform/TerminalBackend.h"
#include "terminal/TerminalCell.h"

#include <algorithm>
#include <cstdint>
#include <deque>
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

// Process-wide test switch: when enabled, terminal startup uses in-process
// placeholders instead of spawning real PTY-backed child processes, and
// SendBytes captures output to TerminalSession::test_sent_bytes_ when no backend
// is attached. Defaults to false, so production behavior is unchanged; the test
// harness enables it once at startup. Replaces the former compile-time test
// forks so core compiles identically for the production and test binaries.
void SetUsePlaceholderTerminalsForTesting(bool enabled);
bool UsePlaceholderTerminalsForTesting();

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

  enum class CursorShape {
    Block,
    Underline,
    Bar,
  };

  // A physical key press carrying its logical key plus active modifiers. The
  // session encodes it per the negotiated keyboard protocol (legacy xterm or
  // the Kitty keyboard protocol when an application has enabled it).
  struct KeyPress {
    enum class Key {
      Char,
      Enter,
      Escape,
      Backspace,
      Tab,
      Up,
      Down,
      Left,
      Right,
      Home,
      End,
      PageUp,
      PageDown,
      Insert,
      Delete,
      F1,
      F2,
      F3,
      F4,
      F5,
      F6,
      F7,
      F8,
      F9,
      F10,
      F11,
      F12,
    };
    Key key = Key::Char;
    char32_t codepoint = 0;  // Base-layout codepoint for Key::Char.
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
    bool super = false;
  };

  TerminalSession() = default;
  ~TerminalSession();

  TerminalSession(const TerminalSession&) = delete;
  TerminalSession& operator=(const TerminalSession&) = delete;

  void SetWakeEventType(Uint32 event_type);
  bool Start(const std::filesystem::path& working_directory, std::string_view command = {},
             std::string_view shell = {});
  void Stop();
  // Test seam: brings the session up without spawning a real PTY/child process.
  // Always compiled; selected at runtime when placeholder-terminal test mode is
  // enabled (see SetUsePlaceholderTerminalsForTesting). Inert in production.
  bool StartPlaceholderForTesting(const std::filesystem::path& working_directory,
                                  std::string_view command = {});
  void Resize(std::size_t rows, std::size_t columns);
  // Configure the scrollback cap (lines retained above the visible grid). Clamped
  // to a sane floor; re-trims immediately when lowered below the current backlog.
  void SetMaxScrollbackLines(std::size_t max_lines);
  void SendBytes(std::string_view bytes);
  void SendKey(Key key);
  // Encode and send a modified key press. Returns true if it produced output.
  bool SendKeyPress(const KeyPress& press);
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
  CursorShape cursor_shape() const;
  bool cursor_blinking() const;
  // True while the application is mid-frame under synchronized output (DEC mode
  // 2026). The host coalesces redraws across the frame to avoid tearing.
  bool synchronized_output_active() const;
  // Current working directory advertised by the shell via OSC 7 (empty if none).
  std::filesystem::path reported_working_directory() const;
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
    std::deque<TerminalLine> lines = {TerminalLine{}};
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
    // DCS (ESC P), SOS (ESC X), PM (ESC ^), APC (ESC _): string payloads that
    // run until a String Terminator (ESC \) or BEL. We do not implement these
    // (Sixel, Kitty graphics, tmux passthrough, DECRQSS, ...), but their
    // payloads must be consumed and discarded rather than printed to the grid.
    StringPayload,
  };

  void AppendOutputLocked(std::string_view data);
  // Append the "[process exited]" marker on a fresh line. Resets any dangling
  // escape-parser state first so a child that died mid-sequence cannot swallow
  // the marker. Caller must hold mutex_.
  void EmitProcessExitMarkerLocked();
  // Reset escape-parsing state after an over-length / malformed sequence so the
  // stream recovers to normal text instead of growing the buffer unbounded.
  void AbandonEscapeSequenceLocked();
  void HandleEscapeSequenceLocked(std::string_view sequence);
  void HandleOscSequenceLocked(std::string_view sequence);
  void HandleKittyKeyboardLocked(char prefix, char final, const std::vector<int>& params);
  void HandlePrivateModeLocked(int mode, bool enabled);
  int QueryPrivateModeStateLocked(int mode) const;
  bool ConsumeWakeDecisionLocked();
  void ResetTabStopsLocked();
  std::size_t NextTabStopLocked(std::size_t column) const;
  std::size_t PreviousTabStopLocked(std::size_t column) const;
  void SendBytesLocked(std::string_view bytes);
  void FlushPendingReply();
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
  void SaveActiveScreenMetadataLocked(ScreenState& screen);
  void SaveActiveScreenLocked();
  void RestoreSavedScreenLocked();
  void ResetScreenLocked(bool fill_rows);
  void SetAlternateScreenLocked(bool enabled, bool clear);
  void TrimScrollbackLocked();
  void AdvanceSnapshotGenerationLocked();
  bool ReserveWakeEvent(Uint32& event_type) const;
  void PushWakeEvent() const;

  mutable std::mutex mutex_;
  // `std::deque` so the scrollback trim `pop_front`-equivalent erase is
  // amortized O(1) per element with no tail moves — `std::vector::erase(begin)`
  // moved up to ~2 000 tail elements per trim under heavy output
  // (round-4 Finding 3). Random access (`lines_[i]`) and iterator-based
  // algorithms (`std::rotate`, `assign`, snapshot copy) still work.
  std::deque<TerminalLine> lines_ = {TerminalLine{}};
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
  bool synchronized_output_ = false;
  int sync_suppressed_wakes_ = 0;
  // Kitty keyboard protocol progressive-enhancement flags (0 = legacy mode) and
  // the push/pop stack maintained by `CSI > flags u` / `CSI < n u`.
  std::uint8_t kitty_keyboard_flags_ = 0;
  std::vector<std::uint8_t> kitty_keyboard_stack_;
  CursorShape cursor_shape_ = CursorShape::Block;
  bool cursor_blinking_ = true;
  // Per-column horizontal tab stops. Rebuilt to the default 8-column grid on
  // resize; mutable via HTS (ESC H) / TBC (CSI g).
  std::vector<bool> tab_stops_;
  std::filesystem::path reported_working_directory_;
  std::optional<std::string> pending_clipboard_text_;
  std::string pending_utf8_sequence_;
  std::size_t rows_ = 24;
  std::size_t columns_ = 80;
  // Scrollback cap (the `terminal.scrollback_lines` setting; default mirrors
  // kMaxScrollbackLines in TerminalInternalConstants.h).
  std::size_t max_scrollback_lines_ = 2000;
  std::size_t cursor_row_ = 0;
  std::size_t cursor_column_ = 0;
  std::size_t saved_cursor_row_ = 0;
  std::size_t saved_cursor_column_ = 0;
  std::size_t scroll_region_top_ = 0;
  std::size_t scroll_region_bottom_ = 23;
  std::uint64_t snapshot_generation_ = 1;

  // Test seam: captures bytes written while no backend is attached (placeholder
  // test mode). Always present so the core ABI is identical for the production
  // and test binaries; never read in production.
  std::string test_sent_bytes_;

  // Replies to terminal queries (DSR/DA/DECRQM/color/focus) are accumulated here
  // under mutex_ by SendBytesLocked and flushed by FlushPendingReply() after the
  // lock is released, so the reader thread never blocks inside a PTY write()
  // while holding mutex_. Capped so a query-flooding, non-draining child cannot
  // grow it without bound.
  std::string pending_reply_;

  friend struct ::microide::tests::TerminalSessionTestAccess;
};

}  // namespace microide::terminal
