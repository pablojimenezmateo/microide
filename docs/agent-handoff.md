# MicroIDE Agent Handoff

This file is for the next agent starting from a cold context. Read this together with `docs/implementation-guide.md` and `docs/todo.md` before making product-direction changes.

## Non-Negotiable Product Decisions

- The visual direction is a reduced-scope, Zed-inspired desktop shell.
- The project root is the launch working directory.
- Do not reintroduce automatic project-root detection or hardcoded absolute-path markers.
- The tree must stay simple and technical: chevrons and text cues only, not pictorial folder/file icons.
- The sidebar stays persistent across tab switches.
- The bottom panel starts hidden by default.
- Activating a file tab should reveal and select that file in the tree.
- When project tabs land, file tabs plus the workspace state below them should be project-local rather than global.
- When project tabs land, everything currently visible below that strip should be project-local, including overlays, command prompt state, bottom-panel state, logs, and terminals.
- Closing the last project tab should leave an empty welcome window rather than quitting the app.
- The menu bar should remain a custom in-window surface; native OS menus are out of scope.
- Tree delete flows must use the OS trash/recycle bin rather than permanent deletion.
- Multi-project restore across restarts is required.
- Colorscheme remains project-local together with the rest of the visible workspace state.
- Compare actions should expose both `Compare Against HEAD` and `Compare Against...`.

## Verified Current State

The current SDL rewrite already has:

- a compact tab strip, persistent sidebar, breadcrumb row, editor surface, overlays, bottom panel host, and status bar
- working file open/edit/save, selection, clipboard, undo/redo, gutter, horizontal scrolling, and mouse caret/selection handling
- editor tabs that now retain unsaved buffer state, cursor position, and undo history across tab switches
- a real filesystem tree with `.gitignore` support, git markers, mouse and keyboard navigation, and commit-picker compare entry
- project file finder, sidebar project search, literal replace-in-project, a command prompt with quoted-argument parsing/history/completion, compare tabs, and working-tree jump from compare
- nested split-tree editor panes with shared buffers, mouse focus switching, divider drag, and `vsplit` / `hsplit` / `unsplit` / `split-next`
- mouse-resizable sidebar and bottom panel dividers
- visible scrollbars in the editor, compare view, sidebar tools, bottom logs, and centered overlays
- a PTY-backed bottom-panel terminal with per-tab fresh sessions, command launch, header-tab switching/close, auto-removal on exit, keyboard input, scrollback, text selection/copy, common ANSI cursor/erase handling, and panel focus
- dirty markers in the tab strip plus save/discard/cancel confirmation for dirty tab close and app shutdown
- a centered welcome card for the empty editor state
- standalone runtime-syntax highlighting driven by a generated in-tree snapshot of the old syntax assets, with PCRE2-backed region/pattern matching, multiline state in the editor and compare view, and richer token categories for types/constants/preprocessor/operators
- generated test fixtures under `tests/fixtures` now cover large plain text, large C++, syntax samples, and deterministic diff pairs for future automated tests
- `microide_tests` now exercises `CompareModel` against fixture pairs and runs a temporary-git integration check for `GitCompareService`
- editor file loads now detect encoding, preserve dominant line endings on save, and surface both in the status bar
- SDL UTF-8 text input now reaches command/search/finder/sidebar-edit fields, the terminal, and editor insertion; editor cursor motion, layout, and backspace/delete are now codepoint-aware
- editor and built-in prompt surfaces now track SDL text-composition/preedit state, render inline composition text, and update SDL text-input areas for IME candidate placement
- project-local editor preferences now persist tab size, indent width, and soft-tabs through a lightweight config file
- the main editor viewport now keeps bounded caches for visible-line layout and syntax tokens to reduce repeated large-file scroll/render work
- crisp `SDL3_ttf` LCD/subpixel text rendering on stable background surfaces
- project-local session restore for editor tabs plus sidebar/bottom-panel visibility and sizing

## Important Code Anchors

- workspace shell and interaction routing: `src/workspace/WorkspaceShell.cpp`
- tree model: `src/project/DirectoryTree.cpp`
- project search runner: `src/project/ProjectSearchService.cpp`
- terminal session model: `src/terminal/TerminalSession.cpp`
- editor painting: `src/editor/EditorViewRenderer.cpp`
- runtime syntax registry and generated bundle: `src/editor/RuntimeSyntaxRegistry.cpp` and `src/editor/RuntimeSyntaxGenerated.cpp`
- text backend and sharpness work: `src/render/SdlTtfTextBackend.cpp`
- planned multi-project/menu/context-menu design: `docs/workspace-expansion-plan.md`
- source-of-truth migration checklist: `docs/todo.md`

## Missing Features And QoL

Partially complete areas:

- terminal support now has per-tab fresh sessions, text selection/copy, common CSI cursor/erase/edit handling, cursor visibility, alternate-screen switching/save-restore behavior, and xterm-style app mouse reporting, but it is still not a full terminal emulator
- UTF-8 text entry now works in the editor too, and IME composition/preedit handling exists for the editor plus built-in prompt surfaces, but the buffer model is still byte-backed and real-IME validation is still pending
- project-local persistence now restores editor tabs, compare tabs, workspace chrome, editor preferences, and colorscheme selection, but it still does not cover terminal session restore or broader user config
- text sharpness is much better on stable backgrounds, but highlighted editor fragments still use the older blended fallback
- split navigation command parity is still narrower than the old editor even though the split tree is now in place

## Suggested Next Priorities

If the goal is the best user-visible progress per unit of work, take the tasks in roughly this order:

1. Build a shared action registry that can back commands, shortcuts, menus, and context menus
2. Add the custom menu bar and tree context menus on top of that action model
3. Add tree file mutations with dirty-tab safeguards and compare-tab path updates
4. Add a visible open-project affordance outside the command prompt, likely through the upcoming menu bar
5. Return to terminal polish, large-file validation, and broader command parity after the shell architecture is stable

## Validation Checklist

After changes, run:

```bash
cmake -S . -B build/microide
cmake --build build/microide
timeout 2s env SDL_VIDEODRIVER=dummy ./build/microide/microide
```

Manual checks worth doing in the real window:

- open several files, switch tabs, and verify the tree follows the active file
- run nested `vsplit` / `hsplit` flows and verify shared edits, click-to-focus, `split-next`, and divider dragging all behave correctly
- confirm the tree still uses chevrons and no pictorial icons
- confirm the text remains crisp on tabs, sidebar, status bar, and ordinary editor rows
- verify the app still treats the launch working directory as the project root
- manually launch `term`, open another terminal from the header `+`, close one with `Ctrl+D`, and confirm the exited tab disappears while new launches start fresh
- manually confirm typing, Enter, arrows, shell line editing, `clear`, cursor visibility changes, and simple cursor-repainting output still behave as expected in the terminal panel
- manually run a full-screen terminal app such as `less docs/todo.md` and confirm enter/exit alternate-screen behavior restores the previous shell contents
- manually run a mouse-aware terminal app that enables xterm mouse mode and confirm clicks, drag tracking, and wheel events reach the PTY instead of starting text selection
- run `tab-size`, `indent-width`, `soft-tabs`, and `colorscheme bubblegum`, restart the app, and confirm the project-local editor settings and colorscheme persist for the same project
- run `project-open <path>` twice for different roots, switch project tabs with clicks plus `project-next` / `project-prev`, and confirm file tabs, tree state, search state, panel state, and terminal tabs all stay with the correct project
- while two projects are open, also confirm overlay state, command prompt state, logs, and panel visibility stay with the correct project
- while two projects are open, also confirm colorscheme changes with the active project when different projects persist different themes
- close a non-last project tab, then close the final project tab, and confirm the app returns to the welcome window instead of quitting
- restart after multiple project tabs are open and confirm the project-tab order plus active-project selection are restored from the app-level workspace session
- after menus/context menus land, confirm menu enablement changes with focus and active tab kind, and confirm tree rename/delete flows update or close affected tabs correctly
- after compare menu/context actions land, confirm `Compare Against HEAD` opens directly while `Compare Against...` still opens the picker
- after tree delete lands, confirm deleted files and directories go to the OS trash/recycle bin rather than being permanently removed

## Documentation Expectations

- Keep `docs/todo.md` tied to verified source state, not aspirational plans.
- Use `[~]` for partial implementations instead of forcing a feature into done or missing.
- If a product decision changes, update `docs/implementation-guide.md` in the same change.
