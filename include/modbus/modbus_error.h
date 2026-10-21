/*
 * modbus_error.h
 *
 * 库的错误分类。
 *
 * 单个枚举涵盖三种不同的代码：
 *   - MB_EXC_*   协议异常（1..4），正是从站放入
 *                异常响应中的字节，
 *   - MB_ERR_*   在校验或交换帧时引发的失败，
 *   - MB_OK      唯一的成功代码。
 *
 * 值 1..4 由 Modbus 规范固定；其余代码是内部代码但保持稳定，
 * 因此应用可以持久化或记录它们。
 *
 * 纯 C 编写，不依赖库的其余部分。
 */

#ifndef MODBUS_ERROR_H
#define MODBUS_ERROR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mb_err{
    MB_OK = 0,

    /* --- Modbus 异常代码（也是异常响应字节） --- */
    MB_EXC_FUNC_CODE  = 1,   /*!< 非法功能                  */
    MB_EXC_ADDR_RANGE = 2,   /*!< 非法数据地址              */
    MB_EXC_REGS_QUANT = 3,   /*!< 非法数据值 / 数量      */
    MB_EXC_EXECUTE    = 4,   /*!< 从站设备故障               */

    /* --- 用法 / 配置错误 --- */
    MB_ERR_NOT_MASTER   = 10, /*!< 从站被要求发起查询   */
    MB_ERR_POLLING      = 11, /*!< 上一个查询尚未被消费         */
    MB_ERR_BAD_CONFIG   = 12, /*!< mb_init() 收到了不可用的配置  */
    MB_ERR_NO_DATA      = 13, /*!< 查询没有数据缓冲区 / 内存映像为空   */
    MB_ERR_BAD_ROLE_ID  = 14, /*!< 从站 id 0 无法寻址从站         */
    MB_ERR_BAD_SLAVE_ID = 15, /*!< 查询目标超出 1..247               */
    MB_ERR_BAD_FC       = 16, /*!< 本库无法发送的功能码     */

    /* --- 帧 / 传输错误 --- */
    MB_ERR_BUFF_OVERFLOW = 20, /*!< 接收缓冲区在帧中途溢出      */
    MB_ERR_BAD_CRC       = 21, /*!< CRC16 不匹配                           */
    MB_ERR_EXCEPTION     = 22, /*!< 对端以异常帧应答     */
    MB_ERR_BAD_SIZE      = 23, /*!< 帧过短或内部不一致*/
    MB_ERR_TIME_OUT      = 24, /*!< 在配置的超时时间内无应答   */
    MB_ERR_TRANSPORT     = 25, /*!< 端口拒绝或传输失败   */
} mb_err_t;

/**
 * @enum mb_err_class_t
 * @brief 调用方应如何处理某个错误。
 *
 * MB_CLASS_PROTO_EXC : 从站拒绝的格式正确的请求；以异常帧
 *                      应答。
 * MB_CLASS_COMM      : 帧不可信（CRC、大小、超时）——
 *                      Modbus 要求保持静默。
 * MB_CLASS_USAGE     : 应用误用了 API。
 * MB_CLASS_CONFIG    : mb_init() 拒绝了该配置。
 */
typedef enum mb_err_class{
    MB_CLASS_OK = 0,
    MB_CLASS_PROTO_EXC,
    MB_CLASS_COMM,
    MB_CLASS_USAGE,
    MB_CLASS_CONFIG,
} mb_err_class_t;

/** 可选的诊断回调，对每个上报的错误调用一次。 */
typedef void (*mb_err_hook_t)(mb_err_t err, const char *where);

/* --- 分类与诊断 ---------------------------------- */

mb_err_class_t mb_err_classify(mb_err_t err);
const char *mb_err_str(mb_err_t err);

/**
 * @brief 将错误映射为 Modbus 异常响应字节。
 * @return 对 MB_CLASS_PROTO_EXC 错误返回 1..4，对其他所有代码返回 0
 *         （含义：不构建异常帧）。
 */
uint8_t mb_err_exception_byte(mb_err_t err);

/* --- 上报 -------------------------------------------------------- */

/** 安装（传入 NULL 则移除）全局错误钩子。 */
void mb_err_set_hook(mb_err_hook_t hook);

/** @brief 上报配置错误：通知钩子，返回 err。 */
mb_err_t mb_err_fail(mb_err_t err, const char *where);

/**
 * @brief 上报运行时错误：将其存入实例统计信息，
 * 通知钩子并返回 err。
 * @param last  指向 mb_t::last_error 的指针，可能为 NULL
 * @param count 指向 mb_t::err_count 的指针，可能为 NULL
 */
mb_err_t mb_err_report(mb_err_t *last, uint16_t *count, mb_err_t err, const char *where);

/** 忘记最后记录的错误（计数器保留）。 */
void mb_err_clear(mb_err_t *last);

#define MB_FAIL(err)         mb_err_fail((err), __func__)
#define MB_REPORT(inst, err) mb_err_report(&(inst)->last_error, &(inst)->err_count, (err), __func__)

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_ERROR_H */
