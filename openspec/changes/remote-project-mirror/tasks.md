## 1. The spec and what already pins it

- [ ] 1.1 Map each `remote-project-mirror` requirement to the test(s) that pin it (`RemoteHostServiceTests`, kernel `MirrorSyncEngine`/`RemoteProject`/`RemoteHostSession`, `tests/parity/`); add a unit test for any requirement with none
- [ ] 1.2 Point `dev-docs/design/remote-projects.md` § 7.1 and § 6.8 at the spec and mark the palette-only entry points as shipped 2026-10-16

## 2. Remove Mirror

- [x] 2.1 `MirrorStore::Remove(tree)` deletes tree, objects, journal and state; unit test in `microide_kernel_tests`
- [x] 2.2 `RemoteHostService::RemoveMirror(root)`: dirty prompt, unsent-push confirmation naming the count, close the project tab, disconnect, delete, drop the recents entry
- [x] 2.3 `ActionId::RemoteRemoveMirror` / `remote-remove-mirror [root]`, available for the active remote project or a named closed one; palette label `Remote: Remove Mirror`
- [x] 2.4 Tests: active project with a dirty tab; closed mirror by root; unsent journal asks; README, man page, control-channel doc

## 3. Remote state on the control channel

- [x] 3.1 `status` query: `remote` object for a remote active project (host, hostRoot, state, filesPending, bytesPending, unsentPushes, conflicts)
- [x] 3.2 `projects` query: `host` and `hostRoot` on remote entries
- [x] 3.3 Tests through `ExecuteControlQuery` with the fake ssh fixture: the state sequence on open, unsent rises on an offline save; document the fields

## 4. The host scenario suite

- [ ] 4.1 `tools/qa/remote.py` on the existing driver: open+sync equality, save, clean-tab reload, dirty-tab banner (Compare/Overwrite/Reload via `banner-action`), offline save + reconnect, killed ssh master, host terminal, go-to-definition, agent churn with saves, relaunch; `MQA_REMOTE_HOST`/`MQA_REMOTE_ROOT`, host reset to a pinned commit, latencies in the summary
- [ ] 4.2 Once 3.x lands, wait on `status.remote.state` instead of the notification row
- [ ] 4.3 README of `tools/qa` names the remote suite and how to run it against a host
