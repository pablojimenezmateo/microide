#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kMenuBarHeight = 25.0f;
constexpr float kWindowFrameHitThickness = 6.0f;
constexpr float kWindowControlButtonGap = 4.0f;
constexpr float kWindowControlButtonRightInset = 8.0f;
constexpr float kProjectTabStripHeight = 32.0f;
constexpr float kTabStripHeight = 34.0f;
constexpr float kHeaderHeight = 26.0f;
constexpr float kDivider = 1.0f;
constexpr float kResizeHandleThickness = 6.0f;
constexpr float kBottomPanelHeaderHeight = 28.0f;
constexpr float kTreeIndentWidth = 14.0f;
constexpr float kTreeChevronSlotWidth = 12.0f;
constexpr float kOverlayMinWidth = 520.0f;
constexpr float kOverlayMaxWidth = 840.0f;
constexpr float kOverlayMinHeight = 220.0f;
constexpr float kOverlayMaxHeight = 360.0f;
constexpr float kScrollbarThickness = 10.0f;
constexpr float kScrollbarInset = 2.0f;
constexpr float kScrollbarMinThumbLength = 24.0f;
constexpr float kMinSidebarWidth = 160.0f;
constexpr float kMaxSidebarWidth = 520.0f;
constexpr float kMinEditorAreaWidth = 280.0f;
constexpr float kMinBottomPanelHeight = 96.0f;
constexpr float kMinEditorAreaHeight = 120.0f;
constexpr float kEditorSplitDividerThickness = 6.0f;
constexpr float kMinSplitPaneExtent = 180.0f;
constexpr float kBottomPanelCommandReserveHeight = 56.0f;
constexpr float kBottomPanelCommandPromptHeight = 18.0f;
constexpr float kBottomPanelCommandInset = 10.0f;
constexpr float kBottomPanelCommandTopPadding = 8.0f;
constexpr float kBottomPanelCommandBottomPadding = 8.0f;
constexpr float kTabCloseButtonSize = 14.0f;
constexpr float kTabCloseButtonRightInset = 6.0f;
constexpr float kMenuPopupSeparatorHeight = 8.0f;
constexpr float kMenuPopupItemHeight = 22.0f;
constexpr float kMergeToolbarHeight = 36.0f;
constexpr float kMergeToolbarButtonHeight = 22.0f;
constexpr float kMergeToolbarButtonGap = 8.0f;
constexpr float kMinMergePaneWidth = 140.0f;
constexpr Uint64 kCaretBlinkIntervalMs = 530;
constexpr std::array<std::string_view, 3> kSidebarToolNames = {
    "git",
    "search",
    "tree",
};

constexpr std::array<std::string_view, 3> kFocusTargetNames = {
    "editor",
    "panel",
    "sidebar",
};

constexpr std::array<std::string_view, 2> kToggleValues = {
    "off",
    "on",
};

constexpr std::array<std::string_view, 3> kUiScaleCommands = {
    "down",
    "reset",
    "up",
};

std::size_t MaxVisualColumns(const editor::TextViewport& viewport) {
  return viewport.max_visual_columns();
}

SDL_HitTestResult ResizeHitTestResult(bool left, bool right, bool top, bool bottom) {
  if (top && left) {
    return SDL_HITTEST_RESIZE_TOPLEFT;
  }
  if (top && right) {
    return SDL_HITTEST_RESIZE_TOPRIGHT;
  }
  if (bottom && left) {
    return SDL_HITTEST_RESIZE_BOTTOMLEFT;
  }
  if (bottom && right) {
    return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
  }
  if (top) {
    return SDL_HITTEST_RESIZE_TOP;
  }
  if (bottom) {
    return SDL_HITTEST_RESIZE_BOTTOM;
  }
  if (left) {
    return SDL_HITTEST_RESIZE_LEFT;
  }
  if (right) {
    return SDL_HITTEST_RESIZE_RIGHT;
  }
  return SDL_HITTEST_NORMAL;
}

}  // namespace

std::span<const WorkspaceShell::ActionSpec> WorkspaceShell::ActionSpecs() {
  static const auto kSpecs = std::to_array<ActionSpec>({
      ActionSpec{ActionId::Colorscheme, "colorscheme", "colorscheme [name|list]", "Colorscheme",
                 ""},
      ActionSpec{ActionId::Compare, "compare", "compare [path] [commit-prefix]",
                 "Compare Against...", ""},
      ActionSpec{ActionId::CompareHead, "", "", "Compare Against HEAD", ""},
      ActionSpec{ActionId::Merge, "merge", "merge <base> <incoming> <current> [output]",
                 "Merge Editor", ""},
      ActionSpec{ActionId::CopyAbsolutePath, "", "", "Copy Absolute Path", ""},
      ActionSpec{ActionId::CopyRelativePath, "", "", "Copy Relative Path", ""},
      ActionSpec{ActionId::CreateDirectory, "", "", "New Folder...", ""},
      ActionSpec{ActionId::CreateFile, "", "", "New File...", ""},
      ActionSpec{ActionId::DeletePath, "", "", "Delete...", ""},
      ActionSpec{ActionId::Files, "files", "files [root]", "Find File", "F6"},
      ActionSpec{ActionId::Find, "find", "find <query>", "Find File By Query", ""},
      ActionSpec{ActionId::Focus, "focus", "focus <editor|sidebar|panel>", "Focus", ""},
      ActionSpec{ActionId::Goto, "goto", "goto <line[:col]>", "Go to Line", ""},
      ActionSpec{ActionId::GitRefresh, "git-refresh", "git-refresh", "Refresh Git", ""},
      ActionSpec{ActionId::IndentWidth, "indent-width", "indent-width [n]", "Indent Width",
                 ""},
      ActionSpec{ActionId::Jump, "jump", "jump <line[:col]>", "Jump Relative", ""},
      ActionSpec{ActionId::Open, "open", "open <path>", "Open File", ""},
      ActionSpec{ActionId::OpenSelectedTreeItem, "", "", "Open", ""},
      ActionSpec{ActionId::OpenSelectedTreeItemInNewTab, "", "", "Open in New Tab", ""},
      ActionSpec{ActionId::ProjectClose, "project-close", "project-close", "Close Project", ""},
      ActionSpec{ActionId::ProjectNext, "project-next", "project-next", "Next Project", ""},
      ActionSpec{ActionId::ProjectOpen, "project-open", "project-open [path]", "Open Project",
                 ""},
      ActionSpec{ActionId::ProjectPrev, "project-prev", "project-prev", "Previous Project", ""},
      ActionSpec{ActionId::ProjectSearch, "project-search", "project-search [query]",
                 "Find in Project", "Ctrl+Shift+F"},
      ActionSpec{ActionId::Quit, "quit", "quit", "Quit", ""},
      ActionSpec{ActionId::RenamePath, "", "", "Rename...", ""},
      ActionSpec{ActionId::Reopen, "reopen", "reopen", "Reopen", ""},
      ActionSpec{ActionId::Save, "save", "save", "Save", "Ctrl+S"},
      ActionSpec{ActionId::Search, "search", "search <query>", "Find in Buffer", "Ctrl+F"},
      ActionSpec{ActionId::SidebarClose, "sidebar-close", "sidebar-close", "Close Sidebar", ""},
      ActionSpec{ActionId::SidebarHide, "sidebar-hide", "sidebar-hide", "Hide Sidebar", ""},
      ActionSpec{ActionId::SidebarShow, "sidebar-show", "sidebar-show [tool]", "Show Sidebar",
                 ""},
      ActionSpec{ActionId::SidebarToggle, "sidebar-toggle", "sidebar-toggle [tool]",
                 "Toggle Sidebar", "F8", true},
      ActionSpec{ActionId::SidebarWidth, "sidebar-width", "sidebar-width <n>", "Sidebar Width",
                 ""},
      ActionSpec{ActionId::SoftTabs, "soft-tabs", "soft-tabs [on|off]", "Soft Tabs", ""},
      ActionSpec{ActionId::SplitFirst, "split-first", "split-first", "First Split", ""},
      ActionSpec{ActionId::SplitLast, "split-last", "split-last", "Last Split", ""},
      ActionSpec{ActionId::SplitNext, "split-next", "split-next", "Next Split", ""},
      ActionSpec{ActionId::SplitPrev, "split-prev", "split-prev", "Previous Split", ""},
      ActionSpec{ActionId::Tab, "tab", "tab [path]", "New Tab", ""},
      ActionSpec{ActionId::TabSize, "tab-size", "tab-size [n]", "Tab Size", ""},
      ActionSpec{ActionId::TabMove, "tabmove", "tabmove <n>", "Move Tab", ""},
      ActionSpec{ActionId::TabSwitch, "tabswitch", "tabswitch <tab>", "Switch Tab", ""},
      ActionSpec{ActionId::Term, "term", "term [command]", "New Terminal", ""},
      ActionSpec{ActionId::Tree, "tree", "tree [root]", "Show Tree", ""},
      ActionSpec{ActionId::TreeRefresh, "tree-refresh", "tree-refresh", "Refresh Tree", ""},
      ActionSpec{ActionId::UiScale, "ui-scale", "ui-scale [n|up|down|reset]", "UI Scale", ""},
      ActionSpec{ActionId::Unsplit, "unsplit", "unsplit", "Close Split", ""},
      ActionSpec{ActionId::Vsplit, "vsplit", "vsplit [path]", "Split Right", ""},
      ActionSpec{ActionId::CloseActiveTab, "", "", "Close Tab", "Ctrl+W"},
      ActionSpec{ActionId::CopyLastTerminalCommand, "", "", "Copy Last Command + Output", ""},
      ActionSpec{ActionId::CopySelection, "", "", "Copy", "Ctrl+C"},
      ActionSpec{ActionId::CopySelectionWithContext, "", "", "Copy with Context", ""},
      ActionSpec{ActionId::CutSelection, "", "", "Cut", "Ctrl+X"},
      ActionSpec{ActionId::OpenCommandPrompt, "", "", "Command Prompt", "Ctrl+E"},
      ActionSpec{ActionId::PasteClipboard, "", "", "Paste", "Ctrl+V"},
      ActionSpec{ActionId::Redo, "", "", "Redo", "Ctrl+Y / Ctrl+Shift+Z"},
      ActionSpec{ActionId::ReplaceInBuffer, "", "", "Replace in Buffer", "Ctrl+H"},
      ActionSpec{ActionId::SelectAll, "", "", "Select All", "Ctrl+A"},
      ActionSpec{ActionId::Undo, "", "", "Undo", "Ctrl+Z"},
  });
  return kSpecs;
}

const WorkspaceShell::ActionSpec* WorkspaceShell::FindActionSpec(ActionId id) {
  const auto specs = ActionSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [id](const ActionSpec& spec) { return spec.id == id; });
  return it == specs.end() ? nullptr : &(*it);
}

const WorkspaceShell::ActionSpec* WorkspaceShell::FindActionByCommand(std::string_view command_name) {
  const auto specs = ActionSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(), [command_name](const ActionSpec& spec) {
    return !spec.command_name.empty() && spec.command_name == command_name;
  });
  return it == specs.end() ? nullptr : &(*it);
}

const std::vector<std::string>& WorkspaceShell::CommandNames() {
  static const std::vector<std::string> kNames = [] {
    std::vector<std::string> names;
    for (const ActionSpec& spec : ActionSpecs()) {
      if (!spec.command_name.empty()) {
        names.emplace_back(spec.command_name);
      }
    }
    return names;
  }();
  return kNames;
}

std::vector<std::string> WorkspaceShell::DocumentedCommandUsages() {
  std::vector<std::string> usages;
  for (const ActionSpec& spec : ActionSpecs()) {
    if (spec.command_name.empty()) {
      continue;
    }
    usages.push_back(spec.command_usage.empty() ? std::string(spec.command_name)
                                                : std::string(spec.command_usage));
  }
  return usages;
}

bool WorkspaceShell::IsActionEnabled(ActionId id) const {
  switch (id) {
    case ActionId::Colorscheme:
    case ActionId::Files:
    case ActionId::OpenCommandPrompt:
    case ActionId::ProjectOpen:
    case ActionId::Quit:
    case ActionId::SidebarClose:
    case ActionId::SidebarHide:
    case ActionId::SidebarShow:
    case ActionId::SidebarToggle:
      return true;
    case ActionId::CloseActiveTab:
      return !open_tabs_.empty();
    case ActionId::CompareHead:
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab:
      return !project_root_.empty() &&
             (surface_.tree_context_menu.open ? surface_.tree_context_menu.target : SelectedTreeTargetKind()) ==
                 TreeContextTargetKind::File;
    case ActionId::CreateDirectory:
    case ActionId::CreateFile: {
      if (project_root_.empty()) {
        return false;
      }
      const TreeContextTargetKind target =
          surface_.tree_context_menu.open ? surface_.tree_context_menu.target : SelectedTreeTargetKind();
      return target == TreeContextTargetKind::Directory || target == TreeContextTargetKind::Root ||
             target == TreeContextTargetKind::Background;
    }
    case ActionId::DeletePath:
    case ActionId::RenamePath: {
      if (project_root_.empty()) {
        return false;
      }
      const TreeContextTargetKind target =
          surface_.tree_context_menu.open ? surface_.tree_context_menu.target : SelectedTreeTargetKind();
      return target == TreeContextTargetKind::File || target == TreeContextTargetKind::Directory;
    }
    case ActionId::Compare:
    case ActionId::Find:
    case ActionId::GitRefresh:
    case ActionId::Merge:
    case ActionId::Open:
    case ActionId::ProjectClose:
    case ActionId::ProjectSearch:
    case ActionId::Tab:
    case ActionId::Term:
    case ActionId::Tree:
    case ActionId::TreeRefresh:
      return !project_root_.empty();
    case ActionId::CopyLastTerminalCommand:
      return ActiveTerminalTab() != nullptr && LastTerminalCommandText().has_value();
    case ActionId::CopySelectionWithContext:
      return ActiveEditableViewport() != nullptr && ActiveEditableViewport()->has_selection();
    case ActionId::CopySelection:
    case ActionId::CutSelection:
    case ActionId::PasteClipboard:
    case ActionId::Redo:
    case ActionId::SelectAll:
    case ActionId::Undo:
      return ActiveEditableViewport() != nullptr;
    case ActionId::Goto:
    case ActionId::Jump:
    case ActionId::ReplaceInBuffer:
    case ActionId::Reopen:
    case ActionId::Search:
    case ActionId::SplitFirst:
    case ActionId::SplitLast:
    case ActionId::SplitNext:
    case ActionId::SplitPrev:
    case ActionId::Unsplit:
    case ActionId::Vsplit:
      return ActiveTabIsEditor();
    case ActionId::Save:
      return ActiveTabIsEditor() || ActiveTabIsMerge() ||
             (ActiveTabIsCompare() && ActiveCompareTab() != nullptr && ActiveCompareTab()->right_editable);
    case ActionId::Focus:
      return true;
    case ActionId::IndentWidth:
      return true;
    case ActionId::CopyAbsolutePath:
      return !ResolveTreeActionPath(ActionSource::ContextMenu).empty();
    case ActionId::CopyRelativePath: {
      const std::filesystem::path path = ResolveTreeActionPath(ActionSource::ContextMenu);
      return !project_root_.empty() && !path.empty() && path != project_root_;
    }
    case ActionId::SidebarWidth:
    case ActionId::SoftTabs:
    case ActionId::TabSize:
    case ActionId::UiScale:
      return true;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev:
      return !project_root_.empty() && project_catalog_.entries.size() > 1;
    case ActionId::TabMove:
    case ActionId::TabSwitch:
      return !project_root_.empty() && !open_tabs_.empty();
  }

  return true;
}

std::span<const WorkspaceShell::MenuSpec> WorkspaceShell::MenuSpecs() {
  const auto item = [](ActionId action, std::string_view label = {},
                       std::string_view accelerator = {},
                       std::array<std::string_view, 2> args = {}, std::size_t arg_count = 0,
                       bool checkable = false, MenuId submenu = MenuId::None) {
    return MenuItemSpec{action, label, accelerator, args, arg_count, false, checkable, submenu};
  };
  const auto separator = [] {
    return MenuItemSpec{ActionId::Colorscheme, {}, {}, {}, 0, true, false, MenuId::None};
  };

  static const auto kFileItems = std::to_array<MenuItemSpec>({
      item(ActionId::ProjectOpen, "New Project Tab..."),
      separator(),
      item(ActionId::Tab),
      item(ActionId::Save),
      item(ActionId::CloseActiveTab),
      item(ActionId::Reopen),
      separator(),
      item(ActionId::ProjectClose),
      separator(),
      item(ActionId::Quit),
  });
  static const auto kEditItems = std::to_array<MenuItemSpec>({
      item(ActionId::Undo),
      item(ActionId::Redo),
      separator(),
      item(ActionId::CutSelection),
      item(ActionId::CopySelection),
      item(ActionId::CopySelectionWithContext),
      item(ActionId::PasteClipboard),
      item(ActionId::SelectAll),
  });
  static const auto kViewItems = std::to_array<MenuItemSpec>({
      item(ActionId::SidebarToggle, {}, {}, {}, 0, true),
      separator(),
      item(ActionId::UiScale, "Zoom In", "Ctrl+=", std::array<std::string_view, 2>{"up", {}}, 1),
      item(ActionId::UiScale, "Zoom Out", "Ctrl+-",
           std::array<std::string_view, 2>{"down", {}}, 1),
      item(ActionId::UiScale, "Reset Zoom", "Ctrl+0",
           std::array<std::string_view, 2>{"reset", {}}, 1),
      separator(),
      item(ActionId::Focus, "Focus Editor", {},
           std::array<std::string_view, 2>{"editor", {}}, 1),
      item(ActionId::Focus, "Focus Sidebar", {},
           std::array<std::string_view, 2>{"sidebar", {}}, 1),
      item(ActionId::Focus, "Focus Panel", {},
           std::array<std::string_view, 2>{"panel", {}}, 1),
  });
  static const auto kSidebarModeItems = std::to_array<MenuItemSpec>({
      item(ActionId::SidebarShow, "Project", {}, std::array<std::string_view, 2>{"tree", {}}, 1,
           true),
      item(ActionId::SidebarShow, "Search", {}, std::array<std::string_view, 2>{"search", {}}, 1,
           true),
      item(ActionId::SidebarShow, "Source Control", {},
           std::array<std::string_view, 2>{"git", {}}, 1, true),
  });
  static const auto kSearchItems = std::to_array<MenuItemSpec>({
      item(ActionId::Search),
      item(ActionId::ReplaceInBuffer),
      item(ActionId::Files),
      item(ActionId::ProjectSearch),
  });
  static const auto kProjectItems = std::to_array<MenuItemSpec>({
      item(ActionId::Compare, "Compare Current File..."),
      item(ActionId::TreeRefresh),
      item(ActionId::GitRefresh),
      separator(),
      item(ActionId::ProjectNext),
      item(ActionId::ProjectPrev),
  });
  static const auto kTerminalItems = std::to_array<MenuItemSpec>({
      item(ActionId::Term),
  });
  static const auto kTerminalTabContextItems = std::to_array<MenuItemSpec>({
      item(ActionId::CopyLastTerminalCommand),
  });
  static const auto kMenus = std::to_array<MenuSpec>({
      MenuSpec{MenuId::File, "File", kFileItems},
      MenuSpec{MenuId::Edit, "Edit", kEditItems},
      MenuSpec{MenuId::View, "View", kViewItems},
      MenuSpec{MenuId::SidebarMode, "Sidebar Mode", kSidebarModeItems},
      MenuSpec{MenuId::Search, "Search", kSearchItems},
      MenuSpec{MenuId::Project, "Project", kProjectItems},
      MenuSpec{MenuId::Terminal, "Terminal", kTerminalItems},
      MenuSpec{MenuId::TerminalTabContext, "Terminal", kTerminalTabContextItems},
  });
  return kMenus;
}

const WorkspaceShell::MenuSpec* WorkspaceShell::FindMenuSpec(MenuId id) {
  const auto menus = MenuSpecs();
  const auto it = std::find_if(menus.begin(), menus.end(),
                               [id](const MenuSpec& spec) { return spec.id == id; });
  return it == menus.end() ? nullptr : &(*it);
}

const project::TreeEntry* WorkspaceShell::SelectedTreeEntry() const {
  if (surface_.sidebar_mode != SidebarMode::Tree) {
    return nullptr;
  }
  const auto& entries = directory_tree_.entries();
  if (directory_tree_.selected_index() >= entries.size()) {
    return nullptr;
  }
  return &entries[directory_tree_.selected_index()];
}

WorkspaceShell::TreeContextTargetKind WorkspaceShell::SelectedTreeTargetKind() const {
  const project::TreeEntry* entry = SelectedTreeEntry();
  if (entry == nullptr) {
    return TreeContextTargetKind::None;
  }
  if (!entry->is_directory) {
    return TreeContextTargetKind::File;
  }
  return entry->path == project_root_ ? TreeContextTargetKind::Root
                                      : TreeContextTargetKind::Directory;
}

void WorkspaceShell::SetPresentationScale(float scale_x, float scale_y) {
  presentation_scale_x_ =
      std::isfinite(scale_x) && scale_x > 0.0f ? scale_x : 1.0f;
  presentation_scale_y_ =
      std::isfinite(scale_y) && scale_y > 0.0f ? scale_y : 1.0f;
}

void WorkspaceShell::SetWindowChromeState(int width,
                                          int height,
                                          bool maximized,
                                          bool custom_enabled) {
  if (width > 0) {
    last_window_width_ = width;
  }
  if (height > 0) {
    last_window_height_ = height;
  }
  window_maximized_ = maximized;
  custom_window_chrome_enabled_ = custom_enabled;
}

SDL_HitTestResult WorkspaceShell::WindowHitTest(float x, float y) const {
  if (!custom_window_chrome_enabled_ || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return SDL_HITTEST_NORMAL;
  }

  const float window_width = static_cast<float>(last_window_width_);
  const float window_height = static_cast<float>(last_window_height_);
  if (x < 0.0f || y < 0.0f || x >= window_width || y >= window_height) {
    return SDL_HITTEST_NORMAL;
  }

  if (!window_maximized_) {
    const bool left = x < kWindowFrameHitThickness;
    const bool right = x >= window_width - kWindowFrameHitThickness;
    const bool top = y < kWindowFrameHitThickness;
    const bool bottom = y >= window_height - kWindowFrameHitThickness;
    if (left || right || top || bottom) {
      return ResizeHitTestResult(left, right, top, bottom);
    }
  }

  if (surface_.menu_bar_open || surface_.tree_context_menu.open) {
    return SDL_HITTEST_NORMAL;
  }

  const WorkspaceLayout layout =
      ComputeLayout(window_width, window_height, surface_.sidebar_visible, BottomPanelVisible(),
                    surface_.sidebar_width, surface_.bottom_panel_height);
  if (!Contains(layout.menu_bar, x, y)) {
    return SDL_HITTEST_NORMAL;
  }

  for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
    if (Contains(item.rect, x, y)) {
      return SDL_HITTEST_NORMAL;
    }
  }
  for (const VisibleWindowControlButton& button :
       ComputeVisibleWindowControlButtons(layout.menu_bar)) {
    if (Contains(button.rect, x, y)) {
      return SDL_HITTEST_NORMAL;
    }
  }

  return SDL_HITTEST_DRAGGABLE;
}

WorkspaceShell::WindowAction WorkspaceShell::ConsumeWindowAction() {
  const WindowAction action = pending_window_action_;
  pending_window_action_ = WindowAction::None;
  return action;
}

std::optional<Uint32> WorkspaceShell::NextAnimationDelayMs() const {
  if (!ShouldBlinkCaret()) {
    return std::nullopt;
  }

  const Uint64 elapsed = SDL_GetTicks() - caret_blink_epoch_ms_;
  const Uint64 remaining = kCaretBlinkIntervalMs - (elapsed % kCaretBlinkIntervalMs);
  return static_cast<Uint32>(std::max<Uint64>(1, remaining));
}

void WorkspaceShell::ResetCaretBlink() {
  caret_blink_epoch_ms_ = SDL_GetTicks();
}

bool WorkspaceShell::ShouldBlinkCaret() const {
  if (surface_.command_mode || prompts_.dirty_visible || prompts_.surface_visible ||
      surface_.overlay_visible || surface_.menu_bar_open || surface_.tree_context_menu.open) {
    return false;
  }

  if (surface_.focus == FocusTarget::Editor) {
    const editor::TextViewport* viewport = ActiveEditableViewport();
    return viewport != nullptr && !viewport->is_placeholder();
  }

  if (surface_.focus == FocusTarget::Panel) {
    return ActiveTerminalTab() != nullptr;
  }

  return false;
}

bool WorkspaceShell::CaretVisibleNow() const {
  if (!ShouldBlinkCaret()) {
    return false;
  }

  const Uint64 elapsed = SDL_GetTicks() - caret_blink_epoch_ms_;
  return ((elapsed / kCaretBlinkIntervalMs) % 2) == 0;
}

bool WorkspaceShell::ActiveTabIsCompare() const {
  return active_tab_index_ < open_tabs_.size() &&
         open_tabs_[active_tab_index_].kind == TabEntry::Kind::Compare &&
         open_tabs_[active_tab_index_].compare.has_value();
}

WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() {
  if (!ActiveTabIsCompare()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].compare.value();
}

const WorkspaceShell::CompareTabState* WorkspaceShell::ActiveCompareTab() const {
  if (!ActiveTabIsCompare()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].compare.value();
}

ScrollSurfaceLayout WorkspaceShell::ComputeEditorScrollLayout(
    const SDL_FRect& rect,
    const editor::TextViewport& viewport,
    const editor::EditorViewMetrics& metrics) const {
  const std::size_t total_columns =
      std::max<std::size_t>(metrics.visible_columns, MaxVisualColumns(viewport));
  return ComputeScrollSurfaceLayout(rect, viewport.line_count(),
                                    static_cast<int>(metrics.visible_rows),
                                    static_cast<int>(viewport.scroll_line()), total_columns,
                                    metrics.visible_columns, viewport.horizontal_scroll());
}

WorkspaceShell::CompareSurfaceLayout WorkspaceShell::ComputeCompareSurfaceLayout(
    const SDL_FRect& rect,
    const CompareTabState& compare_tab) const {
  const auto measure = [&](bool reserve_vertical, bool reserve_horizontal) {
    CompareSurfaceLayout layout;
    layout.line_height = text_renderer_.LineHeight();
    layout.gutter_width = std::max(
        28.0f,
        text_renderer_.MeasureWidth(std::to_string(compare_tab.model.rows.size() + 1)) + 12.0f);
    layout.divider_width = 18.0f;
    layout.left_x = rect.x + 8.0f;
    layout.header_y = rect.y + 6.0f;
    layout.rows_y = rect.y + layout.line_height + 12.0f;

    const float reserved_width =
        reserve_vertical ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
    const float reserved_height =
        reserve_horizontal ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
    const float content_width = std::max(
        40.0f, rect.w - reserved_width - layout.gutter_width * 2.0f - layout.divider_width - 16.0f);
    layout.left_width = std::floor(content_width * 0.5f);
    layout.right_width = content_width - layout.left_width;
    layout.center_x = layout.left_x + layout.gutter_width + layout.left_width;
    layout.right_x = layout.center_x + layout.divider_width + layout.gutter_width;

    const float row_region_height =
        rect.h - reserved_height - (layout.rows_y - rect.y) - 8.0f;
    layout.visible_rows = std::max(
        1, static_cast<int>(row_region_height / std::max(1.0f, layout.line_height)));

    const float pane_text_width = std::max(0.0f, std::min(layout.left_width, layout.right_width) - 8.0f);
    layout.visible_columns = std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::floor(pane_text_width / std::max(1.0f, text_renderer_.CharWidth()))));
    layout.show_vertical = reserve_vertical;
    layout.show_horizontal = reserve_horizontal;
    return layout;
  };

  bool show_vertical = false;
  bool show_horizontal = false;
  for (int iteration = 0; iteration < 4; ++iteration) {
    const CompareSurfaceLayout layout = measure(show_vertical, show_horizontal);
    const bool next_vertical =
        compare_tab.model.rows.size() > static_cast<std::size_t>(layout.visible_rows);
    const bool next_horizontal = compare_tab.max_visual_columns > layout.visible_columns;
    if (next_vertical == show_vertical && next_horizontal == show_horizontal) {
      return layout;
    }
    show_vertical = next_vertical;
    show_horizontal = next_horizontal;
  }

  return measure(show_vertical, show_horizontal);
}

ScrollSurfaceLayout WorkspaceShell::ComputeCompareScrollLayout(
    const SDL_FRect& rect,
    const CompareSurfaceLayout& surface,
    const CompareTabState& compare_tab) const {
  return ComputeScrollSurfaceLayout(rect, compare_tab.model.rows.size(), surface.visible_rows,
                                    compare_tab.scroll_row, compare_tab.max_visual_columns,
                                    surface.visible_columns, compare_tab.horizontal_scroll);
}

TextGridInteractionLayout WorkspaceShell::BuildCompareRightInteractionLayout(
    const CompareSurfaceLayout& surface,
    CompareTabState& compare_tab) const {
  compare_tab.right_viewport.SetViewportSize(static_cast<std::size_t>(surface.visible_rows),
                                             surface.visible_columns);
  compare_tab.right_viewport.SetHorizontalScroll(compare_tab.horizontal_scroll);
  return ComputeTextGridInteractionLayout(
      MakeRect(surface.right_x, surface.rows_y, surface.gutter_width + surface.right_width,
               static_cast<float>(surface.visible_rows) * surface.line_height),
      surface.right_x + surface.gutter_width, surface.rows_y, surface.line_height,
      text_renderer_.CharWidth(), static_cast<std::size_t>(std::max(0, compare_tab.scroll_row)),
      compare_tab.model.rows.size(), compare_tab.horizontal_scroll,
      static_cast<std::size_t>(surface.visible_rows), surface.visible_columns);
}

int WorkspaceShell::CompareMaxScrollRow(const CompareTabState& compare_tab, int visible_rows) const {
  return std::max(0, static_cast<int>(compare_tab.model.rows.size()) - std::max(1, visible_rows));
}

void WorkspaceShell::ClampCompareScrollRow(CompareTabState& compare_tab, int visible_rows) const {
  compare_tab.scroll_row =
      std::clamp(compare_tab.scroll_row, 0, CompareMaxScrollRow(compare_tab, visible_rows));
}

std::size_t WorkspaceShell::CompareMaxScrollColumn(const CompareTabState& compare_tab,
                                                   std::size_t visible_columns) const {
  if (compare_tab.max_visual_columns <= visible_columns) {
    return 0;
  }
  return compare_tab.max_visual_columns - visible_columns;
}

void WorkspaceShell::ClampCompareHorizontalScroll(CompareTabState& compare_tab,
                                                  std::size_t visible_columns) const {
  compare_tab.horizontal_scroll =
      std::min(compare_tab.horizontal_scroll, CompareMaxScrollColumn(compare_tab, visible_columns));
}

void WorkspaceShell::RevealActiveCompareSelection() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
  ClampCompareScrollRow(*compare_tab, surface_layout.visible_rows);
  ClampCompareHorizontalScroll(*compare_tab, surface_layout.visible_columns);
  if (compare_tab->selected_row < static_cast<std::size_t>(compare_tab->scroll_row)) {
    compare_tab->scroll_row = static_cast<int>(compare_tab->selected_row);
  } else if (compare_tab->selected_row >=
             static_cast<std::size_t>(compare_tab->scroll_row + surface_layout.visible_rows)) {
    compare_tab->scroll_row =
        static_cast<int>(compare_tab->selected_row) - surface_layout.visible_rows + 1;
  }
  ClampCompareScrollRow(*compare_tab, surface_layout.visible_rows);
  if (compare_tab->right_editable) {
    SyncCompareViewportScroll(*compare_tab);
  }
}

bool WorkspaceShell::ActiveTabIsMerge() const {
  return active_tab_index_ < open_tabs_.size() &&
         open_tabs_[active_tab_index_].kind == TabEntry::Kind::Merge &&
         open_tabs_[active_tab_index_].merge.has_value();
}

WorkspaceShell::MergeTabState* WorkspaceShell::ActiveMergeTab() {
  if (!ActiveTabIsMerge()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].merge.value();
}

const WorkspaceShell::MergeTabState* WorkspaceShell::ActiveMergeTab() const {
  if (!ActiveTabIsMerge()) {
    return nullptr;
  }
  return &open_tabs_[active_tab_index_].merge.value();
}

WorkspaceShell::MergeToolbarLayout WorkspaceShell::ComputeMergeToolbarLayout(
    const SDL_FRect& rect,
    const MergeSurfaceLayout& surface) const {
  constexpr float kMergeToolbarButtonHeight = 22.0f;
  constexpr float kMergeToolbarButtonGap = 8.0f;
  const auto make_button_rect = [&](float x, std::string_view label) {
    const float width = ComputeChromeButtonWidth(text_renderer_.MeasureWidth(label));
    return MakeRect(x, surface.button_y, width, kMergeToolbarButtonHeight);
  };

  const SDL_FRect save_rect = make_button_rect(0.0f, "Save");
  const float save_x = rect.x + rect.w - 8.0f - save_rect.w;
  const SDL_FRect aligned_save_rect = make_button_rect(save_x, "Save");
  const SDL_FRect open_rect = make_button_rect(
      aligned_save_rect.x - kMergeToolbarButtonGap - make_button_rect(0.0f, "Open Result").w,
      "Open Result");
  const SDL_FRect next_rect = make_button_rect(
      open_rect.x - kMergeToolbarButtonGap - make_button_rect(0.0f, "Next").w, "Next");
  const SDL_FRect prev_rect = make_button_rect(
      next_rect.x - kMergeToolbarButtonGap - make_button_rect(0.0f, "Prev").w, "Prev");
  return {
      .prev_rect = prev_rect,
      .next_rect = next_rect,
      .open_rect = open_rect,
      .save_rect = aligned_save_rect,
  };
}

editor::TextViewport* WorkspaceShell::ActiveEditableViewport() {
  if (ActiveTabIsCompare()) {
    auto* compare_tab = ActiveCompareTab();
    return compare_tab == nullptr || !compare_tab->right_editable || !compare_tab->right_view_active
               ? nullptr
               : &compare_tab->right_viewport;
  }
  if (ActiveTabIsMerge()) {
    auto* merge_tab = ActiveMergeTab();
    return merge_tab == nullptr ? nullptr : &merge_tab->result_viewport;
  }
  if (!ActiveTabIsEditor() || ActiveTabIsCompare()) {
    return nullptr;
  }
  return &text_viewport_;
}

const editor::TextViewport* WorkspaceShell::ActiveEditableViewport() const {
  if (ActiveTabIsCompare()) {
    const auto* compare_tab = ActiveCompareTab();
    return compare_tab == nullptr || !compare_tab->right_editable || !compare_tab->right_view_active
               ? nullptr
               : &compare_tab->right_viewport;
  }
  if (ActiveTabIsMerge()) {
    const auto* merge_tab = ActiveMergeTab();
    return merge_tab == nullptr ? nullptr : &merge_tab->result_viewport;
  }
  if (!ActiveTabIsEditor() || ActiveTabIsCompare()) {
    return nullptr;
  }
  return &text_viewport_;
}

WorkspaceShell::MergeSurfaceLayout WorkspaceShell::ComputeMergeSurfaceLayout(
    const SDL_FRect& rect,
    const MergeTabState& merge_tab) const {
  const auto measure = [&](bool reserve_vertical, bool reserve_horizontal) {
    MergeSurfaceLayout layout;
    layout.line_height = text_renderer_.LineHeight();
    const std::size_t max_line_count =
        std::max({merge_tab.model.incoming_lines.size(), merge_tab.result_viewport.lines().size(),
                  merge_tab.model.current_lines.size(), std::size_t{1}});
    layout.gutter_width =
        std::max(28.0f, text_renderer_.MeasureWidth(std::to_string(max_line_count + 1)) + 12.0f);
    layout.divider_width = 16.0f;
    layout.left_x = rect.x + 8.0f;
    layout.button_y = rect.y + 6.0f;
    layout.secondary_button_y = layout.button_y + kMergeToolbarButtonHeight + 6.0f;
    layout.header_y = rect.y + kMergeToolbarHeight + 4.0f;
    layout.rows_y = rect.y + kMergeToolbarHeight + layout.line_height + 12.0f;

    const float reserved_width =
        reserve_vertical ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
    const float reserved_height =
        reserve_horizontal ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
    const float content_width = std::max(
        kMinMergePaneWidth * 3.0f,
        rect.w - reserved_width - layout.gutter_width * 3.0f - layout.divider_width * 2.0f -
            16.0f);
    const float min_fraction =
        std::min(1.0f / 3.0f, kMinMergePaneWidth / std::max(content_width, 1.0f));
    const float left_fraction =
        std::clamp(merge_tab.left_divider_fraction, min_fraction, 1.0f - min_fraction * 2.0f);
    const float right_fraction = std::clamp(merge_tab.right_divider_fraction,
                                            left_fraction + min_fraction, 1.0f - min_fraction);
    layout.left_width = std::floor(content_width * left_fraction);
    layout.center_width = std::floor(content_width * (right_fraction - left_fraction));
    layout.right_width = std::max(0.0f, content_width - layout.left_width - layout.center_width);
    layout.center_x = layout.left_x + layout.gutter_width + layout.left_width + layout.divider_width;
    layout.right_x =
        layout.center_x + layout.gutter_width + layout.center_width + layout.divider_width;

    const float row_region_height =
        rect.h - reserved_height - (layout.rows_y - rect.y) - 8.0f;
    layout.visible_rows = std::max(
        1, static_cast<int>(row_region_height / std::max(1.0f, layout.line_height)));

    const float pane_text_width = std::max(
        0.0f, std::min({layout.left_width, layout.center_width, layout.right_width}) - 8.0f);
    layout.visible_columns = std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::floor(pane_text_width / std::max(1.0f, text_renderer_.CharWidth()))));
    layout.show_vertical = reserve_vertical;
    layout.show_horizontal = reserve_horizontal;
    return layout;
  };

  bool show_vertical = false;
  bool show_horizontal = false;
  for (int iteration = 0; iteration < 4; ++iteration) {
    const MergeSurfaceLayout layout = measure(show_vertical, show_horizontal);
    const std::size_t line_count =
        std::max({merge_tab.model.incoming_lines.size(), merge_tab.result_viewport.line_count(),
                  merge_tab.model.current_lines.size(), std::size_t{1}});
    const bool next_vertical = line_count > static_cast<std::size_t>(layout.visible_rows);
    const bool next_horizontal = merge_tab.max_visual_columns > layout.visible_columns;
    if (next_vertical == show_vertical && next_horizontal == show_horizontal) {
      return layout;
    }
    show_vertical = next_vertical;
    show_horizontal = next_horizontal;
  }

  return measure(show_vertical, show_horizontal);
}

ScrollSurfaceLayout WorkspaceShell::ComputeMergeScrollLayout(
    const SDL_FRect& rect,
    const MergeSurfaceLayout& surface,
    const MergeTabState& merge_tab) const {
  const std::size_t line_count =
      std::max({merge_tab.model.incoming_lines.size(), merge_tab.result_viewport.line_count(),
                merge_tab.model.current_lines.size(), std::size_t{1}});
  return ComputeScrollSurfaceLayout(rect, line_count, surface.visible_rows, merge_tab.scroll_row,
                                    merge_tab.max_visual_columns, surface.visible_columns,
                                    merge_tab.horizontal_scroll);
}

TextGridInteractionLayout WorkspaceShell::BuildMergeSourceInteractionLayout(
    const MergeSurfaceLayout& surface,
    const MergeTabState& merge_tab,
    bool incoming) const {
  const float pane_x = incoming ? surface.left_x : surface.right_x;
  const float pane_width = incoming ? surface.left_width : surface.right_width;
  const std::size_t line_count =
      incoming ? merge_tab.model.incoming_lines.size() : merge_tab.model.current_lines.size();
  return ComputeTextGridInteractionLayout(
      MakeRect(pane_x, surface.rows_y, surface.gutter_width + pane_width,
               static_cast<float>(surface.visible_rows) * surface.line_height),
      pane_x + surface.gutter_width, surface.rows_y, surface.line_height,
      text_renderer_.CharWidth(), static_cast<std::size_t>(std::max(0, merge_tab.scroll_row)),
      line_count, merge_tab.horizontal_scroll, static_cast<std::size_t>(surface.visible_rows),
      surface.visible_columns);
}

WorkspaceShell::MergeResultInteractionLayout WorkspaceShell::BuildMergeResultInteractionLayout(
    const SDL_FRect& rect,
    const MergeSurfaceLayout& surface,
    MergeTabState& merge_tab) const {
  const SDL_FRect result_rect = ComputeMergeResultViewportRect(
      rect, surface.center_x, surface.rows_y, surface.gutter_width, surface.center_width,
      surface.show_horizontal);
  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, merge_tab.result_viewport, result_rect);
  merge_tab.result_viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const VisibleLineRangeLayout lines = {
      .first_line_y = metrics.first_line_y,
      .line_height = metrics.line_height,
      .scroll_line = merge_tab.result_viewport.scroll_line(),
      .visible_rows = metrics.visible_rows,
  };
  return MergeResultInteractionLayout{
      .rect = result_rect,
      .metrics = metrics,
      .lines = lines,
      .text = ComputeTextGridInteractionLayout(
          result_rect, metrics.text_x, metrics.first_line_y, metrics.line_height,
          text_renderer_.CharWidth(), merge_tab.result_viewport.scroll_line(),
          merge_tab.result_viewport.line_count(), merge_tab.result_viewport.horizontal_scroll(),
          metrics.visible_rows, metrics.visible_columns),
  };
}

WorkspaceShell::MergeInteractionLayout WorkspaceShell::BuildMergeInteractionLayout(
    const SDL_FRect& rect,
    const MergeSurfaceLayout& surface,
    MergeTabState& merge_tab) const {
  const float bottom_reserved =
      surface.show_horizontal ? (kScrollbarThickness + kScrollbarInset) : 0.0f;
  return MergeInteractionLayout{
      .content_bottom = rect.y + std::max(0.0f, rect.h - bottom_reserved),
      .result = BuildMergeResultInteractionLayout(rect, surface, merge_tab),
      .incoming_accept_button_width =
          ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Accept Theirs")),
      .current_accept_button_width =
          ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Accept Ours")),
      .result_action_widths = {
          ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Base")),
          ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Theirs")),
          ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Ours")),
          ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Both")),
      },
  };
}

std::optional<std::size_t> WorkspaceShell::FindMergeTrackedConflictAtSourceLine(
    const MergeTabState& merge_tab,
    std::size_t line,
    bool incoming) const {
  return microide::workspace::FindMergeTrackedConflictAtSourceLine(merge_tab.conflicts, line, incoming);
}

std::optional<std::size_t> WorkspaceShell::FindMergeTrackedConflictAtResultLine(
    const MergeTabState& merge_tab,
    std::size_t line) const {
  return microide::workspace::FindMergeTrackedConflictAtResultLine(merge_tab.conflicts, line);
}

SDL_FRect WorkspaceShell::BuildMergeSourceActionButtonRect(
    const MergeSurfaceLayout& surface,
    const MergeInteractionLayout& interaction,
    const MergeTrackedConflict& conflict,
    bool incoming) const {
  return ComputeMergeSourceActionButtonRect(
      incoming ? surface.left_x : surface.right_x, surface.gutter_width, surface.rows_y,
      surface.line_height, static_cast<int>(interaction.result.text.scroll_line),
      incoming ? conflict.incoming_end_line : conflict.current_end_line, interaction.content_bottom,
      incoming ? interaction.incoming_accept_button_width : interaction.current_accept_button_width,
      kMergeToolbarButtonHeight);
}

std::array<SDL_FRect, 4> WorkspaceShell::BuildMergeResultActionButtonRects(
    const MergeSurfaceLayout& surface,
    const MergeInteractionLayout& interaction,
    const MergeTrackedConflict& conflict) const {
  return ComputeMergeResultActionButtonRects(
      surface.center_x + surface.gutter_width, surface.rows_y, interaction.content_bottom,
      ComputeVisibleLineRangeRect(interaction.result.rect, interaction.result.lines, conflict.start_line,
                                  std::max(conflict.end_line, conflict.start_line + std::size_t{1})),
      interaction.result_action_widths, kMergeToolbarButtonHeight, kMergeToolbarButtonGap);
}

std::optional<WorkspaceShell::MergeHoverState> WorkspaceShell::ClassifyMergeHoverState(
    const MergeSurfaceLayout& surface,
    const MergeInteractionLayout& interaction,
    const MergeTabState& merge_tab,
    float x,
    float y) const {
  return microide::workspace::ClassifyMergeHoverState(
      MergeHoverSurfaceLayout{
          .gutter_width = surface.gutter_width,
          .left_x = surface.left_x,
          .center_x = surface.center_x,
          .right_x = surface.right_x,
          .rows_y = surface.rows_y,
          .line_height = surface.line_height,
      },
      MergeHoverInteractionLayout{
          .content_bottom = interaction.content_bottom,
          .incoming = BuildMergeSourceInteractionLayout(surface, merge_tab, true),
          .current = BuildMergeSourceInteractionLayout(surface, merge_tab, false),
          .result =
              MergeHoverResultLayout{
                  .rect = interaction.result.rect,
                  .lines = interaction.result.lines,
                  .text = interaction.result.text,
              },
          .incoming_accept_button_width = interaction.incoming_accept_button_width,
          .current_accept_button_width = interaction.current_accept_button_width,
          .result_action_widths = interaction.result_action_widths,
          .button_height = kMergeToolbarButtonHeight,
          .button_gap = kMergeToolbarButtonGap,
      },
      merge_tab.conflicts, x, y);
}

int WorkspaceShell::MergeMaxScrollRow(const MergeTabState& merge_tab, int visible_rows) const {
  const std::size_t line_count =
      std::max({merge_tab.model.incoming_lines.size(), merge_tab.result_viewport.line_count(),
                merge_tab.model.current_lines.size(), std::size_t{1}});
  return std::max(0, static_cast<int>(line_count) - std::max(1, visible_rows));
}

void WorkspaceShell::ClampMergeScrollRow(MergeTabState& merge_tab, int visible_rows) const {
  merge_tab.scroll_row =
      std::clamp(merge_tab.scroll_row, 0, MergeMaxScrollRow(merge_tab, visible_rows));
}

std::size_t WorkspaceShell::MergeMaxScrollColumn(const MergeTabState& merge_tab,
                                                 std::size_t visible_columns) const {
  if (merge_tab.max_visual_columns <= visible_columns) {
    return 0;
  }
  return merge_tab.max_visual_columns - visible_columns;
}

void WorkspaceShell::ClampMergeHorizontalScroll(MergeTabState& merge_tab,
                                                std::size_t visible_columns) const {
  merge_tab.horizontal_scroll =
      std::min(merge_tab.horizontal_scroll, MergeMaxScrollColumn(merge_tab, visible_columns));
}

void WorkspaceShell::RevealActiveMergeSelection() {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || last_window_width_ <= 0 || last_window_height_ <= 0 ||
      merge_tab->conflicts.empty()) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  const MergeSurfaceLayout surface_layout =
      ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
  ClampMergeScrollRow(*merge_tab, surface_layout.visible_rows);
  ClampMergeHorizontalScroll(*merge_tab, surface_layout.visible_columns);
  const auto& selected_conflict =
      merge_tab->conflicts[std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1)];
  const int start_row = static_cast<int>(std::min(
      {selected_conflict.incoming_start_line, selected_conflict.start_line,
       selected_conflict.current_start_line}));
  const int end_row = static_cast<int>(std::max(
      {selected_conflict.incoming_end_line, selected_conflict.end_line,
       selected_conflict.current_end_line}));
  if (start_row < merge_tab->scroll_row) {
    merge_tab->scroll_row = start_row;
  } else if (end_row > merge_tab->scroll_row + surface_layout.visible_rows) {
    merge_tab->scroll_row = end_row - surface_layout.visible_rows;
  }
  ClampMergeScrollRow(*merge_tab, surface_layout.visible_rows);
  merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
}

void WorkspaceShell::MoveFileFinderSelection(int delta) {
  file_finder_.MoveSelection(delta);
  if (surface_.overlay_visible && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_),
                      static_cast<float>(last_window_height_), surface_.sidebar_visible,
                      BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

WorkspaceShell::TextInputSurface WorkspaceShell::CurrentTextInputSurface() const {
  if (prompts_.dirty_visible) {
    return TextInputSurface::None;
  }

  if (prompts_.surface_visible) {
    return prompts_.surface.kind == PromptSurfaceState::Kind::TextInput
               ? TextInputSurface::PromptInput
               : TextInputSurface::None;
  }

  if (surface_.menu_bar_open || surface_.tree_context_menu.open) {
    return TextInputSurface::None;
  }

  if (surface_.command_mode) {
    return TextInputSurface::Command;
  }

  if (surface_.overlay_visible) {
    switch (surface_.overlay_mode) {
      case OverlayMode::BufferSearch:
        return TextInputSurface::BufferSearch;
      case OverlayMode::BufferReplace:
        return surface_.buffer_search_field == BufferSearchField::Search
                   ? TextInputSurface::BufferReplaceSearch
                   : TextInputSurface::BufferReplaceReplace;
      case OverlayMode::ProjectSearch:
        return TextInputSurface::ProjectSearchOverlay;
      case OverlayMode::CommitPicker:
        return TextInputSurface::CommitPicker;
      case OverlayMode::FileFinder:
      default:
        return TextInputSurface::FileFinder;
    }
  }

  if (surface_.focus == FocusTarget::Sidebar && surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Search &&
      overlay_workflow_.project_search.editing) {
    return overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Query
               ? TextInputSurface::SidebarSearchQuery
               : TextInputSurface::SidebarSearchReplace;
  }

  if (surface_.focus == FocusTarget::Editor && ActiveEditableViewport() != nullptr) {
    return TextInputSurface::Editor;
  }

  if (surface_.focus == FocusTarget::Panel && ActiveTerminalTab() != nullptr) {
    return TextInputSurface::Terminal;
  }

  return TextInputSurface::None;
}

bool WorkspaceShell::WriteClipboardText(std::string_view text) const {
  if (text.empty()) {
    return false;
  }
  if (clipboard_text_writer_) {
    return clipboard_text_writer_(text);
  }
  return SDL_SetClipboardText(std::string(text).c_str());
}

std::optional<std::string> WorkspaceShell::ReadClipboardText() const {
  if (clipboard_text_reader_) {
    return clipboard_text_reader_();
  }

  char* clipboard_text = SDL_GetClipboardText();
  if (clipboard_text == nullptr) {
    return std::nullopt;
  }

  std::string copied_text(clipboard_text);
  SDL_free(clipboard_text);
  return copied_text;
}

std::optional<std::string> WorkspaceShell::SelectionTextWithContext() const {
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return std::nullopt;
  }

  const std::optional<editor::SelectionRange> range = viewport->selection_range();
  if (!range.has_value()) {
    return std::nullopt;
  }

  const std::string text = viewport->SelectedText();
  if (text.empty()) {
    return std::nullopt;
  }

  const std::filesystem::path path = viewport->path().lexically_normal();
  std::string path_label;
  if (!project_root_.empty() && !path.empty()) {
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(path, project_root_, error);
    if (!error && !relative.empty() && relative.native().rfind("..", 0) != 0) {
      path_label = relative.generic_string();
    }
  }
  if (path_label.empty()) {
    path_label = path.empty() ? "untitled" : path.string();
  }

  const std::size_t start_line = range->start.line + 1;
  std::size_t end_line = range->end.line + 1;
  if (range->end.column == 0 && range->end.line > range->start.line) {
    end_line = range->end.line;
  }

  std::string header = path_label + ":" + std::to_string(start_line);
  if (end_line > start_line) {
    header += "-" + std::to_string(end_line);
  }
  header += "\n";
  header += text;
  return header;
}

std::string WorkspaceShell::BreadcrumbLabel() const {
  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return "compare";
    }
    return BuildCompareBreadcrumbLabel(project_root_, compare_tab->path, compare_tab->left_label,
                                       compare_tab->right_label);
  }
  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return "merge";
    }
    return BuildMergeBreadcrumbLabel(project_root_, merge_tab->output_path,
                                     merge_tab->incoming_label, merge_tab->current_label);
  }
  return BuildEditorBreadcrumbLabel(project_root_, text_viewport_.path(),
                                    text_viewport_.is_placeholder(),
                                    text_viewport_.large_file_mode());
}

std::string WorkspaceShell::ProjectLabel() const {
  return project_root_.empty() ? "microide" : ProjectLabelForRoot(project_root_);
}

std::string WorkspaceShell::ProjectLabelForRoot(const std::filesystem::path& root) const {
  if (root.empty()) {
    return "welcome";
  }
  const std::string filename = root.filename().string();
  return filename.empty() ? root.lexically_normal().string() : filename;
}

std::string WorkspaceShell::ProjectTabDisplayTitle(std::size_t index) const {
  if (index >= project_catalog_.entries.size()) {
    return {};
  }
  const std::filesystem::path root = ProjectCatalogRoot(index);
  const std::string label = ProjectLabelForRoot(root);
  return DirtyEditorTabIndicesForProject(index).empty() ? label : "*" + label;
}

std::string WorkspaceShell::HoveredTabTooltipLabel(const SDL_FRect& tab_strip) const {
  if (!last_mouse_position_valid_ || project_root_.empty()) {
    return {};
  }
  if (!Contains(tab_strip, last_mouse_x_, last_mouse_y_)) {
    return {};
  }

  const auto visible_tabs = ComputeVisibleTabs(tab_strip);
  return HoveredChromeTabTooltipLabel(visible_tabs, last_mouse_x_, last_mouse_y_);
}

WorkspaceShell::CursorKind WorkspaceShell::CursorKindForPosition(float x, float y) const {
  switch (surface_.drag_target) {
    case DragTarget::SidebarDivider:
      return CursorKind::EwResize;
    case DragTarget::BottomPanelDivider:
      return CursorKind::NsResize;
    case DragTarget::EditorSplitDivider: {
      const auto* editor_tab = ActiveEditorTab();
      const auto* split_node = editor_tab != nullptr
                                   ? FindEditorSplitNode(editor_tab->split_root.get(),
                                                         surface_.drag_editor_split_path)
                                   : nullptr;
      return split_node != nullptr &&
                     split_node->orientation == EditorSplitOrientation::Horizontal
                 ? CursorKind::NsResize
                 : CursorKind::EwResize;
    }
    default:
      break;
  }

  if (last_window_width_ <= 0 || last_window_height_ <= 0) {
    return CursorKind::Default;
  }

  if (prompts_.dirty_visible) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const auto buttons = ComputeDirtyPromptButtonRects(ComputeDirtyPromptRect(full));
    for (const SDL_FRect& button : buttons) {
      if (Contains(button, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (prompts_.surface_visible) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const SDL_FRect dialog = ComputePromptSurfaceRect(full);
    for (const SDL_FRect& button : ComputePromptSurfaceButtonRects(dialog)) {
      if (Contains(button, x, y)) {
        return CursorKind::Pointer;
      }
    }
    if (prompts_.surface.kind == PromptSurfaceState::Kind::TextInput &&
        Contains(ComputePromptSurfaceInputRect(dialog), x, y)) {
      return CursorKind::Text;
    }
    return CursorKind::Default;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    surface_.sidebar_visible, BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);

  if (surface_.tree_context_menu.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect();
        popup_rect.has_value() && Contains(*popup_rect, x, y)) {
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(TreeContextMenuItems(surface_.tree_context_menu.target),
                                        surface_.tree_context_menu.active_item_index, *popup_rect)) {
        if (Contains(item.rect, x, y)) {
          return item.separator ? CursorKind::Default : CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
  }

  if (surface_.menu_bar_open) {
    if (const auto popup_rect = ActiveSubmenuRect(layout.menu_bar);
        popup_rect.has_value() && Contains(*popup_rect, x, y)) {
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(surface_.active_submenu_id, *popup_rect)) {
        if (Contains(item.rect, x, y)) {
          return item.separator ? CursorKind::Default : CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
    if (const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, surface_.active_menu_id);
        popup_rect.has_value() && Contains(*popup_rect, x, y)) {
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(surface_.active_menu_id, *popup_rect)) {
        if (Contains(item.rect, x, y)) {
          return item.separator ? CursorKind::Default : CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
  }

  if (Contains(layout.menu_bar, x, y)) {
    for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (Contains(item.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    for (const VisibleWindowControlButton& button :
         ComputeVisibleWindowControlButtons(layout.menu_bar)) {
      if (Contains(button.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (surface_.overlay_visible) {
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
    if (!Contains(overlay, x, y)) {
      return CursorKind::Default;
    }

    const auto overlay_list_layout = ComputeOverlayListLayout(overlay);
    if (overlay_list_layout.scrollbar.has_value() &&
        Contains(overlay_list_layout.scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    if (const auto item_index = ScrollableListIndexAtY(overlay_list_layout, y);
        item_index.has_value() && *item_index >= 0 &&
        *item_index < static_cast<int>(OverlayItemCount())) {
      return CursorKind::Pointer;
    }

    if (surface_.overlay_mode == OverlayMode::BufferReplace) {
      return y >= overlay.y + 40.0f && y < overlay.y + 82.0f ? CursorKind::Text
                                                              : CursorKind::Default;
    }
    if (surface_.overlay_mode == OverlayMode::CommitPicker) {
      return y >= overlay.y + 58.0f && y < overlay.y + 78.0f ? CursorKind::Text
                                                              : CursorKind::Default;
    }
    return y >= overlay.y + 40.0f && y < overlay.y + 60.0f ? CursorKind::Text
                                                            : CursorKind::Default;
  }

  if (surface_.sidebar_visible && Contains(SidebarResizeHandleRect(layout), x, y)) {
    return CursorKind::EwResize;
  }
  if (BottomPanelVisible() && Contains(BottomPanelResizeHandleRect(layout), x, y)) {
    return CursorKind::NsResize;
  }

  if (Contains(layout.project_tab_strip, x, y)) {
    for (const VisibleStripTab& tab : ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (Contains(tab.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (Contains(layout.tab_strip, x, y)) {
    if (project_root_.empty()) {
      return CursorKind::Default;
    }
    if (open_tabs_.empty()) {
      return Contains(MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 2.0f, 220.0f,
                               std::max(22.0f, layout.tab_strip.h - 2.0f)),
                      x, y)
                 ? CursorKind::Pointer
                 : CursorKind::Default;
    }
    for (const VisibleStripTab& tab : ComputeVisibleTabs(layout.tab_strip)) {
      if (Contains(tab.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (surface_.sidebar_visible && Contains(layout.sidebar, x, y)) {
    if (Contains(SidebarModeControlRect(layout.sidebar), x, y)) {
      return CursorKind::Pointer;
    }
    if (surface_.sidebar_mode == SidebarMode::Search) {
      if (Contains(ProjectSearchQueryRect(layout.sidebar), x, y) ||
          Contains(ProjectSearchReplaceRect(layout.sidebar), x, y)) {
        return CursorKind::Text;
      }
      if (Contains(ProjectSearchModeButtonRect(layout.sidebar), x, y) ||
          Contains(ProjectSearchCaseButtonRect(layout.sidebar), x, y) ||
          Contains(ProjectSearchHiddenButtonRect(layout.sidebar), x, y)) {
        return CursorKind::Pointer;
      }

      const auto line_map = BuildProjectSearchLineMap();
      const auto list_layout =
          ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
      if (const auto line_index = ScrollableListIndexAtY(list_layout, y);
          line_index.has_value() && *line_index >= 0 &&
          *line_index < static_cast<int>(line_map.size()) &&
          line_map[static_cast<std::size_t>(*line_index)] >= 0) {
        const SDL_FRect row_rect =
            ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
        return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
      }
      return CursorKind::Default;
    }
    if (surface_.sidebar_mode == SidebarMode::Git) {
      if (Contains(GitSidebarStageAllButtonRect(layout.sidebar), x, y) &&
          CanStageAllGitSidebarEntries()) {
        return CursorKind::Pointer;
      }
      if (Contains(GitSidebarDiscardAllButtonRect(layout.sidebar), x, y) &&
          CanDiscardAllGitSidebarEntries()) {
        return CursorKind::Pointer;
      }
      if (Contains(GitSidebarRefreshButtonRect(layout.sidebar), x, y)) {
        return CursorKind::Pointer;
      }
      const auto lines = BuildGitSidebarLines();
      const auto list_layout = ComputeGitSidebarListLayout(layout.sidebar, lines.size());
      const auto line_index = ScrollableListIndexAtY(list_layout, y);
      if (!line_index.has_value() || *line_index < 0 ||
          *line_index >= static_cast<int>(lines.size())) {
        return CursorKind::Default;
      }
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
      if (!Contains(row_rect, x, y)) {
        return CursorKind::Default;
      }
      const auto& line = lines[static_cast<std::size_t>(*line_index)];
      if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
        return CursorKind::Default;
      }
      const auto& entry = git_sidebar_.entries[static_cast<std::size_t>(line.entry_index)];
      const GitSidebarEntryActionLayout actions =
          ComputeGitSidebarEntryActionLayout(row_rect, entry);
      if ((actions.primary_rect.has_value() && Contains(*actions.primary_rect, x, y)) ||
          (actions.discard_rect.has_value() && Contains(*actions.discard_rect, x, y))) {
        return CursorKind::Pointer;
      }
      return CursorKind::Pointer;
    }

    if (Contains(TreeSidebarRefreshButtonRect(layout.sidebar), x, y)) {
      return CursorKind::Pointer;
    }

    const auto& entries = directory_tree_.entries();
    const auto list_layout = ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
    const auto entry_index = ScrollableListIndexAtY(list_layout, y);
    if (entry_index.has_value() && *entry_index >= 0 &&
        *entry_index < static_cast<int>(entries.size())) {
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *entry_index - list_layout.scroll_row);
      return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
    }
    return CursorKind::Default;
  }

  if (BottomPanelVisible() && Contains(layout.bottom_panel, x, y)) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kBottomPanelHeaderHeight);
    if (ActiveTerminalTab() != nullptr && Contains(panel_header, x, y)) {
      if (Contains(BottomPanelTerminalNewTabRect(panel_header), x, y)) {
        return CursorKind::Pointer;
      }
      for (const VisibleStripTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
        if (Contains(tab.rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
    if (ActiveTerminalTab() != nullptr) {
      const std::size_t line_count =
          ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.LineCount() : 0;
      const auto panel_layout = ComputeBottomPanelLogLayout(layout, line_count);
      if (panel_layout.scroll.vertical_scrollbar.has_value() &&
          Contains(panel_layout.scroll.vertical_scrollbar->track, x, y)) {
        return CursorKind::Default;
      }
    }
    if (surface_.command_mode && Contains(BottomPanelCommandPromptRect(layout), x, y)) {
      return CursorKind::Text;
    }
    if (ActiveTerminalTab() != nullptr && y >= layout.bottom_panel.y + kBottomPanelHeaderHeight) {
      return CursorKind::Text;
    }
    return CursorKind::Default;
  }

  if (const auto popup = ActiveEditorBlamePopupLayout(); popup.has_value()) {
    if (Contains(EditorBlamePopupCopyShaHitRect(*popup), x, y)) {
      return CursorKind::Pointer;
    }
    if (Contains(popup->rect, x, y)) {
      return CursorKind::Default;
    }
  }

  if (!Contains(layout.editor_surface, x, y)) {
    return CursorKind::Default;
  }

  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return CursorKind::Default;
    }
    const CompareSurfaceLayout surface_layout =
        ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
    const auto scroll_layout =
        ComputeCompareScrollLayout(layout.editor_surface, surface_layout, *compare_tab);
    if (scroll_layout.vertical_scrollbar.has_value() &&
        Contains(scroll_layout.vertical_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    if (scroll_layout.horizontal_scrollbar.has_value() &&
        Contains(scroll_layout.horizontal_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    return CursorKind::Pointer;
  }
  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return CursorKind::Default;
    }
    const MergeSurfaceLayout surface_layout =
        ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const auto scroll_layout =
        ComputeMergeScrollLayout(layout.editor_surface, surface_layout, *merge_tab);
    const SDL_FRect left_divider_rect =
        MakeRect(surface_layout.center_x - surface_layout.divider_width, layout.editor_surface.y,
                 surface_layout.divider_width, layout.editor_surface.h);
    const SDL_FRect right_divider_rect =
        MakeRect(surface_layout.right_x - surface_layout.divider_width, layout.editor_surface.y,
                 surface_layout.divider_width, layout.editor_surface.h);
    if (Contains(left_divider_rect, x, y) || Contains(right_divider_rect, x, y)) {
      return CursorKind::EwResize;
    }
    const MergeToolbarLayout toolbar = ComputeMergeToolbarLayout(layout.editor_surface, surface_layout);
    if (Contains(toolbar.prev_rect, x, y) || Contains(toolbar.next_rect, x, y) ||
        Contains(toolbar.save_rect, x, y) || Contains(toolbar.open_rect, x, y)) {
      return CursorKind::Pointer;
    }
    if (scroll_layout.vertical_scrollbar.has_value() &&
        Contains(scroll_layout.vertical_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    if (scroll_layout.horizontal_scrollbar.has_value() &&
        Contains(scroll_layout.horizontal_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    const SDL_FRect result_rect = ComputeMergeResultViewportRect(
        layout.editor_surface, surface_layout.center_x, surface_layout.rows_y,
        surface_layout.gutter_width, surface_layout.center_width, surface_layout.show_horizontal);
    const editor::EditorViewMetrics result_metrics =
        editor::EditorViewRenderer::ComputeMetrics(text_renderer_, merge_tab->result_viewport, result_rect);
    const MergeInteractionLayout interaction = {
        .content_bottom = scroll_layout.content_rect.y + scroll_layout.content_rect.h,
        .result =
            MergeResultInteractionLayout{
                .rect = result_rect,
                .metrics = result_metrics,
                .lines =
                    VisibleLineRangeLayout{
                        .first_line_y = result_metrics.first_line_y,
                        .line_height = result_metrics.line_height,
                        .scroll_line = merge_tab->result_viewport.scroll_line(),
                        .visible_rows = result_metrics.visible_rows,
                    },
                .text = ComputeTextGridInteractionLayout(
                    result_rect, result_metrics.text_x, result_metrics.first_line_y,
                    result_metrics.line_height, text_renderer_.CharWidth(),
                    merge_tab->result_viewport.scroll_line(), merge_tab->result_viewport.line_count(),
                    merge_tab->result_viewport.horizontal_scroll(), result_metrics.visible_rows,
                    result_metrics.visible_columns),
            },
        .incoming_accept_button_width =
            ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Accept Theirs")),
        .current_accept_button_width =
            ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Accept Ours")),
        .result_action_widths =
            {
                ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Base")),
                ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Theirs")),
                ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Ours")),
                ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Both")),
            },
    };
    if (const auto hover = ClassifyMergeHoverState(surface_layout, interaction, *merge_tab, x, y);
        hover.has_value() && hover->kind == MergeHoverState::Kind::ResultAction) {
      return CursorKind::Pointer;
    }
    if (Contains(interaction.result.rect, x, y)) {
      return CursorKind::Text;
    }
    return CursorKind::Pointer;
  }

  for (const EditorSplitDividerLayout& divider :
       ComputeEditorSplitDividerLayouts(layout.editor_surface)) {
    if (Contains(divider.rect, x, y)) {
      return divider.rect.h > divider.rect.w ? CursorKind::EwResize : CursorKind::NsResize;
    }
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const auto pane_it =
      std::find_if(panes.begin(), panes.end(),
                   [&](const EditorPaneLayout& pane) { return Contains(pane.rect, x, y); });
  if (pane_it == panes.end()) {
    return CursorKind::Default;
  }

  const TabEntry::EditorTabState* editor_tab = ActiveEditorTab();
  const editor::TextViewport* viewport =
      pane_it->active ? &text_viewport_
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane_it->leaf_id)
                                               : nullptr);
  if (viewport == nullptr || viewport->is_placeholder()) {
    return CursorKind::Text;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane_it->rect);
  const auto scroll_layout = ComputeEditorScrollLayout(pane_it->rect, *viewport, metrics);
  if (scroll_layout.vertical_scrollbar.has_value() &&
      Contains(scroll_layout.vertical_scrollbar->track, x, y)) {
    return CursorKind::Default;
  }
  if (scroll_layout.horizontal_scrollbar.has_value() &&
      Contains(scroll_layout.horizontal_scrollbar->track, x, y)) {
    return CursorKind::Default;
  }
  if (const editor::EditorBlameLine* blame_line = EditorBlameLineAtPosition(x, y);
      blame_line != nullptr) {
    return blame_line->interactive ? CursorKind::Pointer : CursorKind::Default;
  }
  return CursorKind::Text;
}

SDL_Cursor* WorkspaceShell::CursorHandle(CursorKind kind) {
  switch (kind) {
    case CursorKind::Default:
      return SDL_GetDefaultCursor();
    case CursorKind::Text:
      if (text_cursor_ == nullptr) {
        text_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
      }
      return text_cursor_;
    case CursorKind::Pointer:
      if (pointer_cursor_ == nullptr) {
        pointer_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
      }
      return pointer_cursor_;
    case CursorKind::EwResize:
      if (ew_resize_cursor_ == nullptr) {
        ew_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
      }
      return ew_resize_cursor_;
    case CursorKind::NsResize:
      if (ns_resize_cursor_ == nullptr) {
        ns_resize_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
      }
      return ns_resize_cursor_;
  }

  return SDL_GetDefaultCursor();
}

void WorkspaceShell::UpdateMouseCursor(float x, float y) {
  last_mouse_x_ = x;
  last_mouse_y_ = y;
  last_mouse_position_valid_ = true;
  UpdateEditorBlameHover(x, y);

  const CursorKind next_kind = CursorKindForPosition(x, y);
  if (next_kind == cursor_kind_) {
    return;
  }

  if (SDL_Cursor* cursor = CursorHandle(next_kind); cursor != nullptr && SDL_SetCursor(cursor)) {
    cursor_kind_ = next_kind;
    return;
  }

  if (SDL_Cursor* default_cursor = CursorHandle(CursorKind::Default);
      default_cursor != nullptr && SDL_SetCursor(default_cursor)) {
    cursor_kind_ = CursorKind::Default;
  }
}

char WorkspaceShell::KeycodeToAscii(SDL_Keycode keycode, SDL_Keymod modifiers) {
  const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;

  if (keycode >= SDLK_A && keycode <= SDLK_Z) {
    const char base = static_cast<char>('a' + (keycode - SDLK_A));
    return shift ? static_cast<char>(std::toupper(static_cast<unsigned char>(base))) : base;
  }

  if (keycode >= SDLK_0 && keycode <= SDLK_9) {
    static constexpr char shifted_digits[] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
    const int index = keycode - SDLK_0;
    return shift ? shifted_digits[index] : static_cast<char>('0' + index);
  }

  switch (keycode) {
    case SDLK_SPACE:
      return ' ';
    case SDLK_SLASH:
      return shift ? '?' : '/';
    case SDLK_BACKSLASH:
      return shift ? '|' : '\\';
    case SDLK_PERIOD:
      return shift ? '>' : '.';
    case SDLK_COMMA:
      return shift ? '<' : ',';
    case SDLK_MINUS:
      return shift ? '_' : '-';
    case SDLK_EQUALS:
      return shift ? '+' : '=';
    case SDLK_SEMICOLON:
      return shift ? ':' : ';';
    case SDLK_APOSTROPHE:
      return shift ? '"' : '\'';
    case SDLK_LEFTBRACKET:
      return shift ? '{' : '[';
    case SDLK_RIGHTBRACKET:
      return shift ? '}' : ']';
    default:
      return '\0';
  }
}

}  // namespace microide::workspace
