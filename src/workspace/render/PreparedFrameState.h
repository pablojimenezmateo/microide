#pragma once

// What one prepared frame decided once for all of its renders, and what the last
// prepared frame painted.
//
// Paint must not read the clock, the live pointer or another thread's state: a
// partial frame renders several clips, and the QA redraw verifier renders the same
// frame again in full, so every render of one frame has to paint the same thing.
// The decisions go here, made in PrepareFrameOnce. And whatever moves a caret, a
// hover or a document away from where the last frame painted it needs that region
// repainted; rather than every transition carrying its own damage request (several
// carried none), frame prep compares with what was painted and repaints both.

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "editor/BracketScanner.h"
#include <string>
#include <utility>
#include <vector>

#include "editor/EditorViewRenderer.h"
#include "workspace/render/DerivedDamage.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/EditorHoverTypes.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceInteractionState.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

// Bottom-panel paint elements kept in fixed-capacity lists (util::InlineVector
// needs them default-constructible, which a nested struct with member
// initializers is not until its enclosing class is complete).
struct PanelScrollbarPaint {
  SDL_FRect track{};
  SDL_FRect thumb{};
  bool active = false;  // being dragged
};
struct TerminalHintPaint {
  SDL_FRect rect{};
  std::string_view text;  // empty: no hint
};
struct TerminalDividerPaint {
  SDL_FRect rect{};
  bool active = false;  // being dragged
};
// The debug pane (derived-damage 3.1): a row is its band and up to four text runs,
// each drawn at `x` truncated to `width` on the band's colour.
struct DebugTextRun {
  float x = 0.0f;
  float width = 0.0f;
  SDL_Color color{};
  std::string_view text;  // a view into the debug models, alive for the frame
};
struct DebugModeTabPaint {
  SDL_FRect rect{};
  DebugPaneMode mode = DebugPaneMode::CallStack;
  std::string_view label;  // empty: icon only
  bool hovered = false;
  bool active = false;
};

// The Source Control panel's head (derived-damage 4.1): its buttons, text lines
// and the commit draft's two fields. Text views point into the frame's git
// presentation (GitPanelHeadPaint::presentation) or the commit draft, alive for
// the frame.
enum class GitPanelInk : std::uint8_t { Muted, Secondary, Primary, Error };
enum class GitPanelTone : std::uint8_t { Neutral, Accent, Destructive };
struct GitPanelButtonPaint {
  SDL_FRect rect{};
  std::string_view label;
  GitPanelTone tone = GitPanelTone::Neutral;
  bool enabled = true;
  bool hovered = false;
  bool ellipsize = false;  // the label is cut to the button less 8 px with an ellipsis
};
struct GitPanelTextPaint {
  SDL_FRect rect{};  // one line box; the text is truncated to its width
  std::string_view text;
  GitPanelInk ink = GitPanelInk::Muted;
};
struct GitPanelFieldLine {
  float y = 0.0f;
  std::string_view text;
  SDL_FRect selection{};  // empty: no selection on this line
};
struct GitPanelFieldPaint {
  SDL_FRect frame{};  // empty: the sidebar has no room for the field
  bool focused = false;
  float text_x = 0.0f;
  float text_width = 0.0f;
  std::string_view placeholder;  // shown, muted, instead of the lines
  float placeholder_y = 0.0f;
  std::vector<GitPanelFieldLine> lines;
};

// A compare or merge surface's paint inputs that are not the tab's own state,
// built at frame prep: the settings it paints with, the drag state of its bars
// and dividers, the pointer (its buttons' hover) and the blame overlay of the
// focused one. RenderCompareSurface / RenderMergeSurface and their scrollbars read
// these and the tab, nothing else (CheckDerivedSurfacePaintReadsOnlyItsSnapshot).
struct DiffSurfacePaint {
  bool render_whitespace = false;
  bool indent_guides = true;
  bool bracket_match = true;
  bool line_numbers = true;
  bool vertical_bar_dragged = false;
  bool horizontal_bar_dragged = false;
  bool left_divider_dragged = false;   // merge
  bool right_divider_dragged = false;  // merge
  std::optional<SDL_FPoint> pointer;   // inside the surface, else none
  std::optional<editor::EditorBlameOverlay> blame;
  std::span<const editor::PublishedDiagnostic> diagnostics;  // the editable side's, while clean
};

struct PreparedFrameState {
  // ---- decided once per prepared frame ---------------------------------
  // The caret blink phase. Paint sites read this, never the clock.
  bool caret_visible = false;
  // Whether the focused terminal owns the blinking caret (not its find bar, not a
  // modal): only then does its caret blink; otherwise it shows a steady outline.
  bool terminal_owns_caret = false;
  // The floating or modal surface the pointer is over, if any: PointerOver lights
  // up only rects inside it (an empty rect: the pointer is outside a modal, and
  // nothing hovers).
  std::optional<SDL_FRect> pointer_occluder;

  // Per editor group, its pane's metrics (rows, columns, gutter, the sticky band)
  // as this frame lays them out. Sized at frame prep -- the viewport's size and
  // sticky band set there too -- so paint, and everything resolved between frames
  // against the pane (hover targets, the blame overlay, a click), read ONE
  // geometry. Paint used to derive them, so the blame overlay was built there and
  // prep worked from the previous frame's (TD-2026-10-24-389).
  std::array<editor::EditorViewMetrics, kMaxEditorGroups> pane_metrics{};
  std::array<bool, kMaxEditorGroups> pane_metrics_valid{};

  // The same flags per item index (bit 0 enabled, bit 1 checked), for the open
  // popup and its submenu: paint draws them from here, not from live state. A
  // flag read twice in one frame could differ -- a terminal's exit lands on its
  // reader thread -- and the partial frame and a full render of it disagreed.
  std::vector<std::uint8_t> menu_popup_row_flags;
  std::vector<std::uint8_t> menu_submenu_row_flags;
  // This frame's editor hover card layout (none: no card), laid out at prep;
  // its paint reads it and its Chrome unit keys on its rect.
  std::optional<EditorHoverPopupLayout> hover_popup;
  // This frame's signature-help card rect (none: hidden), laid out at prep.
  std::optional<SDL_FRect> signature_help_rect;
  // The plugin status items' hash, which the breadcrumb bands' units key on.
  std::uint64_t status_items_hash = 0;
  // This frame's menu-bar popup, submenu and context menu rects (none: closed),
  // laid out once at prep for paint and their Chrome units.
  std::optional<SDL_FRect> menu_popup_rect;
  std::optional<SDL_FRect> menu_submenu_rect;
  std::optional<SDL_FRect> context_menu_rect;
  // The editor viewport that had the keyboard last frame (null: none did), for
  // the on_focus_change auto-save.
  const void* focused_editor = nullptr;
  // `editor.autosave = on_focus_change` saves when the USER moves focus off an
  // editor: armed by the first input (a key, text, a mouse press, a wheel) or
  // control command. Before it, the editor in front changes only because the app
  // is setting itself up -- a relaunch restoring its projects showed the start
  // page, a restored tab, the start page again -- and that saved the unsaved
  // buffers the crash recovery had just brought back (soak 0afa15c55c0f).
  bool focus_autosave_armed = false;
  // (A compare tab's scroll, active side and diff shape are in its rows' keys:
  // CompareRowUnits, derived-damage 2.5.)
  // The focused compare or merge tab's scrollbars as painted (no track: hidden).
  // An edit that widens a line or adds one moves a thumb that sits on none of the
  // rows the edit damages -- typing into a compare tab's long line left the
  // horizontal thumb at its old width (QA monkey, seed 17). As an editor pane's
  // bar units do (EditorRowUnits): a bar shown or hidden repaints the surface, a moved one
  // its strip. The lane's markers (a hash) likewise: a merge result edit that
  // wrapped a line onto one more row moved the conflicts below it, and the lane
  // kept their old markers (soak e97cacbc359f).
  struct DiffSurfaceBars {
    const void* tab = nullptr;
    std::optional<SDL_FRect> horizontal_track;
    SDL_FRect horizontal_thumb{};
    // The overview lane sits LEFT of the vertical track; the thumb is in the
    // track, so a moved thumb repaints the track, a changed marker the lane.
    std::optional<SDL_FRect> vertical_lane;
    SDL_FRect vertical_track{};
    SDL_FRect vertical_thumb{};
    std::uint64_t vertical_markers = 0;
  };
  // A compare or merge surface whose syntax tokenizer is still behind its visible
  // window after this frame's capped step: OnFramePresented requests it, so the
  // NEXT frame takes the next step. Damage requested during prep belongs to the
  // frame being prepared, so prep cannot schedule a following frame itself -- the
  // catch-up stalled after one step, and the rows it then reached on some later
  // frame were never repainted highlighted (QA monkey, commands profile, seed 61).
  std::optional<SDL_FRect> syntax_catch_up_rect;
  // The focused pane's frame inputs its rows key on (EditorRowUnits): its
  // bracket-match pair and its occurrence highlights, one (line, hash of that
  // line's ranges) per line, sorted.
  const void* bracket_viewport = nullptr;
  std::optional<editor::BracketMatchPair> bracket_pair;
  const void* occurrence_viewport = nullptr;
  std::vector<std::pair<std::size_t, std::uint64_t>> occurrence_lines;
  // The window title (WorkspaceUiText ComposeWindowTitle) and a hash of what it
  // was composed from. It names the focused group's active tab, its dirty mark and
  // the project, so the taskbar and Alt+Tab tell two windows apart; the custom
  // title bar draws it, and the platform window is retitled when it changes.
  std::uint64_t window_title_inputs = 0;
  std::string window_title = "microide";
  // The floating debug toolbar as painted: whether it shows, its rect, and the
  // run state its Continue/Pause button and step buttons draw. A session that
  // stopped or resumed repainted the debug pane and the menu bar but not the
  // toolbar, so its first button kept reading Pause over a stopped program.
  struct DebugToolbar {
    bool visible = false;
    bool stopped = false;
    SDL_FRect rect{};
    bool operator==(const DebugToolbar& o) const {
      return visible == o.visible && stopped == o.stopped && rect.x == o.rect.x &&
             rect.y == o.rect.y && rect.w == o.rect.w && rect.h == o.rect.h;
    }
  };

  // ---- derived damage (openspec derived-damage) -------------------------
  // Each migrated surface's units as last painted; prep compares and damages.
  DerivedDamage derived_damage;
  // Per-row keys of the editor pane being prepared (EditorRowUnits): scratch,
  // cleared per pane, capacity kept across frames.
  std::vector<std::uint64_t> editor_row_keys;
  // An editor pane's paint inputs, built once at frame prep (derived-damage 2.3):
  // its rows' units key on them and PaintEditorPane draws from nothing else.
  // Pointers name objects that stay put for the frame (the viewport, its fold
  // model, the decoration and blame stores); the view model and the filtered
  // diagnostics are owned here and keep their capacity across frames.
  struct EditorPanePaint {
    bool valid = false;
    editor::TextViewport* viewport = nullptr;
    editor::FoldingModel* folding = nullptr;
    SDL_FRect rect{};
    bool active = false;
    editor::EditorViewMetrics metrics{};
    editor::EditorViewModel view_model;
    const editor::WelcomeViewModel* welcome = nullptr;  // a placeholder viewport's home surface
    std::vector<editor::PublishedDiagnostic> filtered_diagnostics;
    std::span<const editor::PublishedDiagnostic> diagnostics;
    const editor::FileDecorations* decorations = nullptr;
    const editor::EditorBlameOverlay* blame = nullptr;
    const std::optional<editor::EditorBlameOverlay>* blame_source = nullptr;  // what `blame` points into
    std::uint64_t diagnostics_revision = 0;
    // The find widget's highlights: a literal query, or a precomputed match set.
    std::string search_query;
    std::span<const editor::SelectionRange> explicit_search_matches;
    std::optional<editor::SelectionRange> active_search_match;
    std::optional<editor::SelectionRange> find_scope;
    bool bracket_match = false;
    bool indent_guides = false;
    bool render_whitespace = false;
    bool line_numbers = true;
    bool overview_ruler = false;
    editor::DiagnosticSeverity diagnostics_min_severity = editor::DiagnosticSeverity::Hint;
    const BufferSearchState* ruler_search = nullptr;  // the find state while the ruler draws its matches
    bool vertical_bar_dragged = false;
    bool horizontal_bar_dragged = false;
    // A selection drag-and-drop's insertion point on this pane (line, column).
    std::optional<editor::TextPosition> drop_indicator;
  };
  std::array<EditorPanePaint, kMaxEditorGroups> editor_pane_paint{};
  // Per group: see DiffSurfacePaint.
  std::array<DiffSurfacePaint, kMaxEditorGroups> diff_surface_paint{};
  // The status bar this frame paints, settled at prep: its view model, and per
  // segment the row, text, colour and hover it is drawn with. Paint draws these
  // and nothing else, so the fingerprint of each is everything that segment
  // shows (WorkspaceShell::PrepareStatusBarFrame).
  struct StatusBarSegmentPaint {
    SDL_FRect row{};
    std::string_view text;
    SDL_Color color{};
    bool hovered = false;
  };
  StatusBarViewModel status_bar;
  std::vector<StatusBarSegmentPaint> status_bar_segments;

  // ---- bottom panel header (derived-damage 3.1, clip-discipline stage 3) ----
  // The header's tabs, chevrons and buttons as this frame paints them, settled at
  // prep (PrepareBottomPanelHeaderFrame). PaintBottomPanelHeader draws these and
  // nothing else, each inside its own render::PaintElement, so the fingerprint of
  // each is everything that element shows and its unit rect is the element's.
  struct PanelTabPaint {
    SDL_FRect rect{};
    SDL_FRect close_rect{};
    std::string_view title;
    std::string_view badge_text;
    SDL_Color badge_color{};
    std::string_view secondary_title;
    std::optional<editor::WelcomeStageState> remote_state;
    bool show_badge = false;
    bool active = false;
    bool hovered = false;
    bool close_hovered = false;
    bool dimmed = false;
    bool activity_dot = false;
    bool bell_mark = false;
  };
  struct PanelButtonPaint {
    SDL_FRect rect{};
    bool hovered = false;
    std::size_t hidden = 0;  // an overflow chevron's count of tabs out of view
  };
  struct PanelHeaderPaint {
    bool visible = false;
    SDL_FRect header{};
    std::vector<PanelTabPaint> tabs;
    std::string_view label;  // what the header says when it has no tabs
    PanelButtonPaint overflow_left;
    PanelButtonPaint overflow_right;
    PanelButtonPaint new_terminal;
    PanelButtonPaint maximize;
    bool maximized = false;
    // The panel's top edge, the line a drag resizes it by: drawn last, over the
    // header's top row, in the accent while dragged.
    SDL_FRect divider{};
    bool divider_active = false;
    // A tab being dragged: the feedback draws over the strip from the visible
    // tabs (the panel view model's list, alive for the frame).
    const std::vector<VisibleStripTab>* drag_tabs = nullptr;
    bool dragging = false;
    std::size_t drag_source = 0;
    float drag_pointer_x = 0.0f;
    float drag_grab_offset_x = 0.0f;
  };
  PanelHeaderPaint panel_header;

  // ---- the Output channel's body (derived-damage 3.1, clip-discipline stage 3) ----
  // Its rows, empty hint and scrollbar as this frame paints them, settled at prep
  // (PrepareBottomPanelOutputFrame) and drawn by PaintBottomPanelOutputBody alone.
  struct OutputRowPaint {
    SDL_FRect rect{};  // the body's width, one line tall; the text sits at its top
    float text_x = 0.0f;
    float text_width = 0.0f;
    // The entry's serial (WorkspaceOutputChannels::FirstEntrySerial + index). Its
    // text never changes under it, so the serial stands for `text`, `prefix`,
    // `code` and `highlighted` in the fingerprint: a line can be 1 MiB.
    std::uint64_t serial = 0;
    std::string_view text;
    bool snippet = false;  // a context snippet: `prefix`, then `code` syntax-coloured
    std::string_view prefix;
    std::string_view code;
    const editor::HighlightedLine* highlighted = nullptr;  // the channel's memo
    std::uint64_t highlight_path = 0;  // a hash of the path `highlighted` is for
  };
  using PanelScrollbarPaint = workspace::PanelScrollbarPaint;
  struct OutputBodyPaint {
    bool visible = false;
    std::vector<OutputRowPaint> rows;
    SDL_FRect hint_rect{};
    std::string_view hint;  // the empty channel's hint, wrapped in `hint_rect`
    std::optional<PanelScrollbarPaint> scrollbar;
  };
  OutputBodyPaint output_body;

  // ---- the terminal's chrome (derived-damage 3.1, clip-discipline stage 3) ----
  // Around the grid, whose rows keep their own damage (the pane snapshot's
  // generation): each pane's empty hint and scrollbar, the dividers between panes,
  // and the hint over a panel with no session. Settled at prep
  // (PrepareTerminalChromeFrame); paint reads only this.
  using TerminalHintPaint = workspace::TerminalHintPaint;
  using TerminalDividerPaint = workspace::TerminalDividerPaint;
  struct TerminalChromePaint {
    bool visible = false;
    TerminalHintPaint no_session;  // the panel shows Terminal with no live session
    util::InlineVector<TerminalHintPaint, kMaxTerminalPanes> hints;  // one per pane
    util::InlineVector<std::optional<PanelScrollbarPaint>, kMaxTerminalPanes> scrollbars;  // one per pane
    util::InlineVector<TerminalDividerPaint, kMaxTerminalPanes> dividers;
  };
  TerminalChromePaint terminal_chrome;

  // ---- the debug pane (derived-damage 3.1, clip-discipline stage 3) ----
  // Its mode tabs, rows, empty hint and scrollbar, laid out at prep
  // (PrepareDebugPaneFrame) from the four surfaces' models; PaintDebugPaneBody
  // draws only this. The pane's background and focus ring stay with the shell.
  struct DebugPaneRowPaint {
    SDL_FRect band{};          // the row's full width, lifted a pixel over its text
    float text_y = 0.0f;
    bool highlighted = false;  // selected, focused or under the pointer
    util::InlineVector<DebugTextRun, 4> runs;
    bool disclosure = false;   // an expandable value-tree row's triangle
    bool expanded = false;
    float disclosure_x = 0.0f;
    bool editing = false;      // the inline value editor (`debug_edit`) is on this row
  };
  // The one value being edited: its own slot so the displayed text keeps its
  // storage from frame to frame.
  struct DebugEditPaint {
    SDL_FRect field{};
    float text_x = 0.0f;
    std::string text;
    SDL_FRect selection{};  // empty: none
    SDL_FRect caret{};      // empty: not shown (the window is unfocused, or composing)
  };
  struct DebugPaneBodyPaint {
    bool visible = false;
    float line_height = 0.0f;
    util::InlineVector<DebugModeTabPaint, 4> tabs;
    std::vector<DebugPaneRowPaint> rows;
    DebugEditPaint edit;
    TerminalHintPaint hint;  // the empty surface's sentence
    std::optional<PanelScrollbarPaint> scrollbar;
  };
  DebugPaneBodyPaint debug_pane;

  // ---- the Source Control panel's head (derived-damage 4.1, clip-discipline 4.1) ----
  // Laid out by PrepareGitPanelHeadFrame; PaintGitPanelHead draws only this.
  struct GitPanelHeadPaint {
    bool visible = false;
    // The presentation the frame paints, held so its labels outlive a mid-frame
    // rebuild of the memo (soak eab90dac1acf), and so whatever hit-tests the panel
    // can read the one the user sees.
    std::shared_ptr<const GitSidebarPresentation> presentation;
    std::vector<GitPanelButtonPaint> buttons;
    std::vector<GitPanelTextPaint> lines;
    GitPanelFieldPaint subject;
    GitPanelFieldPaint body;
    std::string subject_text;  // the subject's visible slice; `subject.lines` views it
    SDL_FRect caret{};         // the focused field's caret, laid out even while the blink hides it
    bool caret_on = false;
    // The draft's controls as laid out, for the commit workflow's hit-testing;
    // empty when a control has no room or the draft is closed.
    SDL_FRect commit_button{};
    int body_visible_rows = 0;
  };
  GitPanelHeadPaint git_panel;

  // ---- the notification toasts (derived-damage 3.1, clip-discipline stage 3) ----
  // The builder's view model, composed once at prep (PrepareNotificationToastsFrame)
  // instead of in paint; its views point into the service's rows, alive for the
  // frame. PaintNotificationToasts draws only this.
  NotificationsViewModel notification_toasts;
};

// The fingerprint of everything a status-bar segment paints. The structured
// binding names every field: one added to StatusBarSegmentPaint stops this from
// compiling until it is folded in too, so paint and fingerprint cannot drift.
inline std::uint64_t FingerprintOf(const PreparedFrameState::StatusBarSegmentPaint& segment,
                                   const SDL_Color& background) {
  const auto& [row, text, color, hovered] = segment;
  return PaintFingerprint().Add(row).Add(text).Add(color).Add(hovered).Add(background).value();
}

// The fingerprint of everything a panel tab paints, its palette's colours given
// by the caller. The structured binding names every field, as for the status bar.
inline std::uint64_t FingerprintOf(const PreparedFrameState::PanelTabPaint& tab, std::uint64_t palette) {
  const auto& [rect, close_rect, title, badge_text, badge_color, secondary_title, remote_state, show_badge, active,
               hovered, close_hovered, dimmed, activity_dot, bell_mark] = tab;
  return PaintFingerprint()
      .Add(rect)
      .Add(close_rect)
      .Add(title)
      .Add(badge_text)
      .Add(badge_color)
      .Add(secondary_title)
      .Add(remote_state.has_value())
      .Add(remote_state.value_or(editor::WelcomeStageState::Done))
      .Add(show_badge)
      .Add(active)
      .Add(hovered)
      .Add(close_hovered)
      .Add(dimmed)
      .Add(activity_dot)
      .Add(bell_mark)
      .Add(palette)
      .value();
}

inline std::uint64_t FingerprintOf(const PreparedFrameState::PanelButtonPaint& button, std::uint64_t palette) {
  const auto& [rect, hovered, hidden] = button;
  return PaintFingerprint().Add(rect).Add(hovered).Add(hidden).Add(palette).value();
}

// An Output row: its geometry, the entry's serial for its text (see
// OutputRowPaint::serial; the views' sizes are folded in as a check on that), how
// it was highlighted, and the colours and face it paints with (`style`).
inline std::uint64_t FingerprintOf(const PreparedFrameState::OutputRowPaint& row, std::uint64_t style) {
  const auto& [rect, text_x, text_width, serial, text, snippet, prefix, code, highlighted, highlight_path] = row;
  return PaintFingerprint()
      .Add(rect)
      .Add(text_x)
      .Add(text_width)
      .Add(serial)
      .Add(text.size())
      .Add(snippet)
      .Add(prefix.size())
      .Add(code.size())
      .Add(highlighted != nullptr)
      .Add(highlight_path)
      .Add(style)
      .value();
}

inline std::uint64_t FingerprintOf(const PreparedFrameState::TerminalHintPaint& hint, std::uint64_t style) {
  const auto& [rect, text] = hint;
  return PaintFingerprint().Add(rect).Add(text).Add(style).value();
}

inline std::uint64_t FingerprintOf(const PreparedFrameState::TerminalDividerPaint& divider, std::uint64_t style) {
  const auto& [rect, active] = divider;
  return PaintFingerprint().Add(rect).Add(active).Add(style).value();
}

inline std::uint64_t FingerprintOf(const DebugModeTabPaint& tab, std::uint64_t style) {
  const auto& [rect, mode, label, hovered, active] = tab;
  return PaintFingerprint().Add(rect).Add(mode).Add(label).Add(hovered).Add(active).Add(style).value();
}

// A debug-pane row: everything it draws, the inline editor's state when it is the
// row being edited (`edit` is folded in only then).
inline std::uint64_t FingerprintOf(const PreparedFrameState::DebugPaneRowPaint& row,
                                   const PreparedFrameState::DebugEditPaint& edit, std::uint64_t style) {
  const auto& [band, text_y, highlighted, runs, disclosure, expanded, disclosure_x, editing] = row;
  PaintFingerprint fingerprint;
  fingerprint.Add(band).Add(text_y).Add(highlighted).Add(runs.size());
  for (const DebugTextRun& run : runs) {
    const auto& [x, width, color, text] = run;
    fingerprint.Add(x).Add(width).Add(color).Add(text);
  }
  fingerprint.Add(disclosure).Add(expanded).Add(disclosure_x).Add(editing);
  if (editing) {
    const auto& [field, text_x, text, selection, caret] = edit;
    fingerprint.Add(field).Add(text_x).Add(std::string_view(text)).Add(selection).Add(caret);
  }
  return fingerprint.Add(style).value();
}

inline std::uint64_t FingerprintOf(const PreparedFrameState::PanelScrollbarPaint& scrollbar, std::uint64_t style) {
  const auto& [track, thumb, active] = scrollbar;
  return PaintFingerprint().Add(track).Add(thumb).Add(active).Add(style).value();
}

// The Source Control panel's head: a button, a text line, a field. Structured
// bindings name every field, as above.
inline std::uint64_t FingerprintOf(const GitPanelButtonPaint& button, std::uint64_t style) {
  const auto& [rect, label, tone, enabled, hovered, ellipsize] = button;
  return PaintFingerprint()
      .Add(rect)
      .Add(label)
      .Add(tone)
      .Add(enabled)
      .Add(hovered)
      .Add(ellipsize)
      .Add(style)
      .value();
}

inline std::uint64_t FingerprintOf(const GitPanelTextPaint& line, std::uint64_t style) {
  const auto& [rect, text, ink] = line;
  return PaintFingerprint().Add(rect).Add(text).Add(ink).Add(style).value();
}

inline std::uint64_t FingerprintOf(const GitPanelFieldPaint& field, std::uint64_t style) {
  const auto& [frame, focused, text_x, text_width, placeholder, placeholder_y, lines] = field;
  PaintFingerprint fingerprint;
  fingerprint.Add(frame).Add(focused).Add(text_x).Add(text_width).Add(placeholder).Add(placeholder_y);
  fingerprint.Add(lines.size());
  for (const GitPanelFieldLine& line : lines) {
    const auto& [y, text, selection] = line;
    fingerprint.Add(y).Add(text).Add(selection);
  }
  return fingerprint.Add(style).value();
}

}  // namespace microide::workspace
