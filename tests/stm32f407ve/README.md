# STM32F407VE 板端自测

在真实 STM32F407VE 上验证这个 Modbus RTU 库：**先跑纯软件回环**（不接线也能出结果），
**再跑一次真实 UART 自环**（需要一根 PA9–PA10 跳线）。整个工程零厂商依赖：不用 CMSIS、
不用 HAL、不用 CubeMX，寄存器定义在 include/stm32f407ve_regs.h 里手写。

```
tests/stm32f407ve/
├── CMakeLists.txt                 独立交叉编译工程（产出 .elf/.bin/.hex）
├── cmake/arm-none-eabi.cmake      工具链：Cortex-M4F / 硬浮点
├── linker/stm32f407ve.ld          512 KB Flash / 128 KB SRAM
├── startup/startup_stm32f407xx.s  向量表（98 项）+ Reset_Handler
├── include/
│   ├── bsp.h                      板级接口契约
│   └── stm32f407ve_regs.h         手写寄存器定义（含编译期偏移断言）
├── src/
│   ├── board.c                    时钟 / SysTick / USART1 / RS-485 / 环形缓冲
│   └── main.c                     自测用例
└── tools/check_regs.py            寄存器常量与 CMSIS 设备头逐项对拍
```

## 用例

| # | 用例 | 验证什么 | 需要外设 |
|---|---|---|---|
| 1 | 时钟 | HSE→PLL 切到 168 MHz（`bsp_clock_ok()` 且实测频率相符） | — |
| 2 | 时基 | `bsp_delay_ms(10)` 实测落在 8–25 ms，1 ms SysTick 正确 | — |
| 3 | 中断临界区 | PRIMASK 保存/恢复正常 | — |
| 4 | CRC16 | 规范向量 `11 03 006B 0003` → `0x8776` | — |
| 5 | 协议回环 FC6 | 写单个保持寄存器 | — |
| 6 | 协议回环 FC3 | 一次读回 3 个保持寄存器 | — |
| 7 | 协议回环 FC16 | 一次写 4 个保持寄存器 | — |
| 8 | 协议回环 FC5 | 线圈置位/复位（`0xFF00`/`0x0000` 两个线上取值） | — |
| 9 | 协议回环 FC1 | 读 10 个线圈，位按 LSB 在前打包 | — |
| 10 | 协议回环 FC15 | 一次写 5 个线圈 | — |
| 11 | 异常应答 | 越界地址回 `01 83 02` 且 CRC 自洽 | — |
| 12 | 广播 | 写入生效、从站不应答 | — |
| 13 | CRC 坏静默 | 从站保持静默、错误计入统计 | — |
| 14 | 主站超时 | 对端不存在时得到 `MB_ERR_TIME_OUT` | — |
| 15 | RS-485 方向 | DE 电平跟随 `bsp_rs485_tx()`，连续翻转不卡死 | — |
| 16 | 统计与钩子 | 收发帧计数、全局诊断钩子回调 | — |
| 17 | UART 自环 | 真实 Modbus 请求帧逐字节回环 | **PA9–PA10 短接** |

1–16 不需要任何外部接线，烧进去就能看结果——它们的价值是把整条协议栈
（含 32 位对齐、位打包、帧长校验）放到 Cortex-M4 上跑一遍，而不是只在 PC 上验证。

## 接线

| 信号 | 引脚 | 接到 |
|---|---|---|
| USART1_TX | PA9  | USB-TTL 的 RX |
| USART1_RX | PA10 | USB-TTL 的 TX（**仅在看日志时需要；做 17 号自环测试时必须断开**） |
| RS-485 DE/RE | PD4 | MAX485 的 DE 与 /RE（高电平发送） |
| LED | PA6 | 板载 LED（低电平点亮） |

- 串口：**115200 8N1**，无流控。
- 只跑 1–16 号用例时，USB-TTL 都不用接。
- 跑 17 号（UART 自环）时，用一根杜邦线把 **PA9 与 PA10 直接短接**（绕过 RS-485 收发器）。
  此时**必须断开 USB-TTL 的 TX**（它会去驱 PA10）：要么整个 USB-TTL 拔掉，
  要么只把 USB-TTL 的 RX 接到 PA9 用来看日志。两个推挽输出对驱 PA10 会短接两个驱动器。
  没接跳线不会算失败，会打印 skip；接了跳线却收不到字节则会判 **FAIL**（固件会先探测跳线）。

> HSE 频率：默认按 **8 MHz** 晶振。部分 F407 核心板是 25 MHz——把 bsp.h 里的
> BSP_HSE_HZ 改成 25000000 即可，board.c 会据此推导 PLLM，其余参数不变。

## 构建

```
cmake -S tests/stm32f407ve -B build-stm32f407ve \
      -DCMAKE_TOOLCHAIN_FILE=tests/stm32f407ve/cmake/arm-none-eabi.cmake
cmake --build build-stm32f407ve
```

产物：build-stm32f407ve/modbus_stm32f407ve_test.{elf,bin,hex}。
构建末段会打印各段体积（Flash / RAM / CCMRAM 占用）。

## 烧录与运行

接好 ST-Link 之后：

```
# 方式一：openocd（CMake 已注册 flash 目标）
cmake --build build-stm32f407ve --target flash

# 方式二：手工
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program build-stm32f407ve/modbus_stm32f407ve_test.elf verify reset exit"
```

然后用串口终端（或 picocom -b 115200 /dev/ttyACM0）看输出，形如：

```
Modbus RTU · STM32F407VE 自测
SYSCLK = 168000000 Hz

[ ok ] 时钟：已切到 168 MHz
[ ok ] 时基：10 ms 延时实测 10 ms
[ ok ] 中断临界区：PRIMASK 保存/恢复正常
[ ok ] CRC16：规范样例 0x8776
[ ok ] 协议回环 FC6：写单个寄存器
[ ok ] 协议回环 FC3：读回 3 个寄存器
[ ok ] 协议回环 FC16：写 4 个寄存器
[ ok ] 协议回环 FC5：置位/复位线圈
[ ok ] 协议回环 FC1：读 10 个线圈
[ ok ] 协议回环 FC15：写 5 个线圈
[ ok ] 异常应答：01 83 02，CRC 自洽
[ ok ] 广播：写入生效且不应答
[ ok ] CRC 坏静默：保持静默，错误计入统计
[ ok ] 主站超时：MB_ERR_TIME_OUT
[ ok ] RS-485 方向：DE 电平跟随
[ ok ] 统计与钩子：计数与钩子正常
[skip] UART 自环：未收到字节（PA9/PA10 未短接）

断言：76 项，失败 0 项
----------------------------------------
结果：16 通过，0 失败，1 跳过
```

全部通过后 LED 每 500 ms 翻转一次；有失败则 LED 常亮。

## 没有硬件时能验证到什么

这台开发环境里没有 ST-Link、没有串口、也没有 qemu，所以**板端行为未经实机验证**。
可以离线验证的是：

1. **交叉编译零告警**：-Wall -Wextra -Wpedantic -Werror，GCC 14.2.1。
2. **向量表的正确性**：反汇编确认表长 98 项（16 系统异常 + 82 外设中断），
   第 0 项 = 0x20020000（128 KB SRAM 末端），Reset/SysTick/USART1 分别落在
   第 1/15/53 项且带 Thumb 位。
3. **链接布局**：.isr_vector 位于 0x08000000、.data 的加载地址在 Flash、
   .bss 在 RAM，链接脚本给的 4 KB 最小栈会在 RAM 不够时直接报错。
4. **寄存器常量**：tools/check_regs.py 把手写头与 STM32CubeF4 的 CMSIS 设备头
   逐项对拍；结构体偏移另有 header 内的编译期断言兜底。
5. **自测逻辑本身**（可复跑）：`sh tests/stm32f407ve/tools/host_sim.sh` 用宿主 gcc
   链接**真实的库源码** + 桩 BSP（`tools/host_stub.c`）跑一遍 main.c，输出与上节示例
   完全一致：16 通过 / 0 失败 / 1 跳过、76 项断言。也就是说上板前，用例的期望值
   与流程已经验证过一遍；未被验证的只剩"真实外设是否按寄存器配置工作"。

```
python3 tests/stm32f407ve/tools/check_regs.py /path/to/stm32f407xx.h
# 对拍：56 个同名宏，50 个成功解析，0 个不一致
```

（剩下 6 个是 ((gpio_regs_t *)GPIOA_BASE) 这类指针宏，不参与数值比较。）

## 已知边界

- 依赖板子有 **8 MHz HSE**（或按上面的说明改成你的晶振频率）；HSE 起不来会退回
  HSI 16 MHz，此时 bsp_clock_ok() 为 false、第 1 条用例会失败，其余用例照常可跑。
- LED / RS-485 DE 的引脚按常见核心板默认值（PA6 / PD4），不匹配就在 bsp.h 里改 BSP_PIN_*；
  board.c 里的引脚配置是按这些宏算位偏移的，改宏即可。但 **LED 极性（低电平点亮）与
  DE 极性（高电平发送）是硬编码在 board.c 的 bsp_led() / bsp_rs485_tx() 里的**，
  板子不一样时改这两个函数。
- 只用了 USART1 一个串口：主站与从站跑在**同一个实例对**上（内存回环），真实总线
  上做主从对话需要两块板，不在本自测范围内。
- 硬浮点 ABI（-mfloat-abi=hard）：Reset_Handler 里已经打开 CP10/CP11，不用担心。
- UART 自环会把 8 字节原始 Modbus 请求帧也写到 PA9 上，同一个串口既当控制台又当自环线时，
  终端日志里会夹进几个非打印字节——这是固有现象，不是 bug。

## 相关文档

- 本工程的独立审查报告与处置记录：[../../AUDIT_REPORT.md](../../AUDIT_REPORT.md)
