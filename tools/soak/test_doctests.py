"""The examples in the soak's docstrings are tests: nothing else ran them, so a
`>>>` line could rot beside the code it documents."""
import doctest
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "qa"))
import exposure  # noqa: E402
import soak  # noqa: E402


class DoctestTests(unittest.TestCase):
    def test_the_runners_examples_hold(self):
        result = doctest.testmod(soak)
        self.assertGreater(result.attempted, 10, "the examples were not found")
        self.assertEqual(result.failed, 0)

    def test_the_exposure_examples_hold(self):
        self.assertEqual(doctest.testmod(exposure).failed, 0)


if __name__ == "__main__":
    unittest.main()
