"""Regression checks for workflow shell boundaries without GitHub or builds."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SEEDS_EXPRESSION = "${{ inputs.seeds || '1-4' }}"


def asan_step() -> str:
    workflow = (ROOT / ".github/workflows/qa-deep.yml").read_text()
    match = re.search(
        r"^      - name: Run the GUI QA suite and the monkey on the ASan build\n"
        r"(.*?)(?=^      - |\Z)", workflow, re.MULTILINE | re.DOTALL)
    if match is None:
        raise AssertionError("the ASan QA step disappeared; update the positive-control fixture")
    return match.group(1)


class DispatchInputTests(unittest.TestCase):
    def test_seeds_expression_is_environment_data(self):
        step = asan_step()
        env, script = step.split("        run: |\n", 1)
        self.assertIn(f"MICROIDE_QA_ASAN_SEEDS: {SEEDS_EXPRESSION}", env)
        self.assertNotIn("${{", script, "dispatch inputs must not be interpolated into shell source")

    def test_shell_metacharacters_reach_wrapper_literally(self):
        script = asan_step().split("        run: |\n", 1)[1]
        script = "\n".join(line[10:] if line.startswith(" " * 10) else line
                           for line in script.splitlines())
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            (work / "tools").mkdir()
            wrapper = work / "tools/run-checks.sh"
            wrapper.write_text('#!/bin/sh\nset -eu\n[ "$1" = qa-asan ]\n'
                               'printf "%s" "$MICROIDE_QA_ASAN_SEEDS" > received\n')
            wrapper.chmod(0o755)
            # A dispatch runner interpolates expressions before starting bash.
            # This input executes commands if embedded inside double quotes.
            payload = '1-4"; touch injected; # $(touch substituted) `touch backtick`'
            script = script.replace("${{ github.event.inputs.seeds || '1-4' }}", payload)
            script = script.replace(SEEDS_EXPRESSION, payload)
            result = subprocess.run(["bash", "-eu", "-c", script], cwd=work,
                                    env={**os.environ, "MICROIDE_QA_ASAN_SEEDS": payload},
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((work / "received").read_text(), payload)
            self.assertFalse((work / "injected").exists())
            self.assertFalse((work / "substituted").exists())
            self.assertFalse((work / "backtick").exists())


if __name__ == "__main__":
    unittest.main()
