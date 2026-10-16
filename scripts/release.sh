#!/bin/bash
# Publishes a new LiveDeck version:
#   1. write what changed under "## Unreleased" in CHANGELOG.md
#   2. ./scripts/release.sh 0.3.1
# This sets the version in CMakeLists.txt, turns "Unreleased" into "0.3.1 - <date>",
# commits, tags v0.3.1 and pushes. GitHub Actions then builds, tests and publishes the
# release, and every installed LiveDeck offers the update.
set -euo pipefail
cd "$(dirname "$0")/.."

V="${1:-}"
if ! [[ "$V" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Usage: ./scripts/release.sh <major.minor.patch>   (e.g. 0.3.1)"; exit 1
fi

CUR=$(sed -n 's/^project(LiveDeck VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
if [ "$(printf '%s\n%s\n' "$CUR" "$V" | sort -V | tail -1)" != "$V" ] || [ "$CUR" = "$V" ]; then
  # allow re-using the current version only for the very first release (no tag yet)
  if [ "$CUR" != "$V" ] || git rev-parse -q --verify "refs/tags/v$V" >/dev/null; then
    echo "Version $V must be newer than the current $CUR."; exit 1
  fi
fi

git remote get-url origin >/dev/null 2>&1 || { echo "No 'origin' remote. Push the repo to GitHub first (see README)."; exit 1; }
if [ -n "$(git status --porcelain)" ]; then
  echo "You have uncommitted changes. Commit or stash them first:"; git status --short; exit 1
fi
BRANCH=$(git rev-parse --abbrev-ref HEAD)

if [ "$V" = "$CUR" ] && grep -q "^## $V " CHANGELOG.md; then
  # First release of a version already written into CMakeLists.txt and CHANGELOG.md: just tag it.
  echo "Publishing the existing version $V."
else
  if ! awk '/^## Unreleased/{f=1;next} /^## /{f=0} f && /[^[:space:]-]/{found=1} END{exit !found}' CHANGELOG.md; then
    echo "CHANGELOG.md has nothing under '## Unreleased'. Describe the changes there first."; exit 1
  fi
  DATE=$(date +%Y-%m-%d)
  perl -pi -e "s/^project\(LiveDeck VERSION [0-9.]+/project(LiveDeck VERSION $V/" CMakeLists.txt
  perl -0pi -e "s/## Unreleased\n/## Unreleased\n\n## $V - $DATE\n/" CHANGELOG.md
  git add CMakeLists.txt CHANGELOG.md
  git commit -m "Release $V"
fi
git tag -a "v$V" -m "LiveDeck $V"
git push origin "$BRANCH"
git push origin "v$V"

REPO=$(git remote get-url origin | sed -E 's#.*github\.com[:/]([^/]+/[^/]+)$#\1#; s#\.git$##')
echo
echo "Tagged v$V. GitHub is building the release now (about 20 minutes):"
echo "  https://github.com/$REPO/actions"
echo "When it finishes, LiveDeck shows the update under Help > Check for Updates."
