## Why

The roadmap should converge on a narrow technical preview once the Git/diff/merge wedge is coherent. Before that preview, scope, trust documentation, plugin-disable startup, release artifacts, tested workflows, and known limitations must be explicit so users can safely try real repositories without overclaiming.

## What Changes

- Define a 0.1 Git Workstation Preview scope: open local Git repo, inspect working tree, view staged/unstaged diffs, stage/unstage files and hunks, resolve common text conflicts, commit staged changes, and review branch against base.
- Promote a minimal safe startup path into scope: `--disable-plugins`, `--safe-mode`, visible plugins-disabled status, and docs that consistently state repo-local plugin code is not loaded by default.
- Add preview release criteria: tagged release, Linux binary first if Linux remains the primary validated host, checksums, screenshot gallery, short demo, tested workflows, known limitations, crash/data-loss reporting instructions, and no comparative marketing claims.
- Keep plugin sandboxing, marketplace, remote development, hosted PR review, debugger, AI features, and full binary merge out of scope.

## Capabilities

### New Capabilities
- `git-workstation-preview-release`: Scope, safety, packaging, documentation, and release criteria for a narrow Git workstation preview.

### Modified Capabilities
- `product-vision`: Promotes minimal safe-mode/plugin-disable startup from non-goal to scoped preview requirement while keeping broader plugin security hardening out of scope.

## Impact

- Affects CLI startup flags, plugin runtime initialization, status bar/Help/About state, README/docs trust wording, release documentation, packaging scripts, and preview validation checklist.
- Does not require building a plugin sandbox or marketplace.
