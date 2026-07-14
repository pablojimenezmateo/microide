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
  // Monotonic count of scrollback lines dropped off the FRONT by TrimScrollbackLocked.
  // Workspace-side absolute row mirrors (scroll position, selection, last-command row)
  // rebase against the delta of this value so a coalesced trim does not shift the
  // content out from under them (session cursor rows are rebased in-place already).
  std::uint64_t ScrollbackTrimTotal() const;
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
  // True once (then reset) when an OSC 52 clipboard sequence was dropped because it
  // overran the escape-sequence buffer cap. Lets the host surface a status instead
  // of silently swallowing a too-large clipboard write.
  bool ConsumeOversizedOsc52Dropped();
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
    TerminalStyle saved_style = {};
    bool saved_origin_mode = false;
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
  void ResizeTabStopsLocked();
  std::size_t NextTabStopLocked(std::size_t column) const;
  std::size_t PreviousTabStopLocked(std::size_t column) const;
  void SendBytesLocked(std::string_view bytes);
  void FlushPendingReply();
  void EnsureCursorLineExistsLocked();
  void AdvanceCursorRowLocked(bool wrapped_from_previous = false);
  void MoveCursorLocked(std::size_t row, std::size_t column);
  // Absolute deque row of the top of the visible screen on the primary buffer.
  // The primary deque holds scrollback followed by the visible grid, so row-
  // addressing CSIs (CUP/HVP/VPA/DECSTBM home, CPR report) and full-display
  // erases must be relative to this row, not to the top of scrollback. Returns 0
  // on the alternate screen (which has no scrollback) and whenever the deque is
  // not yet taller than the screen.
  std::size_t PrimaryScreenTopLocked() const;
  void PutCharacterLocked(char character);
  void PutGlyphLocked(std::string_view glyph);
  // Blank any dangling half of a previously written double-width pair that a
  // fresh write over [start, start+advance) would orphan, so the renderer never
  // sees a lead without its trailing spacer (stale gap) or a spacer without its
  // lead (overlap).
  void BreakWideGlyphPairForWriteLocked(TerminalLine& line, std::size_t start,
                                        std::size_t advance);
  void ResizeLineLocked(TerminalLine& line, std::size_t size);
  void ClearLineRangeLocked(TerminalLine& line, std::size_t start, std::size_t end);
  // Blank a whole row honoring Background Color Erase (BCE): when the current SGR
  // paints a non-default background, fill full-width styled blanks so the erased
  // row keeps that background (matching EraseInLineLocked); otherwise keep the
  // cheap empty-line reset, which renders identically to the default background.
  void BlankLineToCurrentBackgroundLocked(TerminalLine& line);
  void EraseInLineLocked(int mode);
  void EraseInDisplayLocked(int mode);
  void ResetScrollRegionLocked();
  void ClampScrollRegionLocked();
  std::size_t ActiveScrollRegionTopLocked() const;
  std::size_t ActiveScrollRegionBottomLocked() const;
  // True when a DECSTBM region narrower than the full screen is in effect. Used
  // to gate primary-buffer scroll-region behavior so the common full-screen path
  // (infinite scrollback accumulation) is untouched.
  bool HasCustomScrollRegionLocked() const;
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
  // shared_ptr (not unique_ptr) so Resize()/SendBytes() can copy it under the lock
  // and keep the backend alive across the unlocked Write()/Resize() call even if a
  // concurrent Stop() moves it out — closing a use-after-free on the raw pointer.
  std::shared_ptr<platform::TerminalBackend> backend_;
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
  // Set when an oversized OSC 52 clipboard sequence is abandoned at the escape
  // buffer cap; drained by ConsumeOversizedOsc52Dropped so the host can notify.
  bool oversized_osc52_dropped_ = false;
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
  std::uint64_t scrollback_trim_total_ = 0;
  std::size_t cursor_row_ = 0;
  std::size_t cursor_column_ = 0;
  std::size_t saved_cursor_row_ = 0;
  std::size_t saved_cursor_column_ = 0;
  // DECSC (ESC 7 / CSI s) saves the graphic rendition and origin mode alongside the
  // cursor; DECRC (ESC 8 / CSI u) restores them. Without this an SGR change between
  // save and restore leaks past the restore (e.g. `\0337\033[31mRED\0338X` must
  // print X in the default color, not red).
  TerminalStyle saved_style_{};
  bool saved_origin_mode_ = false;
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
