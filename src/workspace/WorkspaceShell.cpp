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

#include "workspace/WorkspaceConstants.h"
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
constexpr Uint64 kCaretBlinkIntervalMs = 530;
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
      ActionSpec{ActionId::CloseAllTabs, "", "", "Close All Tabs", ""},
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
    case ActionId::CloseAllTabs:
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
      return (ActiveEditableViewport() != nullptr && ActiveEditableViewport()->has_selection()) ||
             (surface_.focus == FocusTarget::Panel && TerminalHasSelection());
    case ActionId::CutSelection:
    case ActionId::Redo:
    case ActionId::SelectAll:
    case ActionId::Undo:
      return ActiveEditableViewport() != nullptr;
    case ActionId::PasteClipboard:
      return ActiveEditableViewport() != nullptr ||
             (surface_.focus == FocusTarget::Panel && ActiveTerminalTab() != nullptr);
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
      item(ActionId::CloseAllTabs),
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
  static const auto kTerminalContextItems = std::to_array<MenuItemSpec>({
      item(ActionId::CopySelection),
      item(ActionId::PasteClipboard),
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
      MenuSpec{MenuId::TerminalContext, "Terminal", kTerminalContextItems},
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

void WorkspaceShell::SetWindowPresentationState(WindowPresentationState state) {
  presentation_scale_x_ =
      std::isfinite(state.scale_x) && state.scale_x > 0.0f ? state.scale_x : 1.0f;
  presentation_scale_y_ =
      std::isfinite(state.scale_y) && state.scale_y > 0.0f ? state.scale_y : 1.0f;
  if (state.logical_width > 0) {
    last_window_width_ = state.logical_width;
  }
  if (state.logical_height > 0) {
    last_window_height_ = state.logical_height;
  }
  window_chrome_ = state.chrome;
}

SDL_HitTestResult WorkspaceShell::WindowHitTest(float x, float y) const {
  if (!window_chrome_.custom_enabled || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return SDL_HITTEST_NORMAL;
  }

  const float window_width = static_cast<float>(last_window_width_);
  const float window_height = static_cast<float>(last_window_height_);
  if (x < 0.0f || y < 0.0f || x >= window_width || y >= window_height) {
    return SDL_HITTEST_NORMAL;
  }

  if (window_chrome_.ResizableFrameEnabled()) {
    const bool left = x < kWindowFrameHitThickness;
    const bool right = x >= window_width - kWindowFrameHitThickness;
    const bool top = y < kWindowFrameHitThickness;
    const bool bottom = y >= window_height - kWindowFrameHitThickness;
    if (left || right || top || bottom) {
      return ResizeHitTestResult(left, right, top, bottom);
    }
  }

  return SDL_HITTEST_NORMAL;
}

bool WorkspaceShell::WindowDragRegionContains(float x, float y) const {
  if (!window_chrome_.custom_enabled || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return false;
  }

  const float window_width = static_cast<float>(last_window_width_);
  const float window_height = static_cast<float>(last_window_height_);
  if (x < 0.0f || y < 0.0f || x >= window_width || y >= window_height) {
    return false;
  }

  if (surface_.menu_bar_open || surface_.tree_context_menu.open) {
    return false;
  }

  const WorkspaceLayout layout =
      ComputeLayout(window_width, window_height, surface_.sidebar_visible, BottomPanelVisible(),
                    surface_.sidebar_width, surface_.bottom_panel_height);
  if (!Contains(layout.menu_bar, x, y)) {
    return false;
  }

  for (const VisibleMenuBarItem& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
    if (Contains(item.rect, x, y)) {
      return false;
    }
  }
  for (const VisibleWindowControlButton& button :
       ComputeVisibleWindowControlButtons(layout.menu_bar)) {
    if (Contains(button.rect, x, y)) {
      return false;
    }
  }

  return true;
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

bool WorkspaceShell::WritePrimarySelectionText(std::string_view text) const {
  if (text.empty()) {
    return false;
  }
  if (primary_selection_text_writer_) {
    return primary_selection_text_writer_(text);
  }
  return SDL_SetPrimarySelectionText(std::string(text).c_str());
}

std::optional<std::string> WorkspaceShell::ReadPrimarySelectionText() const {
  if (primary_selection_text_reader_) {
    return primary_selection_text_reader_();
  }

  char* selection_text = SDL_GetPrimarySelectionText();
  if (selection_text == nullptr) {
    return std::nullopt;
  }

  std::string copied_text(selection_text);
  SDL_free(selection_text);
  return copied_text;
}

void WorkspaceShell::SyncPrimarySelectionWithActiveEditor() {
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || !viewport->has_selection()) {
    return;
  }

  const std::string text = viewport->SelectedText();
  if (!text.empty()) {
    WritePrimarySelectionText(text);
  }
}

void WorkspaceShell::SyncPrimarySelectionWithTerminalSelection() {
  if (!TerminalHasSelection()) {
    return;
  }

  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return;
  }

  const std::string text = SelectedTerminalText(terminal_tab->session.SnapshotLines());
  if (!text.empty()) {
    WritePrimarySelectionText(text);
  }
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

    if (Contains(TreeSidebarCollapseButtonRect(layout.sidebar), x, y) &&
        directory_tree_.CanCollapseAll()) {
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
    const SDL_FRect divider_rect =
        CompareDividerHitRect(layout.editor_surface, surface_layout);
    if (Contains(divider_rect, x, y)) {
      return CursorKind::EwResize;
    }
    if (compare_tab->right_editable && x >= surface_layout.right_x) {
      return CursorKind::Text;
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
    if (Contains(interaction.result.rect, x, y) ||
        (x >= surface_layout.center_x && x < surface_layout.right_x && y >= surface_layout.rows_y)) {
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
