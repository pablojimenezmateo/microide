## 1. Failing first

- [x] 1.1 `RemoteHostServiceTests`: with a remote project open on the fake ssh fixture, assert the project tab title, tooltip, window title and `ProjectLabel()` read `repo` / `host:/…/repo`. Fails today with `tree` and the mirror path.
- [x] 1.2 Session-restore test: an inactive restored remote project is labelled `repo` before activation (fails today)

## 2. The type (kernel)

- [x] 2.1 `src/project/ProjectId.{h,cpp}`: `Local`/`Remote` variant, `==`, `IsRemote`, `Label`, `Location`, `IoRoot`
- [x] 2.2 `ResolveProjectId(io_root)` (the `LocalityForMirror` rule: record present AND path is its default tree)
- [x] 2.3 `microide_kernel_tests`: labels (basename, filesystem root, trailing slash), location, `IoRoot` round trip, a copied mirror resolves `Local`, distinct ids never share an `IoRoot`

## 3. Identity on the project

- [x] 3.1 `ProjectWorkspaceState::id`, kept through `ResetProjectScopedState` beside `locality`
- [x] 3.2 Set at catalog-entry creation: `OpenProjectTab`, session restore, recents. `Open Folder on Host` builds `Remote` directly.
- [x] 3.3 `RemoteHostService::LocalityFor(const ProjectId&)` replaces `LocalityForMirror`, and the activation hook in `WorkspaceShellProjects.cpp` reads the id instead of sniffing
- [x] 3.4 `RecentsService`, `ControlChannelService` (`projects`, `status`) and `RemoteHostService` messages read the id instead of calling `RemoteProject::ReadRecord`. Control JSON is unchanged.

## 4. Presentation

- [x] 4.1 Delete `ProjectLabelForRoot`. Move its six callers and `ProjectTabTooltipLabel` to `id.Label()` / `id.Location()`.
- [x] 4.2 The window title's project label is precomposed in frame prep, with no new string work in `WorkspaceShellRenderFrame.cpp`
- [x] 4.3 1.1 and 1.2 pass. `perf-tests` (tab-strip presentation is a hot path) and `qa` (tab strip and title paint) are green.

## 5. Persistence

- [x] 5.1 `ProjectIdRecord` nested tags: `WorkspaceSessionTag::Project` and `MruStateTag::RecentProject`. Dual-write the legacy path tags.
- [x] 5.2 Reader: prefer the record, map a legacy path through `ResolveProjectId`. Round-trip tests in both directions, including a legacy-only file and a new file read by the legacy-tag path.
- [x] 5.3 `PersistedRecordDump` prints ids. Seed `PersistedRecordReaderFuzz` with both forms and run it for 60 s.
- [x] 5.4 Recents: keep a `Remote` entry whose mirror is gone, and reopen it through `OpenFolderOnHost`. `Remote: Remove Mirror` still drops it. Test both.

## 6. Close out

- [x] 6.1 `dev-docs/design/remote-projects.md`: § 6.8 (state dir stays root-keyed and why), Groundwork table G8 shipped. `active-work.md` § 7.
- [x] 6.2 `tools/run-checks.sh tests`, `asan`, `clang-build`. Archive the change, with the spec deltas merged into `openspec/specs/`.

## Notes from implementation (2026-10-26)

- The proposal says labels read `tree`. They do not: `RemoteProject`'s tree is named
  after the host folder's basename (`TreeName`), so the tab title, badge, window
  title and prompts already read `repo`. The leaks that were real are the project
  tab's tooltip (the mirror path under the data directory), a host root of `/`
  (labelled `root`), and a restored remote project being local until activated
  (unmarked in `projects`). 1.1 and 1.2 assert all of it; the tooltip, the id and
  the `projects` marking are what fail before this change.
- 3.2: `Open Folder on Host` does not build the `Remote` id itself. It prepares the
  mirror (writing its record) and opens the tree through `OpenProjectTab`, whose
  `ResolveProjectId` yields the same `Remote` id by the same rule. Building it
  directly would need a second `OpenProjectTab` entry point, and
  `WorkspaceShellMembers.inc` is at its ratchet (1647).
- 3.4: `status.remote` still comes from `RemoteHostService::SnapshotOf` (it reports
  live connection and sync state, which the id does not have). `projects` reads
  the id, so it marks a restored remote project before it connects.
- 3.3: a remote id whose mirror cannot be opened now fails the open (with the
  existing notification) instead of opening the mirror as a local folder.
- Session restore still drops an entry whose I/O root is missing, remote ones
  included; only the recents list keeps a remote entry without its mirror (the
  spec's requirement).
- 5.3: libFuzzer's runtime is not installed on the development machine
  (`libclang-rt-21-dev`). `ProjectSessionRecordFuzz` now also runs
  `DecodeWorkspaceSessionRecord` and both corpora carry the two session forms; the
  60 s runs used a replay-and-mutate driver over the same harnesses under
  ASan/UBSan (44M and 21M inputs, clean). CI's `fuzz-smoke` builds the real
  targets and runs their corpora.
