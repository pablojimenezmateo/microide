# Remote Projects Over SSH

Last revised 2026-09-22. **Status: designed, not started.**

When a phase is committed to, it graduates into an `openspec/changes/` proposal.

## 1. The problem

Code is increasingly written by agents on a remote server, so the project tree
microide must open is not on the machine running the window. The goal is remote
projects with **no networking code**: no sockets, no bespoke wire protocol, no
authentication, no key management. `ssh` is the network layer and everything we
write is a pipe to it.

Constraints that scope every decision:

- **Latency is far and variable.** A cloud box, not a LAN VM. Anything that pays
  a round trip per keystroke, per file open, or per tree expansion cannot be the
  primary mode.
- **Installing microide on the server is acceptable.** The remote side does not
  have to be written; it is this binary in a headless mode.
- **A remote project means remote processes.** Terminal, git, LSP, DAP, tasks all
  run on the server, next to the toolchain. A local shell in a remote project is
  worse than no terminal: it looks right and runs the wrong `make`.
- **No FUSE.** sshfs is out; § 4 says why.
- **The shell thread never waits on the network.** Not for a stat, not for a read,
  not for a save. A far, variable link turns every blocking call into a UI stall
  and every dropped link into a hang.
- **Nothing the user typed is ever lost**, including across a dropped link, a
  crash, and a restart with the host unreachable.
- **The remote tree changes continuously, written by many agents.** This is the
  constraint that separates this design from ordinary remote editing, and it is
  assumed everywhere below. The human is not the main writer; several agents are,
  concurrently, in bursts of hundreds of files, and they run `git checkout` and
  `git rebase` between those bursts. Three consequences follow and are designed
  for rather than discovered: a mirror is **content-stale in steady state**, not
  only while it is first filling (§ 6.2); anything that reads mirror bytes and
  reports an answer must know which bytes are stale or it is silently wrong
  (§ 6.3, and it is why remote search runs on the host, § 6.11); and remote-change
  events arrive at a rate no per-file notification can survive (§ 6.3, § 7.6).

## 2. What the code already decided

| finding | where | consequence |
| --- | --- | --- |
| **226 I/O-performing `std::filesystem` calls in 63 files.** Counting every `std::filesystem::` token instead gives 3,195 in 393 and is meaningless — almost all are path arithmetic (`/`, `filename()`, the type name) and do no I/O. Of the 63 files, 13 are `src/project`, 9 `src/platform`, 4 `src/util`; the 27 under `src/workspace` hold 1–5 calls each, mostly `exists()` guards. Raw stream opens outside `util/` are ten. | `rg` over `src/` for the I/O-performing names only: `exists`, `is_regular_file`, `status`, `directory_iterator`, `last_write_time`, `file_size`, `read_symlink`, `canonical`, `create_directories`, `rename`, `remove`, `copy` and their siblings | The objection to routing file I/O over the wire is not that there are too many sites to audit. It is that those sites are *synchronous*, most on the shell thread, and correct locally because a stat is microseconds. Making them tolerate a 200 ms round trip is not an audit, it is an async rewrite with a loading state at every site. That is what rules out an agent-backed VFS as the primary mode, and what makes a local mirror the right shape: it leaves every one of those sites alone. |
| `util/TextFileIO.h` is the whole-file read/write chokepoint (`ReadTextFile`, `ReadTextFileClassified`, `WriteTextFileAtomically`, `ReadFileLineWindow`, `StatFileSignature`) | `src/util/TextFileIO.h`, `editor/TextViewportFileIO.cpp` | The editor opens a file with one synchronous `ReadTextFile` on the shell thread (`TextViewport::OpenFile`). On a local mirror that stays exactly as fast as today. |
| `platform::CaptureTreeSnapshot` already produces `(path, type, size, mtime)` for every file under a root, filtered, budgeted — **which is a sync manifest** | `platform/Filesystem.h` | The agent's manifest is a serialization of an existing structure, and the local side already knows how to diff two of them (`FileIndexWatcher`'s poll re-walk). |
| The editor detects external changes by `mtime+size` (`FileSignature`) and **refuses a save when the disk changed underneath** (`request_external_change_banner`) | `EditorTabService`, `WorkspaceTabCoordinator.h` | A mirror written atomically (temp + rename) is indistinguishable from any other external writer. The conflict banner exists; remote conflicts reuse it with one extra choice. |
| `util::Sha256FileHex` exists | `util/Sha256.h` | Content identity for compare-and-swap on save, without trusting cross-machine clocks. |
| The heavy subsystems are **pipe-shaped** | `project/GitCommandUtil.h` (`ReadGitCommandOutput`), `platform/AsyncSubprocess.h` | Prefixing an argv with `ssh -S <ctl> <host> --` makes git, LSP and DAP remote with no protocol work. |
| `StdioJsonRpcClientTransport` is a generic, hardened stdio JSON-RPC client — framing, bounded queues, wedged-peer teardown, poll-based I/O thread, perf counters — and `JsonRpcMessageFraming` is the shared codec | `workspace/StdioJsonRpcClientTransport.h`, `workspace/JsonRpcMessageFraming.h` | The transport's *behaviour* is reused — bounded queues, wedged-peer teardown, the poll-based I/O thread, the perf counters — while its codec is not: § 6.4 carries content rather than requests and uses binary frames. The split matters, because the hardened part is the thread and queue discipline, not `Content-Length`. |
| The terminal is **not** argv-shaped: it `execl`s `shell_path -i` or `shell_path -lc <cmd>` on a local pty | `platform/TerminalBackend.cpp`, `TerminalStartRequest` | `terminal.shell = "ssh host"` fails (`-i` is ssh's identity flag). The remote terminal is a launch-path change, not a setting. § 6.5. |
| Path ↔ URI conversion funnels through `FileUriForPath` / `PathFromFileUri` (12 caller files; all but one are LSP and DAP — `WorkspaceToolDownloader` decodes a download URL, not a workspace URI) | `workspace/FileUri.h` | The single place where the editor's (mirror) paths meet a process that runs on the server. One mapper, injected there, translates both directions. |
| Project identity is `std::filesystem::path` everywhere (`project_roots`, `recent_project_roots`, the per-project state directory name in `WorkspaceProjectPresentation.cpp`, breakpoints keyed by file) | `services/ProjectCatalogService.h`, `persistence/WorkspacePersistenceFormat.h`, `WorkspaceProjectPresentation.cpp:38` | The mirror `tree/` is still the filesystem root every I/O site sees, so those ~170 `.root` uses in `src/workspace` stay paths. What *names* the project becomes a `ProjectId` (Groundwork G8), and the persisted format changes deliberately. § 6.8. |
| Git deliberately reads `.git` directly in places (`ReadPendingMergeHeadId`, `ResolveGitDirectory`, `GitRepositoryMetadataTracker`, `StatusBarModelService::read_head_branch`) | `src/project/Git*`, `services/StatusBarModelService.h` | `.git` is not mirrored (§ 6.2), so these need a data source: the agent pushes `git/metadata`. They must not read the mirror's (absent) `.git` and conclude "not a repo". |
| Project search reads real bytes off disk per candidate (`ReadFileForTextSearch(absolute_root / relative_path, buffer)`) with no content-version check | `project/ProjectSearchService.cpp:382` | Search over a mirror whose content is behind the host does not degrade, it **lies**: it reports matches from bytes the host no longer has and misses matches in bytes it has not pulled. With agents writing continuously that is the normal case, not the edge case, and there is no in-band signal that the answer is wrong. This single fact is why remote search runs on the agent (§ 6.11) rather than over the mirror. |
| `ProjectChangeCoalescer` merges a change batch and hands back one ready batch with a generation counter; `FileIndexWatcher` already carries a "tree shape changed in a way `changes` cannot describe" escape hatch for a resync after dropped events | `project/ProjectChangeCoalescer.h`, `platform/FileIndexWatcher.h` | Both halves of agent churn already have a local vocabulary: coalesce the ordinary burst, and fall back to a full re-diff when the burst is a branch switch. § 6.3 reuses both rather than inventing a remote-specific path. |
| Watching is inotify with a poll fallback and a 50k-entry budget | `platform/FileWatcher.h` | Remote watching is real inotify on the agent; locally, the mirror is a normal directory and the existing watcher sees the sync engine's writes like any other change. |
| The app runs headless on Xvfb with the software renderer and paints through clip rects; the control channel drives it as JSONL | `tools/capture-media/lib.sh`, `dev-docs/control/control-channel.md` | Display forwarding is a real tier-0. Headless `--control` is also how every remote UI flow in this design gets tested without a display. |
| The welcome surface, status bar, notifications, prompt surfaces and quick-open overlays are host-owned, view-model-rendered, and registry-backed | `editor/WelcomeView.h`, `services/StatusBarModelService.h`, `services/NotificationService.h`, `services/PromptSurfaceService.h`, `OverlayIsQuickOpen` | Every UI surface § 7 needs has a home. None of it is new chrome. |

## 3. Measured: what one remote operation costs

Counting syscalls is the honest way to predict a network filesystem, because on a
mount **each path-based call is a round trip**. Method: `strace -f -tt` over a
headless `microide <project> --control` on Xvfb, filtered to calls naming a path
inside the project root, bucketed by control-channel command. A clean `git clone`
of this repo (10,672 files on disk, 4,258 tracked) was used, *not* a working tree
(a live tree here carries a 2.5 GB `.claude/` directory that swamps the count).
Two independent runs both reported exactly 4,895 `newfstatat`.

| operation | path calls in the project tree | breakdown |
| --- | --- | --- |
| open the project | **4,957** (+1,632 `getdents64`, +390 `inotify_add_watch`) | 4,895 `newfstatat`, 47 `openat`, 10 `access`, 5 `readlink` |
| open one file | **162** | 122 `newfstatat`, 20 `openat`, 14 `access`, 6 `readlink` |
| save one file | **148** | 120 `newfstatat`, 16 `openat`, 12 `access` |
| steady state, idle | **0** | the burst is entirely at open |

Upper bound on a naive mount, by round-trip time:

| | 20 ms | 80 ms | 200 ms |
| --- | --- | --- | --- |
| open project (~6.6k ops) | 2.2 min | 8.8 min | 22 min |
| open one file (162 ops) | 3.2 s | 13 s | 32 s |
| save one file (148 ops) | 3.0 s | 12 s | 30 s |

Real wall-clock is lower (attribute caching, page cache, some parallelism), but
4,967 of the 5,347 paths touched at open are distinct, so caching collapses
almost nothing on the first walk. The order of magnitude stands.

What a mirror has to move, measured on this repo's tracked set:

| | |
| --- | --- |
| tracked files | 4,258 |
| tracked bytes | 26.4 MiB |
| bytes in files under 256 KiB | 20.6 MiB across 4,247 files |
| files over 1 MiB | 1 (a `.webm` under `docs/media/`) |

So a cold mirror of this repo is ~26 MiB and ~4.3k files; at a conservative
20 Mbit/s that is ~11 s of transfer, and the manifest that makes the tree
*visible* is one reply of ~4.3k rows. The same tree over a mount is 8.8 min at
80 ms just to open, and 13 s per file after that.

## 4. Why sshfs is out, not just deferred

A mount is tempting as a stop-gap. It is out for four reasons, the first of which
is the one users actually hit:

1. **Permissions.** sshfs runs as the local user against files owned by the
   remote user. Unless uids match or `-o idmap=user` / `uid=`/`gid=` are passed
   correctly, ordinary files show as unwritable and directories as
   untraversable, and `allow_other` needs `user_allow_other` in `/etc/fuse.conf`,
   which is root's decision. Root-owned or group-shared trees on the server
   (common where agents run as a service user) do not map at all. Every one of
   these failures surfaces inside microide as "save failed" or an empty tree, and
   reads as a microide bug.
2. **A hung mount hangs the process.** When the link drops, every path call into
   the mount blocks in the kernel until FUSE gives up. `stat` on the shell thread
   is a frozen window; `-o reconnect` only papers over it. This directly violates
   the "shell thread never waits on the network" constraint and there is no
   user-space fix.
3. **The arithmetic in § 3.** A mount makes open-project minutes long at realistic
   latency, and no amount of caching changes the first walk.
4. **No inotify, no atomic rename semantics you can trust, no exec bits you can
   rely on.** The watcher degrades to a periodic full re-stat over the network,
   and `WriteTextFileAtomically`'s temp+rename can fail on a FUSE layer that does
   not support `renameat2` flags.

If a user already has a mount and points microide at it, it works exactly as well
as it does today: as a local folder. It is not a supported remote mode and the
docs will say so.

## 5. The option space, revised

| option | verdict | why |
| --- | --- | --- |
| **Display forwarding** (`xpra`, `waypipe`, `ssh -X`) | **tier-0, supported now.** | Zero code and total parity, because nothing is remote. Costs an RTT per keystroke echo. `xpra` specifically: it survives a dropped link and reattaches. Agents on the server can drive the same instance via `microide --control`. |
| **sshfs mount** | **out.** | § 4. |
| **Agent-backed VFS, no local copy** (every project I/O is an RPC with a cache in front) | **not the primary mode; a later on-demand tier only if a tree is too big to mirror.** | It is the "correct" endgame in the abstract and the wrong one under this codebase's constraints: it converts ~60 synchronous sites into async ones with a loading state each, pays a round trip on every uncached tree expansion and file open, and needs a local journal of dirty buffers anyway to survive a dropped link, which is a mirror of the files that matter, built ad hoc. |
| **Local mirror + remote agent + remote processes** | **chosen.** § 6. | The editor, tree, search, watcher, index and every unaudited filesystem site work on a real local directory at local speed. The agent keeps that directory equal to the remote tree and performs the writes. Processes run where the toolchain is. A dropped link degrades to offline editing, not to a hang, and nothing is lost because everything is on local disk. |
| **No IDE change**: agents push branches, the human pulls | the baseline every option must beat | Wins when the human only reviews. Loses the moment you run or debug what the agent wrote. |

## 6. Architecture: local mirror, remote agent, remote processes

```
 local machine                                      server
 ┌──────────────────────────────────────────┐       ┌─────────────────────────────┐
 │ microide (window)                        │  ssh  │                             │
 │  editor / tree / search / watch / index  │ ctl   │  microide-agent              │
 │        ▲ local disk, unchanged           │ master│   manifest, read, write(CAS)│
 │        │                                 │◀─────▶│   inotify → watch/changed   │
 │  ~/.local/share/microide/remote/<host>/  │       │   git → git/metadata        │
 │      <slug>/tree/   ◀── MirrorSyncEngine │       │                             │
 │      <slug>/meta/   (journal, manifest)  │       │  git / clangd / gdb / $SHELL│
 │                                          │◀─────▶│   spawned via the same      │
 │  RemotePathMap: mirror ⇄ remote          │       │   ControlMaster connection  │
 └──────────────────────────────────────────┘       └─────────────────────────────┘
```

### 6.1 Components (all host-owned, none in the shell)

| component | lives in | owns |
| --- | --- | --- |
| `RemoteHostSession` | `src/project/remote/` | One host's connection state machine (§ 6.6), the `ssh` ControlMaster lifecycle, reconnect backoff, the auth handoff to a terminal tab. |
| `RemoteAgentClient` | `src/project/remote/` | The protocol peer: the binary framing of § 6.4 over an `AsyncSubprocess` whose argv is `ssh -S <ctl> <host> -- microide-agent --root <remote_root> --attach`, reusing `StdioJsonRpcClientTransport`'s queue and I/O-thread discipline with its codec replaced. Typed request/notification wrappers for § 6.4. |
| `RemoteTerminalChannel` | `src/project/remote/` | A **second** agent connection on the same ControlMaster carrying only `term/*`, so a bulk file transfer cannot head-of-line block keystroke echo (§ 6.5). Owns handle bookkeeping and the trim-total resume offset per terminal (§ 6.12). |
| `MirrorSyncEngine` | `src/project/remote/` | Its own thread. Manifest diff, prioritized pull queue, push queue with compare-and-swap, the persisted journal, the object store and its GC, atomic writes into `tree/`. Publishes progress and conflicts to the shell thread through the existing SDL wake pattern (`ControlChannelService` shape). |
| `MirrorWriteGate` | `src/project/remote/` | The **single** door every local write into `tree/` goes through — the editor's save, the plugin file API, LSP resource ops and workspace edits, replace-in-project, the merge writer, the sidebar's file operations, and the sync engine's own pulls. Records the write against the path's base version, enqueues the push, returns the post-write hash so the caller never re-stats the file, and holds the per-path lock that keeps a pull and a save from racing on the same file. In a local project it is a pass-through. § 6.3, Groundwork G10. |
| `RemotePathMap` | `src/project/remote/` | Pure value type: `(mirror_root, remote_root)` and two **partial** functions `ToRemote(path)` / `ToMirror(path)`, each returning `nullopt` for a path outside its side's root. Injected wherever a mirror path leaves the process or a remote path enters it (§ 6.5). |
| `RemoteProcessLauncher` | `src/project/remote/` | Turns a local `argv + cwd` into `ssh -S <ctl> <host> -- cd <remote_cwd> && exec <argv>`. Used by git, LSP, DAP, tasks and (with `-tt`) the terminal. |
| `RemoteProjectService` | `src/workspace/services/` | The workspace boundary: opens/closes remote projects, owns the per-project `RemoteHostSession`, exposes state to `RenderViewModelBuilder`, `StatusBarModelService` and the actions. Coordinators take this service, never the session. |
| `microide-agent` | `src/agent/` | A separate, SDL-free binary (Groundwork G1), not a flag on the GUI executable. Headless **daemon** on the server, keyed to `(uid, remote_root)`, outliving any one connection (§ 6.12). No SDL, no window. Accepts clients on an AF_UNIX socket via `platform::ControlSocketServer`, and relays stdio when launched with `--attach`. Runs `CaptureTreeSnapshot`, `FileTreeWatcher`, `ReadTextFile`/`WriteTextFileAtomically`, `FileOperationService`, `GitRepositoryMetadataTracker` and `ProjectSearchService` **in-process on the server**. Owns the host ptys and their authoritative scrollback rings. Confines every path to `--root`. |

### 6.2 The mirror

- **Location.** `$XDG_DATA_HOME/microide/remote/<host>/<slug>/` with `tree/` (the
  project root microide opens) and `meta/` (manifest, journal, host record).
  `<slug>` is the remote root's basename plus a short hash of its full path, so
  two remote roots with the same basename do not collide. Data, not cache: a
  cache directory may be wiped while `meta/journal` holds saves that have not
  reached the server. Overridable by `remote.mirror_root`. Mode 0700; it holds
  source code that lived behind ssh. The prefix costs roughly 60–80 bytes of
  `PATH_MAX` headroom that the remote tree does not pay, so a deep remote root plus
  a deep path inside it can exceed 4,096 locally when it fit on the host: the engine
  checks the longest manifest row against the mirror prefix at connect and fails
  there, naming `remote.mirror_root` as the shorter-prefix escape, rather than
  failing on one unlucky file halfway through a sync.
- **Content set, decided on the server.** In a git repository:
  `git ls-files --cached --others --exclude-standard -z`, i.e. tracked plus
  untracked-not-ignored; that is the set a developer means by "the project", and
  it excludes `build/`, `node_modules/`, `.venv/` without any list in a setting.
  Outside a repository: a filtered walk with the same hidden-directory rule the
  scanner uses. Then minus `project.files_exclude` and `remote.exclude`, plus
  `remote.include` for ignored paths the user wants to read anyway (a generated
  header, a `compile_commands.json`).
- **The content set is not the tree, and the difference is a product decision.**
  `git ls-files` lists *files*, and only non-ignored ones, so the mirrored tree
  differs from the host's in four ways that are decided here rather than
  discovered later. **Empty directories do not appear** — git cannot represent
  one, and `CaptureTreeSnapshot` excludes directories as well, so the mirror is
  structurally a file set with directories implied by their contents. **Ignored
  directories are invisible**: `build/`, `node_modules/`, `target/`, `.venv/`.
  Excluding them from *transfer* is the point; not **listing** them is the cost,
  and that is separate from not being able to **reach** them. A path
  in an ignored directory is still openable — a generated header, a build log, a
  dependency's source — through § 6.5's read-only `file/read`, which is what makes
  go to definition and step into work into those paths. What remains is that they
  do not appear in the tree, quick-open or search, so you reach one by following a
  reference to it rather than by browsing to it. `remote.include` promotes a path
  into the project properly when you want it indexed and editable; it is no longer
  the only way to see a file, which is what it was quietly being asked to be. **Submodules are
  not descended**: `git ls-files` reports a gitlink, not the submodule's files,
  so a submodule directory is listed and empty. Mirroring one means running the
  content-set command per submodule root, which Phase 3 does; until then a
  repository with submodules is a supported project with an unsupported subtree,
  and the tree says so on the gitlink row. **`.gitignore` is part of the content
  set, not just of the tree**: an agent editing it changes membership with no
  other file changing, so the agent treats a write to any `.gitignore` or
  `.git/info/exclude` as a content-set invalidation and re-runs the set command,
  taking the § 6.3 resync path if the membership diff is large. Without that
  rule, files silently enter or leave the project and nothing reports it.
- **A tree too large to mirror fails loudly, it does not truncate.** Every number
  in this document is this repository: 4,258 files, 26.4 MiB. `remote.max_file_bytes`
  caps a single file, and the primitives underneath already carry limits of their
  own: `CaptureTreeSnapshot` takes a `max_entries` budget and reports `truncated`,
  and `FileWatcher` carries a 50k-entry budget. What was missing was a cap on the
  *count*, and a decision about what happens at it. A silently truncated manifest is
  the one failure the content-state model cannot express — a file that is missing
  from the manifest is indistinguishable from a file that does not exist, so the
  tree is wrong with no `absent` marker to show for it. Therefore
  `remote.max_manifest_files` (default 50,000, matching the watch budget) is a
  hard limit: exceeding it fails the connection at `agent/hello` with the count
  and the limit, and points at Phase 4's on-demand tier. `truncated` from the
  snapshot is an error, never a shrug. The manifest itself is already chunked as
  `tree/rows` notifications, so the 64 MiB frame ceiling applies per chunk and
  the row count is not what bounds it.
- **`.git` is not mirrored.** Git runs on the server against the real
  repository; the mirror is the editor's working copy, not a second clone. Every
  local reader of `.git` (§ 2) takes `git/metadata` from the agent instead, and
  `is_git_repo_valid` answers from the host record, not from a stat. A mirror
  with `.git` would let a local `git commit` diverge from the truth, and its
  object store churns in ways a sync engine should not chase.
- **Metadata is complete; content is not, and never claims to be.** Every file
  carries a manifest row from the first round trip on, so the tree, quick-open and
  the file index are complete and stay complete. Content is a separate, lazy,
  prioritized thing, and every file is in one of **four** states the rest of the
  design branches on. The fourth state is load-bearing: without it this is a
  two-way comparison over three inputs, and § 6.3 says what that costs.

  | state | meaning |
  | --- | --- |
  | `current` | local bytes hash to the manifest row's `content_hash`, and to the base |
  | `stale` | a row arrived with a different hash **and** local bytes still equal the base; the host moved ahead and we did not |
  | `dirty` | local bytes differ from the **base**; we moved ahead, with or without the host |
  | `absent` | never fetched |

  **`stale` and `dirty` are the same observation until you keep the base.** Both
  are "the local bytes do not hash to the manifest row", and without a third input
  they are indistinguishable — so every local write that does not come through the
  editor's save path reads as the host having moved ahead, and the resolution for
  that state is to pull, which destroys the local write (§ 6.3). The base is the last
  content this mirror knew the host to hold for that path. It is not new
  bookkeeping: the object store already holds it, so `dirty` is one comparison
  against an object the mirror has. A file can be `dirty` **and** behind the host
  at once — local ≠ base ≠ remote — which is exactly a conflict, and it is the
  state the push's compare-and-swap is built to resolve. The rule the rest of the
  design leans on is short: **a pull never overwrites a path that is `dirty`.** It
  parks as a conflict instead.

  A file the user is not looking at is allowed to sit `stale` or `absent`
  indefinitely. **This is a steady state under agent churn, not a startup
  transient** — chasing every remote write with a content pull would keep the link
  saturated to keep bytes nobody is reading up to date.
- **Pull priority.** (1) open tabs and anything the dirty-buffer set names;
  (2) files the user opened this session, most recent first; (3) what the tree or
  quick-open is currently showing; (4) everything else, in batches of ~1 MiB or 64
  files, whichever comes first, so a cold mirror of this repo is ~30 batches, and
  **only while the link is otherwise idle**. Files above `remote.max_file_bytes`
  (default 8 MiB) are fetched on demand only. Opening an `absent` or `stale` file
  jumps it to the front and shows a one-line fetching placeholder for a round trip
  rather than a blank buffer or, worse, the old bytes.
- **Nothing reports an answer from `stale` or `absent` bytes without saying so.**
  Tree rows for `absent` files are dimmed. A `stale` open tab is refreshed before
  it is shown. Search does not read the mirror at all (§ 6.11). This is the rule
  the many-agents case turns from a nicety into a correctness requirement.
- **The mirror is an object store with a materialized working tree.** `tree/` is a
  real directory of real files, because that is what lets every unaudited
  `std::filesystem` site keep working (§ 2) — that does not change. What changes is
  what sits beside it: `meta/objects/` holds content addressed by hash, and the
  manifest is a `path → hash` map rather than a list of file facts. Three things
  fall out, and they are the difference between a mirror that survives agent churn
  and one that merely copes with it. **A branch switch transfers almost nothing**,
  because most files are byte-identical across branches and the mirror already has
  those objects — the 1,842-file resync of § 6.3 becomes a manifest diff plus a
  handful of fetches. **The journal becomes trivially correct**, because a queued
  push references immutable content by hash instead of describing a mutation that
  has to be replayed against a moving target. And **"known remote version" stops
  being bookkeeping** and becomes the object id itself. This is git's own model,
  which is fitting given § 6.2 already lets git define the content set.
- **The object store is bounded and swept, and the mirror costs more than the
  tree.** Two things the object-store decision implies. The mirror is **at least
  twice the tree** — every file exists once
  materialized in `tree/` and once as an object — plus every superseded version the
  agents have produced, and `meta/` is declared data rather than cache precisely so
  that nothing may wipe it. Left alone under continuous agent churn that grows
  without bound on the user's disk. So the store carries a policy rather than a
  hope: an object is **reachable** if it is a current manifest row's hash, a base
  for a path in `tree/`, or referenced by an unacked journal entry; everything else
  is sweepable. The engine sweeps on connect and on an idle timer, oldest-unreachable
  first, down to `remote.object_store_budget` (default 2 GiB, counted per mirror).
  Dropping an unreachable object costs at worst a whole-file transfer instead of a
  delta, never correctness. A reachable set that exceeds the budget on its own is
  reported, not swept — that is a tree too large for the mirror and § 6.2's ceiling,
  not a GC problem.
- **Running out of local disk is a first-class failure, not an I/O error.** The
  mirror is the thing that makes "nothing the user typed is ever lost" true, and
  every part of that promise is a local write: the journal entry, the object, the
  file in `tree/`. `ENOSPC` on any of them has to stop the sync engine loudly —
  pulls suspend, the status segment goes to an error tone naming the mirror path and
  the free space, and **pushes already journaled keep their content**, because
  discarding a journaled object to make room is losing exactly the bytes the journal
  exists to hold. A sweep runs first; if the reachable set alone does not fit, the
  project goes read-only-with-a-reason rather than silently half-syncing.
- **A stale file fetches a delta, not a file.** An agent rewriting one function in
  a 200 KiB source file is the single most common remote change in this workload,
  and re-pulling the whole file for it is the design's largest avoidable cost.
  Because the mirror has the previous object, the agent sends a `zstd --patch-from`
  delta against it and the client reconstructs. Whole-file transfer stays as the
  fallback when the base object is missing or the delta is not smaller.

  **This is a new third-party dependency on both sides and it is linked, not
  shelled.** `third_party/` holds one vendored library today (`stb`), so zstd and
  blake3 are both additions. Both ends need zstd — the agent to produce a delta, the client to reconstruct — so it is vendored
  and linked into `microide_kernel` alongside blake3, not invoked as a `zstd`
  binary: a subprocess per delta is a fork per changed file on the hot churn path,
  it makes the agent's install "one static binary **and** a zstd new enough for
  `--patch-from`", and the CLI's window-size flags become a correctness detail on
  files larger than the default window. Linking it makes the window an argument.
  Both libraries are built from their portable C sources with compiler-intrinsic
  dispatch, not from the per-architecture assembly files, so the static agent build
  does not pick up an assembler dependency per target.
- **Manifest rows** carry `(relative_path, kind, size, mode_bits, mtime_ns,
  content_hash)`. The hash is **blake3** (Groundwork G6), it is the object's
  address in `meta/objects/`, it is what a push presents for compare-and-swap, and
  it is the same hash the local external-change check uses — one hash, not a
  cross-machine one and a local one that resemble each other. mtimes are never
  compared across machines. Hashing 26 MiB on the server is a fraction of a second
  and runs once per manifest, incrementally after that (the watcher rehashes only
  what changed); blake3 rather than sha256 matters because manifest construction
  sits on the connect critical path (§ 9).
- **Symlinks** are reported in the manifest with their target; a link inside the
  root is recreated as a link, a link outside the root becomes a regular file
  holding the target's content (read-only; a save to it is refused with a clear
  message), and a directory link is listed but not descended (loop guard). This
  is a stated limitation, not a to-do.

  **The local writer resolves before it writes.** Validating the manifest row's
  path string against the mirror root is not sufficient once in-root symlinks are
  recreated as symlinks: a later row naming a path *through* one escapes the root
  on temp+rename, writing agent-supplied bytes outside the mirror. Pull writes
  therefore open with `O_NOFOLLOW` on the final component and verify the resolved
  parent is still under `tree/`, on the local side, in addition to the agent's own
  server-side confinement (§ 6.9). The agent is semi-trusted and its output is
  already treated as untrusted data everywhere else in this design; this is the
  one place where a string check reads as if it were a path check.
- **Exec bits** are preserved on pull and on push. Ownership is not transferred.

### 6.3 Sync semantics

- **The remote tree is the truth.** The mirror is a replica the user edits.
- **Remote → local, metadata.** The agent's `FileTreeWatcher` (real inotify)
  coalesces through `ProjectChangeCoalescer` and sends `watch/changed` batches of
  manifest rows plus deletes. Rows are cheap and always applied in full: the tree,
  the index and quick-open are never behind. A row whose hash differs from the
  local bytes marks that file `stale` **if the local bytes still equal the base**,
  and `dirty`-with-a-conflict if they do not (§ 6.2); either way it does not by
  itself pull anything.
- **Remote → local, content.** Only `stale` files that something is actually
  showing are pulled: open tabs first, then the § 6.2 priority order, then idle
  backfill. The pull writes with temp+rename into `tree/`, at which point the
  editor's existing `FileSignature` check sees an ordinary external change and the
  existing reload/banner flow applies unchanged — a clean tab reloads silently, a
  dirty one gets the banner. Renames are applied as renames, and the source that
  makes that possible is content addressing rather than inotify: inotify reports a
  delete and a create, and it is the object hash appearing at a new path that says
  they were one move. A tab follows its file.

  **A pull never writes over a `dirty` path.** It parks as a conflict (§ 6.3, below)
  exactly as a refused push does, and the file keeps the local bytes until the user
  chooses. Without it the sequence is ordinary and the loss is silent: a push is
  refused and parks, the local file correctly keeps the user's saved content, the
  agent writes that file again, the new row marks it different from the local
  bytes, the tab is open so it is first in the pull queue — and the pull replaces
  the user's saved work with the agent's. The buffer was clean, so the reload is
  silent and nothing reports it. That path needs no user mistake at all, and it
  falsifies § 1's "nothing the user typed is ever lost".

  **A pull and a local write of the same path are serialized**, by the per-path
  lock `MirrorWriteGate` holds. Both are a temp+rename into `tree/<path>` from
  different threads — the engine's and the shell's — and compare-and-swap protects
  only the host's copy. The window is small and the outcome is not: a pull landing
  between the user's local write and its push leaves the mirror holding the agent's
  bytes while the journal holds the user's, which is recoverable only because the
  journal references immutable content, and is not a state anything should have to
  recover from.

  **Deletes are applied, but a mass deletion is not.** A file that is gone must not
  stay openable, so an ordinary delete row applies immediately. A *diff* that
  deletes a large fraction of the mirror is a different event, and applying it
  because "the remote tree is the truth" is the single most famous way a sync
  product destroys its users' data. The resync path makes it more likely, not less,
  because a large batch is routed **towards** the wholesale manifest diff (§ 6.3,
  churn control). The causes are mundane: the remote root on an unmounted
  network filesystem, a `git ls-files` that failed and returned nothing, a root
  that was moved or reimaged. So there are two guards and both are required. The
  **agent** never reports an empty or shrunken content set as a success — it
  distinguishes "root unreadable / content-set command failed" from "root is empty"
  and fails the request in the first case, because an error that reads as "zero
  files" is how this ends badly. And the **client** refuses to apply a diff that
  deletes more than `remote.mass_delete_threshold` of the manifest (default 25%, or
  100 rows, whichever is larger): it holds the diff, leaves `tree/` untouched, and
  raises a blocking prompt naming the count and the host, with Apply and Disconnect.
  Nothing about a mirror is urgent enough to delete a thousand files without asking.
- **Churn control.** Agent bursts are the normal traffic, so the engine is built
  around them rather than around a human's occasional save. Batches coalesce on a
  250 ms window and collapse repeated writes to the same path to the last row, so
  an agent that rewrites a file five times costs one pull. Pull concurrency is
  capped (default 4 in flight) and the backfill tier yields to anything
  user-visible, so a 500-file refactor on the host never delays the file the user
  just clicked. If a batch exceeds `remote.resync_threshold` rows (default 2,000)
  the engine stops applying rows one by one and requests a fresh `tree/manifest`
  to diff wholesale — the remote counterpart of the "tree shape changed" escape
  hatch `FileIndexWatcher` already has.
- **Watch exhaustion on the host is reported, not absorbed.** `FileWatcher`'s Linux
  path returns false if any `inotify_add_watch` fails
  (`platform/FileWatcher.cpp:231`), which drops the whole watcher to polling. That
  is a reasonable local fallback and a poor remote one: `fs.inotify.max_user_watches`
  is a per-**user** kernel limit, the shared build box this design targets is where
  agents and several daemons are already consuming it, and the fallback is a
  periodic re-stat of a 50,000-file tree that something is rewriting continuously.
  So the agent reports which mode it is in at `agent/hello` and on transition, the
  status segment says `watch: polling` with the interval, and the failure names
  `max_user_watches` rather than presenting degraded freshness as normal. A polling
  agent still works; a user who cannot tell it is polling cannot explain why a
  change took 30 seconds to appear.
- **Branch switches are a named event, not a disaster.** `git checkout` and
  `git rebase` on the host change thousands of files at once **and** change the
  content set itself (different files are tracked). `git/metadata` reports the
  HEAD move, so the engine knows the large diff is coming before the rows arrive:
  it takes the resync path above, and the UI says *"host switched to `feature/x`,
  resyncing 1,842 files"* instead of showing a thousand individual changes. Open
  tabs whose file does not exist on the new branch are marked missing rather than
  emptied, and dirty buffers are never discarded — they become conflicts to
  resolve (a dirty buffer for a file the new branch does not have is offered as
  save-as or discard).
- **Local → remote.** A save lands on local disk first, synchronously, at local
  speed, so the buffer is safe before anything crosses the wire. The engine then
  sends `file/write {path, content, expect}` where `expect` is the known remote
  version's hash. The agent writes atomically **only if** the current remote hash
  equals `expect`, and returns the new hash, which becomes the known version.
  Tree operations (create, rename, trash, delete, from the sidebar) take the same
  route as `fs/op` requests with the same precondition where one applies.
- **The editor's save is not the only local writer.** The bullet above describes
  one writer; the tree has six, each of which mutates a file under the project root
  without going anywhere near `TextViewport::Save`.

  | writer | where | what it does |
  | --- | --- | --- |
  | plugin file API | `PluginWorkspaceInterop.cpp:187` | `files.write_text`, capability-gated, straight to `WriteTextFileAtomically` |
  | LSP resource ops | `LspService.cpp:1799-1977` | create / rename / delete with its own rollback journal |
  | LSP rename and code actions | `AssistService.cpp:1500` | *"The host decides whether to apply in place or open + save the files that are not currently open"* |
  | replace-in-project | `WorkspaceShellProjectSearch.cpp:112` | reads, substitutes, `WriteTextFileAtomically` per file — **not** the save path § 6.11 claims it takes |
  | merge writer | `WorkspaceCompareInteractionCoordinator.cpp:741,749` | writes the merge output, restores a backup |
  | sidebar file ops | `WorkspaceSidebarCoordinatorActions.cpp:56`, `FileOperationService` | create, rename, trash, delete |

  Each of these in a remote project writes into `tree/` and **stops there**: no
  push, no journal entry, nothing on the host. Worse than not arriving, the write
  is then *reverted* — the file no longer hashes to its manifest row, and without
  the base (§ 6.2) that is indistinguishable from the host having moved ahead, so
  the resolution is to pull. A plugin's write, an LSP rename across
  forty files, a replace-in-project over two hundred: applied locally, never sent,
  and silently undone the next time anything pulls that path.

  **Every write into `tree/` therefore goes through `MirrorWriteGate`, and there is
  a lint for it.** The gate takes the path, the new content or the tree operation, and the
  base the write was computed against; it takes the per-path lock, writes, records
  the new base, and enqueues the push or the `fs/op`. In a local project it is a
  pass-through, which is what makes converting the six sites a refactor with a local
  meaning rather than remote scaffolding: they currently have six private answers to
  "how do I safely replace a file", and `WriteTextFileAtomically` is only the
  innermost of them. The lint is the same shape as
  `CheckDescriptorCreationIsCloseOnExec` — no `WriteTextFileAtomically`,
  `fs::rename`, `fs::remove` or `fs::create_directories` naming a path under a
  project root outside the gate — because "everything takes the same route" is
  otherwise a convention, and a convention across six sites in four directories
  does not hold.

  **Two of the six need more than routing.** Replace-in-project cannot substitute in
  a file whose content is `absent`, since there are no local bytes to read: the
  matches came from the host (§ 6.11) and the file may never have been fetched. It
  promotes every file it is about to rewrite to the front of the pull queue and
  waits, exactly as compare does (§ 6.13) — which also bounds it, because a replace
  across a thousand `absent` files is a thousand-file pull and the progress row has
  to say so. And an LSP workspace edit is a *host-computed* edit applied to local
  bytes, which is § 6.5's rule, not this one.
- **Create carries `expect: absent`, and this is the one path that could lose
  remote bytes.** Compare-and-swap protects an *update*, because there is a prior
  hash to present. A create has none, and the tempting reading of "the same
  precondition where one applies" is that none applies. It does: the precondition
  for a create is that the file does not exist on the host. Without it, a file
  that is `absent` locally — present on the host, correctly never fetched, which
  § 6.2 says is an indefinitely valid steady state — can be created through the
  sidebar or through Save As, push with no precondition, and **overwrite content
  the user never saw and had no way to see**. That is reachable through ordinary
  UI, it destroys bytes rather than parking a conflict, and it is the only such
  path in this design. So `file/write` and `fs/op` both take `expect` as a
  three-valued field — a hash, `absent`, or `any` — `absent` is what a create
  sends, the agent implements it with `O_EXCL`, and a violated `absent` returns
  the same `conflict {current_hash}` an update's would. `any` exists for the
  Overwrite choice in § 6.3's conflict flow and is never a default.

  A **rename** has the same hole on its destination. `fs/op rename` carries `expect`
  on the source, and renaming onto a path that is `absent` locally but present on the
  host would clobber unseen bytes exactly as a create would. So the destination
  carries `expect: absent` too, implemented with `renameat2(RENAME_NOREPLACE)`, and a
  violated one returns the same `conflict {current_hash}`.
- **A file the user creates may not be in the content set, and then deletes
  itself.** The content set is `git ls-files --cached --others
  --exclude-standard` (§ 6.2) and the manifest is authoritative (§ 6.3), so a path
  that is not in the manifest does not exist. Put those two together and an ordinary
  action destroys its own result: create `debug.log`, or anything under an ignored
  path, or a `.env`, from the sidebar or Save As — it writes locally, pushes, the
  host writes it, the host's content-set command does not list it because it is
  ignored, the next manifest does not carry it, and the diff deletes it. An **empty
  directory** is the same story with no escape at all, since git cannot represent
  one: creating a folder in a remote project is a no-op that then removes itself.

  These are not edge cases, they are the first ten minutes of using the thing. So
  the mirror tracks a fifth membership state beside the content set: a path this
  client created or wrote that the host's content set does not list is
  **`local-only`**, recorded in `meta/` with its pushed hash, excluded from the
  deletion half of every manifest diff, and drawn in the tree with a marker whose
  tooltip says it is not part of the project's tracked set. It leaves that state the
  moment the host's content set does list it (an agent commits it, or `.gitignore`
  changes — which § 6.2 already treats as a content-set invalidation). A
  `local-only` path that the host reports as *deleted* is deleted; what it is
  immune to is being deleted for never having been listed. Empty directories are
  carried the same way, in `meta/` rather than in the manifest, which is the only
  place they can live given the content set is a file set.
- **Conflict.** If `expect` does not match, the push is refused and parked; the
  local file keeps the user's content. The shell shows the external-change banner
  with three choices: **Reload** (pull remote, discard local), **Overwrite**
  (push without precondition), **Compare** (a compare tab: local buffer vs the
  fetched remote content, using `CompareMergeService`; the user resolves and
  saves, which pushes). Nothing is done automatically; both sides' content exist
  until the user chooses.
- **Ordering with LSP.** `textDocument/didSave` is sent after the push is acked,
  not after the local write, or a server that re-reads from disk on save sees the
  previous content.
- **Journal.** Every parked or in-flight push, and every local tree operation not
  yet acked, is appended to `meta/journal` (through `PersistedRecordWriter`, the
  same record format as everything else the app persists) before it is attempted
  and removed when acked. A crash or a dropped link loses nothing; the next
  connection replays it, with the same compare-and-swap, so a file the server
  changed meanwhile becomes a conflict rather than a silent overwrite.

  **Replay is ordered, and the order is the journal's, not the queue's.** The
  journal holds two kinds of entry — content pushes and tree operations — and
  they are not independent: a push to `a.c` recorded before a rename of `a.c` to
  `b.c` must replay in that order, or the content lands at a path that no longer
  exists and the rename then moves stale bytes. Replaying tree ops first, or
  replaying pushes in path order, is the natural implementation and it is wrong.
  So entries carry a monotonic sequence number, replay is strictly sequential,
  and a replayed entry that conflicts stops the replay of *that path's* remaining
  entries rather than the whole journal, so one conflicted file does not park
  every unrelated queued save. A path with a parked entry keeps its later entries
  queued behind it, because applying them would skip the user's decision.
- **Reconnect.** A full manifest is fetched and diffed against `meta/manifest`;
  remote changes made while offline are pulled, then the journal is replayed.
  The tree stays usable throughout.
- **Debounce.** Local saves to the same file within 100 ms coalesce into one push
  (the last content wins locally, which is what the user sees). Remote batches
  arrive already coalesced.
- **Your own push comes back at you as a remote change, and content addressing is
  what makes that harmless.** The agent's watcher sees the write it just performed
  on your behalf and reports it in the next `watch/changed` batch. Under a
  mtime-and-size model that would be an echo needing suppression, a request id to
  correlate, and a race when a real remote write lands in the same window. Under
  content addressing the row's hash equals the hash the push acked, so the file is
  `current` and nothing happens. It is worth stating rather than leaving to be
  rediscovered: the echo is expected traffic, it is not filtered, and the reason it
  needs no filter is the same reason `object/fetch` is hash-keyed.
- **Format-on-save runs before the push, not after it.** `editor.format_on_save`
  is a shipped setting, and § 6.5 says everything that runs a command runs on the
  host — which, taken naively, makes a save into: write locally, push, the host
  formatter rewrites the file, `watch/changed` marks the tab stale, pull, reload.
  That reformats the buffer under the user's cursor a round trip after they saved,
  discards the undo stack's relationship to what is on screen, and can raise the
  external-change banner **against the user's own formatter**, which is an
  incomprehensible prompt. So the formatter is sequenced into the save instead of
  observed after it, and the sequence is G5's save pipeline (§ 8): snapshot the
  buffer, format the snapshot off-thread (one RTT to the host formatter, buffer
  still local), apply the result only if the buffer did not change meanwhile, then
  write locally, then push the formatted bytes. The save is one RTT slower and the
  budget in § 9 says so. A formatter that is not reachable fails the format and
  still saves, exactly as a local one does today.
- **Autosave pushes on the autosave interval, and that interval is the throttle.**
  `editor.autosave` with `autosave.delay_ms` means a dirty file pushes every
  interval, each push a CAS round trip and each one a conflict candidate under
  agent churn. The 100 ms save debounce is far below any autosave delay so it
  coalesces nothing here. This is acceptable rather than accidental: an autosave
  that does not reach the host is not an autosave, since the host is where the
  file is compiled, tested and read by the agents. What the design does not do is
  raise the banner for an autosave conflict — an autosave that hits a conflict
  parks in the journal silently and the tab is marked, because interrupting the
  user with a decision they did not ask for by typing is worse than waiting for
  their explicit save.
- **Conflicts are common here, and that is the design working.** With one human
  writer, a save colliding with a remote write is rare; with several agents it is
  routine. Two properties keep it tolerable. The user learns at the moment the
  agent writes the file (the `watch/changed` row marks the open tab stale and the
  banner appears), not ten minutes later at save time. And compare-and-swap means
  the late case still cannot lose bytes: the push is refused, both versions exist,
  the user chooses. What the design deliberately does **not** do is merge
  automatically. Auto-merging an agent's rewrite into a human's edit produces a
  file neither of them wrote.
- **Who changed what.** inotify reports paths, not processes, so per-file
  attribution to a given agent is not available at acceptable cost (`fanotify`
  would need `CAP_SYS_ADMIN` on the host). The design does not pretend otherwise:
  attribution is git's job, and the tracking surface microide offers is the
  **Changed on Host** list (§ 7.9) plus the branch and dirty state from
  `git/metadata`. If agents commit their work, the history answers the question
  properly; if they do not, the list at least says which files moved under you.

### 6.4 Transport and protocol

- One `ssh -o ControlMaster=auto -o ControlPersist=10m -o ServerAliveInterval=15
  -S <ctl>` per host. The agent is one channel on it; every process spawn is
  another channel on the same master, so a spawn costs milliseconds, not a
  handshake. `<ctl>` is `$XDG_RUNTIME_DIR/microide/ssh-<hash>` — short, because
  AF_UNIX paths cap at 108 bytes and a long path fails silently, which the control
  channel already learned the hard way.
- **Framing is length-prefixed binary, not JSON-RPC.** Reusing
  `JsonRpcMessageFraming` and `StdioJsonRpcClientTransport` is the right instinct
  for a control protocol and the wrong one for a bulk one. This protocol's traffic
  is not requests: it is file content, manifest rows and terminal output, and JSON
  charges for all three — every file
  body is an escaped string or base64 (+33%), a 50,000-row manifest is about 6 MB
  of JSON with 64-character hex hashes, and terminal payloads pay base64 on the one
  path where latency is felt directly. So a frame is a small fixed header
  (`length`, `type`, `id`) followed by raw payload bytes: control messages carry a
  JSON body, content messages carry the bytes themselves, and a hash is 32 bytes
  rather than 64 characters. The manifest is a packed columnar blob, streamed as
  `tree/rows` chunks exactly as before.

  The hardened parts of the existing stack are kept rather than rewritten: the
  bounded queues, the poll-based I/O thread, the wedged-peer teardown and the perf
  counters are transport behaviour, not codec behaviour, and `AsyncSubprocess`
  underneath is unchanged. What is replaced is the codec, and the 64 MiB ceiling
  stays as the per-frame bound.
- **File and terminal content travels as raw bytes in a content frame**, with no
  encoding step at all — no JSON string escaping, no base64, no `+33%`. It is worth
  recording the hazard that a text codec would carry, because any return to one
  reintroduces it: a terminal is a raw byte stream
  (`TerminalSession::SendBytes(std::string_view)`) chunked at whatever boundary the
  read returned, so a multi-byte UTF-8 sequence or an escape sequence splits across
  two messages routinely under load, and each half is individually invalid UTF-8
  that no JSON string can carry, so a text codec would need unconditional base64 on
  the one path where latency is felt directly. A binary one simply moves the bytes.
- **The scrollback line stream still has flow control.** Shipping screen state
  rather than bytes (§ 6.5) bounds the *visible* half of terminal traffic by
  construction, but the lines scrolling off the top are unbounded: `yes` or a
  verbose build produces completed scrollback lines faster than a far link drains
  them. The transport's bounded queues and wedged-peer teardown are what make it
  hardened, and they are exactly what would turn that into a **teardown that also
  kills file sync and every other terminal** — one careless command in one tab
  ending the whole session. `remote.host_scrollback_budget` does not help: it
  bounds host memory, not the wire. So the host applies a per-handle credit window
  (`remote.term_credit_bytes`, default 256 KiB outstanding, replenished as the
  client acknowledges) to the line stream, and lines produced beyond it are dropped
  at the ring with the same visible gap rule § 6.12 uses for overflow — a rule and
  a count, never a silent loss. The screen itself is never dropped, because the
  screen is what the user is looking at. Dropping backlog from a runaway command is
  correct behaviour; a user who wants all of it redirects to a file, which is a
  host-side operation and costs nothing on the wire.
- **The file channel needs the same head-of-line answer the terminal got.** § 6.5
  gives terminals their own ssh channel so a bulk transfer cannot block keystroke
  echo, and then the file channel is left carrying both the idle backfill and the
  file the user just clicked. Batching backfill in "~1 MiB or 64 files" (§ 6.2)
  bounds the *request*, not the delay: a 1 MiB batch already in flight on an 80 ms,
  20 Mbit/s link is most of a second the interactive fetch waits behind, and
  priority in a local queue cannot reorder bytes already on the wire. So the bound
  is **outstanding bytes, not batch size** — `remote.backfill_inflight_bytes`
  (default 256 KiB, the same shape as the terminal's credit window) caps what
  backfill may have unacknowledged, which is what makes the § 6.2 priority order
  mean anything. Interactive fetches are exempt from it and are what the backfill
  yields to. `op/cancel` covers the rest: an in-flight request whose reason has gone
  away (the tab closed, the query changed) is cancelled rather than waited out.
- **The protocol version is its own small integer, not the app version.** § 12
  worries about drift between a client and an agent someone copied to a host once,
  and tying the handshake to the release version answers it by forbidding drift
  entirely — which turns "copy one static binary" into "copy it again on every
  release, to every host, before you can open anything". A protocol version with a
  minimum-accepted floor on both sides lets a 2.14 client talk to a 2.12 agent for
  as long as the wire has not actually changed, and fails loudly and specifically
  when it has. The version a mismatch message names is still the agent's release
  version, because that is what the user has to act on.
- Methods (requests unless marked as notifications):

| method | direction | purpose |
| --- | --- | --- |
| `agent/hello` | → | **protocol** version (a small integer of its own, not the app version) plus a minimum the peer accepts, agent version, root, capabilities (`git`, `watch`, `search`), a `daemon_epoch` unique to this daemon process, and the content-set size. An out-of-range protocol version fails here naming both; so does a content set over `remote.max_manifest_files` (§ 6.2). |
| `tree/manifest` | → then ← `tree/rows` (notifications, chunked) | the full content set with hashes; the last chunk carries `complete: true` and a `manifest_id`. |
| `object/fetch` | → | batch of content hashes → object bytes, or a zstd delta against a base hash the client says it holds (§ 6.2). The client asks for content it lacks, not for a path whose content might have moved on. Bounded by outstanding bytes, not by batch count (below). |
| `file/read` | → | one **path** → `{hash, content}`, read-only, for a file the manifest does not carry: a header under an ignored `build/`, a dependency's source, a system include a stack frame names. Not root-confined, and § 6.9 says why that is not the boundary it looks like. Never writes, and what it returns lands outside `tree/`. § 6.5. |
| `op/cancel` | → (notification) | cancels an in-flight `search/run` or `object/fetch` by id. A far link plus a big tree makes an uncancellable request a stall the user can see and cannot stop; typing a new search query is the ordinary way to produce one. |
| `file/write` | → | `path, content, mode, expect` → the new `content_hash`, or `conflict {current_hash}`. `expect` is a hash, `absent` (create, `O_EXCL`) or `any` (the user's explicit Overwrite); there is no unconditional default. § 6.3. |
| `fs/op` | → | `mkdir`, `rename` (with `expect` on the source and `expect: absent` on the destination, `RENAME_NOREPLACE`), `trash`, `delete`. Carries the same three-valued `expect` as `file/write`. `trash` on a headless server usually has no XDG trash directory to move into, so the agent reports the capability in `agent/hello` and the sidebar's Move to Trash becomes Delete, named as such, rather than silently deleting under a label that promises recovery. |
| `watch/subscribe` | → then ← `watch/changed` (notification) | coalesced batches of manifest rows plus deletes. |
| `git/metadata` | ← (notification, on change) | branch, HEAD id, detached flag, pending merge/rebase state, upstream ahead/behind. Replaces every local `.git` read. |
| `search/run` | → then ← `search/results` (notifications) | the default path for project search and replace: the agent runs `ProjectSearchService` against the real tree and streams matches. § 6.11. |
| `agent/attach` | → | reattach to a running daemon with the client's last `manifest_id`; returns what changed since plus live terminal handles. § 6.12. |
| `term/open`, `term/screen`, `term/lines`, `term/input`, `term/event`, `term/resize`, `term/close` | ↔ | host-side pty and terminal-model lifecycle, on their own ssh channel. `term/screen` carries screen deltas, `term/lines` the completed scrollback lines (credit-windowed), `term/input` **semantic key and mouse events** rather than encoded bytes (§ 6.5), `term/event` the model's outward signals — OSC 52 clipboard, OSC 7 cwd, title, bell. § 6.4, § 6.5. |
| `term/scrollback` | → | older history for a handle from a given trim-total offset, for attach prefetch and for lazy backfill on scroll-up. § 6.12. |
| `agent/shutdown` | → | stops the daemon and its terminals; the transport's wedged-peer teardown covers the unclean case. Detaching a client is not a shutdown. |

Everything the agent returns is untrusted data: paths are validated to stay under
the mirror root before any write, sizes are bounded by the frame ceiling and
`remote.max_file_bytes`, and the decoder is a fuzz target (§ 10).

### 6.5 Remote processes and path translation

**Everything in this section computes on host bytes and reports against local
ones, so § 6.11's rule — nothing reports an answer from bytes it cannot vouch for —
governs all of it.** Translating *paths* is not enough, because a path is not the
whole of the correspondence: a diagnostic is a line number, a breakpoint is a line
number, a stack frame is a line number, a rename is a byte range, and every one of
them is computed against the host's copy of a file the mirror may hold at an older
version or not at all.

This is the same objection § 8 raises against shipping Phase 1 — *diagnostics land
on lines the buffer does not have, the debugger stops at a line number that means
something else in your copy, and `git status` describes a tree you are not looking
at* — and it applies here whenever a file is `stale`. The difference, and the whole
reason this is fixable while Phase 1 is not, is that there **is** a relation between
local and host bytes: the manifest hash. So:

- **Every host-computed position carries the hash it was computed against.** The
  LSP and DAP adapters on the host already know which file they read; the mapper
  records the manifest hash for that path at the moment of the reply, and the
  client compares it against the base for that path before rendering or applying.
- **A mismatch is never rendered as if it were current.** Diagnostics for a path
  whose hash does not match are held, not drawn at the wrong lines, and the path is
  promoted in the pull queue; when the content lands the held reply either matches
  or is discarded and re-requested. A breakpoint on a `stale` or `absent` file
  pulls before `setBreakpoints` goes out. A stack frame that lands on a mismatched
  file shows the fetching placeholder rather than the wrong line.
- **A host-computed *edit* is refused, not adapted.** An LSP rename or code action
  is a set of byte ranges against host content, and `AssistService.cpp:1500` applies
  it by opening and saving the files that are not currently open. Applying those
  ranges to older local bytes corrupts every file it touches, quietly, at a scale
  the action itself chose — a cross-file rename is dozens of files at once. So the
  whole edit is checked against the per-path hashes before any of it applies, which
  is the same all-or-nothing discipline `lsp_workspace_edit::VersionsCurrent` and
  `FlattenResourceOps` already use for their own reasons; a mismatch pulls the
  named paths and re-requests rather than adapting offsets. It then applies through
  `MirrorWriteGate` like every other writer (§ 6.3), so it pushes.

- **Git.** `ReadGitCommandOutput` (`GitCommandUtil.cpp:324`) is the funnel. Its
  spawn goes through the project's launcher (Groundwork G2), which in a remote
  project is `RemoteProcessLauncher`, and the cwd is translated with
  `RemotePathMap::ToRemote`. Paths in git's output (`status --porcelain`,
  `diff`, `blame`) are relative to the repo root and need no translation; the
  few absolute ones (`rev-parse --show-toplevel`, worktree lists) go through
  `ToMirror`.

  **Git describes the host's working tree, which is not what is on screen while a
  push is queued.** `git status` runs on the host, so a save that is
  written locally and not yet acked — every save while Offline, every parked
  conflict, everything in the journal — is invisible to it. The sidebar says clean
  and you have three unsent files; worse, **staging or committing from the sidebar
  commits the host's version of a file you have already edited**, silently dropping
  your unpushed change from the commit. So git's surfaces are gated on the journal
  being empty for the paths they touch: the git sidebar shows the pending-push
  count as a first-class row, a file with an unacked push is marked in the status
  list, and stage / commit / discard on such a file is disabled with a reason until
  its push acks. This is § 6.11's rule applied to git — the answer is computed from
  bytes the client knows are not the ones on screen, so it says so rather than
  presenting it as the truth.
- **LSP.** The server runs on the host, so `rootUri`, every `textDocument` URI
  and every `workspace/didChangeWatchedFiles` event name **remote** paths, and
  every URI in a reply names a remote path the editor must map back. The **12**
  files that call `FileUriForPath` / `PathFromFileUri` route through a
  `ProjectUriMapper` held by the project state: the identity mapper for a local
  project, `RemotePathMap` for a remote one. `LspService::Operations` gets the
  mapper; the transport does not know it exists.

  **One of those 12 files is not a project path.**
  `WorkspaceToolDownloader.cpp:49` calls `PathFromFileUri` on a download **URL**,
  not on a workspace URI, and routing it through the mapper would map a path that
  never belonged to either machine. "Inject the mapper at every `FileUri` caller"
  is therefore the wrong instruction, and the kind that passes review: the mapper
  belongs at the LSP and DAP protocol seams specifically, and `FileUri.h` stays a
  general-purpose converter.
- **DAP.** Source paths in `setBreakpoints`, `stackTrace` and `source` are
  translated the same way; `cwd` and `program` in a launch config are remote
  paths already (the user wrote them for the box the program runs on) and pass
  through unchanged.
- **`ToMirror` is partial, and the paths it cannot map are ones the user wants to
  open.** Neither direction can be total: the mirror holds the content set, and the
  host names paths outside it constantly. Go to
  definition lands in a generated header under `build/`; step into lands in a
  dependency's source or `/usr/include/c++/…`; a stack frame names a file in an
  ignored directory; a `search/run` hit names a file the content set excludes. Each
  of those is `ToMirror` returning nothing, and the three available behaviours —
  fabricate a mirror path that does not exist, drop the navigation silently, or say
  "not available" — are all wrong for what is an ordinary, daily action.

  This is not a browsing preference about reading a build log; it is
  go-to-definition, and an editor whose go-to-definition works except when it
  matters is not usefully a remote editor. A host path that does not map to the
  mirror
  is fetched read-only with `file/read` and materialized under
  `meta/external/<host-path-hash>/`, opened as a read-only buffer labelled with its
  host path. It is outside `tree/` deliberately: it is not part of the project, it
  must never be pushed, it must never appear in the content set or in a manifest
  diff, and it is swept by the same GC that sweeps objects (§ 6.2). Editing one is
  refused the way any read-only buffer is. `remote.include` (§ 6.2) is then an
  optimization for a path you want *in* the project — indexed, searchable, editable
  — rather than the only way to see a file at all.
- **Terminal — the pty lives on the host, not locally.** Keeping a local pty and
  `execl`ing `ssh -tt` is simpler and it is wrong for this workload: the remote shell
  is then a child of the ssh connection, so a dropped link (a closed laptop, a
  sleeping VPN) sends `SIGHUP` to every remote shell and **kills whatever was
  running in it** — the build, the test run, the agent the user started by hand.
  Losing a 40-minute build to a lid close is not an acceptable failure mode for a
  remote-first editor.

  Instead the **agent owns the pty and the terminal model**. It already has the
  code: `TerminalBackend`'s `posix_openpt` + `O_CLOEXEC` path and `TerminalSession`
  itself compile into the same binary once Groundwork G1 has taken SDL out of the
  terminal's data types. A terminal tab becomes `term/open {cwd, shell, rows,
  cols}` → a handle; `term/resize` replaces relying on ssh's window-change
  propagation; `term/close` replaces `RequestTerminalChildShutdown`.

  **The host ships screen state, not bytes, and this is where the remote terminal
  stops feeling remote.** Host-owned pty with a local parser would mean parsing the
  stream twice and crossing the link with raw output — the full 10 MB/s of a noisy
  compile, every byte of which the local parser throws away as it scrolls past. With
  the model on the host, the wire carries **screen deltas plus completed scrollback
  lines**: a screen has a few thousand cells and a bounded update rate
  no matter how loudly the program writes to it. A build that emits a megabyte a
  second costs the host one parse and the link a few kilobytes of visible change.

  Three things fall out rather than needing to be designed. Reattach becomes a
  screen snapshot instead of a replay. Full-screen programs — `vim`, `htop` — are
  *exactly* a screen delta, so they need no special case. And the credit window
  below is not load-bearing for output volume, because a screen cannot outrun
  itself; it bounds the scrollback line stream, which is the part that is genuinely
  unbounded.

  The local side keeps selection, find and rendering, operating on the scrollback
  lines and screen it is given.

  **Input cannot be raw bytes.** Keystrokes travel as bytes while the model is
  local, because the thing that turns a key press into bytes *is* the model. Moving
  the model to the host leaves the encoder on the wrong side of the link, and the
  encoder is not a pure function of the key — it reads mode state the host owns:

  | encoder input | where it comes from | set by |
  | --- | --- | --- |
  | `application_cursor_keys_mode` | `FormatTerminalKeyPress`, `TerminalSessionInputEncoding.h:31` | DECCKM, on entering/leaving a full-screen program |
  | `kitty_flags` | same | the Kitty keyboard protocol handshake |
  | `bracketed_paste_mode` | `FormatTerminalPasteBytes`, same header | DECSET 2004 |
  | mouse tracking any / drag / normal | `CurrentTerminalMouseTrackingMode`, same header | DECSET 1000 / 1002 / 1003 |
  | `mouse_sgr_ext_mode`, rows, columns | `TerminalMouseEncodeRequest`, `TerminalMouseEncoder.h:26-35` | DECSET 1006, resize |

  Every one of those flips while a program starts and exits. A client encoding
  against a mode set that is one round trip old sends the wrong bytes at exactly
  the moment the modes change — press an arrow as `vim` exits and it goes out in
  application-cursor form to a shell that reads it as an escape and a letter; paste
  as a program clears 2004 and the bracket markers arrive as literal text. Shipping
  the modes down with each screen delta does not fix it either, because the race is
  the round trip itself, not the absence of the data.

  So `term/input` carries **semantic events** — a key press as keysym plus
  modifiers, a mouse event as button, action and cell coordinates — and the host,
  which owns the modes and is the only side that can be sure of them, encodes. The
  payload is smaller than the bytes it replaces, ordering is unchanged, and the
  latency is identical: this costs nothing and removes a whole class of
  wrong-at-the-boundary bugs. It is protocol shape, so it is free to decide now and
  a break to change later.

  **The model's outward signals need a channel back, and OSC 7 is not the only
  one.** Besides `reported_working_directory()`, the parser on the host produces the window/tab title, the bell, and OSC 52
  clipboard writes (`TerminalOscClipboard.h`) — and a clipboard write whose whole
  purpose is to reach the user's clipboard is useless on the server. `term/event`
  carries all four. OSC 52 specifically arrives as a *request* the local side
  applies under the same policy a local terminal uses, because a remote program
  writing your clipboard is a capability worth keeping deliberate.

  **There is no local echo prediction.** § 1 says nothing that pays a round trip
  per keystroke can be the primary mode, and the remote terminal pays exactly that:
  keystroke to echo is 1 RTT, and § 9 budgets it as such. That constraint is about
  the *editor*, where a per-keystroke round trip would make the product unusable and
  where the mirror removes it entirely. A terminal is different in kind — it is
  already a remote conversation, and every ssh user accepts this latency — but the
  asymmetry is stated here so it does not read as an oversight. Predicting echo
  locally, mosh-style, would mean a second terminal model on the client guessing at
  the host's, which is the duplication shipping screen state removed.

  **Terminal I/O gets its own ssh channel**, a second `microide-agent --terminals`
  on the same ControlMaster. Sharing one channel with manifest pulls and file reads
  would let a multi-megabyte transfer head-of-line block keystroke echo, which is
  the one latency a user feels directly. A separate channel on an existing master
  costs milliseconds to open and removes the coupling entirely.

  `remote.shell` (project scope) overrides the remote login shell. A dropped link
  no longer ends the session; see § 6.12.
- **OSC 7** cwd reports from a remote shell name server paths. The first consumer
  of `reported_working_directory()` (reveal-in-tree, open-terminal-here) maps
  through `ToMirror` or it resolves a server path against the local disk.
- **A local terminal in a remote project is available but never the default.**
  The constraint in § 1 is that a local shell must not *masquerade* as a remote
  one, not that it must be impossible — local tooling is a real need. `Remote: Open
  Local Terminal` opens one, and it is labelled `local · bash` against the remote
  tabs' `build-box · zsh`, so the two are never confused. Every other path to a new
  terminal in a remote project is remote.
- **Everything that runs a command takes the same route** — tasks, `launch_label`
  paths, formatter and tool invocations — or a session silently splits across two
  machines.

### 6.6 Connection lifecycle and authentication

```
 Disconnected ─▶ Connecting ─▶ StartingAgent ─▶ Syncing ─▶ Ready
       ▲             │                              ▲         │
       │             ▼ (ssh exits 255 / auth)       │         ▼ (link lost)
       │        NeedsAuth ──(terminal tab: ssh -N)──┘     Reconnecting ─▶ Offline
       └────────────────────── user: Disconnect ◀─────────────┴───────────┘
```

- **Connecting** runs `ssh -o BatchMode=yes -S <ctl> -o ControlMaster=auto
  -o ControlPersist=10m -fN <host>`. `BatchMode` makes a passphrase, password or
  2FA prompt fail fast instead of hanging on a TTY nobody can see.
- **NeedsAuth** opens a terminal tab in the panel running the same command
  *without* `BatchMode` and *with* a pty: the user answers the prompt in the
  terminal they already have. The session watches for `<ctl>` to appear and
  continues; the tab reads "connected, you can close this". No password field
  ever exists in microide; no credential is stored.
- **StartingAgent** attaches to the host daemon if one is running for this root,
  and starts one if not (§ 6.12), then completes `agent/hello` / `agent/attach`.
  A reattach here is the common case, not the exception: it is what reopening a
  laptop does. `ssh: command not found` or a version mismatch is a terminal
  error for this attempt with the install hint (the `.deb` name and
  `remote.agent_command` to point at a non-PATH binary).
- **Syncing** is § 6.2's population; the project is usable from the manifest on.
- **Reconnecting** applies exponential backoff from 1 s to 30 s while the tree
  stays editable; **Offline** is the user-visible name after the first failed
  retry, and after `Disconnect`. Reconnect is automatic unless `remote.reconnect`
  is off.
- **Session restore with the host down** opens the project from the mirror
  immediately, in Offline, with every tab restored. Startup never waits on the
  network; connection is attempted after the first frame.

### 6.7 Offline behavior

Offline is a property of the *link*, not of the host: the daemon, its watch and
its terminals keep running on the server (§ 6.12), and reconnecting picks them up.
Locally, everything that reads or writes the tree works: open, edit, save,
quick-open and compare between local files. **Search falls back to the local mirror and says so**
— its header names the fallback and the last sync time, because offline results
come from whatever bytes were pulled before the link dropped (§ 6.11). Files that
were `stale` or `absent` at disconnect stay that way and are marked in the tree, and
a `dirty` one keeps the user's bytes and is never pulled over on reconnect (§ 6.3);
opening one reports that its content is not available offline rather than showing
an older version as if it were current. Saves queue in the journal and the status
segment counts them. Terminal tabs stop updating and are marked **detached**
rather than closed — their shells are alive on the host and their output is
buffered for reattach; LSP and DAP sessions end and their status shows
disconnected; git actions are
disabled with a tooltip naming the state; the remote-derived git metadata stays
at its last value with a stale marker. On reconnect the queue flushes (§ 6.3).

### 6.8 Identity and persistence

- **Identity is a `ProjectId`, not a path** (Groundwork G8). Keeping identity
  path-shaped works and then costs § 7.5 a leak-suppression exercise at every
  presentation site, where missing one shows the user a directory under
  `$XDG_DATA_HOME` and calls it their project. A `ProjectId` carrying
  `Location { Local(path) | Remote(host, path) }` makes that leak structurally
  impossible and makes § 6.13's "a project is local or remote, never both" a
  property of the type rather than a rule reviewers enforce. The mirror `tree/`
  path is still the root every filesystem site sees; it is simply no longer the
  thing that *names* the project.
- **This costs a persisted-format migration, and that is accepted.** `project_roots`,
  `recent_project_roots`, breakpoints and the per-project state directory key are
  all path-keyed today. They migrate to `ProjectId`, through `PersistenceService`
  and `PersistedRecordReader`/`Writer` like everything else; a record written by an
  older build reads as `Location::Local(path)`, which is what it meant. Avoiding
  the migration is possible and not worth it: it buys compatibility this project
  does not want at the price of a leak it does.
- The remote half of a `ProjectId` carries `host, remote_root, user (optional),
  last_manifest_id, last_connected_at`, written through the same persistence path.
  The per-project state directory is keyed by the `ProjectId`, not by the mirror
  path, so `remote.mirror_root` can change without orphaning a project's settings,
  session or breakpoints.
  Presentation (`WorkspaceProjectPresentation`, the welcome recents, the window
  title) renders a `ProjectId` and therefore shows `host:/remote/root` by
  construction rather than by remembering to.
- **Remote: Remove Mirror** deletes `tree/` and `meta/` after the dirty prompt and
  a confirm that names the pending-push count; it is the only way a mirror is
  removed. Nothing prunes mirrors automatically.

### 6.9 Security posture

- The agent refuses any **write** to a path that does not resolve (after symlink
  resolution on the server) under `--root`; `..` and absolute paths in write
  requests are rejected before any I/O. Every mutating method — `file/write`,
  every `fs/op` — is confined, without exception.
- **`file/read` is deliberately not confined, because read confinement is not a
  boundary here.** § 6.5 needs to open a system header a stack frame named and a
  generated file under an ignored directory, so a blanket "confine every path" makes
  go-to-definition fail on exactly the files it is most often used for. Confinement
  is worth keeping for writes and is theatre for reads: the same authenticated ssh connection carries `RemoteProcessLauncher`, and
  the user has a shell on that host in a terminal tab, so anything `file/read` could
  return is already one `cat` away. What it must not do is let a *read* become a
  write or a foothold, so it never follows a path into a write, its results are
  materialized under `meta/external/` and never inside `tree/`, they are opened
  read-only, and they never enter a manifest or a push. The agent still refuses to
  read a path the client did not receive from the host in a reply of its own.
- Remote execution is **unsandboxed**: `SubprocessSandbox` confines the local
  `ssh` and nothing on the server. `SECURITY.md` states this, and the plugin
  trust model states that a plugin-issued command in a remote project runs on
  the host with the user's remote privileges.
- Host-key verification, agent forwarding, identities and jump hosts are ssh's:
  `remote.ssh_options` appends to the command line and `~/.ssh/config` is
  honored by construction. microide never disables `StrictHostKeyChecking`.
- `<ctl>` lives in a 0700 directory under `$XDG_RUNTIME_DIR`; the mirror is 0700.
- **The agent socket refuses a world-writable parent rather than falling back to
  one.** See § 6.12: on a host with no `$XDG_RUNTIME_DIR` the tempting fallback is
  `/tmp`, and a predictable socket path under a world-writable directory on a
  shared build box is a hijack, not a leak.
- **Local pull writes resolve before writing** (§ 6.2), so a recreated in-root
  symlink cannot be used to land agent-supplied bytes outside the mirror. The
  agent's server-side confinement and the client's local check are both required;
  neither is a substitute for the other.
- The agent's stdin is a hostile-input surface and is fuzzed (§ 10).

### 6.10 Thread discipline

The engine thread and the transport's I/O thread do all waiting. Results reach
the shell thread through the SDL wake used by `ControlChannelService` and the
LSP client. `platform::RunSubprocess` stays banned in workspace units; the ssh
spawns for git go through `ProjectBackgroundExecutor` exactly as local git does,
and the RTT they now carry is why that invariant matters more, not less. The
perf harness gets a scenario with a stalled fake agent asserting that no shell-
thread frame exceeds 16 ms while the engine waits (§ 9).

### 6.11 Search runs on the host

Search is the one subsystem where the many-agents constraint forces the answer to
*move machines*, rather than to wait for a pull. The rule behind it — nothing
reports an answer from bytes it cannot vouch for — governs compare (§ 6.13) and
everything in § 6.5 as well; those resolve it by fetching first, because they touch
a handful of files. Search cannot, because pulling the whole tree to answer a query
is absurd, and that is the whole of why it is different. `ProjectSearchService` reads real bytes per
candidate file (`ProjectSearchService.cpp:382`) and has no notion of a content
version, so searching a mirror that is `stale`/`absent` in places returns results
that are **wrong without saying so**: hits from bytes the host has replaced, and
misses in files never fetched. Under continuous agent writes that is the ordinary
state of the mirror, so "search the mirror and warn when it is incomplete" is not
good enough — the warning would be permanent and therefore ignored.

Therefore, for a remote project in the Ready state, **`search/run` on the agent is
the default**, not a Phase 3 optimization for big trees. The agent runs the
existing `ProjectSearchService` in-process against the real tree and streams
`search/results`; the local side renders them through the existing panel, mapping
paths with `RemotePathMap`. This is also simply faster: no bytes cross the wire
except the matches.

Two fallbacks, both explicit in the UI:

- **Offline.** The agent is unreachable, so search runs locally over the mirror
  and the panel header says *"offline — searching your local copy, last synced
  14:02"*. Results are labelled, not silently served.
- **`remote.search = local`.** For someone who wants offline-style search always.
  The same header label applies whenever it is in effect.

**The host's search scope is the content set, not the host's tree.** The agent
runs `ProjectSearchService` in-process against a real directory, and that directory
contains everything the content set excludes: `build/`, `node_modules/`, `.venv/`,
`.git`. Left alone, remote search returns hits in files the mirror has no row for
and no path to, so opening a result is the `ToMirror` failure of § 6.5 reached
through the most ordinary action in the panel. The agent therefore filters
`search/run` to the content set by construction, and a hit outside it — which can
only come from `remote.include` or from a path the user reached through § 6.5's
read-only fetch — is returned labelled, opening read-only rather than pretending to
be a project file.

Replace-in-project follows search, and what it does today is not the save path: it
reads each file,
substitutes, and writes with `WriteTextFileAtomically` per file
(`WorkspaceShellProjectSearch.cpp:112`), aborting the whole operation if any target
is open and dirty. Two consequences. It is one of the six writers that must route
through `MirrorWriteGate` (§ 6.3), or a two-hundred-file replace lands locally,
pushes nothing, and is silently reverted file by file as the mirror pulls. And it
**cannot substitute in a file whose content is `absent`** — there are no local bytes
to read, because the matches came from the host and the file may never have been
fetched — so it promotes every file it is about to rewrite to the front of the pull
queue and waits for them, with the progress row saying so, before it writes
anything. A file an agent changed between the search and the replace then becomes a
conflict rather than a clobber, which is what the old sentence promised and the
pull-first ordering is what makes true.

### 6.12 The agent is persistent: detach and reattach

A laptop closes. A VPN drops. A user goes home and opens the same project from a
different machine. In all three the work on the host — a build, a test run, an
agent mid-refactor — must still be running, and reconnecting must show it rather
than restart it. That requires the agent to **outlive the ssh connection**, which
an agent spawned as a child of ssh does not: it dies with the connection.

- **The agent is a daemon keyed to `(uid, remote_root)`**, not a child of a
  connection. It binds an AF_UNIX socket at
  `$XDG_RUNTIME_DIR/microide/agent-<hash>.sock` on the host and keeps running when
  the connection goes away.
- **`$XDG_RUNTIME_DIR` is frequently unset on exactly the connection this design
  uses, and the fallback must not be `/tmp`.** It is set by the session manager
  for a login session; a non-login, non-interactive `ssh host -- command` — which
  is every connection here — often does not have it. This tree already met the
  problem and its answer is a warning, not a precedent to copy:
  `ControlChannelService` falls back to `/tmp`, records that the parent is then
  **world-writable** (`ControlChannelService.cpp:258`), and treats every descriptor
  found there as untrusted input (`:32`). For the control channel that is a
  containable risk. For the agent it is not: the socket path is derived from a
  hash of the root, so it is *predictable*, and a shared build server — precisely
  where agents run as a service user — lets any local user pre-create that path,
  accept the attach, and receive read and write access inside the project root
  over a connection the user authenticated. So the agent resolves its runtime
  directory in order — `$XDG_RUNTIME_DIR`, then `/run/user/<uid>` if it exists and
  is owned by the uid and mode 0700 — and if neither is available it **refuses to
  start**, reporting that the host has no per-user runtime directory and naming
  `remote.agent_socket_dir` as the override for a host where the administrator has
  provided one elsewhere. Before binding it verifies the parent's owner and mode,
  and it never adopts an existing socket it did not create. Failing to start is a
  legible error; starting on a hijackable path is a silent compromise.
- **This machinery already exists and is hardened.** `platform::ControlSocketServer`
  is an AF_UNIX server with one poll-based I/O thread, per-client fd management, a
  socket created 0600, stale-socket removal on bind, and recovery for the case
  where `$XDG_RUNTIME_DIR` is cleaned out underneath a live process. It was written
  for the control channel; the agent is the second user. The 108-byte AF_UNIX path
  cap that bit the control channel applies here too, which is why the socket name
  is a hash rather than the root path.
- **Connecting is attach-or-start.** `ssh -S <ctl> <host> -- microide-agent
  --root <path> --attach` connects to the existing socket and relays stdio to it;
  if no agent is running it starts one, daemonizes it and relays to that. The local
  side does not care which happened, beyond what `agent/hello` reports.
- **Daemonizing detaches stdio explicitly, or the connection that started it never
  returns.** `ssh -- command` does not exit while any process holds the inherited
  stdout or stderr open, so a daemon that forks and keeps them — the default if
  nobody thinks about it — leaves the starting `ssh` alive forever and
  `StartingAgent` waiting on a handshake that already succeeded. The failure looks
  like a hang in connection setup and has nothing to do with the protocol. So the
  started daemon closes and reopens 0, 1 and 2 on `/dev/null` (its diagnostics go
  to a log file under its runtime directory) and `setsid`s before the relay client
  reports success, and the relay distinguishes *started* from *still holding my
  descriptors* by waiting on the socket becoming connectable, never on the spawn
  exiting. This is the same descriptor discipline `CheckDescriptorCreationIsCloseOnExec`
  enforces locally, applied to the one process this design starts on a machine the
  lint cannot see.
- **`agent/attach` carries the last `manifest_id`.** A reattaching client says what
  it already knows and gets back only what changed since, plus the set of live
  terminal handles. A cold client omits it and gets a full manifest. Designing this
  in from the start matters: an agent protocol that assumes a fresh start per
  connection cannot be retrofitted with resume without a protocol break, because
  the server has no reason to have kept the state.

  **A `manifest_id` is scoped to a daemon instance, not to a tree.** The resume
  token means "send me what changed since", which only a process that *watched*
  the interval can answer. A daemon that idle-exited and was restarted by the next
  connection — the ordinary case after 30 minutes, and § 6.6 says reattach is the
  common path — has watched nothing, and an id derived from tree content would let
  it accept a token whose interval it cannot account for and reply "nothing
  changed". That is a resume that silently skips every change made while nobody was
  attached, and it looks exactly like a working reattach. So the id is
  `(daemon_epoch, sequence)` with `daemon_epoch` unique per daemon process
  (§ 6.4's `agent/hello`), an id from a different epoch is refused rather than
  interpreted, and the client falls back to the full manifest diff it would have
  done cold. That diff is cheap — it is why the mirror survives a week away — so
  the conservative answer costs nothing and the optimistic one is a correctness
  hole.
- **Terminals survive, and so does their history.** The shells are children of the
  daemon, holding host ptys, so a dropped link is invisible to them.

  **The host holds the authoritative scrollback**, not just the bytes produced
  while nobody was attached. Buffering only the detached window would be enough
  for the same client reconnecting — it still has its own scrollback in memory —
  but it is wrong for the two cases § 6.12 exists to support: attaching from a
  *different machine*, and reopening after restarting microide. Both have an empty
  local buffer, and a live terminal showing no history is not a reattached session.
  So there is **one** scrollback per terminal, it lives with the pty on the host,
  it is sized by the existing `terminal.scrollback_lines` (default 2,000, up to
  100,000), and the local terminal renders a view of it. This is the same rule the
  mirror already follows: the side that owns the thing is the truth.

  **Resume carries an offset, and the primitive already exists.**
  `TerminalSession::ScrollbackTrimTotal()` is a monotonic count of lines trimmed
  off the front, and the scrollback read API already takes an `expected_trim_total`
  so a reader can resume across a concurrent trim. The protocol carries that same
  counter per handle: a client says which line it has through, and the host sends
  from there. Nothing is re-sent and nothing is duplicated, which is exactly the
  role `manifest_id` plays for files.

  **What arrives on attach is the tail, not the archive.** The visible screen plus
  `remote.scrollback_prefetch_lines` (default 500) comes immediately so the tab
  paints in one round trip; older history is backfilled lazily when the user
  scrolls up. Shipping 100,000 lines × N terminals before the first repaint would
  make reattach slower than reconnecting the files, which is the wrong trade.

  **A build that finished while the laptop was shut** lands in the scrollback with
  its exit line, and a handle whose shell exited while detached retains its final
  output and exit status until a client has seen it, rather than being reaped —
  otherwise the one thing you reattached to find out is the one thing missing.

  **Full-screen programs need no special case.** The alternate screen has no
  scrollback by construction, so reattaching to `vim` or `htop` wants the current
  screen and nothing else — which, now that the host ships screen state rather than
  bytes (§ 6.5), is simply what a `term/screen` message already is. Under a
  byte-replay model this would be an exception to name; here it is the ordinary
  path, and the scrollback prefetch below is what is exceptional.

  **Overflow is marked, never silent.** A verbose build that outran the ring while
  detached drops the oldest lines and the tab shows a rule saying so, with the
  count. A gap the user can see is recoverable; one they cannot is a bug report.
- **Two clients may attach at once**, which is what "open it from the other
  machine" means. Both get the same events; terminal handles are shared and their
  output fans out to both. This is a deliberate small amount of collaboration
  falling out of the daemon model, not a collaboration feature — there is no shared
  cursor and no shared buffer state, because each client has its own mirror.
- **Settings the daemon consumes arrive at attach, and the value that protects the
  host wins.** `terminal.scrollback_lines`, `remote.host_scrollback_budget`,
  `remote.max_manifest_files`, `remote.term_credit_bytes` and
  `remote.agent_idle_timeout` are read on the host by a process that has no settings
  registry and may be serving two clients with different values. Each client sends
  its values in `agent/hello` / `agent/attach`. The daemon applies, per setting, the
  **minimum** of any memory budget or cap and the **maximum** of any timeout or
  history size, and reports the effective values in the reply, so a client whose
  request was overridden can show that in `Remote: Show Status` rather than
  wondering why its scrollback is shorter than it asked for.
- **Lifetime.** The daemon exits after `remote.agent_idle_timeout` (default 30
  minutes) with no attached client **and** no live terminal handle; a running
  terminal keeps it alive indefinitely, because that is the whole point. `Remote:
  Stop Host Agent` ends it explicitly. A version mismatch between a reattaching
  client and a running daemon is reported with both versions and offers to restart
  the daemon, which ends its terminals — stated plainly, because it is the one
  routine action that loses running work.
- **The mirror is unaffected either way.** It is local, on disk, and journaled, so
  a client that reattaches after a week is a manifest diff, not a re-clone.

What this does **not** do is move the editing session to the host. Tabs, layout,
cursors and undo stay local, in the mirror's project state, and restore from there
as they do for any local project. The thing that reattaches is the host-side work:
the file state, the watch, and the terminals.

### 6.13 Local and remote projects side by side

A remote project is an **ordinary project** whose filesystem root happens to be a
mirror directory. Nothing in the catalog special-cases it: `ProjectWorkspaceState::root`
is the mirror's `tree/` for every I/O site, and the `ProjectId` (§ 6.8) is what
distinguishes it in persistence and presentation. So local and remote project tabs
coexist in one window and you can work on both at once.

This is not something the design has to add, because the isolation already exists.
Everything that could collide is a member of `ProjectWorkspaceState`, one per
project:

| per-project, already | consequence |
| --- | --- |
| `terminal_tabs` | a remote project's terminals are on its host; a local project's are local. Switching tabs cannot run a command on the wrong machine. |
| `lsp_manager`, `dap_manager` (each `unique_ptr`, per project) | a remote project's language servers and debug adapters run on its host while a local project's run locally, at the same time, with no shared state. |
| `file_index`, `directory_tree`, project search state | scope, results and the tree belong to one root. |
| git state, breadcrumbs, breakpoints, project-scoped settings | already keyed to the project. |

Three rules make it safe rather than merely possible:

- **A project is local or remote, never both.** Mixing files from two machines
  inside one project would make search scope, the git repository, the language
  server root and the terminal's working directory each ambiguous, and every one of
  those ambiguities resolves silently and wrongly. A file opened through
  `Open File…` that falls outside the active project's root keeps working as the
  loose file it already is today, and is labelled with its host (or `local`) in the
  tab tooltip so its provenance is never a guess.
- **An open remote project keeps syncing while another tab is active.** It is
  *open*, not *active*, that holds the host session: agents keep changing the tree
  whether or not you are looking at it, and returning to a tab that stopped
  watching an hour ago would mean a cold re-diff at exactly the moment you want to
  read something. The daemon's idle timeout (§ 6.12) is keyed to the project being
  **closed**, not to it being switched away from.
- **The host is visible everywhere the project is.** With a local and a remote
  checkout of the same repository open, the two project tabs would otherwise carry
  the same name. The host badge on the project tab, the `host:/path` breadcrumb and
  tooltip, the status segment and the `build-box · zsh` / `local · bash` terminal
  labels (§ 7.5) exist for exactly this case, and they are what keeps "work on both
  at once" from becoming "run the wrong build".

One thing falls out for free: **a remote project that loses its link degrades to
an offline local project** (§ 6.7) while your genuinely local project tab carries
on untouched.

One thing looks free and is not. **Comparing your local checkout against the
server's version is mechanically an ordinary compare** — the mirror is a real
local directory, both sides are local paths, and `CompareMergeService` needs no
remote awareness — but the answer it gives is wrong unless the mirror side is
`current`. A `stale` or `absent` file compares your checkout against *what the
mirror last pulled*, not against "the server's version" as the sentence promises,
and § 6.2 says stale content is an indefinite steady state rather than a startup
transient. This is § 6.11's rule exactly: nothing reports an answer from bytes it
cannot vouch for. Search resolved it by moving to the host because pulling the
whole tree to answer a query is absurd; **compare resolves it by pulling**,
because a compare touches two files and a two-file pull is one round trip. So
opening any compare in a remote project promotes both sides to the front of the
pull queue and shows the same fetching placeholder a tab does (§ 6.2), and
`CompareMergeService` still needs no remote awareness — the promotion happens
where the compare is opened, not inside it. The conflict Compare in § 7.7 was
always correct, because it explicitly fetches the remote content; this makes the
general case behave the same way.

The cost is worth stating, and the two halves of it are keyed differently on
purpose. The **daemon** is per `(uid, remote_root)`, so two remote projects on the
same host get two, deliberately: sharing one across roots would put one project's
watch budget and scrollback memory at the mercy of another's. The **ControlMaster**
is per `(user, host, port)` and is therefore *shared* by both — multiplexing many
channels over one authenticated connection is the entire reason ssh has masters,
and one master per project would pay a second handshake and a second
`ControlPersist` lifetime to obtain nothing. The
master is reference-counted across the projects using it and torn down when the
last one closes.

## 7. UI

Every surface below is host-owned and rendered from a view model built in
`RenderViewModelBuilder`, per the workspace-architecture spec. Nothing here is
plugin-owned or render-TU product logic.

### 7.1 Entry points

| surface | what |
| --- | --- |
| Welcome (no project) | An **Open Remote Folder…** action beside Open Folder… (`WelcomeHitRegion::Kind::OpenRemote`). Remote entries in the recents list show a host badge (`host:/path`, never the mirror path). |
| File menu | **Open Remote Folder…** under Open Folder…, `ActionId::ProjectOpenRemote`. |
| Command palette | `Remote: Open Folder…`, `Remote: Reconnect`, `Remote: Disconnect`, `Remote: Resync (full manifest)`, `Remote: Show Status`, `Remote: Open Terminal on Host`, `Remote: Remove Mirror`. Availability follows connection state (`WorkspaceActionAvailability`). |
| Command line / control channel | `project-open ssh://[user@]host/abs/path` and `project-open-remote [user@]host:/abs/path`. The `ssh://` form is canonical; the scp form is accepted when the string is not an existing local path. This is also what the headless tests drive. |
| CLI | `microide ssh://host/path` opens straight into a remote project. |

### 7.2 The Open Remote overlay

A quick-open card (`OverlayMode::RemoteOpen`, `OverlayIsQuickOpen == true`), so it
wears the same chrome as Ctrl+P: one query field, a result list, a hint row.

- Query: `[user@]host:/absolute/path`. Typing filters the list.
- List: recent remote projects (from `remote_hosts`, newest first, showing
  `host:/path` and last-connected), then `Host` aliases parsed from
  `~/.ssh/config` (a small deterministic parser in `src/platform/`; `Host *`
  patterns are skipped). Selecting a host alias fills `host:` and leaves the
  caret after the colon.
- Validation is inline and non-blocking: a relative path shows "path must be
  absolute on the host"; a missing colon shows the expected shape. Enter with a
  valid query opens.
- Hint row: `Enter open · Esc cancel`, joined through `JoinHintSegments`.
- No password, key or port fields. Ports, users and jump hosts belong in
  `~/.ssh/config`, and the overlay says so in its empty-state line.

### 7.3 Connecting and syncing

Opening a remote project is seconds, not milliseconds, so the project tab and the
sidebar appear **immediately** and the editor area shows the project home with a
**remote status block** above the usual actions:

```
  build-box:/srv/work/microide
  ✓ connected            ✓ agent 2.12.0        ● syncing 1,204 / 4,258 files · 6.1 MiB   00:07
                                                                       [Cancel]  [Details]
```

- Rows advance through § 6.6's states; a failed stage shows the reason and the
  exact command to reproduce it in a terminal (`ssh -o BatchMode=yes host
  microide-agent --version`), plus a **Retry** and, for auth, **Open Terminal**.
- The sidebar tree renders from the manifest after the first round trip; rows
  whose content is pending are drawn dimmed. Opening one puts it at the front of
  the queue; the tab shows a one-line "fetching from host…" placeholder until the
  content lands, then behaves as a normal buffer.
- Quick-open and the file index are complete from the manifest; project search
  runs on the host (§ 6.11), so it is complete and correct from the first round
  trip regardless of how much content has been pulled.

### 7.4 Status bar

One host-owned segment per remote project, through `StatusBarService`:

| state | text | tone |
| --- | --- | --- |
| Ready | `⇅ build-box` | default |
| Syncing | `⇅ build-box · 1,204/4,258` | info |
| Reconnecting | `⇅ build-box · reconnecting` | warning |
| Offline | `⇅ build-box · offline · 3 unsent` | error |
| NeedsAuth | `⇅ build-box · sign in` | warning |

Clicking it opens the command palette filtered to `Remote:`; the tooltip is the
full `host:/path`, the last-connected time and the pending-push count.

### 7.4a Reattach

Reconnecting to a daemon that kept working is the common path, so it is stated
rather than silent. The status segment shows `⇅ build-box · reattached` briefly,
and a notification reports what came back: *"reattached to build-box — 3 terminals
still running, 41 files changed on host"*, with **Show** opening § 7.9. Detached
terminal tabs are drawn dimmed with a `detached` badge while the link is down and
refill from the buffered output on reattach, with a rule marking where the gap
was. A terminal whose shell exited while detached shows its exit line and then
behaves like any finished terminal.

### 7.5 Labels

Window title, project tab, editor and compare breadcrumbs, and welcome recents
show `host:/remote/path` or `host:name`; a remote terminal tab is labelled
`build-box · zsh`. The mirror path appears only in **Remote: Show Status**, so a
user can find it when they need it and never mistakes it for their project.

### 7.6 Notifications

Through `NotificationService`, each with the one action that matters:

| event | tone | action |
| --- | --- | --- |
| connected / reconnected, journal flushed (N pushes) | info | — |
| link lost, working offline | warning | Reconnect |
| `path` changed on host while you have local changes | warning | Compare |
| host switched to branch `x`, resyncing N files | info | — |
| agent not found or version mismatch | error | Copy install command |
| authentication required | warning | Open Terminal |
| initial sync complete (only if it took over 5 s) | info | — |

**Some of these must not expire, which the current service cannot express.**
`NotificationService` auto-dismisses at `DurationMs() == 4000` and keeps at most
`MaxVisible() == 4`, with no way to pin one (`services/NotificationService.h`).
Half the rows above are then useless: a *link lost — Reconnect* toast that vanishes
after four seconds leaves the user with an offline project and no visible reason,
and § 7.3's sync progress row expires in the middle of the sync it is reporting. So
Groundwork G7's `Notification { key, tone, body, actions[], progress }` also carries
a **lifetime** — timed as today, or sticky until dismissed or replaced by key. A
row that reports a state (offline, syncing, authentication required) is sticky and
is cleared by the state changing; a row that reports an event (connected, journal
flushed) is timed. Sticky rows do not count against the four-row cap, which exists
to stop toast stacking, not to hide the connection state.

**Aggregation is mandatory, not a refinement.** One notification per changed file
is unusable when agents rewrite hundreds at a time. Only a change to a file the
user has **open and dirty** notifies individually, because only that one needs a
decision. Everything else is counted: remote changes are summarized at most once
every 30 seconds as *"41 files changed on host"* with a **Show** action opening
§ 7.9, and a burst that triggers the § 6.3 resync path reports once as the branch
or resync event rather than as its contents.

### 7.7 Conflict flow

The existing external-change banner, with **Compare** added beside Reload and
Overwrite. Compare opens a compare tab (local buffer, remote content) in the
existing compare surface; saving the local side from there pushes with the
remote's current hash as the precondition, so a second remote change in between
becomes a second conflict, not a lost write.

Two more surfaces belong here, because both are cases where the design refuses to
act rather than acting wrongly.

**The mass-deletion prompt** (§ 6.3) is blocking and deliberately unlike a toast: a
manifest diff that would delete more than `remote.mass_delete_threshold` of the
mirror stops, `tree/` is untouched, and a prompt names the host, the count, the
total, and the last-connected time, with **Apply**, **Disconnect** and a **Show**
that lists the paths. It uses the prompt surface rather than a notification because
a notification can be missed and this one deletes a thousand files if it is.

**`dirty`, `local-only` and held-diagnostic markers** are tree and tab state, not
notifications. A `local-only` row (§ 6.3) carries a marker whose tooltip says it is
outside the project's tracked set and will not be visible to the host's tools. A
`dirty` path with a parked conflict carries the conflict marker until the user
resolves it. A file whose diagnostics are held pending a pull (§ 6.5) shows the
fetching placeholder in the gutter rather than an empty diagnostic list, because
"no problems" and "not checked yet" are different answers and only one of them is
true.

### 7.8 Settings (a new **Remote** section in the overlay)

| id | scope | type | default | purpose |
| --- | --- | --- | --- | --- |
| `remote.mirror_root` | User | path | `$XDG_DATA_HOME/microide/remote` | where mirrors live |
| `remote.exclude` | Project | globs | empty | paths never mirrored, on top of git's ignore rules and `project.files_exclude` |
| `remote.include` | Project | globs | empty | ignored paths to mirror anyway — indexed, searchable and editable; reaching one read-only needs no setting (§ 6.5) |
| `remote.max_file_bytes` | User | int | 8 MiB | above this a file is listed and fetched on demand only |
| `remote.ssh_options` | User | string | empty | appended to every ssh argv |
| `remote.agent_command` | User | string | `microide-agent` | for a non-PATH install |
| `remote.shell` | Project | string | empty (= remote `$SHELL`) | remote login shell |
| `remote.reconnect` | User | bool | on | automatic reconnect |
| `remote.search` | Project | enum `host`/`local` | `host` | where project search runs (§ 6.11) |
| `remote.resync_threshold` | User | int | 2000 | change-batch size above which the engine re-diffs a full manifest instead of applying rows |
| `remote.pull_concurrency` | User | int | 4 | content fetches in flight |
| `remote.agent_idle_timeout` | User | int (min) | 30 | daemon exit delay with no client and no live terminal (§ 6.12) |
| `remote.scrollback_prefetch_lines` | User | int | 500 | scrollback sent on attach before lazy backfill (§ 6.12) |
| `remote.host_scrollback_budget` | User | int | 64 MiB | total host memory across all terminal rings for one daemon; the oldest lines of the least-recently-active terminal are dropped first |
| `remote.max_manifest_files` | User | int | 50000 | hard cap on the content set; exceeding it fails the connection rather than truncating the manifest (§ 6.2) |
| `remote.term_credit_bytes` | User | int | 256 KiB | per-terminal outstanding output before the host drops with a marked gap (§ 6.4) |
| `remote.agent_socket_dir` | User | path | empty | override for a host with no per-user runtime directory; empty means resolve `$XDG_RUNTIME_DIR` then `/run/user/<uid>`, and refuse if neither qualifies (§ 6.12) |
| `remote.mass_delete_threshold` | User | percent | 25 | share of the manifest a single diff may delete before it prompts instead of applying; floor of 100 rows (§ 6.3) |
| `remote.object_store_budget` | User | int | 2 GiB | per-mirror cap on `meta/objects/`; unreachable objects are swept oldest-first down to it (§ 6.2) |
| `remote.backfill_inflight_bytes` | User | int | 256 KiB | unacknowledged bytes idle backfill may have outstanding, so an interactive fetch is never queued behind a bulk one (§ 6.4) |
| `remote.experimental_link_local` | User | bool | off | Phase 1's link-a-local-checkout mode; off, undocumented in the overlay, removed when Phase 2 lands (§ 8) |

All registered in `WorkspaceSettingsRegistry` (the `CheckSettingsReadAreRegistered`
lint makes an unregistered one fail the build). Project-scoped values persist in
the project's state directory, keyed by `ProjectId` (§ 6.8), like any other project
setting. The five the daemon consumes travel to the host at attach (§ 6.12).

### 7.9 Changed on Host

The surface that answers "what did the agents do while I was working". A sidebar
view (registered through `WorkspaceSidebarRegistry`, so it is an ordinary sidebar
contribution) listing files the host changed since the later of connect time and
the last time the user cleared it, newest first, with the change kind (modified,
added, deleted, renamed) and whether the local copy is `current`, `stale`, `dirty`
or `absent`. Selecting a row opens the file; a row whose local copy is stale offers
**Compare** against the version the user last had.

It is deliberately a plain list and not a second git UI: with agents committing,
the git sidebar already answers the question better, and this view exists for the
window between commits. It has a **Clear** action and shows the count that the
aggregated notification (§ 7.6) and the status-bar tooltip report.

### 7.10 Failure messages

Every remote failure names the stage, the host, and a command the user can paste
into a terminal to see the same failure. "Connection failed" alone is a bug.

## 8. Phasing

Each phase is independently useful and independently measurable.

- **Groundwork — now, in the local tree, no remote code.** Ten changes that are
  worth making on their own merits and that happen to be exactly the seams the
  rest of this design plugs into. Doing them now means Phase 1 is wiring rather
  than refactoring, and if remote projects never ship, the tree is still better
  for every one of them.

  **This phase breaks compatibility deliberately, and that is the point.**
  Persisted formats, setting scopes, the save contract and project identity are all
  allowed to change, and several of the items below are worth doing *only* if they
  are allowed to. Held to non-disruptive refactors this phase would be three items
  and ~450 lines instead of ten and ~4,200; the return on the larger one is that the
  two phases after it get smaller and the tree gets a layering it does not currently
  have. Ordered by what unblocks what: G1 and G2 are preconditions for most of the
  rest, and G10 — the write gate — is a precondition for § 6.3 being true at all.

  1. **Split an SDL-free kernel out of the tree.** `microide_core` links SDL3
     (`CMakeLists.txt:38`) and puts `<SDL3/SDL.h>` into every core TU's precompiled
     header (`CMakeLists.txt:20`), so every service the agent needs on the server
     drags SDL, SDL_ttf and fontconfig with it. The coupling inside the kernel
     directories is narrow, but it is more than two things, and each needs a named
     home or the lint fails on day one:

     | coupling | where | replacement |
     | --- | --- | --- |
     | the cross-thread wake: `SetWakeEventType(Uint32)` and `util::PushSdlWake` | `util/SdlWake.h`, `util/MainThreadMailbox`, and every producer that wakes the UI loop — over 40 files across `project`, `terminal`, `platform`, `plugin`, `workspace` and `app` | a `Waker` interface held by `MainThreadMailbox`: an SDL waker in the shell, an eventfd waker in the agent. `MainThreadMailbox` carries owed-wake and retry semantics (`HasUndeliveredWake`, `RetryWakeIfPending`) because `SDL_PushEvent` rejects on a full queue; the interface keeps that contract for the SDL implementation and lets the eventfd one report that it cannot fail. This is the bulk of G1's lines. |
     | SDL types in terminal data: `SDL_Color` in `TerminalCell`, `SDL_Keymod` in `TerminalSession`'s input API, `TerminalAnsiColors`' whole surface, `TerminalMouseEncoder`'s modifiers | `terminal/TerminalCell.h:43`, `TerminalSession.h:200,204`, `TerminalAnsiColors.h`, `TerminalMouseEncoder.h:36` | a plain `Rgba8` and a plain modifier bitmask, converted at the SDL boundary only |
     | `SDL_Log` | `platform/FileIndexWatcher.cpp:861,1719` | a kernel log sink in `util/`, which the shell binds to SDL's logger |
     | `SDL_getenv_unsafe`, `SDL_GetBasePath` | `platform/RuntimePaths.cpp:21,27` | `getenv` and `/proc/self/exe` |
     | `SDL_OpenURL` | `platform/HostIntegration.cpp:74` | host integration moves to the shell layer; it is desktop-only by nature and the agent never opens a URL |
     | `Uint8` / `Uint32` | throughout the kernel directories | `std::uint8_t` / `std::uint32_t` |

     **The kernel is a CMake source list, not a set of directories.** `terminal/`
     includes `render/AnsiPalette.h`, which includes SDL; `project/` includes
     `compare/` in three files (`PatchGenerator`, `GitPatchApply`, `PatchApplyTypes`)
     and `editor/SingleLineEditor.h` (`FileFinder`). `compare/` and `SingleLineEditor`
     are SDL-free and join the kernel; the palette data moves down with the `Rgba8`
     change. So the result is `microide_kernel` — util, platform, project, compare,
     the terminal model, persistence, and the editor value types they pull, with zero
     SDL — under `microide_shell` (editor render, workspace, plugin UI), each an
     object library with its **own** precompiled-header list. The lint is a
     transitive-include check in the `CheckLuaStaysBehindPluginBoundary` shape: no
     kernel TU may include, transitively, an SDL header or a header outside the kernel
     list, with a positive fixture so it is known to fire. A directory rule cannot
     express this: `editor/` already includes upward into `workspace/WorkspaceUiText.h`
     and `project/ProjectBackgroundExecutor.h`, so directory is not layer anywhere in
     this tree.

     Three payoffs that have nothing to do with remote. The data model stops depending
     on the windowing library, which is a layering the tree half-has and never states.
     A `microide-agent` binary becomes a couple of MB instead of thirty, which turns
     install into "copy one static binary" and closes § 12's agent-install question.
     And the kernel's tests can run without SDL — **which requires a second test
     executable**, `microide_kernel_tests`, registered with the same sharding, added to
     `tools/run-checks.sh` (which builds and names `microide_tests`), the coverage lane
     and the clang lane. 81 of 203 test TUs name SDL today; the rest are the
     candidates. That second binary is in G1's estimate; without it the faster-suite
     payoff is a claim with nothing behind it. G1 is also the precondition for running
     the terminal model on the host (§ 6.5): `TerminalSession` already owns its
     `platform::TerminalBackend` and the pty reader thread lives in the backend, so
     once its types are SDL-free the model moves as one unit.

  2. **One process launcher, owned by the project.** The spawn surface is eight sites
     in four directories, and `platform::RunSubprocess` callers are only half of it:
     four `RunSubprocess` sites (`GitCommandUtil.cpp:324`, `HostIntegration.cpp:91` for
     `xdg-open`, `PluginProcessInterop.cpp:268`, and the format-on-save call in
     `WorkspaceTabCoordinatorShellBridge.cpp:292`), three `AsyncSubprocess::Start`
     users (`WorkspaceLspClient`, `WorkspaceDapClient`, `StdioJsonRpcClientTransport`),
     and `TerminalBackend`'s `fork`/`execl`. `project/SubprocessHelper.cpp` is a
     forwarder, not a site. § 6.5 says everything that runs a command must take the
     same route "or a session silently splits across two machines", and a convention
     across eight sites will not hold.

     So locality is **owned, not chosen per call**: the project state holds a
     `ProcessLauncher` — local in a local project, ssh-prefixed in a remote one — and
     every spawn goes through the launcher of the project it belongs to. The spawns
     that must never follow the project are typed as such: `xdg-open` takes an
     explicitly local launcher, because opening a file manager on the build server is
     always wrong, and § 6.5's `Remote: Open Local Terminal` does the same. The lint
     covers **both** `RunSubprocess` and `AsyncSubprocess::Start`, or LSP and DAP
     escape it. The immediate local payoff is that git becomes testable against a
     scripted fake instead of whatever `git` the test machine has, which is exactly
     the dependence `dev-docs/project/validation-traps.md` keeps finding.

  3. **Make the terminal launch argv-shaped.** `terminal.shell`'s own description
     says *"Shell command used by new terminals"*, but `TerminalBackend` treats it as
     a program **path** and `execl`s it with fixed arguments
     (`TerminalBackend.cpp:189,191`: `-i`, or `-lc <command>`). The setting promises
     something the code refuses, so `terminal.shell = "ssh host"` fails with a usage
     error from ssh's identity-file flag. `TerminalStartRequest::shell` becomes a
     command vector and `TerminalStartRequest::command` stays a string handed to
     `<shell> -lc`, which is what every caller means by it. This honours the
     documented behaviour, makes wrapper scripts work, and is where G2's launcher
     attaches. `terminal.shell` moves to `SettingScope::Project` at the same time:
     which shell to run is a property of the project, and the remote case makes that
     obvious.

     The scope move changes nothing about resolution. `SettingsStore::Resolve` is
     project-over-user for every setting regardless of its declared scope
     (`workspace/persistence/SettingsStore.h`); `SettingScope` only steers where the
     Settings overlay writes. A user with `terminal.shell` in their user config keeps
     it. A settings test pins that anyway, because "the value is still read" is
     exactly the assertion whose absence lets a scope change pass green while doing
     nothing.

  4. **Make file open asynchronous, and introduce the completion guard every async
     tab operation shares.** `TextViewport::OpenFile` reads the whole file
     synchronously on the shell thread (`TextViewportFileIO.cpp:152`). That is
     correct locally and it is the one place the mirror's "every synchronous site is
     left alone" promise genuinely leaks, because § 6.2's `absent` and `stale` states
     need a fetching placeholder. `EditorTabState` already carries `needs_restore`
     with `restored_path`, caret and scroll for lazy session-restore hydration
     (`WorkspaceTabState.h:361`), so "a tab that exists before its content does" is
     present and needs promoting from a restore special case to a first-class
     `TabContent { Ready, Loading, Failed }`. Local files resolve in the same frame,
     so nothing regresses; a 500 MB file, or any file on a stalled mount, stops
     freezing the shell thread.

     Two things the restore precedent does not have, and this item adds:

     - **A guarded completion.** The read finishes on another thread and the tab may
       have been closed, retargeted or edited meanwhile. So the completion carries
       the tab's identity and the viewport's `content_revision` at the moment the
       work was posted, and applies only if both still match; otherwise it is
       dropped. This is the one primitive G5 (apply the formatted text only if the
       buffer did not change under the formatter) and G6 (apply a hash result only if
       the file is still the one that was checked) also need. It is defined once, in
       `editor/`, and the three items consume it rather than each inventing a version.
     - **A reader that is not the serial executor.** `ProjectBackgroundExecutor` is a
       serial queue with `PostLatest(key)`; a 500 MB read on it parks git status and
       search behind it. File reads get a dedicated reader thread with cancellation on
       tab close. One thread is enough for a workload that is one file at a time.

     The hydration code lives in `WorkspaceShellEditor.cpp` today and the shell
     companion count is at its cap, so the content state and its transitions land on
     `EditorTabService`, not in a shell TU.

  5. **Make save an asynchronous pipeline, so a formatter never blocks the shell
     thread.** Format-on-save runs the contributed formatter *synchronously on the
     shell thread* with a 5 s timeout, and it is a documented, allowlisted exception
     to this repo's own "no synchronous subprocess in workspace" lint
     (`tests/architecture/WorkspaceCoordinatorArchitectureRules.cpp:273-282`). The
     allowlist's reasoning — an explicit save is a user-initiated blocking action that
     must complete before returning — is sound at local latency and collapses at an
     RTT: across a far link it is a five-second freeze on every save with a flaky
     connection, which § 6.10 forbids outright.

     The save contract becomes: snapshot the buffer and its `content_revision`; run
     save participants and the formatter off-thread on the snapshot, bounded by the
     existing timeout; when they return, **if the revision is unchanged** the
     formatted text replaces the buffer as part of the save — not as a separate edit
     that leaves a just-saved file dirty — and is written through the gate (G10) and,
     remotely, pushed. Otherwise the unformatted snapshot is written and the format
     result is dropped with a notice. The tab shows a saving state for the duration,
     and a second save request while one is pending coalesces into it. Ordering is
     therefore **format, then write, then push**, and § 6.3 relies on exactly that
     order to keep the host formatter from reformatting the buffer a round trip after
     the save. A formatter that fails or times out still saves, as today. Autosave
     keeps suppressing formatters (`WorkspaceTabCoordinatorShellBridge.cpp:146`).
     When this lands the allowlist entry is **deleted**, so the lint has zero
     exceptions.

  6. **Content hashes replace mtime+size for change detection.** `FileSignature`
     compares mtime and size, which misses a same-size edit inside the mtime's
     granularity — a real if uncommon local bug, and the exact shape of edit an agent
     rewriting one line produces. The remote design needs content hashes anyway
     (§ 6.2), and computing them in one place means the local and remote
     external-change paths are the *same* path. Use **blake3** rather than
     `util::Sha256FileHex`: same collision resistance, roughly an order of magnitude
     faster, and manifest construction sits on the connect critical path (§ 9).
     `Sha256FileHex` stays for the tool downloader's manifest check, which is sha256
     by contract.

     The checks are event-driven — the watcher batch in
     `WorkspaceShellProjectChanges.cpp:79`, save time in `DetectDiskConflict`, and
     the LSP workspace-edit guard in `AssistService.cpp:1948` — never per frame, so
     the cost is bounded. Two rules keep it so. **Stat stays the prefilter**: a file
     is hashed only when its stat signature differs or the watcher named it, so a
     save on an unchanged 100 MB file hashes nothing. **Hashing never happens on the
     shell thread**: the watcher path calls `StatFileSignature` inline today, and a
     hash there is a whole-file read on the shell thread; it moves to G4's reader with
     G4's guarded completion. After a save the recorded signature is the hash of the
     bytes just serialized, not a re-read of the file.

  7. **Give notifications identity, actions, progress and a lifetime.**
     `NotificationService::Show(tone, message, now_ms)` takes no key and no action,
     and de-duplicates only by exact tone-and-text match, across 28 callers. § 7.6
     needs an action on every row (Reconnect, Compare, Show, Copy install command),
     needs "41 files changed on host" to update **in place** rather than stack, and
     § 7.3 needs a progress row. So: `Notification { key, tone, body, actions[],
     progress, lifetime }` with replace-by-key. The **lifetime** matters as much as
     the key: the service auto-dismisses every row at `DurationMs() == 4000` and caps
     the stack at `MaxVisible() == 4`, so a state row (offline, syncing, sign in)
     disappears four seconds after the state it reports began, and a progress row
     expires mid-progress. Sticky rows clear when their state does and do not count
     against the visible cap. Progress text is composed in `RenderViewModelBuilder`,
     since the render lint bans string materialization in render TUs. Nothing about
     any of this is remote-specific — a local build, a long git operation and a
     plugin task all want the same four things, and the absence of them is why
     long-running local work currently reports as a burst of identical toasts or as
     nothing at all.

  8. **`ProjectId` replaces `std::filesystem::path` as project identity.** Keeping
     identity path-shaped costs nothing up front and then makes § 7.5 a
     leak-suppression exercise repeated at every presentation site — the window
     title, the project tab, both breadcrumbs, the welcome recents, the tab tooltips —
     where missing one shows the user a path under `$XDG_DATA_HOME` and calls it their
     project. An opaque `ProjectId` carrying `Location { Local(path) | Remote(host, path) }`
     makes the mirror path structurally unable to leak, and turns § 6.13's "a project
     is local or remote, never both" into a property of the type.

     **Identity and root are two fields, not one.** `ProjectWorkspaceState::root`
     stays a `std::filesystem::path` — the mirror's `tree/` in a remote project —
     because the ~170 `.root` uses in `src/workspace` want a directory to do I/O in,
     and converting them buys nothing. What converts is everything that *keys* or
     *shows* a project: `project_roots`, `recent_project_roots`, the per-project
     state directory name (`WorkspaceProjectPresentation.cpp:38`, also used by the
     control channel), breakpoint and launch-config records, and
     `WorkspaceProjectPresentation`. The state directory keys on the `ProjectId`,
     **not** on the mirror path, or changing `remote.mirror_root` orphans every
     remote project's settings and session. This is the item whose entire cost *is*
     the compatibility break: it needs a persisted-format migration (§ 6.8), through
     `PersistenceService` and the record reader/writer like everything else.

  9. **Consolidate the direct `.git` readers behind one metadata source.** Four
     places independently know `.git`'s on-disk layout: `ResolveGitDirectory` and
     `ReadPendingMergeHeadId` (`GitCommandUtil.h:52,62`),
     `GitRepositoryMetadataTracker`'s HEAD and loose-ref sampling, and
     `StatusBarModelService::read_head_branch`. They exist for a good reason (keeping
     the shell thread off a 60 s-capped fork) and they stay, behind one
     `GitMetadataSource` rather than four copies of the same knowledge. The source
     has **three** answers, not two: repository, not a repository, and **unknown**.
     The local implementation is never unknown; the remote one is unknown from open
     until the first `git/metadata` arrives a round trip later, and the status bar
     and `is_git_repo_valid` render that as unknown rather than as "not a
     repository". Designing the third state in now is what makes the agent's
     `git/metadata` a drop-in implementation instead of a retrofit.

  10. **Route every write into a project tree through one gate.** Six subsystems
     independently know how to replace a file under the project root: the editor's
     save (`TextViewportFileIO.cpp:241`), the plugin file API
     (`PluginWorkspaceInterop.cpp:187`), LSP resource ops with their own staging
     journal of raw `fs::rename`/`ofstream` calls (`LspService.cpp:1799-1977`) and
     rename's open-and-save (`AssistService.cpp:1500`), replace-in-project
     (`WorkspaceShellProjectSearch.cpp:112`), the merge writer
     (`WorkspaceCompareInteractionCoordinator.cpp:741,749`), and the sidebar's file
     operations (`WorkspaceSidebarCoordinatorActions.cpp:56`, over
     `project/FileOperationService`, `platform/FsOps` and `platform/Trash`). They
     share `WriteTextFileAtomically` and nothing above it: no common notion of what
     the write was computed against, no serialization between them, no single place
     that knows a file under the project root just changed.

     **The editor's save is itself several doors.** `PrepareEditorViewportForSave`
     (`WorkspaceTabCoordinatorShellBridge.cpp:236`) runs save participants and the
     formatter, and six call sites bypass it by calling `viewport.Save()` directly:
     the external-change banner (`WorkspaceTabExternalChangeOps.cpp:39`), the dirty
     prompt path (`WorkspacePathMutationCoordinatorDirty.cpp:207`), plugins
     (`WorkspaceShellPlugins.cpp:1658`), and the compare and merge result viewports
     (`WorkspaceTabCoordinator.cpp:84,109`, `WorkspaceShellCompareMerge.cpp:268`).
     Those saves run no formatter and no save participant today. The gate and the
     fix are the same change: `TextViewport::Save` takes the writer, so every
     viewport save is gated, and G5's pipeline runs at the gate rather than in the
     bridge.

     The gate takes the path, the new content or the tree operation, and the base the
     write was computed against; it takes the per-path lock, writes, records the new
     base, **returns the post-write hash** so the caller does not stat the file again,
     and — remotely — enqueues the push or the `fs/op`. `MirrorWriteGate` is the
     remote implementation; the local one is a pass-through. The lint cannot know
     statically whether a path is under a project root, so it is stated as a ban:
     `WriteTextFileAtomically`, `fs::rename`, `fs::remove`, `fs::remove_all`,
     `create_directories`, `copy_file` and `std::ofstream` may not appear in
     `src/workspace`, `src/plugin` or `src/editor` outside the gate TU, with an
     explicit allowlist for the writers that are not project writes: persistence, the
     tool downloader's cache, the control channel's descriptor file, and the plugin
     data directory. The project state directory lives under the user state
     directory, not under the root, so nothing there needs gating. The local merits
     stand on their own — the external-change check, the file index and the watcher
     each currently learn about these writes by observing the filesystem afterwards,
     which is how a save and a background reload can race today — and converting
     these sites must not happen concurrently with building the thing that depends
     on them.

  **Order.** G1 first, because it is mechanical and touches the most files. Then
  G10, because G5 and G4 both sit on the save and open paths it owns. Then G4, then
  G5 and G6 on top of G4's completion guard. G2, G3, G7, G8 and G9 are independent
  of each other and of that chain.

  **Groundwork has its own performance gates and its own coverage**, in § 9 and
  § 10, because speed is this repo's first priority and a phase that only prepares
  for remote must not pay for it locally.

  **What is deliberately *not* groundwork.** The two protocol-level decisions —
  content-addressed objects with binary framing (§ 6.4) and shipping terminal
  screen state rather than bytes (§ 6.5) — are Phase 2 work, not groundwork, because neither has a local consumer and neither can be exercised
  without an agent to talk to. G1 is their precondition and that is the whole of
  their relationship to this phase.

  **One hard constraint on all remote work, worth stating before anyone starts:**
  the `WorkspaceShell*.cpp` companion-TU count is at its ratchet-only cap of 47
  with zero headroom, and the shell's own line caps are hard-linted. No part of
  this design may add a shell companion. The architecture here already complies
  (`RemoteProjectService` plus the `src/project/remote/` components), but it is a
  build-breaking gate, not a style preference.

- **Phase 0 — no product code.** Document display forwarding as tier-0; ship
  `tools/remote-session.sh` (`xpra` attach/reattach) and a remote-shell wrapper for
  `terminal.shell`. Days.
- **Phase 1 — remote processes for a local tree.** `RemoteHostSession` (ControlMaster,
  state machine, auth via terminal tab), `RemotePathMap`, `RemoteProcessLauncher`,
  `ProjectUriMapper` at the `FileUri` seam, remote git/LSP/DAP, the `remote.*`
  settings, the status segment and notifications. A project opened from a **local
  checkout** can declare a host and remote root (`Remote: Link This Project to a
  Host…`) and get a remote toolchain over it — useful on its own for anyone who
  syncs with git, and it lands the whole translation layer with every file I/O
  still local. Best value per line in the design.

  **Phase 1 is a development stepping stone, and § 12 no longer asks whether it
  ships as a user feature — it does not.** With a local checkout and no mirror
  there is no manifest and no hash, so *nothing relates the bytes on screen to the
  host bytes that git, LSP and DAP are reporting on*. Diagnostics land on lines
  the buffer does not have, the debugger stops at a line number that means
  something else in your copy, and `git status` describes a tree you are not
  looking at. That is § 6.11's argument with the detection removed: mirror search
  is silently wrong and at least has a hash that could in principle say so, while
  Phase 1 has no in-band signal of divergence at all. "The user keeps them in sync
  with git" is a real workflow and an unenforced invariant, and an editor that
  reports confidently from an unenforced invariant is the failure this design
  exists to avoid. So Phase 1 ships behind `remote.experimental_link_local` (off,
  User scope, undocumented in the Settings overlay), exists to land and exercise
  the translation layer against a real host before Phase 2 depends on it, and is
  deleted as a user-reachable mode when Phase 2 lands. Its value is that Phase 2
  is then wiring; its value is not a shipped feature.

  **The Phase 1 terminal is `ssh -tt` with a local pty, and it is interim.** There
  is no daemon yet to own a host pty, so the shells are children of the connection
  and die with it — the exact failure § 6.5 rejects. It ships anyway, for two
  reasons: a phase that can build remotely but not run anything is not worth
  testing, and after the groundwork's argv change the whole thing is *a different
  argv*, not a feature. It is deleted in Phase 2 rather than migrated. The terminal
  tab says so in its status line, because a limitation the user discovers by losing
  a build is a bug report.
- **Phase 2 — `microide-agent`, the mirror, the Open Remote flow.** The agent
  as a **persistent daemon** with attach-or-start and host-side ptys (§ 6.5,
  § 6.12) — both are protocol shape, so neither can be retrofitted cheaply —
  plus manifest, read, write with CAS, fs ops, watch, git metadata and
  **`search/run`**,
  `MirrorSyncEngine` with journal, priority pulls, churn control and reconnect, the
  `remote_hosts` record, the Open Remote overlay, the connecting block, dimmed
  pending rows, the conflict Compare, offline mode. This is the product.

  Eight things in this phase look deferrable and are not: the four-state content
  model with base tracking, the `local-only` membership state, the mass-deletion
  guard, the object-store sweep, the hash guard on every host-computed position and
  edit (§ 6.5), the read-only out-of-content-set fetch, semantic terminal input with
  `term/event`, and the backfill in-flight bound. None is polish. Each is a case
  where the phase without it ships a silent wrong answer or loses bytes, and each is
  cheaper now than once a protocol and an on-disk layout exist to be compatible
  with.
  Host-side search is **in this phase, not deferred**: without it, search over a
  content-stale mirror is silently wrong from the first agent write (§ 6.11), so
  shipping the mirror without it ships a bug.
- **Phase 3 — scale and polish.** Incremental hashing on the agent, the branch-switch
  resync path tuned against a real multi-agent host, the **Changed on Host** view
  (§ 7.9), perf gates in the harness, an `agent install` helper that copies the
  running binary's `.deb` to the host, and **submodule content sets** — running the
  content-set command per submodule root so a submodule is a mirrored subtree
  rather than an empty gitlink row (§ 6.2).
- **Phase 4 — on-demand tier.** Only if measurements show a tree too big to
  mirror: a per-directory lazy mode where the manifest is fetched but content is
  never pre-pulled. The existing lazy population is most of it already.

### 8.1 Sizing

Estimated against comparable subsystems already in this tree, not guessed. For
calibration: `src/` is 163,283 code lines and `tests/` is 139,548; the
language-server subsystem is 5,914 lines, the debugger 5,951, the control channel
plus its socket server 2,398, and git 3,704.

| phase | production | tests |
| --- | --- | --- |
| Groundwork (§ 8, local tree, ships with or without remote) | ~4,200 | ~3,800 |
| Phase 0 (scripts and docs, no product code) | ~100 | — |
| Phase 1 (remote processes over a local tree) | ~2,600 | ~2,200 |
| Phase 2 (daemon, host terminal model, mirror, Open Remote flow) | ~9,000 | ~9,500 |
| Phase 3 (scale and polish) | ~1,700 | ~1,450 |
| **total** | **~17,600** | **~16,950** |

Groundwork by item, since it is now the phase most likely to be scheduled on its
own: G1 SDL-free kernel ~1,000 (mechanical across 40+ wake sites, plus the `Waker`
interface, the log sink, the CMake split with its own PCH, the transitive-include
lint, and the second test executable), G8 `ProjectId` ~600 (every path-keyed store
plus a format migration), G4 async open ~450 (the `TabContent` state, the guarded
completion G5 and G6 reuse, the dedicated reader), G10 write gate ~450 (the gate,
the six subsystem writers, the six direct `viewport.Save()` entry points, the
lint), G2 owned launcher ~400 across its eight spawn sites, G5 async save pipeline
~300, G9 `.git` readers with the unknown state ~300, G7 notifications ~300 including
the lifetime, G6 content hashes ~250 (excluding vendored blake3), G3 argv-shaped
terminal ~150.

The heaviest single items, so a schedule can see where the mass is:
`MirrorSyncEngine`'s four-state model with base tracking and the
pull-never-over-dirty rule (~250), the host-computed-position hash guard across
LSP, DAP and git (~250), semantic terminal input with host-side encoding and
`term/event` (~250), the object-store sweep and the `ENOSPC` path (~200), the
read-only out-of-content-set fetch and its external cache (~200), the `local-only`
membership state and its persistence (~150), the credit window and gap marking for
terminal output (~150), the mass-deletion guard with its prompt and the agent's
root-health reporting (~120), the backfill in-flight bound and `op/cancel` (~120),
runtime-directory resolution and socket ownership checks (~120), compare's pull
promotion (~100), format-on-save sequencing (~100), local symlink-resolving writes
(~80), journal sequence numbers and ordered replay (~60), the `expect` three-valued
field and its `O_EXCL` path (~60), replace-in-project's pull-before-write (~60),
the daemon epoch and the split protocol version (~50), daemon stdio detach (~50),
content-set invalidation on `.gitignore` (~50), search scoped to the content set
(~40), the shared ControlMaster (~30) and the manifest ceiling (~30). Phase 3
carries submodule content sets (~300) and watch-mode reporting (~200).

Roughly 4,200 lines of this plan — the whole of Groundwork — ship value whether or
not a remote project is ever opened: a kernel test binary that needs no SDL, a
launcher that makes git testable against a fake, a save that cannot freeze the UI
and runs the formatter from every entry point instead of one, an open that cannot
freeze it either, one door for every write into a project tree, notifications that
can carry an action, and a project identity that cannot leak a cache path into the
window title.

Treat production as a band of 11,000–17,600 across roughly 35 new files: about 10%
growth on the source tree, and about the combined size of the four subsystems
listed above. The largest single pieces are `MirrorSyncEngine` (~2,400 — it carries
the base tracking, the membership states, the sweep and the deletion guard), the
agent daemon (~1,700 plus ~800 for pty ownership, input encoding and the scrollback
rings), `RemoteHostSession` (~800), `RemoteAgentClient` (~600) and the write gate
(~450, in Groundwork). Everything else is under 500 apiece.

It is not larger because roughly 4,800 lines of exactly the needed machinery
already exist and are hardened: the stdio transport's queue and I/O-thread
discipline (§ 6.4 reuses the behaviour and replaces only the codec),
`AsyncSubprocess`, `ControlSocketServer`, the file scanner, the file index, both watchers, the change
coalescer, `ProjectSearchService`, `TextFileIO`, and the persisted-record writer.
The agent is mostly a dispatch loop over services this binary already runs.

**Nearly all the correctness risk is in two components.** `MirrorSyncEngine` owns a
thread, the four-state content model with its base tracking, compare-and-swap, the
write gate's per-path locking, journal replay and the churn paths; the daemon owns process lifetime, pty multiplexing and scrollback
resume. Those two carry the test weight and are where a schedule slips. The other
~9,500 lines are wiring with a settled shape.

Every design review of this plan has found its problems inside those same two
components — the create precondition, ordered journal replay, the base tracking,
the deletion guard and compare's pull promotion in `MirrorSyncEngine`; the credit
window, the runtime directory, stdio detach, the manifest ceiling and the scrollback
resume in the daemon. That is mildly reassuring about the risk model and not at all
reassuring about the schedule for those two files.

## 9. Performance gates

Measured in the perf harness against a local `microide-agent` with injected
latency (`--agent-delay-ms`), so they run without a server:

| gate | budget |
| --- | --- |
| open remote project → tree visible | ≤ 1 RTT + 200 ms |
| open remote project → Ready, this repo, cold, 80 ms RTT, 20 Mbit/s | ≤ 20 s |
| open remote project → Ready, warm (manifest diff only, no changes) | ≤ 1 RTT + 500 ms |
| open a synced file | identical to local (0 RTT; existing `OpenFile` scenario) |
| open an unsynced file | ≤ 1 RTT + size / bandwidth |
| save | local latency; push acked ≤ 1 RTT |
| remote change → manifest row applied locally | ≤ 1 RTT + 250 ms coalesce |
| remote change → content visible in an open tab | ≤ 1 RTT + 250 ms + size / bandwidth |
| 500-file agent burst on the host → tree consistent | ≤ 2 RTT; user-clicked file still opens within its own budget throughout |
| branch switch on the host (1,800 files) → tree consistent | ≤ 3 RTT + manifest transfer |
| shell-thread frame while the agent is stalled | never > 16 ms |
| shell-thread frame during a 500-file burst | never > 16 ms |
| project search, remote, warm | ≤ 1 RTT + host search time (no mirror bytes read) |
| pull a stale file an agent edited in one place | delta transfer, ≤ 1 RTT + the delta, not the file |
| branch switch where most files exist in both branches | objects already held transfer nothing; only the genuinely new content moves |
| terminal on host → first prompt | ≤ 1 RTT + shell startup |
| terminal keystroke → echo | ≤ 1 RTT + 5 ms, and unaffected by a concurrent bulk file transfer |
| terminal running a build that emits 10 MB/s | wire carries screen deltas plus credit-windowed lines, never the raw stream; frame time unaffected |
| reattach to a daemon with 3 live terminals | ≤ 2 RTT to first repaint, visible screen + 500 lines each |
| scroll back into un-backfilled history | ≤ 1 RTT per page, and never blocks the shell thread |
| save with `editor.format_on_save` on | ≤ 2 RTT (format, then push), and the buffer is never reformatted after the save returns |
| open a compare in a remote project | ≤ 1 RTT when both sides are `current`; otherwise the two-file pull, with the placeholder shown |
| git status refresh / blame one file | ≤ 1 RTT + host git time, off the shell thread (`ProjectBackgroundExecutor`) |
| terminal producing output faster than the link drains | output is dropped with a marked gap; the transport never tears down and other terminals are unaffected |
| open a file the user clicked while idle backfill is running | ≤ 1 RTT + size / bandwidth, unchanged by the backfill — the gate is the bound on outstanding backfill bytes, so assert the delay, not the queue order |
| replace-in-project across 200 files, half of them `absent` | the pull of those files is reported as progress and precedes any write; no file is written from bytes it did not first fetch |
| LSP rename across 40 files in a churning tree | every target's hash is checked before any edit applies; a mismatch pulls and re-requests, and no file is written at adapted offsets |
| mirror disk footprint, this repo, after 100 simulated agent bursts | ≤ 2× tree + `remote.object_store_budget`; the sweep runs and the reachable set is never swept |
| manifest diff that deletes > 25% of the mirror | nothing in `tree/` is touched before the user answers |
| terminal keystroke across a mode change (entering and leaving a full-screen program) | the bytes the pty receives are byte-identical to a local session's; the gate exists because the client no longer encodes |

**Groundwork has gates of its own**, run in the existing harness with no agent and no
network, because speed is this repo's first priority and a phase that only prepares
for remote must not pay for it locally:

| gate | budget |
| --- | --- |
| open a 100 MB local file (G4) | shell-thread frame never > 16 ms; the tab is `Loading` until the read lands; closing the tab mid-read applies nothing |
| save with a formatter that takes 5 s (G5) | shell-thread frame never > 16 ms; the tab shows a saving state; a keystroke during the wait is kept and the format result is dropped |
| save an unchanged 100 MB file (G6) | zero bytes hashed — the stat prefilter is what the gate asserts |
| watcher reports a change to an open 100 MB file (G6) | the hash runs off the shell thread; frame never > 16 ms |
| `microide-agent` binary (G1) | static, ≤ 5 MB stripped, and links no SDL — asserted by the kernel include lint and by reading the dynamic section |
| `microide_kernel_tests` (G1) | runs with no display and no SDL initialization |
| `settings_change_many_tabs` (existing) | unchanged by G8; the state-directory memo still hits |
| a save from every `viewport.Save()` entry point (G10) | one write, one hash, no re-stat |

## 10. Test strategy

- **The agent is testable with no ssh.** `RemoteAgentClient` takes an argv, so a
  test runs `microide-agent --root <fixture>` over a pipe with a delay flag;
  ssh is never a test dependency. A `remote.ssh_command` test seam (like the
  dialog `launcher` seams) points `RemoteHostSession` at a shim for lifecycle
  tests.
- Unit coverage for: manifest diff, priority queue order, compare-and-swap
  success/conflict, journal append/replay across a simulated crash, reconnect
  re-diff, coalescing, `RemotePathMap` round trips including symlink and
  trailing-slash cases, `~/.ssh/config` parsing, `ssh://` and scp-form parsing,
  ControlPath length.
- **Churn coverage, because the many-agents case is the product's normal state.**
  A fixture that mutates the agent's tree from a second process while assertions
  run: a 500-file burst does not stall the user's file open; repeated writes to one
  path collapse to one pull; a batch over `remote.resync_threshold` takes the
  manifest path; a simulated `git checkout` (mass change + HEAD move) reports as
  one branch event and leaves the tree consistent; a remote write landing between a
  local save's local write and its push produces a conflict, never a clobber; and
  a file left `stale` is never served to search or shown in a tab without a pull.
  This is also where a vacuous pass is easiest to get: assert the number of
  `object/fetch` requests the agent received, not just the final tree state, or a
  test that "passes" by pulling everything eagerly looks identical to one that
  prioritizes correctly.
- Headless `--control` drives the UI flows (`project-open ssh://…` against the
  local agent) and asserts status text, notifications and the connecting block
  through the existing view-model tests.
- Fuzz: the agent's request decoder and the client's `tree/rows` / `watch/changed`
  decoders, in the `PersistedRecordReaderFuzz` pattern.
- **Detach/reattach coverage**, which is where a green suite is easiest to fake:
  kill the transport mid-session and assert the host shells are still alive and
  still producing; reattach and assert the missed output arrives **once**, in
  order, with no duplication of what the client already had (the trim-total offset
  is the thing under test, and a test that reattaches with an empty client cannot
  detect a duplicate — attach with a partial buffer); attach from a **fresh**
  client with no local scrollback and assert the full tail arrives, which is the
  different-machine case; overflow the ring while detached and assert the oldest
  lines are dropped with the gap marked rather than the newest silently lost;
  reattach to a handle running a full-screen program and assert a repaint rather
  than a replay; assert a shell that exited while detached still reports its final
  output and exit status; attach two clients and assert both see the same output;
  assert an idle daemon with a live terminal does **not** exit at the idle timeout.
- **Coverage for the silent failures**, each of which is easy to leave untested
  because nothing reports it at runtime: a create against a path that is `absent`
  locally but present on the host is **refused**, not applied (the one case that
  could destroy remote bytes — assert the host file is unchanged, not merely that
  a conflict was reported); a journal holding a push and a later rename of the
  same path replays in sequence order, and a parked conflict on one path does not
  block unrelated queued saves; an object delivered across two content frames
  reassembles byte-exactly when a multi-byte UTF-8 sequence **straddles the frame
  boundary** (a test that sends whole sequences cannot fail); a terminal producing faster than the link drains marks a
  gap and leaves the transport and its sibling terminals alive; the agent refuses
  to start when no per-user runtime directory qualifies, and refuses to adopt a
  pre-existing socket it did not create; a pull whose manifest row names a path
  through a recreated in-root symlink does not write outside `tree/`; a content set
  over `remote.max_manifest_files` fails the connection instead of truncating; a
  `.gitignore` write invalidates the content set; a compare opened on a `stale`
  file pulls before it compares; and format-on-save produces one formatted push
  rather than a push followed by a remote-change reload.
- **Coverage for the write-path and staleness failures.** Three of these destroy
  data, so each asserts the *bytes*, not the status: a plugin `files.write_text`, an LSP resource op, a replace-in-project and
  a sidebar rename in a remote project each produce a push (assert the agent
  received it) and survive a subsequent pull of that path; a path that is `dirty` is
  never overwritten by a pull, asserted by parking a conflict, letting the agent
  write the file again, and checking the local bytes are still the user's; a pull
  and a local save racing on one path leave the mirror holding one of the two whole
  versions and the journal holding the other, never a mix; a file created under an
  ignored path survives a full manifest diff (the `local-only` case, and the test
  that fails without it is "create `debug.log`, resync, assert it is still there");
  an empty directory created in the sidebar survives the same; a manifest diff
  deleting more than the threshold leaves `tree/` untouched until answered, and the
  agent reports a failed content-set command as an error rather than as an empty
  set — assert the client never saw a delete; a diagnostic, a breakpoint and a stack
  frame naming a `stale` file are held and the file pulled, rather than drawn at the
  wrong lines (assert the rendered line, since "a diagnostic appeared" passes
  either way); an LSP rename whose targets moved on the host is refused whole, with
  every target's bytes unchanged; go-to-definition into a path outside the content
  set opens read-only from `meta/external/` and never writes into `tree/`; a
  terminal key press sent while the host is entering and leaving a full-screen
  program produces the same bytes at the pty as a local session would (the test
  that cannot fail is one that changes modes between keystrokes, so change them
  *during*); `term/event` delivers an OSC 52 write to the local clipboard policy;
  an object sweep never drops an object that is a base, a manifest hash or a
  journal reference; `ENOSPC` during a pull suspends sync and leaves every journaled
  push intact; and a reattach with a `manifest_id` from a previous daemon epoch
  takes the full-manifest path instead of being answered "nothing changed".
- **Groundwork coverage**, since that phase can be scheduled alone and each of these
  is a failure nothing reports at runtime: a save from **each** of the six direct
  `viewport.Save()` entry points runs the formatter and the save participants (assert
  the formatted bytes on disk, per entry point — one shared fixture, six callers);
  a formatter that completes after the buffer changed leaves the user's edit intact,
  writes the unformatted snapshot, and does not mark the buffer dirty; a file open
  cancelled by closing the tab never applies its content to whatever tab now has that
  slot; a save on a file whose stat signature is unchanged hashes nothing (count the
  reads); a rename onto a path that exists on the host is refused with the host bytes
  unchanged; a remote project's state directory is the same before and after
  `remote.mirror_root` changes; the git metadata source reports unknown, not
  not-a-repository, before its first answer; a kernel TU that includes an SDL header
  or a shell header fails the include lint (with a positive fixture, per
  `validation-traps.md`); and `terminal.shell` set only in the user layer is still
  the shell a new terminal runs after the scope move.
- TSAN over the engine thread and transport thread, and specifically over the
  gate's per-path lock with a pull and a save contending on one path; ASAN/UBSAN
  over the agent.
- The perf scenarios in § 9, each in its own child process like every other
  scenario, with a stalled-agent case for the frame-time gate.

## 11. Decisions (every open question, answered)

| question | decision |
| --- | --- |
| Keep the mount? | No. § 4. |
| Session restore when the host is down | Opens from the mirror instantly, Offline, all tabs; connects after first frame. § 6.6. |
| Disconnect mid-edit | Cannot lose work: saves are local first, then journaled until acked. § 6.3. |
| Credentials / passphrase prompt | `BatchMode` first; on failure the prompt is answered in a terminal tab running ssh. Nothing stored. § 6.6. |
| Security posture | Remote execution unsandboxed and stated; agent confines paths; ssh owns host keys. § 6.9. |
| Thread discipline | Engine + transport threads wait; shell thread never does; `RunSubprocess` ban stands. § 6.10. |
| Where processes run | Always on the host, always via the same ControlMaster. The terminal's pty is on the host too, owned by the daemon; only the parser, scrollback view and input handling stay local. § 6.5, § 6.12. |
| Who decides the content set | The server, with git's ignore rules; `.git` excluded. § 6.2. |
| Cross-machine mtimes | Never compared. Hashes carry identity. § 6.2. |
| Content encoding on the wire | Raw bytes in a length-prefixed binary content frame — no JSON string escaping and no base64. The hazard a text codec would carry, split UTF-8 sequences in a chunked terminal stream, is recorded in § 6.4 so a return to one does not reintroduce it. § 6.4. |
| Where search runs for a remote project | On the host, by default, from Phase 2. Local mirror search is the offline fallback and is labelled as such. § 6.11. Resolved by the many-agents constraint: mirror search is silently wrong once content is stale. |
| Is a stale mirror acceptable in steady state | Yes for content, never for metadata, and never silently. § 6.2. |
| Per-file attribution to a specific agent | Not offered. inotify reports paths, not processes. Git is the attribution surface; **Changed on Host** (§ 7.9) covers the gap between commits. § 6.3. |
| Automatic merge of concurrent edits | Never. Conflicts are surfaced and resolved by the user. § 6.3. |
| Agent awareness (soft locks, "an agent is editing this", who-holds-what) | **No.** It would need the agents to cooperate through a channel that does not exist, and inventing one reintroduces exactly the protocol this design exists to avoid. Compare-and-swap (§ 6.3) already makes concurrent writes lossless, and **Changed on Host** (§ 7.9) already answers what moved. |
| Where the terminal pty lives | On the **host**, owned by the agent daemon, with terminal I/O on its own ssh channel. The local-pty-plus-`ssh -tt` shortcut loses every running remote process on a dropped link. § 6.5. |
| Detach and reattach | Supported, and the reason the agent is a daemon keyed to `(uid, remote_root)` rather than a child of the connection. Terminals survive; output is buffered while detached; the editing session itself stays local. § 6.12. |
| Terminal scrollback ownership | The **host** holds the authoritative scrollback, sized by the existing `terminal.scrollback_lines`; the local terminal renders a view of it, resumes by trim-total offset, and backfills older history lazily. § 6.12. |
| Mixing local and remote projects in one window | Supported, and free: per-project isolation already covers terminals, LSP, DAP, index, tree and search. A single *project* is never half-remote. § 6.13. |
| Local terminals in a remote project | Available through an explicit command and labelled `local · …`; never the default. § 6.5. |
| Churn tuning constants | Settled as defaults, not left open: 250 ms coalesce window, `remote.resync_threshold` 2,000 rows, `remote.pull_concurrency` 4. All three are settings, so a host that disagrees is a config change rather than a redesign; § 13 measures them when a multi-agent host is available. |
| Does Phase 1 ship as a user feature | **No.** Development stepping stone behind `remote.experimental_link_local`, removed when Phase 2 lands. Nothing relates the local bytes to the host bytes that git, LSP and DAP report on, and unlike mirror search there is not even a hash that could detect the divergence. § 8. |
| Compare over a stale mirror | Pulls both sides first. The two-file pull is one round trip, so § 6.11's rule is satisfied by fetching rather than by relocating. `CompareMergeService` stays remote-unaware. § 6.13. |
| Precondition for creating a file | `expect: absent`, implemented with `O_EXCL`. Without it a locally-`absent` file created through the sidebar or Save As overwrites host content the user never saw — the only path in this design that destroys bytes rather than parking a conflict. § 6.3. |
| Journal replay order | The journal's own sequence, strictly. Pushes and tree ops interleave and are not independent; a conflict parks that path's remaining entries, not the whole journal. § 6.3. |
| Runaway terminal output | Per-handle credit window; output beyond it is dropped at the ring with a visible gap. Without it the transport's bounded queues turn one runaway command into a teardown of file sync and every other terminal. § 6.4. |
| Host with no `$XDG_RUNTIME_DIR` | Resolve `$XDG_RUNTIME_DIR`, then a qualifying `/run/user/<uid>`, else **refuse to start**. Never `/tmp`: the socket path is hash-derived and therefore predictable, so a world-writable parent on a shared host is a hijack of an authenticated channel, not a leak. § 6.12, § 6.9. |
| Daemon stdio | Closed and reopened on `/dev/null` with `setsid` before the relay reports success; readiness is the socket becoming connectable, never the spawn exiting. Otherwise `ssh` never returns and `StartingAgent` hangs on a handshake that already succeeded. § 6.12. |
| Tree completeness vs the content set | The content set is files, non-ignored, one repository deep: no empty directories, no ignored directories, no submodule contents until Phase 3. Stated as a limitation in the docs rather than implied away by "the tree is never behind". Two are softened rather than absolute: an ignored path is not *listed* but is still openable through § 6.5's read-only fetch, and an empty directory the **user** creates is kept as `local-only` (§ 6.3) though one the host already had is invisible. § 6.2. |
| A tree too large to mirror | Fails at `agent/hello` against `remote.max_manifest_files`. A truncated manifest is the one state the content-state model cannot express, since a missing row is indistinguishable from a missing file. § 6.2. |
| Should groundwork stay cheap and non-disruptive | **No.** Compatibility breaks are allowed, so groundwork is ten items and ~4,200 lines rather than three and ~450. All ten stand alone if remote never ships, which is the test each had to pass to be in that phase rather than in Phase 1. § 8. |
| Does the agent link SDL | No. Groundwork G1 splits an SDL-free kernel, so the agent is a small separate binary. The coupling is only the cross-thread wake and SDL types in terminal data. This also closes the agent-install question: copy one static binary. § 8. |
| Wire format | Length-prefixed binary frames, not JSON-RPC. The traffic is content, manifests and terminal state, and JSON charges +33% on bodies, ~6 MB on a large manifest, and base64 on the latency path. The hardened transport behaviour is kept; only the codec changes. § 6.4. |
| What the mirror stores | An object store addressed by content hash, with `tree/` materialized from it. Gives near-free branch switches, a journal that references immutable content, and `zstd --patch-from` deltas for the one-function-edit case that dominates this workload. § 6.2. |
| Content hash | blake3, used for the manifest, the object address, compare-and-swap **and** local external-change detection — one hash rather than a cross-machine one beside a local mtime+size check. § 6.2, § 8. |
| What the terminal ships | Screen deltas plus completed scrollback lines, because the terminal model runs on the host once G1 removes SDL from it. A 10 MB/s build costs the host one parse and the link a few KB. Full-screen repaint stops being a special case. Input is semantic events, encoded on the host. § 6.5. |
| Project identity | `ProjectId` with `Location { Local \| Remote }`, replacing the bare path. Makes the mirror path structurally unleakable and "a project is local or remote, never both" a type property. Costs a persisted-format migration, which is accepted. § 6.8, § 8. |
| Synchronous format-on-save | Replaced by an asynchronous save pipeline: snapshot the buffer and its content revision, run participants and the formatter off-thread, apply the result only if the revision is unchanged, then write through the gate, then push. Ordering is format → write → push everywhere, and the lint's allowlist entry is deleted. It is an allowlisted 5 s synchronous spawn on the shell thread today; across an RTT that is a five-second freeze per save and § 6.10 forbids it. § 8 G5, § 6.3. |
| Format-on-save in a remote project | Formats before the push, not after. Observing the host formatter's write as a remote change reformats the buffer a round trip after the save and can raise the conflict banner against the user's own formatter. § 6.3. |
| How many local writers into the mirror there are | Six subsystems — the editor's save, the plugin file API, LSP resource ops, LSP rename's open-and-save, replace-in-project, the merge writer and the sidebar's file operations — and the editor's save has six further entry points that bypass the formatter and save participants today by calling `viewport.Save()` directly. All route through the gate (Groundwork G10): `TextViewport::Save` takes the writer, so no entry point can skip it, and a lint bans the raw filesystem calls outside the gate TU. § 6.3, § 8. |
| How many content states a file has | **Four.** `current`, `stale`, `dirty`, `absent`. Three states was a two-way comparison over three inputs and could not tell "the host moved ahead" from "we moved ahead", so it resolved every out-of-band local write by pulling over it. The base is already in the object store. § 6.2. |
| May a pull overwrite local bytes | **Never when the path is `dirty`.** It parks as a conflict. Without the rule, a parked conflict plus one more agent write silently replaces the user's saved work with the agent's, through a clean buffer, with nothing reporting it. § 6.2, § 6.3. |
| A user-created file the content set does not list | Recorded `local-only` and excluded from the deletion half of every manifest diff. Otherwise creating `debug.log`, a `.env`, anything ignored, or any empty directory is an action that undoes itself on the next resync. § 6.3. |
| A manifest diff that deletes most of the mirror | Refused and prompted, above `remote.mass_delete_threshold` (25%, floor 100 rows), and the agent never reports a failed content-set command as an empty set. The causes are mundane — an unmounted root, a failed `git ls-files` — and the outcome without the guard is the sync-product disaster. § 6.3. |
| Host-computed positions and edits against a stale mirror | Every one carries the hash it was computed against; a mismatch holds or pulls, and a host-computed *edit* is refused whole rather than adapted. This is § 6.11's rule applied to LSP, DAP and git, and it is § 8's argument against Phase 1 applied inside Phase 2, where it bites whenever a file is `stale`. § 6.5. |
| Git surfaces while pushes are queued | Gated on the journal. `git status` describes the host's tree, so staging or committing a file with an unacked push would commit the host's version and drop the user's edit. Those actions are disabled with a reason until the push acks. § 6.5. |
| Paths outside the content set (go to definition, step into, stack frames) | Fetched read-only with `file/read` into `meta/external/`, opened read-only, never pushed, never in a manifest. § 12's "should ignored directories stay navigable" was framed as browsing preference; it is go-to-definition, so it is decided rather than deferred. § 6.5, § 6.9. |
| Is `ToMirror` total | **No, both directions are partial.** The host names paths outside the root constantly and the previous "two total functions" hid the case rather than handling it. § 6.1, § 6.5. |
| Is `file/read` root-confined | No, and writes always are. The user has a shell on that host over the same connection, so read confinement is theatre; write confinement is the actual boundary. Results land outside `tree/`, read-only. § 6.9. |
| Terminal input encoding | **Semantic key and mouse events**, encoded on the host. The local encoder reads seven pieces of mode state the host owns (`TerminalSessionInputEncoding.h`, `TerminalMouseEncoder.h`) and they flip as programs start and exit, so a client encoding one round trip behind sends wrong bytes at exactly the boundary. Smaller payload, same latency, and protocol shape. § 6.5. |
| Local echo prediction in the remote terminal | **No.** 1 RTT to echo is accepted and budgeted. § 1's no-round-trip-per-keystroke rule is about the editor, where the mirror removes it; predicting echo locally would mean a second terminal model guessing at the host's, which is what shipping screen state removed. Stated so the asymmetry does not read as an oversight. § 6.5, § 9. |
| Object store growth | Swept. Reachable = a manifest hash, a base, or a journal reference; everything else goes, oldest first, down to `remote.object_store_budget`. The mirror is ≥ 2× the tree before history, in a directory declared data rather than cache, so "it grows" was not a survivable answer. § 6.2. |
| Running out of local disk | A first-class failure: sweep, then suspend sync loudly and keep every journaled push. Discarding a journaled object to make room loses exactly the bytes the journal exists to hold. § 6.2. |
| Backfill vs. the file the user just clicked | Bounded by outstanding **bytes** (`remote.backfill_inflight_bytes`), not by batch size, plus `op/cancel`. Priority in a local queue cannot reorder bytes already on the wire — the same head-of-line problem the terminal got its own channel for. § 6.4. |
| Protocol version | Its own small integer with a minimum-accepted floor, not the app version. Tying it to the release forbids drift instead of handling it, which turns "copy one static binary" into copying it to every host on every release. § 6.4. |
| `manifest_id` across a daemon restart | Scoped to a `daemon_epoch`. A restarted daemon has watched nothing, so accepting a resume token it cannot account for answers "nothing changed" and silently skips everything that happened while nobody was attached. § 6.12. |
| ControlMaster keying | Per `(user, host, port)` and shared between projects, reference-counted; the daemon stays per `(uid, remote_root)`. Multiplexing is what masters are for. § 6.13. |
| zstd | Vendored and linked, like blake3 — not the `zstd` binary. A subprocess per delta is a fork per changed file on the churn path, and it would make the agent's install "one binary and a new enough zstd". § 6.2. |
| Should git itself be the transport for the cold sync | **Not in Phase 2; revisit in Phase 3 as an optimization for cold population only.** The object store, blake3 addressing, delta transfer and near-free branch switches are git's model rebuilt by hand, and `git fetch` over the same ControlMaster would deliver the committed tree as one delta-compressed packfile instead of ~30 `object/fetch` batches. Two things stop it being the answer rather than an accelerator: it moves only *committed* content, and uncommitted agent output between commits is precisely the churn this design exists for; and git addresses blobs by its own hash, so every fetched object still has to be blake3-hashed to enter the store, which costs the "one hash everywhere" property § 6.2 bought deliberately. Worth measuring against a real cold sync before building. § 6.2. |
| Precondition for renaming onto a path | `expect: absent` on the destination, `RENAME_NOREPLACE`. Without it a rename onto a locally-`absent` path clobbers host bytes the user never saw — the same hole the create precondition closes. § 6.3. |
| Settings the daemon consumes | Sent by each client at attach. The daemon takes the minimum of any budget or cap and the maximum of any timeout or history size, and reports the effective values back; it has no settings registry and may be serving two clients. § 6.12. |
| What the kernel is | An explicit CMake source list with its own PCH and a transitive-include lint, not a set of directories: `terminal/` includes `render/AnsiPalette.h`, `project/` includes `compare/` and `editor/SingleLineEditor.h`, and `editor/` includes upward into `workspace/`. § 8 G1. |
| Where the async completion guard lives | Once, in `editor/`, introduced by G4 and consumed by G5 and G6: a completion carries the tab identity and content revision it was posted against and applies only if both still match. § 8 G4. |
| Project identity versus filesystem root | Two fields. `ProjectId` names the project — persistence keys, presentation, the state-directory key. `ProjectWorkspaceState::root` stays a path, the mirror's `tree/`, for every I/O site. § 6.8, § 8 G8. |
| Does Groundwork have performance gates | Yes, § 9: large-file open and slow-formatter save never exceed the frame budget, an unchanged file is never hashed, the agent binary is small and SDL-free. Speed is the first priority and a preparatory phase must not pay for it locally. |

## 12. Still open

- Version drift between the client and a manually installed agent. The install
  question itself is **closed** — Groundwork G1 makes `microide-agent` a small
  SDL-free binary, so installing it is copying one file rather than shipping a
  `.deb` and its dependency chain. What remains is what happens when someone copies
  it once and never again: `agent/hello` reports both versions and § 6.12 offers to
  restart the daemon, which is a report, not a policy. Phase 3 decides whether the
  client should offer to push a matching binary.
- Whether ignored directories should be *navigable as a tree*, rather than merely
  readable. § 6.5 closed the half that mattered: any host path the mirror does not
  carry is fetched read-only on demand, so go to definition, step into and a stack
  frame in `build/` all work. What is still open is browsing — listing an ignored
  subtree in the sidebar without mirroring it, which needs a metadata-only listing
  and therefore a second content-set concept. That is a convenience, not a
  correctness gap, and it waits for usage rather than speculation.
- Whether `git fetch` should carry the cold sync (§ 11). It is measurable rather
  than arguable: run one cold sync of this repo both ways over a delayed loopback
  link and compare. § 13 already needs that harness.

Everything else that once looked open — agent awareness, the churn tuning
constants, whether Phase 1 ships as a user feature, whether files outside the
content set are reachable — is decided in § 11.

## 13. What was not measured

The empirical latency table (native vs mirror vs xpra at 20/80/200 ms of injected
delay) has not been produced; this machine has no `sshd`, `xpra` or `waypipe`.
The § 3 syscall counts and tree sizes are measured; the § 9 budgets are targets
derived from them. To complete it: install `openssh-server` and `xpra`, run
`microide-agent` over loopback ssh, inject delay with
`sudo tc qdisc add dev lo root netem delay 80ms` (remove with `tc qdisc del dev
lo root`), and measure project open to tree, to Ready, file open, save-to-ack,
search, git status refresh, terminal-to-prompt and keystroke-to-echo under xpra.
The scan harness from § 3 is a `strace -f -tt` wrapper around a headless
`--control` launch on Xvfb; it is a dozen lines and should be rewritten rather
than recovered.
