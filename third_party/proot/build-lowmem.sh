#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"

RAM_LIMIT_MB=${RAM_LIMIT_MB:-6144}
PER_JOB_MB=${PER_JOB_MB:-1024}
STRICT=${STRICT:-1}
TARGET=${1:-proot}

avail_mb=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo 2>/dev/null || echo "$RAM_LIMIT_MB")
budget=$(( avail_mb < RAM_LIMIT_MB ? avail_mb : RAM_LIMIT_MB ))
cores=$(nproc 2>/dev/null || echo 1)

if [ -z "${JOBS:-}" ]; then
  JOBS=$(( budget / PER_JOB_MB ))
  [ "$JOBS" -gt "$cores" ] && JOBS=$cores
  [ "$JOBS" -lt 1 ] && JOBS=1
fi

echo ">> RAM disponível: ${avail_mb} MB | teto: ${budget} MB | jobs: ${JOBS} | por processo: ${PER_JOB_MB} MB"

[ "${STATIC:-0}" = 1 ] && export LDFLAGS="${LDFLAGS:-} -static"

if [ "$STRICT" = 1 ]; then
  ulimit -v $(( PER_JOB_MB * 1024 )) || echo "!! ulimit -v indisponível, seguindo sem"
fi

exec make -C src -j"$JOBS" "$TARGET" \
  GIT=false WITHOUT_PYTHON=1 DEBUG="${DEBUG:-0}" ${EXTRA_MAKE_ARGS:-}
