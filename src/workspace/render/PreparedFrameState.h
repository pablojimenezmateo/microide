#pragma once

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
#include "terminal/TerminalCell.h"
#include "workspace/render/DerivedDamage.h"
#include "workspace/render/HoverCardPaint.h"
#include "workspace/StatusTone.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/render/TerminalRowPaint.h"
#include "workspace/EditorHoverTypes.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceInteractionState.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

struct PanelScrollbarPaint {
  SDL_FRect track{};
  SDL_FRect thumb{};
  bool active = false;
};
struct TerminalHintPaint {
  SDL_FRect rect{};
  std::string_view text;
};
struct TerminalDividerPaint {
  SDL_FRect rect{};
  bool active = false;
};
struct TerminalGridRowPaint {
  SDL_FRect rect{};
  float text_y = 0.0f;
  const terminal::TerminalLine* line = nullptr;
  TerminalRowEmphasis emphasis;
  std::size_t link_begin = 0;
  std::size_t link_end = 0;
};
struct TerminalCaretPaint {
  enum class Style : std::uint8_t { None, Outline, Solid };
  Style style = Style::None;
  terminal::TerminalCursorShape shape = terminal::TerminalCursorShape::Block;
  SDL_FRect cell{};
  SDL_FRect bounds{};
  const terminal::TerminalCell* glyph = nullptr;
};
struct TerminalGridPanePaint {
  SDL_FRect body{};
  TerminalCaretPaint caret;
  float text_x = 0.0f;
  float text_width = 0.0f;
  float line_height = 0.0f;
  bool reverse_video = false;
  bool selection_live = false;
  std::span<const terminal::TerminalUnderlineColor> underline_colors;
  std::size_t first = 0;
  std::size_t count = 0;
};
struct DebugTextRun {
  float x = 0.0f;
  float width = 0.0f;
  SDL_Color color{};
  std::string_view text;
};
struct DebugModeTabPaint {
  SDL_FRect rect{};
  DebugPaneMode mode = DebugPaneMode::CallStack;
  std::string_view label;
  bool hovered = false;
  bool active = false;
};

enum class GitPanelInk : std::uint8_t { Muted, Secondary, Primary, Error };
enum class GitPanelTone : std::uint8_t { Neutral, Accent, Destructive };
struct GitPanelButtonPaint {
  SDL_FRect rect{};
  std::string_view label;
  GitPanelTone tone = GitPanelTone::Neutral;
  bool enabled = true;
  bool hovered = false;
  bool ellipsize = false;
};
struct GitPanelTextPaint {
  SDL_FRect rect{};
  std::string_view text;
  GitPanelInk ink = GitPanelInk::Muted;
};
struct GitPanelFieldLine {
  float y = 0.0f;
  std::string_view text;
  SDL_FRect selection{};
};
struct GitPanelRowPaint {
  SDL_FRect rect{};
  GitSidebarLine::Kind kind = GitSidebarLine::Kind::Empty;
  std::string_view label;
  float indent = 0.0f;
  bool expanded = false;
  project::GitFileStatus status = project::GitFileStatus::Clean;
  bool selected = false;
  bool hovered = false;
  SDL_FRect base_button{};
  bool base_button_hovered = false;
  bool base_menu_open = false;
};
struct MenuRowPaint {
  SDL_FRect rect{};
  std::uint32_t label_at = 0;
  std::uint32_t label_size = 0;
  std::uint32_t accelerator_at = 0;
  std::uint32_t accelerator_size = 0;
  bool enabled = false;
  bool checked = false;
  bool hovered = false;
  bool separator = false;
};
struct MenuPopupPaint {
  bool visible = false;
  bool follows_pointer = false;
  int pointer_row = -1;
  SDL_FRect rect{};
  std::string text;
  std::vector<MenuRowPaint> rows;

  void Clear() {
    visible = false;
    follows_pointer = false;
    pointer_row = -1;
    rect = SDL_FRect{};
    text.clear();
    rows.clear();
  }
  void SetLabel(MenuRowPaint& row, std::string_view label) {
    row.label_at = static_cast<std::uint32_t>(text.size());
    row.label_size = static_cast<std::uint32_t>(label.size());
    text.append(label);
  }
  void SetAccelerator(MenuRowPaint& row, std::string_view accelerator) {
    row.accelerator_at = static_cast<std::uint32_t>(text.size());
    row.accelerator_size = static_cast<std::uint32_t>(accelerator.size());
    text.append(accelerator);
  }
  std::string_view Label(const MenuRowPaint& row) const {
    return std::string_view(text).substr(row.label_at, row.label_size);
  }
  std::string_view Accelerator(const MenuRowPaint& row) const {
    return std::string_view(text).substr(row.accelerator_at, row.accelerator_size);
  }
};
enum class MenuPopupSlot : std::uint8_t { Popup, Submenu, Overflow, Context, Count };

struct SidebarModeTabPaint {
  SDL_FRect rect{};
  SidebarMode mode = SidebarMode::None;
  std::string_view label;
  bool hovered = false;
  bool active = false;
};
struct TreeSidebarButtonPaint {
  SDL_FRect rect{};
  std::string_view label;
  bool enabled = false;
  bool hovered = false;
};
struct SearchSidebarFieldPaint {
  SDL_FRect rect{};
  SDL_FRect bounds{};
  std::string_view text;
  bool focused = false;
  bool editing = false;
};
struct SearchSidebarButtonPaint {
  SDL_FRect rect{};
  std::string_view label;
  bool enabled = true;
  bool hovered = false;
  bool active = false;
};
struct SearchSidebarRowPaint {
  SDL_FRect rect{};
  std::string_view text;
  bool file = false;
  bool selected = false;
  bool hovered = false;
  std::size_t match_start = 0;
  std::size_t match_length = 0;
};
struct TreeSidebarRowPaint {
  SDL_FRect rect{};
  std::string_view label;
  float indent = 0.0f;
  bool is_directory = false;
  bool expanded = false;
  bool selected = false;
  bool hovered = false;
  bool muted = false;
  project::GitFileStatus git_status = project::GitFileStatus::Clean;
  SDL_Color git_color{};
  bool has_icon = false;
  editor::GutterIconShape icon_shape = editor::GutterIconShape::Dot;
  SDL_Color icon_color{};
};
struct GitPanelFieldPaint {
  SDL_FRect frame{};
  bool focused = false;
  float text_x = 0.0f;
  float text_width = 0.0f;
  std::string_view placeholder;
  float placeholder_y = 0.0f;
  std::vector<GitPanelFieldLine> lines;
};

struct DiffSurfacePaint {
  bool render_whitespace = false;
  bool indent_guides = true;
  bool bracket_match = true;
  bool line_numbers = true;
  bool vertical_bar_dragged = false;
  bool horizontal_bar_dragged = false;
  bool left_divider_dragged = false;
  bool right_divider_dragged = false;
  std::optional<SDL_FPoint> pointer;
  std::optional<editor::EditorBlameOverlay> blame;
  std::span<const editor::PublishedDiagnostic> diagnostics;
  const editor::FileDecorations* decorations = nullptr;
};

enum class PaintPass : std::uint8_t { Editor, WindowChrome, Sidebar, RightPane, BottomPanel, Count };
enum class ChromePainter : std::uint8_t { Editor, WindowChrome, Sidebar, RightPane, BottomPanel, Always };
static_assert(static_cast<int>(ChromePainter::Editor) == static_cast<int>(PaintPass::Editor) &&
                  static_cast<int>(ChromePainter::WindowChrome) == static_cast<int>(PaintPass::WindowChrome) &&
                  static_cast<int>(ChromePainter::Sidebar) == static_cast<int>(PaintPass::Sidebar) &&
                  static_cast<int>(ChromePainter::RightPane) == static_cast<int>(PaintPass::RightPane) &&
                  static_cast<int>(ChromePainter::BottomPanel) == static_cast<int>(PaintPass::BottomPanel) &&
                  static_cast<int>(ChromePainter::Always) == static_cast<int>(PaintPass::Count),
              "a skippable painter is its pass");

class PaintPassExtent {
 public:
  static constexpr std::size_t kSeparateRects = 24;
  void Clear() {
    rects_.clear();
    spill_.reset();
  }
  void Add(const SDL_FRect& rect) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
      return;
    }
    if (rects_.size() < kSeparateRects) {
      rects_.push_back(rect);
      return;
    }
    if (!spill_.has_value()) {
      spill_ = rect;
      return;
    }
    const float left = std::min(spill_->x, rect.x);
    const float top = std::min(spill_->y, rect.y);
    const float right = std::max(spill_->x + spill_->w, rect.x + rect.w);
    const float bottom = std::max(spill_->y + spill_->h, rect.y + rect.h);
    spill_ = SDL_FRect{left, top, right - left, bottom - top};
  }
  bool Touches(const SDL_FRect& clip) const {
    const auto overlaps = [&clip](const SDL_FRect& r) {
      return clip.x < r.x + r.w && r.x < clip.x + clip.w && clip.y < r.y + r.h && r.y < clip.y + clip.h;
    };
    return std::any_of(rects_.begin(), rects_.end(), overlaps) || (spill_.has_value() && overlaps(*spill_));
  }
  bool Empty() const { return rects_.size() == 0 && !spill_.has_value(); }
  bool Covers(const SDL_FRect& drawn) const {
    constexpr float kSlack = 1.0f;
    const auto holds = [&drawn](const SDL_FRect& r) {
      return drawn.x >= r.x - kSlack && drawn.y >= r.y - kSlack && drawn.x + drawn.w <= r.x + r.w + kSlack &&
             drawn.y + drawn.h <= r.y + r.h + kSlack;
    };
    return std::any_of(rects_.begin(), rects_.end(), holds) || (spill_.has_value() && holds(*spill_));
  }
  static bool CoversThunk(const void* extent, const SDL_FRect& drawn) {
    return static_cast<const PaintPassExtent*>(extent)->Covers(drawn);
  }
  std::string Describe() const {
    std::string out;
    const auto append = [&out](const SDL_FRect& r) {
      out += out.empty() ? "" : ";";
      out += std::to_string(static_cast<int>(r.x)) + "," + std::to_string(static_cast<int>(r.y)) + "," +
             std::to_string(static_cast<int>(r.w)) + "," + std::to_string(static_cast<int>(r.h));
    };
    for (const SDL_FRect& r : rects_) {
      append(r);
    }
    if (spill_.has_value()) {
      append(*spill_);
    }
    return out.empty() ? std::string("none") : out;
  }
  static std::string DescribeThunk(const void* extent) {
    return static_cast<const PaintPassExtent*>(extent)->Describe();
  }

 private:
  util::InlineVector<SDL_FRect, kSeparateRects> rects_;
  std::optional<SDL_FRect> spill_;
};

struct PaintPassExtents {
  std::array<bool, static_cast<std::size_t>(PaintPass::Count)> region_shown{};
  std::array<PaintPassExtent, static_cast<std::size_t>(PaintPass::Count)> extents;
  void Clear() {
    region_shown.fill(false);
    for (PaintPassExtent& extent : extents) {
      extent.Clear();
    }
  }
  PaintPassExtent& operator[](PaintPass pass) { return extents[static_cast<std::size_t>(pass)]; }
  bool Runs(PaintPass pass, const std::optional<SDL_FRect>& hint) const {
    const std::size_t i = static_cast<std::size_t>(pass);
    return region_shown[i] && (!hint.has_value() || extents[i].Touches(*hint));
  }
};

struct PreparedFrameState {
  bool caret_visible = false;
  bool terminal_owns_caret = false;
  std::optional<SDL_FRect> pointer_occluder;

  std::array<editor::EditorViewMetrics, kMaxEditorGroups> pane_metrics{};
  std::array<bool, kMaxEditorGroups> pane_metrics_valid{};

  std::array<MenuPopupPaint, static_cast<std::size_t>(MenuPopupSlot::Count)> menu_popups{};
  CardPaint editor_hover_card;
  CardPaint signature_card;
  CardPaint tooltip_card;
  struct WindowChromePaint {
    struct MenuItem {
      SDL_FRect rect{};
      std::string_view label;
      bool active = false;
      bool hovered = false;
    };
    struct WindowButton {
      SDL_FRect rect{};
      int id = 0;
      bool hovered = false;
    };
    struct Group {
      SDL_FRect tab_strip{};
      SDL_FRect breadcrumb{};
      std::string breadcrumb_label;
    };
    struct StatusItem {
      SDL_FRect rect{};
      std::string text;
      std::string icon;
      StatusTone tone = StatusTone::Default;
      float progress = -1.0f;
      bool hovered = false;
      bool left_aligned = false;
    };
    std::vector<MenuItem> menu_items;
    std::optional<SDL_FRect> chevron;
    bool chevron_hovered = false;
    std::optional<SDL_FRect> title_rect;
    std::vector<WindowButton> window_buttons;
    bool expanded = false;
    std::vector<Group> groups;
    std::size_t focused_group = 0;
    std::vector<StatusItem> status_items;
    std::optional<SDL_FRect> tab_body_drop;
    std::optional<SDL_FRect> path_drop;
    std::optional<SDL_FRect> path_ghost;
    std::string path_label;
  };
  WindowChromePaint window_chrome;
  struct EditorPassPane {
    enum class Kind : std::uint8_t { Editor, Compare, Merge };
    Kind kind = Kind::Editor;
    std::size_t group = 0;
    SDL_FRect area{};
    bool focused = false;
    bool active = false;
    CompareTabState* compare = nullptr;
    MergeTabState* merge = nullptr;
  };
  std::vector<EditorPassPane> editor_pass_panes;
  struct OverlayPointerPaint {
    std::vector<bool> row_hovered;
    bool scrollbar_dragged = false;
    std::array<bool, kFindWidgetMaxToggles> toggle_hovered{};
    bool prev_hovered = false;
    bool next_hovered = false;
    bool close_hovered = false;
    bool replace_hovered = false;
    bool replace_all_hovered = false;
  };
  OverlayPointerPaint overlay_pointer;
  struct EditorAreaChromePaint {
    struct Divider {
      SDL_FRect rect{};
      bool lit = false;
    };
    struct Banner {
      bool shown = false;
      SDL_FRect strip{};
      std::string message;
      EditorBannerButtonLayout buttons;
      std::array<bool, kMaxEditorBannerButtons> hovered{};
      bool dismiss_hovered = false;
    };
    std::vector<Divider> dividers;
    Banner banner;
    bool focus_ring = false;
  };
  EditorAreaChromePaint editor_area_chrome;
  struct FrameBasePaint {
    bool sidebar_visible = false;
    bool bottom_panel_visible = false;
    bool sidebar_divider_lit = false;
  };
  FrameBasePaint frame_base;
  struct PromptDialogPaint {
    bool shown = false;
    std::string title;
    std::string message;
    std::string detail;
    bool text_input = false;
    std::string input_text;
    int laid_out_buttons = 0;
    std::size_t button_count = 0;
    std::array<std::string_view, 4> labels{};
    std::array<bool, 4> hovered{};
    int selected = -1;
  };
  PromptDialogPaint prompt_dialog;
  PromptDialogPaint dirty_prompt_dialog;
  CardPaint text_input_marks;
  CardPaint text_input_caret;
  std::uint64_t status_items_hash = 0;
  std::optional<SDL_FRect> menu_popup_rect;
  std::optional<SDL_FRect> menu_submenu_rect;
  std::optional<SDL_FRect> context_menu_rect;
  const void* focused_editor = nullptr;
  bool focus_autosave_armed = false;
  bool syntax_catch_up_owed = false;
  const void* bracket_viewport = nullptr;
  std::optional<editor::BracketMatchPair> bracket_pair;
  const void* occurrence_viewport = nullptr;
  std::vector<std::pair<std::size_t, std::uint64_t>> occurrence_lines;
  std::uint64_t window_title_inputs = 0;
  std::string window_title = "microide";
  struct DebugToolbarPaint {
    bool visible = false;
    bool stopped = false;
    DebugToolbarLayout layout;
    std::array<bool, static_cast<std::size_t>(DebugToolbarButton::Count)> pointer_over{};
  };
  DebugToolbarPaint debug_toolbar;

  DerivedDamage derived_damage;
  std::vector<std::uint64_t> editor_row_keys;
  struct EditorPanePaint {
    bool valid = false;
    editor::TextViewport* viewport = nullptr;
    editor::FoldingModel* folding = nullptr;
    SDL_FRect rect{};
    bool active = false;
    editor::EditorViewMetrics metrics{};
    editor::EditorViewModel view_model;
    std::vector<std::size_t> sticky_lines_storage;
    std::vector<editor::RowGap> row_gaps_storage;
    std::vector<editor::RowGapContent> row_gap_contents_storage;
    const editor::WelcomeViewModel* welcome = nullptr;
    std::vector<editor::PublishedDiagnostic> filtered_diagnostics;
    std::span<const editor::PublishedDiagnostic> diagnostics;
    const editor::FileDecorations* decorations = nullptr;
    const editor::EditorBlameOverlay* blame = nullptr;
    const std::optional<editor::EditorBlameOverlay>* blame_source = nullptr;
    std::uint64_t diagnostics_revision = 0;
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
    const BufferSearchState* ruler_search = nullptr;
    bool vertical_bar_dragged = false;
    bool horizontal_bar_dragged = false;
    std::optional<editor::TextPosition> drop_indicator;
  };
  std::array<EditorPanePaint, kMaxEditorGroups> editor_pane_paint{};
  std::array<DiffSurfacePaint, kMaxEditorGroups> diff_surface_paint{};
  struct StatusBarSegmentPaint {
    SDL_FRect row{};
    std::string_view text;
    SDL_Color color{};
    bool hovered = false;
  };
  StatusBarViewModel status_bar;
  std::vector<StatusBarSegmentPaint> status_bar_segments;

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
    std::size_t hidden = 0;
  };
  struct PanelHeaderPaint {
    bool visible = false;
    SDL_FRect header{};
    std::vector<PanelTabPaint> tabs;
    std::string text;
    std::string_view label;
    PanelButtonPaint overflow_left;
    PanelButtonPaint overflow_right;
    PanelButtonPaint new_terminal;
    PanelButtonPaint maximize;
    bool maximized = false;
    SDL_FRect divider{};
    bool divider_active = false;
    std::vector<VisibleStripTab> drag_tabs_storage;
    const std::vector<VisibleStripTab>* drag_tabs = nullptr;
    bool dragging = false;
    std::size_t drag_source = 0;
    float drag_pointer_x = 0.0f;
    float drag_grab_offset_x = 0.0f;
  };
  PanelHeaderPaint panel_header;

  struct TabStripPaint {
    bool shown = false;
    bool placeholder = false;
    std::vector<PanelTabPaint> tabs;
    std::string text;
    PanelButtonPaint overflow_left;
    PanelButtonPaint overflow_right;
    bool dragging = false;
    std::vector<VisibleStripTab> drag_tabs;
    SDL_FRect drag_tabs_strip{};
    std::size_t drag_source = 0;
    float drag_pointer_x = 0.0f;
    float drag_grab_offset_x = 0.0f;
  };
  TabStripPaint project_strip;
  std::array<TabStripPaint, kMaxEditorGroups> editor_strips;

  struct OutputRowPaint {
    SDL_FRect rect{};
    float text_x = 0.0f;
    float text_width = 0.0f;
    std::uint64_t serial = 0;
    std::string_view text;
    bool snippet = false;
    std::string_view prefix;
    std::string_view code;
    const editor::HighlightedLine* highlighted = nullptr;
    std::uint64_t highlight_path = 0;
  };
  using PanelScrollbarPaint = workspace::PanelScrollbarPaint;
  struct OutputBodyPaint {
    bool visible = false;
    std::vector<OutputRowPaint> rows;
    SDL_FRect hint_rect{};
    std::string_view hint;
    std::optional<PanelScrollbarPaint> scrollbar;
  };
  OutputBodyPaint output_body;

  using TerminalHintPaint = workspace::TerminalHintPaint;
  using TerminalDividerPaint = workspace::TerminalDividerPaint;
  struct TerminalChromePaint {
    bool visible = false;
    TerminalHintPaint no_session;
    util::InlineVector<TerminalHintPaint, kMaxTerminalPanes> hints;
    util::InlineVector<std::optional<PanelScrollbarPaint>, kMaxTerminalPanes> scrollbars;
    util::InlineVector<TerminalDividerPaint, kMaxTerminalPanes> dividers;
  };
  TerminalChromePaint terminal_chrome;

  struct TerminalGridPaint {
    bool visible = false;
    util::InlineVector<TerminalGridPanePaint, kMaxTerminalPanes> panes;
    std::vector<TerminalGridRowPaint> rows;
  };
  TerminalGridPaint terminal_grid;

  struct SettingsCardPaint {
    SettingsOverlayViewModel vm;
    SDL_FRect backdrop{};
    int hovered_category = -1;
    int hovered_row = -1;
    bool scrollbar_active = false;
    bool category_scrollbar_active = false;
    bool picker_scrollbar_active = false;
    bool tooltip_visible = false;
    SDL_FRect tooltip_rect{};
    std::vector<std::string> tooltip_lines;
  };
  SettingsCardPaint settings_card;

  struct DebugPaneRowPaint {
    SDL_FRect band{};
    float text_y = 0.0f;
    bool highlighted = false;
    util::InlineVector<DebugTextRun, 4> runs;
    bool disclosure = false;
    bool expanded = false;
    float disclosure_x = 0.0f;
    bool editing = false;
  };
  struct DebugEditPaint {
    SDL_FRect field{};
    float text_x = 0.0f;
    std::string text;
    SDL_FRect selection{};
    SDL_FRect caret{};
  };
  struct DebugPaneBodyPaint {
    bool visible = false;
    float line_height = 0.0f;
    util::InlineVector<DebugModeTabPaint, 4> tabs;
    std::vector<DebugPaneRowPaint> rows;
    DebugEditPaint edit;
    TerminalHintPaint hint;
    std::optional<PanelScrollbarPaint> scrollbar;
  };
  DebugPaneBodyPaint debug_pane;

  struct SidebarModeRowPaint {
    bool visible = false;
    std::array<SidebarModeTabPaint, 3> tabs{};
    int tab_count = 0;
    bool has_overflow = false;
    SDL_FRect overflow_rect{};
    bool overflow_hovered = false;
    bool overflow_active = false;
  };
  SidebarModeRowPaint sidebar_mode_row;
  struct TreeSidebarPaint {
    bool visible = false;
    std::array<TreeSidebarButtonPaint, 2> buttons{};
    std::vector<TreeSidebarRowPaint> rows;
    std::optional<PanelScrollbarPaint> scrollbar;
  };
  TreeSidebarPaint tree_sidebar;
  struct SidebarListRowPaint {
    SDL_FRect rect{};
    float label_x = 0.0f;
    float tree_x = 0.0f;
    bool chevron = false;
    bool collapsed = false;
    bool selected = false;
    bool emphasized = false;
    std::size_t primary_at = 0;
    std::size_t primary_length = 0;
    std::size_t secondary_at = 0;
    std::size_t secondary_length = 0;
  };
  struct SidebarListPaint {
    bool visible = false;
    std::vector<SidebarListRowPaint> rows;
    std::string text;
    SDL_FRect placeholder_box{};
    std::size_t placeholder_at = 0;
    std::size_t placeholder_length = 0;
    bool placeholder_is_error = false;
    std::optional<PanelScrollbarPaint> scrollbar;
  };
  SidebarListPaint sidebar_list;
  struct SearchSidebarPaint {
    bool visible = false;
    std::array<SearchSidebarFieldPaint, 4> fields{};
    std::array<SearchSidebarButtonPaint, 5> buttons{};
    SDL_FRect status_rect{};
    std::string_view status;
    std::vector<SearchSidebarRowPaint> rows;
    SDL_FRect placeholder_rect{};
    std::string_view placeholder;
    bool placeholder_error = false;
    std::optional<PanelScrollbarPaint> scrollbar;
    std::string text;
  };
  SearchSidebarPaint search_sidebar;
  struct GitPanelPaint {
    bool visible = false;
    std::shared_ptr<const GitSidebarPresentation> presentation;
    std::vector<GitPanelButtonPaint> buttons;
    std::vector<GitPanelTextPaint> lines;
    GitPanelFieldPaint subject;
    GitPanelFieldPaint body;
    std::string subject_text;
    SDL_FRect caret{};
    bool caret_on = false;
    SDL_FRect commit_button{};
    int body_visible_rows = 0;
    ScrollableListLayout list;
    std::vector<GitPanelRowPaint> rows;
    std::optional<PanelScrollbarPaint> scrollbar;
    std::vector<const GitSidebarRowViewModel*> row_by_entry;
  };
  GitPanelPaint git_panel;

  NotificationsViewModel notification_toasts;

  PaintPassExtents paint_pass_extents;
};

inline std::uint64_t FingerprintOf(const PreparedFrameState::StatusBarSegmentPaint& segment,
                                   const SDL_Color& background) {
  const auto& [row, text, color, hovered] = segment;
  return PaintFingerprint().Add(row).Add(text).Add(color).Add(hovered).Add(background).value();
}

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

inline std::uint64_t FingerprintOf(const PreparedFrameState::WindowChromePaint::MenuItem& item) {
  const auto& [rect, label, active, hovered] = item;
  return PaintFingerprint().Add(rect).Add(label).Add(active).Add(hovered).value();
}
inline std::uint64_t FingerprintOf(const PreparedFrameState::WindowChromePaint::WindowButton& button, bool expanded) {
  const auto& [rect, id, hovered] = button;
  return PaintFingerprint().Add(rect).Add(id).Add(hovered).Add(expanded).value();
}
inline std::uint64_t FingerprintOf(const PreparedFrameState::WindowChromePaint::StatusItem& item) {
  const auto& [rect, text, icon, tone, progress, hovered, left_aligned] = item;
  return PaintFingerprint()
      .Add(rect)
      .Add(std::string_view(text))
      .Add(std::string_view(icon))
      .Add(tone)
      .Add(progress)
      .Add(hovered)
      .Add(left_aligned)
      .value();
}

inline std::uint64_t FingerprintOf(const PreparedFrameState::EditorAreaChromePaint::Banner& banner) {
  const auto& [shown, strip, message, buttons, hovered, dismiss_hovered] = banner;
  PaintFingerprint key;
  key.Add(shown);
  if (!shown) {
    return key.value();
  }
  key.Add(strip).Add(std::string_view(message)).Add(buttons.buttons.size()).Add(buttons.dismiss).Add(dismiss_hovered);
  for (std::size_t i = 0; i < buttons.buttons.size(); ++i) {
    key.Add(buttons.rects[i]).Add(buttons.buttons[i].label).Add(buttons.buttons[i].destructive).Add(hovered[i]);
  }
  return key.value();
}

inline void AddFindWidget(PaintFingerprint& key, const OverlayFindWidgetViewModel& find) {
  const auto& [fw, replace_mode, search_focused, replace_focused, toggles, has_matches, has_query,
               search_display_text, replace_display_text, count_text] = find;
  key.Add(fw.widget).Add(fw.search_field).Add(fw.replace_field).Add(fw.count_rect).Add(fw.toggle_count);
  key.Add(fw.prev_button).Add(fw.next_button).Add(fw.close_button).Add(fw.replace_button).Add(fw.replace_all_button);
  key.Add(fw.replace_mode).Add(replace_mode).Add(search_focused).Add(replace_focused);
  for (std::size_t i = 0; i < toggles.size(); ++i) {
    key.Add(fw.toggle_buttons[i]).Add(toggles[i].label).Add(toggles[i].active);
  }
  key.Add(has_matches).Add(has_query).Add(search_display_text).Add(replace_display_text).Add(count_text);
}
inline std::uint64_t FingerprintOfOverlayCard(const OverlaySurfaceViewModel& vm) {
  const auto& [visible, mode, scroll_row, current_surface, buffer_search_query_text, caret_anchored, overlay_rect,
               list_layout, total_rows, selected_row, title, note, note_x, context_label, has_query_field,
               query_surface, query_row_y, query_display_text, summary_line, summary_y, hint, hint_x, error_line,
               error_at_title_row, empty_label, rows, find_widget, label_storage] = vm;
  (void)rows;
  (void)label_storage;
  PaintFingerprint key;
  key.Add(visible).Add(mode).Add(scroll_row).Add(current_surface).Add(buffer_search_query_text);
  key.Add(caret_anchored).Add(overlay_rect).Add(list_layout.list_rect).Add(list_layout.row_height);
  key.Add(list_layout.row_step).Add(list_layout.visible_units).Add(total_rows).Add(selected_row);
  key.Add(title).Add(note).Add(note_x).Add(context_label).Add(has_query_field).Add(query_surface);
  key.Add(query_row_y).Add(query_display_text).Add(summary_line).Add(summary_y).Add(hint).Add(hint_x);
  key.Add(error_line).Add(error_at_title_row).Add(empty_label);
  AddFindWidget(key, find_widget);
  return key.value();
}

inline std::uint64_t FingerprintOf(const PreparedFrameState::DebugToolbarPaint& toolbar) {
  const auto& [visible, stopped, layout, pointer_over] = toolbar;
  PaintFingerprint key;
  key.Add(visible);
  if (!visible) {
    return key.value();
  }
  key.Add(stopped).Add(layout.widget).Add(layout.button_count);
  for (std::size_t i = 0; i < layout.button_count; ++i) {
    key.Add(layout.buttons[i]).Add(layout.kinds[i]).Add(pointer_over[i]);
  }
  return key.value();
}

inline std::uint64_t FingerprintOf(const PreparedFrameState::PromptDialogPaint& dialog) {
  const auto& [shown, title, message, detail, text_input, input_text, laid_out_buttons, button_count, labels, hovered,
               selected] = dialog;
  PaintFingerprint key;
  key.Add(shown);
  if (!shown) {
    return key.value();
  }
  key.Add(std::string_view(title)).Add(std::string_view(message)).Add(std::string_view(detail));
  key.Add(text_input).Add(std::string_view(input_text)).Add(laid_out_buttons).Add(button_count).Add(selected);
  for (std::size_t i = 0; i < labels.size(); ++i) {
    key.Add(labels[i]).Add(hovered[i]);
  }
  return key.value();
}

inline std::uint64_t FingerprintOf(const PreparedFrameState::PanelButtonPaint& button, std::uint64_t palette) {
  const auto& [rect, hovered, hidden] = button;
  return PaintFingerprint().Add(rect).Add(hovered).Add(hidden).Add(palette).value();
}

inline std::uint64_t FingerprintOf(const PreparedFrameState::TabStripPaint& strip, std::uint64_t palette) {
  const auto& [shown, placeholder, tabs, text, overflow_left, overflow_right, dragging, drag_tabs, drag_tabs_strip,
               drag_source, drag_pointer_x, drag_grab_offset_x] = strip;
  (void)text;
  PaintFingerprint key;
  key.Add(shown).Add(placeholder).Add(tabs.size());
  for (const PreparedFrameState::PanelTabPaint& tab : tabs) {
    key.Add(FingerprintOf(tab, palette));
  }
  key.Add(FingerprintOf(overflow_left, palette)).Add(FingerprintOf(overflow_right, palette)).Add(dragging);
  if (dragging) {
    key.Add(drag_tabs_strip).Add(drag_source).Add(drag_pointer_x).Add(drag_grab_offset_x);
    if (const VisibleStripTab* ghost = DraggedStripTab(drag_tabs, drag_source); ghost != nullptr) {
      key.Add(ghost->rect).Add(std::string_view(ghost->display_title)).Add(std::string_view(ghost->badge_text));
      key.Add(ghost->badge_color).Add(ghost->show_badge).Add(ghost->remote_state.has_value());
      key.Add(ghost->remote_state.value_or(editor::WelcomeStageState::Done));
    }
  }
  return key.value();
}

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

inline std::uint64_t FingerprintOf(const GitPanelRowPaint& row, std::uint64_t style) {
  const auto& [rect, kind, label, indent, expanded, status, selected, hovered, base_button, base_button_hovered,
               base_menu_open] = row;
  return PaintFingerprint()
      .Add(rect)
      .Add(kind)
      .Add(label)
      .Add(indent)
      .Add(expanded)
      .Add(status)
      .Add(selected)
      .Add(hovered)
      .Add(base_button)
      .Add(base_button_hovered)
      .Add(base_menu_open)
      .Add(style)
      .value();
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

}
