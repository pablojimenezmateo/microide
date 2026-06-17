# Release checklist

Use this checklist when cutting a microide release tag (e.g. `v1.3.0`). Items assume Linux is the
primary validated host unless release notes state otherwise.

## Standard release procedure

When a release is requested ("do a release", "cut a release", "release vX.Y.Z"), perform **all** of
these steps in order. None is optional — every published release must carry the `.deb` and its
checksum so users have a packaged install path.

1. **Bump the version.** Update `project(microide VERSION ...)` in `CMakeLists.txt` and refresh the
   `vX.Y.Z` references in `README.md`.
2. **Update `CHANGELOG.md`.** Add a new dated section for the version with grouped changes derived
   from `git log <previous-tag>..HEAD`.
3. **Build.** `cmake -S . -B build && cmake --build build -j8`; confirm the new version is baked
   into the binary.
4. **Build the package.** From `build/`, run `cpack -G DEB` to produce `microide_X.Y.Z_amd64.deb`.
5. **Generate the checksum.** `sha256sum microide_X.Y.Z_amd64.deb > microide_X.Y.Z_amd64.deb.sha256`.
6. **Commit and tag.** Commit the version/changelog/README changes, create an annotated tag
   `vX.Y.Z`, and push both `main` and the tag.
7. **Create the GitHub release.** `gh release create vX.Y.Z` with notes summarizing scope,
   limitations, and safe-startup flags.
8. **Attach artifacts.** `gh release upload vX.Y.Z microide_X.Y.Z_amd64.deb microide_X.Y.Z_amd64.deb.sha256`.
9. **Verify.** `gh release view vX.Y.Z --json assets` and confirm both the `.deb` and `.deb.sha256`
   are listed.

## Pre-tag engineering

- [ ] `ctest --test-dir build --output-on-failure` green on release configuration
- [ ] `./build/microide/microide_tests AppStartupOptions` green (startup flags + UI surfacing)
- [ ] Plugin host tests green (`PluginHost/*`)
- [ ] Focused workspace Git/compare/merge smoke tests green
- [ ] Docs links resolve: `SECURITY.md`, `dev-docs/project/git-workstation.md`, `guidelines/plugin-trust-model.md`
- [ ] No comparative marketing claims in README or release notes (see README performance section)

## Tag and artifacts

- [ ] `CHANGELOG.md` updated with the release version, date, and grouped changes
- [ ] Git annotated tag `v1.3.0` (or chosen version) on the release commit
- [ ] Release notes summarizing scope, limitations, and safe-startup flags
- [ ] SHA256 checksums for distributed Linux x86_64 binary (if published)
- [ ] Build-from-source instructions (`dev-docs/platform/linux-build.md` or README build section)

## User-facing artifacts

- [ ] Screenshot gallery or equivalent visual walkthrough (Git sidebar, compare, merge, commit)
- [ ] Short demo or scripted walkthrough (asciinema, video, or step list in release notes)
- [ ] Known limitations section (copy/adapt from README **Known Limitations** + Git Workstation doc)
- [ ] Tested workflows matrix (below)
- [ ] Crash / data-loss reporting instructions (link `SECURITY.md`)

## Tested workflows matrix

Mark each row **pass** / **fail** / **n/a** on the release candidate build.

| # | Workflow | Steps (abbreviated) | Result |
|---|----------|---------------------|--------|
| 1 | Open local repo | `File` / project open on git repo | |
| 2 | Inspect changes | Git sidebar lists worktree sections | |
| 3 | View unstaged diff | Open file from changes → compare tab | |
| 4 | View staged diff | Stage file → compare staged side | |
| 5 | Stage / unstage hunk | Stage hunk from compare or sidebar | |
| 6 | Text conflict resolve | Merge tab: take left/right/chunk | |
| 7 | Commit | Stage + commit message + commit | |
| 8 | Branch review | Compare branch vs base ref | |
| 9 | Safe mode empty shell | `microide --safe-mode` → no restored projects | |
| 10 | Disable plugins | `microide --disable-plugins` → status shows Plugins off | |

## Crash and data-loss reporting

Publish in release notes:

1. Open a bug with OS, version/commit, and reproduction steps.
2. Note whether `--safe-mode` or `--disable-plugins` was active.
3. State whether data loss involved unsaved buffers, merge results, or persisted session files.
4. For security-sensitive issues, follow `SECURITY.md` private reporting.

## Post-tag

- [ ] Update `dev-docs/project/active-work.md` release status if scope shipped
- [ ] Archive any OpenSpec change that shipped with this tag (move it under `openspec/changes/archive/`)
