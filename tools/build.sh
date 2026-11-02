#!/bin/bash
# 统一构建入口: tools/build.sh <mode> [cmake 透传选项]
#   debug    build-debug/    -DCMAKE_BUILD_TYPE=Debug
#   release  build-release/  -DCMAKE_BUILD_TYPE=Release
#   test     build-test/     Debug + 构建并运行 cpp-pi-it (--no-run 只构建)
#   tsan     build-tsan/     Debug + -fsanitize=thread (data race 检测)
#   cov      覆盖率门禁另有 tools/coverage.sh (build-cov/)
# 额外参数原样透传给 cmake configure, 例:
#   tools/build.sh release -DFOO=BAR
set -euo pipefail
cd "$(dirname "$0")/.."

MODE=${1:-debug}
shift || true

TDSL_ROOT=${TDSL_ROOT:-../trans-dsl-2}
JOBS=$(sysctl -n hw.ncpu)

build() {
   local dir=$1; shift
   cmake -B "$dir" -DTDSL_ROOT="$TDSL_ROOT" "$@"
   cmake --build "$dir" -j"$JOBS"
}

case "$MODE" in
   debug)
      build build-debug -DCMAKE_BUILD_TYPE=Debug "$@"
      ;;
   release)
      build build-release -DCMAKE_BUILD_TYPE=Release "$@"
      ;;
   test)
      RUN=1
      [ "${1:-}" = "--no-run" ] && { RUN=0; shift; }
      build build-test -DCMAKE_BUILD_TYPE=Debug "$@"
      if [ "$RUN" = 1 ]; then "./build-test/cpp-pi-it"; fi
      ;;
   tsan)
      build build-tsan -DCMAKE_BUILD_TYPE=Debug \
         -DCMAKE_CXX_FLAGS=-fsanitize=thread \
         -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread "$@"
      ;;
   *)
      sed -n '2,10p' "$0" >&2
      exit 64
      ;;
esac
