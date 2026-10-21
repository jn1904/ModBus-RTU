/*
 * modbus.h
 *
 * 可移植的 Modbus RTU 主站与从站库。
 *
 * 本库不依赖操作系统、RTOS 或
 * 硬件抽象层。所有与外界交互的部分
 * 都通过 mb_cfg_t 中的回调完成，时间由
 * 应用通过毫秒时钟提供。
 *
 * 典型从站：
 *
 *     static mb_t mb;
 *     mb_map_t map;
 *     mb_map_shared(&map, holding_regs, 10);
 *
 *     mb_cfg_t cfg = {
 *         .mode     = MB_MODE_SLAVE,
 *         .write    = uart_write,      // 把字节发送到总线上
 *         .set_tx   = uart_set_dir,    // 可选，RS-485 方向控制
 *         .millis   = HAL_GetTick,     // 单调递增的毫秒时钟
 *         .user     = &uart,
 *         .slave_id = 1,
 *         .map      = &map,
 *     };
 *     mb_init(&mb, &cfg);
 *
 *     for (;;) {
 *         mb_rx(&mb, rx_bytes, rx_len);   // 来自 UART 接收路径
 *         mb_poll(&mb);                   // 组帧、处理、应答
 *     }
 *
 * 典型主站：
 *
 *     uint16_t values[4];
 *     mb_query_t q = {
 *         .slave_id = 7, .fc = MB_FC_READ_HOLDING_REGS,
 *         .addr = 0, .count = 4, .data = values,
 *     };
 *
 *     mb_master_send(&mb, &q);
 *     for (;;) {
 *         mb_poll(&mb);
 *         mb_err_t err;
 *         if (mb_master_done(&mb, &err)) {
 *             // err == MB_OK：values[] 中存放应答数据
 *             break;
 *         }
 *     }
 *
 * 并发：一个实例只能在单一上下文中使用。如果
 * 字节来自中断，不要在另一上下文中同时运行 mb_poll()
 * （在 mb_poll() 前后关闭接收中断，
 * 或从同一上下文调用 mb_poll()）。
 */

#ifndef MODBUS_MODBUS_H
#define MODBUS_MODBUS_H

#include <stdint.h>
#include <stdbool.h>

#include "modbus_config.h"
#include "modbus_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * 功能码
 * ------------------------------------------------------------------- */

typedef enum mb_fc{
    MB_FC_READ_COILS          = 1,  /*!< 读取线圈                    */
    MB_FC_READ_DISCRETE_INPUTS= 2,  /*!< 读取离散输入                */
    MB_FC_READ_HOLDING_REGS   = 3,  /*!< 读取保持寄存器              */
    MB_FC_READ_INPUT_REGS     = 4,  /*!< 读取输入寄存器              */
    MB_FC_WRITE_COIL          = 5,  /*!< 写单个线圈                  */
    MB_FC_WRITE_REG           = 6,  /*!< 写单个寄存器                */
    MB_FC_WRITE_COILS         = 15, /*!< 写多个线圈                  */
    MB_FC_WRITE_REGS          = 16, /*!< 写多个寄存器                */
} mb_fc_t;

/* ---------------------------------------------------------------------
 * 数据映射
 *
 * 线圈和离散输入采用位打包：一个 uint16_t 存放 16 位，
 * 最低有效位在前。
 * ------------------------------------------------------------------- */

typedef struct mb_region{
    uint16_t *data;   /*!< 存储，NULL 表示该区域不存在 */
    uint16_t  start;  /*!< 区域第一个合法的 Modbus 地址；位区域的地址单位是“位” */
    uint16_t  count;  /*!< data[] 里 uint16_t 字的个数。
                           注意区域跨度：保持/输入寄存器区域的可寻址范围是 count，
                           线圈/离散输入是位区域，一个 uint16_t 装 16 个位，
                           所以可寻址范围是 count * 16 个位 */
} mb_region_t;

typedef struct mb_map{
    mb_region_t coils;      /*!< FC1、FC5、FC15 */
    mb_region_t discrete;   /*!< FC2           */
    mb_region_t input;      /*!< FC4           */
    mb_region_t holding;    /*!< FC3、FC6、FC16 */
} mb_map_t;

/**
 * @brief 把每个功能码组都绑定到地址 0 开始的同一个共享数组。
 *
 * 这是经典的单内存模型：线圈和寄存器位于
 * 相同的字中。当四个区域需要
 * 独立存储或不同起始地址时，请分别使用 mb_map_t 的各个成员。
 */
void mb_map_shared(mb_map_t *map, uint16_t *regs, uint16_t count);

/* ---------------------------------------------------------------------
 * 帧工作区
 *
 * 仅因为 mb_t 内嵌它而公开；对它的操作位于
 * 私有的核心层中。
 * ------------------------------------------------------------------- */

typedef struct mb_frame{
    uint8_t  buf[MB_MAX_FRAME];
    uint16_t len;
} mb_frame_t;

/* ---------------------------------------------------------------------
 * 主站查询
 * ------------------------------------------------------------------- */

typedef struct mb_query{
    uint8_t   slave_id;  /*!< 1..247 */
    uint8_t   fc;        /*!< mb_fc_t */
    uint16_t  addr;      /*!< 第一个线圈/寄存器地址 */
    uint16_t  count;     /*!< 线圈或寄存器的数量 */
    uint16_t *data;      /*!< 读：存放应答数据的缓冲区；写：待写入数据的来源。
                              FC5/FC6 这类单点写只读取 data[0]。
                              该缓冲区在本次查询结果被 mb_master_done() 取走之前必须保持有效。 */
} mb_query_t;

/* ---------------------------------------------------------------------
 * 配置
 * ------------------------------------------------------------------- */

typedef enum mb_mode{
    MB_MODE_SLAVE  = 0,
    MB_MODE_MASTER = 1,
} mb_mode_t;

typedef struct mb_cfg{
    mb_mode_t mode;

    /* 传输层。write() 在字节真正发送到总线上之前不能返回：
     * 本库会在它返回后立即
     * 把 RS-485 方向切回接收。成功时返回 0。 */
    int  (*write)(void *user, const uint8_t *buf, uint16_t len);
    void (*set_tx)(void *user, bool tx);   /*!< 可选，可为 NULL       */

    /* 单调递增的毫秒时钟，用于 T3.5 和查询超时。 */
    uint32_t (*millis)(void *user);

    void *user;                /*!< 传递给上面每个回调          */

    /* 仅从站使用。 */
    uint8_t        slave_id;   /*!< 1..247                               */
    const mb_map_t *map;       /*!< 借用，其生命周期必须长于实例  */

    /* 0 表示使用库默认值（见 modbus_config.h）。 */
    uint16_t t35_ms;
    uint16_t timeout_ms;
} mb_cfg_t;

/* ---------------------------------------------------------------------
 * 实例
 *
 * 静态分配（或分配在栈上）后调用 mb_init()。初始化之后
 * 再使用下面的访问函数；这些字段之所以公开，是为了让
 * 实例无需堆即可被内嵌。
 * ------------------------------------------------------------------- */

typedef struct mb mb_t;

struct mb{
    mb_cfg_t cfg;              /*!< 配置副本 */

    /* 统计信息。 */
    mb_err_t last_error;
    uint16_t err_count;
    uint16_t in_count;         /*!< 已接收的帧数   */
    uint16_t out_count;        /*!< 已发送的帧数 */

    /* 接收路径：字节在这里累积，直到 T3.5 静默将其定格。 */
    uint8_t  rx[MB_MAX_FRAME];
    uint16_t rx_len;
    uint32_t rx_last_ms;
    bool     rx_overflow;

    /* 当前正在校验或处理的帧。 */
    mb_frame_t work;
    bool     frame_ready;

    /* 从站。 */
    bool     broadcast;

    /* 主站。 */
    mb_query_t query;
    uint32_t   deadline;
    mb_err_t   result;
    bool       has_result;
    bool       waiting;
};

/* ---------------------------------------------------------------------
 * 生命周期与数据路径
 * ------------------------------------------------------------------- */

/**
 * @brief 校验配置并准备一个实例。
 * @param mb   要初始化的存储
 * @param cfg  配置，会被复制进 mb（map 指针为借用）
 * @return MB_OK 或 MB_ERR_BAD_CONFIG（配置不可用时统一返回该码）
 */
mb_err_t mb_init(mb_t *mb, const mb_cfg_t *cfg);

/**
 * @brief 把接收到的字节交给本库。
 *
 * 对从总线上到达的每一个数据块都调用它（UART 中断、CDC
 * 回调、测试框架）。这些字节会被缓冲，直到 mb_poll()
 * 检测到的 T3.5 静默把它们变成一帧。
 */
void mb_rx(mb_t *mb, const uint8_t *data, uint16_t len);

/**
 * @brief 驱动本库向前运行。在循环中调用它。
 *
 * 检测帧结束（T3.5），运行从站状态机，推进
 * 主站查询及其超时。从不阻塞。
 */
void mb_poll(mb_t *mb);

/* ---------------------------------------------------------------------
 * 主站 API
 * ------------------------------------------------------------------- */

/**
 * @brief 排队并发送一次查询。
 * @return MB_OK、MB_ERR_NOT_MASTER、MB_ERR_POLLING（上一次查询尚未
 *         被取走）、MB_ERR_BAD_SLAVE_ID、MB_ERR_NO_DATA、
 *         MB_ERR_BAD_SIZE 或 MB_ERR_TRANSPORT
 */
mb_err_t mb_master_send(mb_t *mb, const mb_query_t *q);

/** @brief 当有查询正在进行时返回 true。 */
bool mb_master_busy(const mb_t *mb);

/**
 * @brief 取出上一次查询的结果。
 * @param err  成功时接收到 MB_OK，否则为失败码，可为 NULL
 * @return 当有结果待取走时返回 true（该结果被本次调用消费）
 */
bool mb_master_done(mb_t *mb, mb_err_t *err);

/* ---------------------------------------------------------------------
 * 统计信息
 * ------------------------------------------------------------------- */

mb_err_t mb_last_error(const mb_t *mb);
uint16_t mb_err_count(const mb_t *mb);
uint16_t mb_in_count(const mb_t *mb);
uint16_t mb_out_count(const mb_t *mb);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_MODBUS_H */
