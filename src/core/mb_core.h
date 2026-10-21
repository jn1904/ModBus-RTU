/*
 * mb_core.h
 *
 * 协议核心的私有接口：字节工具、内存映像解析、帧的构造/校验/解码，
 * 以及从站功能码的处理函数。
 *
 * 这一层只操作普通缓冲区：不碰时钟、不碰传输层、不做动态分配，
 * 因此可以脱离任何平台单独做单元测试。
 */

#ifndef MB_CORE_H
#define MB_CORE_H

#include "modbus.h"

/* 帧内字段位置（RTU ADU：地址, PDU, CRC16）。 */
enum{
    MB_F_ID       = 0,   /* 从站地址                         */
    MB_F_FUNC     = 1,   /* 功能码                           */
    MB_F_ADD_HI   = 2,   /* 地址高字节                       */
    MB_F_ADD_LO   = 3,   /* 地址低字节                       */
    MB_F_NB_HI    = 4,   /* 数量/取值的高字节                */
    MB_F_NB_LO    = 5,   /* 数量/取值的低字节                */
    MB_F_BYTE_CNT = 6    /* 字节数（仅多线圈/多寄存器写请求有）*/
};

/* 读应答里"字节数"字段的位置（ADU 第 3 字节，与上面的字段枚举无关）。 */
#define MB_F_ACK_BYTES 2u

/* 写应答长度：地址 + 功能码 + 地址 + 数量/取值，不含 CRC。 */
#define MB_F_RESPONSE_SIZE 6
/* 异常应答长度：地址 + 功能码|0x80 + 异常码，不含 CRC。 */
#define MB_F_EXCEPTION_SIZE 3

/* --- 字节工具（crc.c） ------------------------------------------------ */

/** @brief 把两个字节拼成一个 16 位字，hi 是高字节。 */
uint16_t mb_word(uint8_t hi, uint8_t lo);

/**
 * @brief Modbus CRC-16。
 * 返回"自然"的 CRC 值；上线时按低字节在前的方式发送。
 */
uint16_t mb_crc16(const uint8_t *buf, uint16_t len);

/* --- 内存映像（map.c） ------------------------------------------------ */

typedef enum mb_area{
    MB_AREA_COILS = 0,
    MB_AREA_DISCRETE,
    MB_AREA_INPUT,
    MB_AREA_HOLDING
} mb_area_t;

/** @brief 取功能码所对应的内存区域。 */
mb_area_t mb_area_for_fc(uint8_t fc);

/** @brief 取某区域的内存映像描述（data 为 NULL 表示该区域不存在）。 */
const mb_region_t *mb_map_region(const mb_map_t *map, mb_area_t area);

/** @brief 映像里至少有一个区域挂了存储时返回 true。 */
bool mb_map_has_any(const mb_map_t *map);

/* --- 定帧与校验（frame.c） -------------------------------------------- */

/**
 * @brief 在帧尾追加 CRC16（低字节在前），并把帧长加 2。
 * 本文件里所有"构造"类函数都只产出不含 CRC 的帧，统一由调用方用本函数收尾。
 */
void mb_frame_seal(mb_frame_t *f);

/**
 * @brief 校验从站收到的请求帧。
 * @return MB_OK、某个 MB_EXC_* 协议异常码，或某个 MB_ERR_* 链路错误码
 */
mb_err_t mb_frame_validate_request(mb_frame_t *f, const mb_map_t *map);

/**
 * @brief 校验主机收到的应答帧。
 * @return MB_OK、MB_ERR_BAD_CRC、MB_ERR_EXCEPTION、MB_ERR_BAD_SIZE 或 MB_EXC_FUNC_CODE
 */
mb_err_t mb_frame_validate_answer(mb_frame_t *f);

/** @brief 把当前帧就地改写成异常应答（功能码 | 0x80，第 3 字节放异常码）。 */
void mb_frame_build_exception(mb_frame_t *f, uint8_t exception_byte);

/** @brief 把主机查询序列化成请求帧。 @return MB_OK 或用法/长度类错误码 */
mb_err_t mb_frame_build_query(mb_frame_t *f, const mb_query_t *q);

/**
 * @brief 把校验通过的读应答里的数据搬进主机的数据缓冲区。
 * @param dst       目标缓冲区
 * @param dst_words dst 以 16 位字为单位的容量
 * @return MB_OK，或数据放不下时返回 MB_ERR_BAD_SIZE
 */
mb_err_t mb_frame_decode_answer(mb_frame_t *f, uint16_t *dst, uint16_t dst_words);

/* --- 功能码（func.c） -------------------------------------------------- */

/**
 * @brief 执行一个已校验请求的功能码，并把帧改写成应答（尚未追加 CRC）。
 * 广播请求的写入照常生效，但不应答——要不要应答由引擎层决定。
 */
void mb_frame_process_request(mb_frame_t *f, const mb_map_t *map, bool broadcast);

#endif /* MB_CORE_H */
