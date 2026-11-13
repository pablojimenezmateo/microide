## 1. Model

- [ ] 1.1 `RecentsService::RecentRemoteFolders(limit)`: the remote ids' `Location()`, newest first, capped; tested over a mixed local/remote MRU and the cap
- [ ] 1.2 `PromptSurfaceState::recent_inputs`, cleared on every open; filled for `RemoteFolder` with the latest 5

## 2. Layout and paint

- [ ] 2.1 `ComputePromptSurfaceRect(full, recent_rows)` (required argument) and `ComputePromptSurfaceRecentRowRects(dialog, count)`; every caller passes the prompt's row count; layout test: rows sit between input and buttons, the dialog grows only with rows, a short window drops rows instead of overlapping the buttons
- [ ] 2.2 Render the Recent heading and rows (highlight = row equal to the input; hover lifts)

## 3. Interaction

- [ ] 3.1 Click a row opens it; cursor is a pointer over rows; Up/Down cycle rows into the input
- [ ] 3.2 Control ui lists rows as `prompt/recent/<n>` buttons
- [ ] 3.3 Shell tests: rows listed from the MRU; click opens; Up/Down/Enter; no-recents prompt unchanged; redraw verifier clean over the grown card

## 4. Validation and docs

- [ ] 4.1 `tools/run-checks.sh tests`, `clang-build`, `qa`
- [ ] 4.2 `remote-projects.md` entry points, `active-work.md`; deferred items to `known-tech-debt.md`
