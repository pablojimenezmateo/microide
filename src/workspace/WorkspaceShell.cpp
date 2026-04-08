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

#include "editor/SyntaxHighlighter.h"
#include "project/FileOperationService.h"
#include "project/GitStatusService.h"
#include "util/StartupTrace.h"
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
constexpr float kStatusBarHeight = 22.0f;
constexpr float kDivider = 1.0f;
constexpr float kResizeHandleThickness = 6.0f;
constexpr float kSidebarHeaderHeight = 30.0f;
constexpr float kBottomPanelHeaderHeight = 28.0f;
constexpr float kBottomPanelHeaderButtonSize = 18.0f;
constexpr float kSidebarInset = 10.0f;
constexpr float kSidebarRowHeight = 20.0f;
constexpr float kSearchSidebarResultsTop = 88.0f;
constexpr float kTreeIndentWidth = 14.0f;
constexpr float kTreeChevronSlotWidth = 12.0f;
constexpr float kOverlayMinWidth = 520.0f;
constexpr float kOverlayMaxWidth = 840.0f;
constexpr float kOverlayMinHeight = 220.0f;
constexpr float kOverlayMaxHeight = 360.0f;
constexpr float kDirtyPromptWidth = 460.0f;
constexpr float kDirtyPromptHeight = 176.0f;
constexpr float kDirtyPromptButtonWidth = 96.0f;
constexpr float kDirtyPromptButtonHeight = 28.0f;
constexpr float kDirtyPromptButtonGap = 10.0f;
constexpr float kPromptSurfaceWidth = 520.0f;
constexpr float kPromptSurfaceHeight = 188.0f;
constexpr float kPromptSurfaceInputHeight = 24.0f;
constexpr float kPromptSurfaceButtonWidth = 108.0f;
constexpr float kPromptSurfaceButtonHeight = 28.0f;
constexpr float kPromptSurfaceButtonGap = 10.0f;
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
constexpr float kMergeToolbarHeight = 54.0f;
constexpr float kMergeToolbarButtonHeight = 22.0f;
constexpr float kMergeToolbarButtonGap = 8.0f;
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

SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full) {
  const float width = std::min(kDirtyPromptWidth, full.w - 32.0f);
  const float height = std::min(kDirtyPromptHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog) {
  const float total_width =
      kDirtyPromptButtonWidth * 3.0f + kDirtyPromptButtonGap * 2.0f;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kDirtyPromptButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + kDirtyPromptButtonWidth + kDirtyPromptButtonGap, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + (kDirtyPromptButtonWidth + kDirtyPromptButtonGap) * 2.0f, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceRect(const SDL_FRect& full) {
  const float width = std::min(kPromptSurfaceWidth, full.w - 32.0f);
  const float height = std::min(kPromptSurfaceHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 2> ComputePromptSurfaceButtonRects(const SDL_FRect& dialog) {
  const float total_width =
      kPromptSurfaceButtonWidth * 2.0f + kPromptSurfaceButtonGap;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kPromptSurfaceButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
      MakeRect(start_x + kPromptSurfaceButtonWidth + kPromptSurfaceButtonGap, y,
               kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceInputRect(const SDL_FRect& dialog) {
  return MakeRect(dialog.x + 16.0f, dialog.y + 98.0f, dialog.w - 32.0f, kPromptSurfaceInputHeight);
}

std::size_t MaxVisualColumns(const editor::TextViewport& viewport) {
  return viewport.max_visual_columns();
}

std::string EditorTabLabel(const editor::TextViewport& viewport) {
  if (!viewport.path().empty()) {
    return viewport.path().filename().string();
  }
  return viewport.is_placeholder() ? "welcome" : "untitled";
}

bool ParseLineColumnSpec(std::string_view location,
                         long long* line,
                         std::size_t* column,
                         bool allow_zero_line) {
  if (line == nullptr || column == nullptr || location.empty()) {
    return false;
  }

  const std::size_t colon = location.find(':');
  const std::string line_text(location.substr(0, colon));
  const std::string column_text =
      colon == std::string_view::npos ? std::string{} : std::string(location.substr(colon + 1));

  long long parsed_line = 0;
  std::size_t parsed_column = 0;
  try {
    parsed_line = std::stoll(line_text);
    if (!column_text.empty()) {
      parsed_column = static_cast<std::size_t>(std::stoull(column_text));
    }
  } catch (...) {
    return false;
  }

  if (!allow_zero_line && parsed_line == 0) {
    return false;
  }

  *line = parsed_line;
  *column = parsed_column;
  return true;
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
      ActionSpec{ActionId::Grep, "grep", "grep <query>", "Show Project Search", ""},
      ActionSpec{ActionId::Help, "help", "help", "Help", ""},
      ActionSpec{ActionId::Hsplit, "hsplit", "hsplit [path]", "Split Down", ""},
      ActionSpec{ActionId::IndentWidth, "indent-width", "indent-width [n]", "Indent Width",
                 ""},
      ActionSpec{ActionId::Jump, "jump", "jump <line[:col]>", "Jump Relative", ""},
      ActionSpec{ActionId::Open, "open", "open <path>", "Open File", ""},
      ActionSpec{ActionId::OpenSelectedTreeItem, "", "", "Open", ""},
      ActionSpec{ActionId::OpenSelectedTreeItemInNewTab, "", "", "Open in New Tab", ""},
      ActionSpec{ActionId::PanelHide, "panel-hide", "panel-hide", "Hide Bottom Panel", ""},
      ActionSpec{ActionId::PanelShow, "panel-show", "panel-show", "Show Bottom Panel", ""},
      ActionSpec{ActionId::ProjectClose, "project-close", "project-close", "Close Project", ""},
      ActionSpec{ActionId::ProjectNext, "project-next", "project-next", "Next Project", ""},
      ActionSpec{ActionId::ProjectOpen, "project-open", "project-open <path>", "Open Project",
                 ""},
      ActionSpec{ActionId::ProjectPrev, "project-prev", "project-prev", "Previous Project", ""},
      ActionSpec{ActionId::Quit, "quit", "quit", "Quit", ""},
      ActionSpec{ActionId::RenamePath, "", "", "Rename...", ""},
      ActionSpec{ActionId::Reopen, "reopen", "reopen", "Reopen", ""},
      ActionSpec{ActionId::Rg, "rg", "rg <query>", "Find in Project", "Ctrl+Shift+F"},
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
      ActionSpec{ActionId::CopySelection, "", "", "Copy", "Ctrl+C"},
      ActionSpec{ActionId::CutSelection, "", "", "Cut", "Ctrl+X"},
      ActionSpec{ActionId::OpenCommandPrompt, "", "", "Command Prompt", "Ctrl+E"},
      ActionSpec{ActionId::PasteClipboard, "", "", "Paste", "Ctrl+V"},
      ActionSpec{ActionId::Redo, "", "", "Redo", "Ctrl+Y / Ctrl+Shift+Z"},
      ActionSpec{ActionId::ReplaceInBuffer, "", "", "Replace in Buffer", "Ctrl+H"},
      ActionSpec{ActionId::SelectAll, "", "", "Select All", "Ctrl+A"},
      ActionSpec{ActionId::ToggleBottomPanel, "", "", "Toggle Bottom Panel", "F9", true},
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

const std::string& WorkspaceShell::CommandHelpSummary() {
  static const std::string kSummary = [] {
    std::string summary;
    for (const ActionSpec& spec : ActionSpecs()) {
      if (spec.command_usage.empty()) {
        continue;
      }
      if (!summary.empty()) {
        summary += ", ";
      }
      summary += spec.command_usage;
    }
    return summary;
  }();
  return kSummary;
}

bool WorkspaceShell::IsActionEnabled(ActionId id) const {
  switch (id) {
    case ActionId::Colorscheme:
    case ActionId::Files:
    case ActionId::Help:
    case ActionId::OpenCommandPrompt:
    case ActionId::ProjectOpen:
    case ActionId::Quit:
    case ActionId::SidebarClose:
    case ActionId::SidebarHide:
    case ActionId::SidebarShow:
    case ActionId::SidebarToggle:
    case ActionId::ToggleBottomPanel:
      return true;
    case ActionId::CloseActiveTab:
      return !open_tabs_.empty();
    case ActionId::CompareHead:
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab:
      return !project_root_.empty() &&
             (tree_context_menu_.open ? tree_context_menu_.target : SelectedTreeTargetKind()) ==
                 TreeContextTargetKind::File;
    case ActionId::CreateDirectory:
    case ActionId::CreateFile: {
      if (project_root_.empty()) {
        return false;
      }
      const TreeContextTargetKind target =
          tree_context_menu_.open ? tree_context_menu_.target : SelectedTreeTargetKind();
      return target == TreeContextTargetKind::Directory || target == TreeContextTargetKind::Root ||
             target == TreeContextTargetKind::Background;
    }
    case ActionId::DeletePath:
    case ActionId::RenamePath: {
      if (project_root_.empty()) {
        return false;
      }
      const TreeContextTargetKind target =
          tree_context_menu_.open ? tree_context_menu_.target : SelectedTreeTargetKind();
      return target == TreeContextTargetKind::File || target == TreeContextTargetKind::Directory;
    }
    case ActionId::Compare:
    case ActionId::Find:
    case ActionId::Grep:
    case ActionId::Merge:
    case ActionId::Open:
    case ActionId::ProjectClose:
    case ActionId::Rg:
    case ActionId::Tab:
    case ActionId::Term:
    case ActionId::Tree:
    case ActionId::TreeRefresh:
      return !project_root_.empty();
    case ActionId::CopySelection:
    case ActionId::CutSelection:
    case ActionId::Goto:
    case ActionId::Hsplit:
    case ActionId::Jump:
    case ActionId::PasteClipboard:
    case ActionId::Redo:
    case ActionId::ReplaceInBuffer:
    case ActionId::Reopen:
    case ActionId::Search:
    case ActionId::SelectAll:
    case ActionId::SplitFirst:
    case ActionId::SplitLast:
    case ActionId::SplitNext:
    case ActionId::SplitPrev:
    case ActionId::Undo:
    case ActionId::Unsplit:
    case ActionId::Vsplit:
      return ActiveTabIsEditor();
    case ActionId::Save:
      return ActiveTabIsEditor() || ActiveTabIsMerge();
    case ActionId::Focus:
      return true;
    case ActionId::IndentWidth:
    case ActionId::PanelHide:
    case ActionId::PanelShow:
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
      return !project_root_.empty() && projects_.size() > 1;
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
    return MenuItemSpec{ActionId::Help, {}, {}, {}, 0, true, false, MenuId::None};
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
      item(ActionId::PasteClipboard),
      item(ActionId::SelectAll),
  });
  static const auto kViewItems = std::to_array<MenuItemSpec>({
      item(ActionId::SidebarToggle, {}, {}, {}, 0, true),
      item(ActionId::Help, "Sidebar Mode", {}, {}, 0, false, MenuId::SidebarMode),
      separator(),
      item(ActionId::ToggleBottomPanel, {}, {}, {}, 0, true),
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
      item(ActionId::SidebarShow, "Tree", {}, std::array<std::string_view, 2>{"tree", {}}, 1, true),
      item(ActionId::SidebarShow, "Search", {}, std::array<std::string_view, 2>{"search", {}}, 1,
           true),
      item(ActionId::SidebarShow, "Git", {}, std::array<std::string_view, 2>{"git", {}}, 1, true),
  });
  static const auto kSearchItems = std::to_array<MenuItemSpec>({
      item(ActionId::Search),
      item(ActionId::ReplaceInBuffer),
      item(ActionId::Files),
      item(ActionId::Rg),
  });
  static const auto kProjectItems = std::to_array<MenuItemSpec>({
      item(ActionId::Compare, "Compare Current File..."),
      item(ActionId::TreeRefresh),
      separator(),
      item(ActionId::ProjectNext),
      item(ActionId::ProjectPrev),
  });
  static const auto kTerminalItems = std::to_array<MenuItemSpec>({
      item(ActionId::Term),
  });
  static const auto kHelpItems = std::to_array<MenuItemSpec>({
      item(ActionId::Help, "Command Summary"),
  });
  static const auto kMenus = std::to_array<MenuSpec>({
      MenuSpec{MenuId::File, "File", kFileItems},
      MenuSpec{MenuId::Edit, "Edit", kEditItems},
      MenuSpec{MenuId::View, "View", kViewItems},
      MenuSpec{MenuId::SidebarMode, "Sidebar Mode", kSidebarModeItems},
      MenuSpec{MenuId::Search, "Search", kSearchItems},
      MenuSpec{MenuId::Project, "Project", kProjectItems},
      MenuSpec{MenuId::Terminal, "Terminal", kTerminalItems},
      MenuSpec{MenuId::Help, "Help", kHelpItems},
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
  if (sidebar_mode_ != SidebarMode::Tree) {
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

bool WorkspaceShell::Initialize(const std::filesystem::path& project_root) {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::Initialize");
  caret_blink_epoch_ms_ = SDL_GetTicks();
  cursor_kind_ = CursorKind::Default;
  last_mouse_position_valid_ = false;
  quit_requested_ = false;
  dirty_prompt_visible_ = false;
  projects_.clear();
  active_project_index_ = 0;
  project_tab_scroll_index_ = 0;

  project_search_event_type_ = SDL_RegisterEvents(1);
  if (project_search_event_type_ != static_cast<Uint32>(-1)) {
    project_search_service_.SetWakeEventType(project_search_event_type_);
  } else {
    project_search_event_type_ = 0;
  }

  terminal_event_type_ = SDL_RegisterEvents(1);
  if (terminal_event_type_ == static_cast<Uint32>(-1)) {
    terminal_event_type_ = 0;
  }

  {
    util::StartupTrace::Scope restore_config_scope("WorkspaceShell::RestoreUserConfig");
    RestoreUserConfig();
  }
  {
    util::StartupTrace::Scope refresh_colors_scope(
        "WorkspaceShell::RefreshAvailableColorschemeNames");
    RefreshAvailableColorschemeNames();
  }
  {
    util::StartupTrace::Scope reset_state_scope("WorkspaceShell::ResetProjectScopedState");
    ResetProjectScopedState(true);
  }

  {
    util::StartupTrace::Scope restore_workspace_scope("WorkspaceShell::RestoreWorkspaceSession");
    if (RestoreWorkspaceSession()) {
      return true;
    }
  }

  if (project_root.empty()) {
    return true;
  }

  util::StartupTrace::Scope open_project_scope("WorkspaceShell::OpenProjectTab");
  return OpenProjectTab(project_root, true, true);
}

void WorkspaceShell::Shutdown() {
  SaveUserConfig();

  if (!projects_.empty() && !project_root_.empty() && active_project_index_ < projects_.size()) {
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[active_project_index_]);
  }

  for (std::size_t i = 0; i < projects_.size(); ++i) {
    if (projects_[i] == nullptr || !projects_[i]->initialized || i == active_project_index_) {
      continue;
    }
    LoadProjectState(*projects_[i]);
    SaveConfigState();
    SaveSessionState();
    StoreCurrentProjectState(*projects_[i]);
  }
  SaveWorkspaceSession();

  StopProjectSearch();
  terminal_tabs_.clear();

  if (SDL_Cursor* default_cursor = SDL_GetDefaultCursor(); default_cursor != nullptr) {
    SDL_SetCursor(default_cursor);
  }

  if (text_cursor_ != nullptr) {
    SDL_DestroyCursor(text_cursor_);
    text_cursor_ = nullptr;
  }
  if (pointer_cursor_ != nullptr) {
    SDL_DestroyCursor(pointer_cursor_);
    pointer_cursor_ = nullptr;
  }
  if (ew_resize_cursor_ != nullptr) {
    SDL_DestroyCursor(ew_resize_cursor_);
    ew_resize_cursor_ = nullptr;
  }
  if (ns_resize_cursor_ != nullptr) {
    SDL_DestroyCursor(ns_resize_cursor_);
    ns_resize_cursor_ = nullptr;
  }

  cursor_kind_ = CursorKind::Default;
  last_mouse_position_valid_ = false;
}

void WorkspaceShell::RequestQuit() {
  if (dirty_prompt_visible_) {
    focus_ = FocusTarget::Overlay;
    return;
  }

  std::size_t dirty_count = DirtyEditorTabIndices().size();
  for (std::size_t i = 0; i < projects_.size(); ++i) {
    if (!project_root_.empty() && i == active_project_index_) {
      continue;
    }
    dirty_count += DirtyEditorTabIndicesForProject(i).size();
  }

  if (dirty_count == 0) {
    quit_requested_ = true;
    return;
  }

  ShowDirtyPromptForQuit();
}

bool WorkspaceShell::ConsumeQuitRequested() {
  const bool requested = quit_requested_;
  quit_requested_ = false;
  return requested;
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

  if (menu_bar_open_ || tree_context_menu_.open) {
    return SDL_HITTEST_NORMAL;
  }

  const WorkspaceLayout layout =
      ComputeLayout(window_width, window_height, sidebar_visible_, bottom_panel_visible_,
                    sidebar_width_, bottom_panel_height_);
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
  return focus_ == FocusTarget::Editor && !command_mode_ && !dirty_prompt_visible_ &&
         !overlay_visible_ && !menu_bar_open_ && !tree_context_menu_.open &&
         ActiveTabIsEditor() && !text_viewport_.is_placeholder();
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

int WorkspaceShell::CompareVisibleRows(const SDL_FRect& rect) const {
  const float line_height = text_renderer_.LineHeight();
  const float rows_y = rect.y + line_height + 12.0f;
  return std::max(
      1, static_cast<int>((rect.h - (rows_y - rect.y) - 8.0f) / std::max(1.0f, line_height)));
}

int WorkspaceShell::CompareMaxScrollRow(const CompareTabState& compare_tab, int visible_rows) const {
  return std::max(0, static_cast<int>(compare_tab.model.rows.size()) - std::max(1, visible_rows));
}

void WorkspaceShell::ClampCompareScrollRow(CompareTabState& compare_tab, int visible_rows) const {
  compare_tab.scroll_row =
      std::clamp(compare_tab.scroll_row, 0, CompareMaxScrollRow(compare_tab, visible_rows));
}

void WorkspaceShell::RevealActiveCompareSelection() {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (compare_tab == nullptr || last_window_width_ <= 0 || last_window_height_ <= 0) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  const int visible_rows = CompareVisibleRows(layout.editor_surface);
  ClampCompareScrollRow(*compare_tab, visible_rows);
  if (compare_tab->selected_row < static_cast<std::size_t>(compare_tab->scroll_row)) {
    compare_tab->scroll_row = static_cast<int>(compare_tab->selected_row);
  } else if (compare_tab->selected_row >=
             static_cast<std::size_t>(compare_tab->scroll_row + visible_rows)) {
    compare_tab->scroll_row = static_cast<int>(compare_tab->selected_row) - visible_rows + 1;
  }
  ClampCompareScrollRow(*compare_tab, visible_rows);
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

int WorkspaceShell::MergeVisibleRows(const SDL_FRect& rect) const {
  const float line_height = text_renderer_.LineHeight();
  const float rows_y = rect.y + kMergeToolbarHeight + line_height + 12.0f;
  return std::max(
      1, static_cast<int>((rect.h - (rows_y - rect.y) - 8.0f) / std::max(1.0f, line_height)));
}

int WorkspaceShell::MergeMaxScrollRow(const MergeTabState& merge_tab, int visible_rows) const {
  return std::max(0,
                  static_cast<int>(merge_tab.display_model.rows.size()) - std::max(1, visible_rows));
}

void WorkspaceShell::ClampMergeScrollRow(MergeTabState& merge_tab, int visible_rows) const {
  merge_tab.scroll_row =
      std::clamp(merge_tab.scroll_row, 0, MergeMaxScrollRow(merge_tab, visible_rows));
}

void WorkspaceShell::RevealActiveMergeSelection() {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr || last_window_width_ <= 0 || last_window_height_ <= 0 ||
      merge_tab->display_model.hunks.empty()) {
    return;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  const int visible_rows = MergeVisibleRows(layout.editor_surface);
  ClampMergeScrollRow(*merge_tab, visible_rows);
  const auto& selected_hunk =
      merge_tab->display_model.hunks[std::min(merge_tab->selected_hunk,
                                              merge_tab->display_model.hunks.size() - 1)];
  if (selected_hunk.start_row < merge_tab->scroll_row) {
    merge_tab->scroll_row = selected_hunk.start_row;
  } else if (selected_hunk.end_row >= merge_tab->scroll_row + visible_rows) {
    merge_tab->scroll_row = selected_hunk.end_row - visible_rows + 1;
  }
  ClampMergeScrollRow(*merge_tab, visible_rows);
}

std::string WorkspaceShell::ActiveTabTitle() const {
  if (active_tab_index_ >= open_tabs_.size()) {
    return EditorTabLabel(text_viewport_);
  }
  return open_tabs_[active_tab_index_].title;
}

bool WorkspaceShell::SaveTab(std::size_t index) {
  if (index >= open_tabs_.size()) {
    return false;
  }

  if (open_tabs_[index].kind == TabEntry::Kind::Merge && open_tabs_[index].merge.has_value()) {
    auto& merge_tab = open_tabs_[index].merge.value();
    if (!merge_tab.result_viewport.dirty()) {
      return true;
    }
    if (!merge_tab.result_viewport.Save()) {
      return false;
    }
    directory_tree_.Refresh();
    return true;
  }

  if (open_tabs_[index].kind != TabEntry::Kind::Editor) {
    return false;
  }

  auto& editor_state = open_tabs_[index].editor_state;
  if (!editor_state.has_value() || editor_state->views.empty()) {
    return false;
  }

  if (index == active_tab_index_) {
    SyncActiveEditorTab();
  }

  bool attempted_save = false;
  for (auto& view : editor_state->views) {
    editor::TextViewport* candidate = &view.viewport;
    if (index == active_tab_index_ && view.leaf_id == editor_state->active_leaf_id) {
      candidate = &text_viewport_;
    }

    if (candidate->path().empty()) {
      if (candidate->dirty()) {
        return false;
      }
      continue;
    }

    if (!candidate->dirty()) {
      continue;
    }

    if (!candidate->Save()) {
      return false;
    }
    attempted_save = true;

    if (index == active_tab_index_ && candidate == &text_viewport_) {
      view.viewport = text_viewport_;
    }
  }

  if (index == active_tab_index_) {
    SyncActiveEditorTab();
  }
  if (attempted_save) {
    directory_tree_.Refresh();
  }
  return attempted_save || !editor_state->views.empty();
}

bool WorkspaceShell::TabIsDirty(std::size_t index) const {
  if (index >= open_tabs_.size()) {
    return false;
  }

  if (open_tabs_[index].kind == TabEntry::Kind::Merge && open_tabs_[index].merge.has_value()) {
    return open_tabs_[index].merge->result_viewport.dirty();
  }

  if (open_tabs_[index].kind != TabEntry::Kind::Editor) {
    return false;
  }

  const auto& editor_state = open_tabs_[index].editor_state;
  if (!editor_state.has_value() || editor_state->views.empty()) {
    return false;
  }

  for (const auto& view : editor_state->views) {
    if (index == active_tab_index_ && view.leaf_id == editor_state->active_leaf_id) {
      if (text_viewport_.dirty()) {
        return true;
      }
      continue;
    }
    if (view.viewport.dirty()) {
      return true;
    }
  }
  return false;
}

std::string WorkspaceShell::TabDisplayTitle(std::size_t index) const {
  if (index >= open_tabs_.size()) {
    return {};
  }

  const std::string& title = open_tabs_[index].title;
  return TabIsDirty(index) ? "*" + title : title;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices() const {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(open_tabs_.size());
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    if (TabIsDirty(i)) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices(
    const ProjectWorkspaceState& state) {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state.open_tabs.size());
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    const auto& tab = state.open_tabs[i];
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() && tab.merge->result_viewport.dirty()) {
      dirty_tabs.push_back(i);
      continue;
    }
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
        tab.editor_state->views.empty()) {
      continue;
    }
    const bool dirty = std::any_of(tab.editor_state->views.begin(), tab.editor_state->views.end(),
                                   [](const auto& view) { return view.viewport.dirty(); });
    if (dirty) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndicesForProject(
    std::size_t project_index) const {
  if (project_index >= projects_.size()) {
    return {};
  }
  if (!project_root_.empty() && project_index == active_project_index_) {
    return DirtyEditorTabIndices();
  }
  return projects_[project_index] == nullptr ? std::vector<std::size_t>{}
                                             : DirtyEditorTabIndices(*projects_[project_index]);
}


void WorkspaceShell::ReloadCleanEditorTabsForPath(const std::filesystem::path& path) {
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    auto& tab = open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() || TabIsDirty(i)) {
      continue;
    }

    editor::TextViewport reopened_view;
    if (!reopened_view.OpenFile(path)) {
      continue;
    }
    ApplyEditorPreferences(reopened_view);

    bool reloaded_any = false;
    for (auto& view : tab.editor_state->views) {
      const bool active_view =
          i == active_tab_index_ && view.leaf_id == tab.editor_state->active_leaf_id &&
          !view.needs_restore;
      const std::filesystem::path current_path =
          active_view ? text_viewport_.path().lexically_normal() : EditorViewPath(view);
      if (current_path != path.lexically_normal()) {
        continue;
      }
      view.viewport = reopened_view;
      view.restored_path = path.lexically_normal();
      view.restored_cursor_line = reopened_view.cursor_line();
      view.restored_cursor_column = reopened_view.cursor_column();
      view.restored_scroll_line = reopened_view.scroll_line();
      view.restored_horizontal_scroll = reopened_view.horizontal_scroll();
      view.needs_restore = false;
      if (active_view) {
        text_viewport_ = reopened_view;
      }
      reloaded_any = true;
    }
    if (reloaded_any && i == active_tab_index_) {
      NormalizeEditorSplitTree(*tab.editor_state);
      SyncActiveEditorTabMetadata();
    }
  }
}

std::optional<std::size_t> WorkspaceShell::FindOpenCompareTabIndex(
    const std::filesystem::path& path,
    std::string_view left_ref,
    std::string_view right_ref) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    const auto& tab = open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value()) {
      continue;
    }
    if (tab.compare->path == normalized_path && tab.compare->commit_hash == left_ref &&
        tab.compare->right_ref == right_ref) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> WorkspaceShell::FindOpenMergeTabIndex(
    const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    const auto& tab = open_tabs_[i];
    if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value()) {
      continue;
    }
    if (tab.merge->output_path == normalized_path) {
      return i;
    }
  }
  return std::nullopt;
}

bool WorkspaceShell::OpenWorkingTreeComparison(const std::filesystem::path& path,
                                               const std::string& left_ref,
                                               const std::string& left_label) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, "WORKTREE");
      existing_index.has_value()) {
    SyncActiveEditorTab();
    active_tab_index_ = *existing_index;
    RevealActiveCompareSelection();
    EnsureActiveTabVisible();
    focus_ = FocusTarget::Editor;
    return true;
  }
  const auto left_content = project::ReadGitFileAtCommit(project_root_, normalized_path, left_ref);
  if (!left_content.has_value()) {
    LogMessage("Failed to read git content for comparison");
    return false;
  }
  const std::optional<std::string> working_content = ReadFileText(normalized_path);
  auto compare_tab = BuildCompareTabFromBuffers(
      normalized_path, left_content->exists ? left_content->content : "", working_content.value_or(""),
      left_label, "Working tree", 0, true);
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    LogMessage("Failed to build comparison");
    return false;
  }
  compare_tab->compare->commit_hash = left_ref;
  compare_tab->compare->right_ref = "WORKTREE";
  SyncActiveEditorTab();
  open_tabs_.push_back(std::move(*compare_tab));
  active_tab_index_ = open_tabs_.size() - 1;
  RevealActiveCompareSelection();
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  LogMessage("Comparison opened");
  return true;
}

bool WorkspaceShell::OpenBranchHeadComparison(const std::filesystem::path& path,
                                              const std::string& left_ref,
                                              const std::string& left_label,
                                              const std::string& right_ref,
                                              const std::string& right_label) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const auto existing_index = FindOpenCompareTabIndex(normalized_path, left_ref, right_ref);
      existing_index.has_value()) {
    SyncActiveEditorTab();
    active_tab_index_ = *existing_index;
    RevealActiveCompareSelection();
    EnsureActiveTabVisible();
    focus_ = FocusTarget::Editor;
    return true;
  }
  const auto left_content = project::ReadGitFileAtCommit(project_root_, normalized_path, left_ref);
  const auto right_content = project::ReadGitFileAtCommit(project_root_, normalized_path, right_ref);
  if (!left_content.has_value() || !right_content.has_value()) {
    LogMessage("Failed to read branch content for comparison");
    return false;
  }
  auto compare_tab = BuildCompareTabFromBuffers(
      normalized_path, left_content->exists ? left_content->content : "",
      right_content->exists ? right_content->content : "", left_label, right_label, 0, false);
  if (!compare_tab.has_value() || !compare_tab->compare.has_value()) {
    LogMessage("Failed to build comparison");
    return false;
  }
  compare_tab->compare->commit_hash = left_ref;
  compare_tab->compare->right_ref = right_ref;
  SyncActiveEditorTab();
  open_tabs_.push_back(std::move(*compare_tab));
  active_tab_index_ = open_tabs_.size() - 1;
  RevealActiveCompareSelection();
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  LogMessage("Comparison opened");
  return true;
}

bool WorkspaceShell::OpenGitConflictMerge(const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const auto existing_index = FindOpenMergeTabIndex(normalized_path); existing_index.has_value()) {
    SyncActiveEditorTab();
    active_tab_index_ = *existing_index;
    RevealActiveMergeSelection();
    EnsureActiveTabVisible();
    focus_ = FocusTarget::Editor;
    return true;
  }
  const auto base_content = project::ReadGitFileAtCommit(project_root_, normalized_path, ":1");
  const auto current_content = project::ReadGitFileAtCommit(project_root_, normalized_path, ":2");
  const auto incoming_content = project::ReadGitFileAtCommit(project_root_, normalized_path, ":3");
  if (!current_content.has_value() || !incoming_content.has_value()) {
    LogMessage("Failed to read merge conflict stages");
    return false;
  }

  auto merge_tab = BuildMergeTabFromBuffers(
      normalized_path, base_content.has_value() && base_content->exists ? base_content->content : "",
      incoming_content->exists ? incoming_content->content : "",
      current_content->exists ? current_content->content : "", "Incoming", "Result", "Current", 0,
      false);
  if (!merge_tab.has_value() || !merge_tab->merge.has_value()) {
    LogMessage("Failed to build merge editor");
    return false;
  }
  merge_tab->merge->base_path = normalized_path;
  merge_tab->merge->incoming_path = normalized_path;
  merge_tab->merge->current_path = normalized_path;
  SyncActiveEditorTab();
  open_tabs_.push_back(std::move(*merge_tab));
  active_tab_index_ = open_tabs_.size() - 1;
  RevealActiveMergeSelection();
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  LogMessage("Merge editor opened");
  return true;
}

void WorkspaceShell::MoveFileFinderSelection(int delta) {
  file_finder_.MoveSelection(delta);
  if (overlay_visible_ && last_window_width_ > 0 && last_window_height_ > 0) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                      sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
    RevealOverlaySelection(ComputeOverlayRect(layout.editor_area));
  }
}

bool WorkspaceShell::OpenUntitledTab() {
  if (project_root_.empty()) {
    LogMessage("No project is loaded");
    return false;
  }
  SyncActiveEditorTab();

  editor::TextViewport untitled_view;
  untitled_view.SetUntitledBuffer();
  ApplyEditorPreferences(untitled_view);
  text_viewport_ = untitled_view;

  open_tabs_.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = {},
      .title = "untitled",
      .editor_state = MakeEditorTabState(untitled_view),
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  active_tab_index_ = open_tabs_.size() - 1;
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  return true;
}

bool WorkspaceShell::OpenFileInNewTab(const std::filesystem::path& path) {
  if (project_root_.empty()) {
    return false;
  }
  SyncActiveEditorTab();

  auto existing = std::find_if(open_tabs_.begin(), open_tabs_.end(), [&](const TabEntry& tab) {
    return tab.kind == TabEntry::Kind::Editor && tab.path == path;
  });

  directory_tree_.SelectPath(path);

  if (existing != open_tabs_.end()) {
    ActivateTab(static_cast<std::size_t>(std::distance(open_tabs_.begin(), existing)));
    return true;
  }

  editor::TextViewport opened_view;
  if (!opened_view.OpenFile(path)) {
    return false;
  }
  ApplyEditorPreferences(opened_view);
  text_viewport_ = opened_view;

  open_tabs_.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = path,
      .title = path.filename().string(),
      .editor_state = MakeEditorTabState(opened_view),
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  active_tab_index_ = open_tabs_.size() - 1;
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  return true;
}

bool WorkspaceShell::MoveActiveTabTo(std::size_t index) {
  if (active_tab_index_ >= open_tabs_.size() || index >= open_tabs_.size()) {
    return false;
  }

  if (active_tab_index_ == index) {
    return true;
  }

  SyncActiveEditorTab();

  TabEntry moved_tab = std::move(open_tabs_[active_tab_index_]);
  open_tabs_.erase(open_tabs_.begin() + static_cast<std::ptrdiff_t>(active_tab_index_));
  open_tabs_.insert(open_tabs_.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved_tab));

  active_tab_index_ = index;
  EnsureActiveTabVisible();
  focus_ = FocusTarget::Editor;
  return true;
}

std::optional<std::size_t> WorkspaceShell::FindTabIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  if (specifier.empty()) {
    if (error_message != nullptr) {
      *error_message = "usage: tabswitch <tab>";
    }
    return std::nullopt;
  }

  const std::string lowered_specifier = ToLower(specifier);
  try {
    std::size_t parsed_length = 0;
    const int tab_number = std::stoi(std::string(specifier), &parsed_length);
    if (parsed_length == specifier.size()) {
      if (tab_number >= 1 && static_cast<std::size_t>(tab_number) <= open_tabs_.size()) {
        return static_cast<std::size_t>(tab_number - 1);
      }
      if (error_message != nullptr) {
        *error_message = "Invalid tab index";
      }
      return std::nullopt;
    }
  } catch (...) {
  }

  std::vector<std::size_t> exact_matches;
  std::vector<std::size_t> partial_matches;
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    const TabEntry& tab = open_tabs_[i];
    const std::string lowered_title = ToLower(tab.title);
    const std::string lowered_path = ToLower(RelativePathLabel(project_root_, tab.path));
    const std::string lowered_absolute_path = ToLower(tab.path.lexically_normal().string());
    const bool exact_match = lowered_title == lowered_specifier ||
                             (!lowered_path.empty() && lowered_path == lowered_specifier) ||
                             (!lowered_absolute_path.empty() &&
                              lowered_absolute_path == lowered_specifier);
    const bool partial_match = lowered_title.find(lowered_specifier) != std::string::npos ||
                               (!lowered_path.empty() &&
                                lowered_path.find(lowered_specifier) != std::string::npos) ||
                               (!lowered_absolute_path.empty() &&
                                lowered_absolute_path.find(lowered_specifier) != std::string::npos);
    if (exact_match) {
      exact_matches.push_back(i);
    } else if (partial_match) {
      partial_matches.push_back(i);
    }
  }

  if (exact_matches.size() == 1) {
    return exact_matches.front();
  }
  if (exact_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (partial_matches.size() == 1) {
    return partial_matches.front();
  }
  if (partial_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (error_message != nullptr) {
    *error_message = "Unknown tab: " + std::string(specifier);
  }
  return std::nullopt;
}

void WorkspaceShell::OpenFile(const std::filesystem::path& path) {
  if (!OpenFileInNewTab(path)) {
    LogMessage("Failed to open file: " + path.lexically_normal().string());
    return;
  }
  LogMessage("Opened file: " + path.lexically_normal().string());
}

WorkspaceShell::TextInputSurface WorkspaceShell::CurrentTextInputSurface() const {
  if (dirty_prompt_visible_) {
    return TextInputSurface::None;
  }

  if (prompt_surface_visible_) {
    return prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput
               ? TextInputSurface::PromptInput
               : TextInputSurface::None;
  }

  if (menu_bar_open_ || tree_context_menu_.open) {
    return TextInputSurface::None;
  }

  if (command_mode_) {
    return TextInputSurface::Command;
  }

  if (overlay_visible_) {
    switch (overlay_mode_) {
      case OverlayMode::BufferSearch:
        return TextInputSurface::BufferSearch;
      case OverlayMode::BufferReplace:
        return buffer_search_field_ == BufferSearchField::Search
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

  if (focus_ == FocusTarget::Sidebar && sidebar_visible_ && sidebar_mode_ == SidebarMode::Search &&
      project_search_editing_) {
    return project_search_edit_field_ == ProjectSearchEditField::Query
               ? TextInputSurface::SidebarSearchQuery
               : TextInputSurface::SidebarSearchReplace;
  }

  if (focus_ == FocusTarget::Editor && !ActiveTabIsCompare() && !ActiveTabIsMerge()) {
    return TextInputSurface::Editor;
  }

  if (focus_ == FocusTarget::Panel && BottomPanelShowsTerminal()) {
    return TextInputSurface::Terminal;
  }

  return TextInputSurface::None;
}

bool WorkspaceShell::ReopenActiveTab() {
  if (active_tab_index_ >= open_tabs_.size()) {
    LogMessage("No editor tab is active");
    return false;
  }

  auto& tab = open_tabs_[active_tab_index_];
  if (tab.kind != TabEntry::Kind::Editor) {
    LogMessage("Reopen only works for editor tabs");
    return false;
  }
  const std::filesystem::path reopen_path = text_viewport_.path().empty()
                                                ? tab.path.lexically_normal()
                                                : text_viewport_.path().lexically_normal();
  if (reopen_path.empty()) {
    LogMessage("No file is open");
    return false;
  }
  if (text_viewport_.dirty()) {
    LogMessage("Reopen blocked by unsaved changes");
    return false;
  }

  editor::TextViewport reopened_view;
  if (!reopened_view.OpenFile(reopen_path)) {
    LogMessage("Failed to reopen file: " + reopen_path.string());
    return false;
  }
  ApplyEditorPreferences(reopened_view);

  if (tab.editor_state.has_value() && !tab.editor_state->views.empty()) {
    NormalizeEditorSplitTree(*tab.editor_state);
    for (auto& view : tab.editor_state->views) {
      if (view.leaf_id == tab.editor_state->active_leaf_id ||
          EditorViewPath(view) == reopen_path) {
        view.viewport = reopened_view;
        view.restored_path = reopen_path;
        view.restored_cursor_line = reopened_view.cursor_line();
        view.restored_cursor_column = reopened_view.cursor_column();
        view.restored_scroll_line = reopened_view.scroll_line();
        view.restored_horizontal_scroll = reopened_view.horizontal_scroll();
        view.needs_restore = false;
      }
    }
    text_viewport_ = reopened_view;
  } else {
    text_viewport_ = reopened_view;
    tab.editor_state = MakeEditorTabState(reopened_view);
  }
  SyncActiveEditorTabMetadata();
  focus_ = FocusTarget::Editor;
  ResetCaretBlink();
  LogMessage("Reopened file from disk: " + reopen_path.string());
  return true;
}

bool WorkspaceShell::ExecuteAction(ActionId id,
                                   const std::vector<std::string>& args,
                                   ActionSource source) {
  if (source != ActionSource::ContextMenu) {
    CloseTreeContextMenu();
  }

  const auto require_project = [&]() {
    if (!project_root_.empty()) {
      return false;
    }
    LogMessage("No project is loaded");
    return true;
  };

  switch (id) {
    case ActionId::Help:
      if (source == ActionSource::Menu) {
        bottom_panel_mode_ = BottomPanelMode::Logs;
        SetBottomPanelVisible(true);
      }
      LogMessage("Commands: " + CommandHelpSummary());
      return true;
    case ActionId::Colorscheme:
      if (args.empty()) {
        LogMessage("Colorscheme: " + active_colorscheme_name_);
        return true;
      }
      if (args[0] == "list") {
        RefreshAvailableColorschemeNames();
        if (available_colorscheme_names_.empty()) {
          LogMessage("No bundled colorschemes found");
        } else {
          LogMessage("Colorschemes: " + JoinCommandArguments(available_colorscheme_names_, 0));
        }
        return true;
      }
      RefreshAvailableColorschemeNames();
      ApplyColorscheme(args[0], true, true);
      return true;
    case ActionId::ProjectOpen:
      if (args.empty()) {
        if (source == ActionSource::Menu) {
          command_mode_ = true;
          SetBottomPanelVisible(true);
          command_input_ = "project-open ";
          ResetCommandSessionState();
          LogMessage("Enter a project path");
          return true;
        }
        LogMessage("usage: project-open <path>");
        return true;
      }
      OpenProjectTab(std::filesystem::path(args[0]), true, true);
      return true;
    case ActionId::ProjectClose:
      if (projects_.empty() || project_root_.empty()) {
        LogMessage("No project is loaded");
        return true;
      }
      RequestCloseProject(active_project_index_);
      return true;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev: {
      if (projects_.empty() || project_root_.empty()) {
        LogMessage("No project is loaded");
        return true;
      }
      if (projects_.size() == 1) {
        LogMessage("Only one project is open");
        return true;
      }
      const int delta = id == ActionId::ProjectNext ? 1 : -1;
      const int project_count = static_cast<int>(projects_.size());
      const int next_index =
          (static_cast<int>(active_project_index_) + delta + project_count) % project_count;
      SwitchProject(static_cast<std::size_t>(next_index), true);
      return true;
    }
    case ActionId::SidebarToggle: {
      const std::string tool = args.empty() ? std::string{} : args[0];
      if (tool == "git") {
        if (sidebar_visible_ && sidebar_mode_ == SidebarMode::Git) {
          CloseSidebar();
        } else {
          ShowGitSidebar();
        }
        return true;
      }
      if (tool == "tree") {
        if (sidebar_visible_ && sidebar_mode_ == SidebarMode::Tree) {
          CloseSidebar();
        } else {
          const std::filesystem::path root_arg =
              args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::path{};
          ShowTreeSidebar(root_arg);
        }
        return true;
      }
      if (tool == "search") {
        const std::string query = JoinCommandArguments(args, 1);
        if (sidebar_visible_ && sidebar_mode_ == SidebarMode::Search && !sidebar_temporary_) {
          CloseSidebar();
        } else {
          ShowSearchSidebar(query, false);
        }
        return true;
      }
      ToggleSidebar();
      return true;
    }
    case ActionId::SidebarShow: {
      const std::string tool = args.empty() ? std::string{} : args[0];
      if (tool == "git") {
        ShowGitSidebar();
        return true;
      }
      if (tool == "tree") {
        const std::filesystem::path root_arg =
            args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::path{};
        ShowTreeSidebar(root_arg);
        return true;
      }
      if (tool == "search") {
        ShowSearchSidebar(JoinCommandArguments(args, 1), false);
        return true;
      }
      sidebar_visible_ = true;
      focus_ = FocusTarget::Sidebar;
      LogMessage("Sidebar shown");
      return true;
    }
    case ActionId::SidebarHide:
    case ActionId::SidebarClose:
      CloseSidebar();
      return true;
    case ActionId::SidebarWidth:
      if (args.empty()) {
        LogMessage("usage: sidebar-width <n>");
        return true;
      }
      try {
        const float width = std::stof(args[0]);
        sidebar_width_ =
            ClampSidebarWidth(width, static_cast<float>(std::max(1, last_window_width_)));
        LogMessage("Sidebar width updated");
      } catch (...) {
        LogMessage("Invalid sidebar width");
      }
      return true;
    case ActionId::TabSize:
      if (args.empty()) {
        LogMessage("Tab size: " + std::to_string(editor_preferences_.tab_size));
        return true;
      }
      try {
        editor_preferences_.tab_size =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(args[0])), 1, 16);
        ApplyEditorPreferencesToAllTabs();
        SaveConfigState();
        LogMessage("Tab size set to " + std::to_string(editor_preferences_.tab_size));
      } catch (...) {
        LogMessage("Invalid tab size");
      }
      return true;
    case ActionId::IndentWidth:
      if (args.empty()) {
        LogMessage("Indent width: " + std::to_string(editor_preferences_.indent_width));
        return true;
      }
      try {
        editor_preferences_.indent_width =
            std::clamp<std::size_t>(static_cast<std::size_t>(std::stoull(args[0])), 1, 16);
        ApplyEditorPreferencesToAllTabs();
        SaveConfigState();
        LogMessage("Indent width set to " + std::to_string(editor_preferences_.indent_width));
      } catch (...) {
        LogMessage("Invalid indent width");
      }
      return true;
    case ActionId::UiScale:
      if (args.empty()) {
        LogMessage("UI scale: " + UiScaleLabel(ui_scale_));
        return true;
      }
      if (args[0] == "up") {
        ApplyUiScale(StepUiScale(ui_scale_, 1), true, true);
        return true;
      }
      if (args[0] == "down") {
        ApplyUiScale(StepUiScale(ui_scale_, -1), true, true);
        return true;
      }
      if (args[0] == "reset") {
        ApplyUiScale(1.0f, true, true);
        return true;
      }
      if (const auto scale = ParseUiScaleValue(args[0]); scale.has_value()) {
        ApplyUiScale(*scale, true, true);
      } else {
        LogMessage("usage: ui-scale <n|up|down|reset>");
      }
      return true;
    case ActionId::SoftTabs:
      if (args.empty()) {
        LogMessage(std::string("Soft tabs: ") + (editor_preferences_.soft_tabs ? "on" : "off"));
        return true;
      }
      if (const std::string value = ToLower(args[0]);
          value != "on" && value != "off" && value != "true" && value != "false" &&
          value != "1" && value != "0") {
        LogMessage("usage: soft-tabs <on|off>");
        return true;
      } else {
        editor_preferences_.soft_tabs =
            value == "on" || value == "true" || value == "1";
      }
      ApplyEditorPreferencesToAllTabs();
      SaveConfigState();
      LogMessage(std::string("Soft tabs ") + (editor_preferences_.soft_tabs ? "enabled"
                                                                            : "disabled"));
      return true;
    case ActionId::PanelShow:
      SetBottomPanelVisible(true);
      LogMessage("Bottom panel shown");
      return true;
    case ActionId::PanelHide:
      SetBottomPanelVisible(false);
      LogMessage("Bottom panel hidden");
      return true;
    case ActionId::ToggleBottomPanel:
      SetBottomPanelVisible(!bottom_panel_visible_);
      LogMessage(std::string("Bottom panel ") + (bottom_panel_visible_ ? "shown" : "hidden"));
      return true;
    case ActionId::TreeRefresh:
      if (require_project()) {
        return true;
      }
      RefreshProjectFiles();
      LogMessage("Project tree refreshed");
      return true;
    case ActionId::CreateFile:
    case ActionId::CreateDirectory: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path base_path = TreeMutationBasePath(source);
      if (base_path.empty()) {
        LogMessage("No tree directory is selected");
        return true;
      }
      OpenPromptSurface(id == ActionId::CreateFile ? PromptSurfaceState::Action::CreateFile
                                                   : PromptSurfaceState::Action::CreateDirectory,
                        PromptSurfaceState::Kind::TextInput, base_path);
      return true;
    }
    case ActionId::RenamePath: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree path is selected");
        return true;
      }
      OpenPromptSurface(PromptSurfaceState::Action::RenamePath,
                        PromptSurfaceState::Kind::TextInput, path, path.filename().string());
      return true;
    }
    case ActionId::DeletePath: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree path is selected");
        return true;
      }
      OpenPromptSurface(PromptSurfaceState::Action::DeletePath,
                        PromptSurfaceState::Kind::Confirm, path);
      return true;
    }
    case ActionId::CopyRelativePath:
    case ActionId::CopyAbsolutePath: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree path is selected");
        return true;
      }

      std::string clipboard_text;
      if (id == ActionId::CopyRelativePath) {
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(path, project_root_, error);
        if (error || relative.empty()) {
          LogMessage("Failed to compute relative path");
          return true;
        }
        clipboard_text = relative.generic_string();
      } else {
        clipboard_text = path.lexically_normal().string();
      }

      if (SDL_SetClipboardText(clipboard_text.c_str())) {
        LogMessage(std::string(id == ActionId::CopyRelativePath ? "Relative" : "Absolute") +
                   " path copied");
      } else {
        LogMessage("Failed to copy path");
      }
      return true;
    }
    case ActionId::Focus: {
      const std::string target = args.empty() ? std::string{} : args[0];
      if (target == "sidebar" && sidebar_visible_) {
        focus_ = FocusTarget::Sidebar;
        LogMessage("Focus moved to sidebar");
        return true;
      }
      if (target == "editor") {
        focus_ = FocusTarget::Editor;
        LogMessage("Focus moved to editor");
        return true;
      }
      if (target == "panel" && bottom_panel_visible_ && BottomPanelShowsTerminal()) {
        focus_ = FocusTarget::Panel;
        LogMessage("Focus moved to terminal panel");
        return true;
      }
      LogMessage("Unknown focus target");
      return true;
    }
    case ActionId::Term:
      if (require_project()) {
        return true;
      }
      OpenTerminal(JoinCommandArguments(args, 0));
      return true;
    case ActionId::Find:
      if (require_project()) {
        return true;
      }
      file_index_.Refresh();
      file_finder_.SetIndex(&file_index_);
      file_finder_.SetQuery(JoinCommandArguments(args, 0));
      overlay_visible_ = true;
      overlay_mode_ = OverlayMode::FileFinder;
      focus_ = FocusTarget::Overlay;
      ResetOverlayScroll();
      LogMessage("Finder opened from command");
      return true;
    case ActionId::Files: {
      const std::string root_arg = args.empty() ? std::string{} : args[0];
      if (!root_arg.empty() && !OpenProjectTab(root_arg, true, true)) {
        return true;
      }
      if (source == ActionSource::Shortcut && overlay_visible_) {
        overlay_visible_ = false;
        focus_ = sidebar_visible_ ? FocusTarget::Sidebar : FocusTarget::Editor;
        LogMessage("Finder overlay closed");
        return true;
      }
      if (source != ActionSource::Shortcut && require_project()) {
        return true;
      }
      overlay_visible_ = true;
      overlay_mode_ = OverlayMode::FileFinder;
      file_index_.Refresh();
      file_finder_.SetIndex(&file_index_);
      file_finder_.SetQuery("");
      focus_ = FocusTarget::Overlay;
      ResetOverlayScroll();
      LogMessage(source == ActionSource::Shortcut ? "Finder overlay opened" : "Finder opened");
      return true;
    }
    case ActionId::Tree: {
      const std::filesystem::path root_arg =
          args.empty() ? std::filesystem::path{} : std::filesystem::path(args[0]);
      if (root_arg.empty() && require_project()) {
        return true;
      }
      ShowTreeSidebar(root_arg);
      return true;
    }
    case ActionId::Grep:
      if (require_project()) {
        return true;
      }
      ShowSearchSidebar(JoinCommandArguments(args, 0), false);
      return true;
    case ActionId::Rg:
      if (require_project()) {
        return true;
      }
      ShowSearchSidebar(JoinCommandArguments(args, 0), true);
      return true;
    case ActionId::Search:
      if (require_project()) {
        return true;
      }
      if (ActiveTabIsCompare() || ActiveTabIsMerge()) {
        LogMessage("search only works in editor tabs");
        return true;
      }
      OpenBufferSearch();
      buffer_search_query_ = JoinCommandArguments(args, 0);
      RefreshBufferSearch();
      LogMessage("Buffer search opened");
      return true;
    case ActionId::ReplaceInBuffer:
      OpenBufferReplace();
      return true;
    case ActionId::Open:
      if (require_project()) {
        return true;
      }
      if (args.empty()) {
        LogMessage("usage: open <path>");
        return true;
      }
      {
        std::filesystem::path path = args[0];
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();

        auto* editor_tab = ActiveEditorTab();
        if (editor_tab != nullptr && editor_tab->views.size() > 1) {
          editor::TextViewport opened_view;
          if (!opened_view.OpenFile(path)) {
            LogMessage("Failed to open file: " + path.string());
            return true;
          }
          if (!ReplaceActiveEditorView(opened_view)) {
            LogMessage("Failed to open file in active split: " + path.string());
            return true;
          }
          LogMessage("Opened file in active split: " + path.string());
          return true;
        }

        OpenFile(path);
        return true;
      }
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree file is selected");
        return true;
      }
      if (id == ActionId::OpenSelectedTreeItemInNewTab) {
        if (!OpenFileInNewTab(path)) {
          LogMessage("Failed to open file: " + path.string());
          return true;
        }
        LogMessage("Opened tab: " + open_tabs_[active_tab_index_].title);
      } else {
        OpenFile(path);
      }
      return true;
    }
    case ActionId::Compare: {
      if (require_project()) {
        return true;
      }
      std::filesystem::path path;
      if (!args.empty()) {
        path = std::filesystem::path(args[0]);
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();
      } else if (source == ActionSource::ContextMenu) {
        path = ResolveTreeActionPath(source);
      } else if (!text_viewport_.path().empty()) {
        path = text_viewport_.path().lexically_normal();
      } else if (sidebar_visible_ && sidebar_mode_ == SidebarMode::Tree) {
        const auto& entries = directory_tree_.entries();
        if (directory_tree_.selected_index() < entries.size() &&
            !entries[directory_tree_.selected_index()].is_directory) {
          path = entries[directory_tree_.selected_index()].path.lexically_normal();
        }
      }

      if (path.empty()) {
        LogMessage("usage: compare [path] [commit-prefix]");
        return true;
      }

      if (!std::filesystem::exists(path)) {
        LogMessage("File does not exist: " + path.string());
        return true;
      }

      const std::string commit_spec = args.size() > 1 ? args[1] : "";
      OpenComparePickerForPath(path, commit_spec);
      return true;
    }
    case ActionId::CompareHead: {
      if (require_project()) {
        return true;
      }
      const std::filesystem::path path = ResolveTreeActionPath(source);
      if (path.empty()) {
        LogMessage("No tree file is selected");
        return true;
      }
      if (!std::filesystem::exists(path)) {
        LogMessage("File does not exist: " + path.string());
        return true;
      }
      compare_picker_path_ = path.lexically_normal();
      OpenComparison(project::GitCommitEntry{
          .hash = "HEAD",
          .short_hash = "HEAD",
          .subject = "HEAD",
      });
      return true;
    }
    case ActionId::Merge: {
      if (require_project()) {
        return true;
      }
      if (args.size() < 3 || args.size() > 4) {
        LogMessage("usage: merge <base> <incoming> <current> [output]");
        return true;
      }

      auto resolve_path = [&](const std::string& text) {
        std::filesystem::path path = text;
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        return path.lexically_normal();
      };

      const std::filesystem::path base_path = resolve_path(args[0]);
      const std::filesystem::path incoming_path = resolve_path(args[1]);
      const std::filesystem::path current_path = resolve_path(args[2]);
      const std::filesystem::path output_path =
          args.size() > 3 ? resolve_path(args[3]) : current_path;
      if (!std::filesystem::exists(base_path) || !std::filesystem::exists(incoming_path) ||
          !std::filesystem::exists(current_path)) {
        LogMessage("merge expects existing base, incoming, and current files");
        return true;
      }

      OpenMergeEditor(base_path, incoming_path, current_path, output_path);
      return true;
    }
    case ActionId::Tab:
      if (require_project()) {
        return true;
      }
      if (args.empty()) {
        OpenUntitledTab();
        LogMessage("Opened untitled tab");
        return true;
      }

      for (const std::string& arg : args) {
        std::filesystem::path path = arg;
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();

        if (!OpenFileInNewTab(path)) {
          LogMessage("Failed to open file: " + path.string());
          return true;
        }
      }

      LogMessage("Opened tab: " + open_tabs_[active_tab_index_].title);
      return true;
    case ActionId::TabSwitch: {
      if (require_project()) {
        return true;
      }
      std::string error_message;
      const std::optional<std::size_t> tab_index =
          FindTabIndexBySpecifier(JoinCommandArguments(args, 0), &error_message);
      if (!tab_index.has_value()) {
        LogMessage(error_message);
        return true;
      }
      ActivateTab(*tab_index);
      LogMessage("Switched to tab: " + open_tabs_[*tab_index].title);
      return true;
    }
    case ActionId::TabMove:
      if (require_project()) {
        return true;
      }
      if (args.empty()) {
        LogMessage("usage: tabmove <n>");
        return true;
      }
      if (open_tabs_.empty()) {
        LogMessage("No tabs are open");
        return true;
      }
      {
        std::size_t parsed_length = 0;
        int slot = 0;
        try {
          slot = std::stoi(args[0], &parsed_length);
        } catch (...) {
          LogMessage("Invalid tab slot");
          return true;
        }
        if (parsed_length != args[0].size()) {
          LogMessage("Invalid tab slot");
          return true;
        }

        const bool relative = !args[0].empty() && (args[0].front() == '+' || args[0].front() == '-');
        const int current_slot = static_cast<int>(active_tab_index_) + 1;
        const int requested_slot = relative ? current_slot + slot : slot;
        const int clamped_slot =
            std::clamp(requested_slot, 1, static_cast<int>(open_tabs_.size()));
        MoveActiveTabTo(static_cast<std::size_t>(clamped_slot - 1));
        LogMessage("Moved tab to slot " + std::to_string(clamped_slot));
        return true;
      }
    case ActionId::Reopen:
      if (require_project()) {
        return true;
      }
      ReopenActiveTab();
      return true;
    case ActionId::Save:
      if (require_project()) {
        return true;
      }
      if (SaveTab(active_tab_index_)) {
        if (source == ActionSource::Shortcut) {
          ResetCaretBlink();
        }
        const std::filesystem::path saved_path =
            ActiveTabIsMerge() && ActiveMergeTab() != nullptr
                ? ActiveMergeTab()->output_path.lexically_normal()
                : text_viewport_.path().lexically_normal();
        LogMessage("Saved file: " + saved_path.string());
      } else {
        LogMessage("Save failed");
      }
      return true;
    case ActionId::Vsplit:
    case ActionId::Hsplit: {
      if (require_project()) {
        return true;
      }
      const EditorSplitOrientation orientation =
          id == ActionId::Vsplit ? EditorSplitOrientation::Vertical
                                 : EditorSplitOrientation::Horizontal;
      const std::string command = id == ActionId::Vsplit ? "vsplit" : "hsplit";
      const std::string split_label = id == ActionId::Vsplit ? "Vertical" : "Horizontal";

      if (args.empty()) {
        if (!SplitActiveEditor(orientation)) {
          LogMessage(command + " only works in editor tabs");
        } else {
          LogMessage(split_label + " split opened");
        }
        return true;
      }

      for (const std::string& arg : args) {
        std::filesystem::path path = arg;
        if (path.is_relative()) {
          path = project_root_ / path;
        }
        path = path.lexically_normal();

        editor::TextViewport opened_view;
        if (!opened_view.OpenFile(path)) {
          LogMessage("Failed to open file: " + path.string());
          return true;
        }
        if (!SplitActiveEditor(orientation)) {
          LogMessage(command + " only works in editor tabs");
          return true;
        }
        if (!ReplaceActiveEditorView(opened_view)) {
          LogMessage("Failed to load file into split: " + path.string());
          return true;
        }
      }

      LogMessage(split_label + " split opened");
      return true;
    }
    case ActionId::Unsplit:
      if (!UnsplitActiveEditor()) {
        LogMessage("No editor split is active");
      } else {
        LogMessage("Editor split closed");
      }
      return true;
    case ActionId::SplitNext:
      if (!CycleEditorSplit(1)) {
        LogMessage("No other split is available");
      } else {
        LogMessage("Focus moved to the next split");
      }
      return true;
    case ActionId::SplitPrev:
      if (!CycleEditorSplit(-1)) {
        LogMessage("No other split is available");
      } else {
        LogMessage("Focus moved to the previous split");
      }
      return true;
    case ActionId::SplitFirst:
      if (!ActivateOrderedEditorSplit(0)) {
        LogMessage("No other split is available");
      } else {
        LogMessage("Focus moved to the first split");
      }
      return true;
    case ActionId::SplitLast: {
      auto* editor_tab = ActiveEditorTab();
      const std::size_t last_index =
          editor_tab == nullptr || editor_tab->views.empty() ? 0 : editor_tab->views.size() - 1;
      if (!ActivateOrderedEditorSplit(last_index)) {
        LogMessage("No other split is available");
      } else {
        LogMessage("Focus moved to the last split");
      }
      return true;
    }
    case ActionId::Quit:
      RequestQuit();
      return true;
    case ActionId::Goto:
    case ActionId::Jump: {
      const std::string command = id == ActionId::Goto ? "goto" : "jump";
      if (ActiveTabIsCompare() || ActiveTabIsMerge()) {
        LogMessage(command + " only works in editor tabs");
        return true;
      }
      if (args.empty()) {
        LogMessage("usage: " + command + " <line[:col]>");
        return true;
      }

      long long requested_line = 0;
      std::size_t column = 0;
      if (!ParseLineColumnSpec(args[0], &requested_line, &column, id == ActionId::Jump)) {
        LogMessage("Invalid " + command + " target");
        return true;
      }

      if (id == ActionId::Goto && requested_line == 0) {
        LogMessage("goto expects 1-based positions");
        return true;
      }

      const std::size_t line_count = std::max<std::size_t>(1, text_viewport_.line_count());
      std::size_t line = 0;
      if (id == ActionId::Jump) {
        const long long current_line = static_cast<long long>(text_viewport_.cursor_line()) + 1;
        const long long target_line = current_line + requested_line;
        line = static_cast<std::size_t>(
            std::clamp(target_line - 1, 0LL, static_cast<long long>(line_count - 1)));
      } else if (requested_line > 0) {
        line = static_cast<std::size_t>(requested_line - 1);
      } else {
        const std::size_t from_end = static_cast<std::size_t>(-requested_line);
        line = from_end >= line_count ? 0 : line_count - from_end;
      }

      text_viewport_.MoveCursorTo(line, column > 0 ? column - 1 : 0);
      focus_ = FocusTarget::Editor;
      LogMessage("Cursor moved to requested location");
      return true;
    }
    case ActionId::CloseActiveTab:
      if (!open_tabs_.empty()) {
        RequestCloseTab(active_tab_index_);
      }
      return true;
    case ActionId::OpenCommandPrompt:
      command_mode_ = true;
      SetBottomPanelVisible(true);
      command_input_.clear();
      ResetCommandSessionState();
      LogMessage("Command mode opened");
      return true;
    case ActionId::SelectAll:
      text_viewport_.SelectAll();
      ResetCaretBlink();
      focus_ = FocusTarget::Editor;
      return true;
    case ActionId::Undo:
      if (text_viewport_.Undo()) {
        LogMessage("Undo");
        ResetCaretBlink();
      }
      return true;
    case ActionId::Redo:
      if (text_viewport_.Redo()) {
        LogMessage("Redo");
        ResetCaretBlink();
      }
      return true;
    case ActionId::CopySelection: {
      const std::string text = text_viewport_.SelectedText();
      if (!text.empty() && SDL_SetClipboardText(text.c_str())) {
        LogMessage("Selection copied");
      }
      return true;
    }
    case ActionId::CutSelection: {
      const std::string text = text_viewport_.SelectedText();
      if (!text.empty() && SDL_SetClipboardText(text.c_str())) {
        text_viewport_.DeleteSelectedText();
        ResetCaretBlink();
        LogMessage("Selection cut");
      }
      return true;
    }
    case ActionId::PasteClipboard: {
      char* clipboard_text = SDL_GetClipboardText();
      if (clipboard_text != nullptr) {
        text_viewport_.InsertText(clipboard_text);
        ResetCaretBlink();
        SDL_free(clipboard_text);
        LogMessage("Clipboard pasted");
      }
      return true;
    }
  }

  return true;
}

std::string WorkspaceShell::BreadcrumbLabel() const {
  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return "compare";
    }
    return RelativePathLabel(project_root_, compare_tab->path) + "  |  " + compare_tab->left_label +
           " -> " + compare_tab->right_label;
  }
  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return "merge";
    }
    return RelativePathLabel(project_root_, merge_tab->output_path) + "  |  " +
           merge_tab->incoming_label + " -> " + merge_tab->current_label;
  }
  if (text_viewport_.path().empty()) {
    return text_viewport_.is_placeholder() ? "welcome" : "untitled";
  }
  return RelativePathLabel(project_root_, text_viewport_.path());
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
  if (index >= projects_.size()) {
    return {};
  }
  const std::filesystem::path root =
      (!project_root_.empty() && index == active_project_index_) ? project_root_
      : projects_[index] != nullptr                               ? projects_[index]->root
                                                                 : std::filesystem::path{};
  const std::string label = ProjectLabelForRoot(root);
  return DirtyEditorTabIndicesForProject(index).empty() ? label : "*" + label;
}

WorkspaceShell::CursorKind WorkspaceShell::CursorKindForPosition(float x, float y) const {
  switch (drag_target_) {
    case DragTarget::SidebarDivider:
      return CursorKind::EwResize;
    case DragTarget::BottomPanelDivider:
      return CursorKind::NsResize;
    case DragTarget::EditorSplitDivider: {
      const auto* editor_tab = ActiveEditorTab();
      const auto* split_node = editor_tab != nullptr
                                   ? FindEditorSplitNode(editor_tab->split_root.get(),
                                                         drag_editor_split_path_)
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

  if (dirty_prompt_visible_) {
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

  if (prompt_surface_visible_) {
    const SDL_FRect full = MakeRect(0.0f, 0.0f, static_cast<float>(last_window_width_),
                                    static_cast<float>(last_window_height_));
    const SDL_FRect dialog = ComputePromptSurfaceRect(full);
    for (const SDL_FRect& button : ComputePromptSurfaceButtonRects(dialog)) {
      if (Contains(button, x, y)) {
        return CursorKind::Pointer;
      }
    }
    if (prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput &&
        Contains(ComputePromptSurfaceInputRect(dialog), x, y)) {
      return CursorKind::Text;
    }
    return CursorKind::Default;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, bottom_panel_visible_, sidebar_width_, bottom_panel_height_);

  if (tree_context_menu_.open) {
    if (const auto popup_rect = ComputeTreeContextMenuRect();
        popup_rect.has_value() && Contains(*popup_rect, x, y)) {
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(TreeContextMenuItems(tree_context_menu_.target),
                                        tree_context_menu_.active_item_index, *popup_rect)) {
        if (Contains(item.rect, x, y)) {
          return item.separator ? CursorKind::Default : CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
  }

  if (menu_bar_open_) {
    if (const auto popup_rect = ActiveSubmenuRect(layout.menu_bar);
        popup_rect.has_value() && Contains(*popup_rect, x, y)) {
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(active_submenu_id_, *popup_rect)) {
        if (Contains(item.rect, x, y)) {
          return item.separator ? CursorKind::Default : CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
    if (const auto popup_rect = ComputePopupMenuRect(layout.menu_bar, active_menu_id_);
        popup_rect.has_value() && Contains(*popup_rect, x, y)) {
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(active_menu_id_, *popup_rect)) {
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

  if (overlay_visible_) {
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
    if (!Contains(overlay, x, y)) {
      return CursorKind::Default;
    }

    const float overlay_list_y = overlay.y + OverlayListStartOffset();
    const int overlay_visible_rows = OverlayVisibleRows(overlay);
    const int overlay_max_scroll =
        std::max(0, static_cast<int>(OverlayItemCount()) - overlay_visible_rows);
    const auto overlay_scrollbar = MakeVerticalScrollbarGeometry(
        MakeRect(overlay.x, overlay_list_y, overlay.w,
                 std::max(0.0f, overlay.y + overlay.h - overlay_list_y - 8.0f)),
        static_cast<float>(OverlayItemCount()), static_cast<float>(overlay_visible_rows),
        static_cast<float>(std::clamp(overlay_scroll_row_, 0, overlay_max_scroll)));
    if (overlay_scrollbar.has_value() && Contains(overlay_scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    if (y >= overlay_list_y && y < overlay.y + overlay.h) {
      return CursorKind::Pointer;
    }

    if (overlay_mode_ == OverlayMode::BufferReplace) {
      return y >= overlay.y + 40.0f && y < overlay.y + 82.0f ? CursorKind::Text
                                                              : CursorKind::Default;
    }
    if (overlay_mode_ == OverlayMode::CommitPicker) {
      return y >= overlay.y + 58.0f && y < overlay.y + 78.0f ? CursorKind::Text
                                                              : CursorKind::Default;
    }
    return y >= overlay.y + 40.0f && y < overlay.y + 60.0f ? CursorKind::Text
                                                            : CursorKind::Default;
  }

  if (sidebar_visible_ && Contains(SidebarResizeHandleRect(layout), x, y)) {
    return CursorKind::EwResize;
  }
  if (bottom_panel_visible_ && Contains(BottomPanelResizeHandleRect(layout), x, y)) {
    return CursorKind::NsResize;
  }

  if (Contains(layout.project_tab_strip, x, y)) {
    for (const VisibleProjectTab& tab : ComputeVisibleProjectTabs(layout.project_tab_strip)) {
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
      return Contains(MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 5.0f, 220.0f, 24.0f),
                      x, y)
                 ? CursorKind::Pointer
                 : CursorKind::Default;
    }
    for (const VisibleTab& tab : ComputeVisibleTabs(layout.tab_strip)) {
      if (Contains(tab.rect, x, y)) {
        return CursorKind::Pointer;
      }
    }
    return CursorKind::Default;
  }

  if (sidebar_visible_ && Contains(layout.sidebar, x, y)) {
    if (sidebar_mode_ == SidebarMode::Search) {
      if (y < layout.sidebar.y + 66.0f) {
        return CursorKind::Text;
      }

      const auto line_map = BuildProjectSearchLineMap();
      const int visible_rows = std::max(
          1, static_cast<int>((layout.sidebar.h - kSearchSidebarResultsTop) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      const float list_y = layout.sidebar.y + kSearchSidebarResultsTop;
      const int clicked_row = static_cast<int>((y - list_y) / kSidebarRowHeight);
      const int line_index = std::clamp(sidebar_scroll_row_, 0, max_scroll) + clicked_row;
      if (clicked_row >= 0 && line_index >= 0 && line_index < static_cast<int>(line_map.size()) &&
          line_map[static_cast<std::size_t>(line_index)] >= 0) {
        const SDL_FRect row_rect = MakeRect(
            layout.sidebar.x + kSidebarInset,
            list_y + static_cast<float>(clicked_row) * kSidebarRowHeight, row_width,
            kSidebarRowHeight - 2.0f);
        return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
      }
      return CursorKind::Default;
    }
    if (sidebar_mode_ == SidebarMode::Git) {
      const auto lines = BuildGitSidebarLines();
      const float list_y = layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(lines.size()) - visible_rows);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      const int clicked_row = static_cast<int>((y - list_y) / kSidebarRowHeight);
      const int line_index = std::clamp(sidebar_scroll_row_, 0, max_scroll) + clicked_row;
      if (clicked_row < 0 || line_index < 0 || line_index >= static_cast<int>(lines.size())) {
        return CursorKind::Default;
      }
      const SDL_FRect row_rect = MakeRect(layout.sidebar.x + kSidebarInset,
                                          list_y + static_cast<float>(clicked_row) * kSidebarRowHeight,
                                          row_width, kSidebarRowHeight - 2.0f);
      if (!Contains(row_rect, x, y)) {
        return CursorKind::Default;
      }
      const auto& line = lines[static_cast<std::size_t>(line_index)];
      if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
        return CursorKind::Default;
      }
      const auto& entry = git_sidebar_entries_[static_cast<std::size_t>(line.entry_index)];
      if (entry.section == GitSidebarEntry::Section::Modified) {
        float right_edge = row_rect.x + row_rect.w - 8.0f;
        if (!entry.staged) {
          const float stage_width = std::max(42.0f, text_renderer_.MeasureWidth("Stage") + 12.0f);
          const SDL_FRect stage_rect =
              MakeRect(right_edge - stage_width, row_rect.y + 1.0f, stage_width, row_rect.h - 2.0f);
          if (Contains(stage_rect, x, y)) {
            return CursorKind::Pointer;
          }
          right_edge = stage_rect.x - 6.0f;
        }
        const float discard_width = std::max(54.0f, text_renderer_.MeasureWidth("Discard") + 12.0f);
        const SDL_FRect discard_rect =
            MakeRect(right_edge - discard_width, row_rect.y + 1.0f, discard_width, row_rect.h - 2.0f);
        if (Contains(discard_rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      return CursorKind::Pointer;
    }

    const auto& entries = directory_tree_.entries();
    const float list_y = layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
    const int visible_rows =
        std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / kSidebarRowHeight));
    const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
    const float row_width =
        std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                           (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
    const int clicked_row = static_cast<int>((y - list_y) / kSidebarRowHeight);
    const int entry_index = std::clamp(sidebar_scroll_row_, 0, max_scroll) + clicked_row;
    if (clicked_row >= 0 && entry_index >= 0 && entry_index < static_cast<int>(entries.size())) {
      const SDL_FRect row_rect = MakeRect(layout.sidebar.x + kSidebarInset,
                                          list_y + static_cast<float>(clicked_row) * kSidebarRowHeight,
                                          row_width, kSidebarRowHeight - 2.0f);
      return Contains(row_rect, x, y) ? CursorKind::Pointer : CursorKind::Default;
    }
    return CursorKind::Default;
  }

  if (bottom_panel_visible_ && Contains(layout.bottom_panel, x, y)) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kBottomPanelHeaderHeight);
    if (BottomPanelShowsTerminal() && Contains(panel_header, x, y)) {
      if (Contains(BottomPanelTerminalNewTabRect(panel_header), x, y)) {
        return CursorKind::Pointer;
      }
      for (const VisibleTerminalTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
        if (Contains(tab.rect, x, y)) {
          return CursorKind::Pointer;
        }
      }
      return CursorKind::Default;
    }
    if (BottomPanelShowsTerminal()) {
      const std::size_t line_count =
          ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.LineCount() : 0;
      const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
      const int scroll_row = BottomPanelScrollRow(line_count, visible_rows);
      const auto scrollbar =
          MakeVerticalScrollbarGeometry(BottomPanelContentRect(layout, command_mode_),
                                        static_cast<float>(line_count),
                                        static_cast<float>(visible_rows),
                                        static_cast<float>(scroll_row));
      if (scrollbar.has_value() && Contains(scrollbar->track, x, y)) {
        return CursorKind::Default;
      }
    }
    if (command_mode_ && Contains(BottomPanelCommandPromptRect(layout), x, y)) {
      return CursorKind::Text;
    }
    if (BottomPanelShowsTerminal() && y >= layout.bottom_panel.y + kBottomPanelHeaderHeight) {
      return CursorKind::Text;
    }
    return CursorKind::Default;
  }

  if (!Contains(layout.editor_surface, x, y)) {
    return CursorKind::Default;
  }

  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return CursorKind::Default;
    }
    const int visible_rows = CompareVisibleRows(layout.editor_surface);
    const int scroll_row =
        std::clamp(compare_tab->scroll_row, 0, CompareMaxScrollRow(*compare_tab, visible_rows));
    const auto scrollbar = MakeVerticalScrollbarGeometry(
        layout.editor_surface, static_cast<float>(compare_tab->model.rows.size()),
        static_cast<float>(visible_rows), static_cast<float>(scroll_row));
    if (scrollbar.has_value() && Contains(scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
    return CursorKind::Pointer;
  }
  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return CursorKind::Default;
    }
    const int visible_rows = MergeVisibleRows(layout.editor_surface);
    const int scroll_row =
        std::clamp(merge_tab->scroll_row, 0, MergeMaxScrollRow(*merge_tab, visible_rows));
    const auto scrollbar = MakeVerticalScrollbarGeometry(
        layout.editor_surface, static_cast<float>(merge_tab->display_model.rows.size()),
        static_cast<float>(visible_rows), static_cast<float>(scroll_row));
    if (scrollbar.has_value() && Contains(scrollbar->track, x, y)) {
      return CursorKind::Default;
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
  const std::size_t total_columns =
      std::max<std::size_t>(metrics.visible_columns, MaxVisualColumns(*viewport));
  const bool show_vertical = viewport->line_count() > metrics.visible_rows;
  const bool show_horizontal = total_columns > metrics.visible_columns;
  if (show_vertical) {
    const auto scrollbar = MakeVerticalScrollbarGeometry(
        pane_it->rect, static_cast<float>(viewport->line_count()),
        static_cast<float>(metrics.visible_rows), static_cast<float>(viewport->scroll_line()),
        show_horizontal);
    if (scrollbar.has_value() && Contains(scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
  }
  if (show_horizontal) {
    const auto scrollbar = MakeHorizontalScrollbarGeometry(
        pane_it->rect, static_cast<float>(total_columns),
        static_cast<float>(metrics.visible_columns),
        static_cast<float>(viewport->horizontal_scroll()), show_vertical);
    if (scrollbar.has_value() && Contains(scrollbar->track, x, y)) {
      return CursorKind::Default;
    }
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
