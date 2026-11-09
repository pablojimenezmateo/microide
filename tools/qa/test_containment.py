"""A QA run's git stays inside its scratch project.

Scratch projects live under build/, inside the microide checkout. A soak run once
broke its project's `.git` and every git after that -- the monkey's and the
app's -- resolved to the checkout: a commit on main, a branch switch, a stash and
a branch pushed to GitHub. These pin the fences and the soak runner's alarm.
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "soak"))

import microide_qa as q  # noqa: E402
import monkey as m  # noqa: E402
import soak  # noqa: E402

GIT_ID = {"GIT_AUTHOR_NAME": "Owner", "GIT_AUTHOR_EMAIL": "owner@example.com",
          "GIT_COMMITTER_NAME": "Owner", "GIT_COMMITTER_EMAIL": "owner@example.com"}


def git(repo: Path, *args: str) -> str:
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(GIT_ID)
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True,
                          text=True, env=env).stdout.strip()


def make_repo(path: Path) -> Path:
    path.mkdir(parents=True)
    git(path, "init", "-q", "-b", "main")
    (path / "tracked.txt").write_text("owner\n")
    git(path, "add", "-A")
    git(path, "commit", "-q", "-m", "owner commit")
    return path


class ContainmentTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="mqa-contain-", dir=q.REPO / "build")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        # Its own repository: the tests must not lean on the checkout around them.
        self.outer = make_repo(self.root / "outer")

    def broken_project(self) -> Path:
        project = self.outer / "runs" / "project"
        (project / ".git" / "qa-deleted").mkdir(parents=True)  # a marker, not a repository
        (project / "file.txt").write_text("scratch\n")
        return project

    def test_monkey_git_never_reaches_an_enclosing_repository(self):
        project = self.broken_project()
        for op in ("git-commit", "git-switch", "git-stash"):
            m.apply_external(project, op, "file.txt", 7)
        self.assertEqual(git(self.outer, "log", "--format=%s"), "owner commit")
        self.assertEqual(git(self.outer, "branch", "--format=%(refname:short)"), "main")
        self.assertEqual(git(self.outer, "stash", "list"), "")

    def test_app_environment_fences_git_to_the_project(self):
        project = self.broken_project()
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env.update(q.git_fence_env(project))
        result = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=project, env=env,
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(q.git_fence_env(project)["GIT_ALLOW_PROTOCOL"], "file")

    def test_a_cloned_project_cannot_push_into_its_source(self):
        clone = self.root / "fixtures" / "project"
        clone.parent.mkdir()
        m.copy_project(self.outer, clone)
        origin = Path(git(clone, "remote", "get-url", "origin"))
        self.assertNotEqual(origin.resolve(), self.outer.resolve())
        git(clone, "switch", "-q", "-c", "qa-branch-1")
        git(clone, "push", "-q", "origin", "qa-branch-1")
        self.assertEqual(git(self.outer, "branch", "--format=%(refname:short)"), "main")

    def test_soak_alarm_sees_monkey_commits_and_branches(self):
        def in_outer(*args, cwd=None):
            return git(self.outer, *args)
        with patch.object(soak, "git", in_outer):
            self.assertIsNone(soak.containment_breach())
            git(self.outer, "branch", "qa-branch-2")
            self.assertIn("refs/heads/qa-branch-2", soak.containment_breach())
            git(self.outer, "branch", "-D", "qa-branch-2")
            env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
            env.update(m.git_env())
            subprocess.run(["git", "-C", str(self.outer), "commit", "-q", "--allow-empty", "-m",
                            "qa commit 29"], check=True, env=env)
            breach = soak.containment_breach()
            self.assertIn("refs/heads/main", breach)
            self.assertIn("HEAD", breach)


if __name__ == "__main__":
    unittest.main()
