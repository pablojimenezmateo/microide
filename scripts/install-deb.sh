#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build/package-deb}"

if ! compgen -G "${BUILD_DIR}/*.deb" > /dev/null; then
  "$(dirname "$0")/package-deb.sh"
fi

package_path="$(find "${BUILD_DIR}" -maxdepth 1 -type f -name '*.deb' | sort | tail -n 1)"
sudo apt install -y "${package_path}"
