## 1. Product Scope And Docs

- [ ] 1.1 Update README, docs/active-work.md, implementation guide, and plugin trust docs to consistently state preview scope and plugin trust behavior.
- [ ] 1.2 Add `SECURITY.md` covering trusted local plugins, ignored repo-local plugin directories, safe mode, reporting, and known limitations.
- [ ] 1.3 Add preview supported/unsupported workflow documentation without comparative marketing claims.

## 2. Safe Startup Flags

- [ ] 2.1 Add CLI parsing for `--disable-plugins` and `--safe-mode`.
- [ ] 2.2 Make `--disable-plugins` skip user-scope plugin loading and plugin syntax loading.
- [ ] 2.3 Make `--safe-mode` imply plugin disabling and any documented recovery-safe startup behavior.
- [ ] 2.4 Surface plugins-disabled or safe-mode state in status/help UI through view models.

## 3. Release Checklist

- [ ] 3.1 Add release checklist for tag, checksums, Linux binary if Linux remains primary validated host, build-from-source instructions, screenshots, demo, known limitations, and tested workflows.
- [ ] 3.2 Add a tested workflows matrix for local repo open, inspect changes, view diffs, stage/unstage, resolve text conflicts, commit, and branch review.
- [ ] 3.3 Add crash/data-loss reporting instructions.

## 4. Verification

- [ ] 4.1 Add tests proving `--disable-plugins` does not load user plugins or plugin syntax files.
- [ ] 4.2 Add tests proving `--safe-mode` implies plugin disabling and surfaces visible UI state.
- [ ] 4.3 Run plugin runtime, startup, status/help UI, docs/link, and smoke workflow tests before cutting a preview.
