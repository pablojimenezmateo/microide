## Context

The full design is `dev-docs/design/remote-projects.md`. Sections 6.4 (transport), 6.5 (terminal), 6.6 (lifecycle and install), 6.12 (persistent server), 8 (phasing), 9 (budgets) and 10 / 10.1 (tests and parity) are authoritative for this change; this document records how to build Phase 2a on the tree as it stands on 2026-10-08, and the decisions taken with the user on that date.

What already exists and must be reused rather than rebuilt:

| seam | where | what it gives this change |
| --- | --- | --- |
| `platform::ProcessLauncher` | `src/platform/ProcessLauncher.h` | the one interface every spawn uses. `Run` owns working-directory mapping (callers never pre-resolve), `ResolveWorkingDirectory` / `LocalPathFromHost` are the two-way path map, `ExpandWorkspaceFolder` resolves `${workspaceFolder}` in argv |
| `project::ProjectLocality` | `src/project/ProjectLocality.h` | launcher plus write gate, fixed at `OpenProjectTab` and carried through the project-state reset; the plugin host's type is an alias of it |
| `project::GitMetadataSource` / `GitMetadataFor` | `src/project/GitMetadataSource.h` | a launcher that also implements the interface answers "is this a repository" and "where is the git dir" for its trees; the remote launcher implements it |
| `HostPathTranslator` | `src/workspace/HostPathTranslator.h` | key-scoped path translation of every LSP/DAP message at the shared stdio transport, inactive for a local launcher |
| `FileReadService` with `read_path = false` | `src/project/FileReadService.h` | the shell's off-thread worker with a guarded main-thread completion; used for git work that must not block the shell |
| `platform::ControlSocketServer` | `src/platform/ControlSocketServer.*` | hardened AF_UNIX server: one poll thread, per-client fds, 0600 socket, stale-socket recovery |
| `TerminalSession` + `TerminalBackend` | `src/terminal/` (kernel) | the pty, the parser and the scrollback with `ScrollbackTrimTotal()`; runs on the host unchanged |
| `StdioJsonRpcClientTransport` | `src/workspace/StdioJsonRpcClientTransport.h` | the queue, wedged-peer teardown and I/O-thread discipline to copy for the binary transport |
| `NotificationService` | `src/workspace/services/NotificationService.h` | keyed, sticky, progress rows; actions are what this change adds |
| `SaveFormatterService` deferred saves | `src/workspace/services/SaveFormatterService.*`, `ApplyDeferredSaveFormat` | the completion hook where save continuations run |
| parity harness | `tests/parity/` | `LoopbackProcessLauncher`, `RecordingLocalLauncher`, split host/mirror trees, outcome and spawn-count comparison, shrink-only known gaps |

## Goals / Non-Goals

**Goals:**
- A user can open a terminal on a host over stock ssh, close the laptop, and reattach to the same running shell with its history.
- Every process a remote project would spawn can run on the host through `RemoteProcessLauncher`, proven by the parity suite against the real server over a pipe.
- Typed characters appear in the keystroke's frame on a far link, and are never wrong for more than a round trip.
- Nothing about local projects gets slower or changes behavior, except that quit and close-project stop freezing on formatters.

**Non-Goals:**
- Opening a remote *project*: no mirror, no manifest, no `file/write`, no `watch/*`, no `search/run`, no `git/status` push (Phase 2b).
- `ProjectId` and the persisted-format migration (G8), blake3 manifest hashes (rest of G6) — Phase 2b.
- Remote git over `proc/spawn` for a LOCAL checkout: the design rejects it as silent divergence.
- Handing ptys to a restarted server binary (Phase 3).
- Moving save participants off the shell thread, and compare/merge saves (TD-2026-09-28-304 remainder).

## Decisions

1. **Build the prerequisites first, in this order: kernel test binary, notification actions, save continuations.** Each is small and independently shippable. The server links only the kernel, so the kernel needs its own SDL-free test executable under every sanitizer lane before server code depends on it (TD-2026-09-22-303). The connection UI needs action buttons (Reconnect, Copy Install Command). Quit with a host session open must not freeze. *Alternative:* build the server first and retrofit — rejected, because the server's tests and UI would be written against stand-ins.

2. **Notification actions are `ActionId` + arguments, rendered as VS Code-style inline buttons.** Decided with the user on 2026-10-08. Data rather than closures, so a keyed row can be reposted, the control channel can list and invoke them, and plugins can contribute them later through the existing action vocabulary. Composition (labels, truncation, button rects) happens in `RenderViewModelBuilder`; the render TU only draws (the render lints forbid string materialization there). *Alternative:* a "More actions…" command-palette route — rejected as undiscoverable.

3. **Quit and close-project wait for formatter saves with a progress row and a Cancel button.** Decided with the user on 2026-10-08. The application never exits with a save in flight and never silently exits unformatted on a timeout. One continuation queue serves close, rename/delete, project close and quit, replacing `EditorTabState::close_after_save`, `EditorTabState::path_mutation_after_save` and `PromptState::deferred_path_mutation`. A continuation records the tab ids it waits on (stable ids, not indices — tabs close in completion order) and re-checks its preconditions when it runs; any refused or failed save cancels it with a notification. *Alternatives:* blocking with the existing 5 s cap (the freeze this removes); exit unformatted after a timeout (loses the formatting the user asked for).

4. **First server slice = frame codec + daemon skeleton + `proc/spawn` + `RemoteProcessLauncher`, tested as the third parity locality over a pipe.** `microide-server attach` speaks the protocol on stdio, so the test harness spawns it locally and talks to it through a pipe — no ssh, no socket — and every existing parity row (save, file ops, format-on-save, plugin tools, git sidebar, terminal, language server, real gdb, real clangd) runs against the real server immediately. This proves the protocol shape and the launcher contract before terminals are built on them. *Alternative:* terminals first, as the user-visible feature — rejected because they would be built on an unproven `proc/*` and lane design.

5. **The server is its own process tree under `src/server/`, linking `microide_kernel` only.** A CMake target with no SDL in its link line, plus an extension of `CheckKernelStaysFreeOfTheWindowingLibrary` (or a sibling rule) over `src/server/`, so the build enforces it. Release builds are static musl; dev and test builds use the host toolchain so sanitizers work.

6. **The binary transport copies the stdio transport's discipline, not its codec.** One I/O thread with poll, bounded outbound queues per lane, a wedged-peer teardown, perf counters. Frames: `uint32 length`, `uint16 type`, `uint8 lane`, `uint64 id`, then payload; control payloads are JSON (`util::JsonValue`), content payloads raw. `RemoteServerClient` owns the transport; `RemoteHostSession` owns the ssh master, the state machine and the `ProjectLocality` it hands out.

7. **Process stdio and terminal lines are credit-windowed per handle; screens are never dropped.** The credit window is what keeps one runaway stream from tearing down the connection. Stdio beyond the window waits on the server; terminal lines beyond it are dropped at the ring with a counted gap (the design's rule).

8. **`TerminalPredictionOverlay` lives in `src/terminal/` as a pure value type.** No I/O, no clocks of its own (RTT is an input), unit-tested against recorded frame sequences, including mode changes during a burst. `RemoteTerminalView` composes it with the screen and scrollback the client was sent, and the existing terminal renderer draws `RemoteTerminalView` the way it draws a local session.

9. **Connection failures are sticky notification rows with actions, not modal dialogs.** NeedsAuth is a terminal tab; Installing and Reconnecting are progress rows; failures carry Reconnect / Show Log / Copy Install Command / Copy ssh Command.

## Risks / Trade-offs

- [The daemon is one of the two components that carry nearly all the correctness risk (§ 8.1).] → Build it test-first against the pipe harness; every lifecycle rule in `remote-server-daemon` has a scenario; run ASAN/TSAN on the server binary in the lanes from its first commit.
- [Prediction draws glyphs the user did not type.] → Pure overlay, adaptive by default (never visible on a LAN), contradicted predictions suppress further ones; recorded-frame tests include the mode-change-during-burst case.
- [Self-install writes a binary to the user's home over ssh.] → One fixed script, directory as `$0`, validated character set, 0700, temp-then-rename; `remote.server_install = off` for users who manage it themselves.
- [Static musl and sanitizers do not mix.] → Release-only static linking; dev/test builds link normally so every lane covers the server.
- [Real-tool tests can pass vacuously by taking a skip path.] → Decide every real-tool skip from a LOCAL reference run, never from the run under test (the gdb parity row first passed with translation switched off). Keep a positive control for every new lint and parity check.
- [Sanitizer lanes compiling while the source is edited report false failures.] → Never edit source while `tools/run-checks.sh` is building a lane.
- [A host that kills user processes at logout makes "survives a disconnect" false.] → Reported at connect from logind config, as a sticky row; documented, not hidden.

## Migration Plan

No persisted format changes in this phase. New settings are additive and registered in `WorkspaceSettingsRegistry`. The save-continuation refactor replaces two flags and one parked prompt internally; behavior for tab close and rename/delete is unchanged and covered by existing tests (`RenameOfDirtyFileWaitsForTheFormatterNotTheWindow`, the close-after-save tests). Rollback is reverting the change; nothing on disk depends on it.

## Open Questions

- Whether the release build bundles one `microide-server` per supported host architecture (x86_64, aarch64) inside the `.deb`, or fetches it once through `WorkspaceToolDownloader` with a sha256 manifest. Bundling is simpler and works offline; recommend bundling x86_64 and aarch64.
- The exact semantic key event encoding on the wire (keysym + modifiers is decided; whether to reuse SDL keycodes or define a kernel-level key enum). Recommend a kernel enum: the server must not depend on SDL.
