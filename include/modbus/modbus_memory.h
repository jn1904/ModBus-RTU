/*
 * modbus_memory.h
 *
 * 内存传输。
 *
 * 捕获库发送的每个字节，并可选地将其转发给对等实例。
 * 它是测试套件使用的传输方式，对于希望在没有硬件的
 * 情况下演练协议的应用（仿真、调试工具、CI）
 * 也很方便。
 */

#ifndef MODBUS_MEMORY_H
#define MODBUS_MEMORY_H

#include <stdint.h>
#include <stdbool.h>

#include "modbus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mb_memory
{
    uint8_t  buf[MB_MAX_FRAME]; /*!< 最后发送的帧 */
    uint16_t len;               /*!< buf 中捕获的字节数  */
    bool     overflow;          /*!< 一次传输未能容纳 */
    bool     tx;                /*!< 当前 set_tx() 状态 */
    mb_t    *peer;              /*!< 接收转发字节的实例，可能为 NULL */
} mb_memory_t;

/**
 * @brief 准备一个内存传输。
 * @param m    要初始化的传输
 * @param peer 接收所有写入内容的实例，可能为 NULL
 *             （此时这些字节仅被捕获以供检查）
 */
void mb_memory_init(mb_memory_t *m, mb_t *peer);

/** @brief 清空捕获缓冲区并清除标志。 */
void mb_memory_reset(mb_memory_t *m);

/** @brief 端口回调：捕获（并转发）一次传输。返回 0。 */
int mb_memory_write(void *user, const uint8_t *buf, uint16_t len);

/** @brief 记录 RS-485 方向的端口回调。 */
void mb_memory_set_tx(void *user, bool tx);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_MEMORY_H */
