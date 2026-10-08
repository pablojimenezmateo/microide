## 1. Kernel test binary (TD-2026-09-22-303)

- [x] 1.1 Add `microide_kernel_tests`: a second test executable linking only `microide_kernel` + the shared test support that has no SDL dependency, registered with the same `--shard-index/--shard-count` sharding as `microide_tests`
- [x] 1.2 Move every test TU that names no SDL and touches only kernel sources into it (start from the ~120 SDL-free test TUs; keep any that need `WorkspaceShell` in `microide_tests`)
- [x] 1.3 Build and run it in `tools/run-checks.sh` for tests, asan, ubsan, tsan, clang-build and coverage; build `microide_kernel_link_probe` in the sanitizer and clang lanes too
- [x] 1.4 Resolve TD-2026-09-22-303 in `dev-docs/project/known-tech-debt.md`

## 2. Notification actions (G7, VS Code-style inline buttons)

- [x] 2.1 Add `std::vector<NotificationAction>{label, ActionId, args}` to `NotificationService::Request` and `Notification`; reposting a keyed row replaces its actions in place
- [x] 2.2 Compose button labels, truncation and rects in `RenderViewModelBuilder`; draw them in the notification render TU with no string materialization (render lints)
- [x] 2.3 Hit-test and dispatch a click through the action executor; dismiss a transient row after its action unless the action opts out; keep a hovered row alive past expiry and give rows with actions a longer default lifetime
- [x] 2.4 Keyboard: focus the notification stack and move between buttons; Enter invokes
- [x] 2.5 Control channel: list a row's actions and invoke one by label or id (`dev-docs/control/control-channel.md`)
- [x] 2.6 First consumers: `Show Output` on formatter failures, `Compare` on the external-change banner row if it is a notification, `Reload` where a reload is offered — done 2026-10-08: `Show Output` (new `show-output <channel-id>`) on formatter failures. No notification offers Compare or Reload today: the external-change prompt is an editor banner, not a toast, so those two consumers do not exist yet
- [x] 2.7 Tests: view-model tests for layout and truncation, a click-dispatch test, a control-channel invoke test, an allocation check in the perf-tests lane for the steady-state render

## 3. Save continuations (rest of TD-2026-09-28-304)

- [x] 3.1 Introduce a continuation queue keyed by stable tab ids: register `(tab ids, precondition, run, cancel-message)`; `ApplyDeferredSaveFormat` completes a tab and runs every continuation whose tabs are all done, cancelling on any failed or refused save
- [x] 3.2 Port tab close (`close_after_save`) and rename/delete (`path_mutation_after_save`, `PromptState::deferred_path_mutation`) onto it and delete the old flags; existing tests must pass unchanged
- [x] 3.3 Close-project: defer the dirty buffers' saves, show a sticky progress row with a Cancel action (needs 2.x), tear down after the last write; never run a continuation against a switched-away project
- [x] 3.4 Quit: same, "Saving N files before quitting…" with Cancel; exit when the last write lands; Cancel aborts the quit and leaves buffers open; no exit with a save in flight, no silent unformatted exit
- [x] 3.5 Remove the remaining `SaveMode::Blocking` callers that 3.3/3.4 replace and tighten the `CheckNoSynchronousSubprocessInWorkspace` allowlist accordingly — done 2026-10-08: quit/close-project no longer save Blocking; the `RunBlocking` allowlist was already the save-pipeline TU only (compare/merge saves keep it), so the tightening is `SaveMode` losing its `Blocking` default — every caller now names its mode
- [x] 3.6 Tests: quit and close-project with a slow formatter keep the shell responsive and finish; Cancel leaves the buffer open and dirty; a refused save cancels the continuation with a notification
- [x] 3.7 Update TD-2026-09-28-304 (only save participants and compare/merge saves remain)

## 4. Protocol codec and transport

- [x] 4.1 `src/project/remote/RemoteFrame.{h,cpp}`: header `uint32 length, uint16 type, uint8 lane, uint64 id`, JSON control payloads, raw content payloads, the 64 MiB ceiling; incremental decoder that never allocates an announced size before validating it
- [x] 4.2 Fuzz target for the decoder (`tests/fuzz/`, corpora checked in), in the `PersistedRecordReaderFuzz` pattern
- [x] 4.3 Binary transport with the stdio transport's discipline: one poll I/O thread, per-lane bounded outbound queues, interactive-first draining, wedged-peer teardown, perf counters
- [x] 4.4 Adaptive bulk in-flight bound (~100 ms of measured bandwidth, 64 KiB floor, 1 MiB cap) and `op/cancel`
- [ ] 4.5 `server/hello` with protocol version + minimum, release version, `daemon_epoch`, capabilities, session-survival report, effective settings; mismatch errors naming both release versions — messages, validation and the compatibility check done 2026-10-08 (`RemoteProtocol`); the effective-settings echo lands with the settings in 8.6
- [x] 4.6 `link/ping` every 2 s on the interactive lane with clock and RTT estimate; three misses = dead link
- [x] 4.7 Unit tests: round trips of every frame type, a UTF-8 sequence split across content frames, oversized and truncated frames, lane ordering under a queued bulk backlog

## 5. Server daemon skeleton

- [x] 5.1 CMake target `microide-server` from `microide_kernel` + `src/server/`; extend the windowing-free lint to `src/server/`; static musl only in release — done 2026-10-08 except the static musl link, which belongs to the release packaging (8.3 bundles the binary); the lint is `CheckServerIncludesOnlyTheKernel` plus `server/` in the kernel SDL rule
- [x] 5.2 Subcommands `start`, `stop`, `status`, `attach`; `attach` speaks the protocol on stdio (relay to the socket, or serve directly for tests)
- [x] 5.3 Socket dir `~/.local/state/microide/server/` (override `remote.server_socket_dir`), verified on every bind (owner, 0700, no symlinked component, `O_PATH|O_NOFOLLOW`), reusing `ControlSocketServer`
- [x] 5.4 Daemonize: `setsid`, stdio to `/dev/null`, size-capped log beside the socket, success = socket connectable; every fd close-on-exec at creation
- [x] 5.5 One daemon per uid, workspaces per root, idle-exit rules (hand-started never idles out), session-survival report from logind config
- [x] 5.6 Tests: bind refusal on bad modes and symlinks, attach-or-start, the starter does not hold stdio, idle exit for on-demand only, `status` output

## 6. proc/spawn and RemoteProcessLauncher (first vertical slice)

- [ ] 6.1 Server: `proc/spawn` (argv array, host cwd, env additions, `keep_on_detach`), `proc/stdin`, `proc/stdout`, `proc/stderr`, `proc/signal`, `proc/exit`; per-stream byte offsets; per-handle credit window
- [ ] 6.2 `RemoteServerClient` (owns the transport) and `RemoteProcessLauncher` implementing `platform::ProcessLauncher` and `project::GitMetadataSource`; `Run` collects to completion; long-lived spawns (`AsyncSubprocess` users: LSP, DAP) get a process handle whose stdio is the protocol
- [ ] 6.3 Parity harness: add the `server` locality — spawn `microide-server attach` locally over a pipe with split host/mirror roots and a host root on the server side; run every existing `Parity/*` row against it, including the spawn-count check
- [ ] 6.4 Real gdb and clangd rows against the server locality (copy the loopback rows; skips decided by a local reference run)
- [ ] 6.5 Tests: argv fidelity (space, quotes, `$HOME`, newline), exit status and signals, a kept process surviving a transport drop and resuming stdio from its offset with no duplicates, a non-kept process terminated on detach, exit while detached delivered on attach

## 7. Host terminals

- [ ] 7.1 Server: `term/open`, `term/resize`, `term/close` over the kernel's `TerminalSession`; screen deltas coalesced to 16 ms / 33 ms; `term/lines` under the credit window with counted gaps; capture bits in every `term/screen`
- [ ] 7.2 Semantic input: a kernel key/mouse event enum (no SDL), `term/input` with `input_seq`, host-side encoding through the existing `FormatTerminalKeyPress` / paste / mouse encoders; `echo_ack` per the 50 ms rule
- [ ] 7.3 `term/event`: title, bell, OSC 7 (client maps with `LocalPathFromHost`), OSC 52 under the local clipboard policy
- [ ] 7.4 Host-authoritative scrollback with trim-total offsets; attach sends screen + `remote.scrollback_prefetch_lines`; `term/scrollback` backfill; shells that exited while detached keep their output until seen; two-client smaller-size rule
- [ ] 7.5 `src/terminal/TerminalPredictionOverlay` (pure) and `RemoteTerminalView`; the terminal renderer draws a remote view like a local session; pending predictions underlined
- [ ] 7.6 Tests: recorded-frame prediction tests (confirm, contradict and suppress, right margin, hidden cursor, alternate screen, mode change during a burst), reattach with a partial buffer duplicates nothing, a fresh client gets the tail, ring overflow shows a counted gap, two clients agree

## 8. Connection lifecycle, install and UI

- [ ] 8.1 `RemoteHostSession`: ssh ControlMaster per (user, host, port) with `BatchMode`, validated host/user strings after `--`, short control path; state machine Disconnected → Connecting → (NeedsAuth) → StartingServer → (Installing) → Ready → Reconnecting/Offline
- [ ] 8.2 NeedsAuth in a terminal tab running ssh without `BatchMode`; continue when the control socket appears
- [ ] 8.3 Self-install over the existing connection with the one fixed script; bundled server per architecture; `remote.server_install = off` shows the copy command; mismatch with live terminals installs beside and asks before restart
- [ ] 8.4 Reconnecting = attach with resume of every terminal and kept process; `daemon_epoch` mismatch falls back to a cold attach
- [ ] 8.5 `workspace/services/RemoteHostService` (no new shell companion): `Remote: Open Terminal on Host…`, `Remote: Show Status`, `Remote: Stop Host Server`, status-bar segment, sticky notification rows with actions (Reconnect, Show Log, Copy Install Command, Copy ssh Command), session-survival warning row
- [ ] 8.6 Register `remote.server_command`, `remote.server_install`, `remote.server_socket_dir`, `remote.predict`, `remote.term_credit_bytes`, `remote.backfill_inflight_bytes`, `remote.scrollback_prefetch_lines` in `WorkspaceSettingsRegistry`
- [ ] 8.7 Tests with a `remote.ssh_command` seam pointing at a shim: auth fallback, install path, link death within 6 s and resume, host-string rejection

## 9. Budgets, lanes and wrap-up

- [ ] 9.1 Perf-harness scenarios with `--server-delay-ms`: first prompt, predicted glyph in the keystroke frame, confirmed echo ≤ 1 RTT + 5 ms under a 50 MiB bulk transfer, contradicted prediction gone in 1 RTT, dead link ≤ 6 s, reattach of 3 terminals ≤ 2 RTT, no frame > 16 ms with a stalled server
- [ ] 9.2 Run the full lanes once at the end (`tools/run-checks.sh tests asan ubsan tsan clang-build perf-tests`), never editing source while a lane builds
- [ ] 9.3 Update `dev-docs/design/remote-projects.md` § 8 status, `dev-docs/project/active-work.md`, the control-channel doc for the new actions, and `CHANGELOG.md`
