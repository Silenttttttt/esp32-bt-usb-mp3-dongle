#!/usr/bin/env bash
# Build/flash a board from an exact past commit, with that commit's own flash
# script (so its own flag list), from a separate git worktree. The way to put a
# known build back on a board without touching the working tree.
#
#   tools/flash_build.sh car-2026-09-25 s3|classic|both [flash.sh args...]
#   tools/flash_build.sh <commit>       s3|classic|both [flash.sh args...]
#
# Named builds are listed in KNOWN_BUILDS.md (and below). Both boards must run
# the same build: the link format (ENCODE_ON_S3, LINK_Q) has to match.
# Each flash is also logged to this checkout's logs/flash_history.log.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

declare -A BUILDS=(
  [car-2026-09-25]=4cef646   # in the car since 2026-09-25 (32 KB clusters car-verified 09-26); plain flash.sh, v2 flags
  [bench-2026-10-10-fixlink]=c209365  # bench-verified link fix; flash with: both --fix-link --diag
)

[[ $# -ge 2 ]] || { sed -n '2,12p' "$0"; exit 2; }
NAME=$1; BOARD=$2; shift 2
COMMIT=${BUILDS[$NAME]:-$NAME}
SHA=$(git rev-parse --short=7 "$COMMIT^{commit}")
WT=$ROOT/.worktrees/$SHA
if [[ ! -d $WT ]]; then
  git worktree add --detach "$WT" "$SHA" >/dev/null
fi
echo "building $NAME ($SHA) from $WT"

flash_one() {
  local dir=$1; shift
  (cd "$WT/$dir" && ./flash.sh "$@")
  if [[ " $* " != *" --no-upload "* ]]; then
    tail -1 "$WT/logs/flash_history.log" | sed "s/\$/  [via tools\/flash_build.sh $NAME]/" >> "$ROOT/logs/flash_history.log"
  fi
}
case $BOARD in
  s3) flash_one esp32-s3-msc "$@" ;;
  classic) flash_one esp32-bt-mp3-test "$@" ;;
  both) flash_one esp32-bt-mp3-test "$@"; flash_one esp32-s3-msc "$@" ;;
  *) echo "board must be s3, classic or both" >&2; exit 2 ;;
esac
