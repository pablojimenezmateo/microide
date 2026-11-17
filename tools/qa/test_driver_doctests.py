"""The `>>>` examples in the driver's docstrings are tests, and every module's
run here: nothing else runs them.

They were run module by module, by whoever remembered to add a line: the
oracles' (monkey_oracles) ran nowhere, and the driver's own went through a
`DocTestRunner().run(test)` whose result nobody read -- a failing example
printed its diff and the test passed. A module with an example is found by its
text, so a new one needs no line here."""
import doctest
import importlib
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def modules_with_examples(directory: Path) -> list[str]:
    """The modules of `directory` (not its tests) whose source has a `>>>`."""
    return sorted(path.stem for path in directory.glob("*.py")
                  if not path.name.startswith("test_") and ">>> " in path.read_text())


class DoctestTests(unittest.TestCase):
    def test_every_module_with_examples_is_found(self):
        names = modules_with_examples(HERE)
        for expected in ("microide_qa", "monkey", "monkey_oracles", "run"):
            self.assertIn(expected, names)

    def test_the_examples_hold(self):
        attempted = 0
        for name in modules_with_examples(HERE):
            with self.subTest(module=name):
                result = doctest.testmod(importlib.import_module(name))
                self.assertEqual(result.failed, 0, f"{name}: {result.failed} example(s) failed (printed above)")
                attempted += result.attempted
        self.assertGreater(attempted, 12, "the examples were not found")

    def test_a_failing_example_fails(self):
        # The negative control: the runner this replaces reported nothing.
        def broken():
            """
            >>> 1 + 1
            3
            """
        runner = doctest.DocTestRunner(verbose=False)
        for test in doctest.DocTestFinder().find(broken, "broken", globs={}):
            runner.run(test, out=lambda text: None)
        self.assertEqual(runner.failures, 1)


if __name__ == "__main__":
    unittest.main()
