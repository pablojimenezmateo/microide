#include "workspace/services/TerminalPanelService.h"
#include "workspace/coordinators/SelectionAutoscroll.h"
#include "workspace/shell/WorkspaceShell.h"

#include "platform/HostIntegration.h"

#include <algorithm>
#include <functional>
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



// The cell under (x, y) in a laid-out terminal pane whose visible lines start at
// `first_row`. A template so it can take the shell's private LogSurfaceLayout.
template <typename Layout>
std::optional<TerminalSelectionPoint> CellInPaneLayout(int x, int y, const Layout& panel_layout,
                                                       const std::vector<terminal::TerminalLine>& lines,
                                                       std::size_t first_row, float char_width) {
  if (lines.empty() || panel_layout.line_height <= 0.0f || y < panel_layout.text_y ||
      y >= panel_layout.content_rect.y + panel_layout.content_rect.h) {
    return std::nullopt;
  }
  const int local_row =
      static_cast<int>((static_cast<float>(y) - panel_layout.text_y) / panel_layout.line_height);
  if (local_row < 0 || local_row >= panel_layout.scroll.visible_rows) {
    return std::nullopt;
  }
  const std::size_t row = std::min<std::size_t>(first_row + static_cast<std::size_t>(local_row),
                                                first_row + lines.size() - 1);
  const float local_x = std::max(0.0f, static_cast<float>(x) - panel_layout.text_x);
  const std::size_t column = static_cast<std::size_t>(
      std::max(0L, std::lround(local_x / std::max(1.0f, char_width))));
  return TerminalSelectionPoint{
      .row = row,
      .column = std::min(column, lines[row - first_row].cells.size()),
  };
}


bool IsTerminalUrlTerminator(char character) {
  return util::IsAsciiSpace(static_cast<unsigned char>(character)) != 0 || character == '"' ||
         character == '\'' || character == '<' || character == '>';
}

// Strips the punctuation prose wraps a link in ("see https://x.org/a." or
// "(https://x.org/a)"). A closing bracket is punctuation only when the URL has no
// opener for it: `https://en.wikipedia.org/wiki/Foo_(bar)` keeps its `)`, as VS
// Code's link detector does, where the old unconditional strip cut it off.
std::string TrimTerminalUrl(std::string url) {
  const auto has_unmatched_closer = [&](char open, char close) {
    std::ptrdiff_t depth = 0;
    for (const char ch : url) {
      if (ch == open) ++depth;
      else if (ch == close) --depth;
    }
    return depth < 0;
  };
  while (!url.empty()) {
    const char tail = url.back();
    if (tail == '.' || tail == ',' || tail == ';' || tail == ':' || tail == '!' || tail == '?') {
      url.pop_back();
      continue;
    }
    if ((tail == ')' && has_unmatched_closer('(', ')')) ||
        (tail == ']' && has_unmatched_closer('[', ']')) ||
        (tail == '}' && has_unmatched_closer('{', '}'))) {
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

// A link found in a line's text: what it opens and the bytes of the text it is.
struct TerminalTextLink {
  std::string target;
  std::size_t begin = 0;
  std::size_t end = 0;
};

// The grid column holding byte `byte` of TerminalLineText(line): the inverse of
// TerminalColumnToByteOffset (the line's width when `byte` is past its text).
std::size_t TerminalByteOffsetToColumn(const terminal::TerminalLine& line, std::size_t byte) {
  std::size_t offset = 0;
  for (std::size_t cell_index = 0; cell_index < line.cells.size(); ++cell_index) {
    const auto& cell = line.cells[cell_index];
    if (cell.style.wide_trailing()) {
      continue;
    }
    if (offset >= byte) {
      return cell_index;
    }
    const auto display_text = cell.DisplayText();
    offset += display_text.empty() ? 1 : display_text.size();
  }
  return line.cells.size();
}

// The logical line through row `hit` of `lines`: the rows soft-wrap joins (each
// row `wrapped_from_previous` continues the one above) as one text, and the byte
// offset in it where each row starts. A URL or path the terminal wrapped is one
// token here, where the row alone held only the part up to the wrap
// (TD-2026-10-29-422 item 4). Bounded by the rows `lines` holds: a link wrapped
// past the visible window is a link over the part on screen.
struct TerminalLogicalLine {
  std::size_t first = 0;  // index into `lines` of the logical line's first row
  std::string text;
  std::vector<std::size_t> row_starts;  // row_starts[i]: where row first + i begins
};

TerminalLogicalLine JoinTerminalLogicalLine(const std::vector<terminal::TerminalLine>& lines,
                                            std::size_t hit) {
  std::size_t first = hit;
  while (first > 0 && lines[first].wrapped_from_previous) {
    --first;
  }
  std::size_t last = hit;
  while (last + 1 < lines.size() && lines[last + 1].wrapped_from_previous) {
    ++last;
  }
  TerminalLogicalLine joined{.first = first};
  joined.row_starts.reserve(last - first + 1);
  for (std::size_t row = first; row <= last; ++row) {
    joined.row_starts.push_back(joined.text.size());
    // Only the last row drops its trailing blanks: a wrapped row's every cell is
    // part of the text, and TerminalColumnToByteOffset counts every cell.
    joined.text += TerminalLineSliceText(lines[row], 0, lines[row].cells.size(), row == last);
  }
  return joined;
}

// The (index into `lines`, column) of byte `byte` of `joined.text`. `exclusive_end`
// resolves a byte that is exactly a row boundary to the end of the row before it,
// so a link that ends at a wrap does not reach onto the next row.
std::pair<std::size_t, std::size_t> TerminalLogicalByteToCell(
    const std::vector<terminal::TerminalLine>& lines, const TerminalLogicalLine& joined,
    std::size_t byte, bool exclusive_end) {
  std::size_t index = 0;
  while (index + 1 < joined.row_starts.size() &&
         (exclusive_end ? joined.row_starts[index + 1] < byte : joined.row_starts[index + 1] <= byte)) {
    ++index;
  }
  const std::size_t row = joined.first + index;
  return {row, TerminalByteOffsetToColumn(lines[row], byte - joined.row_starts[index])};
}

std::optional<TerminalTextLink> TerminalUrlAtColumn(std::string_view text, std::size_t target_byte) {
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
        return TerminalTextLink{.target = std::move(url), .begin = start, .end = trimmed_end};
      }
      start = lowered_view.find(scheme, start + 1);
    }
  }

  return std::nullopt;
}

// Tokens for file references stop at whitespace, quotes and bracket/list
// punctuation: `src/a.cpp:12:3:` in "src/a.cpp:12:3: error: x" and `(a/b.py:7)`
// both isolate the path with its position.
bool IsTerminalPathTokenTerminator(char character) {
  return IsTerminalUrlTerminator(character) || character == '(' || character == ')' ||
         character == '[' || character == ']' || character == ',' || character == ';';
}

// A file reference under `target_byte` -- `path`, `path:line` or `path:line:col`
// (1-based, as compilers, linters and grep print them) -- resolved against
// `root` and kept only when `exists` says the file is there, so an arbitrary
// word never becomes a link (VS Code's terminal link provider does the same
// existence check). Returned as a `file://` link with an `#L<line>:<col>`
// fragment so it travels the same hover / pointer / click path as URLs; the
// click handler opens file links in the editor rather than externally.
std::optional<TerminalTextLink> TerminalFileReferenceAtByte(
    std::string_view text, std::size_t target_byte, const std::filesystem::path& root,
    const std::function<bool(const std::filesystem::path&)>& exists) {
  if (target_byte >= text.size() || IsTerminalPathTokenTerminator(text[target_byte])) {
    return std::nullopt;
  }
  std::size_t start = target_byte;
  while (start > 0 && !IsTerminalPathTokenTerminator(text[start - 1])) {
    --start;
  }
  std::size_t end = target_byte + 1;
  while (end < text.size() && !IsTerminalPathTokenTerminator(text[end])) {
    ++end;
  }
  std::string_view token = text.substr(start, end - start);
  while (!token.empty() && (token.back() == ':' || token.back() == '.' || token.back() == ',')) {
    token.remove_suffix(1);
  }
  if (token.empty() || token.find("://") != std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t token_end = start + token.size();  // `path:line:col`, as printed
  // Peel up to two trailing `:<digits>` groups: column, then line.
  std::size_t line = 0;
  std::size_t column = 0;
  for (int group = 0; group < 2; ++group) {
    const std::size_t colon = token.rfind(':');
    if (colon == std::string_view::npos || colon + 1 >= token.size()) {
      break;
    }
    const auto number = util::ParseSize(token.substr(colon + 1));
    if (!number.has_value()) {
      break;
    }
    column = line;
    line = *number;
    token = token.substr(0, colon);
  }
  if (line == 0) {
    column = 0;
  }
  // A bare word ("error", "make") is not a path; require a separator or an
  // extension dot so the existence check only runs for path-shaped tokens.
  if (token.empty() || token.find_first_of("/.") == std::string_view::npos ||
      token == "." || token == "..") {
    return std::nullopt;
  }
  std::filesystem::path path(token);
  if (path.is_relative()) {
    if (root.empty()) {
      return std::nullopt;
    }
    path = root / path;
  }
  path = path.lexically_normal();
  if (!exists(path)) {
    return std::nullopt;
  }
  std::string url = "file://" + path.generic_string();
  if (line > 0) {
    url += "#L" + std::to_string(line);
    if (column > 0) {
      url += ":" + std::to_string(column);
    }
  }
  return TerminalTextLink{.target = std::move(url), .begin = start, .end = token_end};
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


bool WorkspaceShell::BottomPanelShowsTerminal() const {
  return context_.current_project_state.panel.content == PanelContentKind::Terminal &&
         ActiveTerminalPane() != nullptr;
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
  terminal_text_renderer_.SetFontPointSize(static_cast<float>(size));
  terminal_text_renderer_.SetFontFamily(GetSettingValue("terminal.font_family").value_or(""));
  // The generation also moves on the first frame a terminal shows, when its
  // renderer trades the stand-in backend for a real one: that grid needs the
  // real cell metrics as much as a font change does.
  if (const std::size_t generation = terminal_text_renderer_.MetricsGeneration();
      generation != terminal_grid_metrics_generation_) {
    terminal_grid_metrics_generation_ = generation;
    // Cell metrics moved: force a terminal re-grid (rows/cols) and repaint. A font
    // change does not move the panel rect, so drop the cached rect to make the
    // resize guard in PrepareFrameOnce re-run ResizeTerminalToPanel off the new
    // metrics; the extra full redraws cover the reflow.
    last_terminal_panel_rect_.reset();
    NoteLayoutInputsChanged();
    post_render_redraws_remaining_ = std::max(post_render_redraws_remaining_, 2);
  }
}

namespace {

// The part of a log layout that is pure geometry: the body and its text insets.
WorkspaceShell::LogSurfaceLayout LogLayoutForBody(const SDL_FRect& body, float line_height) {
  WorkspaceShell::LogSurfaceLayout panel_layout;
  panel_layout.content_rect = body;
  panel_layout.text_x = body.x + kBottomPanelTextInset;
  panel_layout.text_y = body.y + kBottomPanelTextTopInset;
  panel_layout.line_height = line_height;
  return panel_layout;
}

}  // namespace

SDL_FRect WorkspaceShell::ActiveTerminalBodyRect(const WorkspaceLayout& layout) const {
  const SDL_FRect body = BottomPanelContentRect(layout);
  const TerminalTabState* tab = context_.current_project_state.active_terminal_tab();
  return tab != nullptr && BottomPanelShowsTerminal() ? tab->ActivePaneRect(body) : body;
}

WorkspaceShell::LogSurfaceLayout WorkspaceShell::ComputeTerminalPaneLogLayout(
    const SDL_FRect& body,
    float panel_height,
    const TerminalPaneState& pane,
    std::size_t line_count) const {
  LogSurfaceLayout panel_layout = LogLayoutForBody(body, terminal_text_renderer_.LineHeight());
  const int visible_rows =
      BottomPanelVisibleRowsForHeight(panel_height, panel_layout.line_height);
  const int max_scroll = TailScrollRowForContent(line_count, visible_rows);
  const int scroll_row = pane.follow_tail
                             ? max_scroll
                             : ClampScrollRowToContent(pane.scroll_row, line_count, visible_rows);
  panel_layout.scroll = ComputeScrollSurfaceLayout(body, line_count, visible_rows, scroll_row);
  panel_layout.text_width =
      std::max(0.0f, body.w - kBottomPanelTextInset * 2.0f -
                         (panel_layout.scroll.show_vertical ? kBottomPanelScrollbarTextReserve
                                                            : 0.0f));
  return panel_layout;
}

WorkspaceShell::LogSurfaceLayout WorkspaceShell::ComputeBottomPanelLogLayout(
    const WorkspaceLayout& layout,
    std::size_t line_count) const {
  // The terminal lays out per PANE: the active pane's slice of the body with its
  // own scroll state. Output (and an empty Terminal panel) keep the whole body.
  if (BottomPanelShowsTerminal()) {
    if (const TerminalPaneState* pane = ActiveTerminalPane(); pane != nullptr) {
      return ComputeTerminalPaneLogLayout(ActiveTerminalBodyRect(layout), layout.bottom_panel.h,
                                          *pane, line_count);
    }
  }
  LogSurfaceLayout panel_layout =
      LogLayoutForBody(BottomPanelContentRect(layout), PanelTextRenderer().LineHeight());
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
    if (const auto* terminal_tab = ActiveTerminalPane(); terminal_tab != nullptr) {
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
  if (const auto* terminal_tab = ActiveTerminalPane(); terminal_tab != nullptr) {
    return terminal_tab->follow_tail ? max_scroll
                                     : ClampScrollRowToContent(terminal_tab->scroll_row,
                                                               line_count,
                                                               visible_rows);
  }
  return 0;
}

std::optional<TerminalLinkHit> WorkspaceShell::TerminalLinkAtPoint(float x, float y) const {
  // The pane under the point, not the active one: a link in any pane of the tab
  // hovers and resolves (VS Code's split terminals).
  const TerminalTabState* tab = context_.current_project_state.active_terminal_tab();
  const auto layout_state = CurrentWorkspaceLayout();
  if (tab == nullptr || !BottomPanelShowsTerminal() || !layout_state.has_value()) {
    return std::nullopt;
  }
  const TerminalPaneRectsLayout pane_rects = tab->PaneRects(BottomPanelContentRect(*layout_state));
  const std::size_t pane_index = TerminalPaneIndexAt(pane_rects, x, y);
  if (pane_index >= tab->pane_count()) {
    return std::nullopt;
  }
  const TerminalPaneState* terminal_tab = tab->panes[pane_index].get();
  const std::size_t line_count = terminal_tab->session.LineCount();
  const LogSurfaceLayout panel_layout =
      ComputeTerminalPaneLogLayout(pane_rects.panes[pane_index], layout_state->bottom_panel.h,
                                   *terminal_tab, line_count);
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
      CellInPaneLayout(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
                       panel_layout, lines, first_row, terminal_text_renderer_.CharWidth());
  if (!position.has_value() || position->row < first_row ||
      position->row - first_row >= lines.size()) {
    return std::nullopt;
  }

  // A row the terminal wrapped onto continues past the snapshot's edge when the
  // hit's logical line reaches that edge: snapshot a range widened by up to
  // kMaxTerminalLinkWrapRows each way so the join, the URL scan and the OSC 8 run
  // see the whole link, not the part on screen (TD-2026-10-29-425 item 4).
  std::size_t base_row = first_row;
  std::size_t hit_index = position->row - first_row;
  {
    std::size_t first = hit_index;
    while (first > 0 && lines[first].wrapped_from_previous) {
      --first;
    }
    std::size_t last = hit_index;
    while (last + 1 < lines.size() && lines[last + 1].wrapped_from_previous) {
      ++last;
    }
    const bool continues_above = first == 0 && lines[0].wrapped_from_previous && first_row > 0;
    const bool may_continue_below =
        last + 1 == lines.size() && first_row + lines.size() < line_count;
    if (continues_above || may_continue_below) {
      constexpr std::size_t kMaxTerminalLinkWrapRows = 64;
      thread_local std::vector<terminal::TerminalLine> widened_lines;
      base_row = continues_above ? first_row - std::min(first_row, kMaxTerminalLinkWrapRows)
                                 : first_row;
      const std::size_t widened_count = (first_row - base_row) + lines.size() +
                                        (may_continue_below ? kMaxTerminalLinkWrapRows : 0);
      terminal_tab->session.SnapshotLineRangeInto(base_row, widened_count, widened_lines);
      if (position->row - base_row < widened_lines.size()) {
        lines_ptr = &widened_lines;
        hit_index = position->row - base_row;
      } else {
        base_row = first_row;
      }
    }
  }
  const std::vector<terminal::TerminalLine>& link_lines = *lines_ptr;
  const terminal::TerminalLine& hit_line = link_lines[hit_index];
  // An OSC 8 hyperlink is what the program said the text links to; it wins over
  // anything the text looks like. A `file://` link for another machine (the
  // session keeps those verbatim) has no local path to open.
  if (position->column < hit_line.cells.size() && hit_line.cells[position->column].link != 0) {
    const auto id = hit_line.cells[position->column].link;
    std::string uri = terminal_tab->session.LinkUri(id);
    if (!uri.empty() && (!uri.starts_with("file://") || uri.starts_with("file:///"))) {
      // The span is the run of cells the program marked with this link, across
      // a soft wrap: the run continues onto a wrapped row from its first cell.
      std::size_t begin_index = hit_index;
      std::size_t begin = position->column;
      while (true) {
        const auto& cells = link_lines[begin_index].cells;
        while (begin > 0 && cells[begin - 1].link == id) {
          --begin;
        }
        if (begin != 0 || !link_lines[begin_index].wrapped_from_previous || begin_index == 0 ||
            link_lines[begin_index - 1].cells.empty() || link_lines[begin_index - 1].cells.back().link != id) {
          break;
        }
        --begin_index;
        begin = link_lines[begin_index].cells.size();
      }
      std::size_t end_index = hit_index;
      std::size_t end = position->column + 1;
      while (true) {
        const auto& cells = link_lines[end_index].cells;
        while (end < cells.size() && cells[end].link == id) {
          ++end;
        }
        if (end != cells.size() || end_index + 1 >= link_lines.size() ||
            !link_lines[end_index + 1].wrapped_from_previous || link_lines[end_index + 1].cells.empty() ||
            link_lines[end_index + 1].cells.front().link != id) {
          break;
        }
        ++end_index;
        end = 0;
      }
      return TerminalLinkHit{.target = std::move(uri),
                             .span = {.row = base_row + begin_index,
                                      .begin_column = begin,
                                      .end_row = base_row + end_index,
                                      .end_column = end},
                             .pane = pane_index};
    }
  }
  const TerminalLogicalLine joined = JoinTerminalLogicalLine(link_lines, hit_index);
  const std::string& line_text = joined.text;
  const std::size_t target_byte = joined.row_starts[hit_index - joined.first] +
                                  TerminalColumnToByteOffset(hit_line, position->column);
  const auto hit = [&](TerminalTextLink link) {
    const auto [begin_index, begin_column] =
        TerminalLogicalByteToCell(link_lines, joined, link.begin, /*exclusive_end=*/false);
    const auto [end_index, end_column] =
        TerminalLogicalByteToCell(link_lines, joined, link.end, /*exclusive_end=*/true);
    return TerminalLinkHit{.target = std::move(link.target),
                           .span = {.row = base_row + begin_index,
                                    .begin_column = begin_column,
                                    .end_row = base_row + end_index,
                                    .end_column = end_column},
                           .pane = pane_index};
  };
  if (auto url = TerminalUrlAtColumn(line_text, target_byte); url.has_value()) {
    return hit(std::move(*url));
  }
  // This runs on every pointer move over the terminal, so the existence check
  // (a stat) is memoized on the last path it asked about.
  thread_local std::filesystem::path last_probe;
  thread_local bool last_probe_exists = false;
  std::optional<TerminalTextLink> file = TerminalFileReferenceAtByte(
      line_text, target_byte, context_.current_project_state.root,
      [](const std::filesystem::path& path) {
        if (path != last_probe) {
          std::error_code error;
          last_probe = path;
          last_probe_exists = std::filesystem::is_regular_file(path, error) && !error;
        }
        return last_probe_exists;
      });
  if (!file.has_value()) {
    return std::nullopt;
  }
  return hit(std::move(*file));
}

bool WorkspaceShell::OpenExternalUrl(std::string_view url) {
  if (url.empty()) {
    return false;
  }
  if (external_url_opener_) {
    return external_url_opener_(url);
  }
  // Validate the URL cheaply on the shell thread so a rejected scheme still reports
  // synchronously, then dispatch the actual xdg-open subprocess to the background
  // executor — the same treatment the file-manager reveal got in TD-2026-07-17-061,
  // for the same reason. This used to be SDL_OpenURL, which forked and returned at
  // once; routing it through the launcher seam made it a captured, waited-on child
  // with a 10s timeout, so a $BROWSER fallback that does not background its handler
  // would freeze the editor for ten seconds per clicked link and then report failure
  // for a browser that opened fine.
  if (!platform::IsOpenableExternalUrl(url)) {
    return false;
  }
  project_background_executor_.Post(
      [target = std::string(url)]() { platform::OpenUrl(target); });
  return true;
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
    if (auto* terminal_tab = ActiveTerminalPane(); terminal_tab != nullptr) {
      terminal_tab->scroll_row = clamped_scroll;
      terminal_tab->follow_tail = clamped_scroll >= max_scroll;
      // At the top of a host terminal's history: ask for the page before it, as
      // VS Code's terminal reveals scrollback as you scroll. A no-op locally.
      if (clamped_scroll == 0 && max_scroll > 0) {
        terminal_tab->session.RequestOlderHostHistory();
      }
      // And a gap rule on screen asks for the lines it withheld, so scrolling over
      // one shows (and copies) the text rather than the rule. A no-op locally.
      terminal_tab->session.RequestHostGapFill(static_cast<std::size_t>(clamped_scroll),
                                               static_cast<std::size_t>(std::max(0, visible_rows)));
    }
    return;
  }
  if (BottomPanelShowsOutput()) {
    auto& output = context_.current_project_state.panel.output;
    output.scroll_row = clamped_scroll;
    output.follow_tail = clamped_scroll >= max_scroll;
    return;
  }
  if (auto* terminal_tab = ActiveTerminalPane(); terminal_tab != nullptr) {
    terminal_tab->scroll_row = clamped_scroll;
    terminal_tab->follow_tail = clamped_scroll >= max_scroll;
  }
}

// Samples every pane of the terminal tab on screen once per prepared frame -- the
// line count, the clamped scroll (persisted, as paint used to do mid-frame), the
// visible lines and the cursor -- so every render of the frame paints the same
// terminal. The sessions are written by their PTY reader threads; paint reading
// them live let a partial frame's clips, and the QA redraw verifier's reference,
// see different output and a different caret.
void WorkspaceShell::PrepareTerminalPanesForFrame(TerminalTabState& tab,
                                                  const TerminalPaneRectsLayout& rects,
                                                  float panel_height) {
  // Before the layouts read scroll_row, so a scrolled-up view tracks the same
  // content instead of jumping forward by a trimmed batch.
  RebaseActiveTerminalForScrollbackTrim();
  for (std::size_t pane_index = 0; pane_index < tab.panes.size() && pane_index < rects.panes.size();
       ++pane_index) {
    TerminalPaneState* pane = tab.panes[pane_index].get();
    if (pane == nullptr) {
      continue;
    }
    const std::size_t line_count = pane->session.LineCount();
    const LogSurfaceLayout pane_layout =
        ComputeTerminalPaneLogLayout(rects.panes[pane_index], panel_height, *pane, line_count);
    if (pane_index == tab.active_pane) {
      SetBottomPanelScrollRow(pane_layout.scroll.vertical_scroll, line_count,
                              pane_layout.scroll.visible_rows);
    } else {
      pane->scroll_row = pane_layout.scroll.vertical_scroll;
      pane->follow_tail = pane_layout.scroll.vertical_scroll >= pane_layout.scroll.max_vertical_scroll;
    }
    const std::size_t first_row = static_cast<std::size_t>(std::max(0, pane_layout.scroll.vertical_scroll));
    const std::size_t visible_rows = static_cast<std::size_t>(std::max(0, pane_layout.scroll.visible_rows));
    const bool same_visible_range =
        pane->visible_lines_first_row == first_row && pane->visible_lines_max_rows == visible_rows;
    const std::uint64_t previous_generation =
        same_visible_range ? pane->visible_lines_snapshot.generation : 0;
    // Paint reads this snapshot, so a frame that takes a new one repaints the pane:
    // output can land after the wake that announced it was handled, and the frame
    // that samples it may have been drawn for something else entirely (a caret
    // blink repainted only the caret over vim's changed rows -- CI QA,
    // terminal-programs). A wake's own repaint covers the usual case; this covers
    // the frame that gets there first.
    if (pane->session.SnapshotLineRangeIfChanged(first_row, visible_rows, previous_generation,
                                                 &pane->visible_lines_snapshot)) {
      RequestRedrawRect(rects.panes[pane_index]);
    }
    pane->visible_lines_first_row = first_row;
    pane->visible_lines_max_rows = visible_rows;
    pane->frame_cursor = pane->session.CursorSnapshot();
    pane->frame_line_count = line_count;
    // The caret this frame paints against the one on screen: a cursor that hid or
    // moved without its rows changing (DECTCEM, a bare cursor move) left the old
    // caret painted in a pane nothing else repainted (QA monkey, seed 1).
    const std::optional<SDL_FRect> caret = TerminalCaretRectIn(pane_layout, pane->frame_cursor);
    const auto same_rect = [](const std::optional<SDL_FRect>& a, const std::optional<SDL_FRect>& b) {
      return a.has_value() == b.has_value() &&
             (!a.has_value() || (a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h));
    };
    if (!same_rect(caret, pane->painted_caret_rect)) {
      for (const std::optional<SDL_FRect>& rect : {pane->painted_caret_rect, caret}) {
        if (rect.has_value()) {
          RequestRedrawRect(*rect);
        }
      }
      pane->painted_caret_rect = caret;
    }
  }
}

void WorkspaceShell::RebaseActiveTerminalForScrollbackTrim() {
  auto* terminal_tab = ActiveTerminalPane();
  if (terminal_tab == nullptr) {
    return;
  }
  // A gap filled in the middle of a host terminal's scrollback moves every row
  // below it down by the lines it inserted (and nothing above it), so the view and
  // the selection stay on the text they were on. Applied first, in the row space
  // last observed: a shift names an absolute row, which trims do not move.
  if (const std::uint64_t sequence = terminal_tab->session.RowShiftSequence();
      sequence != terminal_tab->observed_row_shift_sequence) {
    std::vector<terminal::TerminalHostHistory::RowShift> shifts;
    if (terminal_tab->session.RowShiftsSince(terminal_tab->observed_row_shift_sequence, shifts)) {
      const std::uint64_t base = terminal_tab->observed_scrollback_trim_total;
      for (const terminal::TerminalHostHistory::RowShift& shift : shifts) {
        const auto moved = [&](std::uint64_t row) { return base + row > shift.after_row; };
        if (terminal_tab->scroll_row > 0 &&
            moved(static_cast<std::uint64_t>(terminal_tab->scroll_row))) {
          terminal_tab->scroll_row += static_cast<int>(shift.added);
        }
        for (auto* point : {&terminal_tab->selection_anchor, &terminal_tab->selection_head}) {
          if (point->has_value() && moved((*point)->row)) {
            (*point)->row += static_cast<std::size_t>(shift.added);
          }
        }
        if (terminal_tab->has_last_command && moved(terminal_tab->last_command_start_row)) {
          terminal_tab->last_command_start_row += static_cast<std::size_t>(shift.added);
        }
      }
    }
    // Further behind than the log: nothing to rebase by, so the view keeps its
    // row numbers and the next frame clamps them.
    terminal_tab->observed_row_shift_sequence = sequence;
  }
  const std::uint64_t total = terminal_tab->session.ScrollbackTrimTotal();
  if (total == terminal_tab->observed_scrollback_trim_total) {
    return;
  }
  if (total < terminal_tab->observed_scrollback_trim_total) {
    // Older history was PREPENDED (a host terminal's term/scrollback page): every
    // row this pane names moved down by the page, and the view stays on the same
    // text rather than jumping to the new top.
    const std::uint64_t added = terminal_tab->observed_scrollback_trim_total - total;
    terminal_tab->observed_scrollback_trim_total = total;
    terminal_tab->scroll_row += static_cast<int>(added);
    for (auto* point : {&terminal_tab->selection_anchor, &terminal_tab->selection_head}) {
      if (point->has_value()) {
        (*point)->row += static_cast<std::size_t>(added);
      }
    }
    if (terminal_tab->has_last_command) {
      terminal_tab->last_command_start_row += static_cast<std::size_t>(added);
    }
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
  if (auto* terminal_tab = ActiveTerminalPane(); terminal_tab != nullptr) {
    terminal_tab->mouse_selecting = false;
    selection_autoscroll::Disarm(context_.interaction_state);
    terminal_tab->selection_anchor.reset();
    terminal_tab->selection_head.reset();
  }
}

void WorkspaceShell::AppendTerminalPendingInput(std::string_view input) {
  auto* terminal_tab = ActiveTerminalPane();
  if (terminal_tab == nullptr) {
    return;
  }
  // TD-2026-07-17A-068: pending_input feeds only copy-last-command prompt stripping,
  // never the PTY, so bound its growth across repeated sends/pastes before a newline.
  std::string& pending = terminal_tab->pending_input;
  if (pending.size() >= TerminalPaneState::kMaxPendingInputBytes) {
    terminal_tab->pending_input_truncated = true;
    return;
  }
  const std::size_t remaining = TerminalPaneState::kMaxPendingInputBytes - pending.size();
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
  if (auto* terminal_tab = ActiveTerminalPane(); terminal_tab != nullptr) {
    (void)util::RemoveLastUtf8Codepoint(&terminal_tab->pending_input);
  }
}

void WorkspaceShell::SubmitTerminalPendingInput() {
  auto* terminal_tab = ActiveTerminalPane();
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




std::optional<WorkspaceShell::TerminalSelectionPoint>
WorkspaceShell::TerminalSelectionPointAt(
    int x,
    int y,
    const std::vector<terminal::TerminalLine>& lines,
    std::size_t first_row) const {
  if (!BottomPanelVisible() || ActiveTerminalPane() == nullptr || lines.empty()) {
    return std::nullopt;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  return CellInPaneLayout(x, y, ComputeBottomPanelLogLayout(*layout_state, lines.size()), lines,
                          first_row, terminal_text_renderer_.CharWidth());
}

std::optional<WorkspaceShell::TerminalSelectionPoint>
WorkspaceShell::TerminalViewportPositionForPoint(int x, int y) const {
  if (!BottomPanelVisible() || ActiveTerminalPane() == nullptr) {
    return std::nullopt;
  }

  const auto* terminal_tab = ActiveTerminalPane();
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
  // Mouse reports are in the ACTIVE pane's grid; a point in another pane is
  // outside it (the press that lands there activates that pane first).
  const SDL_FRect panel_content = ActiveTerminalBodyRect(layout);
  if (!Contains(panel_content, x, y)) {
    return std::nullopt;
  }

  const float text_x = panel_content.x + kBottomPanelTextInset;
  const float text_y = panel_content.y + kBottomPanelTextTopInset;
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



bool WorkspaceShell::TerminalCellSelected(std::size_t row, std::size_t column) const {
  const std::optional<TerminalSelectionBounds> selection =
      TerminalPanelService::ActiveSelectionBounds(context_.current_project_state);
  if (!selection.has_value()) {
    return false;
  }

  return TerminalSelectionContainsCell(*selection, row, column);
}

}  // namespace microide::workspace
