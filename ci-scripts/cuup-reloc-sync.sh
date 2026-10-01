#!/bin/bash
# Resync this checkout from the remote and rebuild the images the inter-CU-UP
# relocation test bed uses.
#
# Run this on the TEST machine.  Code is authored and committed elsewhere and
# arrives here only through git -- never edit in place here, or the next resync
# throws the edit away.
#
#   ./ci-scripts/cuup-reloc-sync.sh             resync, then rebuild ran-build + the 3 images
#   ./ci-scripts/cuup-reloc-sync.sh --full      also rebuild ran-base (toolchain/deps changed)
#   ./ci-scripts/cuup-reloc-sync.sh --no-build  resync only
#   BRANCH=some-other ./ci-scripts/cuup-reloc-sync.sh
#
# E2 is off: the relocation path does not touch it, and the pinned FlexRIC
# commit does not compile.  Set E2=ON once that submodule is fixed.

set -euo pipefail

BRANCH=${BRANCH:-cuup-reloc}
E2=${E2:-OFF}
TAG=${TAG:-reloc}
full=0
build=1
for a in "$@"; do
  case "$a" in
    --full)     full=1 ;;
    --no-build) build=0 ;;
    -h|--help)  sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$(readlink -f "$0")")/.."
echo "== repo   $PWD"
echo "== branch $BRANCH"

# A dirty tree here means someone edited the test machine directly.  Stop rather
# than silently discarding it -- resync is destructive by design.
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo
  echo "working tree is dirty -- this machine is supposed to be a pure consumer."
  git status --short --untracked-files=no
  echo
  echo "commit it somewhere, or 'git checkout -- .', then rerun."
  exit 1
fi

before=$(git rev-parse HEAD)
git fetch --tags origin "$BRANCH"
git checkout -q -B "$BRANCH" "origin/$BRANCH"
git submodule sync --recursive
git submodule update --init --recursive
after=$(git rev-parse HEAD)

echo
if [ "$before" = "$after" ]; then
  echo "== already at $(git rev-parse --short HEAD) -- nothing new"
else
  echo "== $(git rev-parse --short "$before") -> $(git rev-parse --short "$after")"
  git log --oneline --no-decorate "$before..$after" | sed 's/^/     /'
fi
echo

[ "$build" -eq 1 ] || exit 0

# ran-base carries the toolchain and the installed dependencies.  It only needs
# rebuilding when those change -- it is the slow one, so it is opt-in.
if [ "$full" -eq 1 ]; then
  echo "== ran-base"
  docker build --progress=plain -t ran-base:latest -f docker/Dockerfile.base.ubuntu .
elif ! docker image inspect ran-base:latest >/dev/null 2>&1; then
  echo "== ran-base (absent, building it)"
  docker build --progress=plain -t ran-base:latest -f docker/Dockerfile.base.ubuntu .
else
  echo "== ran-base    reusing $(docker image inspect -f '{{.Id}}' ran-base:latest | cut -c8-19)"
fi

# ran-build compiles the tree.  Always rebuilt: this is what carries the change.
echo "== ran-build"
docker build --progress=plain -t ran-build:latest -f docker/Dockerfile.build.ubuntu \
       --build-arg BUILD_OPTION="--cmake-opt -DE2_AGENT=$E2" .

for t in gNB:Dockerfile.gNB.ubuntu \
         nr-cuup:Dockerfile.nr-cuup.ubuntu \
         nr-ue:Dockerfile.nrUE.ubuntu ; do
  name=${t%%:*}; dfile=${t#*:}
  echo "== oai-${name,,}:$TAG"
  docker build --progress=plain -t "oai-${name,,}:$TAG" -f "docker/$dfile" .
done

echo
echo "== images"
docker images --filter "reference=oai-*:$TAG" \
  --format '   {{.Repository}}:{{.Tag}}  {{.ID}}  {{.CreatedSince}}  {{.Size}}'
echo
echo "built from $(git rev-parse --short HEAD) on $BRANCH"
