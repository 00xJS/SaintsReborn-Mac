#!/bin/bash
# Brings the newest Saints Reborn (whompay/SaintsReborn) into this Mac fork.
# The Mac files stay as they are; everything else follows upstream.
#
#   scripts/mac/sync-upstream.sh           merge only
#   scripts/mac/sync-upstream.sh --build   merge, then rebuild with setup-mac.sh
#
# Nothing is pushed: test the build, then push and publish a release yourself.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
UPSTREAM="https://github.com/whompay/SaintsReborn.git"
# Files this fork owns or changes; they must still carry the Mac changes after a merge.
MAC_FILES=(README.md NOTICE LICENSE THIRD_PARTY_NOTICES.md "Install Saints Reborn.command" "Update Saints Reborn.command"
           scripts/setup-mac.sh scripts/mac/launch.sh scripts/mac/make_app.sh)
cd "$ROOT"
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "Commit or discard your local changes first."; exit 1; }
git fetch -q "$UPSTREAM" main
NEW="$(git log --oneline HEAD..FETCH_HEAD | wc -l | tr -d ' ')"
if [ "$NEW" = "0" ]; then echo "Already up to date with upstream."; exit 0; fi
echo "New upstream commits:"; git log --oneline HEAD..FETCH_HEAD
BEFORE="$(git rev-parse HEAD)"
# On a conflict upstream wins; the checks below make sure no Mac change was lost.
git merge -q -X theirs FETCH_HEAD -m "Merge upstream Saints Reborn ($(git log -1 --format=%s FETCH_HEAD))"
fail() { echo "STOPPED: $1"; echo "Undo with: git reset --hard $BEFORE"; exit 1; }
for f in "${MAC_FILES[@]}"; do [ -f "$f" ] || fail "$f is missing after the merge."; done
grep -q "RawMouseProc" project/src/kbm.cpp || fail "the raw mouse input change in project/src/kbm.cpp was lost."
grep -q "SDL_AUDIO_DRIVER" project/src/main.cpp || fail "the Wine audio change in project/src/main.cpp was lost."
grep -q "Saints Reborn on Mac" README.md || fail "README.md is no longer the Mac README."
[ "$(grep -c "^## Credits" README.md)" = "1" ] || fail "README.md has a duplicated Credits section after the merge; keep one."
grep -q "Saints Reborn by Whompay" NOTICE || fail "the upstream NOTICE text is missing."
grep -q "Saints Reborn on Mac" NOTICE || fail "the Mac addendum in NOTICE was lost."
grep -q "whompay/SaintsReborn'" .github/workflows/setup-app.yml || fail "the upstream Setup workflow would run in this fork again (.github/workflows/setup-app.yml)."
if ! grep -q "^/prefix/$" .gitignore; then
  printf '\n# macOS setup (scripts/setup-mac.sh)\n/prefix/\n/Saints Reborn.app/\n/config/generated/\n.DS_Store\n' >> .gitignore
  git add .gitignore && git commit -q -m "Mac: keep the macOS ignore rules after the upstream merge"
fi
echo; echo "Merged. Files that differ from upstream now:"; git diff --stat FETCH_HEAD HEAD | sed '$d'
if [ "${1:-}" = "--build" ]; then
  /bin/bash "$ROOT/scripts/setup-mac.sh" --accept-license
fi
echo; echo "Next: play-test the build, then  git push  and publish a release (scripts/mac/make_release.sh)."
