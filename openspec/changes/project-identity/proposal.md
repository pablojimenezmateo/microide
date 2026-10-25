## Why

A project is identified by a filesystem path, and for a remote project that path is
the mirror's tree: `~/.local/share/microide/remote/<host>/<name>-<hash>/tree`. Every
presentation site labels a project with `ProjectLabelForRoot(root)`, which is the
path's last component, so a remote project's tab, tab badge, window title
(`main.cpp — tree — microide`) and the prompts that name the project ("Discard all
… changes in tree?") all say **tree**, and the tab tooltip shows the mirror's
directory under `~/.local/share`. Only the sites that remembered to call
`RemoteProject::ReadRecord` (the recents list, `RemoteHostService`'s own messages,
the control channel's `host`/`hostRoot`) show `host:/path`. This is the leak
`dev-docs/design/remote-projects.md` § 6.8 predicted, and why it scheduled
Groundwork **G8** (`ProjectId`), the one groundwork item that never started.

Whether a project is remote is also worked out after the fact rather than stored.
A restored session or a recents entry gives only a path. The project is found to be
remote when it is *activated*: `LocalityForMirror` reads `meta/remote` beside the
tree. Until then a restored remote project in the catalog is local by default.
Separately, a recents entry whose mirror is gone (deleted by hand, or a wiped data
directory) cannot be reopened. All the entry holds is the deleted path, not the
host and folder it came from.

## What Changes

- **New**: `project::ProjectId`, a value type naming a project as
  `Local{path}` or `Remote{host, host_root}`. The filesystem root every I/O site
  uses stays a path and is *derived* from the id (a local path, or
  `RemoteProject::DefaultTree(host, host_root)`).
- The id is resolved **once, when a catalog entry is created** (open, session
  restore, recents), and stored on `ProjectWorkspaceState` beside `locality`. Like
  `locality`, it is identity and survives the scoped-state reset.
  `LocalityForMirror`'s activation-time sniff becomes a resolution from the id.
- **Presentation renders the id.** `ProjectLabelForRoot` is deleted. Its
  replacements take a `ProjectId`: a short label (`repo`, the host root's basename)
  and a full location (`host:/abs/path` for remote; the path for local). The six
  callers (project tab title, tab badge, window title, `ProjectLabel()` for
  prompts) and the tab tooltip move to them. A remote project never shows a
  mirror path, and that is guaranteed by the type rather than by each caller.
- **Persistence stores the id.** The workspace session's project roots and the MRU
  recent projects write a `ProjectId` record (**BREAKING** for the persisted format
  only in the accepted, additive way: an old path-only record reads as
  `Local(path)`, or as `Remote` when that path is a mirror tree whose `meta/remote`
  record names it). Older builds skip the new record per the compatibility rule.
- A remote recents entry whose mirror no longer exists stays listed and reopens by
  creating the mirror again (`Open Folder on Host` with the stored host and root),
  instead of being dropped. `Remote: Remove Mirror` still removes the entry.

## Capabilities

### New Capabilities
- `project-identity`: what names a project, when the name is fixed, how it is
  shown, and that a project is local or remote but never both.

### Modified Capabilities
- `persisted-state-format`: the workspace session and MRU state store project
  identities instead of bare root paths, and older path-only records read back
  without loss.

## Impact

- New `src/project/ProjectId.{h,cpp}` (kernel, SDL-free; unit tests in
  `microide_kernel_tests`).
- `src/workspace/state/WorkspaceProjectState.h` (`id` beside `locality`),
  `services/ProjectCatalogService`, `coordinators/WorkspaceProjectStateCoordinator`,
  `shell/WorkspaceShellProjects.cpp` (resolution replaces the sniff),
  `services/RemoteHostService` (`LocalityForMirror` → `LocalityFor(const ProjectId&)`).
- Presentation: `WorkspaceUiText.h`, `WorkspaceTabStripChrome.cpp`,
  `shell/WorkspaceShellPresentation.cpp`, `shell/WorkspaceShellPrompts.cpp`,
  `render/WorkspaceShellRenderFrame.cpp` (the label arrives precomposed; no new
  string work in the render TU).
- Persistence: `persistence/WorkspacePersistenceFormat.h`,
  `WorkspacePersistenceBinaryInternal.h` (new tags),
  `WorkspacePersistenceBinaryFormatSessions.cpp`, `RecentsService`,
  `WorkspacePersistenceCoordinatorWorkspaceSession.cpp`, `PersistedRecordDump`, and
  the `PersistedRecordReaderFuzz` corpus.
- No shell growth: no new `WorkspaceShell` member or companion TU.
  `WorkspaceShellMembers.inc` stays at or under its ratchet.
- Docs: `dev-docs/design/remote-projects.md` (§ 6.8 and the Groundwork table),
  `dev-docs/project/active-work.md` § 7.
- Size: ~450–600 production lines, ~400 test lines.
