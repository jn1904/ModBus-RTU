# ModBus-RTU · STM32F407VE 板端自测工程 独立审查报告

审查者：auditor（独立复核；未修改 tests/ 或 src/ 下任何源码）
审查时间：2026-10-21 18:10 CST
审查快照（sha256 前 16 位）：
- tests/stm32f407ve/README.md 42467c5fdaae709d（审查期间 17:59 被更新为 17 用例版本，本报告按当前版本行号）
- src/board.c 55bc2da4cd3b52fb、src/main.c 7089a7bc40f3f6e0、include/bsp.h 44dc5f82530ac6a5
- include/stm32f407ve_regs.h 1df8d53a8a4e4e30、startup/startup_stm32f407xx.s 6eeee3cff34e5d9e
- linker/stm32f407ve.ld 2b97e97d668b9f4a、tests CMakeLists db8f7ce96250a211、toolchain 5c0d267b7f7b08d6
- tools/check_regs.py d0075be8066885be、根 README.md 77f5382042a9ef0d、src/core/frame.c ae8dd923018bce3d

---

## 一、确定是问题（与硬件无关，已查实）

### A1【文档】用例编号在 13→17 改版后残留（tests/stm32f407ve/README.md:44、57、58）
- 表格已改成 17 条（与 main.c:948-964 完全对应），但正文仍写：README:44 “1–10、12–13 不需要任何外部接线”；README:57 “只跑 1–10 号用例时，USB-TTL 都不用接”；README:58 “跑 11 号用例时……短接”。
- 为什么是问题：现在 #11 是异常应答、#14 是主站超时、#15 RS-485、#16 统计，真正需要跳线的是 #17 UART 自环。按 44/57 行执行会误导 11–16 的接线要求。
- 建议：正文统一改为“1–16 不需要接线，只有 #17 需要 PA9–PA10 跳线”，或直接用用例名代替编号。

### A2【文档/注释】“8 个功能码”与实现不符（根 README.md:80、src/main.c:9）
- 根 README.md:80：“内存回环覆盖 8 个功能码”；main.c:9 同样写“8 个功能码”。
- 实际 main.c 只测 FC1/FC3/FC5/FC6/FC15/FC16 共 6 个；grep main.c 没有 MB_FC_READ_DISCRETE_INPUTS / MB_FC_READ_INPUT_REGS。FC2/FC4 只在宿主 tests/test_modbus.c 覆盖（宿主输出有“FC2 / FC4 / FC15”一条）。
- 建议：改为“6 个功能码（FC2/FC4 由宿主机测试覆盖）”，或在板端补 FC2/FC4 用例后再写 8。

### A3【注释】关于 libc 的说法与链接事实相反（src/main.c:34-35）
- 注释称“本工程的链接脚本会把 libc.a 整个丢掉，任何一次 printf 都会让链接失败”。
- 事实：linker/stm32f407ve.ld:97-98 明确写“不要 DISCARD libc/libgcc”；实际 ELF 中 arm-none-eabi-nm 显示 08000188 T memcpy、080002bc T memset，map 里拉入了 libc_nano.a(libc_a-memcpy.o)、libc_a-memset.o；--specs=nosys.specs 下 printf 会链接（_write 桩返回错误），不会因丢 libc 而链接失败。
- 建议：改为“libc 仍会链接（目前只用 memcpy/memset）；printf 会拉入 stdio/格式化代码且无有效 _write，禁止使用”。

### A4【构建】板端库源码实际 -O0，只有 board.c/main.c 是 -O2（tests CMakeLists.txt:44-47 + 根 CMakeLists.txt:28-48）
- 交叉工程未设 CMAKE_BUILD_TYPE；-O2 -g3 只加在可执行目标（main.c/board.c），根工程的 modbus 库目标只有告警选项、没有 -O。
- 实测 build-audit 的 flags.make：库 C_FLAGS = ... -std=c99 -Wall -Wextra -Wpedantic -Werror（无 -O，即 -O0）；可执行 C_FLAGS 末尾有 -O2 -g3。
- 为什么是问题：上板自测验证的是未优化库，-O2 下才可能暴露的 strict-aliasing/volatile/codegen 问题未覆盖，且库速度/体积不代表发布形态。
- 建议：交叉工程里给 modbus 目标加 -O2（或显式设置 Release 构建类型并确认工具链 flags 不冲突），与发布构建一致。

### A5【文档/接线安全】README 描述的 #17 接线会让两个推挽输出对驱动（tests README.md:51-54、58）
- README:51-52 要求 PA9→USB-TTL RX、PA10←USB-TTL TX；README:58 又要求跑 #17 时把 PA9 与 PA10 直接短接；README:53 还让 PD4 接 MAX485，若 MAX485 的 RO/DI 照常接在 PA10/PA9，则 RO 也在驱动 PA10。
- 为什么是问题：照字面接线时，MCU 的 PA9 推挽输出与 USB-TTL TX（以及 MAX485 RO）硬短接，轻则 #17 数据错乱/假失败，重则长时间对驱动过流。文档至少应明确“不能这样接”。
- 建议：明确写“跑 #17 时只接 USB-TTL 的 RX 到 PA9，断开 TX；MAX485 的 RO/DI 也不要接”，或改用 GPIO 检测跳线（见 B5）。

---

## 二、依赖硬件才能确认 / 可疑（已做代码审读与宿主仿真）

### B1【伪通过风险】时钟用例是“自证”，不是“实测频率相符”（src/main.c:453-471、477-496；tests README:26-27）
- 用例 1 只比较 bsp_sysclk_hz() == BSP_SYSCLK_HZ 且 bsp_clock_ok()；而 bsp_sysclk_hz() 返回的正是 board.c:187 赋的常量 BSP_SYSCLK_HZ，bsp_clock_ok() 只表示 SWS 已确认切到 PLL。若 PLLN/PLLP 或 HSE 假设有误、但 PLLRDY/SWS 仍正常，实际主频不是 168 MHz 时该用例仍会通过。
- 用例 2 的延时与测量都基于同一个 SysTick：SysTick 整体频率错时，bsp_delay_ms(10) 量出来仍是 10 ms，所以不能证明“1 ms SysTick 正确”。
- README:26 的“且实测频率相符”和 27 的“1 ms SysTick 正确”属于过度表述。板上唯一真正独立的外部标尺是“115200 终端能否正常显示”。
- 建议：读回 RCC->PLLCFGR/CFGR 断言字段等于期望，并由读回值计算 SYSCLK 与 BSP_SYSCLK_HZ 比较；README 改为“配置/切换状态检查”，并写明“终端首行可读即说明时钟与波特率正确”。

### B2【可疑】PLL 推导硬编码，缺少非整数 MHz HSE 保护（src/board.c:38-42、187-188）
- BSP_PLLM = BSP_HSE_HZ / BSP_PLL_VCO_IN_HZ 是整数除法，PLLN=336/PLLP=2 固定，s_sysclk_hz 直接赋 168 MHz。
- 8/25 MHz 都成立（25/25=1）；若按 README 的“改 BSP_HSE_HZ”指引改成 12.288 MHz（部分板），PLLM=12 → VCO 输入 1.024 MHz → 实际约 172.032 MHz（超频），而程式仍报告 168 MHz，SysTick/UART 全偏。
- 建议：加静态断言 BSP_HSE_HZ % 1000000u == 0u，并用 (BSP_HSE_HZ/BSP_PLLM)*BSP_PLLN/PLLP == BSP_SYSCLK_HZ 把“报告频率=实际频率”钉死。

### B3【可疑】启动文件未设置 VTOR（startup/startup_stm32f407xx.s:140-179）
- Reset_Handler 没有写 SCB->VTOR。默认从主 Flash 启动（BOOT0=0）时 VTOR=0，经 0x00000000 别名取到同一张表，能工作；但若由 bootloader/调试器跳转到 0x08000000 且 VTOR 仍指向旧表，SysTick/USART1 中断会进错 handler。需实机/启动路径确认。
- 建议：Reset_Handler 里加 SCB->VTOR = 0x08000000（一条 ldr/str）。

### B4【覆盖缺口】真实硬件传输路径没有任何用例覆盖（src/main.c:347-348、357-358；src/board.c:483-496）
- main.c 的主站/从站都用 mb_memory_write/set_tx（内存回环），唯一被用作回调的是 bsp_mb_millis；--gc-sections 实际把 .text.bsp_mb_write、.text.bsp_mb_set_tx、.text.bsp_uart_rx_dropped 全丢弃（build-audit 的 nm 里没有 bsp_mb_write/set_tx，map 中标记 discarded）。
- 因此以下“库与板级的接缝”从未在板上跑过：bsp_uart_write 的 TXE/TC 等待（TC 等待是 DE 拉低时序的关键）、USART1_IRQHandler→环形缓冲→mb_rx、缓冲满丢字节。现状 #17 是裸 UART 回环（不经过库），#15 只翻转 GPIO。
- 建议：增加用例——USART1 中断把字节喂给真实 mb_t 从站，主站查询经 PA9 发、PA10 收、从站库应答；这样 bsp_mb_*、TC/DE 时序与 ISR 路径同时被覆盖（跳线仍在时跑，否则 SKIP）。

### B5【伪通过风险】#17 的“0 字节即 SKIP”会掩盖 RX 通路故障（src/main.c:833-840）
- 现象：rx_len == 0 就 case_skip("未收到字节（PA9/PA10 未短接）")。
- 为什么可疑：PA10 断线、USART RX 配置错、引脚损坏同样表现为 0 字节，会被当成“没接跳线”而显示跳过而非失败。README:121 也承认没有实机，因此这条“跳过”在首板上很可能是真实故障的唯一线索。
- 建议：先做跳线检测——PA9 配输出低、PA10 配输入上拉并读 IDR，若为低说明跳线在；跳线在却 0 字节 → FAIL；不在 → SKIP。或至少检查 200 ms 内的 USART ORE/FE 错误。

### B6【时序结论，已仿真，依赖 SysTick 正常】t35 / 每轮 3 ms / 8 轮 / 超时
- 结论：时序够。bus_pump_once 每轮 bsp_delay_ms(3) 保证 millis 差值 ≥3（t35=2），所以第 1 轮从站定帧、第 2 轮主站定帧；宿主仿真确认所有 do_query rounds=2，8 轮上限绰绰有余（8 轮约 24 ms < 50 ms）。主站超时：timeout=20、delay 25 后 poll，仿真得到 MB_ERR_TIME_OUT(24)。
- 依赖硬件点：以上都依赖 SysTick 中断真的在跑；若 SysTick ISR 没装上，bsp_delay_ms 是死等，现象是整机卡住而不是测试失败（不会被用例捕获）。
- 隐患（当前不触发）：src/main.c:412-417 的兜底若 timeout_ms 以后调大，8 轮+60 ms 可能还没到超时，代码直接返回 MB_ERR_TIME_OUT 却不消费 waiting/has_result，后续查询会被 MB_ERR_POLLING 拒绝。建议改成“poll 到 done 或确认超时”。

### B7【低风险/需实机】其余可疑点
- src/board.c:48：HSE 等待固定 100000 次自旋（HSI 16 MHz 下约 20–30 ms），慢起振晶振可能被误判失败而退回 HSI。
- src/board.c:99-124：若 SWS 一直确认不了，代码保留 PLL/HSE 原状却把 s_sysclk 报成 16 MHz，状态自相矛盾（不会停摆，现实中不应发生）。
- src/board.c:400-403：bsp_uart_flush 只清 ring 的 tail，不排空 USART 已锁存在 DR/RXNE 的字节；#17 前若恰有 echo 字节尚未搬进 ring，会污染逐字节比较。按 bsp_uart_write 等 TC 的时序实际很难触发，属理论窗口，建议 flush 时顺带读一次 SR/DR。
- tests README:132-135 的“宿主桩 BSP 跑 main.c，输出与示例完全一致（16/0/1、67 断言）”：我用自制 stub 独立复现成功（见第四节），但该 stub 没有进仓库，读者无法复跑；建议放在 tests/stm32f407ve/tools/host_stub/ 并加 host 构建目标。
- tests README:153-154 的“自环会把 8 字节原始帧写进终端日志”：实测成立（我宿主复现时原始帧 11 03 00 6B 00 03 76 87 会混在输出里），这条说明准确。

---

## 三、风格建议

- S1 GPIO 配置顺序（src/board.c:245-253）：先 MODER=AF 再写 AFR，切换瞬间 PA9 由 AF0 驱动，上电时可能在 TX 线上产生毛刺（外接终端可能看到一个乱字节）。建议先写 AFR 再切 MODER。
- S2 IRQ handler 无原型（src/board.c:203、409）：开 -Wmissing-prototypes 后会报 2 条；建议在 bsp.h 声明 SysTick_Handler / USART1_IRQHandler。
- S3 LED 极性不是宏（src/board.c:454-461；tests README:148-149）：README 说“LED/DE 引脚不匹配就在 bsp.h 改宏”，但低电平点亮是硬编码的，没有极性宏；建议加 BSP_LED_ACTIVE_LOW，或 README 说明极性要改 board.c。
- S4 README:57 措辞：“USB-TTL 都不用接”但那样只能看 LED；若要串口结果仍需接 RX，建议写清。
- S5 内存回环的“自洽”局限：主站/从站都是同一份库，位序/字节序即使实现反了也可能两边自洽通过。建议像异常应答用例那样增加硬编码线上字节断言。我宿主仿真得到的规范黄金向量可直接用：FC15 请求（addr=0x10、count=5、data[0]=0x0015）= 01 0F 00 10 00 05 01 15 6F 5A；FC1 应答（addr=0、count=10、regs[0]=0x0009）= 01 01 02 09 00 BF AC；异常应答 = 01 83 02 C0 F1。
- S6 边界覆盖（src/main.c:609-620、627-637）：FC1 只测 bits 0/3（<8），FC15 也未跨字节/跨 16 位边界；建议补 count>8、addr 跨 16 位边界的用例。
- S7 check_regs.py 非强制（tools/check_regs.py:69-74）：找不到 CMSIS 头时 exit 0，CI 可能静默跳过；建议加 --strict。另它只比较 53 个同名宏中的 47 个（GPIOA/RCC/USART1 等 6 个指针宏未解析），PLLCFGR 的 *_POS 等只在本文件出现的宏不参与比较——这部分我已用反汇编/手工核对。
- S8 hook 断言过弱（src/main.c:928-929）：g_hook_calls >= 1 即使成功路径误报一次错也会通过；建议断言恰好 +1，并在 hook 中记录 err/where。
- S9 私有头依赖：main.c 直接 include core/mb_core.h 构造/投喂裸帧，对自测合理，但板端工程依赖私有 API，README 可注明。

---

## 四、四项必做验证（实际命令 + 关键输出）

### 1) 从零构建（独立目录 build-audit，未碰已有 build-*）
命令：
    rm -rf build-audit
    cmake -S tests/stm32f407ve -B build-audit -DCMAKE_TOOLCHAIN_FILE=$PWD/tests/stm32f407ve/cmake/arm-none-eabi.cmake
    cmake --build build-audit
结果：exit 0；无编译器 warning/error（grep -iE 'warning|error' 唯一命中是文件名 modbus_error.c.obj）。
体积：FLASH 28752 B / 512 KB（5.48%），RAM 7008 B / 128 KB（5.35%），CCMRAM 0 B。

### 2) ELF / 向量表 / 符号 / 布局
- .isr_vector = 0x188 = 392 B = 98 项；w[0]=0x20020000；w[1]=0x08000359（Reset_Handler 0x08000358 + Thumb）；w[15]=0x08000415（SysTick 0x08000414 + Thumb）；w[53]=0x080006a5（USART1 0x080006a4 + Thumb）。
- arm-none-eabi-nm -u 输出 0 行。
- 全表核对：81 个具名中断 + 1 个 reserved（IRQ79）与 CMSIS stm32f407xx.h 的 IRQn 0..81 顺序逐名比对，无不一致。
- readelf -l：.data LMA=0x08007048 / VMA=0x20000000，.bss NOBITS 0x20000008..0x20000b5c，_estack=0x20020000，._user_heap_stack 0x1004（4 KB 栈保留）。
- 反汇编 Reset_Handler 确认：MSP=_estack、CPACR |= 0xF<<20、.data 8 B 从 0x08007048 拷贝、.bss 清零、bl main。
- ARM 属性：Tag_CPU_arch v7E-M、Tag_FP_arch VFPv4-D16、Tag_ABI_VFP_args VFP registers（硬浮点 ABI 确认）。

### 3) 寄存器对拍
命令：python3 tests/stm32f407ve/tools/check_regs.py /home/o431/zephyrproject/modules/hal/stm32/stm32cube/stm32f4xx/soc/stm32f407xx.h
输出：“对拍：53 个同名宏，47 个成功解析，0 个不一致”，exit 0。
负向对照：把 stm32f407ve_regs.h 副本里的 PPRE1_DIV4 从 0x1400 改成 0xC00 后再跑 → “1 个不一致”、exit 1，证明脚本不是永远 PASS。
补充（宿主 gcc offsetof）：rcc PLLCFGR/CFGR/AHB1ENR/APB1ENR/APB2ENR = 4/8/48/64/68；gpio 各偏移 = 0/4/8/12/16/20/24/28/32；usart = 0/4/8/12，与 header 内编译期断言一致。BRR@84MHz=729、@16MHz=139；SysTick LOAD=167999。

### 4) 宿主机回归
命令：
    rm -rf build-audit-host
    cmake -S . -B build-audit-host -DBUILD_TESTING=ON
    cmake --build build-audit-host
    ctest --test-dir build-audit-host --output-on-failure
结果：100% tests passed, 0 tests failed；直接运行 build-audit-host/modbus_tests 打印“13 个用例，165 项断言，0 项失败”。

### 额外实际执行（交叉验证）
- 反汇编常量核对（build-audit）：PLLCFGR=0x07405408（PLLM=8、PLLN=336、PLLP=/2、PLLSRC=HSE、PLLQ=7）；FLASH_ACR=0x705；CFGR=0x9400；USART1 BRR=0x2D9(729)；SysTick LOAD=0x28FFF(167999)；NVIC IPR 地址 0xE000E424 写入 0x8000（IRQ37 优先级 8）、ISER 0xE000E104 写入 0x20（bit5）；PA9/PA10 AFR1 分别写 0x70/0x700（AF7）。
- 额外严格告警复编译（-Wconversion -Wsign-conversion -Wshadow -Wcast-align -Wdouble-promotion -Wstrict-overflow=2 -Wmissing-prototypes -Wundef -Wcast-qual）与 -fanalyzer：board.c/main.c 除 2 条 IRQ handler missing-prototype 外无告警，analyzer 无告警。
- 宿主仿真 main.c 的 do_query/各断言（自写 C 程序连真实库）：40 项检查 0 失败；FC15 请求线上字节 01 0F 00 10 00 05 01 15 6F 5A、FC1 应答 01 01 02 09 00 BF AC、异常帧 01 83 02 C0 F1（CRC 自洽）；每笔查询 rounds=2；超时用例返回 24。
- 宿主桩 BSP 直接编译运行真实 src/main.c：输出 16 个 [ ok ] + 1 个 [skip]，打印“断言：67 项，失败 0 项”“结果：16 通过，0 失败，1 跳过”，与更新后的 README 示例一致（原始 8 字节帧也会混进输出，与 README:153-154 说明一致）。

---

## 五、逐项 A–E 对照结论（对应委托要求）

- A 板级寄存器编程：核对通过，未发现功能性 bug。PLL 参数/168 MHz 推导、Flash 5WS 设置时机、CFGR 先预分频后切 PLL、BRR=729（84 MHz/115200）、NVIC IRQ37→ISER1 bit5 / IPR word9 byte1 / 优先级 0x80、AF7 位偏移、SysTick 167999、DE 高=发送、TC 等待，均与 RM0090/CMSIS 一致（反汇编常量见上）。SPSC 环形缓冲：head 仅 ISR 写、tail 仅主循环写，32 位对齐 + volatile 在单核 Cortex-M4 上足够。HSE 失败退路有界，不会死等/停摆；但见 B2/B3/B7。
- B 用例期望值/时序：FC1 位打包期望 buf[0]==0x0009、FC15 写 addr16 后 regs[1]==0x0015、FC3/FC16/FC5/FC6、异常码 2、广播不回、CRC 坏静默、超时 24——全部经宿主仿真用真实库验证正确。t35=2 ms 与每轮 3 ms 足够（每轮 millis 差 ≥3）；8 轮足够（实际 2 轮）；超时用例真会超时。已知伪通过/掩盖点：B1（时钟/时基自证）、B5（0 字节 SKIP）、B4（传输层未覆盖）、S5（回环自洽）。
- C 链接/启动：.data LMA 在 Flash、_sidata 正确、.bss 清零、_estack=0x20020000、4 KB 栈保留、FPU CP10/CP11 在硬浮点 ABI 下位置正确——全部用 readelf/objdump 确认。唯一缺口是 VTOR（B3）。
- D 隐藏依赖/UB：nm -u 为空；无宿主机/新库隐藏依赖；实际链接 libc_nano 的 memcpy/memset（因此 A3 注释错误）；额外严格告警与 -fanalyzer 均干净；未发现对 volatile 的多重读或整型提升 UB。构建层面唯一问题是库 -O0（A4）。
- E README 与实现一致性：更新后的 17 条表格/示例输出/汇总行（16/0/1、67 断言）与实现一致（我用宿主桩复现）；仍不一致的是 A1（正文编号残留）、A2（8 个功能码）、A3（libc 注释）、B1（“实测频率相符”过强）、S3（LED 极性说“改宏”）。

---

## 六、明确未覆盖（不要当作已验证）

- 没有任何硬件：真实时钟/PLL 锁定、HSE 起振、USART 电气与波特率、RS-485 差分/DE 时序、NVIC 实际抢占行为均未实测。
- 未在硬件上执行 HSE/PLL 失败退路（board.c 的寄存器代码无法在宿主运行），只做了代码审读与反汇编核对。
- board.c 只做了交叉编译 + 反汇编核对；main.c 用宿主桩跑过，但宿主桩是我自写的临时文件，不在仓库内。
- 未做逻辑分析仪/示波器/总线抓包，未验证 3.3V/5V 电平与 MAX485 实际接线。
- VTOR 是否真的造成问题取决于实际启动路径，软件侧无法判定。

---

## 七、总体结论与上板前最该先确认的三件事

总体结论：在没有硬件的情况下，这个工程的可信度属于中高——构建/链接/向量表/寄存器常量/协议核心行为/17 个用例的期望值与流程都被独立复核过，且 main.c 用例逻辑在宿主上复现出与 README 完全相同的 16 通过/1 跳过/67 断言；board.c 的时钟/USART/SysTick/NVIC/环形缓冲经反汇编与寄存器推导核对未发现功能性错误。剩余风险集中在“真实外设是否按配置工作”和“接线/启动路径”上，而不是协议逻辑或用例期望值。

上板前最该先确认的三件事：
1. 晶振频率与时钟：实际 HSE 是否等于 BSP_HSE_HZ（8 MHz 还是 25 MHz，必须二选一改对）；上电后串口首行能否在 115200 下正常读出 SYSCLK = 168000000 Hz。若显示 16000000 或乱码，说明时钟/波特率有问题，后续所有用例结论都不可信。
2. 自环接线安全：跑 #17 前断开 USB-TTL 的 TX（只需 RX）以及 MAX485 的 RO/DI，避免 PA9/PA10 上两个推挽输出对驱动；同时确认 PD4 高=发送、PA6 低=点亮与板子一致。
3. 启动路径/向量表：确认从 0x08000000 主 Flash 启动（BOOT0=0 或 ST-Link reset）；若经 bootloader 跳转，先补写 SCB->VTOR = 0x08000000。上电后用调试器确认 0x08000000 首字为 0x20020000、0x08000004 是带 Thumb 的 Reset_Handler（离线已确认，实机再复核一次）。

---

## 处置记录（Lead 复核后）

A 类问题全部修复；B 类中可在无硬件条件下修掉的已修，其余保留为明确限制。

| 编号 | 处置 | 说明 |
|---|---|---|
| A1 | 已修 | README 用例编号改为 1–16 不需要接线、#17 为 UART 自环 |
| A2 | 已修 | 板端实际覆盖 6 个功能码（FC1/3/5/6/15/16）；根 README 与 main.c 注释已改，FC2/FC4 由宿主测试覆盖 |
| A3 | 已修 | main.c 关于 libc 的注释改正：nano.specs 链接 libc_nano，链接器并不丢弃 libc |
| A4 | 已修 | 交叉工程默认 CMAKE_BUILD_TYPE=Release；实测库与用例均为 -O3（flags.make） |
| A5 | 已修 | README 明确自环测试必须断开 USB-TTL TX，避免两个推挽输出对驱 PA10 |
| B1 | 已修 | 时钟用例改为从 RCC 读回 PLLCFGR/CFGR/FLASH->ACR 重算 SYSCLK 并断言各分频 |
| B2 | 已修 | board.c 增加编译期 #error：HSE 必须是 1 MHz 整数倍、推导 SYSCLK 必须等于 BSP_SYSCLK_HZ |
| B3 | 已修 | Reset_Handler 显式写 SCB->VTOR=0x08000000（反汇编已确认） |
| B4 | 部分修复 | UART 自环改走 bsp_mb_set_tx/bsp_mb_write，引擎真实 DE→发送→等 TC 调用链被覆盖；"USART 中断→环形缓冲→mb_rx"需两块板，已记为限制 |
| B5 | 已修 | 新增 bsp_loopback_probe()：跳线在却收不到字节判 FAIL，未接跳线才 skip |
| B6 | 已修 | do_query 兜底改为等主站自身超时并取走结果，不再残留 waiting |
| B7 | 部分保留 | HSE 自旋上限、SWS 失败分支未改（只有实机才有判断依据）；宿主桩已入库为 tools/host_stub.c + host_sim.sh，可复跑 |
| S1 | 已修 | GPIO 初始化改为先配 AFR/上下拉、最后切 MODER=AF，消除 PA9 切换瞬间毛刺 |
| S2 | 已修 | board.c 为两个 ISR 增加前置声明 |
| S3 | 已修 | README 明确 LED/DE 极性是 board.c 硬编码的，需要时改函数而非改宏 |
| S4 | 已修 | README 说明看日志需接 USB-TTL RX→PA9，自环测试时断开 TX |
| S5/S6 | 保留 | 位域边界与硬编码黄金向量已由宿主测试在 core 层覆盖，板端不重复 |
| S7 | 保留 | check_regs.py 缺 CMSIS 头时跳过（无厂商 SDK 的机器不应失败） |
| S8 | 保留 | 钩子断言保持 >=1，未来增加错误上报点时不误报 |
| S9 | 保留 | main.c 依赖 core 私有头是刻意为之（要直接验证帧构造），CMake 已把 src 加入包含路径 |

复核后重新验证：交叉构建 0 告警（Flash 38,828 B / RAM 7,008 B）、向量表 98 项与 SP/VTOR 不变、
check_regs.py 53 同名宏/47 解析/0 不一致、宿主 ctest 全过、tools/host_sim.sh 16 通过 / 0 失败 / 1 跳过 / 76 断言。

