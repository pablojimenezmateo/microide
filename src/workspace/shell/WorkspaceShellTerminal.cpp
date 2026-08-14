#include "workspace/coordinators/SelectionAutoscroll.h"
#include "workspace/shell/WorkspaceShell.h"

#include "platform/HostIntegration.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

#include "util/Parse.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceTerminalSelection.h"

namespace microide::workspace {

namespace {

constexpr float kBottomPanelTextInset = 12.0f;
constexpr float kBottomPanelTextTopInset = 8.0f;
constexpr float kBottomPanelScrollbarTextReserve = 16.0f;

std::string TrimTrailingTerminalBlanks(std::string text) {
  while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

std::string TerminalLineText(const terminal::TerminalLine& line) {
  return TerminalLineSliceText(line, 0, line.cells.size(), true);
}

std::string FirstLine(std::string_view text) {
  const std::size_t newline = text.find('\n');
  return std::string(text.substr(0, newline));
}

bool IsTerminalUrlTerminator(char character) {
  return util::IsAsciiSpace(static_cast<unsigned char>(character)) != 0 || character == '"' ||
         character == '\'' || character == '<' || character == '>';
}

std::string TrimTerminalUrl(std::string url) {
  while (!url.empty()) {
    const char tail = url.back();
    if (tail == '.' || tail == ',' || tail == ';' || tail == ':' || tail == '!' ||
        tail == '?' || tail == ')' || tail == ']' || tail == '}') {
      url.pop_back();
      continue;
    }
    break;
  }
  return url;
}

// Byte offset within TerminalLineText(line) of the start of grid cell `column`,
// mirroring TerminalLineSliceText's cell→text mapping (wide-trailing spacers
// contribute no bytes; an empty cell contributes one space; a glyph contributes
// its UTF-8 bytes). Needed because a wide/multibyte cell BEFORE a URL desyncs a
// raw grid column from the byte offset TerminalUrlAtColumn works in.
std::size_t TerminalColumnToByteOffset(const terminal::TerminalLine& line, std::size_t column) {
  const std::size_t clamped = std::min(column, line.cells.size());
  std::size_t byte_offset = 0;
  for (std::size_t cell_index = 0; cell_index < clamped; ++cell_index) {
    const auto& cell = line.cells[cell_index];
    if (cell.style.wide_trailing()) {
      continue;
    }
    const auto display_text = cell.DisplayText();
    byte_offset += display_text.empty() ? 1 : display_text.size();
  }
  return byte_offset;
}

std::optional<std::string> TerminalUrlAtColumn(std::string_view text, std::size_t target_byte) {
  static constexpr std::string_view kSchemes[] = {
      "https://",
      "http://",
      "ftp://",
      "file://",
      "git://",
  };

  // URL schemes are case-insensitive (RFC 3986 §3.1), so match against a
  // lowercased copy of the line while slicing the URL out of the original text
  // to preserve the real casing of the path/query. Without this, `HTTPS://…`
  // or `File://…` links were never detected.
  std::string lowered(text);
  for (char& ch : lowered) {
    ch = util::ToLowerAsciiChar(static_cast<char>(ch));
  }
  const std::string_view lowered_view(lowered);

  for (std::string_view scheme : kSchemes) {
    std::size_t start = lowered_view.find(scheme);
    while (start != std::string_view::npos) {
      std::size_t end = start + scheme.size();
      while (end < text.size() && !IsTerminalUrlTerminator(text[end])) {
        ++end;
      }
      std::string url = TrimTerminalUrl(std::string(text.substr(start, end - start)));
      const std::size_t trimmed_end = start + url.size();
      // `target_byte` is a byte offset into `text` (see TerminalColumnToByteOffset
      // at the call site), so it compares directly against the URL's byte range.
      if (target_byte >= start && target_byte < trimmed_end && !url.empty()) {
        return url;
      }
      start = lowered_view.find(scheme, start + 1);
    }
  }

  return std::nullopt;
}

struct CapturedTerminalInvocation {
  std::size_t start_row = 0;
  std::string text;
};

CapturedTerminalInvocation CaptureVisibleTerminalInvocation(
    const std::vector<terminal::TerminalLine>& lines,
    std::size_t cursor_row,
    std::size_t cursor_column) {
  if (lines.empty()) {
    return {};
  }

  const std::size_t clamped_row = std::min(cursor_row, lines.size() - 1);
  std::size_t start_row = clamped_row;
  while (start_row > 0 && lines[start_row].wrapped_from_previous) {
    --start_row;
  }

  std::string text;
  for (std::size_t row = start_row; row <= clamped_row; ++row) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    const std::size_t end =
        row == clamped_row ? std::min(cursor_column, lines[row].cells.size()) : lines[row].cells.size();
    text += TerminalLineSliceText(lines[row], 0, end, false);
  }

  return CapturedTerminalInvocation{
      .start_row = start_row,
      .text = TrimTrailingTerminalBlanks(std::move(text)),
  };
}

std::size_t FindWrappedInvocationStartRow(const terminal::TerminalSession& session,
                                          std::size_t cursor_row) {
  std::size_t start_row = cursor_row;
  while (start_row > 0) {
    const auto current_line = session.SnapshotLineRange(start_row, 1);
    if (current_line.empty() || !current_line.front().wrapped_from_previous) {
      break;
    }
    --start_row;
  }
  return start_row;
}

}  // namespace

std::string TerminalLineSliceText(const terminal::TerminalLine& line,
                                  std::size_t start,
                                  std::size_t end,
                                  bool trim_trailing) {
  const std::size_t clamped_start = std::min(start, line.cells.size());
  const std::size_t clamped_end = std::min(std::max(clamped_start, end), line.cells.size());
  std::string text;
  text.reserve(clamped_end - clamped_start);
  for (std::size_t column = clamped_start; column < clamped_end; ++column) {
    const auto& cell = line.cells[column];
    // The trailing spacer of a double-width glyph holds no text of its own;
    // skipping it keeps copied/selected text free of phantom spaces.
    if (cell.style.wide_trailing()) {
      continue;
    }
    const auto display_text = cell.DisplayText();
    if (!display_text.empty()) {
      text.append(display_text);
      continue;
    }
    text.push_back(' ');
  }
  return trim_trailing ? TrimTrailingTerminalBlanks(std::move(text)) : text;
}

bool WorkspaceShell::BottomPanelShowsTerminal() const {
  return context_.current_project_state.panel.content == PanelContentKind::Terminal &&
         ActiveTerminalTab() != nullptr;
}

bool WorkspaceShell::BottomPanelShowsOutput() const {
  return context_.current_project_state.panel.content == PanelContentKind::Output;
}

bool WorkspaceShell::BottomPanelVisible() const {
  // Must agree with the visibility PrepareFrameOnce feeds ComputeLayout
  // (panel content != None): a PluginSurface panel is rendered and laid out,
  // so the interactive paths (wheel, clicks, resize handle, cursor) must see
  // it as visible too. It previously said Terminal-or-Output only, which left
  // plugin surface previews painted but mouse-dead (TD-2026-07-16-60/61).
  return context_.current_project_state.panel.content != PanelContentKind::None;
}

const render::TextRenderer& WorkspaceShell::PanelTextRenderer() const {
  return BottomPanelShowsTerminal() ? terminal_text_renderer_ : text_renderer_;
}

void WorkspaceShell::ApplyTerminalFontPreferences() {
  // Runs every prepared frame a terminal is shown. terminal.font_* only change on a
  // cold settings mutation (the store bumps its revision on any change), so skip the
  // GetSettingValue allocations + SetFontFamily entirely when nothing has changed
  // since the last apply — mirroring ApplyLiveSettings' allocation-free fast path.
  const std::uint64_t settings_revision = settings_store_.Revision();
  if (settings_revision == last_applied_terminal_font_settings_revision_) {
    return;
  }
  last_applied_terminal_font_settings_revision_ = settings_revision;

  const int size = std::clamp(util::ParseIntOr(GetSettingValue("terminal.font_size"), 13), 8, 32);
  bool changed = false;
  // SetFontPointSize always drops the renderer's width cache, so calling it on
  // an unchanged size would re-measure every glyph every frame the terminal is
  // shown. Only touch it when the resolved size actually moved.
  if (size != last_applied_terminal_font_size_) {
    last_applied_terminal_font_size_ = size;
    terminal_text_renderer_.SetFontPointSize(static_cast<float>(size));
    changed = true;
  }
  if (terminal_text_renderer_.SetFontFamily(
          GetSettingValue("terminal.font_family").value_or(""))) {
    changed = true;
  }
  if (changed) {
    // Cell metrics moved: force a terminal re-grid (rows/cols) and repaint. A font
    // change does not move the panel rect, so drop the cached rect to make the
    // resize guard in PrepareFrameOnce re-run ResizeTerminalToPanel off the new
    // metrics; the extra full redraws cover the reflow.
    last_terminal_panel_rect_.reset();
    MarkLayoutDirty();
    post_render_full_redraws_remaining_ = std::max(post_render_full_redraws_remaining_, 2);
  }
}

WorkspaceShell::LogSurfaceLayout WorkspaceShell::ComputeBottomPanelLogLayout(
    const WorkspaceLayout& layout,
    std::size_t line_count) const {
  LogSurfaceLayout panel_layout;
  panel_layout.content_rect = BottomPanelContentRect(layout);
  panel_layout.text_x = panel_layout.content_rect.x + kBottomPanelTextInset;
  panel_layout.text_y = panel_layout.content_rect.y + kBottomPanelTextTopInset;
  panel_layout.line_height = PanelTextRenderer().LineHeight();

  const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
  const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
  panel_layout.scroll =
      ComputeScrollSurfaceLayout(panel_layout.content_rect, line_count, visible_rows, scroll_row);
  panel_layout.text_width =
      std::max(0.0f, panel_layout.content_rect.w - kBottomPanelTextInset * 2.0f -
                         (panel_layout.scroll.show_vertical ? kBottomPanelScrollbarTextReserve
                                                            : 0.0f));
  return panel_layout;
}

int WorkspaceShell::BottomPanelVisibleRows(float panel_height) const {
  return BottomPanelVisibleRowsForHeight(panel_height, PanelTextRenderer().LineHeight());
}

int WorkspaceShell::BottomPanelScrollRow(std::size_t line_count, int visible_rows) const {
  const int max_scroll = TailScrollRowForContent(line_count, visible_rows);
  if (BottomPanelShowsTerminal()) {
    if (const auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
      return terminal_tab->follow_tail ? max_scroll
                                       : ClampScrollRowToContent(terminal_tab->scroll_row,
                                                                 line_count,
                                                                 visible_rows);
    }
    return 0;
  }
  if (BottomPanelShowsOutput()) {
    const auto& output = context_.current_project_state.panel.output;
    return output.follow_tail
               ? max_scroll
               : ClampScrollRowToContent(output.scroll_row, line_count, visible_rows);
  }
  if (const auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    return terminal_tab->follow_tail ? max_scroll
                                     : ClampScrollRowToContent(terminal_tab->scroll_row,
                                                               line_count,
                                                               visible_rows);
  }
  return 0;
}

std::optional<std::string> WorkspaceShell::TerminalUrlAtPoint(float x, float y) const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return std::nullopt;
  }

  const std::size_t line_count = terminal_tab->session.LineCount();
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const LogSurfaceLayout panel_layout = ComputeBottomPanelLogLayout(*layout_state, line_count);
  const std::size_t first_row =
      static_cast<std::size_t>(std::max(0, panel_layout.scroll.vertical_scroll));
  const std::size_t visible_rows =
      static_cast<std::size_t>(std::max(0, panel_layout.scroll.visible_rows));
  // Hover cursor-kind resolution runs on every pointer move over the terminal. The
  // render frame already copied this exact visible range into
  // `visible_lines_snapshot` (keyed by first_row/max_rows/generation), so reuse it
  // instead of re-snapshotting the visible lines per move (TD-2026-07-17A-027). Fall
  // back to a fresh snapshot only when the cached range does not match (e.g. a hover
  // arriving before the first terminal-panel render).
  // The fallback is NOT rare: a mouse wheel tick moves `first_row` and the render
  // that refreshes `visible_lines_snapshot` has not run yet, so every tick of a
  // scroll takes it. A by-value SnapshotLineRange there allocates one vector per
  // visible row per tick; the reusing overload into a main-thread scratch keeps
  // the buffers.
  thread_local std::vector<terminal::TerminalLine> fallback_lines;
  const std::vector<terminal::TerminalLine>* lines_ptr = nullptr;
  if (terminal_tab->visible_lines_first_row == first_row &&
      terminal_tab->visible_lines_max_rows == visible_rows &&
      !terminal_tab->visible_lines_snapshot.lines.empty()) {
    lines_ptr = &terminal_tab->visible_lines_snapshot.lines;
  } else {
    terminal_tab->session.SnapshotLineRangeInto(first_row, visible_rows, fallback_lines);
    lines_ptr = &fallback_lines;
  }
  const std::vector<terminal::TerminalLine>& lines = *lines_ptr;
  const auto position =
      TerminalSelectionPointAt(static_cast<int>(std::lround(x)),
                                        static_cast<int>(std::lround(y)), lines, first_row);
  if (!position.has_value() || position->row < first_row ||
      position->row - first_row >= lines.size()) {
    return std::nullopt;
  }

  const terminal::TerminalLine& hit_line = lines[position->row - first_row];
  return TerminalUrlAtColumn(TerminalLineText(hit_line),
                             TerminalColumnToByteOffset(hit_line, position->column));
}

bool WorkspaceShell::OpenExternalUrl(std::string_view url) const {
  if (url.empty()) {
    return false;
  }
  if (external_url_opener_) {
    return external_url_opener_(url);
  }
  return platform::OpenUrl(url).ok;
}

bool WorkspaceShell::RevealPathInFileExplorer(const std::filesystem::path& directory) {
  if (directory.empty()) {
    return false;
  }
  if (file_manager_opener_) {
    return file_manager_opener_(directory);
  }
  // Validate the path cheaply on the shell thread so an obviously-bad reveal still
  // reports synchronously, then dispatch the actual xdg-open subprocess to the
  // background executor: xdg-open normally forks and returns at once, but a wedged
  // file manager would otherwise park the UI thread for the whole timeout
  // (TD-2026-07-17-061). A rare post-validation launch failure is dropped rather
  // than surfaced — a toast would need a main-thread hop for a marginal case.
  std::error_code error;
  if (!std::filesystem::exists(directory, error) || error) {
    return false;
  }
  const std::filesystem::path target = directory;
  project_background_executor_.Post([target]() { platform::OpenPathInFileManager(target); });
  return true;
}

void WorkspaceShell::SetBottomPanelScrollRow(int scroll_row,
                                             std::size_t line_count,
                                             int visible_rows) {
  const int max_scroll = TailScrollRowForContent(line_count, visible_rows);
  const int clamped_scroll = ClampScrollRowToContent(scroll_row, line_count, visible_rows);
  if (BottomPanelShowsTerminal()) {
    if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
      terminal_tab->scroll_row = clamped_scroll;
      terminal_tab->follow_tail = clamped_scroll >= max_scroll;
    }
    return;
  }
  if (BottomPanelShowsOutput()) {
    auto& output = context_.current_project_state.panel.output;
    output.scroll_row = clamped_scroll;
    output.follow_tail = clamped_scroll >= max_scroll;
    return;
  }
  if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    terminal_tab->scroll_row = clamped_scroll;
    terminal_tab->follow_tail = clamped_scroll >= max_scroll;
  }
}

void WorkspaceShell::RebaseActiveTerminalForScrollbackTrim() {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return;
  }
  const std::uint64_t total = terminal_tab->session.ScrollbackTrimTotal();
  if (total <= terminal_tab->observed_scrollback_trim_total) {
    terminal_tab->observed_scrollback_trim_total = total;
    return;
  }
  const std::uint64_t delta = total - terminal_tab->observed_scrollback_trim_total;
  terminal_tab->observed_scrollback_trim_total = total;

  const auto drop_rows = [delta](std::size_t row) -> std::size_t {
    return static_cast<std::uint64_t>(row) > delta ? row - static_cast<std::size_t>(delta) : 0;
  };
  // scroll_row is a signed absolute row; clamp at 0 so a fully-trimmed scroll position
  // snaps to the new top rather than jumping forward by the trim batch.
  if (terminal_tab->scroll_row > 0) {
    terminal_tab->scroll_row =
        static_cast<std::uint64_t>(terminal_tab->scroll_row) > delta
            ? terminal_tab->scroll_row - static_cast<int>(delta)
            : 0;
  }
  if (terminal_tab->selection_anchor.has_value()) {
    terminal_tab->selection_anchor->row = drop_rows(terminal_tab->selection_anchor->row);
  }
  if (terminal_tab->selection_head.has_value()) {
    terminal_tab->selection_head->row = drop_rows(terminal_tab->selection_head->row);
  }
  if (terminal_tab->has_last_command) {
    terminal_tab->last_command_start_row = drop_rows(terminal_tab->last_command_start_row);
  }
}

void WorkspaceShell::ClearTerminalSelection() {
  if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    terminal_tab->mouse_selecting = false;
    selection_autoscroll::Disarm(context_.interaction_state);
    terminal_tab->selection_anchor.reset();
    terminal_tab->selection_head.reset();
  }
}

void WorkspaceShell::AppendTerminalPendingInput(std::string_view input) {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return;
  }
  // TD-2026-07-17A-068: pending_input feeds only copy-last-command prompt stripping,
  // never the PTY, so bound its growth across repeated sends/pastes before a newline.
  std::string& pending = terminal_tab->pending_input;
  if (pending.size() >= TerminalTabState::kMaxPendingInputBytes) {
    terminal_tab->pending_input_truncated = true;
    return;
  }
  const std::size_t remaining = TerminalTabState::kMaxPendingInputBytes - pending.size();
  if (input.size() <= remaining) {
    pending.append(input);
    return;
  }
  // Append a UTF-8-safe prefix that fits the budget, then flag truncation so submit
  // skips the (now partial, unreliable) prompt-prefix match.
  pending.append(input.substr(0, util::Utf8ByteBudgetPrefixLength(input, remaining)));
  terminal_tab->pending_input_truncated = true;
}

void WorkspaceShell::EraseLastTerminalPendingInputCodepoint() {
  if (auto* terminal_tab = ActiveTerminalTab(); terminal_tab != nullptr) {
    (void)util::RemoveLastUtf8Codepoint(&terminal_tab->pending_input);
  }
}

void WorkspaceShell::SubmitTerminalPendingInput() {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return;
  }

  const terminal::TerminalCursorSnapshot cursor = terminal_tab->session.CursorSnapshot();
  const std::size_t start_row =
      FindWrappedInvocationStartRow(terminal_tab->session, cursor.row);
  const auto lines =
      terminal_tab->session.SnapshotLineRange(start_row, cursor.row - start_row + 1);
  const CapturedTerminalInvocation captured =
      CaptureVisibleTerminalInvocation(lines, cursor.row - start_row, cursor.column);
  terminal_tab->last_command_start_row = start_row + captured.start_row;
  terminal_tab->last_command_invocation = captured.text;
  terminal_tab->last_command_prompt_prefix.clear();
  terminal_tab->has_last_command = !captured.text.empty();
  // A truncated capture is only a partial suffix of the real command, so the
  // ends_with match would strip the wrong prefix — skip it (TD-2026-07-17A-068).
  if (!terminal_tab->pending_input_truncated &&
      !terminal_tab->pending_input.empty() &&
      captured.text.size() >= terminal_tab->pending_input.size() &&
      captured.text.ends_with(terminal_tab->pending_input)) {
    terminal_tab->last_command_prompt_prefix = captured.text.substr(
        0, captured.text.size() - terminal_tab->pending_input.size());
  }
  terminal_tab->pending_input.clear();
  terminal_tab->pending_input_truncated = false;
}

bool WorkspaceShell::HasLastTerminalCommand() const {
  // Cheap predicate for action enablement: the exact precondition under which
  // LastTerminalCommandText() is guaranteed to return a value. Menu/context enablement
  // must not snapshot + join the whole scrollback transcript just to answer "is Copy Last
  // Command available?" (TD-2026-07-17A-065).
  const auto* terminal_tab = ActiveTerminalTab();
  return terminal_tab != nullptr && terminal_tab->has_last_command &&
         !terminal_tab->last_command_invocation.empty();
}

std::optional<std::string> WorkspaceShell::LastTerminalCommandText() const {
  if (!HasLastTerminalCommand()) {
    return std::nullopt;
  }
  const auto* terminal_tab = ActiveTerminalTab();

  if (terminal_tab->session.using_alternate_screen()) {
    return terminal_tab->last_command_invocation;
  }

  const std::size_t line_count = terminal_tab->session.LineCount();
  if (terminal_tab->last_command_start_row >= line_count) {
    return terminal_tab->last_command_invocation;
  }
  // Cap the snapshot to a bounded window of the most-recent-command output. A
  // long-running command can retain output up to the full scrollback cap, so copying
  // every retained row (and then joining a second full transcript) would let the invoke
  // path allocate unboundedly on the UI thread (TD-2026-07-17A-037). Keep the head of the
  // output and mark the tail truncated.
  const std::size_t available = line_count - terminal_tab->last_command_start_row;
  const std::size_t capped_lines = std::min(available, kDefaultLastTerminalCommandMaxLines);
  const bool source_truncated = available > capped_lines;
  const auto lines =
      terminal_tab->session.SnapshotLineRange(terminal_tab->last_command_start_row, capped_lines);
  if (lines.empty()) {
    return terminal_tab->last_command_invocation;
  }

  std::vector<std::string> rows;
  rows.reserve(lines.size());
  for (const auto& line : lines) {
    rows.push_back(TerminalLineText(line));
  }

  std::string transcript = BuildLastTerminalCommandTranscript(
      rows, TrimTrailingTerminalBlanks(terminal_tab->last_command_prompt_prefix),
      FirstLine(terminal_tab->last_command_invocation), source_truncated);

  return transcript.empty() ? std::optional<std::string>(terminal_tab->last_command_invocation)
                            : std::optional<std::string>(std::move(transcript));
}

bool WorkspaceShell::TerminalHasSelection() const {
  const auto selection = ActiveTerminalSelectionBounds();
  return selection.has_value() &&
         (selection->start.row != selection->end.row ||
          selection->start.column != selection->end.column);
}

std::optional<WorkspaceShell::TerminalSelectionPoint>
WorkspaceShell::TerminalSelectionPointAt(
    int x,
    int y,
    const std::vector<terminal::TerminalLine>& lines,
    std::size_t first_row) const {
  if (!BottomPanelVisible() || ActiveTerminalTab() == nullptr || lines.empty()) {
    return std::nullopt;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;
  const LogSurfaceLayout panel_layout = ComputeBottomPanelLogLayout(layout, lines.size());
  if (panel_layout.line_height <= 0.0f || y < panel_layout.text_y ||
      y >= panel_layout.content_rect.y + panel_layout.content_rect.h) {
    return std::nullopt;
  }

  const int local_row =
      static_cast<int>((static_cast<float>(y) - panel_layout.text_y) / panel_layout.line_height);
  if (local_row < 0 || local_row >= panel_layout.scroll.visible_rows) {
    return std::nullopt;
  }

  const std::size_t row =
      std::min<std::size_t>(first_row + static_cast<std::size_t>(local_row),
                            first_row + lines.size() - 1);
  const float local_x = std::max(0.0f, static_cast<float>(x) - panel_layout.text_x);
  const std::size_t column = static_cast<std::size_t>(
      std::max(0L, std::lround(local_x / std::max(1.0f, terminal_text_renderer_.CharWidth()))));
  return TerminalSelectionPoint{
      .row = row,
      .column = std::min(column, lines[row - first_row].cells.size()),
  };
}

std::optional<WorkspaceShell::TerminalSelectionPoint>
WorkspaceShell::TerminalViewportPositionForPoint(int x, int y) const {
  if (!BottomPanelVisible() || ActiveTerminalTab() == nullptr) {
    return std::nullopt;
  }

  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return std::nullopt;
  }

  const std::size_t rows = terminal_tab->session.rows();
  const std::size_t columns = terminal_tab->session.columns();
  if (rows == 0 || columns == 0) {
    return std::nullopt;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;
  const SDL_FRect panel_content = BottomPanelContentRect(layout);
  if (!Contains(panel_content, x, y)) {
    return std::nullopt;
  }

  const float text_x = panel_content.x + 12.0f;
  const float text_y = panel_content.y + 8.0f;
  const float line_height = terminal_text_renderer_.LineHeight();
  const float char_width = std::max(1.0f, terminal_text_renderer_.CharWidth());
  if (line_height <= 0.0f || y < text_y) {
    return std::nullopt;
  }

  const std::size_t row = static_cast<std::size_t>(
      std::max(0.0f, static_cast<float>(std::floor((static_cast<float>(y) - text_y) / line_height))));
  if (row >= rows) {
    return std::nullopt;
  }

  const float local_x = std::max(0.0f, static_cast<float>(x) - text_x);
  const std::size_t column = static_cast<std::size_t>(std::floor(local_x / char_width));
  return TerminalSelectionPoint{
      .row = row,
      .column = std::min(column, columns - 1),
  };
}

terminal::TerminalSession::MouseButton WorkspaceShell::TerminalMouseButtonForSdl(
    Uint8 button) const {
  return TerminalMouseButtonFromSdl(button);
}

std::optional<TerminalSelectionBounds> WorkspaceShell::ActiveTerminalSelectionBounds() const {
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr || !terminal_tab->selection_anchor.has_value() ||
      !terminal_tab->selection_head.has_value()) {
    return std::nullopt;
  }

  return NormalizeTerminalSelection(terminal_tab->selection_anchor, terminal_tab->selection_head);
}

std::string WorkspaceShell::SelectedTerminalText() const {
  const auto selection = ActiveTerminalSelectionBounds();
  if (!selection.has_value()) {
    return {};
  }

  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return {};
  }

  const std::size_t first_row = selection->start.row;
  const auto lines = terminal_tab->session.SnapshotLineRange(
      first_row, selection->end.row - first_row + 1);
  if (lines.empty()) {
    return {};
  }

  TerminalSelectionBounds rebased = *selection;
  rebased.start.row -= first_row;
  rebased.end.row -= first_row;
  return ExtractTerminalSelectionText(lines, rebased);
}

bool WorkspaceShell::TerminalCellSelected(std::size_t row, std::size_t column) const {
  const std::optional<TerminalSelectionBounds> selection = ActiveTerminalSelectionBounds();
  if (!selection.has_value()) {
    return false;
  }

  return TerminalSelectionContainsCell(*selection, row, column);
}

}  // namespace microide::workspace
