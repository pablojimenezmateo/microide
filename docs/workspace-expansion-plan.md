# Workspace Expansion Plan

This document captures the reviewed-code plan for the next heavy shell features:

- project tabs above the existing file tabs
- a typical desktop menu bar with the currently available actions
- tree context menus with file and directory operations

This is a forward-looking design document. `docs/todo.md` remains the source of truth for what is and is not implemented today.

## Reviewed Code Anchors

- `src/workspace/WorkspaceShell.h`
- `src/workspace/WorkspaceShell.cpp`
- `src/project/DirectoryTree.h`
- `src/project/DirectoryTree.cpp`
- `src/project/FileIndex.h`
- `src/project/FileIndex.cpp`
- `src/project/ProjectSearchService.h`
- `src/project/ProjectSearchService.cpp`
- `src/project/GitCompareService.h`
- `src/terminal/TerminalSession.h`
- `src/terminal/TerminalSession.cpp`
- `docs/implementation-guide.md`
- `docs/todo.md`
- `docs/agent-handoff.md`

## Current Constraints From The Code

The current C++ shell is still built around one live project context inside `WorkspaceShell`.

That single context currently owns:

- one `project_root_`
- one `DirectoryTree`
- one `FileIndex`
- one `FileFinder`
- one `ProjectSearchService`
- one `open_tabs_` list and one `active_tab_index_`
- one sidebar mode and sidebar scroll state
- one bottom-panel mode, one terminal-tab set, and one log buffer
- one overlay state for finder, search, replace, and compare picker
- one project-local config path and one project-local session path

That shape was correct for the current single-project shell, but it is the main blocker for project tabs. Right now, changing project root mutates the only live workspace instead of switching between multiple workspaces.

The second architectural constraint is action dispatch. Keyboard shortcuts, the command prompt, mouse handling, and several tab/sidebar flows currently call behavior directly from `HandleEvent` and `ExecuteCommand`. Menus and context menus need shared action definitions, enablement rules, labels, and accelerators. The current direct-call structure does not provide that.

The third constraint is tree mutability. `DirectoryTree` currently supports read-only navigation plus compare entry. Rename, create, and delete need a separate file-operation layer because they affect:

- the filesystem
- open editor tabs
- compare tabs
- tree selection and expansion state
- cached file indexes and project-search results

## Goals

- Keep the app single-window and dense.
- Keep the tree simple and technical.
- Make project tabs switch the entire working context below them.
- Make everything currently visible below the future project-tab strip project-local, including tabs, tree state, search state, overlays, bottom-panel state, logs, command prompt state, and terminals.
- Make menus and context menus call the same underlying actions as shortcuts and commands.
- Avoid turning `WorkspaceShell` into a larger monolith than it already is.

## Proposed Architecture

### 1. Split shell-global state from project-scoped workspace state

Introduce a `ProjectWorkspaceState` inside the workspace layer and move all project-scoped members there.

Suggested shape:

```cpp
struct ProjectWorkspaceState {
  std::filesystem::path root;
  std::string label;

  project::DirectoryTree directory_tree;
  project::FileIndex file_index;
  project::FileFinder file_finder;

  std::vector<TabEntry> open_tabs;
  std::size_t active_tab_index = 0;
  int file_tab_scroll_index = 0;

  bool sidebar_visible = true;
  SidebarMode sidebar_mode = SidebarMode::Tree;
  SidebarMode sidebar_prev_mode = SidebarMode::None;
  bool sidebar_temporary = false;
  int sidebar_scroll_row = 0;
  float sidebar_width = 288.0f;

  bool bottom_panel_visible = false;
  BottomPanelMode bottom_panel_mode = BottomPanelMode::Logs;
  float bottom_panel_height = 184.0f;
  int bottom_panel_scroll_row = 0;
  bool bottom_panel_follow_tail = true;
  std::vector<std::unique_ptr<TerminalTabState>> terminal_tabs;
  std::size_t active_terminal_tab_index = 0;
  std::vector<std::string> log_messages;

  bool overlay_visible = false;
  OverlayMode overlay_mode = OverlayMode::FileFinder;
  int overlay_scroll_row = 0;

  bool command_mode = false;
  std::string command_input;
  std::vector<std::string> command_history;

  std::string project_search_query;
  std::string project_replace_text;
  std::vector<project::ProjectSearchResult> project_search_results;
  std::size_t project_search_selected_index = 0;
  bool project_search_running = false;
  std::string project_search_error;

  std::filesystem::path compare_picker_path;
  std::string compare_picker_query;
  std::vector<project::GitCommitEntry> compare_picker_commits;
  std::vector<project::GitCommitEntry> compare_picker_matches;
  std::size_t compare_picker_selected_index = 0;

  EditorPreferences editor_preferences;
  std::string active_colorscheme_name = "default";
};
```

Keep these shell-global:

- window size
- focus target
- drag state
- cursor state
- IME composition state
- dirty-close prompt state
- menu bar and popup menu state
- project-tab strip state

Keep one shared `ProjectSearchService` in `WorkspaceShell`, but bind it to the active project only. On project switch, stop the active run, keep the stored results/query in the outgoing project state, and restart only when the user explicitly reruns or edits the query.

### 2. Add an explicit project-tab model

Add a second tab layer:

- project tabs at the shell level
- file tabs inside the active project

Suggested shell-global members:

```cpp
std::vector<ProjectWorkspaceState> projects_;
std::size_t active_project_index_ = 0;
int project_tab_scroll_index_ = 0;
```

Behavior rules:

- switching project tabs swaps all state below the project-tab strip
- file tabs, compare tabs, split trees, tree expansion, search state, overlays, command prompt state, terminal tabs, log output, and sidebar/panel visibility are all project-local
- switching projects first syncs the outgoing active editor tab back into that project state
- project-tab labels should show the root name and a dirty marker if any editor tab in that project is dirty
- project tabs should support close buttons and middle-click close, like file tabs
- closing the last project tab should leave the app in an empty welcome window rather than quitting

### 3. Expand the top chrome into three layers

The default top chrome should become:

- row 1: menu bar
- row 2: project-tab strip
- row 3: file-tab strip

That requires `WorkspaceLayout` to stop treating the top area as a single `tab_strip`.

Suggested layout fields:

```cpp
struct WorkspaceLayout {
  SDL_FRect menu_bar;
  SDL_FRect project_tab_strip;
  SDL_FRect file_tab_strip;
  SDL_FRect sidebar;
  SDL_FRect editor_area;
  SDL_FRect breadcrumb;
  SDL_FRect editor_surface;
  SDL_FRect bottom_panel;
  SDL_FRect status_bar;
};
```

### 4. Introduce a shared action registry

Menus and context menus should not synthesize command strings and feed them back through `ExecuteCommand`.

Add an action registry that can be called from:

- keyboard shortcuts
- command prompt handlers
- menu items
- context-menu items
- future toolbar or palette entries

Suggested model:

```cpp
enum class ActionId {
  NewTab,
  OpenFile,
  Save,
  CloseTab,
  Reopen,
  ToggleSidebar,
  ShowTree,
  ShowProjectSearch,
  FindFile,
  FindInBuffer,
  ReplaceInBuffer,
  CompareCurrentFile,
  NewTerminal,
  RefreshTree,
  OpenProject,
  CloseProject,
  Quit,
  // ...
};

struct ActionSpec {
  ActionId id;
  std::string label;
  std::string accelerator;
  bool checkable = false;
};
```

The command prompt can remain string-based for user input, but it should become a parser plus dispatcher over the same lower-level action helpers used by menus and shortcuts.

## Menu Bar Plan

The menu bar should be a custom in-window SDL surface. Native OS menu integration is not a target for this project.

Recommended top-level menus:

### File

- New Tab
- Open File...
- Reopen
- Save
- Close Tab
- Quit

### Edit

- Undo
- Redo
- Cut
- Copy
- Paste
- Select All

### View

- Toggle Sidebar
- Show Tree
- Show Search
- Toggle Bottom Panel
- Focus Editor
- Focus Sidebar
- Focus Panel
- Colorscheme submenu

### Search

- Find in Buffer
- Replace in Buffer
- Find File
- Find in Project
- Go to Line

### Project

- Open Project...
- Close Project
- Next Project
- Previous Project
- Refresh Tree
- Compare Current File...
- Reveal Active File in Tree

### Terminal

- New Terminal
- Close Terminal Tab
- Focus Terminal

### Help

- Command Summary
- Open README
- Open Implementation Guide
- Open Migration TODO

Menu items should be context-aware:

- disabled when they do not apply
- checked when they reflect toggle state
- labeled with existing accelerators when those shortcuts already exist

## Tree Context Menu Plan

Add a generic popup menu layer that can be anchored to a screen rect and reused for:

- tree context menus
- future editor-tab context menus
- future terminal-tab context menus

Tree-specific context targets:

- file entry
- directory entry
- project root entry
- empty tree background

Recommended file menu:

- Open
- Open in New Tab
- Compare Against HEAD
- Compare Against Commit...
- Rename...
- Delete...
- Copy Relative Path
- Copy Absolute Path

Recommended directory menu:

- New File...
- New Folder...
- Rename...
- Delete...
- Refresh
- Copy Relative Path
- Copy Absolute Path

Recommended root menu:

- New File...
- New Folder...
- Refresh
- Close Project
- Copy Absolute Path

Recommended background menu:

- New File...
- New Folder...
- Refresh

## File Operation Layer

Do not teach `DirectoryTree` how to mutate the filesystem directly.

Add a small file-operation layer, for example `project/FileOperationService.{h,cpp}`, responsible for:

- create file
- create directory
- rename path
- delete path

`WorkspaceShell` still owns UI consequences, but the service should own the actual filesystem work and validation.

Rules for mutations:

- renaming the project root is out of scope for the first pass
- deleting the project root should also be out of scope for the first pass
- file rename should retarget open editor tabs and compare tabs
- directory rename should retarget every open tab under that subtree
- file delete should move the path to the OS trash/recycle bin after confirmation, then close affected editor and compare tabs
- directory delete should move the subtree to the OS trash/recycle bin after confirmation, and should block or prompt if any dirty editor tabs exist under that subtree
- if the OS trash operation is unavailable, the action should fail with clear feedback rather than silently falling back to permanent deletion
- every mutation should refresh the tree and file index and invalidate stale project-search results

## Compare Entry Plan

The tree surface should expose both compare entry points:

- `Compare Against HEAD`, which bypasses the picker and opens the compare tab directly against `HEAD`
- `Compare Against...`, which opens the existing commit picker flow

That same pair of actions should be available from:

- the tree context menu for files
- the main Project menu when the active target is a file-backed editor tab

## Prompt Surface Plan

Rename, create, delete confirmation, and project open all need small prompts.

Instead of adding bespoke inline editors to the tree first, add one reusable prompt surface:

- text input prompt for rename, new file, new folder, open project path
- confirmation prompt for delete and close-project with dirty tabs

That keeps the first version of tree context menus simpler and reuses the existing text-input/composition handling paths already present in the shell.

## Persistence Plan

Project tabs create the first real need for app-global state.

Keep the current split:

- per-project `.microide-config`
- per-project `.microide-session`

Add one lightweight app-level workspace session file for:

- open project roots
- project-tab order
- active project index

Recommended location:

- `$XDG_STATE_HOME/microide/workspace-session`
- fallback: `~/.local/state/microide/workspace-session`

Do not move per-project editor preferences into that global file.
Do not move colorscheme selection into that global file either; colorscheme stays project-local together with the rest of the visible workspace state.

Still out of scope for the first pass:

- terminal session restore
- broader user settings UI

## Recommended Delivery Order

1. Extract project-scoped state out of `WorkspaceShell` even while still running one project.
2. Add the shared action registry and move command handling onto it.
3. Add the project-tab strip and explicit open/close/switch project flows.
4. Add the custom menu bar using the action registry.
5. Add popup-menu support, then tree context menus.
6. Add file mutation prompts and the file-operation layer.
7. Add app-level multi-project session restore.

This order keeps the highest-risk refactors ahead of the visible chrome work and prevents duplicate plumbing.

## Resolved Product Decisions

- Closing the last project tab should leave an empty welcome window with no project loaded.
- The menu bar should stay custom and in-window; native OS menus are out of scope.
- Tree delete must use the OS trash/recycle-bin behavior, not permanent deletion.
- Multi-project restore across app restarts is required.
- Everything currently visible below the project-tab strip should be project-local, including bottom-panel state, logs, overlays, command prompt state, and terminals.
- Tree compare actions should expose both `Compare Against HEAD` and `Compare Against...`.
- Colorscheme stays project-local, so switching project tabs may change the full chrome theme with the active project.
