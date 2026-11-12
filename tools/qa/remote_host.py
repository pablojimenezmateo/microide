"""A loopback ssh host of a run's own, for the monkey's `remote` profile.

The qa-remote lane's host (tools/qa/local-sshd.sh with its defaults) is one sshd
that every `start` restarts and whose installed server every `start` drops, so a
soak run sharing it would have its link cut by the next lane, and the lane its
host repository edited by the run. A run gets its own: an sshd on a free port
with its state (keys, config, the sandbox HOME the host server installs into) in
a short directory of its own, sharing only the unpacked openssh-server package.

The host is this machine, so the host-side project is a local directory: the
monkey changes it directly as "the outside world" of a remote project, and the
convergence oracle reads both trees without an ssh round trip.
"""
from __future__ import annotations

import hashlib
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "local-sshd.sh"
SHARED_ROOT = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "microide-qa" / "sshd" / "root"
HOST_NAME = "qa-local"
# What a fault can be: the ssh link (its ControlMaster), the host's server, or nothing.
FAULTS = ("link", "server")


SCRATCH_PREFIX = "mqa-ssh-"


def sweep_orphans() -> int:
    """Stops the hosts whose driver is gone. sshd daemonizes, so it cannot die
    with the driver the way a child can (App._dies_with_driver): a driver killed
    by its caller -- a soak job past its timeout -- left its sshd and the host's
    server running. Each host records its driver's pid; the next one to start
    stops every host whose driver no longer runs. Returns how many it stopped."""
    stopped = 0
    for scratch in Path("/tmp").glob(SCRATCH_PREFIX + "*"):
        try:
            pid = int((scratch / "driver.pid").read_text())
            os.kill(pid, 0)
            continue  # its driver still runs
        except PermissionError:
            continue  # it runs, as someone else
        except ProcessLookupError:
            pass
        except (OSError, ValueError):
            # No pid yet: a host being made right now, or one from before pid
            # files. Only an old one is an orphan.
            try:
                if time.time() - scratch.stat().st_mtime < 600:
                    continue
            except OSError:
                continue
        LoopbackHost._kill_matching(str(scratch))
        shutil.rmtree(scratch, ignore_errors=True)
        stopped += 1
    return stopped


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def _fingerprint(path: Path) -> str | None:
    """What reading the path gives, for comparing two trees: the sha256 of the
    regular file it is or links to; None for anything else (a dangling link, a
    link to a directory, a file that went away).

    Through links, because the two sides need not hold the same KIND of entry:
    a host link whose target is outside the root, or named by an absolute path,
    reaches the mirror as a regular file holding the target's content, and a
    dangling or directory link as nothing to read (the design's stated symlink
    limitation, dev-docs/design/remote-projects.md § 6.2)."""
    try:
        if path.is_file():  # follows links
            return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        pass
    return None


def _git_paths(root: Path, *args: str, stdin: str | None = None) -> set[str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    result = subprocess.run(["git", "-C", str(root), *args], input=stdin, capture_output=True, text=True,
                            env=env, timeout=60)
    return {p for p in result.stdout.split("\0") if p}


def content_set(host_root: Path, mirror: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Both sides of a remote project as the sync defines it
    (dev-docs/design/remote-projects.md, "Content set, decided on the server"):
    the host's tracked and untracked-but-not-ignored files, and every mirror file
    the host's ignore rules would not exclude (one created in the editor is owed
    to the host; an ignored one never syncs). {relative path: fingerprint}."""
    host_paths = _git_paths(host_root, "ls-files", "--cached", "--others", "--exclude-standard", "-z")
    mirror_paths = {path.relative_to(mirror).as_posix() for path in mirror.rglob("*")
                    if (path.is_file() or path.is_symlink())
                    and path.relative_to(mirror).parts[0] != ".git"}
    only_mirror = sorted(mirror_paths - host_paths)
    ignored = _git_paths(host_root, "check-ignore", "--stdin", "-z",
                         stdin="\0".join(only_mirror) + "\0") if only_mirror else set()
    host = {p: f for p in host_paths if (f := _fingerprint(host_root / p)) is not None}
    # A host file that is there but cannot be read (mode 000, the outside world's
    # chmod): the sync reports it unreadable and leaves the mirror's last copy
    # alone, so neither side is compared (soak 37ec61829b08).
    unreadable = {p for p in host_paths if p not in host and (host_root / p).is_file()
                  and not os.access(host_root / p, os.R_OK)}
    mirror_side = {p: f for p in mirror_paths - ignored - unreadable if (f := _fingerprint(mirror / p)) is not None}
    return host, mirror_side


class LoopbackHost:
    """Start with `start()`, end with `stop()` (or use it as a context manager)."""

    def __init__(self, fixture: Path, host_parent: Path):
        sweep_orphans()
        # Short: the host server's sockets live under the sandbox HOME, and an
        # AF_UNIX path stops at 108 bytes -- a soak run's directory is ~100 long.
        # One level down: local-sshd.sh makes the state's parent private too.
        self.scratch = Path(tempfile.mkdtemp(prefix=SCRATCH_PREFIX, dir="/tmp"))
        (self.scratch / "driver.pid").write_text(str(os.getpid()))
        self.state = self.scratch / "s"
        self.port = free_port()
        self.ssh_config = self.state / "ssh_config"
        self.home = self.state / "hosthome"
        host_parent.mkdir(parents=True, exist_ok=True)
        self.root = host_parent.resolve() / fixture.name
        shutil.copytree(fixture, self.root, symlinks=True)
        self.name = HOST_NAME

    def _env(self) -> dict[str, str]:
        return {**os.environ, "MQA_SSHD_DIR": str(self.state), "MQA_SSHD_PORT": str(self.port),
                "MQA_SSHD_ROOT": str(SHARED_ROOT), "MQA_SSHD_NO_REPO": "1"}

    def start(self) -> None:
        result = subprocess.run(["bash", str(SCRIPT), "start"], env=self._env(), capture_output=True,
                                text=True, timeout=120)
        if result.returncode != 0:
            raise RuntimeError(f"the loopback host did not start: {result.stderr.strip()[-400:]}")

    def stop(self) -> None:
        subprocess.run(["bash", str(SCRIPT), "stop"], env=self._env(), capture_output=True, timeout=60)
        self._kill_matching(str(self.state))
        shutil.rmtree(self.scratch, ignore_errors=True)

    def __enter__(self) -> "LoopbackHost":
        self.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.stop()

    def launch_args(self) -> list[str]:
        """The app's arguments that make `qa-local` this host."""
        return ["--set", "remote.ssh_command", f"ssh -F {self.ssh_config}"]

    def open_line(self) -> str:
        return f"remote-open {self.name}:{self.root}"

    def fault(self, kind: str) -> int:
        """Cuts the link or kills the host's server; how many processes it killed."""
        if kind == "link":
            # The editor's ssh processes for this host: an ssh whose command line
            # names this host's config file, which nothing else on the machine
            # does -- but the app's own does too (its remote.ssh_command setting),
            # so only a process that IS ssh.
            return self._kill_matching(str(self.ssh_config), program="ssh")
        if kind == "server":
            return self._kill_matching(str(self.home / ".local" / "share" / "microide" / "server"))
        return 0

    @staticmethod
    def _kill_matching(needle: str, program: str | None = None) -> int:
        killed = 0
        own = os.getpid()
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit() or int(entry.name) == own:
                continue
            try:
                argv = (entry / "cmdline").read_bytes().split(b"\0")
            except OSError:
                continue
            if program is not None and os.path.basename(argv[0].decode(errors="replace")) != program:
                continue
            if needle in b" ".join(argv).decode(errors="replace"):
                try:
                    os.kill(int(entry.name), signal.SIGKILL)
                    killed += 1
                except (ProcessLookupError, PermissionError):
                    pass
        return killed
