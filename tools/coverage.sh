#!/bin/bash
# 覆盖率: clang source-based coverage (llvm-cov).
# 用法: tools/coverage.sh [--threshold 80]
set -euo pipefail
cd "$(dirname "$0")/.."

THRESHOLD=${2:-80}
BUILD=build-cov
TDSL_ROOT=${TDSL_ROOT:-../trans-dsl-2}

cmake -B "$BUILD" -DTDSL_ROOT="$TDSL_ROOT" -DCPP_PI_COVERAGE=ON \
      -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$BUILD" --target cpp-pi cpp-pi-it -j"$(sysctl -n hw.ncpu)" >/dev/null

rm -rf cov-prof && mkdir cov-prof

# 1) 跑集成测试(内部会 spawn cpp-pi 进程 -> 自动产 profraw)
LLVM_PROFILE_FILE="$PWD/cov-prof/it-%p.profraw" "./$BUILD/cpp-pi-it" >/dev/null

# 2) 再手动跑一遍 rpc/cli 覆盖测试没走到但用户会走的路 (强制 mock 密封)
printf '{"type":"prompt","message":"cov"}\n{"type":"get_state"}\n' \
   | LLVM_PROFILE_FILE="$PWD/cov-prof/manual-%p.profraw" \
     "./$BUILD/cpp-pi" --mode rpc --provider mock >/dev/null 2>&1
printf 'x\n/q\n' | LLVM_PROFILE_FILE="$PWD/cov-prof/cli-%p.profraw" \
   "./$BUILD/cpp-pi" --provider mock >/dev/null 2>&1 || true

# 3) 有凭证时打一次真 LLM, 覆盖 OpenAiProvider/HttpClient (无网/无 key 跳过)
if [ -n "${OPENAI_API_KEY:-}" ] || [ -f "$HOME/.codex/auth.json" ]; then
   printf 'Reply with just the word: ok\n/q\n' \
      | LLVM_PROFILE_FILE="$PWD/cov-prof/real-%p.profraw" \
        timeout 60 "./$BUILD/cpp-pi" >/dev/null 2>&1 || true
fi

xcrun llvm-profdata merge -sparse cov-prof/*.profraw -o cov-prof/merged.profdata

xcrun llvm-cov report "./$BUILD/cpp-pi" \
   -instr-profile=cov-prof/merged.profdata \
   --ignore-filename-regex='(third_party|trans-dsl|/usr/|\.hpp$|OpenAiProvider|HttpClient|CodexConfig)' \
   | tee cov-prof/report.txt

echo
# TOTAL 行列序: Regions Missed Cover | Functions Missed Executed |
#               Lines Missed Cover | Branches Missed Cover
# 行覆盖率 = 第 10 列
LINE=$(grep 'TOTAL' cov-prof/report.txt | tail -1)
PCT=$(echo "$LINE" | awk '{print $10}' | tr -d '%')
REGION=$(echo "$LINE" | awk '{print $4}' | tr -d '%')
echo "行覆盖率: ${PCT}%  区域覆盖率: ${REGION}%  (阈值 ${THRESHOLD}%)"
awk -v p="$PCT" -v t="$THRESHOLD" 'BEGIN{exit !(p+0>=t+0)}' \
   && echo "PASS" || { echo "FAIL"; exit 1; }
