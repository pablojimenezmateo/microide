#!/usr/bin/env python3
"""What the monkey does for the agent-workflow features: working trees beside
the project that the worktree pick lists and the run never opens, a terminal
that announces while another application has the focus, an image pasted from
the clipboard, and another machine's open files left on a remote host."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import microide_qa as q  # noqa: E402
import monkey as mk  # noqa: E402
import remote_host as rh  # noqa: E402
import x_clipboard as xc  # noqa: E402


class ScriptedRandom:
    """random() answers from a script; choice() takes the first."""

    def __init__(self, *rolls: float):
        self.rolls = list(rolls)

    def random(self) -> float:
        return self.rolls.pop(0)

    def choice(self, items):
        return items[0]


class RecordingApp:
    wayland = False
    outside_change_pending = False

    def __init__(self, ui: dict | None = None):
        self.did: list[tuple] = []
        self.last_ui = ui or {}

    def ui(self) -> dict:
        return self.last_ui

    def __getattr__(self, name: str):
        return lambda *args, **kwargs: self.did.append((name, *args))


def _monkey(app: RecordingApp, *rolls: float) -> mk.Monkey:
    monkey = mk.Monkey.__new__(mk.Monkey)
    monkey.app, monkey.rng, monkey.counter = app, ScriptedRandom(*rolls), 0
    monkey.files, monkey.weird, monkey.project, monkey.host = ["README.md", "src/main.cpp"], [], None, None
    return monkey


class WorktreeOpTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.project = Path(scratch.name) / "taskflow"
        self.project.mkdir()
        (self.project / "a.txt").write_text("a\n")
        for args in (["init", "-q", "-b", "main"], ["add", "-A"], ["commit", "-q", "-m", "first"]):
            subprocess.run(["git", "-C", str(self.project), *args], check=True, capture_output=True,
                           env=mk.git_env(self.project))

    def trees(self) -> list[dict[str, str]]:
        listing = subprocess.run(["git", "-C", str(self.project), "worktree", "list", "--porcelain"],
                                 check=True, capture_output=True, text=True, env=mk.git_env(self.project)).stdout
        return [dict(line.partition(" ")[::2] for line in block.splitlines())
                for block in listing.strip().split("\n\n")][1:]

    def test_a_slot_is_added_locked_deleted_and_added_again(self) -> None:
        tree = self.project.parent / f"{mk.WORKTREE_PREFIX}-0"
        mk.apply_external(self.project, "git-worktree", "", 0)
        self.assertTrue((tree / "a.txt").is_file())
        self.assertIn("detached", self.trees()[0])
        mk.apply_external(self.project, "git-worktree", "", 3)
        self.assertIn("locked", self.trees()[0])
        mk.apply_external(self.project, "git-worktree", "", 6)
        self.assertFalse(tree.exists())
        self.assertIn("prunable", self.trees()[0])
        mk.apply_external(self.project, "git-worktree", "", 12)  # a branch this time
        self.assertEqual(self.trees()[0].get("branch"), "refs/heads/qa-wt-0")
        self.assertNotIn("prunable", self.trees()[0])

    def test_the_slots_are_beside_the_project_and_named_for_the_guard(self) -> None:
        for counter in (0, 1, 2):
            mk.apply_external(self.project, "git-worktree", "", counter)
        names = sorted(Path(tree["worktree"]).name for tree in self.trees())
        self.assertEqual(names, [f"{mk.WORKTREE_PREFIX}-{slot}" for slot in range(3)])
        self.assertTrue(all(Path(tree["worktree"]).parent == self.project.parent for tree in self.trees()))


class WorktreeGuardTests(unittest.TestCase):
    """Choosing another working tree opens it as the project: the run would leave
    its scratch copy."""

    @staticmethod
    def pick(selected: str) -> dict:
        return {"overlay": {"visible": True, "mode": "quick-pick"}, "elements": [
            {"kind": "overlay-row", "name": f"overlay-row:{label}", "label": label, "rect": [10, 10 + 20 * i, 200, 20],
             "selected": label == selected}
            for i, label in enumerate(("taskflow", "qa-worktree-0", "qa-worktree-1"))]}

    def test_a_key_does_not_choose_another_tree(self) -> None:
        self.assertTrue(_monkey(RecordingApp(self.pick("qa-worktree-1")))._activation_chooses_forbidden())

    def test_a_key_may_choose_the_project_itself(self) -> None:
        self.assertFalse(_monkey(RecordingApp(self.pick("taskflow")))._activation_chooses_forbidden())

    def test_left_and_right_move_the_caret_in_the_pick(self) -> None:
        self.assertFalse(_monkey(RecordingApp(self.pick("qa-worktree-1")))._activation_chooses_forbidden(stepping=True))

    def test_the_pointer_does_not_press_another_tree(self) -> None:
        rects = mk.Monkey._forbidden_rects(self.pick("taskflow"))
        self.assertEqual(rects, [[10, 30, 200, 20], [10, 50, 200, 20]])


class ExternalSourceTests(unittest.TestCase):
    def test_a_worktree_change_is_followed_by_the_pick(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            app = RecordingApp()
            monkey = _monkey(app, 0.9, 0.9)  # not the clipboard, no checkpoint
            monkey.project = monkey.world = Path(scratch)
            monkey.rng.choice = lambda items: "git-worktree" if items is mk.EXTERNAL_OPS else items[0]
            with mock.patch.object(mk, "apply_external") as applied:
                self.assertEqual(monkey.external_action(), "ext:git-worktree")
            applied.assert_called_once()
            self.assertEqual(app.did[-1], ("send_line", "git-worktrees"))

    def test_a_change_between_a_checkpoint_and_its_review(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            app = RecordingApp()
            monkey = _monkey(app, 0.9, 0.01)
            monkey.project = monkey.world = Path(scratch)
            with mock.patch.object(mk, "apply_external") as applied:
                self.assertEqual(monkey.external_action(), "ext:checkpoint-review")
            names = [entry[:2] for entry in app.did if entry[0] in ("send_line", "record_external")]
            self.assertEqual(names[0], ("send_line", "review-checkpoint-mark"))
            self.assertEqual(names[1][0], "record_external")
            self.assertEqual(names[2], ("send_line", "review-checkpoint"))
            applied.assert_called_once()

    def test_an_image_can_be_what_another_application_left(self) -> None:
        app = RecordingApp()
        monkey = _monkey(app, 0.01, 0.01)
        monkey.project = monkey.world = Path("/nonexistent")
        self.assertEqual(monkey.external_action(), "ext:foreign-clipboard-image")
        self.assertEqual(app.did, [("foreign_clipboard_image", xc.IMAGE_KINDS[0])])


class TerminalSourceTests(unittest.TestCase):
    FOCUSED = {"focus": "panel", "panel": {"visible": True, "content": "terminal"}}

    def test_an_alert_arrives_after_the_focus_left(self) -> None:
        app = RecordingApp(self.FOCUSED)
        monkey = _monkey(app, 0.9, 0.9, 0.05)  # no new terminal, no key, the alert
        with mock.patch.object(mk.time, "sleep") as slept:
            self.assertEqual(monkey.terminal_action(), "terminal:alert-unfocused")
        self.assertEqual([entry[0] for entry in app.did], ["type", "focus_out"])
        self.assertTrue(app.did[0][1].startswith("sleep 0.3; printf"))
        # The program's own sleep is over before the driver's next action.
        self.assertGreater(slept.call_args.args[0], 0.3)

    def test_an_image_is_pasted_with_the_terminals_chord(self) -> None:
        app = RecordingApp(self.FOCUSED)
        monkey = _monkey(app, 0.9, 0.9, 0.12)
        self.assertEqual(monkey.terminal_action(), "terminal:paste-image")
        self.assertEqual(app.did, [("foreign_clipboard_image", xc.IMAGE_KINDS[0]), ("key", "ctrl+shift+v")])

    def test_the_alerts_are_shell_lines_too(self) -> None:
        for alert in mk.TERMINAL_ALERTS:
            self.assertIn(alert + "\n", mk.TERMINAL_LINES)


class RoamingTests(unittest.TestCase):
    def host(self, home: Path) -> rh.LoopbackHost:
        host = rh.LoopbackHost.__new__(rh.LoopbackHost)
        host.home = home
        return host

    def test_nothing_is_left_before_the_editor_left_its_own(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            self.assertEqual(self.host(Path(scratch)).leave_session(["a.txt"], 0), 0)
            self.assertEqual(list(Path(scratch).rglob("*")), [])

    def test_the_other_machines_session_replaces_the_editors(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            held = Path(scratch) / ".local" / "share" / "microide" / "server" / "workspace-state" / "ab12" / "session"
            held.parent.mkdir(parents=True)
            held.write_text("{}")
            self.assertEqual(self.host(Path(scratch)).leave_session(["a.txt", "b.txt"], 0), 1)
            left = json.loads(held.read_text())
            self.assertEqual(left["client"], "qa-other-machine")
            self.assertEqual([entry["path"] for entry in left["files"]], ["a.txt", "b.txt"])

    def test_the_remote_source_leaves_one_and_connects_again(self) -> None:
        app = RecordingApp()
        monkey = _monkey(app, 0.9, 0.05)  # no fault, the roam
        monkey.host = mock.Mock()
        self.assertEqual(monkey.remote_action(), "remote:roam")
        monkey.host.leave_session.assert_called_once_with(monkey.files, 1)
        self.assertEqual(app.did, [("record_external", {"external": "remote-roam", "target": "", "counter": 1}),
                                   ("send_line", "remote-disconnect"), ("send_line", "remote-reconnect")])

    def test_a_replay_leaves_the_same_session(self) -> None:
        monkey = _monkey(RecordingApp())
        monkey.host, monkey.world = mock.Mock(), Path("/nonexistent")
        monkey.replay_external({"external": "remote-roam", "target": "", "counter": 9})
        monkey.host.leave_session.assert_called_once_with(monkey.files, 9)

    def test_leaving_for_another_application_is_a_remote_action(self) -> None:
        app = RecordingApp()
        monkey = _monkey(app, 0.9, 0.1)
        monkey.host = mock.Mock()
        self.assertEqual(monkey.remote_action(), "remote:leave")
        self.assertEqual(app.did, [("focus_out",)])


class ReplayTests(unittest.TestCase):
    def test_an_image_clipboard_row_is_replayed(self) -> None:
        app = RecordingApp()
        app.replay_root = None
        app.check = lambda label="": []
        q.replay(app, [{"foreign-clipboard-image": "png-large"}], pause=0.0)
        self.assertIn(("foreign_clipboard_image", "png-large"), app.did)

    def test_the_driver_records_the_image_it_offers(self) -> None:
        class WaylandApp(q.App):
            wayland = True  # no clipboard owner is started: the row is what is under test

        app = WaylandApp.__new__(WaylandApp)
        recorded: list[dict] = []
        app._record = recorded.append
        app.foreign_clipboard_image("png")
        self.assertEqual(recorded, [{"foreign-clipboard-image": "png"}])


if __name__ == "__main__":
    unittest.main()
