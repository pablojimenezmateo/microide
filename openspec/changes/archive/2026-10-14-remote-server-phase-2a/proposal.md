## Why

Code is increasingly written by agents on remote machines, and microide cannot yet run anything there. The groundwork is done: every spawn takes the project's launcher, every write goes through the project's gate, LSP and DAP paths are translated at the transport, git metadata is asked of the host, and a local/remote parity harness proves all of it against a simulated host. What is missing is the host itself.

Phase 2a of `dev-docs/design/remote-projects.md` (§ 8) is the first piece that runs on another machine. It ships on its own as a user-visible feature: **a host's terminals in microide, that survive a dropped link and reattach, with predicted echo**. It is also where every protocol-shape decision lives, so it has to land before the mirror (Phase 2b) can build on it.

## What Changes

- **New binary `microide-server`**, built from the SDL-free kernel and statically linked against musl. It runs as one daemon per user, serving many roots as workspaces. Its subcommands are `start`, `stop`, `status` and `attach`.
  - **Socket:** under `~/.local/state/microide/server/`. The directory is checked for owner, mode and symlinks on every bind, as tmux does.
  - **Lifetime:** a server started by hand never exits for idleness. One started on demand exits when idle.
  - **Session survival:** the server reports whether processes survive logout (`KillUserProcesses`, linger).
- **New wire protocol.**
  - **Frames:** length-prefixed binary frames with `length`, `type`, `lane` and `id`. Control frames carry a JSON body. Content frames carry raw bytes.
  - **Lanes:** two priority lanes, interactive and bulk, with an adaptive bound on bulk bytes in flight.
  - **Versioning:** a protocol version with a minimum-accepted floor, independent of the app version.
  - **Methods in this phase:** `server/hello`, `server/attach`, `server/shutdown`, `link/ping`, `op/cancel`, `proc/*`, `term/*`.
- **`proc/spawn`: every remote process is a child of the server.**
  - argv travels as an array, never as a shell string.
  - Each stdio stream has a credit window and a byte offset, so it can resume.
  - `keep_on_detach` decides whether a process outlives a dropped link.
  - A new `RemoteProcessLauncher` implements `platform::ProcessLauncher`, and `project::GitMetadataSource`, over `proc/spawn`. Git, the formatter, plugin tools, the language server and the debug adapter therefore all run on the host without call-site changes.
- **Host terminals.**
  - **Host side:** the pty and the terminal model live on the host. The wire carries screen deltas plus credit-windowed scrollback lines, never the raw byte stream.
  - **Input:** keystrokes travel as semantic events, each with an `input_seq`. The host encodes them, because it owns the terminal modes.
  - **Prediction:** a pure `TerminalPredictionOverlay` predicts echo mosh-style, validated by `echo_ack`. It is adaptive by default and only predicts when the round trip exceeds 30 ms.
  - **Reattach:** the host holds the authoritative scrollback. Reattach resumes from the trim-total offset.
  - **Overflow:** lines dropped at the ring are shown as a visible gap.
- **Connection lifecycle on the client.**
  - **States:** Disconnected, Connecting, NeedsAuth, StartingServer, Installing, Ready, Reconnecting and Offline.
  - **Transport:** one ssh ControlMaster per host and one channel per project, over stock `sshd`, with no root and no `sshd_config` change.
  - **Auth:** authentication happens in a terminal tab. No password field exists anywhere.
  - **Self-install:** if the server is missing or its protocol version does not match, the client installs it into `~/.local/share/microide/server/` over the existing connection.
  - **Heartbeat:** a `link/ping` every 2 s detects a dead link within about 6 s.
- **UI.**
  - **Command:** `Remote: Open Terminal on Host…` opens a host terminal. This phase opens no remote *project*.
  - **Status:** a status-bar segment and `Remote: Show Status`.
  - **Notifications:** connection state uses sticky notification rows with **VS Code-style inline action buttons**, such as Reconnect, Show Log and Copy Install Command.
- **Prerequisites folded into this change.** These are small, and the phase's UI or tests need them.
  - **Notification actions (G7).** Inline buttons on notification rows, each carrying an `ActionId` plus arguments rather than a closure, rendered from the view model and clickable or keyboard-reachable.
  - **Quit and close-project wait for formatters.** They wait **with a Cancel button**, instead of freezing (TD-2026-09-28-304). One "after save" continuation queue replaces today's `close_after_save` flag and the parked `deferred_path_mutation`.
  - **Kernel test binary `microide_kernel_tests` (TD-2026-09-22-303).** The server links only the kernel, so the kernel's tests must run without SDL and under every sanitizer lane.
- **Testing.**
  - **Parity:** the real server, spoken to over a pipe, becomes the third locality of the parity harness (`tests/parity/`). Every existing parity scenario runs against it with no ssh involved.
  - **New scenarios:** terminals, `proc/spawn` argv fidelity, detach and reattach, and link death.
  - **Fuzzing:** the frame decoder becomes a fuzz target.

Nothing here is **BREAKING** for local projects. Groundwork G8 (`ProjectId`) and the mirror are deliberately out of scope: this phase opens no remote project.

## Capabilities

### New Capabilities

- `remote-server-protocol`: frame format, lanes and the bulk in-flight bound, protocol versioning and the handshake, heartbeat and link-death detection, cancellation, per-handle credit windows, and the untrusted-data rules for every decoded field.
- `remote-server-daemon`: the `microide-server` binary and its subcommands. Covers one daemon per uid with workspaces per root, socket placement and verification, daemonization that detaches stdio, idle-exit rules, the session-survival report, `daemon_epoch`, attach-or-start, and self-install over the existing connection.
- `remote-process-spawn`: `proc/spawn` semantics (argv array, cwd as a host path, environment additions, stdio offsets, signals, exit status, `keep_on_detach`), plus `RemoteProcessLauncher` as the project launcher and git metadata source of a host session.
- `remote-terminal`: host-owned pty and terminal model, screen deltas and scrollback lines, semantic input with host-side encoding, `term/event`, the size rule for multiple clients, the prediction overlay with `echo_ack`, and authoritative host scrollback with offset resume and visible gaps.
- `remote-connection-lifecycle`: client states and transitions, ssh ControlMaster handling with validated host strings, NeedsAuth in a terminal tab, Installing, Reconnecting with resume, the `Remote: Open Terminal on Host…` entry point, status-bar segment, and notification rows with inline actions.
- `notification-actions`: inline action buttons on notification rows, both transient and sticky. Actions are an `ActionId` plus arguments, rendered and hit-tested from the view model, keyboard reachable, and invocable from the control channel.
- `save-continuations`: one queue of work that runs after a deferred save lands, used by tab close, rename and delete, project close and quit. Quit and close-project show a progress row with a Cancel action and never freeze the shell thread on a formatter.

### Modified Capabilities

- `performance-budgets`: adds the Phase 2a remote budgets.
  - **Time budgets, measured in the perf harness:** terminal first prompt; predicted echo in the keystroke's frame; confirmed echo within 1 RTT + 5 ms under a concurrent bulk transfer; link death detected within 6 s; warm reattach within 2 RTT to first repaint.
  - **Count budgets, in the parity suite:** a remote run may not start more host processes than the local run.
- `workspace-architecture`: remote components live outside the shell, in `src/project/remote/` plus one workspace-level `RemoteHostService`. No new `WorkspaceShell*.cpp` companion and no shell-member growth beyond the existing ratchets. The server links only `microide_kernel`, enforced by build and lint.

## Impact

- **New code:**
  - `src/project/remote/`: the frame codec, `RemoteServerClient`, `RemoteHostSession`, `RemoteProcessLauncher` and `RemoteTerminalView`.
  - `src/server/`: the daemon, its socket and workspace management, and host terminals reusing `TerminalSession` from the kernel.
  - `src/terminal/TerminalPredictionOverlay.*`.
  - A `workspace/services/RemoteHostService`.
- **New targets:** `microide-server` (static musl in release, ordinary in dev and test), `microide_kernel_tests`, and a fuzz target for the frame decoder.
- **Reused, not rewritten:** `platform::ControlSocketServer` (AF_UNIX, poll thread, stale-socket recovery), `AsyncSubprocess`, the stdio transport's queue and teardown discipline, `TerminalSession` and its backend, `HostPathTranslator`, `GitMetadataSource`, `ProjectLocality`, and `FileReadService` for off-thread waits.
- **Settings:** new settings must be registered in `WorkspaceSettingsRegistry`. They are `remote.server_command`, `remote.server_install`, `remote.server_socket_dir`, `remote.predict`, `remote.term_credit_bytes`, `remote.backfill_inflight_bytes` and `remote.scrollback_prefetch_lines`.
- **Docs:** update `dev-docs/design/remote-projects.md` § 8 status, `dev-docs/project/active-work.md`, and `dev-docs/project/known-tech-debt.md` to resolve TD-303 and the rest of TD-304.
- **Size:** about 4,200 production lines and 4,400 test lines per the design's sizing. The prerequisite slices add roughly 900 production lines.
