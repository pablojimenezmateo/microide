## 1. Model

- [x] 1.1 `RecentsService::RecentRemoteFolders(limit)`: the remote ids' `Location()`, newest first, capped; tested over a mixed local/remote MRU and the cap (`RecentsService/RemoteFoldersAreTheLatestRemoteOnly`)
- [x] 1.2 `PromptSurfaceState::recent_inputs`, cleared on every open; filled for `RemoteFolder` with the latest `kPromptSurfaceMaxRecentRows` (5; the card's capacity is the cap, one constant)

## 2. Layout and paint

- [x] 2.1 `ComputePromptSurfaceRect(full, recent_rows)` (required argument) and `ComputePromptSurfaceRecentLayout(dialog, count)`; every caller passes the prompt's row count; layout test `WorkspaceShared/PromptSurfaceRecentSection`: rows sit between input and buttons, the card grows only in height and only with rows, the buttons keep the plain card's gap, the cap holds, a short window drops rows instead of overlapping the buttons
- [x] 2.2 Render the Recent heading and rows (highlight = row equal to the input, accent strip; hover lifts)

## 3. Interaction

- [x] 3.1 Click a row opens it through the same confirm as Enter; cursor is a pointer over rows; Up/Down cycle rows into the input (wrapping), and keep their old meaning with no rows
- [x] 3.2 Control ui lists rows as `prompt/recent/<n>` buttons (`control-channel.md`)
- [x] 3.3 Shell tests: `WorkspaceShell/RemoteFolderPromptOffersRecentFolders` (rows from the MRU, no-recents prompt unchanged, Up/Down, click opens the clicked row) and `WorkspaceShell/RemoteFolderRecentRowsRepaintExactly` (retained redraw over the grown card; recorded negative control `prompt-redraw-plain-card.patch` fails it)

## 4. Validation and docs

- [x] 4.1 `tools/run-checks.sh tests`, `clang-build`, `perf-tests` green; `qa` 230/232 + monkey clean. The two QA findings (`huge-file` undo budget, `lsp-csharp` startup) and one soak-driver unit test were machine load (load avg ~55): base failed `huge-file` the same in an A/B, the others passed isolated (TD-2026-11-13-446)
- [x] 4.2 `remote-projects.md` entry points, `active-work.md`; deferred items to `known-tech-debt.md` (TD-2026-11-13-445; TD-446, TD-447 found along the way)
