#pragma once

#include "util/Rgba8.h"
#include "platform/ProcessLauncher.h"
#include "platform/TerminalBackend.h"
#include "terminal/TerminalCell.h"
#include "terminal/TerminalHoverAffordance.h"
#include "terminal/TerminalCellExtraTable.h"
#include "terminal/TerminalHostChannel.h"
#include "terminal/TerminalHostHistory.h"
#include "terminal/TerminalHostMode.h"
#include "terminal/TerminalHostWire.h"
#include "terminal/TerminalInput.h"
#include "terminal/TerminalPredictionOverlay.h"
#include "terminal/TerminalLineBufferPool.h"
#include "terminal/TerminalScreenState.h"
#include "terminal/TerminalSearch.h"
#include "util/KeyModifiers.h"
#include "util/Revision.h"
#include "util/Waker.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
struct TerminalSessionTestAccess;
}

namespace microide::terminal {

void SetUsePlaceholderTerminalsForTesting(bool enabled);
bool UsePlaceholderTerminalsForTesting();

class TerminalSession {
 public:
  using MouseButton = TerminalMouseButton;
  using CursorShape = TerminalCursorShape;
  using KeyPress = TerminalKeyPress;

  TerminalSession() = default;
  ~TerminalSession();

  TerminalSession(const TerminalSession&) = delete;
  TerminalSession& operator=(const TerminalSession&) = delete;

  void SetWakeChannel(util::WakeChannel channel);
  bool Start(const platform::ProcessLauncher& launcher, const std::filesystem::path& working_directory,
             std::string_view command = {}, std::string_view shell = {}, HostIdentity attach = {});
  bool StartArgv(const platform::ProcessLauncher& launcher,
                 const std::filesystem::path& working_directory, std::string_view command,
                 std::vector<std::string> shell_argv, HostIdentity attach = {},
                 std::vector<std::string> environment = {});
  void Stop();
  bool StartPlaceholderForTesting(const std::filesystem::path& working_directory,
                                  std::string_view command = {});
  void Resize(std::size_t rows, std::size_t columns);
  void SetMaxScrollbackLines(std::size_t max_lines);
  void SendBytes(std::string_view bytes);
  bool SendKeyPress(const KeyPress& press);
  bool running() const;
  std::size_t LineCount() const;
  std::uint64_t ScrollbackTrimTotal() const;
  std::vector<TerminalLine> SnapshotLines() const;
  std::vector<TerminalLine> SnapshotLineRange(std::size_t start_row, std::size_t max_lines) const;
  void SnapshotLineRangeInto(std::size_t start_row,
                             std::size_t max_lines,
                             std::vector<TerminalLine>& out) const;
  bool SnapshotLineRangeIfChanged(std::size_t start_row, std::size_t max_lines, std::uint64_t previous_generation,
                                  TerminalLineRangeSnapshot* snapshot, TerminalCursorSnapshot* cursor = nullptr) const;
  struct SearchScan {
    std::size_t line_count = 0;
    std::size_t stable_row_end = 0;
    std::uint64_t trim_total = 0;
    bool full_rescan = false;
    bool truncated = false;
  };
  SearchScan FindMatches(const TerminalSearchQuery& query,
                         std::size_t start_row,
                         std::uint64_t expected_trim_total,
                         std::size_t max_matches,
                         TerminalSearchScratch& scratch,
                         std::vector<TerminalSearchMatch>& out) const;
  std::string LaunchLabel() const;
  std::size_t rows() const;
  std::size_t columns() const;
  std::size_t cursor_row() const;
  std::size_t cursor_column() const;
  bool screen_reverse_video() const;
  TerminalCursorSnapshot CursorSnapshot() const;
  bool using_alternate_screen() const;
  CursorShape cursor_shape() const;
  bool cursor_blinking() const;
  bool synchronized_output_active() const;
  std::filesystem::path reported_working_directory() const;
  int child_process_id() const { std::scoped_lock lock(mutex_); return child_pid_; }
  bool ConsumeWakeEvent();
  bool WantsMouseCapture() const;
  bool WantsMouseMotionCapture(bool buttons_down) const;
  bool WantsFocusEvents() const;
  std::optional<std::string> ConsumePendingClipboardText();
  void SetDefaultColors(util::Rgba8 foreground, util::Rgba8 background);
  bool ConsumeBell();
  struct Notification {
    std::string title;
    std::string body;
  };
  std::optional<Notification> ConsumeNotification();
  TerminalHostRequests ConsumeHostRequests();
  std::string LinkUri(std::uint16_t extra) const;
  bool ConsumeOversizedOsc52Dropped();
  void PasteText(std::string_view text);
  void SendFocusEvent(bool focused);
  bool SendMouseButton(MouseButton button,
                       bool pressed,
                       std::size_t row,
                       std::size_t column,
                       util::KeyModifiers modifiers);
  bool SendMouseMotion(MouseButton button,
                       std::size_t row,
                       std::size_t column,
                       util::KeyModifiers modifiers);
  TerminalPointerShape PointerShapeAt(std::size_t row, std::size_t column) const;

  bool is_host_terminal() const;

  bool host_pending() const;
  void SetPendingStatus(std::string_view text);
  bool StartPendingOnHost(const platform::ProcessLauncher& launcher);
  HostIdentity host_identity() const;
  std::uint64_t host_handle() const;
  HostReattach host_reattach() const;
  void DetachFromHost();
  bool host_shell_exited() const;
  bool host_link_down() const;

  bool ApplyHostFrame(TerminalHostFrame frame);
  void HostEnded(HostEnd end, std::string_view reason);
  struct HostResumePoint {
    std::uint64_t screen_top = 0;
    bool alternate = false;
  };
  std::optional<HostResumePoint> host_resume_point() const;
  void RequestOlderHostHistory();
  void RequestHostGapFill(std::size_t first_row, std::size_t rows);
  std::uint64_t RowShiftSequence() const;
  bool RowShiftsSince(std::uint64_t sequence, std::vector<TerminalHostHistory::RowShift>& out) const;
  void ReattachHost(std::shared_ptr<TerminalHostChannel> channel);
  void NoteHostReattached();
  void SetPredictionMode(TerminalPredictionOverlay::Mode mode);
  std::size_t PendingPredictions() const;

  struct HostCapture {
    TerminalHostFrame header;
    std::uint64_t first_line = 0;
    std::vector<TerminalLine> lines;
    std::uint64_t generation = 0;
    std::uint64_t output_count = 0;
  };
  void CaptureForHost(std::uint64_t from, std::size_t max_lines_before_screen, HostCapture& out);
  std::uint64_t CaptureHostHistory(std::uint64_t before, std::size_t count,
                                   std::vector<TerminalLine>& out) const;
  void SetOutputObserver(std::function<void()> observer);
  bool HasUnreadOutput() const;
  std::uint64_t OutputCount() const;

 private:
  enum class EscapeMode {
    None,
    AfterEscape,
    Csi,
    Osc,
    CharsetDesignate,
    StringPayload,
  };

  void AppendOutputLocked(std::string_view data);
  void EmitProcessExitMarkerLocked();
  void AbandonEscapeSequenceLocked();
  void EnterEscapeModeLocked(EscapeMode mode);
  void ConsumeGroundByteLocked(unsigned char byte);
  void ConsumeUtf8ByteLocked(unsigned char byte);
  void ConsumeAfterEscapeByteLocked(unsigned char byte);
  void ConsumeCharsetDesignateByteLocked(unsigned char byte);
  void ConsumeCsiByteLocked(unsigned char byte);
  enum class ControlStringByte { Consumed, Reprocess };
  ControlStringByte ConsumeControlStringByteLocked(unsigned char byte);
  void FullResetLocked();
  void ReverseIndexLocked();
  void HandleEscapeSequenceLocked(std::string_view sequence);
  std::size_t AddressingOriginRowLocked() const;
  std::size_t CursorRowUpLocked(std::size_t count) const;
  void HandlePrivateCsiLocked(char final, char intermediate, const std::vector<int>& params);
  void SoftResetLocked();
  void InsertLinesLocked(std::size_t count);
  void DeleteLinesLocked(std::size_t count);
  void DeleteCharactersLocked(std::size_t count);
  void EraseCharactersLocked(std::size_t count);
  void ScrollUpLocked(std::size_t count);
  void SetScrollMarginsLocked(const std::vector<int>& params);
  void RepeatLastGlyphLocked(int requested);
  void SetCursorStyleLocked(int style);
  void HandleOscSequenceLocked(std::string_view sequence);
  std::uint16_t InternExtraLocked(std::string_view uri, TerminalColor underline_color);
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
  void ExecuteC0Locked(unsigned char byte);
  void MoveCursorLocked(std::size_t row, std::size_t column);
  std::size_t PrimaryScreenTopLocked() const;
  void PutGlyphLocked(std::string_view glyph);
  std::size_t PutAsciiRunLocked(std::string_view run);
  void BreakWideGlyphPairForWriteLocked(TerminalLine& line, std::size_t start,
                                        std::size_t advance);
  void ResizeLineLocked(TerminalLine& line, std::size_t size);
  void ClearLineRangeLocked(TerminalLine& line, std::size_t start, std::size_t end);
  void BlankLineToCurrentBackgroundLocked(TerminalLine& line);
  void EraseInLineLocked(int mode);
  void EraseInDisplayLocked(int mode);
  void ResetScrollRegionLocked();
  void ClampScrollRegionLocked();
  std::size_t ActiveScrollRegionTopLocked() const;
  std::size_t ActiveScrollRegionBottomLocked() const;
  bool HasCustomScrollRegionLocked() const;
  struct ScrollRotation {
    std::size_t first_row = 0;
    std::size_t last_row = 0;
    std::size_t shift = 0;
  };
  ScrollRotation ResolveScrollRegionLocked(std::size_t top, std::size_t bottom, std::size_t count);
  void ScrollRegionUpLocked(std::size_t top, std::size_t bottom, std::size_t count);
  void ScrollRegionDownLocked(std::size_t top, std::size_t bottom, std::size_t count);
  void SaveCursorLocked();
  void RestoreCursorLocked();
  void SaveActiveScreenMetadataLocked(TerminalScreenState& screen);
  void SaveActiveScreenLocked();
  void RestoreSavedScreenLocked();
  void ResetScreenLocked(bool fill_rows);
  void SetAlternateScreenLocked(bool enabled, bool clear);
  void TrimScrollbackLocked();
  std::size_t CopyLineRangeIntoLocked(std::size_t start_row,
                                      std::size_t max_lines,
                                      std::vector<TerminalLine>& out) const;
  void AdvanceSnapshotGenerationLocked();
  void ResetEmulationStateLocked();
  void ResetTerminalModesLocked();
  void ReseedForStartLocked(const std::filesystem::path& working_directory,
                            std::string launch_label);
  bool ReserveWakeChannel(util::WakeChannel& channel) const;
  void PushWakeEvent() const;

  mutable std::mutex mutex_;
  std::deque<TerminalLine> lines_ = {TerminalLine{}};
  TerminalLineBufferPool line_buffer_pool_;
  TerminalScreenState primary_screen_;
  TerminalScreenState alternate_screen_;
  std::shared_ptr<platform::TerminalBackend> backend_;
  std::filesystem::path working_directory_;
  std::string default_launch_label_;
  std::string launch_label_;
  TerminalStyle current_style_;
  std::string pen_link_uri_;
  TerminalColor pen_underline_color_;
  std::uint16_t current_extra_ = 0;
  TerminalCellExtraTable extras_;
  std::string escape_sequence_buffer_;
  util::WakeChannel wake_channel_ = 0;
  int child_pid_ = -1;
  bool running_ = false;
  bool stop_requested_ = false;
  mutable bool wake_event_pending_ = false;
  EscapeMode escape_mode_ = EscapeMode::None;
  bool osc_escape_pending_ = false;
  bool oversized_osc52_dropped_ = false;
  bool pending_bell_ = false;
  std::optional<Notification> pending_notification_;
  bool pending_host_bell_ = false;
  bool use_alternate_screen_ = false;
  TerminalMouseModes mouse_;
  TerminalPointerState pointer_;
  bool application_cursor_keys_mode_ = false;
  bool origin_mode_ = false;
  bool auto_wrap_mode_ = true;
  bool insert_mode_ = false;
  std::array<char, 4> last_glyph_ = {};
  std::uint8_t last_glyph_length_ = 0;
  TerminalCharsets charsets_ = {};
  TerminalCharsets saved_charsets_ = {};
  bool bracketed_paste_mode_ = false;
  bool focus_event_mode_ = false;
  bool cursor_visible_ = true;
  bool screen_reverse_video_ = false;
  bool synchronized_output_ = false;
  bool color_scheme_updates_ = false;
  util::Rgba8 default_foreground_ = util::Rgba8{0xf5, 0xf7, 0xfa, 0xff};
  util::Rgba8 default_background_ = util::Rgba8{0x1f, 0x24, 0x2c, 0xff};
  int sync_suppressed_wakes_ = 0;
  bool sync_frame_completed_ = false;
  std::uint8_t kitty_keyboard_flags_ = 0;
  std::vector<std::uint8_t> kitty_keyboard_stack_;
  CursorShape cursor_shape_ = CursorShape::Block;
  bool cursor_blinking_ = true;
  std::vector<bool> tab_stops_;
  std::filesystem::path reported_working_directory_;
  std::optional<std::string> pending_clipboard_text_;
  std::string pending_utf8_sequence_;
  std::size_t rows_ = 24;
  std::size_t columns_ = 80;
  std::size_t max_scrollback_lines_ = 2000;
  std::uint64_t scrollback_trim_total_ = 0;
  std::size_t cursor_row_ = 0;
  std::size_t cursor_column_ = 0;
  std::size_t saved_cursor_row_ = 0;
  std::size_t saved_cursor_column_ = 0;
  TerminalStyle saved_style_{};
  TerminalColor saved_underline_color_;
  bool saved_origin_mode_ = false;
  std::size_t scroll_region_top_ = 0;
  std::size_t scroll_region_bottom_ = 23;
  util::Revision snapshot_generation_{1};
  std::uint64_t output_count_ = 0;

  std::string test_sent_bytes_;

  std::string pending_reply_;

  bool StartOnHost(const HostTerminalSource& source, const std::filesystem::path& working_directory,
                   std::string_view command, std::vector<std::string> shell_argv, HostIdentity attach);
  void StopHost(const std::shared_ptr<TerminalHostChannel>& channel);
  bool SendToHost(TerminalInputEvent event);
  bool SendLocalMouseEvent(MouseButton button, bool pressed, bool motion, std::size_t row,
                           std::size_t column, util::KeyModifiers modifiers);
  bool SendMouseToHost(TerminalInputEvent::Kind kind, MouseButton button, bool pressed,
                       std::size_t row, std::size_t column, util::KeyModifiers modifiers);
  std::shared_ptr<TerminalHostChannel> HostChannel() const;
  TerminalHostMode host_;
  void PrependHostHistory(std::uint64_t first, std::vector<TerminalLine> lines);
  void FillHostGap(std::uint64_t gap_id, std::uint64_t before, std::uint64_t first,
                   std::vector<TerminalLine> lines);
  std::function<void()> output_observer_;
  TerminalPredictionOverlay prediction_;
  TerminalPredictionOverlay::View PredictionViewLocked();

  friend struct ::microide::tests::TerminalSessionTestAccess;
};

}
