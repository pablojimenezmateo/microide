## Context

`ProjectWorkspaceState::root` is three things at once: the directory every I/O site
opens (~170 `.root` uses in `src/workspace`), the key of the catalog and of every
persisted list (`project_roots`, `recent_project_roots`, `PersistedRecentFile::project_root`,
the per-project state directory), and the thing presentation prints. For a local
project those coincide. For a remote one, the first two are the mirror tree, and
the third should be `host:/path`.

Today's state, read from the code on 2026-10-25:

- `ProjectLabelForRoot` (`WorkspaceUiText.h:42`) is `root.filename()`. Its six
  callers (`WorkspaceTabStripChrome.cpp:126`, `WorkspaceShellPresentation.cpp:115,123,137`,
  `WorkspaceShellPrompts.cpp:137`, `WorkspaceShellRenderFrame.cpp:673`) label a
  remote project `tree` (`MirrorStore`'s default tree name), and
  `ProjectTabTooltipLabel` prints the mirror path.
- The remote sites that are right each call `RemoteProject::ReadRecord(root)`
  themselves: `RecentsService::ExistingRecentProjects`, `RemoteHostService`'s
  messages, `ControlChannelService`'s `projects`/`status`.
- Locality is resolved at activation (`WorkspaceShellProjects.cpp:31-40`,
  `RemoteHostService::LocalityForMirror`): an explicit locality wins, otherwise the
  `meta/remote` record decides, and only for the mirror's own default tree (a copy
  of a mirror is a local folder). Session restore builds inactive catalog entries
  with a root and no locality (`WorkspacePersistenceCoordinatorWorkspaceSession.cpp:51`).

## Goals / Non-Goals

**Goals:**
- One value that names a project, fixed when its catalog entry is created, from
  which the I/O root, the locality and every user-visible label are derived.
- Make it impossible to label a remote project by its mirror path without
  noticing: no presentation helper takes a bare root.
- Persist the identity, so a remote recents/session entry survives its mirror.

**Non-Goals:**
- Changing the ~170 I/O uses of `.root`. The mirror tree *is* the filesystem root
  (design § 6.8), and they stay paths.
- Re-keying the per-project state directory (see Decisions).
- Re-keying breakpoints or `PersistedRecentFile::project_root`. They name files
  and filter by the active project's I/O root. Both are pure functions of the id.
- A control-channel change. `projects`/`status` already report `host`/`hostRoot`.
  They switch to reading the id rather than the record file, and their JSON is the
  same.

## Decisions

- **`ProjectId` is a closed variant in `src/project/`, not `src/workspace/`.**
  `struct ProjectId { struct Local { path root; }; struct Remote { std::string host; std::string host_root; }; std::variant<Local, Remote> location; }`
  with `operator==`, `IsRemote()`, `Label()` (short: the basename; the host root's
  basename for remote, `/` → the normalized path, as today), `Location()` (full:
  the path, or `host:/abs/path`) and `IoRoot()` (the path, or
  `RemoteProject::DefaultTree`). It lives in the SDL-free kernel because
  `RemoteProject` and the persistence format are kernel code. Its tests go in
  `microide_kernel_tests`. Rejected alternative: `std::optional<RemoteProjectRecord>`
  beside `root`. That keeps "local or remote" as two fields that can disagree,
  which is what § 6.13 asks the type to rule out.

- **Resolve once, at catalog-entry creation, through one function.**
  `ResolveProjectId(const path& io_root)` returns `Remote` exactly when
  `LocalityForMirror` would today (a `meta/remote` record *and* the path is that
  record's default tree), otherwise `Local`. It runs at the three entry points
  that create catalog entries: `OpenProjectTab`, session restore, and recents.
  An explicit open (`Open Folder on Host`) constructs the `Remote` id directly
  and never sniffs. The result is stored as `ProjectWorkspaceState::id`, carried
  through `ResetProjectScopedState` exactly like `locality`, and the activation
  path asks `RemoteHostService::LocalityFor(id)` instead of re-reading the record.
  Rejected alternative: resolve lazily at each presentation site. That is today's
  `ReadRecord` scatter, plus a file read per label per frame.

- **Catalog identity compares ids, and duplicate detection still compares I/O roots.**
  `OpenProjectTab` dedupes by `IoRoot()`. Two different ids cannot share an I/O
  root: a remote id's root is its default tree, and a local id equal to a mirror
  tree resolves `Remote` by the rule above. So the two comparisons agree. The
  invariant gets a unit test, not a second lookup.

- **Presentation takes the id, and `ProjectLabelForRoot` is deleted.** The compiler
  then finds every caller. `ProjectTabTooltipLabel` prints `Location()`. The window
  title's label is composed in frame prep from the active project's id. The render
  TU receives it as a `std::string_view` it already holds (`frame_state_`), so the
  hot-path string lint is unaffected. `ProjectStateDirectoryName` keeps taking the
  I/O root (next decision). No architecture lint is needed: nothing left takes a
  bare root, so the mistake has nowhere to compile. A lint would be the right
  tool only if a root-taking label helper were re-added, and review catches that.

- **The per-project state directory stays keyed by the I/O root.** § 6.8 wanted
  it keyed by the id "so `remote.mirror_root` can change without orphaning a
  project's settings". That setting was never built. The mirror path is a pure
  function of `(host, host_root)` and the data directory, so the key is already
  stable for everything the editor can do. Re-keying would cost a directory
  migration with a rename-on-first-open step, for a directory name (`tree-<hash>`)
  that no user sees. Revisit if the mirror root becomes configurable. This is
  written into § 6.8 so the reasoning does not have to be rediscovered.

- **Persisted form: a nested `ProjectIdRecord`, written alongside the legacy path
  tag during one release.** New tags `WorkspaceSessionTag::Project` and
  `MruStateTag::RecentProject`, each a nested record
  `{ Kind (Local=1, Remote=2), Path, Host, HostRoot }`. The writer emits the new
  record. The reader takes the new record when present and otherwise maps a
  legacy `ProjectRoot`/`RecentProjectRoot` path through `ResolveProjectId`, so
  old files lose nothing. The writer also keeps emitting the legacy path tag
  (the I/O root) so that a downgrade within the same major version still restores
  the session. That is cheap and removable later. The rest of the format is
  unaffected, and the fuzz corpus gains seeds for both forms.

- **A remote recents entry whose mirror is gone stays listed.**
  `ExistingRecentProjects` currently filters on `exists(root)`. For a `Remote` id
  the filter is dropped, and opening the entry goes through
  `RemoteHostService::OpenFolderOnHost(host, host_root)`, which recreates the
  mirror. A `Local` id keeps the existence filter. `Remote: Remove Mirror`
  already removes the entry explicitly, so a deliberately removed mirror does not
  come back.

## Risks / Trade-offs

- [A restored session lists a remote project before any connection] → Correct
  and intended: the label comes from the id, and nothing connects until
  activation, as today.
- [Persisted files now carry host names in two more lists] → They already do
  under `meta/remote` and the mirror path. The state files are private
  (`EnsureSecurePrivateDirectory`).
- [Resolution reads `meta/remote` once per legacy entry on the first load after
  upgrade] → It is one small file per restored remote project, read once. The new
  record makes later loads read nothing.
- [Hot paths] → The tab-strip sources and window title already cache per
  revision. Labels are computed from the id once per catalog change, not per
  frame. `perf-tests` is run because `WorkspaceTabStripChrome` is a
  presentation hot path.

## Migration Plan

Additive format change with the dual-write described above. No user action.
Rollback to an older build reads the legacy tags it knows.

## Open Questions

None blocking. The dual-write window (when to stop writing the legacy path tags)
is decided at the next major format bump.
