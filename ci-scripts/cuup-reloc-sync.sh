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
#   E2=OFF   build without OAI's E2 agent.  Defaults to ON: the xApp-driven test
#            bed needs the CU-CP, both CU-UPs and the DU to register with the
#            RIC, so E2 is part of the deliverable, not an extra.  Pass OFF only
#            to isolate a FlexRIC-vs-tree compile failure from everything else.
#   EXTRA=   extra args appended to build_oai after every flag this script sets,
#            so they win.  The usual need is relaxing warnings-as-errors while
#            the E2 agent is being fixed:
#              EXTRA='--cmake-opt -DCMAKE_C_FLAGS= --cmake-opt -DCMAKE_CXX_FLAGS='
#   FLEXRIC=auto  also rebuild the RIC images (flexric_test:comnetsemu from this
#            tree's FlexRIC submodule, then comnetsemu_flexric:comnetsemu from
#            DFS) -- only where the RIC runs.  auto = when this machine is the
#            core (owns CORE_IP); 1 = always; 0 = never.
#   CORE_IP=192.168.0.200   the core's address, as docker-compose-core.yml binds it
#   DFS_DIR=../DFS          DFS checkout, for Dockerfile.comnetsemu_flexric
#   BRANCH, TAG

set -euo pipefail

BRANCH=${BRANCH:-cuup-reloc}
E2=${E2:-ON}
EXTRA=${EXTRA:-}
TAG=${TAG:-reloc}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)}
LOGDIR=${LOGDIR:-/tmp/cuup-reloc-logs}
FLEXRIC=${FLEXRIC:-auto}
CORE_IP=${CORE_IP:-192.168.0.200}

full=0; build=1; force=0
for a in "$@"; do
  case "$a" in
    --full)      full=1 ;;
    --force)     force=1 ;;
    --no-build)  build=0 ;;
    -h|--help)   sed -n '2,28p' "$0"; exit 0 ;;
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
DFS_DIR=$(readlink -f "${DFS_DIR:-$PWD/../DFS}")

# The RIC (FlexRIC daemon + xApps) runs on the core only, and its images carry
# the same service-model libraries as the RAN's E2 agent: an xApp and an agent
# built from different FlexRIC commits can disagree on the encoding (e.g. an
# old agent asserts on a sliceID label). So on the core they are rebuilt with
# the RAN images, from the same commit.
case "$FLEXRIC" in
  auto) if ip -4 -o addr show 2>/dev/null | grep -q " ${CORE_IP}/"; then ric=1; else ric=0; fi ;;
  1|yes|on|ON) ric=1 ;;
  *) ric=0 ;;
esac
if [ "$ric" -eq 1 ] && [ ! -f "$DFS_DIR/dockerfiles/Dockerfile.comnetsemu_flexric" ]; then
  echo "FlexRIC images requested but $DFS_DIR/dockerfiles/Dockerfile.comnetsemu_flexric" >&2
  echo "is missing -- set DFS_DIR to the DFS checkout, or FLEXRIC=0." >&2
  exit 1
fi

echo "== repo   $PWD"
echo "== branch $BRANCH    jobs $JOBS    E2 $E2    RIC images $([ "$ric" -eq 1 ] && echo "yes ($DFS_DIR)" || echo no)"

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

# The stamp carries the E2 setting as well as the commit.  Flipping E2 does not
# move HEAD, so a commit-only stamp would make an E2=ON rerun report "already
# built" and skip the very rebuild that was asked for.
stamp="${after}+e2${E2}"

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

images=(ran-build:latest "oai-gnb:$TAG" "oai-nr-cuup:$TAG" "oai-nr-ue:$TAG")
[ "$ric" -eq 1 ] && images+=(flexric_test:comnetsemu comnetsemu_flexric:comnetsemu)
current=1
for i in "${images[@]}"; do
  [ "$(stamp_of "$i")" = "$stamp" ] || current=0
done
if [ "$current" -eq 1 ] && [ "$force" -eq 0 ] && [ "$full" -eq 0 ]; then
  echo "== all images already built from $short with E2 $E2 -- pass --force to rebuild anyway"
  exit 0
fi

run() {   # run <logname> <docker build args...>   (context: this repo; CTX= to change)
  local name=$1; shift
  local log="$LOGDIR/$name.log"
  echo "== $name   ($log)"
  if ! docker build --progress=plain "$@" "${CTX:-.}" 2>&1 | tee "$log" | grep -E '^#[0-9]+ (DONE|ERROR)|^ERROR' ; then
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
    --label "reloc.commit=$stamp" \
    --build-arg E2_AGENT="$E2" \
    --build-arg BUILD_OPTION="--build-tool-opt -j$JOBS $EXTRA"

# The three leaf images only copy artefacts out of ran-build, so they are
# independent of each other and run together.
pids=()
for t in gnb:Dockerfile.gNB.ubuntu \
         nr-cuup:Dockerfile.nr-cuup.ubuntu \
         nr-ue:Dockerfile.nrUE.ubuntu ; do
  name=${t%%:*}; dfile=${t#*:}
  ( docker build --progress=plain -t "oai-$name:$TAG" -f "docker/$dfile" \
        --label "reloc.commit=$stamp" . >"$LOGDIR/oai-$name.log" 2>&1 ) &
  pids+=("$!:oai-$name")
done
fail=0
for p in "${pids[@]}"; do
  if wait "${p%%:*}"; then echo "== ${p#*:}   ok"
  else echo "== ${p#*:}   FAILED, last 40 lines:" >&2; tail -40 "$LOGDIR/${p#*:}.log" >&2; fail=1; fi
done
[ "$fail" -eq 0 ] || exit 1

if [ "$ric" -eq 1 ]; then
  # flexric_test: FlexRIC from this tree's submodule (context openair2/E2AP,
  # where the Dockerfile COPYs ./flexric), linked against ran-build's ASN.1.
  CTX=openair2/E2AP run flexric_test -t flexric_test:comnetsemu \
      -f openair2/E2AP/Dockerfile.asn_flexric --label "reloc.commit=$stamp"
  # comnetsemu_flexric: FROM flexric_test, plus the test bed's tools; built
  # the way DFS's adapt_docker_images.sh builds it.
  CTX="$DFS_DIR" run comnetsemu_flexric -t comnetsemu_flexric:comnetsemu \
      -f "$DFS_DIR/dockerfiles/Dockerfile.comnetsemu_flexric" \
      --network=host --build-arg APT_FORCE_IPV4=true --label "reloc.commit=$stamp"
fi

echo
docker images --filter "reference=oai-*:$TAG" \
  --format '   {{.Repository}}:{{.Tag}}  {{.ID}}  {{.CreatedSince}}  {{.Size}}'
if [ "$ric" -eq 1 ]; then
  for i in flexric_test:comnetsemu comnetsemu_flexric:comnetsemu; do
    docker images --format '   {{.Repository}}:{{.Tag}}  {{.ID}}  {{.CreatedSince}}  {{.Size}}' "$i"
  done
fi
echo
echo "built from $short on $BRANCH    logs in $LOGDIR"
