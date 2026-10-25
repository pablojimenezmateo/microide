## 1. Failing first

- [ ] 1.1 `RemoteHostServiceTests`: with a remote project open on the fake ssh fixture, assert the project tab title, tooltip, window title and `ProjectLabel()` read `repo` / `host:/…/repo`. Fails today with `tree` and the mirror path.
- [ ] 1.2 Session-restore test: an inactive restored remote project is labelled `repo` before activation (fails today)

## 2. The type (kernel)

- [ ] 2.1 `src/project/ProjectId.{h,cpp}`: `Local`/`Remote` variant, `==`, `IsRemote`, `Label`, `Location`, `IoRoot`
- [ ] 2.2 `ResolveProjectId(io_root)` (the `LocalityForMirror` rule: record present AND path is its default tree)
- [ ] 2.3 `microide_kernel_tests`: labels (basename, filesystem root, trailing slash), location, `IoRoot` round trip, a copied mirror resolves `Local`, distinct ids never share an `IoRoot`

## 3. Identity on the project

- [ ] 3.1 `ProjectWorkspaceState::id`, kept through `ResetProjectScopedState` beside `locality`
- [ ] 3.2 Set at catalog-entry creation: `OpenProjectTab`, session restore, recents. `Open Folder on Host` builds `Remote` directly.
- [ ] 3.3 `RemoteHostService::LocalityFor(const ProjectId&)` replaces `LocalityForMirror`, and the activation hook in `WorkspaceShellProjects.cpp` reads the id instead of sniffing
- [ ] 3.4 `RecentsService`, `ControlChannelService` (`projects`, `status`) and `RemoteHostService` messages read the id instead of calling `RemoteProject::ReadRecord`. Control JSON is unchanged.

## 4. Presentation

- [ ] 4.1 Delete `ProjectLabelForRoot`. Move its six callers and `ProjectTabTooltipLabel` to `id.Label()` / `id.Location()`.
- [ ] 4.2 The window title's project label is precomposed in frame prep, with no new string work in `WorkspaceShellRenderFrame.cpp`
- [ ] 4.3 1.1 and 1.2 pass. `perf-tests` (tab-strip presentation is a hot path) and `qa` (tab strip and title paint) are green.

## 5. Persistence

- [ ] 5.1 `ProjectIdRecord` nested tags: `WorkspaceSessionTag::Project` and `MruStateTag::RecentProject`. Dual-write the legacy path tags.
- [ ] 5.2 Reader: prefer the record, map a legacy path through `ResolveProjectId`. Round-trip tests in both directions, including a legacy-only file and a new file read by the legacy-tag path.
- [ ] 5.3 `PersistedRecordDump` prints ids. Seed `PersistedRecordReaderFuzz` with both forms and run it for 60 s.
- [ ] 5.4 Recents: keep a `Remote` entry whose mirror is gone, and reopen it through `OpenFolderOnHost`. `Remote: Remove Mirror` still drops it. Test both.

## 6. Close out

- [ ] 6.1 `dev-docs/design/remote-projects.md`: § 6.8 (state dir stays root-keyed and why), Groundwork table G8 shipped. `active-work.md` § 7.
- [ ] 6.2 `tools/run-checks.sh tests`, `asan`, `clang-build`. Archive the change, with the spec deltas merged into `openspec/specs/`.
