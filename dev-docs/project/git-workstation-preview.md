# Git Workstation Preview (0.1)

MicroIDE is preparing a narrow **Git Workstation Preview**: a native, low-footprint desktop
shell focused on local repository inspection, diffing, staging, text conflict resolution, commit,
and branch review. This document states what the preview promises and what remains out of scope.

Performance wording follows the project methodology: internal regression baselines and harness
scenarios — not comparisons to other editors.

## Supported workflows

| Workflow | Preview expectation |
|----------|---------------------|
| Open local Git repository | Open project tab on a directory with a Git worktree |
| Inspect working tree | Git sidebar, directory tree dirty markers, status summaries |
| View staged / unstaged diffs | Compare tabs (HEAD, index, commits as implemented) |
| Stage / unstage files and hunks | Git sidebar and compare staging affordances |
| Resolve common text conflicts | Three-way merge tab for line-oriented text conflicts |
| Commit staged changes | Commit workflow in Git sidebar |
| Review branch against base | Branch review mode in compare (local base ref) |

### Git/Diff UI behavior

- Compare headers keep review context compact and show shortcut discovery as a hint instead of a full
  always-on shortcut wall.
- Collapsed unchanged diff context renders as an intentional row block with attached reveal controls
  (`Show previous 20`, `Show all`, `Show next 20`).
- Git sidebar row action buttons are surfaced on the selected entry, reducing always-visible noise
  while preserving keyboard and command-palette actions.

### Conflict support boundaries

- **Supported in preview:** common line-oriented text conflicts surfaced in merge tabs.
- **Recognized but not fully interactive:** binary, submodule, and complex rename/file-directory
  conflicts may appear in summaries without full three-way editing.

### Branch review persistence

Branch review against a base is in preview scope. **Persistent** reviewed-file or reviewed-hunk
markers may ship after the preview if core Git/diff/merge/commit paths and safety requirements
are already met. Do not assume durable review markers in 0.1 unless release notes say otherwise.

## Unsupported (explicit non-goals for preview)

- Hosted PR review, account auth, provider sync
- Plugin marketplace, remote install, signed plugins
- Project-local plugin directories (repo `.microide/plugins/` remains ignored)
- Plugin sandbox, per-plugin capability prompts, plugin signing
- Debugger / DAP
- Recent-project / recent-file UI
- Native OS menu bar
- Full binary merge editing
- AI features, cloud collaboration, account systems

## Safe startup

See [SECURITY.md](../SECURITY.md) and [plugin-trust-model.md](plugin-trust-model.md).

- `--disable-plugins` — deterministic startup without user plugins or plugin syntax
- `--safe-mode` — plugins off, no workspace/session restore, empty shell unless a project path is passed

## Validation

Before tagging a preview release, maintainers run the checklist in
[preview-release-checklist.md](preview-release-checklist.md) and the tested-workflows matrix there.
