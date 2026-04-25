# macOS Support Plan

Reviewed on 2026-04-25.

This document outlines what `microide` needs to support macOS as a first-class host.
It is a host-platform plan, not a compatibility promise.
The goal is a fast, correct macOS build with the same core editor, compare, merge, search, git,
plugin, and terminal workflows expected on Linux.

## Bottom Line

`microide` does not need a rendering rewrite to support macOS.
The SDL3 shell, text model, compare or merge flows, and most workspace rendering should carry over.

The actual work is in platform services and packaging:

- replace Linux-first or generic POSIX assumptions with explicit host-owned service seams
- make macOS app directories, file watching, subprocess launch, terminal integration, and URL or
  shell integration first-class
- add a real macOS bundle, launch, and test path instead of treating macOS as “Unix with SDL”

The two biggest implementation areas are:

1. terminal and subprocess services
2. native host integration and packaging

## Current State

The codebase is already partly portable:

- the product is built around SDL3 and CMake
- rendering is host-owned and mostly platform-agnostic
- app directories now resolve to macOS-correct `Application Support` and `Caches` roots through
  `src/platform/AppDirectories.*`
- trash, URL open, reveal-path, and runtime asset discovery now live in dedicated
  `src/platform/Trash.*`, `src/platform/HostIntegration.*`, and `src/platform/RuntimePaths.*`
- the project picker already goes through `SDL_ShowOpenFolderDialog`, which may work on macOS
  without a separate Cocoa implementation for the first pass

But several important services are still Linux-first or only loosely portable:

- `src/terminal/TerminalSession.cpp` is a POSIX PTY implementation built around `openpty`, `fork`,
  `ioctl`, process groups, and signal handling
- `src/platform/Subprocess.cpp` and `src/platform/AsyncSubprocess.cpp` are still direct
  `fork`/`execvp` implementations instead of a deliberate platform process layer
- `src/platform/FileWatcher.cpp` has Linux `inotify` wakeups and falls back to snapshot polling on
  non-Linux hosts
- there is no macOS-specific packaging, bundle metadata, signing, notarization, or launch
  validation path in the build
- secret storage is not yet tied to native OS credential storage, which is not a hard blocker for
  first macOS support but is a platform gap

## Success Criteria

macOS support should not be considered complete until all of these are true:

- `microide` builds and launches as a `.app` bundle on both Apple Silicon and Intel macOS hosts
- editor, compare, merge, search, git, sidebar, overlays, and plugin loading behave the same as on
  Linux
- terminal tabs work with correct PTY launch, resize, clipboard paste, and process shutdown
- file watching uses a native macOS watcher instead of snapshot polling for active projects and
  plugin assets
- app config, cache, state, and data directories resolve to correct macOS locations
- delete-to-trash uses a correct macOS flow
- open-folder, open-URL, and subprocess launch paths behave correctly from an app-bundle launch
- targeted tests run on macOS CI, and local launch instructions are documented

## Main Gaps

### 1. App directories are not macOS-correct

`src/platform/AppDirectories.cpp` currently maps non-Windows hosts to XDG-style directories under
`~/.config`, `~/.local/state`, `~/.local/share`, and `~/.cache`.
That is a Linux policy, not a macOS one.

macOS support needs explicit mappings for:

- config: `~/Library/Application Support/microide`
- state: `~/Library/Application Support/microide` or a nearby app-owned state location
- data: `~/Library/Application Support/microide`
- cache: `~/Library/Caches/microide`

If this remains wrong, settings, session data, plugin state, and caches will land in the wrong
places and the app will not feel native.

### 2. Terminal support is still “generic POSIX”, not a host service

`src/terminal/TerminalSession.cpp` likely works on macOS with some adaptation because macOS has
PTYs and `fork`, but that is not the real bar.
The real issue is that terminal launch, shutdown, resize, and child process control are still
embedded in one direct implementation rather than owned by a platform service.

macOS support needs:

- an explicit PTY or terminal backend interface
- a Darwin-backed implementation for session launch and lifecycle
- correct shell selection and environment initialization when launched from Finder or an app bundle
- validation for resize, alternate-screen apps, and process-group shutdown

If the app is launched from Finder, relying on terminal-like inherited environment variables will be
fragile. The terminal path needs a deliberate macOS launch contract.

### 3. Subprocess launch is not yet a real cross-platform service

`src/platform/Subprocess.cpp` and `src/platform/AsyncSubprocess.cpp` are still direct process code.
They should become a host process service with explicit platform implementations.

For macOS this matters because:

- app-bundle launches do not inherit the same shell environment as terminal launches
- tool discovery may need explicit environment setup
- plugin tool launch, git, formatter, and language-server launch all depend on this layer

The first macOS pass does not need a perfect shell-login recreation, but it does need one
consistent policy for:

- working directory
- inherited environment
- overrides
- PATH resolution for bundled versus system tools
- error reporting when executables are missing

### 4. File watching needs a native macOS backend

`src/platform/FileWatcher.cpp` currently has a Linux `inotify` backend and polling elsewhere.
Polling is acceptable as a temporary fallback, but it is not good enough for a first-class macOS
host.

macOS support should add a native watcher backend, likely via FSEvents or a similarly appropriate
Darwin mechanism, behind the existing watcher abstraction.

That backend needs to support:

- project tree refresh
- plugin asset watching
- directory creation, rename, delete, and move detection
- bounded wake traffic so large repos do not flood redraw or refresh work

### 5. Native host integration is incomplete

The shell currently leans on SDL for the open-folder dialog, which is acceptable for the first
slice if it works reliably on macOS.
But the app still needs explicit decisions for host integration points such as:

- browser or URL opening
- reveal-in-Finder
- bundle metadata
- app icon and bundle resources
- launch behavior outside a shell

This should become a small `src/platform/*` or `src/app/*` service layer, not scattered conditionals
in workspace code.

### 6. Packaging, signing, and release validation do not exist yet

Mac support is not done when the binary compiles.
We need a proper bundle and release path.

That includes:

- CMake support for generating a `.app` bundle
- `Info.plist`
- app icon assets
- embedded resource layout for fonts and any bundled runtime assets
- codesigning
- notarization
- release validation on clean macOS hosts

Without this, the app may build locally but still fail as a usable macOS product.

## Recommended Architecture Changes

The cleanest path is to make platform seams explicit and narrow.

### Platform services to add or tighten

- `AppDirectories`
  macOS-specific directory resolution instead of Linux XDG fallback
- `ProcessService`
  shared sync and async process launch contract, with Darwin and Linux implementations
- `TerminalBackend`
  PTY lifecycle contract separate from terminal screen parsing and rendering
- `FileWatcher`
  native backend selection per host
- `HostIntegrationService`
  open URL, reveal path in system file browser, native dialogs if SDL proves insufficient
- `SecretStorage`
  eventually route through Keychain on macOS, though this can follow the first bring-up

### Boundaries that should stay host-owned

- all rendering
- workspace orchestration
- terminal screen parsing and terminal rendering
- editor, compare, merge, and search behavior
- plugin registry and plugin UI contributions

This is not a reason to expose more `WorkspaceShell` state.
It is a reason to move more OS work behind narrow platform APIs.

## Implementation Plan

### Phase 1: Build and launch baseline

Goal: produce a local debug macOS app that launches and opens projects.

Work:

- add explicit macOS support in CMake where packaging or app-bundle metadata is needed
- introduce a `.app` bundle target with a minimal `Info.plist`
- validate SDL3 and optional `SDL3_ttf` discovery on macOS
- resolve runtime asset paths from inside an app bundle
- fix `AppDirectories` to return macOS-correct locations
- document local bring-up steps and required dependencies

Exit criteria:

- app launches on macOS from both terminal and Finder
- project open works
- settings and session files land in correct macOS directories

### Phase 2: Process and terminal platform layer

Goal: make subprocesses and terminal tabs reliable on macOS.

Work:

- split direct process management out of `Subprocess` and `AsyncSubprocess` into a clearer platform
  service contract
- split PTY session launch or lifecycle out of `TerminalSession` so the screen model and escape
  handling stay platform-neutral
- implement a Darwin PTY backend with correct resize, shutdown, and child reaping behavior
- define one explicit environment policy for shell, git, formatter, LSP, DAP, and plugin tool
  launches from an app bundle
- add tests around subprocess launch, terminal session startup, and resize behavior

Exit criteria:

- terminal tabs launch shells correctly on macOS
- git and plugin tool processes launch correctly from both terminal and Finder app starts
- targeted terminal and subprocess tests pass on macOS

### Phase 3: Native file watching and host integration

Goal: remove the biggest non-native behavior gaps.

Work:

- add a native macOS file-watcher backend
- validate repo refresh behavior in large projects
- add or tighten host services for open URL and reveal in Finder
- keep SDL folder dialog only if it proves reliable; otherwise add a host-owned native picker
- verify plugin asset watching and project tree refresh use the same watcher contract

Exit criteria:

- project and plugin asset watching are native and responsive on macOS
- common host actions work without shell-specific assumptions

### Phase 4: Packaging and distribution

Goal: make macOS support shippable.

Work:

- finalize bundle resources and resource lookup
- add codesigning and notarization steps
- verify clean-machine install and launch
- document release process and platform-specific troubleshooting

Exit criteria:

- signed and notarized app launches cleanly on supported macOS versions
- release checklist exists and is repeatable

## Testing Plan

macOS support should be validated in focused layers.

### Automated

- build `microide` and `microide_tests` on macOS CI
- run focused editor, workspace, project, terminal, and plugin-runtime tests
- add `AppDirectories` tests for macOS directory mapping
- add file-watcher tests around create, rename, delete, and move semantics
- add subprocess tests for cwd, stdout or stderr capture, exit codes, and missing-command failures
- keep terminal rendering and selection tests host-agnostic where possible, and add macOS-only
  launch or PTY lifecycle tests where necessary

### Manual

- launch from terminal
- launch from Finder
- open a repo and verify tree refresh
- edit, save, rename, and delete files
- move files to trash
- run git flows
- launch terminal tabs and resize them
- verify plugin loading and plugin asset reloads

## Non-Goals For The First Pass

These should not block initial macOS support:

- native macOS menu-bar integration
- native text rendering outside the existing SDL pipeline
- Keychain-backed secret storage
- macOS-specific UI styling
- App Store packaging

Those may become worthwhile later, but they are not required to ship a correct desktop build.

## Biggest Risks

- Finder-launched environment differences breaking git, shell, formatter, or plugin tool discovery
- terminal behavior working in simple cases but failing in resize, alternate-screen, or shutdown
  paths
- polling file watching remaining in place too long and causing stale project or plugin state
- resource lookup failures once the app is run from a bundle instead of the repo tree
- bundle signing and notarization work surfacing late

## Recommended First Slice

If we start this now, the highest-leverage first implementation slice is:

1. fix `AppDirectories` for macOS
2. add app-bundle resource resolution and a minimal `.app` target
3. split subprocess and PTY launch into clearer Darwin-capable platform services
4. validate launch, project open, git, and terminal flows from Finder and terminal starts

That sequence establishes whether macOS is merely missing packaging or whether host service seams
need deeper correction.
The current code suggests the answer is the latter, so platform-service cleanup should start
immediately.
