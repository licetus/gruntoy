#!/usr/bin/env bash
# ============================================================================
#  run_tests.sh —— 在电脑上验证固件逻辑（不需要 ESP32 硬件、不需要 Arduino 环境）
#  gruntoy v1.0
#
#  原理：用一组最小 Arduino 桩（Arduino.h / Preferences.h / arduino_stub.cpp）
#        把固件的纯逻辑部分编译成本机可执行文件，用假时钟推进时间，跑断言。
#
#  覆盖范围（4 个套件）：
#    test_main    参数体系 P-01~P-58、校验规则 C-01~C-18 / X-01~X-03、
#                 三套预设自洽性、NVS 持久化与 schema 降级守卫
#    test_mood    体感状态机时序：累积触发、PURR 锁 W4、平滑降落、冷却反馈、
#                 迟滞防抖、映射单调性、呼吸链路与节流、P-30/P-31 闸门
#    test_haptic  体感引擎：呼吸包络（谷底 0%）、门控调制是否真的落到输出、
#                 淡入淡出、互斥优先级、扫频推进
#    test_cli     串口调试台：命令解析、写入校验拦截、枚举按名写入、
#                 非阻塞 t feed / t auto（馈入期间状态机仍能推进）
#
#  ⚠ 这不是硬件仿真：不模拟马达惯性、不模拟机构、不产生声音。
#     它只回答"逻辑对不对"，不回答"手感好不好"。
#
#  用法： bash test/run_tests.sh
#         bash test/run_tests.sh --verbose
# ============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT="$HERE/build"
mkdir -p "$OUT"

CXX="${CXX:-g++}"
WARN="-Wall -Wextra -Wno-unused-parameter"
FLAGS="-std=c++17 -O1 -I$HERE -I$ROOT"

VERBOSE=0
[ "${1:-}" = "--verbose" ] && VERBOSE=1

echo "=============================================="
echo " gruntoy v1.0 本机逻辑验证"
echo "=============================================="
echo "固件目录: $ROOT"
echo "编译器  : $($CXX --version | head -1)"
echo

# 参与编译的固件源文件
#   cli.cpp 纳入编译：v1.0 已把 t feed / t auto 改为非阻塞调度，
#   因此调试台同样可以在假时钟下被测试。
SRCS="$ROOT/params.cpp $ROOT/haptic.cpp $ROOT/mood.cpp $ROOT/tail.cpp $ROOT/cli.cpp $HERE/arduino_stub.cpp"

SUITE_NAMES=()
SUITE_PASS=()
SUITE_FAIL=()

TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_COMPILE_ERR=0

run_one() {
  local name="$1" src="$2"
  echo "----------------------------------------------"
  echo " 测试: $name"
  echo "----------------------------------------------"

  local log="$OUT/$name.err"
  if ! $CXX $FLAGS $WARN -o "$OUT/$name" "$src" $SRCS 2>"$log"; then
    echo "  [编译失败]"
    cat "$log"
    TOTAL_FAIL=$((TOTAL_FAIL+1))
    TOTAL_COMPILE_ERR=$((TOTAL_COMPILE_ERR+1))
    SUITE_NAMES+=("$name"); SUITE_PASS+=("-"); SUITE_FAIL+=("编译失败")
    echo
    return
  fi

  # 编译告警（非致命）：--verbose 时展示
  if [ -s "$log" ]; then
    if [ $VERBOSE -eq 1 ]; then
      echo "  [编译告警]"
      sed 's/^/    /' "$log"
    else
      echo "  [编译告警 $(wc -l < "$log" | tr -d ' ') 行，用 --verbose 查看]"
    fi
  fi

  set +e
  local tmpout="$OUT/$name.log"
  "$OUT/$name" > "$tmpout" 2>&1
  local rc=$?
  set -e
  cat "$tmpout"

  # 注意：grep -c 在无匹配时既输出 0 又返回退出码 1，
  # 若写成 `grep -c ... || echo 0` 会打印两行 "0"。这里用 $(... | tr -d '\n') 兜底。
  local ok fail
  ok=$(grep -c 'PASS' "$tmpout" 2>/dev/null | tr -d '\n')
  fail=$(grep -c 'FAIL' "$tmpout" 2>/dev/null | tr -d '\n')
  ok=${ok:-0}
  fail=${fail:-0}

  SUITE_NAMES+=("$name"); SUITE_PASS+=("$ok"); SUITE_FAIL+=("$fail")

  if [ $rc -eq 0 ]; then
    TOTAL_PASS=$((TOTAL_PASS+1))
  else
    TOTAL_FAIL=$((TOTAL_FAIL+1))
  fi
  echo
}

run_one test_main   "$HERE/test_main.cpp"
run_one test_mood   "$HERE/test_mood.cpp"
run_one test_haptic "$HERE/test_haptic.cpp"
run_one test_cli    "$HERE/test_cli.cpp"

# ---------------------------------------------------------------------------
# 固件整体启动冒烟验证：走 gruntoy.ino 的 setup()/loop() 真实入口，
# 且把「调试配置」与「量产配置」两种编译形态各编译并运行一遍。
#   量产配置（ENABLE_DEBUG_CHANNEL=0）是最终提交审核的那一份，
#   不能等到交付前才第一次被编译。
# ---------------------------------------------------------------------------
run_build_check() {
  local label="$1" defs="$2"
  echo "----------------------------------------------"
  echo " 测试: build_check [$label]"
  echo "----------------------------------------------"

  local log="$OUT/build_check_$label.err"
  # .ino 不是 g++ 认识的扩展名，用 -x c++ 显式指定语言；
  # 该选项对其后所有输入生效，因此全部源文件都按 C++ 编译。
  if ! $CXX $FLAGS $WARN $defs -x c++ \
        -o "$OUT/build_check_$label" \
        "$HERE/build_check.cpp" "$ROOT/gruntoy.ino" \
        $SRCS 2>"$log"; then
    echo "  [编译失败]"
    cat "$log"
    TOTAL_FAIL=$((TOTAL_FAIL+1))
    TOTAL_COMPILE_ERR=$((TOTAL_COMPILE_ERR+1))
    SUITE_NAMES+=("build_$label"); SUITE_PASS+=("-"); SUITE_FAIL+=("编译失败")
    echo
    return
  fi
  if [ -s "$log" ] && [ $VERBOSE -eq 1 ]; then
    echo "  [编译告警]"; sed 's/^/    /' "$log"
  fi

  set +e
  local tmpout="$OUT/build_check_$label.log"
  "$OUT/build_check_$label" > "$tmpout" 2>&1
  local rc=$?
  set -e
  cat "$tmpout"

  local ok fail
  ok=$(grep -c 'PASS' "$tmpout" 2>/dev/null | tr -d '\n'); ok=${ok:-0}
  fail=$(grep -c 'FAIL' "$tmpout" 2>/dev/null | tr -d '\n'); fail=${fail:-0}
  SUITE_NAMES+=("build_$label"); SUITE_PASS+=("$ok"); SUITE_FAIL+=("$fail")

  if [ $rc -eq 0 ]; then TOTAL_PASS=$((TOTAL_PASS+1)); else TOTAL_FAIL=$((TOTAL_FAIL+1)); fi
  echo
}

run_build_check debug   "-DENABLE_DEBUG_CHANNEL=1 -DENABLE_TAIL_OUTPUT=0"
run_build_check release "-DENABLE_DEBUG_CHANNEL=0 -DENABLE_TAIL_OUTPUT=1"

echo "=============================================="
echo " 明细"
echo "=============================================="
printf "  %-14s %8s %8s\n" "套件" "断言通过" "断言失败"
for i in "${!SUITE_NAMES[@]}"; do
  printf "  %-14s %8s %8s\n" "${SUITE_NAMES[$i]}" "${SUITE_PASS[$i]}" "${SUITE_FAIL[$i]}"
done

echo
echo "=============================================="
echo " 汇总: 套件 $TOTAL_PASS 通过 / $TOTAL_FAIL 失败"
if [ $TOTAL_FAIL -gt 0 ]; then
  echo "       （其中编译失败 $TOTAL_COMPILE_ERR 个）"
fi
echo "=============================================="

if [ $TOTAL_FAIL -eq 0 ]; then
  echo
  echo "全部通过。可以进入烧录阶段。"
  echo "提醒：本机测试只验证逻辑正确性，验证不了真实体感。"
  echo "      呼吸是否有嗡鸣、咕噜像不像猫，必须烧录后实际听。"
  echo "      扫频找猫感区间（t gate）更是只能在硬件上听出来的。"
  exit 0
else
  echo
  echo "存在失败项，请先修复再烧录。"
  exit 1
fi
