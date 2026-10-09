#!/usr/bin/env bash
# Checkpoint helper: verify, commit, and push a completed step.
#
# WHY: uncommitted work is not saved work. A long session that piles up dozens
# of unstaged files can lose a finished feature when an outside edit partially
# reverts a file. Small verified checkpoints prevent that.
#
# Usage:
#   ./scripts/checkpoint.sh "feat(proxy): bound async upstream execution"
#   ./scripts/checkpoint.sh "fix(tls): resume partial writes" --no-ci   # skip CI (fast checkpoints only)
#
# It runs (in order): git status guard -> ./ci.sh -> git add -A -> commit ->
# push to every configured remote. Aborts on the first failure.
set -euo pipefail
cd "$(dirname "$0")/.."

MESSAGE="${1:-}"
RUN_CI=1
for arg in "${@:2}"; do
    case "$arg" in
        --no-ci) RUN_CI=0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

if [[ -z "$MESSAGE" ]]; then
    echo "usage: $0 \"<commit message>\" [--no-ci]" >&2
    exit 2
fi

if [[ -n "$(git status --porcelain)" ]]; then
    echo "==> Changes to checkpoint:"
    git status --short
else
    echo "Nothing to commit (worktree clean)."
    exit 0
fi

# 1. Verify BEFORE committing: a release once shipped non-compiling code
#    because CI was not re-run right before the commit.
if [[ "$RUN_CI" -eq 1 ]]; then
    echo "==> Running ./ci.sh (build + unit + integration)"
    ./ci.sh
else
    echo "==> Skipping CI (--no-ci)"
fi

# 2. Commit everything that was just verified.
echo "==> Committing"
git add -A
git commit -m "$MESSAGE"

# 3. Push to every remote that has a URL for the current branch.
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
PUSHED=0
for remote in $(git remote); do
    echo "==> Pushing to $remote ($BRANCH)"
    git push "$remote" "$BRANCH"
    PUSHED=$((PUSHED + 1))
done

if [[ "$PUSHED" -eq 0 ]]; then
    echo "WARNING: no git remote configured; commit is local only." >&2
    exit 1
fi

echo "==> Checkpoint complete: $(git rev-parse --short HEAD) pushed to $PUSHED remote(s)"
