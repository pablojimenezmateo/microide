# Changelog

All notable changes to microide are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project aims to follow semantic versioning. microide is a stable, actively developed
project (see [README](README.md)); versions track meaningful shipped work.

## [2.8.1] - 2026-08-04

A packaging fix that matters more than everything else here combined — every
release through v2.8.0 shipped a `.deb` that could not start on a machine other
than the maintainer's — plus editor-configuration fidelity, two memory-safety
fixes, a performance pass over the preference walk and the multi-caret rebuild,
and the validation lanes that were written but never actually ran. No public API
or persisted-format changes.

### Added

- **Drag and drop from the desktop.** Dropping a file on the window opens it as a
  tab; dropping a folder opens it as the project. Dropping a file while no
  project is open opens its parent folder first, so a drop onto the welcome
  screen does something. `SDL_EVENT_DROP_FILE` was simply never in the event
  switch before — the window silently ignored every drop. Non-regular files are
  rejected rather than handed to the editor, because opening a fifo blocks
  forever.
- **Keyboard column (box) selection**, `Ctrl+Shift+Alt+Arrow`, matching VSCode.
  The mouse form (`Shift+Alt+drag`) already worked; with no pointer there was no
  way to make a rectangular selection at all. The moving corner keeps a *virtual*
  column, so dragging a box down across a two-character line and onto a long one
  restores the full width instead of staying narrow. Registered in the keybinding
  registry, so it is listed in the keyboard-shortcuts overlay and can be rebound.

- **`.editorconfig` support.** A project's `.editorconfig` now decides
  `indent_style`, `indent_size`, `tab_width`, `end_of_line`,
  `trim_trailing_whitespace`, `insert_final_newline` and `max_line_length` for the
  files its sections match. It wins over the equivalent microide setting —
  matching VSCode, where EditorConfig overrides both the configured indent and
  detect-indentation — while properties it does not name fall through untouched.
  `root = true`, nearest-file precedence, `unset`, and brace/star globs all
  behave as the spec asks. Note that EditorConfig's `**` crosses `/`
  unconditionally, which is *not* how gitignore and VSCode read the same
  characters; the glob matcher takes the dialect as a parameter rather than
  quietly picking one. Resolution is memoized per path, so a project without an
  `.editorconfig` costs one hash lookup per tab. Toggle with
  `editor.editorconfig.enabled`.
- **Language servers hear about on-disk changes the editor did not make.** A
  server that registers for `workspace/didChangeWatchedFiles` through
  `client/registerCapability` now receives create/change/delete notifications for
  the paths it asked about — so a `git checkout`, an external formatter, or a
  file created outside the editor no longer leaves the server reasoning about a
  tree that has moved. `RelativePattern` bases and the `WatchKind` bitmask are
  honoured, so a server that subscribed only to deletions is not woken for edits,
  and `unregisterCapability` drops the subscription. Registrations are bounded
  (64 registrations, 128 patterns each): the list is server-controlled input and
  the match runs on the shell thread for every file in a change batch.

### Fixed

- **The published `.deb` could not start on any machine but the one that built
  it.** The packaged binary needed `libSDL3.so.0` and `libSDL3_ttf.so.0`, listed
  neither in `Depends:`, carried no `RUNPATH`, and bundled no library.
  `apt install ./microide.deb` satisfied every declared dependency and the binary
  then died in the loader. `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` delegates to
  `dpkg-shlibdeps`, which can only emit a dependency for a library owned by an
  installed `.deb`; no Debian-family distro packages SDL3, so it is built from
  source into `/usr/local`, maps to no package, and shlibdeps silently emits
  nothing. Every release from the introduction of SDL3 onward was affected.
  Linked libraries that dpkg does not own are now bundled into
  `/usr/lib/<triplet>/microide` behind an `$ORIGIN`-relative `RUNPATH`, and
  `scripts/ci/verify-deb-runtime.sh` launches the packaged binary with the loader
  cache inhibited before `release.sh` will sign it. CI additionally installs the
  package into a stock `ubuntu:24.04` container and runs it.
- **A stale language, served silently.** The active viewport's language was
  cached by path alone, so a buffer whose language comes from its *content* — a
  shebang with no extension — kept reporting its first-detected language to every
  LSP and provider caller no matter how the content changed. Editing
  `#!/bin/sh` to `#!/usr/bin/env python3` now re-detects.
- **`.editorconfig` was resolved against a stale project root.** A project-catalog
  slot that had been reset carried its root but a fresh resolver, and an unset
  root resolves to "no opinion" — silently, so the file simply stopped applying.
- **A commit finishing after its project tab closed wrote into freed memory.** The
  git commit runs on a worker thread and posts a completion that resolves back to
  the state that started it — state that lives inside the project, and is
  destroyed when the tab closes. The existing guard only covered "a newer commit
  was dispatched". Looking at it turned up two quieter cases the original report
  did not: returning to the welcome screen, and closing *any earlier* project tab,
  both move a fresh state over the old storage, so the completion landed on live
  memory belonging to a **different project** and published the result into the
  wrong project's commit panel. The operation is now held by an RAII token whose
  destructor, move and copy all give it up, so every teardown path the project
  catalog has goes through it rather than through a hook someone has to remember.
  The commit itself still lands on disk; only the panel update is dropped.

### Performance

- **The preference walk no longer costs anything per open tab.**
  `ApplyEditorPreferencesToAllTabs` runs on every settings change, project
  activation and session restore, over every tab in every editor group. It was
  re-reading seven shell-global settings per viewport, re-detecting each buffer's
  filetype, and building each tab its own deep copy of the language contract —
  four vectors and three strings apiece. Settings now resolve once per walk;
  filetype detection is memoized on the viewport, keyed on the inputs detection
  actually reads; and same-language tabs share one contract view. On the
  reference runner the contract phase went 50,020 → 10,752 allocations and
  2.640 → 0.410 ms, the whole scenario 104,799 → 63,771 allocations. Running it
  at 10 tabs instead of 40 now leaves both phases within 2 allocations of their
  40-tab numbers: the per-tab cost is gone rather than reduced.
- Four independent filetype caches had accreted around the codebase, each added
  by a caller who noticed detection was not free. They are one memo now, and an
  architecture rule keeps a fifth from growing.
- **A held column-select gesture no longer allocates per keystroke.** Box
  selection rebuilds its whole caret set on every arrow key, and the scenario
  added this cycle showed two of the three allocations behind that were pure
  ceremony — a range vector and a candidate vector, both rebuilt from scratch each
  time. Reused buffers make a gesture allocate on its first keystroke and never
  again: 4,247 → 3,319 allocations over the measured burst. The caret set was also
  sorted unconditionally, although both callers already emit it in document order
  (the box walks lines downward, the Ctrl+D scan walks the buffer forwards), so
  that is now an O(k) check instead of a sort. On the reference runner:
  `multi_caret_remap_burst` 19.36 → 18.31 ms, `editor_add_cursor_next_match`
  4.49 → 4.20 ms, `editor_shaping_multi_caret` 11.21 → 10.86 ms.

### Internal

- **Line coverage is measured, with per-area floors.** Wall time, allocations,
  three sanitizers, twelve fuzz targets and a vacuity-probed architecture lint
  were all gated; nothing measured whether a line ever ran. It found
  `WorkspaceShellRenderMerge.cpp` — 568 lines, 0 of 11 functions executed by the
  whole suite, on the surface the product is built around. Sanitizers cannot find
  a defect in code that never runs, and the lint's rules for that file are
  structural. Now covered (0.00% → 74%), and `tools/run-checks.sh coverage`
  enforces a floor per area rather than one global number, which
  `workspace/registries` at 96% would otherwise satisfy on its own.
- **The perf harness measures CPU time and resident growth.** The priority order
  is speed, correctness, low CPU, low memory; all 93 baselines carried only wall
  time and allocation counts, so priorities 3 and 4 were stated but not gated. A
  change that held its latency by burning three extra cores read as neutral.
  `cpu_ms` sums every thread, `rss_growth_bytes` is per-iteration resident delta,
  and both are now recorded across the suite. The first full sweep came back
  clean — 92 of 93 scenarios at a CPU/wall ratio ≤ 1.06, 90 recording zero
  resident growth — and turned up one real defect the allocation oracle cannot
  see by construction (filed as TD-2026-08-04-130).
- **The tech-debt ledger is a queue again.** 555 of its 637 entries were resolved
  or won't-do, including a 215-entry section titled "Still open" in which every
  entry was closed. 5,979 lines → 1,180, with the closed record archived and the
  split verified item-by-item. `ROADMAP.md` is retired: last reviewed
  2026-06-17, absent from the source-of-truth list, and forbidding work that had
  shipped in v2.0.0.
- **Contributor onboarding.** `CONTRIBUTING.md` and issue templates, and the
  repository finally has a description, homepage and topics.
- **CI actually runs.** `.github/workflows/checks.yml` wires the validation lanes
  that already existed as scripts but had never executed on a push: tests, the
  perf-harness test build (which is what arms the counting `operator new`), ASan,
  UBSan, TSan, and a fuzz smoke run. Perf *baselines* are deliberately not gated
  there — they are only meaningful on the reference runner — but a baseline move
  is surfaced on pull requests.
- `src/workspace` is split into subsystem directories (`shell/`, `render/`,
  `coordinators/`, `services/`, `registries/`, `actions/`, `state/`, `lsp/`,
  `debug/`, `git/`, `persistence/`, `control/`). The architecture lint iterates
  recursively and selects by filename, so correctly-named files stay covered
  wherever they sit; twelve rules that had been iterating non-recursively were
  fixed in the same change, which is a silent way for a lint to go blind.
- `active-work.md` is direction again rather than an accumulated inventory; the
  UI rules it had collected moved to `dev-docs/project/ui-invariants.md`.
- **The terminal's session reset is one helper, and its test twin is no longer a
  hand-kept copy.** Starting a session and starting a test placeholder still spelled
  out the same label/geometry/bookkeeping block separately — including a duplicated
  comment explaining why geometry has to be seeded before the reset, which is the
  drift warning rather than the drift. The test-side reset restated ~35 fields
  inline and had already fallen behind: it never cleared the pending OSC-52
  clipboard payload, so a fixture could start with a stale one attached.


## [2.8.0] - 2026-08-03

The largest cycle so far, in three parts. New capability: the language server's
call hierarchy, code lenses and occurrence highlighting; find in the terminal;
branch, sync and stash from inside the Git surface; scoped project search; and
column selection with the mouse. A UI/UX consistency pass over how every surface
answers the mouse, the keyboard and the hovering pointer — with a second pass
driven by screenshots of each one, which is what turned up the text cut mid-word
and the quick-open modals not agreeing on where they sit. And a sustained
performance campaign against typing, scrolling, highlighting and file-open on
large files, measured end to end on the reference runner. No public API or
persisted-format changes.

### Added

- **Call hierarchy.** `call-hierarchy [incoming|outgoing]` answers "who calls
  this?" and "what does this call?" through `prepareCallHierarchy` chained into
  the calls request, rendered as navigable `file:line:col` rows in the same
  formatter Find References uses. Direction is not just a method name: incoming
  calls navigate to the call sites *inside* each caller, which is what someone
  tracing a symbol wants to read, while outgoing calls navigate to each callee's
  own name. A server without `callHierarchyProvider` says so rather than
  reporting "no callers" — that a server cannot answer is not evidence about the
  code.
- **Code lenses.** `textDocument/codeLens` renders line-level server actions
  ("2 references", "Run test") end-of-line, or as above-line strips under
  `plugins.code_lens_above`, and clicking one runs it. `codeLens/resolve` is part
  of the feature rather than a later addition, because rust-analyzer and
  typescript-language-server send lenses title-less; the original lens
  round-trips verbatim so servers can correlate through their private `data`.
- **Occurrence highlighting comes from the language server.** The editor used to
  highlight other uses of the symbol under the caret by scanning visible lines
  for the same spelling — which paints an unrelated local in another scope,
  misses a shadowed name, and cannot tell a write from a read.
  `textDocument/documentHighlight` answers instead where a server can, with write
  highlights taking the strong tint; everything a server cannot answer keeps the
  textual scan. A resting caret costs four integer compares per frame.
- **Find in the terminal.** Ctrl+F over a focused terminal opens a find bar in the
  same compact card the in-file find uses. Every hit highlights in the grid, the
  current one in the active-match tint, and the counter reads `n/m` (`n/m+` when
  the 5000-match cap truncates the scan). Enter and Shift+Enter step between hits
  and scroll them into view; the first search lands on the *newest* hit, because
  a build log is read from the bottom. `Aa`/`ab` (Alt+C / Alt+W) toggle case and
  whole-word. The bar is non-modal — clicking the terminal underneath keeps it
  open and returns focus — and Escape hands the keyboard back so readline's ^F
  stays one keystroke away. Also reachable as `terminal-find [query]`.
- **Branch, sync and stash, from inside the editor.** Nine actions —
  `git-switch-branch`, `git-create-branch`, `git-fetch`, `git-pull`, `git-push`,
  `git-publish-branch`, `git-sync`, `git-stash`, `git-stash-pop` — from the
  command palette, the Git menu and the control channel. The Source Control
  sidebar gains a second action row: the current branch (click to open a
  filterable branch picker over local and remote refs) and a Sync button carrying
  the ahead/behind counts, so the button says what syncing would actually do.
  Every operation funnels through one classifier, so git's prose becomes an
  actionable outcome (`AuthFailed`, `NoUpstream`, `NonFastForward`,
  `DirtyWorktree`, `Conflict`, `BadRef`, `RepoLocked`, `NetworkFailed`,
  `NothingToDo`) instead of a raw dump. A switch or stash pop rewrites files under
  open editors, so clean buffers re-read from disk while dirty ones are left
  alone. Only one git write runs at a time.
- **Scoped project search.** "files to include" / "files to exclude" boxes behind
  a `...` toggle in the search sidebar, taking VS Code-style comma-separated
  globs; `i` and `x` jump straight to them. The scope is tested before a file is
  opened, so an out-of-scope file costs one string match instead of a stat and a
  whole-file read: on this repository a full-scan query goes 15.8 ms unscoped →
  9.5 ms with `--include=*.cpp` → 7.4 ms with `--include=src/**`. Replace-all
  honours the scope too — its whole-catalog fallback previously ignored it, and
  would have rewritten exactly the files the search excluded.
- **Column/box selection with the mouse.** Shift+Alt+drag (and Shift+Alt
  off-column click) makes a rectangular selection: every line in the row span
  gets a per-line selection between the two box columns, the target line holding
  the primary selection and the rest becoming ranged secondary carets. Lines
  shorter than both columns collapse to a zero-width end-of-line caret, as in
  VSCode. This was the last documented multi-caret gap.
- **Workspace edits can create, rename and delete files.** A server-driven rename
  or extract refactor now applies `documentChanges` resource operations, not just
  text edits: every op is validated against a simulated existence overlay
  (project-root containment, collisions, missing sources, non-empty directories)
  before anything is mutated, then applied with rollback-safe staging, with open
  tabs, diagnostics and plugin decorations reconciled after. Edits are
  version-aware.
- **Find File and project search say when the index is incomplete**, and why —
  "project too large", "tree too deep", "some folders unreadable". The scanner
  used to return a prefix of the tree on hitting its entry budget or depth limit
  and present it as an authoritative complete file set; an unreadable folder was
  swallowed entirely rather than reported.
- **The bottom panel's plugin surface is interactive**: wheel scrolling, a
  grabbable scrollbar, and left-click dispatch on hit regions. It painted before
  but was mouse-dead in every respect, because the panel's visibility predicate
  said "terminal or output" while the layout said "content is not none".

- **Double-click a resize divider to restore its default size.** Works on all
  six — sidebar, right/debug pane, editor split, bottom panel, and the compare
  and merge pane dividers. Either merge divider resets both, so one gesture
  recovers the layout.
- **Word and line selection in the terminal.** Double-click selects the word
  under the pointer, triple-click the whole row, matching the editor surface.
  Path/URL punctuation counts as part of a word, so double-clicking
  `src/foo/bar.cpp:42` in a build log selects the whole reference.
- **File > Open File… actually opens a file.** It (and its Ctrl+O accelerator,
  and the welcome screen's Open File action) now opens a native file picker;
  previously all three reported "open requires a path" and did nothing. Ctrl+K
  Ctrl+O for Open Folder, likewise advertised but unimplemented, now works.
- **Page/Home/End in the file tree**, which every other sidebar list already had.
- **Right-click menus on the search, problems and tests lists.** The file tree
  and git sidebar had them; the other three swallowed the right button, so the
  path of a search hit could only be copied by opening the file first. All three
  share one row menu: Reveal in File Tree, Show in File Explorer, Copy Relative
  Path, Copy Absolute Path.
- **Right-click menus in the debug pane.** Breakpoints rows open the same menu
  as the editor gutter (Disable/Enable, Set Condition…, Set Hit Count…, Set Log
  Message…, Remove); Variables and Watch rows offer Copy Value and Add to Watch.
  The pane was the last interactive list in the shell with no menu at all.
- **Keyboard scrolling in Help/About**, which swallowed every key but Escape. It
  answers the same Up/Down, Page Up/Down and Home/End contract as every other
  list, so a Help panel taller than the window is no longer mouse-only.
- **Page Up/Down and Home/End in Settings**, in both the category rail and the
  value list. Both moved one row at a time through lists hundreds of rows long.
- **Ctrl+PageDown / Ctrl+PageUp switch editor tabs**, wrapping at both ends.
  There was no key chord for this at all: tabs could be opened (Ctrl+P) and
  closed (Ctrl+W) from the keyboard, but moving between two open files needed the
  mouse. The typed `tabswitch` also takes `+n`/`-n` now, the form its sibling
  `tabmove` has always accepted.
- **Home/End in the menu bar and the context menus**, the last two lists that
  answered Up/Down and nothing else.
- **`term-close` closes a terminal**, from the command palette, the Terminal menu
  and a terminal tab's context menu. Every other tab kind could be closed without
  the mouse; terminal tabs had the ✕ and a middle click and nothing else.
- **Match Case and Whole Word in the in-file find widget** (`Aa`, `ab`, Alt+C /
  Alt+W) — the same two toggles, in the same order, with the same glyphs and the
  same chords as the terminal find bar. The editor's own find was the weakest of
  the app's three search surfaces: the project search sidebar has pattern mode, a
  case cycle, hidden files and globs; the terminal bar has `Aa` and `ab`; the
  editor had only `.*`. Both new toggles apply in literal *and* regex mode, which
  also fixes a quieter problem: literal find was always case-insensitive while
  regex find was smart-case, so flipping `.*` — a toggle about regular
  expressions — silently changed whether `Alpha` matched `alpha`.
- **Tooltips on the find widget's buttons**, in both the editor and the terminal.
  Five unlabelled or two-glyph buttons that never said what they were, now naming
  themselves and their chords.
- **Tooltips on the status bar.** Six segments built a tooltip string every frame
  — "Go to line/column", "Change indent settings", "Language: cpp", "Open
  Problems", the LSP state — plumbed it through the view model, and nothing ever
  drew it. The one row of chrome that names the language, indent, encoding and
  cursor position explained none of it, while the breadcrumb bar above it did.
- **Call Hierarchy has a feature toggle** (`lsp.call_hierarchy.enabled`) in the
  Settings LSP → Features group, and an entry in the Go menu beside Find
  References. It was the one LSP feature with no switch and no menu entry.
- **The bottom panel answers the keyboard in all of its contents.** Only a
  terminal ever did; with an Output channel or a plugin surface showing, every
  navigation key fell through to the editor *behind* the panel and scrolled that
  instead, in a surface that is in the Ctrl+Tab ring and draws a focus ring. Both
  now answer Up/Down, Page Up/Down and Home/End on the shared step, through one
  scroll model the wheel also uses.
- **The debug pane is fully keyboard-navigable.** Call Stack and Breakpoints were
  clickable but had no keyboard at all, in a pane that already has a focus ring, a
  place in the Ctrl+Tab ring, a grabbable scrollbar and row context menus. Arrow
  keys walk the Call Stack's frames (navigating the editor, as clicking does);
  Breakpoints gains a selection, with Enter to jump to the line and Space to
  toggle. Variables and Watch gained Page Up/Down and Home/End alongside them.

### Changed

- **One quick-open surface for all five list modals.** The file finder, project
  search, command palette and the git commit/launch pickers are the same
  interaction, but painted as three different dialogs: on a 1440x900 window their
  cards measured 666x290, 757x357 and 814x355, at three different positions, so
  flipping between Ctrl+P and Ctrl+Shift+P moved and resized the surface under the
  pointer. They now share one card, one header, and one set of row offsets. The
  file finder and project search also gain the result count and the
  `↑↓ select · Enter choose · Esc cancel` hint the pickers already had — the two
  most-used quick-opens were the ones that never said Enter and Esc worked.
- **Help/About is searchable.** It lists every command with its key chord — about
  190 rows — and could only be scrolled, in a surface whose whole purpose is
  looking one up. It now opens with a filter field focused. (The filtering itself
  had been implemented all along; nothing ever drew a field to type into.)
- **The regex toggle reads `.*` everywhere.** The project-search sidebar spelled it
  `Rx`/`Lit` while both find bars — and VSCode — use `.*` for the identical
  concept.
- **Inline git blame annotates the caret line only**, the way VSCode/GitLens does.
  It painted the same "author, date" string on the rows above and below the caret
  too, which read as a rendering fault — and on blank neighbour lines the text
  floated at the left margin, far from any code. Also strictly less per-frame work:
  one blame-line copy, truncation and measurement instead of three.
- **The status bar names the branch before the first git refresh.** An ordinary
  checkout reported `no-scm [clean]` — a contradiction, since only source control
  can know a tree is clean — until something ran `git status`, which for many
  sessions is never. The branch now comes from `<gitdir>/HEAD` (one file read, no
  subprocess). A detached HEAD reads `detached`; a project outside any repository
  still reads `no-scm`.
- **An all-ASCII file reports UTF-8**, not a separate `ASCII` encoding that flipped
  to `UTF-8` the moment you typed an accented character, as though the editor had
  re-encoded the file.
- **Shift+wheel scrolls the editor sideways.** It already did in the compare and
  merge surfaces, so the same gesture moved sideways in a diff and downwards in
  the file being diffed.
- **The wheel no longer scrolls a tab strip past its last tab.** It clamped on
  the raw scroll index rather than on what was still hidden, so wheeling right
  ran on until one tab sat in an otherwise blank strip — a state the ⟨ ⟩ overflow
  buttons cannot produce. Affected the project, editor and panel strips.
- **The mouse wheel scrolls every list at the editor's speed.** Lists (file
  tree, git, search, debug pane, bottom panel, Settings, the overlay pickers)
  advanced one row per tick while the editor moved three, so the tree felt an
  order of magnitude heavier than the code beside it. All of them now share one
  step.
- **Scrolling no longer steals keyboard focus.** Wheeling over the sidebar, the
  panel or an unfocused editor pane used to hand focus to whatever was under the
  pointer, silently redirecting the next keystroke.
- **The wheel over a list overlay scrolls it instead of moving the selection**,
  so a scroll can no longer change what Enter is about to run. Only the project
  search overlay behaved this way before.
- **Home/End edit the query in every overlay that has one.** The command palette,
  commit picker and launch picker consumed them to jump the result list, leaving
  no way to reach the start of a typed command line. They still address the list
  in the two field-less popups (completion, code actions).
- **The debug pane is a first-class focus surface**: it draws the focus ring the
  other three surfaces draw, and Ctrl+Tab reaches it (the cycle previously
  skipped it, so it was clickable but not tabbable).
- **Every sidebar mode answers the same navigation keys.** Up/Down, Page Up/Down
  and Home/End resolved through six near-identical per-mode blocks that had
  already drifted: Home in the git sidebar could select a row hidden under a
  collapsed directory. They now share one resolver.
- **Escape peels one layer at a time in the sidebar.** It closes an auto-opened
  panel from every mode (the file tree and git sidebar previously ignored it),
  but no longer tears the whole search panel down while you are typing a query —
  that first Escape cancels the field edit, as it does everywhere else.

### Fixed

- **Text is no longer painted as solid blocks under the software renderer.**
  Glyph surfaces store *coverage*, and coverage lives entirely in the alpha
  channel — a blended glyph is the text colour at every pixel with the shape
  carried by per-pixel alpha. Since glyph composites started being built in the
  renderer's own preferred texture format (a real speedup), a renderer
  advertising `XRGB8888` first — which has no alpha — got the shape discarded and
  an opaque rectangle blitted in its place. The tell was that only strings
  containing a non-ASCII byte came out right, because those skip the glyph atlas
  for the whole-string path: the editor body was blocks while `UTF-8 · LF` in the
  status bar was readable. The format is now chosen by walking the renderer's own
  preference order and taking the first entry that can hold alpha.
- **Status-bar segments that promise an action perform it.** Four segments
  carried tooltips written as commands — "Go to Line", "Open Problems", "Open
  Source Control", "Change indent settings" — for controls that did nothing at
  all: the `clickable` flag driving the pointer cursor and hover highlight was
  never set by any segment, and no mouse-down path anywhere looked at the status
  bar. The affordance existed in three places and the action in none. `clickable`
  is now derived from the segment's command, so the two cannot drift apart again.
  Language, Encoding and LSP stay read-only; their tooltips describe rather than
  instruct.
- **Inline blame stops re-spawning git every frame.** Scrolling a compare view in
  a long-lived process could reach a state where blame re-ran its whole git probe
  chain per frame and never once wrote a cache entry — 45 spawns and 264 ms of
  subprocess wait per iteration, against 2 spawns and 5 ms for the same work in a
  fresh process. Three post-subprocess steps were gated on "is my window still the
  newest one", which under a moving viewport it never is, so the file-level
  verdict was never stamped, so the throttle had nothing to hit, so the next frame
  re-probed. None of those steps depend on the window: a span's attributions are a
  pure function of file, head, stamp and line range.
- **The curved toolbar glyphs are drawn as strokes.** SDL's line renderer is 1px
  and unantialiased, so a curve assembled from a handful of long chords shows its
  facets at icon size: Step Over was a ten-chord half-circle that read as a
  scribble, Restart a visibly lumpy sixteen-sided polygon, and the boolean
  checkmark two hairlines with mixed fractional and absolute geometry, so it sat
  thin and lopsided. Arcs are now sampled at two samples per pixel of arc length,
  snapped to the pixel grid, at the same 1px weight as the straight-line glyphs
  beside them.
- **The debug toolbar's tooltip appears when you hover, not when something else
  repaints.** It was the one tooltip in the shell not resolved through the shared
  hover surface: the motion handler invalidated the toolbar *card*, while the
  tooltip is anchored below it, so nothing ever invalidated the region the tooltip
  occupies and it reached the screen only when an unrelated repaint — usually the
  caret blink — happened to cover it. Hover for less than a blink period and no
  tooltip appeared at all.
- **An empty picker says "no matches" once.** Making the count line read "No
  matching files" put it directly above the list's own "No matching files". The
  count row is blank when there is nothing to count and the message lives in the
  list area alone, where the eye goes when a list is empty. That also uncovered a
  hardcoded `0 of 0` fallback that substituted a fake count whenever a picker had
  no summary at all.
- **A failed LSP request no longer reports an authoritative empty answer.**
  Go-to-definition, references, type definition, implementation, declaration,
  workspace symbol, rename and formatting said "No X found" when the request had
  actually timed out or errored — the two were indistinguishable in the callback.
  Every request now carries an explicit outcome, and a transport failure surfaces
  "Language server did not respond" instead.
- **Plugin-provided strings are bounded per field.** A single hostile or buggy
  plugin field could hand the host a multi-hundred-megabyte string to copy,
  retain, measure and render. Every scalar provider field now passes a 4 MiB
  central backstop on a UTF-8 boundary, with tighter 32 KiB caps on the plugin
  diagnostic message and the hover title/content.
- **Key hints spell themselves one way.** The git sidebar's action line and the
  compare review header joined their segments with `|` while every other hint in
  the app used `·` — including the overlay hint whose helper documents `·` as the
  shell's one spelling. Both had grown a private copy of the same six-line join
  helper. They share the real one now, so `Enter default · d diff · s stage · …`
  matches `↑↓ select · Enter choose · Esc cancel`.
- **The bottom panel explains itself when it is empty.** It was the last row list
  in the shell that painted a blank body over a header: an Output tab opened
  before its tool writes a line (a debug console between "session started" and the
  adapter's first byte, for instance), and a Terminal panel with no live session.
  Both now carry the same kind of hint the debug pane and sidebar modes already
  had.
- **Result counts say what they are counting, and stop double-counting.** The
  command palette, launch-config picker and commit picker each showed a bare
  `187 of 187` — a denominator that only ever equals the numerator when no filter
  is active, which reads like one is. All three (and the two new overlay footers)
  now share one helper: `12 of 187 commands` while filtering, `187 commands` when
  not, `No matching commands` when nothing survived.
- **The Problems, Tests and Outline sidebars are reachable again.** All three are
  registered sidebar views that render fine, but both the sidebar's view-tab row
  and its overflow menu filtered them out by name — a leftover from when they were
  retired surfaces, never undone when they came back. They could only be opened by
  typing `sidebar-show problems` in the command palette, and once open the rail
  showed three unhighlighted tabs, no overflow button, and no way back. They now
  appear in the overflow menu, and the overflow button lights up whenever the
  active view is one of the tabless ones.
- **Breakpoints work again on gdb 15.x.** On the Ubuntu 24.04 system gdb, a debug
  session launched, streamed the program's entire output, and terminated without
  ever stopping — every breakpoint silently ignored. The DAP handshake sent
  `launch` before `setBreakpoints`, which is correct for gdb 17.x (it defers
  running the program to `configurationDone` and rejects a `configurationDone`
  with no launch pending) but wrong for gdb 15.x, which starts the program on
  `launch` itself. Breakpoints now go out before `launch`, and `launch` still
  precedes `configurationDone` — the one order that satisfies both.
- **Settings and Help/About say how many rows a filter left, and which keys
  work.** Both gained the footer the quick-open modals already had: a count on
  the left (`2 of 171 shortcuts`, `4 of 79 settings`) and the key hint on the
  right. Typing a needle that matched nothing used to leave a large blank card
  with no explanation; it now says so in the list area and reads "No matches" in
  the footer. Help/About advertises `↑↓ scroll · Esc close` rather than the
  picker's "Enter choose", because its rows are read-only.
- **The Source Control view stops padding itself with empty groups.** A clean
  checkout — the common case — filled the rail with `Conflicts (0)`, `Staged (0)`,
  `Unstaged (0)`, `Untracked (0)` and `Outgoing (0)`, each with its own "No merge
  conflicts" / "No staged changes" placeholder beneath it: ten rows saying
  nothing. Empty groups are hidden now, as in VSCode, and a clean tree says "No
  changes" once. Outgoing is the deliberate exception: its header carries the
  base-branch button, so hiding it when empty would leave no way to choose a base
  to compare against.
- **A modal no longer eats the editor behind it.** Opening the compare/commit
  picker over a file faded the code being compared — breadcrumb band included —
  to a flat rectangle within about a second, and it came back only when the
  overlay closed. Underneath was something wider: SDL's draw blend mode is
  ambient renderer state and nothing armed it, so *every* translucent fill in the
  app (the modal backdrop, the selection fill, search matches, bracket match, the
  execution line, diagnostic underlines) overwrote its region with the raw colour
  and its alpha instead of compositing over it — and the retained scene texture,
  which SDL defaults to blending, then re-composited that region against its own
  previous output on every present until it collapsed to the fill colour. Blend
  mode is now derived from the colour's alpha in one shared primitive, and the
  scene presents opaque (which is also one less per-pixel blend per frame).
  Translucency across the app is what the theme asks for rather than an accident
  of the presentation blend.
- **The status bar updates.** Moving the caret left it reading `Ln 1, Col 1`, for
  the rest of the session unless something unrelated forced a full redraw; opening
  a `.cpp` from a `.md` left `markdown` and `Tabs: 4` sitting next to C++ source.
  Every value on the bar is derived from state owned by another surface, so the
  event that changed one asked for *that* surface to be repainted and the strip
  kept its pixels — there was no request-the-status-bar path anywhere in the
  codebase to call. It asks for itself now, and only when a painted value actually
  moved. (This also unstalled redraws requested from the render path in general —
  the event loop used to block with one outstanding, which had the same latent
  effect on the compare view's progressive syntax highlighting.)
- **The debugger toggle can turn the debugger off.** Over the control channel
  `debug-toggle-enabled` was a no-op that always reported "Debugger disabled",
  in both directions: the channel auto-enables the debugger before any
  `breakpoint-*`/`debug-*` command so headless drivers need no prelude, and the
  prefix rule also caught the one command whose purpose is to flip that setting.
  Each toggle was preceded by an enable, so it read "enabled", wrote "false", and
  the next `debug-` command turned it back on. The master switch is excluded now.
- **Settings and Help/About dim the editor behind them.** They take the keyboard
  entirely, but they were the only two modal surfaces in the shell that painted no
  backdrop — the editor stayed at full brightness behind them while every
  quick-open surface and both prompts dimmed, so neither read as modal. They also
  used to fall through to a whole-window repaint on every keystroke, scroll and
  arrow key (~17 call sites), because the shared overlay redraw path only knew
  about the *other* overlay surface; they repaint their own region now.
- **The status bar describes the file the caret is in.** On a compare or merge
  tab it read the group's active *editor* viewport instead, so a three-way merge
  of a `.cpp` sat under `markdown  Tabs: 4` left over from the previously open
  file — visible in the shipped merge screenshot. It reads the merge result
  buffer / the compare tab's editable side now.
- **The terminal is no longer blank when a startup rc runs `clear`.** The primary
  buffer's viewport is the last N lines of the scrollback and the cursor is an
  absolute index into it, so a `clear` issued before the panel's real geometry was
  known left one pre-layout screen of lines with the cursor on the first — and the
  resize that followed parked the viewport on the blank tail, with the prompt above
  it and every subsequent line the shell printed landing off-screen. Permanently:
  nothing scrolled it back. A shrink now drops the unused blank tail of the old
  screen, the way xterm and VTE do. (The project's own showcase screenshots were
  shipping a blank terminal panel because of this.)
- **Toast messages are no longer sheared mid-word.** A notification capped its text
  at a flat 320px and let the clip rect cut the rest, so `review-branch main:
  opened 1, reused 0, skipped 2` rendered as `review-branch main: opened 1, reused
  0,` — with no ellipsis, on a 1440px window with room to spare, losing exactly the
  part the message was about. The width now scales with the window and anything
  still too long ends in `…`.
- **Empty states in narrow rails wrap instead of being cut.** The Problems panel
  said `No problems detected in this pro…`, and the debug pane's hints ("No
  breakpoints — click the editor gutter to add one.") had no truncation at all and
  simply ran off the panel edge. They word-wrap now. The debug pane's Variables
  mode, which had no empty state whatsoever and painted as a blank box, has one.
- **The search sidebar's status line fits the sidebar.** It packed a five-segment
  key cheat-sheet onto one line inside a 288px rail — about twice the available
  width — so it always rendered as `26 matches | / query | = rep…`, cutting away
  the keys it existed to teach. It is status only now; the keys moved to Help/About
  next to the git sidebar's, where the column has room for them.
- **Tooltips no longer come and go at random.** Each one was its own pair of shell
  methods with a private copy of the placement maths, and every caller had to
  remember to list all six; they had drifted. The project-search option tooltips
  were missing from both menu-blocked invalidation lists, so a stale card stayed
  painted under an opening menu. The git sidebar's was missing from motion
  change-detection, so hovering Refresh only produced a tooltip if the sidebar
  repainted for some other reason. The sidebar mode row's — the only tooltip on an
  icon-only control, and so the one that matters most — was in neither. There is
  now one resolver for the whole window, and one placement rule: centered on the
  control the tooltip describes, below it when it fits and flipped above when it
  does not (so the status bar's appear above the bar), clamped to the window.
  Anchoring to the control rather than to the pointer also keeps the card still
  while the pointer moves inside one button.
- **The find widget's five buttons lift under the pointer**, like every other
  button in the shell. They stayed flat while the cursor over them was already a
  hand.
- **A partial redraw no longer leaves the last pixel row of its range stale.** The
  three row-range dirty-rect builders — editor, compare, merge — each nudged the
  rect's top up by a pixel and left the height alone, so the bottom row of a
  partially repainted range was never invalidated and kept whatever had been drawn
  there. Visible where a blame overlay line sat: typing to dirty a buffer
  suppresses blame, but one row of the old blame text survived.
- **Format-on-save no longer throws away the scroll when text is selected.**
  "Reload this buffer and put the view back" existed twice; the save-path copy
  restored the selection *after* the scroll, and moving the caret drags the scroll
  with it. Select all, then save through a formatter or save participant, and the
  view jumped to the end of the file. Horizontal scroll went the same way.
- **The compare surface's collapsed-context buttons agree with their cursor.** The
  click, the hover highlight, the cursor shape and the test accessor each derived
  the action-button rects independently, against a rule the header states in prose
  ("every hit-test path must derive them from this same rect"). The accessor was
  the one that got it wrong, so the test asserting these buttons offer a hand
  cursor had been probing coordinates several pixels off the painted buttons — it
  passed only because it probed the centre of a 140px-wide button.
- **Every painted scrollbar can now be grabbed.** Three could not. The debug
  pane's had been painted since the pane shipped but never hit-tested, so it was
  decoration — and because the row hit test covered the whole content rect, a
  press on the bar activated the row behind it (in Breakpoints mode, navigating
  the editor to a random file). Help/About's was in the same state, with its
  scroll bound resolved inside the paint pass, so wheel scrolling did nothing
  until a frame had been drawn and used a stale bound after a resize. The
  font-picker dropdown's was the third; worse, because the dropdown floats over
  the settings scrollbar, clicking its bar jumped the rows behind it. An
  architecture lint now fails the build on a scrollbar that can never render as
  being dragged.
- **Settings rows lift under the pointer.** Every other list in the shell does —
  sidebar, overlay pickers, debug pane, tab strips — but the largest list in the
  app, whose rows already turn the cursor into a hand, painted only its keyboard
  selection. Both the value rows and the category rail were affected.
- **Call Hierarchy is greyed out when it cannot run.** It had no availability rule
  at all, so it fell through to the default and offered itself as enabled with no
  editor open, then failed on invocation. It was also the only enumerator the
  build's -Wswitch warning was pointing at.
- **Notification toasts are dismissable, and stop swallowing nothing.** They were
  painted and never hit-tested, so a click on one fell through to whatever it
  covered — over the editor, that moved the caret — and a message you had read
  still sat there for its full four seconds. A click now dismisses the toast it
  lands on, and the pointer says so.
- **Scrolling a diff no longer steals keyboard focus.** Compare and merge set
  focus to the editor on every wheel tick while the other five scrollable
  surfaces explicitly did not, so a wheel nudge over a diff redirected the next
  keystroke away from whatever you were typing into. An architecture lint now
  enforces the rule the other five only stated in a comment.
- **A focused plugin surface draws its focus ring.** The panel's plugin-surface
  branch returned before painting it, leaving the only surface in the shell that
  could hold the keyboard without showing it.
- **The debug pane scrolls to its keyboard selection.** Variables and Watch moved
  the selection without touching the scroll, so arrowing past the last visible row
  walked the highlight off screen and kept going.
- **The resize cursor holds for the whole drag.** Only the sidebar and bottom
  panel kept their resize shape once the pointer left the divider; the right
  pane, editor split, compare and merge dividers flickered back to an arrow
  mid-drag. The merge dividers also had no widened grab margin, so they had to be
  hit within a glyph's width — compare's 12px margin now applies to all three.
- **Saving a file no longer leaks a phantom entry into the file finder.** Every
  save stages its bytes in a temp file beside the target and renames it into
  place; nothing filtered that temp, so a file-watcher batch landing inside the
  save window put it in the index, where it showed up in Ctrl+P and in project
  search results pointing at a file that no longer exists.
- **The control channel's client socket leaked into every spawned process.** It
  was created without close-on-exec, so terminal shells, LSP servers, DAP
  adapters, git, and plugin-launched tools all inherited a live connected handle
  to the interface that drives the editor headlessly.
- **Text handling no longer depends on the process locale.** Word boundaries,
  token classification, command parsing, and terminal escape parsing used
  `<cctype>`, which is locale-sensitive for bytes >= 0x80 — and SDL changes the
  process locale behind our back. JSON numbers were affected the same way: on a
  comma-decimal locale, doubles were written as `1,5` (malformed JSON to every
  LSP/DAP peer) and `1.5` failed to parse, silently resetting float settings and
  persisted pane splits to defaults.
- **Syntax highlighting no longer disappears on lines with invalid UTF-8.** A
  Latin-1 source file, or a single mis-encoded byte in an otherwise-UTF-8 file,
  silently rendered that entire line unhighlighted.
- **Case-insensitive regex search now folds Unicode case**, matching what literal
  search already did. Searching `Δ` finds `δ`, `É` finds `é`. ASCII queries keep
  the faster byte-oriented path.
- **Boolean settings honour `FALSE` and `no`.** A setting written as `FALSE` (any
  casing) or `no` read as *enabled* — the opposite of what was written.
- **Glyph rendering.** A character rendered wider than its measured advance could
  overwrite its neighbour in the glyph atlas, corrupting that character.
- **Reference snippets from large files** (Assist) returned extra unrelated lines
  and read the whole file instead of stopping at the requested range.

### Performance

Seven measurement passes on the reference runner, each one gated by a scenario
before it was optimized. Every number below is measured on that runner; the
per-pass write-ups, including the changes that were measured and **rejected**,
are in `dev-docs/performance/performance-findings.md`.

- **Typing in a large file is roughly 2.4x faster.** Mid-file edit latency on a
  50k-line file went 245 → 102 ms across the campaign, and editing line 0 — the
  worst case, because the bracket resume has nothing to resume from — went 277.9
  → 169.1 ms with shell-thread allocations dropping 57,123 → 19,147. The fixes
  were mostly things being redone that did not need redoing: editing line 0
  rebuilt every line's visual width and cleared two 50k-entry vectors that were
  about to be refilled; the indent fold scan re-derived "is this line blank" for
  every line, on a check that could never fire; the width update heap-allocated a
  vector per keystroke; the indent measure counted leading whitespace one byte at
  a time (2.30 → 0.63 ms); and the "incremental" bracket resume walked the whole
  document anyway.
- **Syntax highlighting is ~1.9x faster** — 24.5 → 12.9 ms per 4000 C++ lines,
  11.5 → 5.8 ms on Python. `PCRE2_UCP` costs about half the match time and
  `PCRE2_UTF` another sixth, but both only change behaviour at code points ≥
  0x80, so each rule is compiled twice and an all-ASCII line takes the byte-mode
  copy — verified across all 1906 built-in rule literals, both compilations,
  20k fixture lines, zero divergences. The match-data cache was a hash probe per
  rule per line and is now an array index, and matching dispatches straight to
  the PCRE2 JIT where that is safe. It had no scenario of its own before this
  cycle, which is why a 2x change in it had been invisible.
- **The fold recompute is ~1.5x faster per keystroke** (4.8 → 3.1 ms on 50k
  lines): the bracket scan skips eight bytes at a time, the per-line syntax-token
  probe is deferred until a line is known to hold a bracket, and the merge buckets
  by opener line instead of comparison-sorting tens of thousands of ranges per
  keystroke (0.75 → 0.06 ms).
- **The editor view model was built twice per frame** whenever sticky scroll was
  on, which is the default: the band's height decides the visible row count, and
  the band's line list came out of the view-model build, so the pane built the
  whole model against a zero-height band just to read a count.
- **Scrolling a large file no longer leaves a second copy of it resident.** The
  render row loop read line text through the copying accessor — six heap copies
  per row, into an unbounded cache cleared only on mutation. Allocations across
  the full-document scroll workout fell 29%.
- **Opening a file is faster**: newlines are indexed with `memchr` instead of byte
  by byte (8.2M iterations down to one scan for the 50k-line fixture, 2.64 → 0.34
  ms), line endings and encoding are classified in fewer passes over the bytes,
  and a fold model attaching no longer wipes every layout cache it is about to
  rebuild.
- **Every subprocess spawn stopped sleeping up to 5 ms** after its output had
  already been read; the child is now waited on as an event. This is paid by git,
  LSP, DAP and every plugin-launched tool.
- **Indent guides are computed without sorting** — 46 → 4.2 µs per frame.
- **The file watcher stopped doing its slow work on the shell thread.**
  `close()` on an inotify descriptor blocks for milliseconds at random, and
  watches were being removed one syscall at a time before the close.
- **Workspace state writes moved off the shell thread**, and a state file that
  already holds the bytes about to be written is not rewritten at all. Opening a
  file no longer pays a durable disk write for the recents list.
- **Git status refresh stops re-deriving normalized paths**, the sidebar resolves
  tree badges by prefix strip rather than path algebra, and blame stops
  re-spawning git per frame for files it has already determined it cannot answer.
- **JSON objects are a sorted flat vector rather than a hash map**, which is
  faster to build and to walk for the small objects LSP and DAP traffic in.
- **Hover cards stop re-wrapping their text every frame.** Wrapping normalizes,
  tokenizes and measures the whole string, and ran several times per frame for as
  long as a card was on screen. It is now memoized on (text, width, line cap,
  font metrics).
- **The project-search sidebar stops rebuilding its empty-state line every
  frame** — two heap allocations per repaint on a surface that repaints on every
  search-progress tick.
- **Five paint paths stop allocating a string per label per frame.** Truncating a
  label to fit returned an owning string in the Settings overlay (three per
  visible row, plus six for the chrome), the debug pane (twelve sites), the
  sidebar, the hover card and the breadcrumb/prompt chrome, while the other
  render paths used the allocation-free variant. A lint keeps them aligned.
- **Much faster terminal output.** Runs of plain printable text are now written
  to the grid in bulk instead of one byte at a time through the full escape
  parser: 3-4x on build logs and compiler diagnostics, ~1.7x on mixed
  CJK/Unicode output.
- **Faster file finder.** Candidates are rejected on a character bitmask before
  the subsequence scan, cutting per-keystroke work 2.4-4x on a large project.
- **Faster diffs, again.** Lines are interned to integer ids before the LCS, so
  the table compares an integer per cell instead of re-running a full string
  compare: ~2x normally and ~9x with "ignore whitespace" enabled.
- **Faster editor scrolling and whole-file reads.** The document model memoizes
  the last line offset it resolved, roughly halving the tree lookups an
  in-order walk needs (~2x on viewport rendering and full-buffer scans).
- **Faster diffs.** Hunk alignment — the dominant phase of building a comparison
  — is ~39% faster on a diff with several thousand modified lines, via reused
  per-thread working buffers, a rolling-row token LCS, and skipping line-pair
  similarity work that cannot affect the result.
- **Faster whole-document operations.** Save, in-file find, replace-all, JSON
  formatting, compare review, and LSP sync now take the document straight from
  the piece tree in one walk, instead of first materializing one heap-allocated
  string per line.
- **Project indexing** does less work per filesystem entry (allocation-free path
  containment).

## [2.7.2] - 2026-07-19

A **search, compare, and editor** cycle on top of 2.7.1. Find & replace
gains full regex support in both the project-wide and in-file surfaces,
comparisons no longer require git, and a JSON formatter lands in the editor. A
focus click-through fix makes the first click on an inactive window land on its
target. No public API or persisted-format changes — a recommended upgrade for
all users.

### Added

- **Regex find & replace.** Both the project-wide and in-file find & replace
  surfaces accept PCRE2 regular expressions, with capture-group substitution in
  the replacement. A single shared engine backs both surfaces.
- **Non-git comparisons.** Compare arbitrary files, editor buffers, and the
  clipboard against each other — no git repository required. The review mode is
  sticky per session.
- **Format JSON command.** A new editor command reformats the active buffer as
  key-sorted, indented JSON in memory.

### Fixed

- **In-file regex across line breaks.** Multi-line regex matches (spanning `\n`)
  now resolve correctly in the in-file search surface, and matched newlines are
  visualized with an end-of-line marker.
- **Focus click-through.** Clicking an inactive microide window now both
  activates the window and delivers the click to the target underneath, instead
  of swallowing the activating click.

## [2.7.1] - 2026-07-19

A **performance, robustness, and hardening** cycle on top of 2.7.0 — the large
tech-debt burndown (`TD-2026-07-16/17` and the `TD-2026-07-17A` addendum) landed
end to end. Hot paths across editor, render, LSP/DAP, terminal, search, and
persistence are measurably faster on the reference runner; a broad "bound the
resource" sweep caps previously unbounded queues, buffers, and result sets; and
several real correctness and plugin-sandbox defects are fixed. The full suite
plus ASAN/UBSAN/TSAN stay green. No public API or persisted-format changes — a
recommended upgrade for all users.

### Performance

- **Off the shell thread.** Project-wide replace-all, forced file-index rescans,
  file-manager reveal, server-pushed `WorkspaceEdit` writes to closed files,
  syntax reload, and buffered PTY writes now run off the UI thread, so large
  operations no longer stall the frame. Shutting-down LSP clients retire to a
  host-owned pool instead of blocking.
- **Render view models.** Breadcrumb labels, git-sidebar entry labels and spans,
  settings edit-control values, output-reference paths, project-search line maps,
  bottom-panel tab models, and commit bodies are precomputed/memoized so paint
  stays allocation-free on the hot path.
- **Algorithmic rewrites.** Roughly a dozen quadratic paths became indexed or
  amortized (multi-caret result remap, snippet mirror shifts, deleted-directory
  stat sweep, soft-wrap edit caches now O(edit) in place, `PostLatest` dedup now
  O(1), Add-Cursor-at-All-Matches folds once and is capped). Replace-All and
  surround reuse existing matches instead of rescanning; merge/undo track applied
  edit spans rather than diffing the whole buffer.

### Fixed

- **Bounded resources.** A wide sweep caps in-flight and retained work that could
  previously grow without limit: LSP/DAP outbound queues (by retained bytes),
  the event drain, session-restore tab rebuilds, raster decode bytes, provider
  and code-action result sets, control-query responses, output channels, debug
  value trees, clipboard export, text-measurement, and more. Silent truncation
  now surfaces status where it matters.
- **Plugin sandbox.** Removed the Lua `loadfile`/`dofile` file loaders; embedded
  NULs and invalid env keys are rejected; the process sandbox is untied from
  filesystem capabilities; and provider results survive a single failing or
  slow provider instead of being dropped wholesale.
- **Filesystem & persistence safety.** Persisted-record writes rotate through the
  symlink target so a symlinked state file is followed, not clobbered; the
  control socket hardens its runtime dir before bind; the tool downloader does a
  strict `file://` decode.
- **Editor & workspace.** Dirty `CloseTab` prompts survive a tab close/reorder;
  background compare & merge tabs retarget or close on path mutation; ranged
  secondary caret anchors are preserved across shaping.

### Internal

- The test suite is sharded for parallel `ctest`, cutting sanitizer wall time
  substantially; new perf coverage gates the rewritten hot paths, with decoupled
  wall-clock vs. allocation baseline tolerances.

## [2.7.0] - 2026-07-15

A **correctness-hardening and tech-debt closeout** cycle on top of 2.6.8. Roughly
two dozen real defects were fixed across the editor, persistence, terminal,
git/diff/merge, LSP, DAP, and plugin subsystems — each with regression coverage —
while the full suite plus ASAN/UBSAN/TSAN stay green. The multi-batch deferred
tech-debt backlog is now fully triaged and closed, and several hot paths (syntax
highlight, git picker, compare, diagnostics) are measurably faster on the
reference runner. No public API or persisted-format changes — a recommended
upgrade for all users.

### Added

- **Terminal clipboard status.** When an oversized OSC 52 clipboard write is
  dropped, the terminal now surfaces a status instead of silently discarding it.

### Fixed

- **Workspace edits.** Both LSP appliers and `editor.apply_edits` now reject
  overlapping, beyond-EOF, and over-cap edits (including in closed files) rather
  than truncating or applying them partially; multi-caret apply paths refuse
  overlapping selections.
- **Editor & input.** Multi-line paste into single-line fields collapses to
  spaces; the mouse wheel scrolls the pane under the cursor; block-comment toggle
  is language-aware; a snippet linked-edit session drops on a multi-line insert
  and discards stale secondary carets.
- **Syntax highlighting.** `^`-anchored rules honor the true line start
  (`PCRE2_NOTBOL` on mid-line segments) so anchored patterns no longer match
  inside a line.
- **Persistence.** A corrupt primary state file is recovered-and-protected with a
  header-first bounded read instead of being trusted or clobbered.
- **Filesystem safety.** No-overwrite `RenamePath` closes the exists()-then-move
  race; symlinked save targets no longer risk data loss.
- **Git / diff / merge.** Merge delete-resolve is transactional; per-file search
  reads are capped; the status bar reflects in-session `git init` / `.git`
  removal; compare picker/ref git queries run off the UI thread.
- **DAP.** Function-breakpoint verification is bounded to the requested count;
  the stopped view is restored when the adapter rejects a resume; late
  thread-list / pause callbacks are state-guarded.
- **Status bar.** Plugin status items are stably ordered (`stable_sort`).
- **Rendering.** The renderer rejects non-finite or negative display-list content
  dimensions; the highlight prefetch worker is drained in the destructor.

### Performance

- Cross-subsystem speed sweep across syntax, LSP, editor, finder, and git.
- Syntax highlighting fast-paths empty skip masking.
- A dedicated git picker lane plus off-UI-thread compare/ref picker queries.
- O(1) debug sibling ordinal and a reused per-line visual-column map for
  diagnostic underline rects.

### Tech debt

- The multi-batch deferred tech-debt backlog (sweep batches A–W plus the residual
  triage tranches) is fully closed. Rationale for the won't-do items is recorded
  in `dev-docs/project/known-tech-debt.md`.

## [2.6.8] - 2026-07-10

A **correctness-hardening and polish** cycle on top of 2.6.7. Five further
cross-subsystem bug-hunt passes fixed real defects across the editor,
persistence, terminal, git/diff/merge, LSP, DAP, and plugin subsystems — each
with regression coverage — while the full suite plus ASAN/UBSAN/TSAN stay green.
This cycle also adds a user-facing **Reveal in File Tree** action, polishes the
Settings surface, closes the flagship plugin-metamethod tech-debt item, and
measurably speeds up the LSP wire-decode paths. No public API or format changes —
a recommended upgrade for all users.

### Added

- **Reveal in File Tree** — a new editor-tab context-menu item and
  `reveal-in-tree` command-palette command. It opens the sidebar on the Tree
  view, force-expands the file's collapsed ancestors, selects it, and scrolls it
  into view (VSCode "Reveal in Explorer" behavior).

### Changed

- **Settings surface polish.** The category rail now scrolls (wheel, keyboard
  reveal-on-navigate, draggable scrollbar) so the last category is no longer
  clipped on shorter windows. Every section renders a fixed header band, and the
  ~25-item ungrouped "General" catch-all is regrouped into named Editor /
  Appearance / Terminal / Diagnostics sections.
- **Faster LSP wire decode.** `semanticTokens/full` decode, `documentSymbol`
  outline parse, and `Content-Length` message framing are measurably faster on
  the reference runner this cycle (no baseline regressions elsewhere).

### Fixed

- **Path/URI security.** Strict `file://` parsing (local-authority-only, strict
  percent-decode, NUL reject); malformed LSP code-action/rename URIs no longer
  edit the active buffer; server `applyEdit` and plugin containment are confined
  to the project root and fail closed on canonicalization errors; watcher,
  batch-apply, and traversal filters reject `..`-escaping paths; exclusive
  (`O_EXCL`) file creation.
- **Split-view correctness.** Replace-all dirty-guard/reopen, compare/merge
  external-change invalidation, and control-channel tab listing now scan every
  editor group; file rename/delete propagates group-aware across all groups.
- **Editor & snippets.** Snippet backspace/delete honor UTF-8 and reject
  multi-line mirror inserts; click/drag horizontal-scroll double-count fixed;
  a failed file open no longer moves the caret in the previously-active buffer.
- **Terminal.** Correct scrollback-trim accounting for modern clear (ED2+ED3),
  `DECXCPR`/`CSI 6n` screen-relative row reporting, `CUU`/`CPL` scrollback climb,
  `DECSC`/`DECRC` and `DECOM` origin/scrollback handling, and soft-wrap flag
  preservation on hard LF.
- **Git / diff / merge.** Partial-stage warning under porcelain v2, compare
  changed-span double-dim, merge preview bottom clip, and copied compare patches
  are now real `git apply`-able diffs with git-quoted paths.
- **LSP / DAP.** Diagnostics use half-open ranges and honor the echoed version;
  `stopped`/`continued` honor optional `threadId`/`allThreadsContinued`;
  breakpoint verification keeps both requested and resolved lines; CRLF
  incremental `didChange` is re-encoded correctly.
- **Plugin host.** Metamethod-capable Lua field harvests are now performed
  under protection so a plugin-installed raising `__index` can no longer
  `longjmp` over live C++ destructors (closes the flagship deferred tech-debt
  item); a plugin-sidebar refresh use-after-free is fixed; harvest counts are
  clamped; editor position fields read at full double precision; raster
  dimensions are range-clamped before narrowing.
- **Robustness caps & validation.** RFC-8259 JSON grammar with control-char
  reject; snippet/env/decoration/MRU/debug-state decode caps; launch-config JSON
  and tool-SHA validation; atomic control-descriptor writes (temp+rename,
  0600/0700); protocol integer-range guards; non-throwing filesystem probes on
  UI paths; `O_CLOEXEC` on the durable-save staging fd and the control socket.

Deferred items (renameat2 no-replace, two-generation persisted backup,
cross-device move rollback, symlink-save root confinement, multi-file rename
atomicity, closed-file diagnostic re-encoding, plugin async-executor cluster,
and the merge/render perf batch) are recorded with rationale in
`dev-docs/project/known-tech-debt.md`.

## [2.6.7] - 2026-07-09

A **correctness-hardening and consolidation** cycle on top of 2.6.6. Eight
successive cross-subsystem bug-hunt passes fixed real defects across the editor,
persistence, terminal, git/diff/merge, LSP, and debugger subsystems; four
verbatim code duplications were consolidated into shared modules (net code
reduction); and the LSP/DAP performance surface gained deterministic,
baseline-gated regression coverage. No public API or format changes — a
recommended upgrade for all users.

### Added

- **Gated LSP performance scenarios** — deterministic micro-benchmarks over the
  language-server wire path: `semanticTokens/full` decode, `publishDiagnostics`
  parse, `documentSymbol` outline parse, and `Content-Length` message framing
  (`tests/perf/LspPerfScenarios.cpp`), each with a committed reference-runner
  baseline.
- **Promoted debugger/DAP performance scenarios** — the six pure-unit DAP
  micro-benchmarks (value-tree expand/rebuild/paging, protocol encode/decode,
  breakpoints-model rebuild, pane hit-test geometry) are now baseline-gated with
  committed baselines rather than advisory-only.

### Changed

- **Shared edit primitives consolidated.** Four byte-for-byte duplications folded
  into focused shared modules — filesystem ops (`platform/FsOps`), lexical path
  prefix replacement (`util/PathMatch`, dropping a per-path syscall on rename
  remap), terminal child-shutdown escalation and shell resolution
  (`platform/ShellProcess`), and the self-pipe wake + outbound-queue machinery
  shared between the LSP and DAP stdio clients (`util/WakePipe`,
  `workspace/StdioClientQueue`).
- **LSP/DAP transport parity.** The DAP client adopted the LSP client's
  bounded-acquisition shutdown so teardown can no longer stall behind an I/O
  thread blocked writing to a wedged-but-alive adapter; requests registered
  during a failed initialize handshake are now explicitly failed on both stdio
  transports instead of being silently dropped.

### Fixed

- **Multi-caret line-move allocation regression.** Grouping `move-line-up/down`
  for correct multi-caret redo had made the undo machinery deep-copy each grouped
  edit's line slices three times; a wide multi-caret line move now records the
  aggregate once, restoring allocation counts to parity (fixes a ~50% allocation
  and ~20% latency regression in the `editor_shaping_multi_caret` scenario).
- **Editor** — multi-caret redo keeps secondary carets and column; a caret
  round-trip breaks the typing-coalesce run; multi-caret copy/cut/paste aggregate
  every selection VSCode-style; snippet placeholder shifts stay in sync across
  delete/choice and lone-CR bodies; assorted caret, selection, and buffer-integrity
  fixes across the bug-hunt passes.
- **Persistence & patches** — patch/diff desync and corruption, and
  session/persistence data-loss edge cases.
- **Terminal** — scrollback, alt-screen, and VT-parsing correctness fixes; safe
  child fork/shutdown.
- **LSP/debugger** — LSP state drift, DAP hang/shutdown races, and line
  breakpoints that now shift correctly on edit.
- **Settings** — integer writes clamp to each spec's declared range at store time.

### Internal

- **Perf harness hardening.** Fixed a selection bug where a bare full
  `microide_perf` run aborted on an opt-in (`run_by_default = false`) scenario
  instead of skipping it; added per-scenario warmup iterations; and made the
  `search_first_result` scenario deterministic (it previously snapshotted a racing
  mid-search state, swinging ~80× run to run). Project search grew an optional
  worker-count cap (`MICROIDE_SEARCH_WORKER_LIMIT`, unset in production) and a
  non-consuming `WorkerFinished` completion signal so the harness can measure a
  settled, single-worker search reproducibly.

## [2.6.6] - 2026-07-08

An **LSP feature + performance** cycle on top of 2.6.5. It rounds out the built-in
language-server integration with a batch of new capabilities, moves the provider
model to a concurrent LSP-primary merge, and takes the per-keystroke serialization
work off the UI thread — all on top of a broad internal consolidation of the LSP
client.

### Added

- **Inlay hints** — mid-line virtual text (parameter names, inferred types) rendered
  in the editor grid, pulled on open/save/clean-undo (never per keystroke) and gated
  on `editor.inlay_hints.enabled` plus the server's `inlayHintProvider`.
- **Signature help** (`textDocument/signatureHelp`) surfaced into the caret-anchored
  popup as an LSP-primary source.
- **Project-wide symbol search** (`workspace/symbol`) via a `workspace-symbol <query>`
  command that renders navigable results into an output channel.
- **Go to type definition / implementation / declaration** for the symbol under the
  cursor.
- **Prepare-rename refinement** (the server's placeholder seeds the rename prompt) and
  **range formatting** (format the current selection, else the whole document).
- **Rename across unopened files** applies silently on disk (VSCode-style), and
  server-initiated `workspace/applyEdit` is now wired end to end.

### Changed

- **Concurrent LSP-primary providers.** Completion, code actions, go-to-definition, and
  find-references now fire the plugin worker and the language server at the same time
  and merge LSP-first, replacing the previous serial plugin-first path. Signature help
  and navigation are LSP-primary too.
- **Serialization is off the UI thread.** JSON-RPC message serialization — and the
  whole-document copy of a full-sync `didChange` — now runs on the per-server I/O
  thread instead of the calling thread, removing roughly 0.25–1 ms of per-keystroke
  UI-thread work on large files served over the utf-16 position encoding.
- **Fewer main-thread copies** parsing completion and code-action results (strings are
  moved out of the decoded response rather than copied).

### Internal

- Broad consolidation of the LSP client: the transport header was split into focused
  translation units, the `Content-Length` wire codec was extracted into a unit-tested
  `LspMessageFramer`, trace/tuning constants were hoisted, and repeated sync/guard and
  test-stub-dispatch boilerplate was deduplicated. Adds an opt-in real-clangd
  end-to-end harness and a deterministic `didChange`-serialization microbench. Clean
  under ASan/UBSan/TSan.

## [2.6.5] - 2026-07-05

### Added
- Shared overview-ruler lane painted left of the vertical scrollbar, used by the
  editor, compare, and merge surfaces to show diff/merge changes, search matches,
  diagnostics, and the caret at a glance. Enabled by
  `editor.view.overview_ruler.enabled` (default on).

### Fixed
- Reserve the overview-lane gutter in editor metrics so a long line's trailing
  glyphs never lay out under the lane.
- Rebuild baked-in overview-ruler marker colors on a live colorscheme switch
  (folded a theme token into the marker cache keys) instead of leaving them stale.
- Deterministic tie-break when equal-priority markers contend for a pixel
  (first writer wins by input order, not palette-insertion order).

## [2.6.4] - 2026-07-05

A **correctness and hardening** cycle on top of 2.6.3. It pairs a broad round of
user-visible bug fixes across editor, compare/merge, git, LSP/DAP, and plugin
surfaces with a continued defensive-caps sweep.

### Changed

- **Default keybindings aligned with VSCode.** The file finder now opens with
  `Ctrl+P` (was F6), the sidebar toggles with `Ctrl+B` (was F8), "add cursor at
  all matches" is `Ctrl+Shift+L` (was `Ctrl+Alt+L`, now matching the palette
  hint), and "go to line" gains `Ctrl+G`. The two freed function keys move to the
  debugger — `F6` = Pause (VSCode's default) and `F8` = Start Debugging — filling
  the gap where F5 only *continued* an already-paused session. `Ctrl+G` /
  "Go to Line…" now opens a dedicated single-line modal (it previously did
  nothing); typing `goto <line>` in the command palette still works too.

### Fixed

- **Compare/merge:** compare panes render on the cell grid so caret, selection, and
  text stay aligned; visible row layouts are cached outside the render loop. Merge
  "Mark Resolved" self-rejection and CRLF conflict-marker parsing fixed.
- **Editor:** fold gutter spacing corrected for wide line numbers; stale fold model
  after undo/redo and `tabmove +N` fixed; multi-caret paste remap desync on lone-CR /
  reversed line endings fixed; syntax highlighting carries the open-region stack across
  blank and over-long lines.
- **Git:** data loss when discarding or unstaging a staged rename fixed; sidebar discard
  validated against the confirmed path rather than a stale index.
- **LSP/DAP:** LSP shutdown deadlock, document leak, and stranded requests fixed; DAP
  scope overflow fixed.
- **Plugins:** plugin worker hang and control-channel reply race fixed.
- **Settings:** steppers use the real ranges for scrollback, blink, autosave, and scale.

### Security / resilience hardening

- Additional allocation and length caps: terminal paste payloads, output-channel
  retained bytes, control-socket connections, DAP protocol arrays, and terminal
  tabulation repeats.
- Hardened JSON/number parsing, persisted-record writes, and CR line-ending file loads.
- Tenth-round resilience sweep: git-v2 crash, DAP list flooding, terminal reply-write stall.

### Performance

- Faster JSON string parse/serialize on the LSP/DAP hot path; render hot-path allocations
  removed; plugin buffer open/save/close dispatch gated on subscriber interest.

## [2.6.3] - 2026-07-04

A **resilience-hardening** release on top of 2.6.2. It lands a multi-round
white-box pentest sweep: adversarial and pathological inputs across the
terminal, editor, compare/merge, git, filesystem-watch, LSP/DAP, and plugin
surfaces are now bounded by allocation caps, recursion-depth guards, and
list-length limits so malformed or hostile data degrades gracefully instead
of exhausting memory or stalling a thread. Changes are purely defensive —
no persisted-format or plugin-API breaks, and no user-visible workflow
changes beyond one new opt-in terminal-clipboard setting.

### Security / resilience hardening
- Terminal: cap allocation-driving CSI operations (`L`/`X`/`@`, cursor moves)
  and cursor-down to screen/width bounds; guard the CSI parser against
  oversized parameters; strip embedded `ESC[201~` end-markers from bracketed
  paste to close a paste-injection vector.
- OSC 52 terminal clipboard writes are now gated behind a new opt-in setting
  (**off by default**), so a remote program can no longer silently overwrite
  the system clipboard.
- Editor: bound per-gesture caret spans and enforce a byte budget on the undo
  history so pathological edits can't grow it without limit.
- Compare/merge: skip intra-line span refinement past a 64 KiB line budget and
  cap the anchored-fallback diff recursion at depth 256 with a correct coarse
  fallback.
- Git: cap git-status entry counts and other parser outputs; add a symlink-loop
  guard for tree traversal; bound patch generation against oversized inputs.
- Filesystem watch: bound the FileIndexWatcher inotify watch count (with clean
  partial-tree degradation) and cap filesystem-event floods.
- LSP/DAP/plugin/console: cap protocol and UI list lengths so a flooding peer
  or plugin can't overwhelm the host; bound plugin registration counts.
- Render: truncate over-long text to a bounded length before surface sizing to
  cap a render-thread allocation, and validate display-list Text-op offsets
  unconditionally.

### Performance
- git-diff backend deep pass: reduce parse allocations (NUL-delimited split
  helper replacing `std::stringstream`), tighten the recompute gate, and fix
  patch-generation paths.

### Fixes
- Balance the background-task counter on a synchronous git sidebar refresh so
  the in-flight task count no longer leaks.
- Walk back over-aggressive caps from the hardening rounds where an earlier
  ceiling clipped legitimate input (code-review follow-up).

### Testing
- Extensive new regression coverage for the caps and guards above, plus new
  fuzz harnesses and seed corpora for JSON parsing, search-regex
  (catastrophic-backtracking seeds), and the terminal CSI parser.

## [2.6.2] - 2026-07-03

A small **workspace-chrome** release on top of 2.6.1. The project tab strip
now hides itself while a single project (or none) is open, so the common
single-project workspace gains a row of vertical space; the strip reappears
the moment a second project opens. No persisted-format or plugin-API breaks.

### Workspace chrome
- New `chrome.project_tabs.hide_when_single` user setting (**on by default**):
  with one project or none open, the project tab strip collapses to a
  zero-height rect that suppresses both its render and its hit-testing.
- `ComputeLayout` gains a `project_tab_strip_visible` input so the render and
  hit-test paths agree on the collapsed geometry, and the render-chrome path
  skips all tab measuring, overflow controls, and drag-ghost work when the
  strip is hidden.

### Performance
- Cache the `ProjectTabStripVisible()` predicate: it runs inside the uncached
  `ComputeLayout` on the per-mouse-move window-drag hit-test path, so the
  settings lookup is now memoized and re-resolved only on a settings-store
  revision bump. On `perf-runner-v1` the predicate drops ~21 ns → ~0.8 ns per
  call and the full uncached layout recompute ~51 ns → ~31 ns per hit-test,
  allocation-neutral. Adds an advisory `project_tab_strip_layout_hittest`
  micro-benchmark as a regression guard.

## [2.6.1] - 2026-07-03

A **font-picker polish** release on top of 2.6.0. The installed-font
dropdown becomes properly scrollable, collapses weight/style variants into
single family names, and enables fontconfig by default so enumeration and
resolution use real system font families. No persisted-format or plugin-API
breaks.

### Settings / Font picker
- The font-family dropdown (editor + terminal) is now scrollable: explicit
  scroll offset with clamping, a real scrollbar when the list overflows, and
  mouse-wheel routing when the pointer is over the dropdown.
- Font enumeration deduplicates weight/style variants into one family name.
- fontconfig is enabled by default in the canonical build environments, so
  installed fonts are listed with real, weight-deduped family names.

### Fixes
- Harden the no-fontconfig fallback to strip trailing style tails (including
  abbreviations/concatenations like `BdIta`, `BoldOblique`) so variants
  collapse to their family.
- Fix `ResolveFamilyToFile`: `FcFontMatch` always returns a default font, so a
  garbage family silently switched to it; only accept a match whose font
  advertises the requested family, keeping unresolved families a no-op.

## [2.6.0] - 2026-07-03

A **settings & tabs** release on top of 2.5.2. The settings surface is
overhauled end to end, gains an installed-font picker, and tabs now reorder
with a Chrome-like sliding animation. No persisted-format or plugin-API breaks.

### Settings
- Full settings overhaul: previously inert settings are wired to live
  behavior, string values are editable inline, and settings can be saved as
  the default.
- Installed-font picker backed by a native file dialog, with portable font
  resolution.
- Two high-value deferred settings wired: autosave delay and terminal font.
- Setting descriptions wrap instead of being clipped with an ellipsis.

### Editor / Tabs
- Chrome-like sliding tab reordering.

### Fixes
- Fix a use-after-free where the font-family value painted freed heap memory.
- Stop the row description overlapping the scope label in the settings list.
- Live-apply font, theme, terminal, autosave, and gutter settings; resolve the
  settings-review and high-effort code-review findings on the overhaul.

## [2.5.2] - 2026-07-02

A **performance & internals** release on top of 2.5.1. The compare/merge
surfaces and sidebar shed per-frame work, the release binary now ships with
LTO, and a broad refactor collapses the workspace provider registries behind a
single generic. No persisted-format or plugin-API breaks.

### Performance
- Compare/merge rendering no longer allocates per visible row: both surfaces
  render through reused scratch `DecoratedTextRow` members and truncate hot
  labels allocation-free, and their render TUs are now under the render lint set
  that enforces it.
- Sidebar project-search status line is cached in the view model instead of
  being rebuilt every frame.

### Build
- The release `.deb` now ships with LTO (`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`),
  matching the LTO codegen the perf gate already measures.

### Internal
- Collapse seven per-kind workspace provider registries into a single generic
  `ProviderRegistry<Spec>`.
- Default `operator==` for the compare/merge hover state.

### Performance baselines
- Refresh committed perf baselines to reflect the compare/merge render and
  sidebar status wins; the maintainer workstation is now the designated
  `perf-runner-v1` host for authoritative rebaselines.

## [2.5.1] - 2026-07-01

A **performance & internals** patch on top of 2.5.0. Startup and teardown get
lighter, editor hot paths shed allocations, and a broad dedup pass consolidates
buffer-renderer helpers. No persisted-format or plugin-API breaks; perf baselines
were refreshed to match.

### Performance
- Faster startup: syntax highlighting compiles its per-definition rules lazily on
  first use instead of eagerly at load.
- Lighter teardown: redundant work on buffer, project, and application close is
  trimmed.
- Fewer hot-path allocations across the editor and render paths, plus path-handling
  dedup, from the continued deep-review passes.

### Fixes
- Action availability: gate the `debug-run` (Start Debugging by program) command
  on `debug.enabled` like the other debug-launch actions, so it no longer appears
  enabled while the debugger is off.

### Internal
- Unify buffer-renderer batching, caret, whitespace, and gutter helpers behind
  shared code paths.
- Multi-caret and soft-wrap speed and correctness pass.
- Fix a latent out-of-bounds access surfaced during deep review.
- Silence the two remaining build warnings (unhandled `DebugRun` switch case;
  a `std::filesystem::path` copy in a test loop).

### Performance baselines
- Refresh committed perf baselines under `tests/perf/baselines/` to reflect the
  startup/teardown/render optimizations shipped since 2.5.0.

## [2.5.0] - 2026-06-30

A **UI, session & packaging** release on top of 2.4.1: seamless session
restore, friendlier tab context menus, a runtime window icon, sharper resize
affordances, and a Nix flake for reproducible builds. The session format gains
additive fields only — old session files still load — and there are no
plugin-API breaks.

### Session
- Seamless restore: per-buffer scroll position is now honored on reopen instead
  of snapping back to the caret, via an authoritative
  `TextViewport::ApplyRestoredViewState` (cursor/selection first, scroll last).
- File-tree state is now persisted and restored: expanded/collapsed folders,
  the selected node, sidebar scroll row, and the active sidebar view. New
  session-record fields are additive (tags 15–19); older session files decode
  with empty tree state.

### UI
- Tab context menus now lead with copy-path and trail with close, putting the
  common action first.
- The application window icon is set at runtime via `SDL_SetWindowIcon`.
- Precise resize handles with consistent mouse hover affordances across the
  shell.

### Packaging
- Add a Nix flake for reproducible build, run, dev shell, and test.

## [2.4.1] - 2026-06-29

A small **fixes & presentation** patch on top of 2.4.0. The git sidebar's
per-row actions move to a right-click context menu, cursor-shape changes now
show on idle Wayland compositors, and the repository gains a generated showcase
gallery (screenshots + hero demo video) surfaced from the README. No
persisted-format or plugin-API breaks.

### Fixes
- Git: replace the inline per-row buttons in the sidebar with a right-click
  context menu, decluttering the change list.
- Cursor: surface cursor-shape changes on idle Wayland compositors that
  previously coalesced the update away.

### Docs & media
- Ship a generated showcase gallery and hero demo video under `docs/media/`,
  produced by `tools/capture-media.sh` and regenerated every release.
- Embed the hero shot in the README and point the former "no screenshots" notes
  at the [project site](https://pablojimenezmateo.github.io/microide/).

## [2.4.0] - 2026-06-29

A **performance & correctness** cycle. The bulk of this release is a sustained
deep-review pass that drives allocation out of the editor, render, and terminal
hot paths, swaps the document model to a piece tree for fast mid-file edits, and
gates the glyph atlas on the GPU renderer. The debugger is surfaced as a
first-class feature and several correctness defects (LSP encoding, whitespace
diffing, stale completions) are fixed. No persisted-format or plugin-API breaks.

### Editor & debugger
- Debugger is now a first-class feature surfaced in the shell (`PluginHost`
  decomposed along the way).
- Project-scoped editor font size, persisted per project.
- Document storage routed behind an `editor::TextBuffer` seam, now backed by a
  **piece tree** that beats the previous vector model on mid-file edits; large
  files load directly past the old split/rejoin round-trip.

### CLI
- New `--version` / `-V` flag prints the version (`microide <x.y.z>`) and exits.

### Fixes
- LSP: negotiate UTF-8 position encoding (correct multi-byte column mapping).
- Compare: honor `ignore_whitespace` inside changed hunks.
- Assist: drop stale LSP completion / code-action responses.
- Plugins: bound provider-query harvest loops to the raw array spine.
- App: map the startup window once to kill the black-flash double-popup.

### Performance
- Allocation-free editor, render, and terminal hot paths (search-match cache,
  multi-caret undo capture, per-row layout, SGR parsing on the reader thread).
- Incremental buffer-local find and bounded-head highlight reads — no whole-doc
  snapshots; off-thread checkpoint backfill removes the first-paint syntax freeze.
- GPU-gated, row/gutter-batched glyph atlas; glyph texture cache bounded by a VRAM
  budget; render gates on the reported SDL renderer backend.
- Skip building the git sidebar view model when hidden; O(1) file-finder recents
  and narrowed candidate set on forward typing; caret-window-only blame snapshots.
- Coalesce search wakes; bound `RunSubprocess` with an optional timeout and cap
  format-on-save; copy trace labels only when tracing is enabled.

### Build & tooling
- Shared `microide_core` object library plus a shared PCH (~15% faster test
  build); ccache / ld.lld / split-dwarf auto-used when present.
- Untrack regenerable perf fixtures and harden `.gitignore`; advisory GPU renderer
  lane and sustained-scroll scenario added to the perf harness.

## [2.3.0] - 2026-06-27

Substantially widens the **plugin rendering surface**. Plugins now move onto a dedicated worker
thread (off the UI thread) and contribute rich editor and presentation content under a strict
host-renders-data model: plugins emit size-capped, validated *data* and the host owns all drawing,
input, and lifecycle. Every new surface is zero-cost when unused.

### Plugins
- **Editor decorations** (`ctx.decorations`): per-file text styles (recolor, background, underline,
  strikethrough, bold/italic, whole-line), gutter marks (built-in icon shapes with color/priority),
  end-of-line inline text (Error Lens / blame style), and clickable code lenses (end-of-line or an
  above-line inset strip via `plugins.code_lens_above`).
- **Content surfaces** (`ctx.surface`): standalone charts/previews drawn from a structured display
  list (`rect` / `line` / `polyline` / `text` / clip ops) or a raster image (PNG/JPEG/RGBA8, decoded
  off-thread and texture-cached), shown in a bottom/side panel tab or anchored inline (experimental,
  gated by `plugins.inline_surfaces`), with clickable hit regions that dispatch host commands.
- **Ghost-text inline suggestions** (`ctx.editor.set_ghost_text` / `clear_ghost_text`): Copilot-style
  caret-anchored dimmed proposals with host-owned lifecycle (Tab accepts, Esc dismisses); gated by
  `plugins.ghost_text`.
- **Host-owned buffer edits** (`ctx.editor.apply_edits`) plus **reactive editor events**
  (`on_buffer_change`, `on_cursor_move`, `on_selection_change`, `on_buffer_close`) for live linting
  and paired-edit workflows.
- **Language providers**: plugin-native go-to-definition, find-references, signature help, and
  document symbols (`ctx.definition` / `ctx.references` / `ctx.signature_help` /
  `ctx.document_symbols`) alongside LSP.
- **Presentation contributions**: color themes (`ctx.themes`), file-icon themes (`ctx.file_icons`),
  and rich status items with icon, tone, click command, and live progress (`ctx.status`).
- **Tree sidebars**: sidebar snapshots may return collapsible nodes (`depth` / `collapsible` /
  `collapsed`) with plugin-owned expand/collapse state via `on_toggle`.
- Execution model: all `lua_State` access runs on a dedicated plugin worker thread behind a
  UI-owned snapshot/mailbox boundary, with a per-call watchdog and non-blocking reload.
- New repo-owned dogfood plugins: `eol-annotations`, `surface-preview`, `presentation-demo`,
  `language-tools` (plus `todo-highlight`), each exercising the same narrow host APIs as user plugins.

## [2.2.0] - 2026-06-23

Focused follow-up to the 2.1.0 navigation work. The **welcome surface** becomes
state-aware, the **command palette** absorbs the standalone command prompt to
become the single command surface, and a theme-switch repaint bug is fixed.

### Shell & navigation
- The welcome surface is now state-aware: a cold-start variant (Open Folder +
  recent projects) when no project is open, and a project-home variant (project
  name, recent files, New File / Open File / Find in Project) when a project is
  open with no editor tab. The duplicated command-palette hint is removed.
- The command palette (Ctrl+Shift+P) is now the single command surface: its
  query doubles as a command line, so queries with arguments or no fuzzy match
  run through the shared command-line executor (e.g. `colorscheme dark`), and
  Tab completes command/path tokens. The separate Ctrl+E command prompt and its
  bottom-panel command-mode UI are retired; the native-picker fallback opens the
  palette pre-filled instead.
- Editor tabs gain a "Copy Absolute Path" action.

### Theming
- Switching color theme via the command palette or keybinding (`toggle-theme` /
  `colorscheme <name>`) now repaints the whole window immediately, instead of
  leaving stale colors until the next unrelated event.

### Internal & tests
- Retire the lingering "command prompt" naming across the executor coordinator,
  state, and test accessors now that the surface is gone.
- Make the real-gdb function-breakpoint E2E test deterministic under CPU load,
  and fix a flaky `ProjectBackgroundExecutor` shutdown test.

## [2.1.0] - 2026-06-22

Feature release focused on **multi-view editing** and **navigation**. Editor tabs can now be
split into independent editor groups (right/down) with per-group tab strips, group-aware input,
and session persistence. A new searchable **command palette** (Ctrl+Shift+P) and **recent
projects/files** tracking make navigation faster, and the **welcome screen** is rebuilt into a
data-driven home surface. Rounding it out: a built-in **light theme**, a themed app icon, and
reverse-debugging support in the DAP integration.

### Editor groups & splitting
- Collapse legacy in-tab splits to a single viewport and introduce a first-class `EditorGroup`
  model, with per-group layout, render, and tab strips.
- Split/focus/close commands operate on editor groups, with group-aware keyboard input routed to
  the focused group.
- Split right / split down available from both the tab context menu and the file-tree context
  menu.
- Editor groups are persisted in session state and restored on reopen.

### Welcome / home surface
- Overhaul the welcome screen into a data-driven home surface with a bold single-card layout and
  fixed empty-state overlap.
- Recents on the home surface are clickable and correct, with a hand cursor and no color halo.

### Navigation & discovery
- Add a searchable command palette overlay (Ctrl+Shift+P).
- Track recent projects/files (MRU) and surface them in the file finder, backed by a new
  persistence record.

### Theming & branding
- Add a built-in light theme and a stronger selection focus bar.
- Add a themed two-tone "m" application icon with a hicolor multi-size icon set.

### Debugger
- Handle a late DAP capabilities event so reverse debugging is recognized when the adapter
  reports it after launch.

### Fixes
- Resolve the per-pane group viewport in split hit-test paths so clicks land in the correct
  group.

### Performance
- Dedup editor-group hot paths and harden group accessors.
- Cache the normalized focused path for per-pane path matching in the debug pane.

### Docs & tests
- Add a GitHub Pages showcase site for microide.
- De-flake fixed-wait timing races in search/subprocess tests and the control-socket self-heal
  test.

## [2.0.1] - 2026-06-20

Patch release adding agent-driven **review verbs** to the control channel. Three new commands
bulk-open the diff/merge tabs needed to review changes, switching to the Source Control view,
deduping against already-open tabs, and cleaning stale (clean) review tabs while preserving any
dirty/edited ones.

### Control channel
- `review-conflicts` — open one merge tab per conflicted working-tree file (non-mutating; pair it
  with your own `git merge`).
- `review-branch [ref]` — open one compare tab (working tree vs `ref`) per differing file; `ref` is
  any commit-ish and defaults to the repo base branch.
- `review-commit [commit]` — open one compare tab (`commit~1` vs `commit`) per file the commit
  changed; defaults to `HEAD`, accepts any commit hash.
- Verbs, recipes, and the generated man page document the new workflows; tab reconciliation is a
  pure, tested `ComputeReviewTabPlan` driven by a host-owned `ReviewSessionCoordinator`.

### Fixes
- Fix a stack-use-after-scope in the merge-tab conflict classifier (`string_view`s were bound to
  `SerializeLines` temporaries), caught by the new AddressSanitizer coverage.
- `tools/run-checks.sh` now folds sanitizer runtime reports into the main log so failures are
  captured in one place.

## [2.0.0] - 2026-06-20

Major release introducing an integrated **debugger**. microide now speaks the Debug Adapter
Protocol (DAP) end to end — breakpoints, stepping, call stacks, variable inspection, watches,
and multi-session debugging are first-class host surfaces, with gdb 17.2 wired up via a bundled
plugin. This release also adds an external **control channel** for headless and agent-driven
operation, plus a round of hot-path performance work. The 1.3.1 rendering fix below is included.

### Debugger (DAP)
- Host-owned DAP protocol client and `DebugSession` / `DapManager` / `DebugService` core, with a
  `ctx.debug.add` plugin seam and a bundled `gdb-dap` plugin for gdb 17.2.
- `debug.enabled` toggle, Start/Stop Debugging, and an always-visible Debug menu.
- Breakpoints with persistence: MATLAB-style gutter (yellow conditional, hollow disabled),
  conditional / hit-count / logpoint breakpoints, function breakpoints, and exception filters
  with conditions.
- Execution control: continue / step over / step into / step out, plus capability-gated reverse
  execution (`reverseContinue` / `stepBack`).
- Stopped-event handling with call stack, multi-thread support, and a session switcher for
  multiple concurrent sessions; restart and Stop All Sessions.
- Variables / Scopes panel with `setVariable`, a richer value tree, Locals open by default, and
  hover-to-inspect via `evaluate`.
- Debug surfaces moved to a dedicated right-side pane: debug toolbar, watch panel, structured
  console REPL, launch-config picker, and precise pane hitboxes with in-buffer cursors.
- Robustness: launch-ordering fixes for gdb 17.2, async stale-apply guards, breakpoint
  verification feedback, dead-adapter teardown, and a `terminated` broadcast on every session end.

### Control channel
- External control channel over a live AF_UNIX socket plus a cold-start `--control-spec` path.
- Headless, deterministic, observable agent-driving entry point, with a one-shot control-send
  client and an agent-drivable debug runbook.
- Notification toasts wired to real shell events.

### Performance
- O(1) settings store and a debug-off fast path.
- Dropped per-key allocations, redundant probes, and duplicate work across the DAP/LSP JSON paths.
- Killed hot-path allocations in editor/project and git paths; deduped transparent hashing.

### Fixes
- Stop a large-tree project file monitor from freezing the UI.
- Render caret and selection in the launch-config picker query field.
- Snap glyph-texture origins onto the physical pixel grid so text stays crisp under fractional
  display scaling (also released as 1.3.1 below).

## [1.3.1] - 2026-06-17

Patch release fixing blurry text on centered overlays under fractional display scaling.

### Rendering
- Snap glyph-texture origins onto the physical pixel grid so NEAREST-sampled text stays 1:1 with
  the device under fractional display scales (e.g. 125%). Fixes soft/blurry glyphs on the
  Help/About and Settings overlays; the editor (already grid-aligned) and integer scales are
  unchanged.

## [1.3.0] - 2026-06-17

Closes out the remaining open editor/folding/project topics, then does a deep documentation pass:
the closed tech-debt history is moved out of the known-debt journal into a dated archive, and the
public-facing and dev docs are refreshed.

### Editor
- Multi-caret brace-split on newline: pressing Enter now fans the single-caret brace-split geometry
  across every caret.
- Stop bogus fold markers on Markdown prose.

### LSP & Project
- Keep the language server warm across project-tab switches.
- Unblock project switch/open stalls.

### Internal
- Architecture size caps now count source lines (SLOC), with the duplicated line counters deduped;
  fix a headless-test flake.
- Docs: archive the closed tech-debt history under `guidelines/tech-debt/archive/` and trim the
  known-debt journal to open items only; refresh README, ROADMAP, active-work, and the release
  checklist; repoint references that named now-archived debt sections.

## [1.2.1] - 2026-06-16

Incremental release building on 1.2.0 with a file-tree convenience action and the standard release
procedure committed to the repo.

### Editor
- Add a "Show in File Explorer" file-tree context-menu action.

### Internal
- Docs: add the mandatory standard release procedure to the release checklist.

## [1.2.0] - 2026-06-16

Builds on 1.1.1 with a per-plugin capability sandbox, kernel-confined language servers, a
color-independent glyph-cell render path, and a round of cross-subsystem correctness and footprint
work.

### Plugins
- Enforce a per-plugin capability sandbox so contributed code runs against an explicit grant set.
- Kernel-confine contributed language-server processes.

### Rendering
- Add a color-independent ASCII glyph-cell atlas on the composite-on-miss path.

### Terminal
- Close deferred terminal debt: T3 split, T5a move-swap, and an output fuzzer.

### Internal
- Deep pass: dedup, correctness, and footprint improvements across render, app, util, and terminal.
- Add a headless Initialize/Render/Shutdown app lifecycle test.
- Docs: drop experimental status, retire the Git Workstation "Preview" naming, archive the
  `expand-git-diff-merge-perf-gates` change, and close R5a glyph-cell atlas tech debt.

## [1.1.1] - 2026-06-15

Incremental release building on 1.1.0: a centralized LSP backbone with more bundled language
servers, host-owned notifications, plugin enable/disable persistence, and shell polish.

### LSP
- Centralize the JSON-RPC codec, extract a dedicated `LspService`, and route all server traffic
  through a single I/O thread.
- Add clangd and .NET server enablers plus bundled `cpp-lsp` and `dotnet-lsp` plugins; refresh the
  `typescript-lsp` plugin.

### Notifications
- Add a host-owned `NotificationService` for transient, auto-dismissing toast messages that
  built-in code and plugins can post; the shell schedules a single wake at the next expiry rather
  than polling.

### Plugins
- Decompose provider registration into focused registration parsers and query interop (remove
  `PluginLuaProviderRegistrationInterop`).
- Persist per-plugin enable/disable state (`disabled_plugin_ids`) across sessions, surfaced in the
  Settings overlay.

### UI & Shell
- Settings: opaque selection highlight so the editor no longer ghosts through the overlay.

## [1.1.0] - 2026-06-14

First tagged release. Builds on the 1.0.0 baseline with editor, diff/merge, git, search, save,
and shell improvements.

### Editor
- Multi-caret position remap with region-stack highlighting and smarter copy-with-context.
- Coalesce typed runs into word-level undo steps.
- Suppress occurrence highlight while actively typing; centralize caret moves.
- Correct soft-wrap caret row resolution and fold/scroll spans; add hanging indent.
- Resolve folds for visible long methods and unify the fold marker as a single `+`/`-` button.

### Diff & Merge
- Unify decorated-row assembly across editor, compare, and merge surfaces.
- Centralize intra-line underline assembly and git conflict-marker / collapsed-run helpers.
- Speed pass: intern diff lines as `string_view` in compare model build; drop dead tokenization
  in exact line-ops; large-file fallback round-trip coverage.
- Unblock horizontal scroll and stop the change marker overlapping line numbers.

### Git
- Restructure the source-control panel and add a branch/commit ref picker.
- Make the commit message editable; improve the source tree and commit flow.

### Search
- Parallelize project search with count-all and match highlighting.

### Save
- Durable writes, save-time conflict guard, and a non-blocking external-change banner.

### UI & Shell
- Deferred-commit tab drag with ghost and consistent behavior across all three tab types.
- Two-pane Settings overlay redesign; collapse the Help menu.
- Simplify the top menu bar and dedup it against Settings; replace a dropdown with dedicated buttons.
- Centralize chrome primitives and share single-line input behaviors.
- Wrap Help/About text and add a Settings/Help overlay scrollbar.
- Keep tree focus when opening a file from the sidebar; detach project-search results scroll from
  the active result.

## [1.0.0]

Baseline native single-window IDE shell (bumped from 0.1.0, not separately tagged): built-in
editor with multi-project/file tabs and shared-buffer splits, compare and three-way merge tabs,
git sidebar with staging and commit, async project search and file finder, PTY-backed terminal
tabs, and a Lua 5.4 plugin runtime with host-owned registries.
