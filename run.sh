#!/bin/bash
# 一键跑起来: 缺构建先构建, 然后启动 cpp-pi.
#   ./run.sh            -> 交互 CLI
#   ./run.sh --mode rpc -> NDJSON RPC 后端 (给 pi TUI 接)
#   ./run.sh test       -> 构建 + 跑集成测试
#   ./run.sh cov        -> 构建 + 覆盖率报告 (80% 门禁)
set -euo pipefail
cd "$(dirname "$0")"

TDSL_ROOT=${TDSL_ROOT:-../trans-dsl-2}
BUILD=build

build() {
   if [ ! -f "$BUILD/cpp-pi" ] || [ "$1" = "-f" ]; then
      cmake -B "$BUILD" -DTDSL_ROOT="$TDSL_ROOT" >/dev/null
      cmake --build "$BUILD" --target cpp-pi -j"$(sysctl -n hw.ncpu)"
   fi
}

case "${1:-}" in
   test)
      cmake -B "$BUILD" -DTDSL_ROOT="$TDSL_ROOT" >/dev/null
      cmake --build "$BUILD" --target cpp-pi cpp-pi-it -j"$(sysctl -n hw.ncpu)"
      exec "./$BUILD/cpp-pi-it" ;;
   cov)
      exec ./tools/coverage.sh ;;
   *)
      build "${FORCE_BUILD:-}"
      exec "./$BUILD/cpp-pi" "$@" ;;
esac
