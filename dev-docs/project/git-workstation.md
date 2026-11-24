# Git Workstation (1.x)

MicroIDE's most validated wedge is the **Git Workstation**: a native, low-footprint desktop
shell focused on local repository inspection, diffing, staging, text conflict resolution, commit,
and branch review. Shipped in the 1.x line. This document states what the workstation promises
and what remains out of scope.

Performance wording follows the project methodology: internal regression baselines and harness
scenarios — not comparisons to other editors.

## Supported workflows

| Workflow | Supported behavior |
|----------|--------------------|
| Open local Git repository | Open project tab on a directory with a Git worktree |
| Inspect working tree | Git sidebar, directory tree dirty markers, status summaries |
| View staged / unstaged diffs | Compare tabs (HEAD, index, commits as implemented) |
| Stage / unstage files and hunks | Git sidebar and compare staging affordances |
| Resolve common text conflicts | Three-way merge tab for line-oriented text conflicts |
| Commit staged changes | Commit workflow in Git sidebar |
| Review branch against base | Branch review mode in compare (local base ref) |
| Switch / create branches | Filterable branch picker, sidebar branch row, `git-switch-branch` / `git-create-branch` |
| Fetch, pull, push, publish, sync | Git menu, sidebar Sync button (with ahead/behind counts), `git-fetch` / `git-pull` / `git-push` / `git-publish-branch` / `git-sync` |
| Review what changed since a marked point | `review-checkpoint-mark` snapshots the working tree (untracked files included, index untouched) under `refs/worktree/microide/review-checkpoint`; `review-checkpoint` opens a compare tab per file that differs from it now, across whatever was committed in between |
| Move between working trees | the status bar names the repository's working trees once it has linked ones (`3 worktrees`, prunable ones left out, counted from the git directory by the background refresh) and a click opens `git-worktrees` (Git ▸ Switch Worktree…): a pick of `git worktree list`, each tree with its changed-file count and its distance from its upstream (one `git status` a tree, asked when the pick opens), opening the chosen tree as a project (on the host for a remote project) |
| Review a recent commit | `git-commits` (Git ▸ Review Commit…): a pick of the branch's last hundred commits; a row runs `review-commit <hash>` |
| Stash and restore work in progress | `git-stash [message]` (includes untracked) / `git-stash-pop` |

Write operations run off the shell thread, one at a time, and report a classified
outcome (auth failure, no upstream, non-fast-forward, dirty worktree, conflict,
locked repo, network failure, timeout) rather than raw porcelain. Credential
prompting is disabled (`GIT_TERMINAL_PROMPT=0`), so an operation needing
credentials fails fast instead of blocking on an invisible prompt.

### Git/Diff UI behavior

- Compare headers keep review context compact and show shortcut discovery as a hint instead of a full
  always-on shortcut wall.
- Collapsed unchanged diff context renders as an intentional row block with attached reveal controls
  (`Show previous 20`, `Show all`, `Show next 20`).
- Git sidebar row action buttons are surfaced on the selected entry, reducing always-visible noise
  while preserving keyboard and command-palette actions.

### Conflict support boundaries

- **Supported:** common line-oriented text conflicts surfaced in merge tabs.
- **Recognized but not fully interactive:** binary, submodule, and complex rename/file-directory
  conflicts may appear in summaries without full three-way editing.

### Branch review persistence

Branch review against a base is supported, and reviewed-file / reviewed-hunk markers
**are** persisted per repository and base ref (`mark-branch-file-reviewed`,
`mark-branch-hunk-reviewed`, `branch-review-comment`, `clear-branch-review-state`),
bounded per repository so the state file cannot grow without limit.

The marks and notes are made on the diff itself (`render/CompareReviewControls`,
`services/CompareReviewService`): the hunk under the pointer or holding the
selection shows Mark Reviewed / Unmark and Comment on its first row, a marked
hunk keeps a Reviewed badge there, a note (a "comment" in the UI) shows as a row
under its hunk with Edit / Delete, and the header carries the review's progress,
Mark File Reviewed / Unmark File and, on every diff, Previous / Next Change. The
comment prompt pins the review, file and hunk identity it was opened on.
A hunk taller than the view keeps its controls on the view's top row
(`CompareHunkControlsOnRow`, the one rule paint, the row key, the click and the
`ui` element list ask). The hover is kept as an on-screen row, so rows scrolled
under a still pointer move it.

A hunk note is placed by `BranchReviewStateService::ResolveHunkMarkers`: on the
hunk at its position (`note_outdated` when that hunk's content changed), else on
a hunk with the same content (lines moved), else it is *detached* and shows at
the top of the diff (`compare/note-detached-N/…` in the `ui` query). Editing a
note re-pins it to the hunk as it stands (`PromptSurfaceState::review_replaced_hunk`).
Marking a file writes every hunk's entry in one pass (`SetFileReviewed`); past
the per-review bound the entries of whole-marked files are pruned first, so a
large file never takes another file's marks.

The notes of a review leave it as text: `copy-review-comments [path]` and
`term-insert-review-comments [path]` give every note of the review -- the active
diff's, else the repository's review touched last -- as a
`path:line[-line]: note` line (a file's note as `path: note`), by path and line,
to the clipboard or to the running terminal; a path narrows them to a file or
directory. The Git menu offers both with the note count, and the git view's
right-click offers them for an entry, a directory or the Outgoing header.

## Unsupported (explicit non-goals)

- Hosted PR review, account auth, provider sync
- Plugin marketplace, remote install, signed plugins
- Project-local plugin directories (repo `.microide/plugins/` remains ignored)
- Plugin sandbox, per-plugin capability prompts, plugin signing
- Native OS menu bar
- Full binary merge editing
- AI features, cloud collaboration, account systems

## Safe startup

See [SECURITY.md](../SECURITY.md) and [plugin-trust-model.md](plugin-trust-model.md).

- `--disable-plugins` — deterministic startup without user plugins or plugin syntax
- `--safe-mode` — plugins off, no workspace/session restore, empty shell unless a project path is passed

## Validation

Before tagging a release, maintainers run the checklist in
[release-checklist.md](release-checklist.md) and the tested-workflows matrix there.
