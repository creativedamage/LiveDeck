#!/bin/bash
# One-time setup: puts LiveDeck on GitHub (public repo) and publishes the first release.
#   ./scripts/setup-github.sh            # repo named "LiveDeck" on your GitHub account
#   ./scripts/setup-github.sh MyName     # or another repo name
set -euo pipefail
cd "$(dirname "$0")/.."
REPO_NAME="${1:-LiveDeck}"

command -v git >/dev/null || { echo "git is missing: run  xcode-select --install"; exit 1; }
command -v gh  >/dev/null || { echo "GitHub CLI is missing: run  brew install gh"; exit 1; }

if ! gh auth status >/dev/null 2>&1; then
  echo "==> Sign in to GitHub (a browser window opens)"
  gh auth login --web --git-protocol https
fi
gh auth setup-git >/dev/null 2>&1 || true

# The GitHub Actions workflows arrive in github-workflows/ and belong in .github/workflows/.
if [ -d github-workflows ]; then
  mkdir -p .github/workflows
  mv -f github-workflows/*.yml .github/workflows/
  rmdir github-workflows
fi

[ -d .git ] || git init -q -b main
if [ -z "$(git config user.name || true)" ]; then
  git config user.name "$(gh api user -q '.name // .login')"
  git config user.email "$(gh api user -q '.id')+$(gh api user -q '.login')@users.noreply.github.com"
fi

git add -A
git commit -q -m "LiveDeck source" || echo "(nothing new to commit)"

if git remote get-url origin >/dev/null 2>&1; then
  git push -u origin main
else
  echo "==> Creating the public GitHub repository $REPO_NAME"
  gh repo create "$REPO_NAME" --public --source=. --remote=origin --push \
     --description "LiveDeck: multitrack recorder and virtual-soundcheck player for live events (macOS)"
fi

OWNER_REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner)
VERSION=$(sed -n 's/^project(LiveDeck VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
echo
echo "Repository: https://github.com/$OWNER_REPO"
echo "Builds:     https://github.com/$OWNER_REPO/actions"
echo
read -r -p "Publish the first release (v$VERSION) now? [y/N] " answer
if [[ "$answer" =~ ^[Yy] ]]; then
  ./scripts/release.sh "$VERSION"
else
  echo "Later, run:  ./scripts/release.sh $VERSION"
fi
