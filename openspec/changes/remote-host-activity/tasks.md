## 1. Server accounting

- [ ] 1.1 `ProcessTable::Process`: `root` and bounded `argv` (8 × 256 bytes) recorded at spawn; `ProcessTable::List()` under the mutex (handle, root, argv, pid, cwd, kept, detached_at, exited, buffered bytes)
- [ ] 1.2 `TerminalTable::List()` (from `remote-terminal-restore`) also returns the shell pid and the scrollback line count
- [ ] 1.3 `server::ProcessActivity`: one `/proc` walk per sample (ppid, ticks, RSS from one `stat` read per pid), subtree sums per requested pid, delta CPU against the previous sample, gone pids dropped; kernel test against a spawned `sh -c 'sleep 30 & sleep 30'` tree and a tight loop (share > 0 on the second sample, `-1` on the first)
- [ ] 1.4 `RemoteServer::StatusJson(bool activity)`: the process rows, the per-terminal pid and scrollback lines, the per-workspace counts/files/hashed/watch (no zeros left), and with `activity` the server figures and per-row `rss_bytes`/`cpu_permille`; `server/status` takes `{activity?: bool}`; kernel test with a terminal and a kept process under two roots, with and without `activity`
- [ ] 1.5 `activity/subscribe` (reply = status with activity; first subscriber creates `activity_sampler_`, a 2 s `PeriodicSampler` beside `port_sampler_`), `activity/changed` pushes per tick, `activity/unsubscribe` and connection close (last subscriber destroys the sampler); `Shutdown` stops it with the port one; kernel test: pushes arrive while subscribed, none within 5 s after the last unsubscribe, and a subscriber that closes its connection is dropped
- [ ] 1.6 `microide-server status`: the table printer (two `activity: true` samples a second apart for the CPU column) and `--json` (a plain `server/status`); test of the table on a fixture reply

## 2. Protocol and client

- [ ] 2.1 `RemoteProtocol.{h,cpp}`: `ServerStatus`/`ServerStatusFromJson` (from the restore change) grow the process rows, the per-terminal pid/scrollback and the optional figures; the new method names; unit tests including a hostile reply (huge counts, negative pids, non-string argv, figures of the wrong type → not reported)
- [ ] 2.2 `RemoteServerClient::SubscribeActivity(handler)` / `UnsubscribeActivity()` mirroring `SubscribePorts`, the `activity/changed` notification handler; `kErrorUnknownMethod` from an older server = no subscription, no error

## 3. The Host view

- [ ] 3.1 `SidebarMode::Host` (in `UsesPluginItemSurface`), `SidebarViewSpec{"host", "Host", contextual}`; shown while a host is connected (the Changed on Host rule); never in the view switcher or the View menu
- [ ] 3.2 `RemoteHostService::HostRows(host)` from the last `ServerStatus`: the four groups and their rows, figures precomposed once per push, "not reported" rendered as `…`; `ShowHost(host)`
- [ ] 3.3 Subscribe on show, unsubscribe on hide/mode switch/connection leaving `Ready`, resubscribe on `came_back` while visible (D5); pushes through `mailbox_.PostLatest`; a restored session with the view visible subscribes once `Ready`
- [ ] 3.4 Enter: focus the tab holding the terminal's handle, else `RemoteAttachHostTerminals <host> <handle>`; Server row → Stop Host Server's confirm; process and workspace rows → nothing
- [ ] 3.5 Delete: one key case on the item surface routed to the mode's row-end action (only `Host` declares one); terminal rows → `RemoteCloseHostTerminals <host> <handle>` (a terminal another client holds is reported as such rather than ended), process rows → new `RemoteEndHostProcess <host> <handle>` (`proc/signal` SIGTERM, then `proc/release`); confirm when running
- [ ] 3.6 `RemoteShowStatus` opens the view; `StatusText()` keeps the text answer for `--control`, with the host totals from the last status seen; the status segment is untouched (it opens the Remote menu)
- [ ] 3.7 Tests (fake-ssh): rows for a project with one terminal running `yes > /dev/null` (CPU > 0 on the second push) and one kept tool; End on each ends it on the host; no `activity/changed` on the wire for 5 s after the view hides; an older server's reply (keys missing, subscribe unknown) renders the connection rows and the terminal rows with `…` figures and no error

## 4. Availability, menus and the palette

- [ ] 4.1 `WorkspaceActionAvailability` for `RemoteEndHostProcess` (connected host only); command registry entry `remote-end-process <host> <handle>`; the Remote menu's Show Status item needs no change (same `ActionId`)
- [ ] 4.2 Help/About and `ui-command-labeling` strings for the new command

## 5. QA, docs and lanes

- [ ] 5.1 `qa-remote` scenario `V`: open the view (palette and Remote > Show Status), assert the Terminals group lists the project's terminal with a pid; run `yes > /dev/null` in it, assert its CPU figure rises within 5 s; Delete it with confirm, assert `pgrep yes` empty on the host and the row gone; hide the view, assert no `activity/changed` in the server log for 5 s; also under `qa-slow`
- [ ] 5.2 Verify by hand on `worky`: `microide-server status` table, then the view with a build running
- [ ] 5.3 Docs: `remote-projects.md` § 6.12 (`status`) and a § 7.9a for the view; `active-work.md`; `known-tech-debt.md` closes the "per-workspace accounting: TD" note in `StatusJson`
- [ ] 5.4 Lanes, tests in batches of 16: `tools/run-checks.sh tests`, `qa`, `qa-remote`, `asan`, `ubsan`, `tsan` (a second sampler thread), `clang-build`, and `hardened` (the `/proc` parser runs on untrusted-shaped input)
