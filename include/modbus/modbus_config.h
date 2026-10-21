/*
 * modbus_config.h
 *
 * 本库的编译期配置。每个值都可以从
 * 构建系统（target_compile_definitions）覆盖，或在包含
 * 任何库头文件之前定义。
 */

#ifndef MODBUS_CONFIG_H
#define MODBUS_CONFIG_H

/**
 * 每个 mb_t 实例所持有的帧缓冲区字节数。
 * 一个 Modbus RTU ADU 最大为 256 字节（1 字节地址 + 253 字节 PDU + 2 字节 CRC），因此
 * 默认值可以接受规范允许的每一帧。如果
 * 你从不交换大块寄存器数据，可以在小目标上降低它。
 */
#ifndef MB_MAX_FRAME
#define MB_MAX_FRAME 256
#endif

/**
 * 默认的 T3.5 帧间静默（毫秒），当
 * mb_cfg_t::t35_ms 为 0 时使用。5 ms 可覆盖所有波特率直至 19200 bps；
 * 高于该速率时规范把间隔固定为 1.75 ms，因此更快的链路
 * 可以把 mb_cfg_t::t35_ms 设为 2。
 */
#ifndef MB_T35_MS_DEFAULT
#define MB_T35_MS_DEFAULT 5
#endif

/**
 * 默认的主站查询超时（毫秒），当
 * mb_cfg_t::timeout_ms 为 0 时使用。
 */
#ifndef MB_TIMEOUT_MS_DEFAULT
#define MB_TIMEOUT_MS_DEFAULT 1000
#endif

#if MB_MAX_FRAME < 32
#error "MB_MAX_FRAME must be at least 32 bytes"
#endif

#endif /* MODBUS_CONFIG_H */
