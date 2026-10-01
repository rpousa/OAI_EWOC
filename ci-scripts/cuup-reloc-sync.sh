#!/bin/bash
# Resync this checkout from the remote and rebuild the images the inter-CU-UP
# relocation test bed uses.
#
# Run this on the TEST machine.  Code is authored and committed elsewhere and
# arrives here only through git -- never edit in place here, or the next resync
# throws the edit away.
#
#   ./ci-scripts/cuup-reloc-sync.sh              resync, rebuild what is stale
#   ./ci-scripts/cuup-reloc-sync.sh --full       also rebuild ran-base
#   ./ci-scripts/cuup-reloc-sync.sh --force      rebuild even if already current
#   ./ci-scripts/cuup-reloc-sync.sh --no-build   resync only
#
#   JOBS=28  how many compile jobs.  Defaults to the machine's online CPU count,
#            NOT nproc -- nproc honours the affinity mask, so on a box with cores
#            isolated for real-time threads it under-reports badly.
#   E2=ON    the pinned FlexRIC commit does not compile against this tree, and
#            the relocation path does not touch E2.  Turn it on once fixed.
#   BRANCH, TAG

set -euo pipefail

BRANCH=${BRANCH:-cuup-reloc}
E2=${E2:-OFF}
TAG=${TAG:-reloc}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)}
LOGDIR=${LOGDIR:-/tmp/cuup-reloc-logs}

full=0; build=1; force=0
for a in "$@"; do
  case "$a" in
    --full)      full=1 ;;
    --force)     force=1 ;;
    --no-build)  build=0 ;;
    -h|--help)   sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

# The build Dockerfile uses "RUN --mount=type=cache" for ccache and the CPM
# package cache.  The classic builder cannot parse that, and even where it can
# the cache does not persist -- which is the difference between a 20 minute
# rebuild and an hour.
if [ "${DOCKER_BUILDKIT:-1}" = "0" ]; then
  echo "DOCKER_BUILDKIT=0 is set.  docker/Dockerfile.build.ubuntu needs BuildKit" >&2
  echo "for its ccache mount -- unset it and rerun." >&2
  exit 1
fi

self=$(readlink -f "$0")
self_sum=$(md5sum "$self" | cut -d' ' -f1)

cd "$(dirname "$self")/.."
mkdir -p "$LOGDIR"
echo "== repo   $PWD"
echo "== branch $BRANCH    jobs $JOBS    E2 $E2"

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
short=$(git rev-parse --short "$after")

# bash reads a script lazily, so the checkout above may have rewritten this file
# out from under the interpreter.  If it changed, start over from the new one.
if [ "$(md5sum "$self" | cut -d' ' -f1)" != "$self_sum" ] && [ -z "${CUUP_RELOC_REEXEC:-}" ]; then
  echo "== sync script updated by the resync -- re-running it"
  CUUP_RELOC_REEXEC=1 exec "$self" "$@"
fi

echo
if [ "$before" = "$after" ]; then
  echo "== already at $short -- nothing new"
else
  echo "== $(git rev-parse --short "$before") -> $short"
  git log --oneline --no-decorate "$before..$after" | sed 's/^/     /'
fi
echo

[ "$build" -eq 1 ] || exit 0

# Every image is stamped with the commit it was built from, so a rerun that has
# nothing to do says so instead of spending an hour proving it.
stamp_of() { docker image inspect -f '{{index .Config.Labels "reloc.commit"}}' "$1" 2>/dev/null || true; }

current=1
for i in ran-build:latest "oai-gnb:$TAG" "oai-nr-cuup:$TAG" "oai-nr-ue:$TAG"; do
  [ "$(stamp_of "$i")" = "$after" ] || current=0
done
if [ "$current" -eq 1 ] && [ "$force" -eq 0 ] && [ "$full" -eq 0 ]; then
  echo "== all images already built from $short -- pass --force to rebuild anyway"
  exit 0
fi

run() {   # run <logname> <docker build args...>
  local name=$1; shift
  local log="$LOGDIR/$name.log"
  echo "== $name   ($log)"
  if ! docker build --progress=plain "$@" . 2>&1 | tee "$log" | grep -E '^#[0-9]+ (DONE|ERROR)|^ERROR' ; then
    echo "   -- $name failed, last 40 lines:" >&2
    tail -40 "$log" >&2
    return 1
  fi
}

if [ "$full" -eq 1 ] || ! docker image inspect ran-base:latest >/dev/null 2>&1; then
  run ran-base -t ran-base:latest -f docker/Dockerfile.base.ubuntu
else
  echo "== ran-base    reusing $(docker image inspect -f '{{.Id}}' ran-base:latest | cut -c8-19)"
fi

# -j is appended after build_oai's own "-j$(nproc)"; ninja takes the last one.
run ran-build -t ran-build:latest -f docker/Dockerfile.build.ubuntu \
    --label "reloc.commit=$after" \
    --build-arg BUILD_OPTION="--cmake-opt -DE2_AGENT=$E2 --build-tool-opt -j$JOBS"

# The three leaf images only copy artefacts out of ran-build, so they are
# independent of each other and run together.
pids=()
for t in gnb:Dockerfile.gNB.ubuntu \
         nr-cuup:Dockerfile.nr-cuup.ubuntu \
         nr-ue:Dockerfile.nrUE.ubuntu ; do
  name=${t%%:*}; dfile=${t#*:}
  ( docker build --progress=plain -t "oai-$name:$TAG" -f "docker/$dfile" \
        --label "reloc.commit=$after" . >"$LOGDIR/oai-$name.log" 2>&1 ) &
  pids+=("$!:oai-$name")
done
fail=0
for p in "${pids[@]}"; do
  if wait "${p%%:*}"; then echo "== ${p#*:}   ok"
  else echo "== ${p#*:}   FAILED, last 40 lines:" >&2; tail -40 "$LOGDIR/${p#*:}.log" >&2; fail=1; fi
done
[ "$fail" -eq 0 ] || exit 1

echo
docker images --filter "reference=oai-*:$TAG" \
  --format '   {{.Repository}}:{{.Tag}}  {{.ID}}  {{.CreatedSince}}  {{.Size}}'
echo
echo "built from $short on $BRANCH    logs in $LOGDIR"
