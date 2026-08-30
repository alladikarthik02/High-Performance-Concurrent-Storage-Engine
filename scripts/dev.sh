#!/usr/bin/env bash
# Run any command inside the lsmeng container, with the repo bind-mounted.
#
#   ./scripts/dev.sh                 -> interactive shell
#   ./scripts/dev.sh <cmd> [args..]  -> run one command and exit
#
# WHY BIND-MOUNT INSTEAD OF COPY:
#   Edits land on the host (real files, real git history, host editor) while all
#   compilation and execution happen on Linux. Copying into the image would mean an image
#   rebuild per edit.
set -euo pipefail

# Docker Desktop on macOS registers /usr/local/bin/docker but leaves its credential
# helpers inside the .app bundle, off a non-interactive shell's PATH. The CLI reads
# credsStore=desktop from ~/.docker/config.json and then fails with
# `docker-credential-desktop: executable file not found`. Prepend the bundle dir here
# rather than editing the user's global config -- scoped to this script, changes nothing
# outside this project. (Carried over from dedupe/wanrep; see hotpath CHALLENGES B1.)
if [ -d "/Applications/Docker.app/Contents/Resources/bin" ]; then
  PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
fi

# CHALLENGES C2: `docker version` prints a daemon connection failure and STILL EXITS 0,
# so gating on its status sails straight past a stopped daemon and fails later somewhere
# confusing. `docker info` exits non-zero when the daemon is unreachable, which is the
# only reason it is the check used here.
if ! docker info >/dev/null 2>&1; then
  echo "error: docker daemon is not reachable." >&2
  echo "       start Docker Desktop (open -a Docker) and wait for it to finish booting." >&2
  exit 1
fi

IMAGE="lsmeng-dev"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo ">>> building $IMAGE (first run only)" >&2
  docker build -t "$IMAGE" "$REPO_ROOT"
fi

# -t only when stdin is a TTY, so this works both interactively and from scripts.
TTY_FLAGS="-i"
[ -t 0 ] && TTY_FLAGS="-it"

# SYS_PTRACE + unconfined seccomp: gdb, perf and valgrind all need them.
exec docker run --rm $TTY_FLAGS \
  -v "$REPO_ROOT":/work \
  -w /work \
  --cap-add=SYS_PTRACE \
  --security-opt seccomp=unconfined \
  "$IMAGE" \
  "${@:-/bin/bash}"
