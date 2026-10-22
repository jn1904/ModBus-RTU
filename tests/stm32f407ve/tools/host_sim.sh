#!/bin/sh
# 在宿主机上跑 main.c 的自测逻辑：真实库源码 + 桩 BSP。
# 用途：上板之前先确认用例的期望值与流程本身没问题（不需要 ARM 工具链或硬件）。
#
#   sh tests/stm32f407ve/tools/host_sim.sh
#
# 预期输出：16 通过 / 0 失败 / 1 跳过（UART 自环在宿主上必然 skip）。
set -e
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${TMPDIR:-/tmp}/modbus_host_sim
gcc -std=c99 -O1 -Wall -Wextra \
  -I"$root/tests/stm32f407ve/include" -I"$root/include/modbus" \
  -I"$root/include" -I"$root/src" \
  "$root/tests/stm32f407ve/src/main.c" \
  "$root"/src/core/*.c "$root"/src/engine/*.c \
  "$root"/src/error/*.c "$root"/src/port/*.c \
  "$root/tests/stm32f407ve/tools/host_stub.c" \
  -o "$out"
"$out"
