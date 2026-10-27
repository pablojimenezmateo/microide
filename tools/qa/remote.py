#!/usr/bin/env python3
"""The remote-project scenarios against a real ssh host, on the tools/qa driver.

    MQA_REMOTE_HOST=worky MQA_REMOTE_ROOT=/home/me/repo tools/qa/remote.py [scenario ...]

The host directory must be a git repository; it is reset to MQA_REMOTE_BASE (default:
its HEAD at the start of the run) before and after, so every scenario starts from
known content and the run leaves nothing behind. Needs ssh key auth to the host in
BatchMode, the built editor (MICROIDE_BIN) and the static server for the host's
architecture (MICROIDE_SERVER_BINARY, default build/server-x86_64/...; the host
installs it on the first connect).

What it checks, each as the user would see it, with the measured latencies:

  A  open + first sync         the mirror equals the host's content set, state == ready
  B  save                       a typed edit reaches the host; the tab is clean
  C  host edit, clean tab       the buffer reloads silently; a later save keeps both
  D  host edit, dirty tab       save refused -> banner; Compare opens a compare tab;
                                Overwrite pushes the buffer
  D2 ...                        Reload takes the host's; the next save pushes
  E  offline save               journaled (status.remote.unsentPushes == 1), replayed
                                on reconnect
  F  link death                 the ssh master is killed; a save still lands after the
                                automatic reconnect
  G  terminals                  Terminal > New prints the host's `hostname`; Remote:
                                Open Local Terminal runs a shell on this machine
  H  clangd on the host         go to definition in a self-contained file
  I  host branch switch         no crash; the tree follows
  J  agent churn                saves land while N host agents rewrite the tree; the
                                trees agree once they stop
  K  git on the host           a saved edit is a change in the git view; staging and
                                committing run on the host
  L  relaunch on the mirror     the project reconnects from its record and a save pushes
  M  project search             a file the host just created is found over the mirror
  N  tree operations            a new file, a rename and a delete reach the host
  R  the Remote menu            the host's segment opens it above itself; the verbs are
                                live; the arrows walk them; Enter runs Show Status
  P  remote commands            status, the server log, the ssh command copied, a port
                                forwarded and unforwarded, host changes listed and
                                cleared, the server stopped and reconnected
  T  host terminals restore     (remote-terminal-restore) checked on the HOST through
                                `microide-server status` and pgrep, not only in the app:
     T1 three shells -- a loop, two with output, one split -- survive a quit; the
        relaunch reattaches the same handles, the loop still printing
     T13 closing a running tab with X ends its process
     T2 the rest closed with X, then quit: nothing left on the host
     T3 shells ended with `exit`, tabs left open, quit: nothing left; they revive
        as fresh shells
     T5 a command that finishes while microide is closed shows its last output
        on reopen, and Enter starts a new shell
     T6 a crash (SIGKILL) leaves orphans the connect's row offers; Attach opens
        one with its screen, Close ends the other; T12 Show Status lists them
     T7 terminal.restore_sessions off: quit ends the shell
     T8 close-project keeps the shell; reopening the project reattaches it
     T4 the host server restarted between quit and relaunch: fresh shells, and
        the row says why; T9 a `+` while connecting is a pending tab at once
     T10 3000 lines survive a quit: the tail is there, nothing duplicated
  V  the Host view              (remote-host-activity) Remote > Show Status opens it;
                                the project's terminal is listed (with its pid on
                                the host); `yes` in it raises its CPU figure; Delete
                                + confirm ends it (pgrep empty, row gone); hidden,
                                the host keeps no activity subscriber

Without MQA_REMOTE_HOST the suite prints why it did not run and exits 0: it is
on-demand against a real host. `tools/qa/local-sshd.sh` stands up a loopback-only
sshd as the current user (no install, no system service) and prints the variables
that point this suite at it; `tools/run-checks.sh qa-remote` does that end to end. Any FAIL or oracle finding exits 1. Findings and
screenshots land in --out (default /tmp/mqa-remote-<time>).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import microide_qa as q  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
# An ssh client config for the host (tools/qa/local-sshd.sh writes one for a
# loopback-only sshd); the suite's own ssh/scp and the app's ssh both read it.
SSH_CONFIG = os.environ.get("MQA_SSH_CONFIG")
# Where the suite's own checks reach the host, when not the way the editor does:
# against `qa-slow` (the loopback host behind a delayed link) the checks go over
# `qa-local`, the same host undelayed, so a measured latency is the editor's and
# not a fresh ssh handshake's six round trips on the slow link.
CHECK_HOST = os.environ.get("MQA_REMOTE_CHECK_HOST")


def ssh_cmd(*args: str) -> list[str]:
    return ["ssh", "-o", "BatchMode=yes", *(["-F", SSH_CONFIG] if SSH_CONFIG else []), *args]
EDIT_FILE = "LICENSE"  # the agents never touch it (not a source extension)


class Host:
    def __init__(self, name: str, root: str, base: str | None):
        self.name, self.root = name, root
        self.base = base or self.ssh(f"git -C {root} rev-parse HEAD").strip()
        self.agents = ""

    def ssh(self, cmd: str, timeout: float = 120) -> str:
        return subprocess.run(ssh_cmd(CHECK_HOST or self.name, cmd), capture_output=True,
                              text=True, timeout=timeout).stdout

    def file(self, rel: str) -> str:
        return self.ssh(f"cat {self.root}/{rel}")

    def tree_hashes(self) -> dict[str, str]:
        out = self.ssh(f"cd {self.root} && git ls-files -z --cached --others --exclude-standard | "
                       "sort -z | xargs -0 sha256sum")
        return {l.split("  ", 1)[1]: l.split("  ", 1)[0] for l in out.splitlines() if "  " in l}

    def reset(self) -> str:
        return self.ssh(f"cd {self.root} && git checkout -q -- . && git reset -q --hard {self.base} && "
                        "git clean -fdxq && git log --oneline -1").strip()

    def install_agents(self) -> None:
        self.agents = self.ssh("mktemp -d /tmp/mqa-agents.XXXXXX").strip() + "/agents.py"
        # Over the ssh connection itself, not scp: scp speaks SFTP, which a minimal
        # sshd (tools/qa/local-sshd.sh) does not serve.
        subprocess.run(ssh_cmd(CHECK_HOST or self.name, f"cat > {self.agents}"), check=True,
                       input=Path(__file__).with_name("agents.py").read_text(), text=True)


def mirror_hashes(mirror: Path) -> dict[str, str]:
    return {p.relative_to(mirror).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in mirror.rglob("*") if p.is_file() and ".git/" not in p.as_posix()}


def wait_for(pred, timeout: float, every: float = 0.25):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        v = pred()
        if v:
            return v, time.monotonic() - t0
        time.sleep(every)
    return None, time.monotonic() - t0


class Suite:
    def __init__(self, app: q.App, host: Host, only: set[str]):
        self.app, self.host, self.only = app, host, only
        self.results: list[tuple[str, str, str]] = []
        self.token = time.strftime("%H%M%S")
        self.mirror: Path | None = None

    # ---- helpers ---------------------------------------------------------
    def record(self, name: str, ok: bool, detail: str = "") -> None:
        self.results.append((name, "PASS" if ok else "FAIL", detail))
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
        if not ok:
            self.app.expect(False, name + ": " + detail)

    def info(self, name: str, detail: str) -> None:
        self.results.append((name, "INFO", detail))
        print(f"[INFO] {name}: {detail}", flush=True)

    def runs(self, name: str) -> bool:
        return not self.only or name in self.only

    def notifs(self):
        return self.app.query("notifications")

    def remote(self) -> dict:
        # A status query can time out while the shell thread is busy with the
        # connection itself (the server install, the first sync); one timed-out
        # poll is "not ready yet", not a reason for the suite to die.
        try:
            return self.app.query("status").get("remote") or {}
        except q.QaFailure as failure:
            print(f"[INFO] status query failed: {failure}", flush=True)
            return {}

    def wait_ready(self, timeout: float = 180):
        seen: list[str] = []

        def pred():
            state = self.remote().get("state")
            if not seen or seen[-1] != state:
                seen.append(state)
            return state if state == "ready" else None
        got, dt = wait_for(pred, timeout, 0.5)
        return (f"ready via {seen}" if got else f"timeout; states {seen}"), dt, bool(got)

    def active_tab(self):
        for group in self.app.query("editor")["groups"]:
            if group["focused"]:
                for tab in group["tabs"]:
                    if tab["active"]:
                        return tab
        return None

    def host_has(self, needle: str, timeout: float = 15):
        return wait_for(lambda: needle in self.host.file(EDIT_FILE), timeout, 0.2)

    def mirror_has(self, rel: str, needle: str, timeout: float = 15):
        path = self.mirror / rel
        return wait_for(lambda: path.exists() and needle in path.read_text(errors="replace"), timeout, 0.2)

    def type_line(self, text: str) -> None:
        self.app.send("goto", "1")
        self.app.send("type", text + "\n")

    def trees_equal(self):
        host, mirror = self.host.tree_hashes(), mirror_hashes(self.mirror)
        diff = {k for k in set(host) | set(mirror) if host.get(k) != mirror.get(k)}
        return host, mirror, diff

    # ---- group T: host terminals across quit, crash and close-project ----------
    def host_terminals(self) -> list[dict]:
        """The host server's terminals under this project's root, from the host's
        own `microide-server status --json` (the server/status reply; the bare
        command prints a table)."""
        out = self.host.ssh('"$HOME/.local/share/microide/server/microide-server" status --json')
        try:
            rows = json.loads(out).get("terminals")
        except ValueError:
            return []
        return [r for r in rows if r.get("root") == self.host.root] if isinstance(rows, list) else []

    def host_pgrep(self, marker: str) -> str:
        # `[Q]A-...`: the pattern must not match the ssh command line carrying it
        # (the `sh -c` running this pgrep holds the marker in its own argv).
        return self.host.ssh(f"pgrep -f '[{marker[0]}]{marker[1:]}' || true").strip()

    def panes(self) -> list[dict]:
        return [dict(p, tab=t["tab"]) for t in self.app.query("terminals") for p in t["panes"]]

    def term_text(self, tab: int, pane: int = 0) -> str:
        return self.app.query("terminal-output", {"tab": tab, "pane": pane, "lines": 20000}).get("text", "")

    def close_all_terminals(self) -> None:
        for _ in range(24):
            if not self.app.query("terminals"):
                return
            self.app.send("term-close"); self.app.settle(0.2)

    def relaunch(self, crash: bool = False, between=None):
        self.app.screenshot("T-before-relaunch")
        if crash:
            self.app.crash_and_relaunch(self.mirror)
        else:
            self.app.restart(self.mirror, between=between)
        return self.wait_ready(180)

    def all_started(self, timeout: float = 60):
        """Every terminal pane past pending, i.e. started or reattached."""
        return wait_for(lambda: (lambda ps: ps if ps and not any(p["pending"] for p in ps) else None)(
            self.panes()), timeout, 0.3)

    def new_terminal(self, line: str | None = None) -> None:
        self.app.send("term"); self.app.settle(0.5)
        if line is not None:
            self.app.send("type", line); self.app.send("key", "enter")

    def run_terminal_restore(self) -> None:
        app, tok = self.app, self.token
        loop = f"QA-T1-{tok}"
        self.close_all_terminals()
        # A clean slate on the host: terminals an earlier run left with nobody
        # attached (its app was killed, the crash path) are not this group's.
        leftover = [str(r["handle"]) for r in self.host_terminals() if r.get("clients") == 0]
        if leftover:
            app.send("remote-close-terminals", self.host.name, *leftover)
            wait_for(lambda: not [r for r in self.host_terminals() if r.get("clients") == 0], 10, 0.3)
            self.info("T.leftover-closed", ",".join(leftover))
        # T1: a loop, two tabs with output, the third split in two.
        self.new_terminal(f"while :; do echo {loop}-$(date +%s); sleep 1; done")
        self.new_terminal(f"echo QA-T1-LS-{tok}-$((20+22))")
        self.new_terminal(f"echo QA-T1-LH-{tok}-$((30+3))")
        app.send("term-split"); app.settle(0.5)
        app.send("type", f"echo QA-T1-SPLIT-{tok}"); app.send("key", "enter")
        before, dt = wait_for(lambda: (lambda ps: ps if len(ps) == 4 and all(p["hostHandle"] for p in ps)
                                       else None)(self.panes()), 30, 0.3)
        handles = sorted(p["hostHandle"] for p in before or [])
        listed = sorted(r["handle"] for r in self.host_terminals())
        self.record("T1.four-host-terminals", len(handles) == 4 and listed == handles,
                    f"panes={handles} host={listed} in {dt:.1f}s")
        wait_for(lambda: f"QA-T1-LS-{tok}-42" in self.term_text(1), 15, 0.3)
        quit_at = int(time.time())
        app.screenshot("T1-before-quit")
        msg, dt, ok = self.relaunch()
        self.record("T1.relaunch-ready", ok, f"{msg} {dt:.1f}s")
        after, dt = self.all_started()
        self.record("T1.same-layout",
                    after is not None and len({p["tab"] for p in after}) == 3 and len(after) == 4 and
                    sorted(p["hostHandle"] for p in after) == handles,
                    f"{dt:.1f}s panes={[(p['tab'], p['pane'], p['hostHandle'], p['pending']) for p in after or []]}")

        def loop_after_quit():
            stamps = [int(x) for x in __import__("re").findall(loop + r"-(\d+)", self.term_text(0))]
            return stamps and max(stamps) > quit_at
        ok, dt = wait_for(loop_after_quit, 20, 0.5)
        self.record("T1.loop-still-running", bool(ok), f"a stamp newer than the quit after {dt:.1f}s")
        text = self.term_text(1)
        self.record("T1.output-kept", f"QA-T1-LS-{tok}-42" in text and "reattached" in text,
                    f"tail={text[-300:]!r}")
        self.record("T1.split-kept", f"QA-T1-SPLIT-{tok}" in self.term_text(2, 0) + self.term_text(2, 1),
                    "the split's output is in one of its panes")
        row, _ = wait_for(lambda: next((n for n in self.notifs() if "Reattached to" in n["message"]), None), 15, 0.3)
        self.info("T1.row", row["message"] if row else f"{[n['message'] for n in self.notifs()]}")
        app.screenshot("T1-reattached"); app.check("T1")

        # T13: closing a running tab with X ends its process on the host.
        active = next((t for t in app.query("terminals") if t["active"]), None)
        closing = active["panes"][0]["hostHandle"] if active and len(active["panes"]) == 1 else 0
        if not closing:  # the split tab: step to a single-pane one
            app.send("term-next"); app.settle(0.3)
            active = next((t for t in app.query("terminals") if t["active"]), None)
            closing = active["panes"][0]["hostHandle"] if active else 0
        app.send("term-close")
        gone, dt = wait_for(lambda: closing not in [r["handle"] for r in self.host_terminals()], 10, 0.3)
        self.record("T13.close-ends-process", bool(gone), f"handle {closing} gone after {dt:.1f}s")

        # T2: everything closed with X, then quit: nothing is left.
        self.close_all_terminals()
        gone, dt = wait_for(lambda: not self.host_terminals() and not self.host_pgrep(loop), 10, 0.3)
        self.record("T2.nothing-left", bool(gone),
                    f"{dt:.1f}s host={self.host_terminals()} pgrep={self.host_pgrep(loop)!r}")
        msg, dt, ok = self.relaunch()
        # The relaunch opens the project's default terminal (attached); nothing of
        # the closed ones may be left running with nobody attached.
        self.record("T2.still-nothing-after-quit",
                    ok and not [r for r in self.host_terminals() if r.get("clients") == 0],
                    f"{self.host_terminals()}")

        # T3: shells ended with `exit`, tabs left open, quit: closed, not kept.
        self.close_all_terminals()
        for _ in range(2):
            self.new_terminal("exit")
        ended, _ = wait_for(lambda: (lambda ps: ps if len(ps) == 2 and not any(p["running"] for p in ps)
                                     else None)(self.panes()), 20, 0.3)
        self.record("T3.ended", ended is not None, f"{self.panes()}")
        msg, dt, ok = self.relaunch()
        started, _ = self.all_started()
        self.record("T3.nothing-kept-fresh-shells",
                    ok and not [r for r in self.host_terminals() if r.get("exited")] and started is not None and
                    len(started) == 2 and all(p["running"] for p in started),
                    f"host={self.host_terminals()} panes={started}")

        # T5: a command that finishes while microide is closed.
        self.close_all_terminals()
        self.new_terminal(f"sleep 3; echo QA-T5-DONE-{tok}; exit")
        wait_for(lambda: any(p["hostHandle"] for p in self.panes()), 10, 0.2)
        msg, dt, ok = self.relaunch(between=lambda: time.sleep(5))  # it finishes while closed
        done, dt = wait_for(lambda: f"QA-T5-DONE-{tok}" in self.term_text(0), 30, 0.3)
        panes = self.panes()
        self.record("T5.finished-output-shown", bool(done) and panes and not panes[0]["running"],
                    f"{dt:.1f}s panes={panes}")
        app.send("focus", "panel"); app.send("key", "enter")
        relaunched, dt = wait_for(lambda: (lambda ps: ps if ps and ps[0]["running"] else None)(self.panes()), 20, 0.3)
        self.record("T5.enter-relaunches", relaunched is not None, f"{dt:.1f}s")

        # T6 / T12: a crash leaves two orphans; the connect offers them.
        self.close_all_terminals()
        orphan = f"QA-T6-{tok}"
        self.new_terminal(f"while :; do echo {orphan}-A-$(date +%s); sleep 1; done")
        self.new_terminal(f"while :; do echo {orphan}-B-$(date +%s); sleep 1; done")
        wait_for(lambda: len([p for p in self.panes() if p["hostHandle"]]) == 2, 15, 0.3)
        # The session last written (T5's relaunch) names neither: a crash records nothing.
        msg, dt, ok = self.relaunch(crash=True)
        row, dt = wait_for(lambda: next((n for n in self.notifs() if "running with no tab" in n["message"]), None),
                           30, 0.3)
        self.record("T6.orphans-offered", row is not None and "2 running with no tab" in row["message"],
                    f"{dt:.1f}s {row['message'] if row else [n['message'] for n in self.notifs()]}")
        status = app.send("remote-status")
        # Show Status opens the Host view; its text answer is the control reply's.
        shown = status.get("feedback", "")
        self.record("T12.show-status-lists-them", bool(status.get("ok")) and "with no tab" in shown,
                    f"{shown[-300:]!r}")
        app.send("sidebar-show", "tree")
        orphans = sorted(r["handle"] for r in self.host_terminals() if r.get("clients") == 0)
        if len(orphans) == 2:
            tabs = len(app.query("terminals"))
            app.send("remote-attach-terminals", self.host.name, str(orphans[0]))
            attached, dt = wait_for(lambda: (lambda ts: ts if len(ts) == tabs + 1 else None)(
                app.query("terminals")), 10, 0.3)
            shows, _ = wait_for(lambda: f"{orphan}-" in self.term_text(tabs), 15, 0.3)
            self.record("T6.attach-opens-it", attached is not None and bool(shows), f"{dt:.1f}s")
            app.send("remote-close-terminals", self.host.name, str(orphans[1]))
            gone, dt = wait_for(lambda: orphans[1] not in [r["handle"] for r in self.host_terminals()], 10, 0.3)
            self.record("T6.close-ends-it", bool(gone), f"{dt:.1f}s")
        else:
            self.record("T6.attach-opens-it", False, f"host orphans={orphans}")
        app.screenshot("T6-attached"); app.check("T6")

        # T7: opted out of revival, quit ends the shell.
        self.close_all_terminals()
        app.send("set-setting", "terminal.restore_sessions", "false")
        marker = f"QA-T7-{tok}"
        self.new_terminal(f"while :; do echo {marker}; sleep 1; done")
        wait_for(lambda: self.host_pgrep(marker), 10, 0.3)
        msg, dt, ok = self.relaunch()
        gone, dt = wait_for(lambda: not self.host_pgrep(marker), 10, 0.3)
        self.record("T7.opted-out-quit-ends-it", bool(gone), f"pgrep={self.host_pgrep(marker)!r}")
        app.send("set-setting", "terminal.restore_sessions", "true")

        # T8: close the project (not the app): the shell stays; reopening reattaches.
        self.close_all_terminals()
        kept = f"QA-T8-{tok}"
        self.new_terminal(f"while :; do echo {kept}-$(date +%s); sleep 1; done")
        wait_for(lambda: any(p["hostHandle"] for p in self.panes()), 10, 0.3)
        handle = next((p["hostHandle"] for p in self.panes() if p["hostHandle"]), 0)
        app.send("project-close"); app.settle(1.0)
        row, dt = wait_for(lambda: next((r for r in self.host_terminals() if r["handle"] == handle
                                         and r["clients"] == 0), None), 10, 0.3)
        self.record("T8.close-project-keeps-it", row is not None, f"{dt:.1f}s host={self.host_terminals()}")
        app.send("remote-open", f"{self.host.name}:{self.host.root}")
        self.wait_ready(120)
        back, dt = wait_for(lambda: (lambda ps: ps if any(p["hostHandle"] == handle and p["running"] for p in ps)
                                     else None)(self.panes()), 30, 0.3)
        self.record("T8.reopen-reattaches", back is not None, f"{dt:.1f}s panes={self.panes()}")

        # T10: deep output survives a quit, nothing duplicated across the prefetch.
        self.close_all_terminals()
        self.new_terminal(f"seq 1 3000 | sed 's/^/QA-T10-{tok}-/'")
        wait_for(lambda: f"QA-T10-{tok}-3000" in self.term_text(0), 20, 0.3)
        msg, dt, ok = self.relaunch()
        self.all_started()
        tail, dt = wait_for(lambda: f"QA-T10-{tok}-3000" in self.term_text(0), 30, 0.3)
        import re as _re
        numbers = [int(n) for n in _re.findall(rf"QA-T10-{tok}-(\d+)", self.term_text(0))]
        self.record("T10.tail-and-no-duplicates", bool(tail) and len(numbers) == len(set(numbers)),
                    f"{len(numbers)} rows held, last {numbers[-1] if numbers else None}")

        # T4 / T9: the host server restarted between quit and relaunch.
        self.app.restart(self.mirror, between=lambda: self.host.ssh(
            '"$HOME/.local/share/microide/server/microide-server" stop'))
        app.send("term")
        pending = [p for p in self.panes() if p["pending"]]
        self.info("T9.plus-while-connecting", f"pending panes at once: {len(pending)} of {len(self.panes())}")
        msg, dt, ok = self.wait_ready(180)
        row, dt = wait_for(lambda: next((n for n in self.notifs() if "started fresh" in n["message"]), None), 30, 0.3)
        self.record("T4.server-restart-starts-fresh-and-says-so",
                    row is not None and "host server restarted" in row["message"],
                    f"{dt:.1f}s {row['message'] if row else [n['message'] for n in self.notifs()]}")
        started, _ = self.all_started()
        self.record("T9.pending-tabs-start", started is not None and all(p["running"] for p in started),
                    f"{started}")
        self.close_all_terminals()
        app.check("T")

    # ---- group V: the Host view (remote-host-activity) -------------------
    def host_status(self) -> dict:
        out = self.host.ssh('"$HOME/.local/share/microide/server/microide-server" status --json')
        try:
            return json.loads(out)
        except ValueError:
            return {}

    def host_rows(self) -> list[dict]:
        return self.app.elements("row", prefix="row:host/")

    def run_host_view(self) -> None:
        app, tok = self.app, self.token
        marker = f"QA-V-{tok}"
        # Through the menu, as a user gets there: Remote > Show Status.
        app.click_named("menu:Remote"); app.settle(0.3)
        app.key("Down"); app.settle(0.1); app.key("Return"); app.settle(0.5)
        view = app.ui()["sidebar"]
        self.record("V.menu-opens-view", view.get("view") == "host" and view.get("visible"), f"sidebar={view}")
        self.close_all_terminals()
        self.new_terminal(f"echo {marker}-ready")
        panes, dt = wait_for(lambda: (lambda ps: ps if ps and ps[0]["hostHandle"] else None)(self.panes()), 30, 0.3)
        handle = panes[0]["hostHandle"] if panes else None
        app.send("remote-status")  # the view again: the new terminal took the keyboard
        name = f"row:host/#{handle}"
        row, dt = wait_for(lambda: next((r for r in self.host_rows() if r["name"].startswith(name)), None), 10, 0.3)
        listed = next((t for t in self.host_status().get("terminals", []) if t.get("handle") == handle), {})
        self.record("V.terminal-listed", row is not None and int(listed.get("pid", 0)) > 0,
                    f"{dt:.1f}s row={row} host={listed}")
        app.send("focus", "panel"); app.settle(0.2)
        app.send("type", f"yes {marker} > /dev/null"); app.send("key", "enter")
        app.send("remote-status")

        def busy():
            r = next((r for r in self.host_rows() if r["name"].startswith(name)), None)
            label = (r or {}).get("label", "")
            words = [w for w in label.replace("\u00b7", " ").split() if w.endswith("%")]
            try:
                return r if words and float(words[0][:-1]) > 10 else None
            except ValueError:
                return None
        row, dt = wait_for(busy, 15, 0.5)
        self.record("V.cpu-rises", row is not None, f"{dt:.1f}s row={row}")
        app.screenshot("V-busy")
        # Delete on it: walk the selection to the row, Delete, confirm.
        app.send("focus", "sidebar"); app.settle(0.2)
        for _ in range(20):
            if any(r.get("selected") and r["name"].startswith(name) for r in self.host_rows()):
                break
            app.key("Down"); app.settle(0.1)
        app.key("Delete"); app.settle(0.3)
        prompt = app.ui().get("prompt") or {}
        app.check("V.confirm")
        app.key("Return"); app.settle(0.5)
        gone, dt = wait_for(lambda: not self.host_pgrep(marker), 15, 0.5)
        row_gone, _ = wait_for(lambda: not any(r["name"].startswith(name) for r in self.host_rows()), 10, 0.3)
        self.record("V.delete-ends-it", bool(gone) and bool(row_gone),
                    f"{dt:.1f}s prompt={prompt} pgrep={self.host_pgrep(marker)!r}")
        # Hidden: no subscriber on the host, for longer than the push interval.
        app.send("sidebar-show", "tree"); app.settle(0.3)
        quiet, dt = wait_for(lambda: self.host_status().get("server", {}).get("activity_subscribers") == 0, 10, 0.5)
        time.sleep(5)
        still = self.host_status().get("server", {}).get("activity_subscribers")
        self.record("V.hidden-unsubscribes", bool(quiet) and still == 0, f"{dt:.1f}s then {still}")
        self.close_all_terminals()
        app.check("V")

    # ---- scenarios -------------------------------------------------------
    def run_remote_menu(self) -> None:
        """R: the host's status segment opens the Remote menu above itself, the
        same menu the bar opens; with the host connected its verbs are live, the
        arrows walk them and Enter on Show Status runs remote-status."""
        app = self.app
        segment = next((e for e in app.elements("status") if e["name"] == "status:remote"), None)
        self.record("R.segment-opens-menu", bool(segment and segment.get("enabled") and segment.get("menu") == "Remote"),
                    f"segment={segment}")
        if segment is None:
            return
        app.click_named("status:remote"); app.settle(0.3); app.check("R.click-segment")
        ui = app.ui()
        rows = app.elements("menu-item", prefix="menu-item:Remote/")
        first = rows[0]["rect"] if rows else None
        self.record("R.popup-above-segment",
                    ui["menu"]["open"] and ui["menu"]["menu"] == "Remote" and first is not None
                    and first[1] + first[3] <= segment["rect"][1],
                    f"menu={ui['menu']} first-row={first} segment={segment['rect']}")
        live = {r["label"] for r in rows if r.get("enabled")}
        want = {"Open Folder on Host…", "Show Status", "Show Changed on Host", "Show Host Server Log",
                "Forward a Port…", "Reconnect", "Disconnect", "Stop Host Server"}
        self.record("R.verbs-live", want <= live, f"live={sorted(live)}")
        app.key("Escape"); app.settle(0.2)
        self.record("R.escape-closes", not app.ui()["menu"]["open"], f"menu={app.ui()['menu']}")
        # The bar's Remote menu is the same one: walk to Show Status and run it.
        app.click_named("menu:Remote"); app.settle(0.3)
        before = app.ui().get("actions", {}).get("dispatched", 0)
        app.key("Down"); app.settle(0.1)
        active = next((r["label"] for r in app.elements("menu-item", prefix="menu-item:Remote/") if r.get("active")), None)
        self.record("R.arrow-walks", active == "Show Status", f"active={active}")
        app.key("Return"); app.settle(0.4); app.check("R.run-show-status")
        actions = app.ui().get("actions", {})
        self.record("R.enter-runs-row", actions.get("last") == "remote-status" and actions.get("dispatched", 0) > before,
                    f"actions={actions}")
        app.key("Escape"); app.settle(0.2)
        app.check("R")

    def run_all(self) -> None:
        app, host, tok = self.app, self.host, self.token
        app.send("remote-open", f"{host.name}:{host.root}")
        msg, dt, ok = self.wait_ready()
        for project in app.query("projects"):
            if project.get("host") == host.name:
                self.mirror = Path(project["root"])
        self.record("A.open-sync", ok and self.mirror is not None and self.mirror.exists(),
                    f"{msg} in {dt:.1f}s; mirror={self.mirror}")
        if self.mirror is None:
            return
        _, _, diff = self.trees_equal()
        self.record("A.tree-equal", not diff, f"differing: {sorted(diff)[:10]}")
        app.screenshot("A-ready"); app.check("A")

        if self.runs("R"):
            self.run_remote_menu()

        if self.runs("B"):
            app.send("open", EDIT_FILE); app.settle(0.5)
            tab = self.active_tab()
            self.record("B.open-tab", bool(tab and tab["path"].endswith(EDIT_FILE)), f"tab={tab}")
            self.type_line(f"QA-SAVE-{tok}"); reply = app.send("save")
            ok, dt = self.host_has(f"QA-SAVE-{tok}")
            self.record("B.save-reaches-host", bool(ok), f"save={reply}; on the host after {dt:.2f}s")
            tab = self.active_tab()
            self.record("B.tab-clean-after-save", bool(tab and not tab["dirty"]), f"tab={tab}")
            app.check("B")

        if self.runs("C"):
            host.ssh(f"cd {host.root} && sed -i '1i HOST-EDIT-{tok}' {EDIT_FILE}")
            ok, dt = self.mirror_has(EDIT_FILE, f"HOST-EDIT-{tok}")
            self.record("C.host-edit-reaches-mirror", bool(ok), f"{dt:.2f}s")
            app.settle(1.0)
            self.type_line(f"QA-AFTER-HOST-{tok}"); app.send("save")
            ok, dt = self.host_has(f"QA-AFTER-HOST-{tok}")
            content = host.file(EDIT_FILE)
            self.record("C.buffer-reloaded-then-saved",
                        bool(ok) and f"HOST-EDIT-{tok}" in content and f"QA-SAVE-{tok}" in content,
                        f"host head {content.splitlines()[:3]} after {dt:.2f}s; "
                        f"notifs={[n['message'] for n in self.notifs()]}")
            app.check("C")

        if self.runs("D"):
            self.type_line(f"QA-DIRTY-{tok}")
            host.ssh(f"cd {host.root} && sed -i '1i HOST-CONFLICT-{tok}' {EDIT_FILE}")
            ok, dt = self.mirror_has(EDIT_FILE, f"HOST-CONFLICT-{tok}")
            self.record("D.host-edit-pulled-under-dirty-tab", bool(ok), f"{dt:.2f}s")
            app.settle(0.5)
            reply = app.send("save")
            self.record("D.save-refused-with-banner", not reply.get("ok") and "banner" in reply.get("error", ""),
                        f"save={reply}")
            app.screenshot("D-banner")
            reply = app.send("banner-action", "Compare"); app.settle(0.8)
            tabs = app.query("tabs")
            self.record("D.compare-opens-a-compare-tab",
                        reply.get("ok") and any(t.get("kind") == "compare" for t in tabs),
                        f"{reply} tabs={[(t.get('kind'), t.get('title')) for t in tabs]}")
            app.screenshot("D-compare")
            app.send("open", EDIT_FILE); app.settle(0.3)
            reply = app.send("banner-action", "Overwrite")
            ok, dt = self.host_has(f"QA-DIRTY-{tok}")
            content = host.file(EDIT_FILE)
            self.record("D.overwrite-pushes-mine",
                        reply.get("ok") and bool(ok) and f"HOST-CONFLICT-{tok}" not in content,
                        f"{reply} after {dt:.2f}s; head={content.splitlines()[:2]}")
            tab = self.active_tab()
            self.record("D.tab-clean-after-overwrite", bool(tab and not tab["dirty"]), f"tab={tab}")
            app.check("D")

        if self.runs("D2"):
            self.type_line(f"QA-DIRTY2-{tok}")
            host.ssh(f"cd {host.root} && sed -i '1i HOST-CONFLICT2-{tok}' {EDIT_FILE}")
            self.mirror_has(EDIT_FILE, f"HOST-CONFLICT2-{tok}"); app.settle(0.5)
            app.send("save")
            reply = app.send("banner-action", "Reload"); app.settle(0.8)
            text = (self.mirror / EDIT_FILE).read_text(errors="replace"); tab = self.active_tab()
            self.record("D2.reload-takes-host",
                        reply.get("ok") and f"HOST-CONFLICT2-{tok}" in text and f"QA-DIRTY2-{tok}" not in text
                        and bool(tab and not tab["dirty"]),
                        f"reload={reply} mirror head={text.splitlines()[:2]} tab={tab}")
            self.type_line(f"QA-POSTRELOAD-{tok}"); app.send("save")
            ok, dt = self.host_has(f"QA-POSTRELOAD-{tok}")
            self.record("D2.save-after-reload", bool(ok), f"{dt:.2f}s")
            app.check("D2")

        if self.runs("E"):
            app.send("remote-disconnect", host.name); app.settle(1.0)
            self.type_line(f"QA-OFFLINE-{tok}"); reply = app.send("save"); app.settle(1.0)
            tab = self.active_tab(); state = self.remote()
            self.record("E.offline-save-local",
                        bool(tab and not tab["dirty"]) and
                        f"QA-OFFLINE-{tok}" in (self.mirror / EDIT_FILE).read_text(errors="replace"),
                        f"save={reply} tab={tab}")
            self.record("E.unsent-push-reported",
                        state.get("unsentPushes") == 1 and state.get("state") in ("disconnected", "offline"),
                        f"remote={state}")
            reply = app.send("remote-reconnect", host.name)
            ok, dt = self.host_has(f"QA-OFFLINE-{tok}", 60)
            self.record("E.journal-flushed-on-reconnect", bool(ok), f"{reply} after {dt:.2f}s")
            msg, dt, ok = self.wait_ready(60)
            self.record("E.ready-again", ok, f"{msg} {dt:.1f}s")
            app.check("E")

        if self.runs("F"):
            out = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True).stdout
            masters = [int(l.split()[0]) for l in out.splitlines()[1:]
                       if "ssh" in l and host.name in l and "ControlMaster" in l]
            self.info("F.ssh-masters", str(masters))
            for pid in masters:
                os.kill(pid, 9)
            row, dt = wait_for(lambda: next((n for n in self.notifs()
                                            if any(a["label"] == "Reconnect" for a in n["actions"])), None), 30, 0.3)
            self.record("F.reconnecting-row", row is not None,
                        f"{dt:.2f}s {json.dumps(row)[:200] if row else [n['message'] for n in self.notifs()]}")
            self.type_line(f"QA-AFTER-DROP-{tok}"); app.send("save")
            ok, dt = self.host_has(f"QA-AFTER-DROP-{tok}", 90)
            self.record("F.save-after-link-death", bool(ok), f"on the host {dt:.2f}s after the kill")
            app.check("F")

        if self.runs("G"):
            # Terminal > New in the remote project runs on its host. The host's own
            # name, not the ssh alias (they differ for a config alias).
            hostname = host.ssh("hostname").strip()
            reply = app.send("term"); app.settle(2.5)
            app.send("type", "echo QA-TERM-$(hostname)"); app.send("key", "enter")
            out, dt = wait_for(lambda: (lambda o: o if f"QA-TERM-{hostname}" in o.get("text", "") else None)(
                app.query("terminal-output")), 20, 0.5)
            self.record("G.host-terminal", out is not None, f"{reply} {dt:.2f}s")
            app.screenshot("G-terminal")
            # Remote: Open Local Terminal runs HERE. The loopback host is this machine,
            # so the hostname cannot tell them apart: the app's isolated config
            # directory can -- only a shell started by the app inherits it.
            marker = app.env["XDG_CONFIG_HOME"]
            reply = app.send("remote-local-terminal"); app.settle(1.5)
            app.send("type", "echo QA-LOCAL-$XDG_CONFIG_HOME"); app.send("key", "enter")
            out, dt = wait_for(lambda: (lambda o: o if f"QA-LOCAL-{marker}" in o.get("text", "") else None)(
                app.query("terminal-output")), 20, 0.5)
            self.record("G.local-terminal", bool(reply.get("ok")) and out is not None, f"{reply} {dt:.2f}s")
            app.screenshot("G-local-terminal"); app.check("G")

        if self.runs("H"):
            # The bundled cpp-lsp plugin registers clangd; the isolated config starts empty.
            plugins = Path(app.env["XDG_CONFIG_HOME"]) / "microide" / "plugins"
            plugins.mkdir(parents=True, exist_ok=True)
            shutil.copytree(REPO / "plugins" / "cpp-lsp", plugins / "cpp-lsp", dirs_exist_ok=True)
            app.settle(0.5)
            reply = app.send("plugins-reload")
            # The reload runs on the plugin worker; its summary arrives as a toast.
            toast, dt = wait_for(lambda: next((n["message"] for n in self.notifs()
                                               if "Loaded 1 plugin" in n["message"]), None), 10, 0.25)
            self.record("H.cpp-lsp-plugin-loaded", toast is not None,
                        f"{reply} toast={toast!r} after {dt:.1f}s")
            app.settle(1.0)
            host.ssh(f"mkdir -p {host.root}/qa && printf 'static int helper(int x) {{\\n  return x + 1;\\n}}\\n"
                     f"\\nint main(void) {{\\n  return helper(41);\\n}}\\n' > {host.root}/qa/qa_gd.c")
            ok, dt = self.mirror_has("qa/qa_gd.c", "helper(41)", 20)
            self.record("H.host-created-file-reaches-mirror", bool(ok), f"{dt:.2f}s")
            app.send("open", "qa/qa_gd.c"); app.settle(4.0)
            app.send("goto", "6:12"); reply = app.send("goto-definition")
            tab, dt = wait_for(lambda: (lambda t: t if t and t["cursorLine"] == 1 else None)(self.active_tab()), 30, 0.5)
            self.record("H.goto-definition-on-host", tab is not None,
                        f"gd={reply} {dt:.1f}s tab={tab or self.active_tab()}; "
                        f"host clangd processes={host.ssh('pgrep -c clangd').strip()}")
            app.screenshot("H-gotodef"); app.check("H")

        if self.runs("I"):
            app.send("sidebar-show", "git"); app.settle(1.0)
            host.ssh(f"cd {host.root} && git checkout -q -b qa-branch-{tok}"); app.settle(3.0)
            app.screenshot("I-git-branch"); app.send("git-refresh"); app.settle(2.0)
            host.ssh(f"cd {host.root} && git checkout -q - && git branch -q -D qa-branch-{tok}"); app.settle(2.0)
            self.record("I.branch-switch-survived", app.alive(), f"notifs={[n['message'] for n in self.notifs()]}")
            app.check("I")

        if self.runs("J"):
            if not host.agents:
                host.install_agents()
            app.send("open", EDIT_FILE); app.settle(0.5)
            app.send("split-right", "README.md"); app.settle(0.5)  # a second pane repaints too
            app.send("focus-group-left"); app.settle(0.2)
            churn = subprocess.Popen(ssh_cmd(CHECK_HOST or host.name, f"python3 {host.agents} churn {host.root} 4 25"),
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            saves, latencies = 0, []
            for i in range(8):
                self.type_line(f"QA-CHURN-{tok}-{i}"); app.send("save")
                ok, dt = self.host_has(f"QA-CHURN-{tok}-{i}", 20)
                latencies.append(round(dt, 2)); saves += bool(ok)
                app.send("sidebar-show", "files"); app.settle(0.3); app.send("sidebar-show", "search"); app.settle(0.3)
            out, _ = churn.communicate(timeout=120)
            self.info("J.churn", out.strip().splitlines()[-1] if out.strip() else "")
            self.record("J.saves-under-churn", saves == 8, f"{saves}/8 reached the host; latencies {latencies}")
            res, dt = wait_for(lambda: (lambda r: r if not r[2] else None)(self.trees_equal()), 30, 2.0)
            if res is None:
                _, _, diff = self.trees_equal()
                self.record("J.tree-equal-after-churn", False, f"{len(diff)} differ: {sorted(diff)[:15]}")
            else:
                self.record("J.tree-equal-after-churn", True, f"consistent {dt:.1f}s after the agents stopped")
            tab = self.active_tab()
            self.record("J.tab-clean", bool(tab and not tab["dirty"]), f"tab={tab}")
            app.screenshot("J-after-churn"); app.send("focus-group-right"); app.send("close-group"); app.check("J")

        if self.runs("K"):
            # The git view against the host's repository: the saved edit is a
            # change there, `s` stages it on the host, the commit workflow commits it.
            app.send("open", EDIT_FILE); app.settle(0.3)
            self.type_line(f"QA-GIT-{tok}"); app.send("save")
            self.host_has(f"QA-GIT-{tok}")
            app.send("sidebar-show", "git"); app.send("git-refresh"); app.settle(1.5)
            row, dt = wait_for(lambda: next((e for e in app.elements("row", prefix="row:git/")
                                            if e["name"].endswith("/" + EDIT_FILE)), None), 20, 0.5)
            self.record("K.change-listed", row is not None, f"{dt:.1f}s rows={[e['name'] for e in app.elements('row', prefix='row:git/')][:6]}")
            if row is not None:
                # Stage from the row's menu: bare letters in the list navigate by
                # name now (TD-2026-10-16-350), so `s` no longer stages.
                app.click(*app.center(row), button=3); app.settle(0.3)
                stage = app.wait_for("menu-item:context/Stage", 5.0)
                app.click(*app.center(stage))
                status, dt = wait_for(lambda: (lambda o: o if o.startswith("M ") else None)(
                    host.ssh(f"cd {host.root} && git status --porcelain -- {EDIT_FILE}")), 20, 0.5)
                self.record("K.stage-on-host", status is not None, f"{dt:.1f}s porcelain={status!r}")
                # Commit is enabled once the git view has the staged state back,
                # a round trip later over a slow link.
                wait_for(lambda: app.element("button:git/commit").get("enabled", True) or None, 20, 0.3)
                app.click_named("button:git/commit")
                confirm = app.wait_for("button:git/confirm-commit", 5.0)
                app.send("type", f"qa remote commit {tok}")
                if confirm is not None:
                    app.click(*app.center(confirm))
                head, dt = wait_for(lambda: (lambda o: o if f"qa remote commit {tok}" in o else None)(
                    host.ssh(f"cd {host.root} && git log --oneline -1")), 30, 0.5)
                self.record("K.commit-on-host", head is not None, f"{dt:.1f}s head={head!r}")
            app.check("K")
        if self.runs("M"):
            # Project search over the mirror finds a file the host just created.
            host.ssh(f"mkdir -p {host.root}/qa && printf 'needle-{tok} here\\n' > {host.root}/qa/needle.txt")
            self.mirror_has("qa/needle.txt", f"needle-{tok}", 20)
            app.send("project-search", f"needle-{tok}")
            rows, dt = wait_for(lambda: [e["name"] for e in app.elements("row", prefix="row:search/")] or None, 20, 0.5)
            self.record("M.search-finds-host-file", bool(rows) and any("needle.txt" in r for r in rows), f"{dt:.1f}s rows={rows}")
            app.send("key", "escape"); app.check("M")
        if self.runs("N"):
            # Tree operations on the mirror reach the host: a new file, a rename, a delete.
            app.send("tab", f"qa/made-{tok}.txt"); app.send("type", "made here\n"); app.send("save")
            made, dt = wait_for(lambda: "made here" in host.file(f"qa/made-{tok}.txt"), 20, 0.5)
            self.record("N.new-file-reaches-host", bool(made), f"{dt:.1f}s")
            # Opening the file makes it the tree's selection (the tree follows the
            # active file); a buffer saved for the first time is not revealed by itself.
            app.send("open", f"qa/made-{tok}.txt"); app.settle(0.5)
            app.send("sidebar-show", "tree"); app.send("focus", "sidebar"); app.settle(0.5)
            selected = [e["name"] for e in app.elements("row", prefix="row:tree/") if e.get("selected")]
            self.record("N.tree-selects-the-file", selected == [f"row:tree/made-{tok}.txt"], f"selected={selected}")
            app.send("key", "F2"); app.settle(0.4)
            app.send("key", "ctrl+a"); app.send("type", f"renamed-{tok}.txt"); app.send("key", "enter")
            renamed, dt = wait_for(lambda: "made here" in host.file(f"qa/renamed-{tok}.txt"), 20, 0.5)
            self.record("N.rename-reaches-host", bool(renamed), f"{dt:.1f}s ls={host.ssh(f'ls {host.root}/qa')!r}")
            app.send("focus", "sidebar"); app.settle(0.3)
            app.send("key", "Delete"); app.settle(0.4); app.send("key", "enter")
            gone, dt = wait_for(lambda: f"renamed-{tok}" not in host.ssh(f"ls {host.root}/qa"), 20, 0.5)
            state = self.remote()
            self.record("N.delete-reaches-host", bool(gone) or state.get("heldDeletes", 0) > 0,
                        f"{dt:.1f}s held={state.get('heldDeletes')} ls={host.ssh(f'ls {host.root}/qa')!r}")
            app.check("N")
        if self.runs("T"):
            self.run_terminal_restore()
        if self.runs("V"):
            self.run_host_view()
        if self.runs("P"):
            # The remote commands no other scenario runs, each with what it answers
            # and, where one is cheap, a receipt: Remove Mirror stays out (L relaunches
            # on the mirror) and Resolve needs a conflict (D/D2 make theirs by banner).
            reply = app.send("remote-status")
            self.record("P.status", bool(reply.get("ok")), f"reply={reply}")
            reply = app.send("remote-show-log", host.name)
            # The log is fetched from the host first: a round trip before it shows.
            shown, _ = wait_for(lambda: (lambda s: s if "remote" in s.lower() else None)(
                app.query("output").get("shown", "")), 10, 0.2)
            shown = shown or app.query("output").get("shown", "")
            self.record("P.show-log", bool(reply.get("ok")) and "remote" in shown.lower(),
                        f"reply={reply} shown={shown!r}")
            reply = app.send("remote-copy-command", host.name, "ssh")
            app.send("tab"); app.settle(0.3); app.send("paste"); app.settle(0.3)
            pasted = app.text()
            app.send("undo")
            self.record("P.copy-command", bool(reply.get("ok")) and "ssh" in pasted and host.name in pasted,
                        f"reply={reply} pasted={pasted[:120]!r}")
            port = "48231"
            reply = app.send("remote-forward-port", port, host.name)
            self.record("P.forward-port", bool(reply.get("ok")), f"reply={reply}")
            reply = app.send("remote-unforward-port", port, host.name)
            self.record("P.unforward-port", bool(reply.get("ok")), f"reply={reply}")
            host.ssh(f"cd {host.root} && echo HOST-CHANGE-{tok} >> {EDIT_FILE}")
            self.mirror_has(EDIT_FILE, f"HOST-CHANGE-{tok}")
            reply = app.send("remote-changes")
            self.record("P.changes", bool(reply.get("ok")), f"reply={reply}")
            reply = app.send("remote-clear-changes")
            self.record("P.clear-changes", bool(reply.get("ok")), f"reply={reply}")
            reply = app.send("remote-apply-deletes")
            self.info("P.apply-deletes", f"reply={reply}")
            reply = app.send("remote-stop-server", host.name)
            stopped = bool(reply.get("ok"))
            reply2 = app.send("remote-reconnect", host.name)
            msg, dt, ready = self.wait_ready(120)
            self.record("P.stop-server-then-reconnect", stopped and ready,
                        f"stop={reply} reconnect={reply2} {msg} after {dt:.1f}s")
            app.check("P")
        self.info("notifications-at-end", str([(n["key"], n["message"]) for n in self.notifs()]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenarios", nargs="*", help="letters to run (default: all)")
    parser.add_argument("--host", default=os.environ.get("MQA_REMOTE_HOST"))
    parser.add_argument("--root", default=os.environ.get("MQA_REMOTE_ROOT"))
    parser.add_argument("--base", default=os.environ.get("MQA_REMOTE_BASE"))
    parser.add_argument("--out", default=None)
    args = parser.parse_args()
    if not args.host or not args.root:
        print("remote suite not run: set MQA_REMOTE_HOST and MQA_REMOTE_ROOT (an ssh host and a git "
              "repository on it)")
        return 0
    out = Path(args.out or f"/tmp/mqa-remote-{time.strftime('%Y%m%d-%H%M%S')}")
    out.mkdir(parents=True, exist_ok=True)
    server = os.environ.get("MICROIDE_SERVER_BINARY") or str(REPO / "build/server-x86_64/microide/microide-server")
    if not Path(server).exists():
        print(f"no static server at {server} (tools/build-server.sh x86_64, or set MICROIDE_SERVER_BINARY)")
        return 1
    host = Host(args.host, args.root, args.base)
    print("host reset:", host.reset())
    fixture = q.make_fixture(out / "fixture")
    env = {"MICROIDE_SERVER_BINARY": server}
    ssh_args = ["--set", "remote.ssh_command", f"ssh -F {SSH_CONFIG}"] if SSH_CONFIG else []
    results: list[tuple[str, str, str]] = []
    findings: list[q.Finding] = []
    mirror: Path | None = None
    work: Path | None = None
    only = set(args.scenarios)
    try:
        with q.App.launch(project=fixture, shots=out / "shots", keep_home=True, extra_env=env,
                          extra_args=ssh_args) as app:
            suite = Suite(app, host, only)
            suite.run_all()
            results, mirror, work = suite.results, suite.mirror, app.work
            findings += app.findings
            # The mirror must survive the first app's exit for L: keep its work dir.
            os.environ["MQA_KEEP_WORK"] = "1"
        if mirror is not None and (not only or "L" in only):
            same_dirs = {k: str(work / k.split("_")[1].lower()) for k in
                         ("XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME")}
            with q.App.launch(project=mirror, shots=out / "shots", keep_home=True,
                              extra_env={**env, **same_dirs}, extra_args=ssh_args) as app2:
                suite2 = Suite(app2, host, only); suite2.mirror = mirror
                app2.settle(1.0); msg, dt, ok = suite2.wait_ready(120)
                suite2.record("L.relaunch-reconnects", ok, f"{msg} after {dt:.1f}s")
                app2.send("open", EDIT_FILE); app2.settle(0.5)
                suite2.type_line(f"QA-RELAUNCH-{suite2.token}"); app2.send("save")
                ok, dt = suite2.host_has(f"QA-RELAUNCH-{suite2.token}", 30)
                suite2.record("L.save-after-relaunch", bool(ok), f"{dt:.2f}s")
                app2.check("L"); results += suite2.results; findings += app2.findings
    finally:
        if work is not None and os.environ.get("MQA_KEEP_WORK") == "1" and not findings:
            shutil.rmtree(work, ignore_errors=True)
        print("host reset:", host.reset())
        if host.agents:
            host.ssh(f"rm -rf {os.path.dirname(host.agents)}")
    print("\n==== RESULTS")
    for name, status, detail in results:
        print(f"{status:5} {name}: {detail[:240]}")
    failed = [r for r in results if r[1] == "FAIL"]
    print(f"==== {len(failed)} failed, {len([f for f in findings if f.kind != 'expect'])} oracle findings; "
          f"output in {out}")
    for finding in findings:
        if finding.kind != "expect":
            print("  finding:", finding.kind, finding.detail[:200], finding.screenshot or "")
    return 1 if failed or any(f.kind != "expect" for f in findings) else 0


if __name__ == "__main__":
    sys.exit(main())
