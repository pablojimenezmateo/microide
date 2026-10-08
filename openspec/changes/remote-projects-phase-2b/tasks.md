## 1. Content hashes and the manifest

- [x] 1.1 `util::ContentHash`: BLAKE3, streaming, file hasher; upstream vectors
- [x] 1.2 `RemoteManifest` row codec: packed, prefix-compressed, validated decode (fuzz target)
- [x] 1.3 Server `WorkspaceTree`: content set (git ls-files / filtered walk), lstat rows, in-memory hash cache, parallel cold hashing, `max_manifest_files`, failure is never an empty set
- [x] 1.4 `tree/manifest` → `TreeRows` bulk content frames, reply on the bulk lane after the last chunk with `manifest_id` and the count; worker thread, never the peer's I/O thread
- [x] 1.5 Persist the hash cache under the server's state directory

## 2. Objects and writes

- [x] 2.1 `object/fetch`: hashes → object bytes (chunked content frames), lane per request; verify the hash before serving
- [x] 2.2 `file/write {path, content, mode, expect}` with expect = hash | absent | any; atomic; returns the new hash or `conflict {current_hash}`
- [x] 2.3 `fs/op`: mkdir, rename (expect on source, `RENAME_NOREPLACE` destination), delete
- [ ] 2.4 `file/read` (read-only, out of the content set)
- [x] 2.5 Root confinement on every server-side path (no `..`, no absolute, no escape through a symlink)

## 3. Watch

- [x] 3.1 `watch/subscribe` → coalesced `watch/changed` rows + deletes from the server's watcher; `.gitignore` edits re-run the content set
- [ ] 3.2 Watch mode (inotify | polling) reported in hello and on transition

## 4. Client mirror

- [ ] 4.1 `MirrorStore`: layout, object store, persisted manifest, confined materialization (`O_NOFOLLOW`, parent under `tree/`)
- [x] 4.2 `MirrorSyncEngine`: manifest diff, current/stale/dirty/absent with base, pull priority (open tabs first), mass-delete guard, `local-only`
- [x] 4.3 `MirrorWriteGate` (a `project::FileWriteGate`): local write, journal, CAS push, conflict parking
- [ ] 4.4 Journal through `PersistedRecordWriter`, ordered replay on reconnect

## 5. Opening a remote project

- [x] 5.1 `Remote: Open Folder on Host…` and the remote project record (host + host root)
- [x] 5.2 Project wiring: root = mirror `tree/`, launcher = host session, write gate = mirror, git metadata = host
- [ ] 5.3 Presentation: `host:/path` on the tab, title and recents; connecting/syncing status
- [ ] 5.4 Pushed `git/metadata` and `git/status`

## 6. Search, conflicts, parity

- [ ] 6.1 `search/run` on the host, streamed results
- [ ] 6.2 Conflict flow: Reload / Overwrite / Compare on a refused push
- [x] 6.3 Parity rows against the real server with the mirror in place
