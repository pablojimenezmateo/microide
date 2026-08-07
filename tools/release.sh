#!/usr/bin/env bash
#
# release.sh — drive a full microide release from a single version argument.
#
# Implements dev-docs/project/release-checklist.md end to end. LOCAL-ONLY by
# default: it bumps the version, builds, runs the test gate, regenerates the man
# page + showcase media, packages the signed .deb, and then STOPS, printing the
# staged artifacts. Passing --publish additionally commits, tags, pushes, and
# creates the GitHub release.
#
# Usage:
#   tools/release.sh <version> [--publish] [--skip-media] [--skip-tests]
#                    [--changelog-file <file>] [--notes <file>] [--yes]
#
# Examples:
#   tools/release.sh 2.4.1                 # local dry build + package + sign, no git
#   tools/release.sh 2.4.1 --publish       # the whole thing, including gh release
#   tools/release.sh 2.5.0 --publish --changelog-file notes.md
#                                          # inject a curated CHANGELOG body (no TODO draft)

set -euo pipefail
INVOKE_PWD="$PWD"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

PUBLISH=0; SKIP_MEDIA=0; SKIP_TESTS=0; ASSUME_YES=0; NOTES_FILE=""; CHANGELOG_FILE=""
VERSION=""
KEY_FPR="0E32 39B7 1B0F 9598 B71A FB7B 6D33 9CCB FC51 5D70"

log()  { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
info() { printf '   %s\n' "$*"; }
die()  { printf '\033[1;31mrelease: %s\033[0m\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --publish)    PUBLISH=1 ;;
    --skip-media) SKIP_MEDIA=1 ;;
    --skip-tests) SKIP_TESTS=1 ;;
    --yes|-y)     ASSUME_YES=1 ;;
    --notes)      NOTES_FILE="$2"; shift ;;
    --changelog-file) CHANGELOG_FILE="$2"; shift ;;
    -h|--help)    sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)           die "unknown flag '$1'" ;;
    *)            [[ -z "$VERSION" ]] && VERSION="$1" || die "unexpected arg '$1'" ;;
  esac
  shift
done

VERSION="${VERSION#v}"
[[ -n "$VERSION" ]] || die "usage: tools/release.sh <version> [--publish]"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "version must be X.Y.Z (got '$VERSION')"

if [[ -n "$CHANGELOG_FILE" ]]; then
  [[ "$CHANGELOG_FILE" == /* ]] || CHANGELOG_FILE="$INVOKE_PWD/$CHANGELOG_FILE"
  [[ -f "$CHANGELOG_FILE" ]] || die "changelog file '$CHANGELOG_FILE' not found"
fi

OLD_VERSION="$(grep -oP 'project\(microide VERSION \K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt)"
[[ -n "$OLD_VERSION" ]] || die "could not read current version from CMakeLists.txt"
TAG="v$VERSION"
DEB="microide_${VERSION}_amd64.deb"
BUILD_DIR="build"
DATE="$(date +%F)"

log "microide release $OLD_VERSION → $VERSION  ($([[ $PUBLISH == 1 ]] && echo PUBLISH || echo local-only))"

# --- guards ----------------------------------------------------------------
[[ "$VERSION" != "$OLD_VERSION" ]] || die "version $VERSION equals current; bump it"
if [[ $PUBLISH == 1 ]]; then
  [[ "$(git rev-parse --abbrev-ref HEAD)" == "main" ]] || die "--publish requires the 'main' branch"
  [[ -z "$(git status --porcelain)" ]] || die "--publish requires a clean working tree"
  git rev-parse "$TAG" >/dev/null 2>&1 && die "tag $TAG already exists"
  command -v gh >/dev/null || die "--publish needs the gh CLI"
fi

confirm() {
  [[ $ASSUME_YES == 1 ]] && return 0
  read -r -p "   $1 [y/N] " a; [[ "$a" == [yY] ]]
}

# --- 1. bump version -------------------------------------------------------
# Rewrite the version by PATTERN, not by the old literal, so a doc surface that
# has already drifted (e.g. a landing page frozen at an ancient tag) self-heals
# instead of being skipped by a s/OLD/NEW/ that no longer matches. tools/
# check-doc-versions.sh (step 3) then hard-asserts every public surface agrees.
log "1/9  Bump version in CMakeLists.txt + README.md + docs/index.html"
sed -i "s/project(microide VERSION ${OLD_VERSION}/project(microide VERSION ${VERSION}/" CMakeLists.txt
sed -i -E "s/Tagged \`v[0-9]+\.[0-9]+\.[0-9]+\`/Tagged \`v${VERSION}\`/g; \
           s/microide_[0-9]+\.[0-9]+\.[0-9]+_amd64\.deb/microide_${VERSION}_amd64.deb/g" README.md
sed -i -E "s/([Tt]agged v)[0-9]+\.[0-9]+\.[0-9]+/\1${VERSION}/g" docs/index.html
# Read the version back with the SAME anchored pattern OLD_VERSION uses, and
# assert it. The unanchored `grep 'VERSION \K[0-9.]+' | head -1` this replaced
# matched `cmake_minimum_required(VERSION 3.28)` on line 1, so the one line a
# reviewer reads to confirm the bump reported "CMakeLists: 3.28" for every
# release regardless of what was actually baked — including v2.9.0's. Confirming
# a value by re-reading it is only worth anything if the read can fail.
BAKED_CMAKE="$(grep -oP 'project\(microide VERSION \K[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt)"
[[ "$BAKED_CMAKE" == "$VERSION" ]] \
  || die "CMakeLists.txt reports '$BAKED_CMAKE' after the bump, expected '$VERSION'"
info "CMakeLists: $BAKED_CMAKE"

# --- 2. changelog draft ----------------------------------------------------
log "2/9  Draft CHANGELOG.md section"
PREV_TAG="$(git describe --tags --abbrev=0 2>/dev/null || echo '')"
{
  echo "## [$VERSION] - $DATE"
  echo
  if [[ -n "$CHANGELOG_FILE" ]]; then
    # Curated section body supplied by --changelog-file (header/date stay ours).
    cat "$CHANGELOG_FILE"
  else
    echo "<!-- TODO(release): summarize the cycle. Draft from commits since ${PREV_TAG:-the start}: -->"
    echo "### Changes"
    git log ${PREV_TAG:+$PREV_TAG..HEAD} --no-merges --pretty='- %s' | grep -vE '^- (release|chore): ' || true
  fi
  echo
} > "$REPO/.changelog-section.tmp"
# Insert the new section just before the most recent existing version block.
awk 'NR==FNR{sec=sec $0 ORS; next}
     !done && /^## \[/ { printf "%s", sec; done=1 }
     { print }' "$REPO/.changelog-section.tmp" CHANGELOG.md > CHANGELOG.md.new
mv CHANGELOG.md.new CHANGELOG.md
rm -f "$REPO/.changelog-section.tmp"
if [[ -n "$CHANGELOG_FILE" ]]; then
  info "inserted curated '## [$VERSION] - $DATE' from $CHANGELOG_FILE"
else
  info "added '## [$VERSION] - $DATE' (review & tighten the TODO before publishing)"
fi

# --- 3. verify doc/version consistency ------------------------------------
# Hard gate: every public surface (README status + verify examples, landing
# page banner, newest CHANGELOG entry) must state the version just baked into
# CMakeLists.txt. Runs AFTER the changelog draft so the new entry is in place.
# This is what stops the stated version from ever drifting again.
log "3/9  Verify doc version consistency"
bash tools/check-doc-versions.sh || die "docs still drift from $VERSION (see DRIFT lines above)"

# --- 4. build --------------------------------------------------------------
# Release ships with LTO (-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON): the perf
# gate already measures RelWithDebInfo+LTO, so cross-TU inlining in the hot
# editor/render paths (e.g. TextViewport <-> EditorViewRenderer) is part of the
# validated codegen. CMakeLists downgrades gracefully if the toolchain lacks IPO
# and auto-skips ld.lld under LTO. The trade-off is a slower final link.
log "4/9  Build (Release + LTO)"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON >/dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)"
BAKED="$("$BUILD_DIR/microide/microide" --version 2>/dev/null | grep -oP '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
[[ "$BAKED" == "$VERSION" ]] || die "built binary reports '$BAKED', expected '$VERSION'"
info "binary reports $BAKED"

# --- 5. test gate ----------------------------------------------------------
# Route through tools/run-checks.sh so the release gate produces the same
# deterministic /tmp/microide-release.log artifact agents/reviewers are told to
# inspect, instead of a bare ctest run. The `release` mode tests the already-built
# Release+LTO tree in $BUILD_DIR without reconfiguring it. (TD-2026-07-16-28.)
if [[ $SKIP_TESTS == 0 ]]; then
  log "5/9  Test gate (tools/run-checks.sh release)"
  MICROIDE_RELEASE_BUILD_DIR="$BUILD_DIR" bash tools/run-checks.sh release
else
  log "5/9  Test gate SKIPPED (--skip-tests)"
fi

# --- 6. man page + media ---------------------------------------------------
log "6/9  Regenerate man page + showcase media"
bash tools/gen-man.sh "$BUILD_DIR/microide/microide"
if [[ $SKIP_MEDIA == 0 ]]; then
  bash tools/capture-media.sh
else
  info "media SKIPPED (--skip-media) — remember the 'UI change ⇒ regenerate media' rule"
fi

# --- 7. package + checksum + sign -----------------------------------------
log "7/9  Package .deb + checksum + GPG sign"
( cd "$BUILD_DIR" && cpack -G DEB )
DEB_PATH="$(find "$BUILD_DIR" -maxdepth 1 -name "$DEB" -print -quit)"
[[ -n "$DEB_PATH" ]] || DEB_PATH="$(find "$BUILD_DIR" -maxdepth 1 -name 'microide_*_amd64.deb' -print -quit)"
[[ -n "$DEB_PATH" ]] || die "cpack did not produce a .deb"

# Install-and-launch the artifact before it is signed. Building, testing and
# signing a package says nothing about whether it starts on a machine that is not
# this one — every release through v2.8.0 passed all three and still could not
# load libSDL3.so.0 anywhere but the maintainer's box.
"$REPO/scripts/ci/verify-deb-runtime.sh" "$DEB_PATH" \
  || die "the built .deb does not start on a clean system — refusing to sign it"

cp "$DEB_PATH" "$REPO/$DEB"
( cd "$REPO" && sha256sum "$DEB" > "$DEB.sha256" )
info "checksum: $(cut -d' ' -f1 "$REPO/$DEB.sha256")"
if gpg --list-secret-keys "${KEY_FPR// /}" >/dev/null 2>&1; then
  gpg --detach-sign --armor --yes "$REPO/$DEB"
  gpg --detach-sign --armor --yes "$REPO/$DEB.sha256"
  ( cd "$REPO" && gpg --verify "$DEB.asc" "$DEB" )
  info "signed: $DEB.asc, $DEB.sha256.asc"
else
  info "WARNING: release key $KEY_FPR not in keyring — skipping signatures"
fi

# --- 8. stop here unless publishing ---------------------------------------
ARTIFACTS=("$DEB" "$DEB.sha256" "$DEB.asc" "$DEB.sha256.asc" "microide-signing-key.asc")
if [[ $PUBLISH == 0 ]]; then
  log "8/9  LOCAL-ONLY — staged, nothing pushed"
  info "Artifacts in $REPO:"
  for a in "${ARTIFACTS[@]}"; do [[ -e "$REPO/$a" ]] && printf '     %s\n' "$a"; done
  cat <<EOF

   Review the CHANGELOG TODO and the regenerated docs/media/, then either:
     • re-run with --publish to commit, tag, push, and create the GitHub release, or
     • finish by hand per dev-docs/project/release-checklist.md.
   Verify the package locally:  gpg --verify $DEB.asc $DEB
EOF
  exit 0
fi

# --- 9. publish ------------------------------------------------------------
log "9/9  Publish: commit, tag, push, GitHub release"
# Final gate before anything leaves the machine: re-verify doc/version
# consistency in case the CHANGELOG TODO or docs were hand-edited after step 3.
bash tools/check-doc-versions.sh || die "docs drift from $VERSION — refusing to publish"
git -C "$REPO" diff --stat
confirm "Commit version/changelog/man/media, tag $TAG, push, and create the GitHub release?" \
  || die "aborted before publish (local changes are staged on disk)"

git add -A
git commit -m "release: microide $TAG"
git tag -s "$TAG" -m "microide $TAG"
git push origin main
git push origin "$TAG"

NOTES_ARG=()
[[ -n "$NOTES_FILE" ]] && NOTES_ARG=(--notes-file "$NOTES_FILE") || NOTES_ARG=(--generate-notes)
gh release create "$TAG" "${NOTES_ARG[@]}" --title "microide $TAG"
gh release upload "$TAG" \
  "$REPO/$DEB" "$REPO/$DEB.sha256" "$REPO/$DEB.asc" "$REPO/$DEB.sha256.asc" \
  "$REPO/microide-signing-key.asc"
gh release view "$TAG" --json assets --jq '.assets[].name'

log "Done — $TAG published. Round-trip verify: gpg --verify $DEB.asc $DEB"
