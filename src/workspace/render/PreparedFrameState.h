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

struct PreparedFrameState {
  // ---- decided once per prepared frame ---------------------------------
  // The caret blink phase. Paint sites read this, never the clock.
  bool caret_visible = false;
  // Whether the focused terminal owns the blinking caret (not its find bar, not a
  // modal): only then does its caret blink; otherwise it shows a steady outline.
  bool terminal_owns_caret = false;
  bool painted_terminal_owns_caret = false;  // outline vs blinking block, as painted
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

  // ---- what the last prepared frame painted ----------------------------
  std::optional<SDL_FRect> painted_caret_rect;
  // The element painted hovered and the tooltip card painted, under the pointer.
  std::optional<SDL_FRect> painted_hover_rect;
  std::optional<SDL_FRect> painted_tooltip_rect;
  // The open menu-bar popup and submenu as painted: which menus, and each row's
  // enabled and checked flags, folded into one value (0: no menu open). A row's
  // flags follow app state -- a terminal started, a language server ready -- and
  // that state changes under an open menu with nothing repainting it.
  std::uint64_t painted_menu_rows_signature = 0;
  // The open menu-bar popup's and submenu's rects as prep laid them out (nullopt:
  // none open). Paint draws them from here. Their height follows rows that come and
  // go with the language server's readiness, which another thread moves: prep and
  // paint laid them out separately and could disagree, and a popup that shrank was
  // repainted at its new size only, its old lower rows left on screen (soak
  // e4353c8cb483: an editor's context menu over a file that lost its server).
  std::optional<SDL_FRect> painted_menu_popup_rect;
  std::optional<SDL_FRect> painted_submenu_rect;
  // The same flags per item index (bit 0 enabled, bit 1 checked), for the open
  // popup and its submenu: paint draws them from here, not from live state. A
  // flag read twice in one frame could differ -- a terminal's exit lands on its
  // reader thread -- and the partial frame and a full render of it disagreed.
  std::vector<std::uint8_t> menu_popup_row_flags;
  std::vector<std::uint8_t> menu_submenu_row_flags;
  std::size_t painted_tooltip_text_hash = 0;  // the same card can name another tab
  // The editor hover card (hover docs, diagnostics, the blame card), laid out at
  // prep: what this frame paints -- every render of it, partial clips and the
  // verifier's full one alike -- and what the next prep compares with. Paint laid
  // the card out again itself, so a card prep had damaged nothing for could be
  // painted, or one it had kept dropped, and its pixels outside the frame's clips
  // stayed on screen (TD-2026-10-22-377: a blame card left on a split's divider).
  std::optional<EditorHoverPopupLayout> painted_hover_popup;
  // The signature-help card the same way: its rect as this frame paints it
  // (none while a menu holds the pointer, or with no caret on screen) and a hash
  // of its text. Paint laid it out from live state, so a menu closing over a
  // card it had hidden brought the card back in a full render while the partial
  // frame's clips -- the menu's -- never reached it (QA monkey, tooling seed 7).
  std::optional<SDL_FRect> painted_signature_help_rect;
  // The context menu's rect as painted (nullopt: none open). Its rows depend on
  // what it was opened over, and that can change under it; closing it damaged the
  // rect recomputed from the new rows, so a list that got shorter left its lower
  // rows on screen (soak e4353c8cb483). Prep repaints old and new when they differ.
  std::optional<SDL_FRect> painted_context_menu_rect;
  std::size_t painted_signature_help_hash = 0;
  // Per editor pane, the (viewport, content revision) painted.
  std::array<std::uint64_t, kMaxEditorGroups> painted_pane_content{};
  // Per editor pane, the (viewport, content, scroll position) painted.
  std::array<std::uint64_t, kMaxEditorGroups> painted_pane_scroll{};
  // Per editor pane, the fold markers painted (FoldMarkerFingerprint) and the
  // (content, fold model revision) they were hashed at.
  std::array<std::uint64_t, kMaxEditorGroups> painted_fold_revision{};
  std::array<std::uint64_t, kMaxEditorGroups> painted_fold_markers{};
  // Per editor pane, whether it painted its file's plugin decorations (they show
  // only while the buffer is clean).
  std::array<bool, kMaxEditorGroups> painted_decorations{};
  // The breakpoint stores' revisions as painted (gutter dots, Breakpoints pane).
  std::uint64_t painted_breakpoints = 0;
  // The window's regions as painted (sidebar, bottom panel, editor surface, right
  // pane, status bar, tab strip), so one that moves repaints old and new.
  static constexpr std::size_t kTrackedRegions = 6;
  std::array<SDL_FRect, kTrackedRegions> painted_regions{};
  bool painted_regions_valid = false;
  // The surface that painted the focus ring (nullopt: no frame yet). Focus moves
  // from many places -- a key, a click, a control-channel command, a plugin --
  // and only HandleEvent repainted the two surfaces, so moving a tab from the
  // command line drew the editor's ring in a full frame and not in a partial one
  // (QA monkey, editor profile, seeds 3 and 6).
  std::optional<FocusTarget> painted_focus;
  // The plugin status items as painted in the breadcrumb row (a hash of each
  // item's id, text, icon, tone, progress and alignment): an item a reloaded
  // plugin re-added, or one a plugin updated from its thread, changed the row's
  // text in a frame that did not repaint it (QA plugins-reload, Wayland lane).
  std::uint64_t painted_status_items = 0;
  // Each pane's breadcrumb label as painted (a hash; 0 = never painted). The band
  // names the active document, and a change to what it names repaints it whoever
  // made the change: a paste into the Welcome page made it an untitled buffer and
  // damaged the editor alone, and the band kept "Welcome" (soak 7d8ebd1d38c6).
  std::array<std::size_t, kMaxEditorGroups> painted_breadcrumb_labels{};
  // The editor group that was focused when the strips were painted: each strip
  // draws its group's active tab as focused or not, and a click in a group's
  // empty strip moved the focus with no strip repainted (QA monkey, seed 9).
  std::optional<std::size_t> painted_focused_group;
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
  // The tab strips painted mid-slide (kind None: none). A new drag can take a
  // strip's animation slot while it still glides, and that strip's tabs then
  // snap back with no frame of their own.
  struct SlideStrip {
    TabDragKind kind = TabDragKind::None;
    std::size_t group_index = 0;
  };
  std::array<SlideStrip, kMaxAnimatingTabStrips> painted_slide_strips{};
  // Which translucent modal backdrops were painted (a bit per surface). Each dims
  // everything under it, so a modal opening over another changes the whole
  // window, not just the new dialog.
  std::uint8_t painted_backdrops = 0;
  // Whether the focused pane painted the find widget's matches: they show only
  // while the widget is the overlay, and another overlay replacing it (a
  // completion request) took them off the editor without repainting it.
  bool painted_find_matches = false;
  // The active compare tab's scroll position as painted. Typing past a pane's
  // edge scrolls BOTH panes sideways to keep the caret in view while the edit
  // damages only its row, so a moved position repaints the surface.
  struct CompareScroll {
    const void* tab = nullptr;
    int row = 0;
    std::size_t column = 0;
    // Whether the working side is the active one: its caret, selection and
    // inline blame are drawn only then, and a click below the last row cleared
    // it with no repaint (QA monkey, seed 18).
    bool right_active = false;
    bool operator==(const CompareScroll&) const = default;
  };
  CompareScroll painted_compare_scroll{};
  // The focused compare or merge tab's scrollbars as painted (no track: hidden).
  // An edit that widens a line or adds one moves a thumb that sits on none of the
  // rows the edit damages -- typing into a compare tab's long line left the
  // horizontal thumb at its old width (QA monkey, seed 17). As painted_horizontal_bars
  // does for an editor pane: a bar shown or hidden repaints the surface, a moved one
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
  DiffSurfaceBars painted_diff_surface_bars{};
  // A compare or merge surface whose syntax tokenizer is still behind its visible
  // window after this frame's capped step: OnFramePresented requests it, so the
  // NEXT frame takes the next step. Damage requested during prep belongs to the
  // frame being prepared, so prep cannot schedule a following frame itself -- the
  // catch-up stalled after one step, and the rows it then reached on some later
  // frame were never repainted highlighted (QA monkey, commands profile, seed 61).
  std::optional<SDL_FRect> syntax_catch_up_rect;
  // Where the editor split's dividers were painted (a hash of their rects). A
  // divider that moved -- the split's shares changed -- moves every pane beside
  // it; the change is repainted whatever damage the next frame carries.
  std::uint64_t painted_split_dividers = 0;
  // The project strip as painted: its source version (WorkspaceTabStripChrome)
  // folded with its scroll position, active project and width.
  std::uint64_t painted_project_strip_version = 0;
  // Which editor tabs painted a dirty marker (a hash of (group, tab) per dirty
  // tab). Many paths change a buffer's dirty state -- typing, but also Replace
  // All, a banner Reload, an undo back to the saved text, a save -- and each used
  // to have to remember to repaint the tab strip; the ones that forgot left the
  // tab's close button where its dirty dot belonged. Each tab's title is in the
  // hash too: a title re-derived on activation (a split's untitled tab) changed
  // on a frame that damaged no strip.
  std::uint64_t painted_editor_tab_labels = 0;
  // The focused pane's occurrence highlights as painted: its viewport and, per
  // highlighted line, a hash of that line's ranges, by line. The highlight follows
  // the word at the caret, so a keystroke or a click changes it on lines nothing
  // else damages -- and repainting the whole pane for it made every keystroke a
  // full-pane paint (editor_typing_paint, 4.6x). Prep repaints the lines whose
  // highlight changed, no more. `occurrence_lines` is the frame's scratch.
  // Per editor pane, its horizontal scrollbar as painted (no track: hidden). An
  // edit, a paste, a resize or a font change moves it, and it is not on the rows
  // they damage; one that appears or goes also moves the text area. Diffed here
  // rather than guessed at each edit, which repainted the pane on every keystroke
  // into an already-wide line (editor_typing_paint).
  struct HorizontalBar {
    const void* viewport = nullptr;
    std::optional<SDL_FRect> track;
    SDL_FRect thumb{};
  };
  std::array<HorizontalBar, kMaxEditorGroups> painted_horizontal_bars{};
  // The focused pane's bracket-match pair as painted. An edit can make or break the
  // pair on a line it does not damage -- commenting out the closer left the opener
  // lit (QA monkey, seed 40 at 600 steps) -- so prep repaints the lines a pair
  // left or reached.
  const void* painted_bracket_viewport = nullptr;
  std::optional<editor::BracketMatchPair> painted_bracket_pair;
  // Per editor pane, its active viewport and the language its rows were
  // highlighted as. An edit can change the language of the whole buffer --
  // typing before a shebang turns a shell script into plain text -- while it
  // damages only its own line, and every other row kept the old colours (QA
  // monkey, seed 60 at 600 steps). Prep repaints the pane when it changes.
  struct PaintedLanguage {
    const void* viewport = nullptr;
    std::string language;
  };
  std::array<PaintedLanguage, kMaxEditorGroups> painted_languages{};
  const void* painted_occurrence_viewport = nullptr;
  std::vector<std::pair<std::size_t, std::uint64_t>> painted_occurrence_lines;
  std::vector<std::pair<std::size_t, std::uint64_t>> occurrence_lines;
  // The window title (WorkspaceUiText ComposeWindowTitle) and a hash of what it
  // was composed from. It names the focused group's active tab, its dirty mark and
  // the project, so the taskbar and Alt+Tab tell two windows apart; the custom
  // title bar draws it, and the platform window is retitled when it changes.
  std::uint64_t window_title_inputs = 0;
  std::string window_title = "microide";
  // Where the overlay card was painted. An overlay that shrinks or moves while it
  // is up -- the completion list narrowing as the user types, a picker whose
  // result count changed -- repainted only its new rect, leaving the strip it no
  // longer covers on screen. A moved rect repaints old and new.
  std::optional<SDL_FRect> painted_overlay_rect;
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
  DebugToolbar painted_debug_toolbar{};
  // The active merge tab's status as painted (result state, conflicts left, the
  // selected one): a git refresh or the file watcher marks a merge result stale
  // or saved asynchronously, and the header kept reading the old state.
  std::uint64_t painted_merge_status = 0;
  // The active compare tab's review header as painted (tab and summary line). An
  // edit in the working side that adds or removes a hunk changes the line's hunk
  // count while damaging only the edited row.
  std::uint64_t painted_compare_header = 0;
  // The active diff's shape as painted -- each presentation row's kind, model row,
  // hunk and collapsed count -- recomputed only when its presentation revision
  // moves. An edit on the working side can regroup the whole diff (a hunk grows,
  // a collapsed block moves) while its damage names the edited line (QA monkey,
  // seed 2).
  const void* painted_compare_tab = nullptr;
  std::uint64_t painted_compare_presentation_revision = 0;
  std::uint64_t painted_compare_shape = 0;
  // The sidebar list selection painted (view and row): the tree follows the
  // active editor's file, and that selection change damaged nothing.
  std::uint64_t painted_sidebar_selection = 0;

  // ---- derived damage (openspec derived-damage) -------------------------
  // Each migrated surface's units as last painted; prep compares and damages.
  DerivedDamage derived_damage;
  // Per-row keys of the editor pane being prepared (EditorRowUnits): scratch,
  // cleared per pane, capacity kept across frames.
  std::vector<std::uint64_t> editor_row_keys;
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
