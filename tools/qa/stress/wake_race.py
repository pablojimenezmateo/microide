#!/usr/bin/env python3
"""The wake-race lane (safety-plan W6): SDL's lost cross-thread wake, contended.

    tools/qa/stress/wake_race.py [--binary build/microide/microide_sdl_wake_race]
                                 [--copies 12] [--rounds 40000] [--backends x11,wayland]

Runs `--copies` instances of the stress probe at once per backend (Xvfb; headless
weston when it is there), first WITH the app's LostWakeRelay -- every instance must
report zero stalls, or the lane fails -- then WITHOUT it, as an informational probe
of SDL's own behaviour: an SDL release that fixes TD-2026-11-07-433 shows up here as
zero stalls without the relay. Exit status: 1 when any relay run stalled or an
instance failed to run; a backend that is not installed is reported, not a pass of
it (`--require` makes it a failure).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "qa"))
import microide_qa as q  # noqa: E402

RESULT = re.compile(r"relay=(\d) rounds=(\d+) stalls=(\d+)")


def start_x11(work: Path) -> tuple[subprocess.Popen, dict[str, str]]:
    read_fd, write_fd = os.pipe()
    server = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "800x600x24", "-nolisten", "tcp"],
                              pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(write_fd)
    with os.fdopen(read_fd) as reader:
        number = reader.readline().strip()
    env = {**os.environ, "DISPLAY": f":{number}", "SDL_VIDEO_DRIVER": "x11"}
    env.pop("WAYLAND_DISPLAY", None)
    return server, env


def start_wayland(work: Path) -> tuple[subprocess.Popen, dict[str, str]] | None:
    weston = q.WESTON_ROOT / "usr" / "bin" / "weston"
    env = {**os.environ, "XDG_RUNTIME_DIR": str(work)}
    if weston.exists():
        lib = q.WESTON_ROOT / "usr" / "lib" / "x86_64-linux-gnu"
        env["LD_LIBRARY_PATH"] = f"{lib}:{lib / 'weston'}"
        env["WESTON_MODULE_MAP"] = ";".join(
            f"{module.name}={module}" for module in
            sorted(lib.glob("libweston-*/*.so")) + sorted((lib / "weston").glob("*.so")))
    elif shutil.which("weston"):
        weston = Path(shutil.which("weston"))
    else:
        return None
    os.chmod(work, 0o700)
    server = subprocess.Popen([str(weston), "--backend=headless", "--shell=kiosk", "--socket=wake-race",
                               "--no-config", "--width=800", "--height=600"],
                              env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    socket = work / "wake-race"
    for _ in range(100):
        if socket.exists() or server.poll() is not None:
            break
        subprocess.run(["sleep", "0.1"])
    if server.poll() is not None or not socket.exists():
        return None
    run_env = {**os.environ, "XDG_RUNTIME_DIR": str(work), "WAYLAND_DISPLAY": "wake-race",
               "SDL_VIDEO_DRIVER": "wayland"}
    run_env.pop("DISPLAY", None)
    return server, run_env


def run_copies(binary: Path, env: dict[str, str], relay: bool, copies: int, rounds: int) -> list[tuple[int, str]]:
    """(stalls, output) per copy; stalls -1 when the copy did not report."""
    def one(_: int) -> tuple[int, str]:
        # Generous: a round is microseconds unloaded, but a dozen copies beside a
        # build (load 40) took one instance past a rounds/100 s budget. A stall
        # cannot hang an instance -- the probe nudges itself after 2 s and says
        # so -- so a timeout is slowness; what it printed before it is still read.
        try:
            result = subprocess.run([str(binary), "1" if relay else "0", str(rounds)], env=env,
                                    capture_output=True, text=True, timeout=max(600, rounds // 10))
            output = result.stdout + result.stderr
        except subprocess.TimeoutExpired as expired:
            partial = (expired.stdout or b"")
            partial = partial.decode(errors="replace") if isinstance(partial, bytes) else partial
            stalls = partial.count("stall at round")
            return (stalls if stalls else -1), f"timed out after {stalls} stall(s): {partial[-300:]}"
        match = RESULT.search(output)
        return (int(match.group(3)) if match else -1), output[-400:]
    with concurrent.futures.ThreadPoolExecutor(max_workers=copies) as pool:
        return list(pool.map(one, range(copies)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=REPO / "build/microide/microide_sdl_wake_race")
    parser.add_argument("--copies", type=int, default=12)
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--backends", default="x11,wayland")
    parser.add_argument("--require", action="store_true", help="a missing backend fails the lane")
    args = parser.parse_args()
    if not args.binary.exists():
        print(f"wake-race: no probe at {args.binary} (cmake --build build --target microide_sdl_wake_race)")
        return 1
    failed = False
    for backend in args.backends.split(","):
        work = Path(tempfile.mkdtemp(prefix="wake-race-"))
        started = start_x11(work) if backend == "x11" else start_wayland(work)
        if started is None:
            print(f"wake-race: {backend}: not available" + (" -- required" if args.require else ", skipped"))
            failed |= args.require
            continue
        server, env = started
        try:
            for relay in (True, False):
                results = run_copies(args.binary, env, relay, args.copies, args.rounds)
                stalls = sum(s for s, _ in results if s > 0)
                broken = [out for s, out in results if s < 0]
                label = "with the relay" if relay else "without the relay (SDL alone, informational)"
                print(f"wake-race: {backend} {label}: {args.copies} x {args.rounds} rounds, {stalls} stall(s)"
                      + (f", {len(broken)} instance(s) did not report" if broken else ""), flush=True)
                for out in broken[:2]:
                    print(f"    {out.strip()}")
                if relay and (stalls or broken):
                    failed = True
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
            shutil.rmtree(work, ignore_errors=True)
    print(f"wake-race: {'FAILED' if failed else 'ok'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
