"""The examples in the soak's docstrings are tests: nothing else ran them, so a
`>>>` line could rot beside the code it documents. Every module with one is
found by its text, so a new module needs no line here -- they were listed one
by one, and the list was always a module short (tools/qa/test_driver_doctests.py is
the driver's)."""
import doctest
import importlib
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))


def modules_with_examples(directory: Path) -> list[str]:
    """The modules of `directory` (not its tests) whose source has a `>>>`."""
    return sorted(path.stem for path in directory.glob("*.py")
                  if not path.name.startswith("test_") and ">>> " in path.read_text())


class DoctestTests(unittest.TestCase):
    def test_every_module_with_examples_is_found(self):
        names = modules_with_examples(HERE)
        for expected in ("codecov", "minimize", "soak", "surface"):
            self.assertIn(expected, names)

    def test_the_examples_hold(self):
        attempted = 0
        for name in modules_with_examples(HERE):
            with self.subTest(module=name):
                result = doctest.testmod(importlib.import_module(name))
                self.assertEqual(result.failed, 0, f"{name}: {result.failed} example(s) failed (printed above)")
                attempted += result.attempted
        self.assertGreater(attempted, 25, "the examples were not found")


if __name__ == "__main__":
    unittest.main()
