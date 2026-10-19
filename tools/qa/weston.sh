#!/usr/bin/env bash
# Unpacks weston for the QA Wayland backend (MQA_BACKEND=wayland), as the current
# user: nothing is installed and no session is touched. microide_qa.App runs it
# headless, one compositor per run, on a socket under that run's own runtime dir.
#
#   tools/qa/weston.sh            # -> ~/.cache/microide-qa/weston/root/usr/bin/weston
#
# A weston already on PATH is used as is; this is for a machine without one.
set -euo pipefail

ROOT="${MQA_WESTON_ROOT:-$HOME/.cache/microide-qa/weston/root}"
DEB="$(dirname "$ROOT")/deb"

if [[ -x "$ROOT/usr/bin/weston" ]]; then
  echo "$ROOT/usr/bin/weston"
  exit 0
fi
command -v apt >/dev/null || { echo "weston.sh: no apt to fetch weston" >&2; exit 1; }
mkdir -p "$DEB"
# The headless backend needs weston, libweston and libseat; the RDP/VNC backends'
# libraries are left out (their modules are never loaded).
(cd "$DEB" && apt download weston "libweston-[0-9]*-0" libseat1 >/dev/null 2>&1) ||
  (cd "$DEB" && apt download weston libweston-13-0 libseat1 >/dev/null)
for deb in "$DEB"/*.deb; do
  dpkg -x "$deb" "$ROOT"
done
[[ -x "$ROOT/usr/bin/weston" ]] || { echo "weston.sh: the packages had no usr/bin/weston" >&2; exit 1; }
echo "$ROOT/usr/bin/weston"
