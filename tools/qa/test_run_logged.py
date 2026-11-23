"""run-checks.sh's run_logged: a lane's exit is the check's, never the logger's.

The logger is `gnutee` when it is on PATH; each case puts a fake one first. The
one that aborts mid-stream is what uutils' tee did under qa-remote: the check's
next write met a broken pipe, and a green lane exited 1 with its summary lost.
"""

import os
import pathlib
import subprocess
import tempfile
import unittest

RUN_CHECKS = pathlib.Path(__file__).resolve().parents[1] / "run-checks.sh"

# Prints a line, waits until the logger is surely gone, prints the rest, exits $1.
CHECK = ("import sys, time\n"
         "print('first', flush=True)\n"
         "time.sleep(0.5)\n"
         "for i in range(2000): print('line', i)\n"
         "print('last line', flush=True)\n"
         "sys.exit(int(sys.argv[1]))\n")

# Writes what it read to its file (the last argument), as tee does, then aborts.
ABORTING_TEE = '#!/bin/bash\nhead -c 1 >> "${@: -1}"\nkill -ABRT $$\n'


class RunLoggedTest(unittest.TestCase):
    def run_logged(self, tee_script, exit_code):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            bin_dir = tmp / "bin"
            bin_dir.mkdir()
            if tee_script is not None:
                fake = bin_dir / "gnutee"
                fake.write_text(tee_script)
                fake.chmod(0o755)
            (tmp / "check.py").write_text(CHECK)
            log = tmp / "lane.log"
            env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}", MICROIDE_LOG_DIR=str(tmp))
            script = (f'source "{RUN_CHECKS}"; '
                      f'run_logged "{log}" python3 "{tmp / "check.py"}" {exit_code}')
            done = subprocess.run(["bash", "-c", script], env=env, capture_output=True, text=True, timeout=60)
            return done.returncode, log.read_text(errors="replace"), done.stderr

    def test_a_logger_that_aborts_leaves_a_green_check_green(self):
        code, log, stderr = self.run_logged(ABORTING_TEE, 0)
        self.assertEqual(code, 0, stderr)
        self.assertIn("last line", log, "the rest of the output still reaches the log")
        self.assertIn("the logger", stderr, "and the lane says the logger died")

    def test_a_logger_that_aborts_leaves_a_failing_check_red(self):
        code, log, _ = self.run_logged(ABORTING_TEE, 3)
        self.assertEqual(code, 3)
        self.assertIn("last line", log)

    def test_the_real_logger_passes_the_status_and_the_output(self):
        for exit_code in (0, 3):
            code, log, _ = self.run_logged(None, exit_code)
            self.assertEqual(code, exit_code)
            self.assertIn("=== run-checks.sh:", log)
            self.assertIn("last line", log)


if __name__ == "__main__":
    unittest.main()
