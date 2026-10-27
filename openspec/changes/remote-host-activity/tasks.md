## 1. Server accounting

- [ ] 1.1 `ProcessTable::Process`: `root` and bounded `argv` recorded at spawn; `ProcessTable::List()` under the mutex (handle, root, argv, pid, cwd, kept, detached_at, exited, buffered bytes)
- [ ] 1.2 `TerminalTable::Terminal`: `root` recorded at open (shared with `remote-terminal-restore` § 6.1); `TerminalTable::List()` (handle, root, cwd, title, shell pid, clients, exited, scrollback lines)
- [ ] 1.3 `server::ProcessActivity`: one `/proc` walk per request (ppid, ticks, RSS), subtree sums per requested pid, delta CPU against the previous sample, gone pids dropped; kernel test against a spawned `sh -c 'sleep 30 & sleep 30'` tree and a tight loop (share > 0 on the second sample, `-1` on the first)
- [ ] 1.4 `RemoteServer::StatusJson`: the new keys (D1), per-workspace counts no longer zero; kernel test with a terminal and a kept process under two roots, and the figures for the server itself
- [ ] 1.5 `microide-server status`: the table printer (two samples a second apart for the CPU column) and `--json`; test of the table on a fixture reply

## 2. Protocol and client decoding

- [ ] 2.1 `RemoteProtocol.h`: `ServerActivity` record and `ServerActivityFromJson` (validating; a missing or mistyped field = not reported); unit tests including a hostile reply (huge counts, negative pids, non-string argv)
- [ ] 2.2 `RemoteServerClient::Activity()` (a `server/status` call on the host-job queue)

## 3. The Host view

- [ ] 3.1 `SidebarMode::Host`, `SidebarViewSpec{"host", "Host", contextual}`; shown while a host is connected (the Changed on Host rule)
- [ ] 3.2 `RemoteHostService::HostRows(host)`: the four groups and their rows, figures precomposed once per refresh; `ShowHost(host)`
- [ ] 3.3 The two-second cadence while visible (D5): start on show, stop on hide/mode switch/connection leaving `Ready`, ask at once on show; a serial in-flight guard
- [ ] 3.4 Enter: focus the tab holding the terminal's handle, else attach it in a new tab (restore change's cold attach); Server row → Stop Host Server's confirm
- [ ] 3.5 Delete: `RemoteEndHostTerminal` / `RemoteEndHostProcess` with a confirm when running; a terminal another client holds is reported as such rather than ended
- [ ] 3.6 `RemoteShowStatus` opens the view; `StatusText()` keeps the text answer for `--control`, with the host totals; the status bar's remote segment opens the view on click through `StatusBarService`
- [ ] 3.7 Tests (fake-ssh): rows for a project with one terminal running `yes > /dev/null` (CPU > 0 on the second refresh) and one kept tool; End on each ends it on the host; refresh stops when the view is hidden (no `server/status` on the wire for 5 s); an older server's reply (keys missing) renders "not reported"

## 4. Availability, menus and the palette

- [ ] 4.1 `WorkspaceActionAvailability` for the new ids (connected host only); command registry entries `remote-end-terminal <host> <handle>`, `remote-end-process <host> <handle>`; the View menu lists Host beside Changed on Host while contextual
- [ ] 4.2 Help/About and `ui-command-labeling` strings for the new commands

## 5. QA, docs and lanes

- [ ] 5.1 `qa-remote` scenario `H`: open the view (palette), assert the Terminals group lists the project's terminal with a pid; run `yes > /dev/null` in it, assert its CPU figure rises within 5 s; Delete it with confirm, assert `pgrep yes` empty on the host and the row gone; hide the view, assert no further `server/status` in the server log for 5 s; also under `qa-slow`
- [ ] 5.2 Verify by hand on `worky`: `microide-server status` table, then the view with a build running
- [ ] 5.3 Docs: `remote-projects.md` § 6.12 (`status`) and a § 7.9a for the view; `active-work.md`; `known-tech-debt.md` closes the "per-workspace accounting: TD" note in `StatusJson`
- [ ] 5.4 Lanes, tests in batches of 16: `tools/run-checks.sh tests`, `qa`, `qa-remote`, `asan`, `ubsan`, `tsan`, `clang-build`, and `hardened` (the `/proc` parser runs on untrusted-shaped input)
