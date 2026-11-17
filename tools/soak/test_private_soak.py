"""A private soak of a branch: the ref the runner follows and the builds it
makes are the environment's to say, so a change to the runner or the driver can
be soaked before it is merged into the `main` the live soak follows."""
import importlib
import os
import random
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import soak  # noqa: E402

HEAD = "a" * 40


class PrivateSoakTests(unittest.TestCase):
    def reloaded(self, **env: str):
        """The soak module as a runner started with `env` has it; the other
        tests' module again when this one ends."""
        scratch = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
        self.addCleanup(scratch.cleanup)
        self.addCleanup(importlib.reload, soak)
        patcher = mock.patch.dict(os.environ, {"MICROIDE_SOAK_DIR": scratch.name, **env})
        patcher.start()
        self.addCleanup(patcher.stop)
        return importlib.reload(soak)

    def test_the_live_soak_follows_main_and_builds_everything(self):
        with mock.patch.dict(os.environ):
            os.environ.pop("MICROIDE_SOAK_REF", None)
            os.environ.pop("MICROIDE_SOAK_BUILDS", None)
            module = self.reloaded()
        self.assertEqual(module.FOLLOW_REF, "main")
        self.assertLessEqual({"default", "release", "asan", "ubsan", "tsan"}, set(module.BUILDS))

    def test_the_ref_followed_is_the_one_built(self):
        module = self.reloaded(MICROIDE_SOAK_REF="soak-review")
        asked: list[tuple[str, ...]] = []

        def git(*args: str, cwd: Path | None = None) -> str:
            asked.append(args)
            return HEAD
        with mock.patch.object(module, "git", git):
            self.assertEqual(module.followed_head(), HEAD)
            # ... and a commit built at that ref's head is not built again.
            tried = {name: HEAD for name in module.BUILDS}
            self.assertFalse(module.refresh_due({"built_sha": HEAD, "good": {}, "tried": tried}))
        self.assertTrue(all("soak-review^{commit}" in args for args in asked), asked)
        self.assertFalse(any("main" in args for args in asked), asked)

    def test_only_the_builds_named_are_built_and_run(self):
        module = self.reloaded(MICROIDE_SOAK_BUILDS="default")
        self.assertEqual(list(module.BUILDS), ["default"])
        # Built: nothing waits for a configuration this soak does not make.
        with mock.patch.object(module.BUILDS["default"], "binary", return_value=Path(sys.executable)):
            self.assertTrue(module.is_built({"built_sha": HEAD, "good": {"default": {}}, "tried": {"default": HEAD}},
                                            HEAD))
        # A state that knows a build this soak does not make (the live soak's,
        # copied) starts no job of it.
        state = {"good": {"default": {}, "asan": {}}}
        rng = random.Random(7)
        self.assertEqual({module.choose_job(rng, state).build for _ in range(300)}, {"default"})

    def test_a_build_that_does_not_exist_is_refused(self):
        with self.assertRaises(SystemExit) as refused:
            self.reloaded(MICROIDE_SOAK_BUILDS="default,asna")
        self.assertIn("asna", str(refused.exception))


if __name__ == "__main__":
    unittest.main()
