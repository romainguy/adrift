#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build the Adrift static website (build/web/) and publish it to the gh-pages branch
# directly via a temporary Git index (leaving your main branch & working tree untouched).
#
# Usage:
#   ./tools/deploy_gh_pages.sh                 # Build site and push to origin/gh-pages
#   ./tools/deploy_gh_pages.sh --rebuild-fw    # Force fresh firmware + screenshot rebuild first
#   ./tools/deploy_gh_pages.sh --skip-build    # Publish existing build/web/ without rebuilding
#   ./tools/deploy_gh_pages.sh --no-push       # Update local gh-pages branch without pushing

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

REMOTE="origin"
BRANCH="gh-pages"
REBUILD_FW=0
SKIP_BUILD=0
NO_PUSH=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --rebuild-fw)
            REBUILD_FW=1
            ;;
        --skip-build)
            SKIP_BUILD=1
            ;;
        --no-push|--dry-run)
            NO_PUSH=1
            ;;
        --remote)
            shift
            REMOTE="$1"
            ;;
        --branch)
            shift
            BRANCH="$1"
            ;;
        -h|--help)
            sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "ERROR: Unknown option '$1' (try --help)" >&2
            exit 1
            ;;
    esac
    shift
done

if [ "$SKIP_BUILD" -eq 0 ]; then
    if [ "$REBUILD_FW" -eq 1 ]; then
        "$ROOT/build.sh" web --rebuild-fw
    else
        "$ROOT/build.sh" web
    fi
fi

DIST="$ROOT/build/web"
for req in \
    index.html \
    .nojekyll \
    adrift.wasm \
    adrift.fwsc \
    version.json \
    css/style.css \
    js/app.js \
    js/emulator.js \
    js/worklet.js \
    js/panel.js \
    js/flasher.js \
    assets/screens/01_home_scope.png
do
    if [ ! -f "$DIST/$req" ]; then
        echo "ERROR: Missing expected build artifact: $DIST/$req" >&2
        exit 1
    fi
done

MAIN_SHA="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_DIR_ABS="$(git -C "$ROOT" rev-parse --absolute-git-dir)"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/adrift-gh-pages.XXXXXX")"
TMP_INDEX="$TMP_DIR/index"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

if [ "$NO_PUSH" -eq 0 ]; then
    echo "==> Fetching latest $REMOTE/$BRANCH (if present)..."
    git -C "$ROOT" fetch "$REMOTE" "$BRANCH" >/dev/null 2>&1 || true
fi

PARENT_SHA=""
if git -C "$ROOT" show-ref --verify --quiet "refs/remotes/$REMOTE/$BRANCH"; then
    PARENT_SHA="$(git -C "$ROOT" rev-parse "refs/remotes/$REMOTE/$BRANCH")"
elif git -C "$ROOT" show-ref --verify --quiet "refs/heads/$BRANCH"; then
    PARENT_SHA="$(git -C "$ROOT" rev-parse "refs/heads/$BRANCH")"
fi

# Stage every file in build/web/ into an isolated temporary index (-f overrides any global gitignore)
GIT_DIR="$GIT_DIR_ABS" GIT_WORK_TREE="$DIST" GIT_INDEX_FILE="$TMP_INDEX" \
    git add -f -A -- .

TREE_SHA="$(GIT_DIR="$GIT_DIR_ABS" GIT_INDEX_FILE="$TMP_INDEX" git write-tree)"

EXISTING_TREE=""
if [ -n "$PARENT_SHA" ]; then
    EXISTING_TREE="$(git -C "$ROOT" rev-parse "$PARENT_SHA^{tree}" 2>/dev/null || true)"
fi

if [ -n "$EXISTING_TREE" ] && [ "$TREE_SHA" = "$EXISTING_TREE" ]; then
    git -C "$ROOT" update-ref "refs/heads/$BRANCH" "$PARENT_SHA"
    echo "==> '$BRANCH' tree is already identical ($PARENT_SHA)."
else
    COMMIT_MSG="Deploy website from main @ $MAIN_SHA"
    if [ -n "$PARENT_SHA" ]; then
        COMMIT_SHA="$(printf '%s\n' "$COMMIT_MSG" | git -C "$ROOT" commit-tree "$TREE_SHA" -p "$PARENT_SHA")"
    else
        echo "==> Creating initial commit on '$BRANCH'..."
        COMMIT_SHA="$(printf '%s\n' "$COMMIT_MSG" | git -C "$ROOT" commit-tree "$TREE_SHA")"
    fi
    git -C "$ROOT" update-ref "refs/heads/$BRANCH" "$COMMIT_SHA"
    SHORT_NEW="$(git -C "$ROOT" rev-parse --short "$COMMIT_SHA")"
    echo "==> Committed $BRANCH ($SHORT_NEW): $COMMIT_MSG"
fi

if [ "$NO_PUSH" -eq 1 ]; then
    echo "==> Skipping push (--no-push). Local branch '$BRANCH' is updated."
else
    echo "==> Pushing '$BRANCH' to '$REMOTE'..."
    git -C "$ROOT" push "$REMOTE" "refs/heads/$BRANCH:refs/heads/$BRANCH"
    echo "==> Published! GitHub Pages branch '$BRANCH' is live."
fi
