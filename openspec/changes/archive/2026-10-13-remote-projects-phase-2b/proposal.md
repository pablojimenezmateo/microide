## Why

Phase 2a put a server on the host and its terminals in the window; it opens no
remote PROJECT. Phase 2b of `dev-docs/design/remote-projects.md` (§ 8) is the
product: open a tree that lives on another machine and edit it at local speed,
through a local mirror kept equal to the host's tree, with every process running on
the host.

## What Changes

- **Content addressing.** BLAKE3 `util::ContentHash` (32 raw bytes on the wire) is
  the object address, the manifest row's hash and the compare-and-swap token.
- **Server: the workspace tree.** The content set is decided on the host
  (`git ls-files --cached --others --exclude-standard`, or a filtered walk), hashed
  through a hash cache keyed by `(dev, ino, size, mtime_ns, ctime_ns)`, and served as
  `tree/manifest` → packed, prefix-compressed `TreeRows` content frames on the bulk
  lane. An unreadable root or a failed content-set command is an error, never an
  empty manifest; a set over `remote.max_manifest_files` fails loudly.
- **Server: objects and writes.** `object/fetch` (by hash, interactive or bulk),
  `file/write` with a three-valued `expect` (hash | absent | any; `O_EXCL` for
  absent), `fs/op` (mkdir, rename with `RENAME_NOREPLACE`, delete), `file/read` for
  out-of-set paths, and `watch/subscribe` → `watch/changed` batches.
- **Client: the mirror.** `$XDG_DATA_HOME/microide/remote/<host>/<slug>/{tree,meta}`;
  a `MirrorStore` (object store, manifest, confined `O_NOFOLLOW` materialization), a
  `MirrorSyncEngine` (four content states with a base, pull priority, mass-delete
  guard, `local-only` membership), and `MirrorWriteGate`, the remote
  `project::FileWriteGate`, which writes locally, journals and pushes with CAS.
- **Open Remote.** `Remote: Open Folder on Host…` opens a project whose root is the
  mirror's `tree/`, whose launcher is the host session's `RemoteProcessLauncher` and
  whose write gate is the mirror's; the tab and title name `host:/path`, never the
  mirror path.
- **Host-side search** (`search/run`) and pushed `git/metadata`/`git/status`.

## Impact

- `src/util/ContentHash.*`, `src/project/remote/RemoteManifest.*`,
  `src/project/remote/Mirror*`, `src/server/WorkspaceTree.*`, the remote host
  service and the open flow. No new `WorkspaceShell*.cpp` companion.
- zstd deltas (§ 6.2) need a vendored zstd; until one is approved, a stale file
  transfers whole (TD).
