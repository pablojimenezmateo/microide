#!/usr/bin/env bash
# A loopback-only ssh host for tools/qa/remote.py, as the current user.
#
#   tools/qa/local-sshd.sh start   # prints the MQA_* variables to export
#   tools/qa/local-sshd.sh stop
#
# Nothing is installed and no system service runs: `sshd` comes out of the
# distribution's openssh-server package unpacked into the state directory, listens
# on 127.0.0.1 only, accepts one key generated here, and gives its sessions a HOME
# of their own -- so the microide-server the editor installs, its sockets and the
# host repository all live under the state directory, not in the real HOME. The
# host repository is a clone of this checkout at its HEAD.
set -euo pipefail
umask 077

STATE="${MQA_SSHD_DIR:-$HOME/.cache/microide-qa/sshd}"
PORT="${MQA_SSHD_PORT:-22220}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SSHD="$STATE/root/usr/sbin/sshd"
HOST_HOME="$STATE/hosthome"
HOST_REPO="$HOST_HOME/repo"

die() { echo "local-sshd: $*" >&2; exit 1; }

unpack_sshd() {
  [[ -x "$SSHD" ]] && return
  command -v apt >/dev/null || die "no sshd under $STATE and no apt to fetch openssh-server"
  mkdir -p "$STATE/deb"
  (cd "$STATE/deb" && apt download openssh-server >/dev/null 2>&1) || die "apt download openssh-server failed"
  dpkg -x "$STATE"/deb/openssh-server_*.deb "$STATE/root"
  [[ -x "$SSHD" ]] || die "the package had no usr/sbin/sshd"
}

write_config() {
  # The server refuses a socket directory under anything group- or other-writable
  # (whatever this machine's umask made these), as it should on a real host.
  mkdir -p "$STATE" "$HOST_HOME"
  chmod 700 "$(dirname "$STATE")" "$STATE" "$HOST_HOME"
  [[ -f "$STATE/host_ed25519" ]] || ssh-keygen -q -t ed25519 -N '' -f "$STATE/host_ed25519"
  [[ -f "$STATE/client_ed25519" ]] || ssh-keygen -q -t ed25519 -N '' -f "$STATE/client_ed25519"
  cp "$STATE/client_ed25519.pub" "$STATE/authorized_keys"
  chmod 600 "$STATE/authorized_keys"
  mkdir -p "$HOST_HOME"
  cat > "$STATE/sshd_config" <<EOF
ListenAddress 127.0.0.1
Port $PORT
HostKey $STATE/host_ed25519
AuthorizedKeysFile $STATE/authorized_keys
AllowUsers $USER
PasswordAuthentication no
KbdInteractiveAuthentication no
PubkeyAuthentication yes
StrictModes no
UsePAM no
PidFile $STATE/sshd.pid
SetEnv HOME=$HOST_HOME
EOF
  cat > "$STATE/ssh_config" <<EOF
# The same host behind a far link: 60 ms each way (a 120 ms round trip), so the
# remote suite exercises what only a slow link reaches (keystroke prediction).
Host qa-slow
  ProxyCommand python3 $REPO/tools/qa/slow-link.py --delay-ms \${MQA_SLOW_LINK_DELAY_MS:-60} 127.0.0.1 $PORT
  User $USER
  IdentityFile $STATE/client_ed25519
  IdentitiesOnly yes
  UserKnownHostsFile $STATE/known_hosts
  StrictHostKeyChecking accept-new
  HostKeyAlias qa-local
  BatchMode yes

Host qa-local
  HostName 127.0.0.1
  Port $PORT
  User $USER
  IdentityFile $STATE/client_ed25519
  IdentitiesOnly yes
  UserKnownHostsFile $STATE/known_hosts
  StrictHostKeyChecking accept-new
  BatchMode yes
EOF
  # Commits the host-side scenarios make (a branch switch) need an identity.
  cat > "$HOST_HOME/.gitconfig" <<EOF
[user]
  name = microide qa
  email = qa@localhost
EOF
}

host_repo() {
  if [[ ! -d "$HOST_REPO/.git" ]]; then
    git clone -q "$REPO" "$HOST_REPO"
  fi
  git -C "$HOST_REPO" fetch -q "$REPO" HEAD
  # Forced: the host repository is disposable, and an aborted run leaves it dirty
  # (an agent's edits, a branch switch) -- a plain checkout then refused, the
  # start failed, and the remote lane ran nothing.
  git -C "$HOST_REPO" checkout -q -f --detach FETCH_HEAD
  git -C "$HOST_REPO" reset -q --hard FETCH_HEAD
  git -C "$HOST_REPO" clean -qfdx
}

stop() {
  if [[ -f "$STATE/sshd.pid" ]] && kill -0 "$(cat "$STATE/sshd.pid")" 2>/dev/null; then
    # The editor's host server (installed under the sandbox HOME) goes first.
    HOME="$HOST_HOME" "$HOST_HOME/.local/share/microide/server/microide-server" stop >/dev/null 2>&1 || true
    kill "$(cat "$STATE/sshd.pid")"
  fi
  rm -f "$STATE/sshd.pid"
}

# The host's installed microide-server is part of what a run tests, so every start
# drops it (stop() has stopped it) and the first connect installs the one built for
# this run. The editor only installs over a MISSING or incompatible server, and a
# dev build keeps its release string, so a kept install never updates: the lane ran
# a 2026-10-22 server for days, and the write answers' manifest id that
# TD-2026-10-22-385's fix depends on never reached it (the J stall came back).
fresh_server() {
  rm -rf "$HOST_HOME/.local/share/microide/server"
}

case "${1:-}" in
  start)
    unpack_sshd
    write_config
    stop
    fresh_server
    "$SSHD" -t -f "$STATE/sshd_config" || die "sshd rejected its config"
    "$SSHD" -f "$STATE/sshd_config" -E "$STATE/sshd.log"
    for _ in $(seq 1 50); do
      ssh -F "$STATE/ssh_config" qa-local true 2>/dev/null && break
      sleep 0.1
    done
    ssh -F "$STATE/ssh_config" qa-local true || die "cannot log in (see $STATE/sshd.log)"
    host_repo
    echo "export MQA_REMOTE_HOST=qa-local MQA_REMOTE_ROOT=$HOST_REPO MQA_SSH_CONFIG=$STATE/ssh_config"
    ;;
  stop)
    stop
    ;;
  *)
    echo "usage: $0 start|stop" >&2
    exit 2
    ;;
esac
