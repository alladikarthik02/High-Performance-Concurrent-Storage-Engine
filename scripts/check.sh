#!/usr/bin/env bash
# Full verification gate: build and run every suite in three configurations.
# Run inside the container:  ./scripts/dev.sh ./scripts/check.sh
#
# WHY THREE BUILDS:
#   none     - the code we actually ship and benchmark (-O2)
#   address  - ASan + UBSan: memory safety and undefined behaviour        (SPEC S20)
#   thread   - TSan: data races and lock-order inversions                 (SPEC S6, S20)
# ASan and TSan cannot be combined in one binary, which is why this is three build trees
# and not one.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

CONFIGS=("${@:-none address thread}")
FAILED=0

# ---------------------------------------------------------------------------
# SPEC S24: no engine code path performs I/O except through Options::env.
# This is a test, not a style rule -- it is what makes the FaultEnv seam reachable
# (SPEC 7 layer 2) and what makes E-25's "there is no API to make this mistake"
# enforceable rather than aspirational. Run first: it is instant, and a violation
# invalidates every durability test that follows.
# ---------------------------------------------------------------------------
echo ">>> S24: direct libc I/O outside env_posix.cc"
if BAD=$(grep -rnE '(^|[^_[:alnum:]])(open|openat|pread|pwrite|read|write|fsync|fdatasync|rename|unlink|mkdir|rmdir)[[:space:]]*\(' \
           src include 2>/dev/null \
         | grep -v 'src/env_posix.cc' \
         | grep -vE '^\S+:[0-9]+:\s*(//|\*|/\*)' \
         | grep -vE '\.(Read|Write|Open|Sync|Rename|Unlink)\(' ); then
  echo "!!! S24 VIOLATION: direct libc I/O outside src/env_posix.cc" >&2
  echo "$BAD" >&2
  FAILED=1
else
  echo "    clean"
fi

for cfg in ${CONFIGS[@]}; do
  dir="build-${cfg}"
  echo ""
  echo "=============================================================="
  echo ">>> configuration: ${cfg}   (${dir})"
  echo "=============================================================="
  cmake -S . -B "${dir}" -G Ninja -DLSMENG_SANITIZE="${cfg}" >/dev/null
  cmake --build "${dir}" -j"$(nproc)"
  if ! ctest --test-dir "${dir}" --output-on-failure -j"$(nproc)"; then
    FAILED=1
    echo "!!! configuration ${cfg} FAILED"
  fi
done

echo ""
if [ "${FAILED}" -eq 0 ]; then
  echo "ALL CONFIGURATIONS GREEN"
else
  echo "FAILURES ABOVE"
fi
exit "${FAILED}"
