"""A replay rebuilds the run: its files, its outside world, and a check that it
went where the run went."""
import unittest
from pathlib import Path

import findings as fnd
import microide_qa as q
import monkey as m
import monkey_model as mm
import monkey_oracles as mo


class FakeApp:
    def __init__(self, root: str | None = None):
        self.replay_root = Path(root) if root else None
        self.sent: list[str] = []
        self.actions: list[dict] = []
        self.last_ui = self.last_buffer = self.last_editor = None

    def send_line(self, line):
        self.sent.append(line)

    def send(self, *words):
        self.sent.append(" ".join(words))

    def key(self, key):
        self.sent.append(key)

    def settle_quiet(self, most=0.0):
        pass

    def check(self, label=""):
        return []

    def record_external(self, action):
        self.actions.append(action)


class RootTests(unittest.TestCase):
    OLD = "/home/u/build/soak/runs/20261114-diff-801/fixtures/fx-094v33bo/taskflow"

    def test_a_line_naming_the_runs_fixture_names_the_replays(self):
        # 204 of the soak's 1187 bundles named files of a deleted run directory.
        app = FakeApp("/new/fx-1/taskflow")
        actions = [{"line": f"compare-files {self.OLD}/qa-diff/v1.left.py {self.OLD}/qa-diff/v1.right.py"},
                   {"line": "goto 3"}, {"send": ["breakpoint-set", f"{self.OLD}/src/main.cpp", "4"]}]
        q.replay(app, actions, check=False, root=self.OLD)
        self.assertEqual(app.sent, ["compare-files /new/fx-1/taskflow/qa-diff/v1.left.py "
                                    "/new/fx-1/taskflow/qa-diff/v1.right.py", "goto 3",
                                    "breakpoint-set /new/fx-1/taskflow/src/main.cpp 4"])

    def test_the_root_is_read_from_the_header_or_from_an_old_records_lines(self):
        self.assertEqual(q.recorded_root({"root": "/a/b", "actions": []}), "/a/b")
        legacy = {"actions": [{"key": "a"}, {"line": f"breakpoint-enable {self.OLD}/include/s.hpp 11"}]}
        self.assertEqual(q.recorded_root(legacy), self.OLD)
        self.assertIsNone(q.recorded_root({"actions": [{"line": "project-replace /etc/passwd"}]}))

    def test_a_path_that_only_starts_like_the_root_is_left_alone(self):
        row = {"line": f"open {self.OLD}-origin.git/config"}
        self.assertEqual(q.rebase_action(row, self.OLD, "/new"), row)


class PathTests(unittest.TestCase):
    def test_the_run_stamps_each_steps_state_on_its_last_action(self):
        trail, actions = mm.StateTrail(), [{"key": "a"}]
        trail.stamp(actions, "f=editor")
        actions.append({"key": "b"})
        trail.stamp(actions, "f=editor")
        trail.stamp(actions, "f=panel")  # a step that recorded nothing: the stamp stays
        actions.append({"key": "c"})
        trail.stamp(actions, "f=panel")
        self.assertEqual([a["s"] for a in actions], ["f=editor", mm.SAME_STATE, "f=panel"])

    def test_a_replay_that_follows_the_run_and_one_that_leaves_it(self):
        follows = mm.Divergence()
        for index, (recorded, actual) in enumerate([("f=editor h=", "f=editor h=hover"), ("=", "f=editor h="),
                                                    ("f=panel h=", "f=panel h=")]):
            follows.see(index, recorded, actual)
        self.assertEqual(follows.summary(), {"verdict": "followed", "compared": 3, "differed": 0})
        leaves = mm.Divergence()
        leaves.see(0, "f=editor t=compare", "f=editor t=compare")
        for index in range(1, 6):
            leaves.see(index, "=", "f=editor t=editor", {"line": "compare-files /gone/a /gone/b", "t": 1.0})
        summary = leaves.summary()
        self.assertEqual((summary["verdict"], summary["differed"], summary["first"]["action"]), ("diverged", 5, 1))
        self.assertIn("compare-files", summary["first"]["after"])
        self.assertEqual(mm.Divergence().verdict(), "unknown")

    def test_replay_calls_follow_only_for_checked_actions_that_carry_a_state(self):
        app, seen = FakeApp(), []
        actions = [{"key": "a", "s": "f=editor"}, {"key": "b"}, {"key": "c", "s": "="}]
        q.replay(app, actions, check=True, follow=lambda index, action: seen.append((index, action["s"])))
        self.assertEqual(seen, [(0, "f=editor"), (2, "=")])
        q.replay(app, actions, check=False, follow=lambda *a: seen.append("unchecked"))
        self.assertEqual(len(seen), 2)


class ExternalRowTests(unittest.TestCase):
    def test_a_replayed_outside_world_row_goes_into_the_new_record(self):
        # A run that starts from a corpus prefix replays it: its git steps and
        # generated sources were applied and not recorded, so the run's own
        # replay opened files nothing had created.
        import tempfile
        with tempfile.TemporaryDirectory() as scratch:
            monkey = m.Monkey.__new__(m.Monkey)
            monkey.app, monkey.world, monkey.host = FakeApp(), Path(scratch), None
            monkey.replay_external({"external": "diff-variant", "target": "", "counter": 3, "t": 9.0, "s": "x"})
            monkey.replay_churn({"churn": "README.md", "counter": 4, "t": 9.5})
            self.assertEqual(monkey.app.actions, [{"external": "diff-variant", "target": "", "counter": 3},
                                                  {"churn": "README.md", "counter": 4}])
            self.assertTrue((Path(scratch) / "qa-diff").is_dir())


CRASH = """microide redraw frame: frame=7
microide crash: signal 6 (SIGABRT) on thread 41 (microide); backtrace:
/opt/bin/microide(+0x70e988) [0x55d0c0a0e988]
/lib/x86_64-linux-gnu/libc.so.6(abort+0x27) [0x7f3a1c0288ff]
/lib/x86_64-linux-gnu/libc.so.6(+0x2a601) [0x7f3a1c02a601]
microide crash: end of backtrace
"""


class CrashTests(unittest.TestCase):
    def test_the_crash_block_is_found_and_its_frames_numbered(self):
        block = q.CRASH_BLOCK.search(CRASH).group(0)
        # No such binary: the app's own frame keeps its offset, a library's its symbol.
        text = q.symbolize_backtrace(block, Path("/nonexistent/microide"))
        lines = text.splitlines()
        self.assertTrue(lines[0].startswith("microide crash: signal 6 (SIGABRT)"))
        self.assertEqual(lines[1], "#0 0x70e988 in ?? ?? [microide+0x70e988]")
        self.assertEqual(lines[2], "#1 0x27 in abort ?? [libc.so.6+0x27]")

    def test_a_crash_is_keyed_on_our_frames_with_or_without_debug_info(self):
        with_lines = ("#0 0x1 in abort ?? [libc.so.6+0x1]\n"
                      "#1 0x70e988 in microide::app::Application::Run() /w/src/app/Application.cpp:293\n"
                      "#2 0x700ea8 in main /w/src/app/main.cpp:117\n")
        self.assertEqual(fnd.stack_key(with_lines),
                         "microide::app::Application::Run@src/app/Application.cpp > main@src/app/main.cpp")
        release = "#1 0x70e988 in microide::app::Application::Run() ?? [microide+0x70e988]\n"
        self.assertEqual(fnd.stack_key(release), "microide::app::Application::Run@?")
        one = fnd.signature("crash", "microide exited (-6: SIGABRT) after step 3", with_lines)
        other = fnd.signature("crash", "microide exited (-6: SIGABRT) after step 9",
                              with_lines.replace("Application::Run", "Application::Render"))
        self.assertNotEqual(one, other)
        self.assertEqual(one, fnd.signature("crash", "microide exited (-11: SIGSEGV) after step 5", with_lines))

    def test_a_sanitizer_report_keeps_its_key(self):
        report = "    #3 0x55 in microide::editor::TextViewport::Undo() /w/src/editor/TextViewport.cpp:812:5\n"
        self.assertEqual(fnd.stack_key(report), "microide::editor::TextViewport::Undo@src/editor/TextViewport.cpp")


class SlowApp:
    def __init__(self, what, seconds, sched, stall="", waits=""):
        self.slowest_answer, self.slowest_sched, self.slowest_stall = (what, seconds), sched, stall
        self.slowest_waits = waits


class LivenessTests(unittest.TestCase):
    def test_a_slow_answer_from_a_starved_machine_is_counted_and_not_filed(self):
        oracle = mo.Liveness(SlowApp("query 'ui'", 6.0, (0.4, 5.0)))
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.starved, 1)
        # Not marked reported: the next slow answer of the verb is judged afresh.
        oracle.app = SlowApp("query 'ui'", 6.0, (5.5, 0.1), "microide stall: the main loop ...")
        found = oracle.check()
        self.assertEqual(len(found), 1)
        self.assertIn("its main thread was mostly computing", found[0][1])
        self.assertIn(b"5.50s on a CPU", oracle.evidence["latency.txt"])
        self.assertIn(b"microide stall", oracle.evidence["latency.txt"])

    def test_a_blocked_main_thread_is_said_so_and_an_unknown_one_is_still_filed(self):
        blocked = mo.Liveness(SlowApp("query 'ui'", 6.0, (0.2, 0.3)))
        self.assertIn("mostly blocked", blocked.check()[0][1])
        unknown = mo.Liveness(SlowApp("hit-test", 6.0, None))
        self.assertEqual(unknown.check()[0][1], "hit-test took 6.00s to answer (budget 2.5s)")

    def test_a_blocked_main_threads_waits_are_evidence(self):
        # 3.01s "mostly blocked" with the watchdog silent said nothing of WHERE
        # (soak 049ca74b0cee): the samples taken while the answer was late do.
        oracle = mo.Liveness(SlowApp("query 'ui'", 3.0, (0.0, 0.0), "",
                                     "  14 x wchan=do_sys_poll syscall=7 0x7ffd"))
        oracle.check()
        self.assertIn(b"where the main thread sat", oracle.evidence["latency.txt"])
        self.assertIn(b"wchan=do_sys_poll", oracle.evidence["latency.txt"])
        self.assertEqual(oracle.app.slowest_waits, "", "the samples are consumed with the answer")

    def test_a_slow_command_is_one_finding_whatever_text_it_carried(self):
        # `type` with a long argument was a signature per text typed, and a run
        # filed one per text: the finding names the command, the evidence the line.
        import findings as fnd
        first, second = repr("type " + "x" * 400), repr('type "word word" ' + "😀" * 300)
        oracle = mo.Liveness(SlowApp(first, 6.0, None))
        found = oracle.check()
        self.assertEqual(found, [("latency", "the type command took 6.00s to answer (budget 2.5s)")])
        self.assertIn(first.encode(), oracle.evidence["latency.txt"])
        oracle.app = SlowApp(second, 7.0, None)
        self.assertEqual(oracle.check(), [], "the same command, slow again in the same run")
        # ... and another command is another finding, with another signature.
        oracle.app = SlowApp(repr("format-document"), 7.0, None)
        other = oracle.check()
        self.assertEqual(other[0][1], "the format-document command took 7.00s to answer (budget 2.5s)")
        self.assertNotEqual(fnd.signature(*found[0]), fnd.signature(*other[0]))
        self.assertNotEqual(fnd.signature("latency", "the ui query took 6.00s to answer (budget 2.5s)"),
                            fnd.signature("latency", "the buffer query took 6.00s to answer (budget 2.5s)"))


class InvariantApp:
    def __init__(self, answers):
        self.answers = list(answers)

    def query(self, verb, args=None):
        if verb != "invariants":
            return {"verb": verb}
        answer = self.answers.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer


class InvariantOracleTests(unittest.TestCase):
    def test_a_broken_rule_is_a_finding_once_with_the_state_beside_it(self):
        broken = {"ok": False, "violations": [{"rule": "untitled", "detail": "Untitled-2 names two different buffers"}]}
        oracle = mo.InvariantOracle(InvariantApp([{"ok": True, "violations": []}, broken, broken]))
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.check(), [("invariant", "untitled: Untitled-2 names two different buffers")])
        self.assertIn("invariants.json", oracle.evidence)
        self.assertIn("tabs.json", oracle.evidence)
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.checks, 3)

    def test_a_binary_without_the_query_is_asked_once(self):
        oracle = mo.InvariantOracle(InvariantApp([q.QaFailure('query \'invariants\' failed: {"error": "unknown query"}')]))
        self.assertEqual(oracle.check(), [])
        self.assertEqual(oracle.check(), [])  # no second ask: the list of answers is empty
        self.assertEqual(oracle.checks, 0)


if __name__ == "__main__":
    unittest.main()
