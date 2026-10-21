/*
 * frame.c
 *
 * 帧层：CRC 收尾、请求/应答校验、异常应答与查询帧的构造、读应答的数据提取。
 *
 * 本文件只操作裸缓冲区，不碰时钟也不碰串口，因此可以脱离平台做单元测试。
 * 统一约定：所有"构造"类函数只把内容写进 buf[] 并更新 len，CRC 一律由调用
 * 方通过 mb_frame_seal() 追加一次，避免同一帧被盖上两遍 CRC（会让整帧失效）。
 */

#include "core/mb_core.h"

/* 协议规定的单次访问上限（参见 Modbus Application Protocol V1.1b3）。 */
#define MB_MAX_READ_BITS   2000u   /* FC1/FC2 单次最多读 2000 个位       */
#define MB_MAX_READ_REGS    125u   /* FC3/FC4 单次最多读 125 个寄存器    */
#define MB_MAX_WRITE_BITS  1968u   /* FC15 单次最多写 1968 个位          */
#define MB_MAX_WRITE_REGS   123u   /* FC16 单次最多写 123 个寄存器       */

/* FC5 写线圈在线上使用的两个合法取值。 */
#define MB_COIL_ON   0xFF00u
#define MB_COIL_OFF  0x0000u

/* 请求帧最短长度：地址 + 功能码 + 地址(2) + 数量(2) + CRC(2)。 */
#define MB_REQ_MIN_LEN 8u
/* 读应答最短长度：地址 + 功能码 + 字节数 + CRC(2)。 */
#define MB_READ_ACK_MIN_LEN 5u
/* 多写请求里"字节数"之后至少要有的载荷长度（FC15 为 1 字节，FC16 为 2 字节）。 */
#define MB_MULTI_WRITE_MIN_LEN 10u

/* 校验帧尾的 CRC16（线上为低字节在前）。 */
static bool mb_frame_crc_ok(const mb_frame_t *f)
{
    uint16_t crc;

    if (f->len < 4u)
    {
        return false;
    }

    crc = mb_crc16(f->buf, (uint16_t)(f->len - 2u));

    return (f->buf[f->len - 2u] == (uint8_t)(crc & 0x00FFu))
        && (f->buf[f->len - 1u] == (uint8_t)((crc >> 8) & 0x00FFu));
}

void mb_frame_seal(mb_frame_t *f)
{
    uint16_t crc;

    if (f == NULL || (uint16_t)(f->len + 2u) > (uint16_t)MB_MAX_FRAME)
    {
        return;
    }

    crc = mb_crc16(f->buf, f->len);

    f->buf[f->len]      = (uint8_t)(crc & 0x00FFu);          /* 低字节在前 */
    f->buf[f->len + 1u] = (uint8_t)((crc >> 8) & 0x00FFu);   /* 高字节在后 */
    f->len = (uint16_t)(f->len + 2u);
}

mb_err_t mb_frame_validate_request(mb_frame_t *f, const mb_map_t *map)
{
    uint8_t  fc;
    uint16_t addr;
    uint16_t count;      /* 帧里的"数量"字段；FC5/FC6 时它放的是"取值" */
    uint16_t quantity;   /* 本次访问实际覆盖的位/寄存器个数              */
    uint8_t  byte_count = 0u;
    mb_area_t area;
    uint32_t span;
    const mb_region_t *region;

    if (f == NULL)
    {
        return MB_ERR_BAD_SIZE;
    }

    /* 太短或 CRC 不对，属于链路层问题：从站必须保持静默。 */
    if (f->len < MB_REQ_MIN_LEN)
    {
        return MB_ERR_BAD_SIZE;
    }
    if (!mb_frame_crc_ok(f))
    {
        return MB_ERR_BAD_CRC;
    }

    fc    = f->buf[MB_F_FUNC];
    addr  = mb_word(f->buf[MB_F_ADD_HI], f->buf[MB_F_ADD_LO]);
    count = mb_word(f->buf[MB_F_NB_HI],  f->buf[MB_F_NB_LO]);

    /* 先按功能码核对帧长：长度对不上就没必要继续解析。 */
    switch (fc)
    {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
    case MB_FC_READ_HOLDING_REGS:
    case MB_FC_READ_INPUT_REGS:
    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
        if (f->len != MB_REQ_MIN_LEN)
        {
            return MB_ERR_BAD_SIZE;
        }
        break;

    case MB_FC_WRITE_COILS:
    case MB_FC_WRITE_REGS:
        if (f->len < MB_MULTI_WRITE_MIN_LEN)
        {
            return MB_ERR_BAD_SIZE;
        }
        byte_count = f->buf[MB_F_BYTE_CNT];
        /* 整帧长度必须是 9 + 字节数（地址1 + 功能码1 + 地址2 + 数量2 + 字节数1 + 载荷 + CRC2）。 */
        if (f->len != (uint16_t)(MB_REQ_MIN_LEN + 1u + byte_count))
        {
            return MB_ERR_BAD_SIZE;
        }
        break;

    default:
        return MB_EXC_FUNC_CODE;   /* 功能码不认识：回异常码 1 */
    }

    /* 单点写（FC5/FC6）只碰 1 个位/寄存器，此时 count 字段是"取值"。 */
    quantity = ((fc == MB_FC_WRITE_COIL) || (fc == MB_FC_WRITE_REG)) ? 1u : count;

    /* 数量与取值的合法性。 */
    switch (fc)
    {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
        if (count < 1u || count > MB_MAX_READ_BITS)
        {
            return MB_EXC_REGS_QUANT;
        }
        break;

    case MB_FC_READ_HOLDING_REGS:
    case MB_FC_READ_INPUT_REGS:
        if (count < 1u || count > MB_MAX_READ_REGS)
        {
            return MB_EXC_REGS_QUANT;
        }
        break;

    case MB_FC_WRITE_COIL:
        if (count != MB_COIL_ON && count != MB_COIL_OFF)
        {
            return MB_EXC_REGS_QUANT;
        }
        break;

    case MB_FC_WRITE_COILS:
        if (count < 1u || count > MB_MAX_WRITE_BITS)
        {
            return MB_EXC_REGS_QUANT;
        }
        if ((uint16_t)((count + 7u) / 8u) != (uint16_t)byte_count)
        {
            return MB_EXC_REGS_QUANT;   /* 声明的字节数与线圈个数对不上 */
        }
        break;

    case MB_FC_WRITE_REGS:
        if (count < 1u || count > MB_MAX_WRITE_REGS)
        {
            return MB_EXC_REGS_QUANT;
        }
        if ((uint16_t)(count * 2u) != (uint16_t)byte_count)
        {
            return MB_EXC_REGS_QUANT;   /* 声明的字节数与寄存器个数对不上 */
        }
        break;

    default:
        break;   /* FC6 的取值不设额外限制 */
    }

        /* 地址范围校验。
       注意：线圈/离散输入是"位区域"，region->count 的单位是 16 位字，
       一个 16 位字装 16 个位，所以可用的位区间是 count * 16；
       保持/输入寄存器的单位就是字，区间就是 count。
       区域不存在（data == NULL）一律按地址非法处理。 */
    area   = mb_area_for_fc(fc);
    region = mb_map_region(map, area);

    if (region->data == NULL)
    {
        return MB_EXC_ADDR_RANGE;
    }
    if (addr < region->start)
    {
        return MB_EXC_ADDR_RANGE;
    }

    span = ((area == MB_AREA_COILS) || (area == MB_AREA_DISCRETE))
         ? (uint32_t)region->count * 16u
         : (uint32_t)region->count;

    if ((uint32_t)addr + (uint32_t)quantity > (uint32_t)region->start + span)
    {
        return MB_EXC_ADDR_RANGE;
    }

    return MB_OK;
}

mb_err_t mb_frame_validate_answer(mb_frame_t *f)
{
    uint8_t fc;
    uint8_t byte_count;

    if (f == NULL || f->len < MB_READ_ACK_MIN_LEN)
    {
        return MB_ERR_BAD_SIZE;
    }
    if (!mb_frame_crc_ok(f))
    {
        return MB_ERR_BAD_CRC;
    }

    fc = f->buf[MB_F_FUNC];

    /* 功能码最高位为 1 表示异常应答：长度固定，异常码只允许 1..4。 */
    if ((fc & 0x80u) != 0u)
    {
        if (f->len != (uint16_t)(MB_F_EXCEPTION_SIZE + 2u))
        {
            return MB_ERR_BAD_SIZE;
        }
        if (f->buf[MB_F_ADD_HI] < 1u || f->buf[MB_F_ADD_HI] > 4u)
        {
            return MB_ERR_BAD_SIZE;
        }
        return MB_ERR_EXCEPTION;
    }

    switch (fc)
    {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
    case MB_FC_READ_HOLDING_REGS:
    case MB_FC_READ_INPUT_REGS:
        /* 读应答第 3 字节是"之后的字节数"，整帧长度必须自洽。 */
        byte_count = f->buf[MB_F_ACK_BYTES];
        if (byte_count == 0u || f->len != (uint16_t)(MB_READ_ACK_MIN_LEN + byte_count))
        {
            return MB_ERR_BAD_SIZE;
        }
        break;

    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
    case MB_FC_WRITE_COILS:
    case MB_FC_WRITE_REGS:
        /* 写应答回显 6 字节（地址 + 功能码 + 地址 + 数量/取值）。 */
        if (f->len != (uint16_t)(MB_F_RESPONSE_SIZE + 2u))
        {
            return MB_ERR_BAD_SIZE;
        }
        break;

    default:
        return MB_EXC_FUNC_CODE;
    }

    return MB_OK;
}

void mb_frame_build_exception(mb_frame_t *f, uint8_t exception_byte)
{
    if (f == NULL || f->len < MB_F_EXCEPTION_SIZE)
    {
        return;
    }

    f->buf[MB_F_FUNC]   = (uint8_t)(f->buf[MB_F_FUNC] | 0x80u);  /* 功能码最高位置 1 */
    f->buf[MB_F_ADD_HI] = exception_byte;                        /* 第 3 字节：异常码 */
    f->len              = (uint16_t)MB_F_EXCEPTION_SIZE;         /* CRC 由 mb_frame_seal() 补 */
}

mb_err_t mb_frame_build_query(mb_frame_t *f, const mb_query_t *q)
{
    uint8_t  fc;
    uint16_t count;
    uint16_t i;
    uint8_t  byte_count;

    if (f == NULL || q == NULL)
    {
        return MB_ERR_BAD_CONFIG;
    }
    if (q->slave_id == 0u)
    {
        return MB_ERR_BAD_ROLE_ID;   /* 主机不能用广播地址发起查询 */
    }
    if (q->slave_id > 247u)
    {
        return MB_ERR_BAD_SLAVE_ID;
    }
    if (q->data == NULL)
    {
        return MB_ERR_NO_DATA;       /* 读要接收缓冲区，写要数据来源 */
    }

    fc = q->fc;
    /* FC5/FC6 的"取值"来自 data[0]；其它功能码的 count 就是个数。 */
    if (fc == MB_FC_WRITE_COIL)
    {
        count = (q->data[0] != 0u) ? MB_COIL_ON : MB_COIL_OFF;
    }
    else if (fc == MB_FC_WRITE_REG)
    {
        count = q->data[0];
    }
    else
    {
        count = q->count;
    }

    /* 数量/取值合法性与协议上限。 */
    switch (fc)
    {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
        if (count < 1u || count > MB_MAX_READ_BITS)
        {
            return MB_ERR_BAD_SIZE;
        }
        break;

    case MB_FC_READ_HOLDING_REGS:
    case MB_FC_READ_INPUT_REGS:
        if (count < 1u || count > MB_MAX_READ_REGS)
        {
            return MB_ERR_BAD_SIZE;
        }
        break;

    case MB_FC_WRITE_COILS:
        if (count < 1u || count > MB_MAX_WRITE_BITS)
        {
            return MB_ERR_BAD_SIZE;
        }
        break;

    case MB_FC_WRITE_REGS:
        if (count < 1u || count > MB_MAX_WRITE_REGS)
        {
            return MB_ERR_BAD_SIZE;
        }
        break;

    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
        break;   /* 单点写已经在上面算好了取值 */

    default:
        return MB_ERR_BAD_FC;
    }

    /* 前 6 字节对读、单写、多写都一样：地址 + 功能码 + 地址(2) + 数量(2)。 */
    f->buf[MB_F_ID]     = q->slave_id;
    f->buf[MB_F_FUNC]   = fc;
    f->buf[MB_F_ADD_HI] = (uint8_t)(q->addr >> 8);
    f->buf[MB_F_ADD_LO] = (uint8_t)(q->addr & 0x00FFu);
    f->buf[MB_F_NB_HI]  = (uint8_t)(count >> 8);
    f->buf[MB_F_NB_LO]  = (uint8_t)(count & 0x00FFu);
    f->len              = (uint16_t)(MB_F_NB_LO + 1u);

    if (fc == MB_FC_WRITE_COILS)
    {
        /* 位载荷：每 8 个线圈压成 1 字节，LSB 在前。 */
        byte_count = (uint8_t)((count + 7u) / 8u);
        f->buf[MB_F_BYTE_CNT] = byte_count;

        for (i = 0u; i < (uint16_t)byte_count; i++)
        {
            uint8_t  value = 0u;
            uint8_t  bit;

            for (bit = 0u; bit < 8u; bit++)
            {
                uint16_t coil = (uint16_t)(i * 8u + bit);
                if (coil >= count)
                {
                    break;
                }
                if ((q->data[coil / 16u] & (uint16_t)(1u << (coil % 16u))) != 0u)
                {
                    value = (uint8_t)(value | (uint8_t)(1u << bit));
                }
            }
            f->buf[MB_F_BYTE_CNT + 1u + i] = value;
        }
        f->len = (uint16_t)(MB_F_BYTE_CNT + 1u + byte_count);
    }
    else if (fc == MB_FC_WRITE_REGS)
    {
        /* 寄存器载荷：每个寄存器 2 字节，高字节在前。 */
        byte_count = (uint8_t)(count * 2u);
        f->buf[MB_F_BYTE_CNT] = byte_count;

        for (i = 0u; i < count; i++)
        {
            f->buf[MB_F_BYTE_CNT + 1u + i * 2u] = (uint8_t)(q->data[i] >> 8);
            f->buf[MB_F_BYTE_CNT + 2u + i * 2u] = (uint8_t)(q->data[i] & 0x00FFu);
        }
        f->len = (uint16_t)(MB_F_BYTE_CNT + 1u + byte_count);
    }

    return MB_OK;
}

mb_err_t mb_frame_decode_answer(mb_frame_t *f, uint16_t *dst, uint16_t dst_words)
{
    uint8_t  fc;
    uint8_t  byte_count;
    uint16_t words;
    uint16_t i;

    if (f == NULL || dst == NULL)
    {
        return MB_ERR_NO_DATA;
    }

    fc         = f->buf[MB_F_FUNC];
    byte_count = f->buf[MB_F_ACK_BYTES];

    if (fc == MB_FC_READ_COILS || fc == MB_FC_READ_DISCRETE_INPUTS)
    {
        /* 位数据按 LSB 在前逐位展开到 16 位字里。 */
        if ((uint32_t)byte_count > (uint32_t)dst_words * 2u)
        {
            return MB_ERR_BAD_SIZE;
        }

        for (i = 0u; i < dst_words; i++)
        {
            dst[i] = 0u;
        }
        for (i = 0u; i < (uint16_t)byte_count; i++)
        {
            uint16_t word  = (uint16_t)(i / 2u);
            uint8_t  shift = (uint8_t)((i % 2u) * 8u);

            dst[word] = (uint16_t)(dst[word]
                       | (uint16_t)((uint16_t)f->buf[MB_F_ACK_BYTES + 1u + i] << shift));
        }
        return MB_OK;
    }

    if (fc == MB_FC_READ_HOLDING_REGS || fc == MB_FC_READ_INPUT_REGS)
    {
        /* 寄存器数据按大端两两拼回 16 位字。 */
        if ((byte_count % 2u) != 0u)
        {
            return MB_ERR_BAD_SIZE;
        }
        words = (uint16_t)(byte_count / 2u);
        if (words > dst_words)
        {
            return MB_ERR_BAD_SIZE;
        }
        for (i = 0u; i < words; i++)
        {
            dst[i] = mb_word(f->buf[MB_F_ACK_BYTES + 1u + i * 2u],
                             f->buf[MB_F_ACK_BYTES + 2u + i * 2u]);
        }
        return MB_OK;
    }

    /* 写应答里没有需要提取的数据。 */
    return MB_OK;
}