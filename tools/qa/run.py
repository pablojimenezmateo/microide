#!/usr/bin/env python3
"""tools/qa — drive the real microide window and check it. One CLI for every mode.

    tools/qa/run.py suite [NAME ...]                 the local scenario suite (default: all)
    tools/qa/run.py monkey --seed N --steps K        the seeded monkey (default profile)
    tools/qa/run.py monkey --profile commands --seeds 1-8 [--rounds 400] [--project PATH]
                                                     the former control-command fuzzer
    tools/qa/run.py replay FINDING/replay.json       re-run a saved action list
    tools/qa/run.py invariants [--only VERB] [--soft-wrap]
                                                     algebraic invariants of every editing command
    tools/qa/run.py remote [SCENARIO ...]            the remote suite (MQA_REMOTE_HOST)

Common options: --binary PATH (default build/microide/microide, or $MICROIDE_BIN),
--out DIR (default /tmp/mqa-out-<time>). Every run is on a private Xvfb display with
its own XDG tree and a fresh fixture copy, with the redraw verifier and the frame
trace on; any finding is written as a directory under --out (the actions up to it,
the frame lines since the previous action, the last screenshots, the window now, and
replay.json) and makes the exit status non-zero. See tools/qa/README.md.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import microide_qa as q  # noqa: E402
import monkey as m  # noqa: E402
import scenarios as s  # noqa: E402
import invariants as inv  # noqa: E402

REQUIRED_TOOLS = ("Xvfb", "xdotool", "xwd", "xdpyinfo")


def require_tools() -> None:
    missing = [tool for tool in REQUIRED_TOOLS if shutil.which(tool) is None]
    if missing:
        raise SystemExit(f"tools/qa needs {', '.join(missing)} on PATH (apt install xvfb "
                         f"xdotool x11-apps x11-utils); not running is not a pass")


def fixture(kind: str, scratch: Path) -> Path:
    """A fresh copy per run, so a seed means the same thing twice."""
    scratch.mkdir(parents=True, exist_ok=True)
    if kind == "taskflow":
        return q.make_fixture(Path(tempfile.mkdtemp(prefix="fx-", dir=scratch)) / "taskflow")
    destination = Path(tempfile.mkdtemp(prefix="fx-", dir=scratch)) / "project"
    m.copy_project(Path(kind), destination)
    return destination


def report(name: str, app: q.App) -> int:
    if not app.findings:
        print(f"  ok    {name}")
        return 0
    print(f"  FAIL  {name}: {len(app.findings)} finding(s)")
    for finding in app.findings[:5]:
        print(f"        {finding.kind}: {finding.detail[:240]}")
        if finding.screenshot:
            print(f"        evidence: {Path(finding.screenshot).parent}")
    return 1


def cmd_suite(args: argparse.Namespace) -> int:
    names = args.names or list(s.SCENARIOS)
    unknown = [name for name in names if name not in s.SCENARIOS]
    if unknown:
        raise SystemExit(f"unknown scenario(s): {', '.join(unknown)}; known: {', '.join(s.SCENARIOS)}")
    failures = 0
    print(f"suite: {len(names)} scenario(s) -> {args.out}")
    for name in names:
        project = fixture("taskflow", args.out / "fixtures")
        with q.App.launch(project=project, binary=args.binary, shots=args.out / name) as app:
            app.replay_header = {"fixture": "taskflow", "scenario": name}
            app.check("start")
            try:
                s.SCENARIOS[name](app)
            except q.QaFailure as failure:
                app.expect(False, f"{name}: {failure}")
            app.check(f"end of {name}")
            failures += report(name, app)
    print(f"suite: {len(names) - failures}/{len(names)} clean")
    return 1 if failures else 0


def cmd_monkey(args: argparse.Namespace) -> int:
    start, _, end = args.seeds.partition("-") if args.seeds else (str(args.seed), "", "")
    seeds = range(int(start), int(end or start) + 1)
    command_profile = args.profile in ("commands", "editor")
    kind = str(args.project) if args.project else ("taskflow" if not command_profile else str(q.REPO))
    steps = args.rounds if command_profile and args.rounds else args.steps
    failures = 0
    print(f"monkey: profile {args.profile}, seeds {seeds.start}-{seeds.stop - 1}, "
          f"{steps} steps -> {args.out}")
    for seed in seeds:
        project = fixture(kind, args.out / "fixtures")
        with q.App.launch(project=project, binary=args.binary,
                          shots=args.out / f"seed-{seed}") as app:
            app.replay_header = {"fixture": kind, "profile": args.profile, "seed": seed}
            app.check("start")
            monkey = m.Monkey(app, seed, args.profile, project)
            if command_profile and not args.no_churn:
                with m.ExternalChurn(project, m.random.Random(seed ^ 0x5F5F)):
                    monkey.run(steps, settle=args.delay)
            else:
                monkey.run(steps, settle=args.delay if command_profile else 0.12)
            if app.alive() and command_profile:
                # The fuzzer's last word: the shell still answers.
                app.wait_until(lambda: bool(app.query("status")), 45.0, "a status answer")
            failures += report(f"seed {seed}", app)
            if app.findings:
                app.save_actions(args.out / f"seed-{seed}" / "replay.json", **app.replay_header)
    print(f"monkey: {len(seeds) - failures}/{len(seeds)} seeds clean")
    return 1 if failures else 0


def cmd_replay(args: argparse.Namespace) -> int:
    record = json.loads(Path(args.file).read_text())
    actions = record["actions"] if isinstance(record, dict) else record
    kind = record.get("fixture", "taskflow") if isinstance(record, dict) else "taskflow"
    project = fixture(kind, args.out / "fixtures")
    size = tuple(record.get("size", (1440, 900))) if isinstance(record, dict) else (1440, 900)
    with q.App.launch(project=project, binary=args.binary, shots=args.out / "replay",
                      size=size) as app:
        app.replay_header = {"fixture": kind, "replay_of": str(args.file)}
        app.check("start")
        q.replay(app, actions)
        return report(f"replay of {args.file}", app)


def cmd_remote(args: argparse.Namespace) -> int:
    import remote  # noqa: E402
    sys.argv = [str(Path(remote.__file__)), *args.names]
    return remote.main()


def cmd_invariants(args: argparse.Namespace) -> int:
    failing = inv.run(args.binary, only=args.only, soft_wrap=args.soft_wrap,
                      verbose=args.verbose, keep=args.keep)
    return 1 if failing else 0


def main() -> int:
    # --binary and --out are accepted before or after the mode: the subparsers
    # take them with SUPPRESS defaults, so a value given after the mode wins and
    # an absent one leaves the top-level default alone.
    def common(defaults: bool) -> argparse.ArgumentParser:
        p = argparse.ArgumentParser(add_help=False)
        p.add_argument("--binary", type=Path, default=(
            Path(os.environ.get("MICROIDE_BIN", q.REPO / "build/microide/microide"))
            if defaults else argparse.SUPPRESS))
        p.add_argument("--out", type=Path, default=(
            Path(f"/tmp/mqa-out-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}")
            if defaults else argparse.SUPPRESS))
        return p

    parser = argparse.ArgumentParser(description=__doc__, parents=[common(True)],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    suite = sub.add_parser("suite", parents=[common(False)])
    suite.add_argument("names", nargs="*")
    monkey = sub.add_parser("monkey", parents=[common(False)])
    monkey.add_argument("--seed", type=int, default=1)
    monkey.add_argument("--seeds", help="a range like 1-8 (overrides --seed)")
    monkey.add_argument("--steps", type=int, default=300)
    monkey.add_argument("--rounds", type=int, help="commands per seed (commands/editor profiles)")
    monkey.add_argument("--profile", choices=("default", "commands", "editor"), default="default")
    monkey.add_argument("--project", type=Path,
                        help="project to copy and drive (default: the taskflow fixture; "
                             "this repository for the commands/editor profiles)")
    monkey.add_argument("--delay", type=float, default=0.05,
                        help="seconds between commands in the command profiles, so a frame "
                             "is painted between them")
    monkey.add_argument("--no-churn", action="store_true",
                        help="commands/editor profiles: do not rewrite files under the editor")
    replay = sub.add_parser("replay", parents=[common(False)])
    replay.add_argument("file")
    invariants = sub.add_parser("invariants", parents=[common(False)])
    invariants.add_argument("--only", help="restrict to commands containing this substring")
    invariants.add_argument("--soft-wrap", action="store_true",
                            help="run the whole matrix with word wrap ON")
    invariants.add_argument("--verbose", action="store_true",
                            help="list every probe in which the command changed nothing")
    invariants.add_argument("--keep", action="store_true", help="keep the scratch project")
    remote = sub.add_parser("remote", parents=[common(False)])
    remote.add_argument("names", nargs="*")
    args = parser.parse_args()

    if args.mode != "remote":
        require_tools()
        if not args.binary.exists():
            raise SystemExit(f"no microide binary at {args.binary} (build it or set MICROIDE_BIN)")
        args.out.mkdir(parents=True, exist_ok=True)
    handler = {"suite": cmd_suite, "monkey": cmd_monkey, "replay": cmd_replay,
               "invariants": cmd_invariants, "remote": cmd_remote}[args.mode]
    code = handler(args)
    if args.mode != "remote":
        shutil.rmtree(args.out / "fixtures", ignore_errors=True)
        print(f"output: {args.out}")
    return code


if __name__ == "__main__":
    sys.exit(main())
