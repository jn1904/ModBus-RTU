/*
 * func.c
 *
 * 从站功能码的执行：读类功能码在这里把请求帧就地改写成应答帧，写类功能码
 * 先把数据落到内存映像上，再（非广播时）改写成回显应答。
 *
 * 进入本文件时请求帧已经通过 mb_frame_validate_request() 校验，因此地址、
 * 数量、帧长都是可信的，可以直接按 count 访问内存。离开时帧里还没有 CRC。
 */

#include "core/mb_core.h"

/* 位操作：把 Modbus 地址折算成"映像里第几个字、字第几位"。 */
static bool mb_bit_get(const mb_region_t *region, uint16_t addr)
{
    uint16_t index = (uint16_t)(addr - region->start);

    return (region->data[index / 16u] & (uint16_t)(1u << (index % 16u))) != 0u;
}

static void mb_bit_set(const mb_region_t *region, uint16_t addr, bool on)
{
    uint16_t index = (uint16_t)(addr - region->start);
    uint16_t mask  = (uint16_t)(1u << (index % 16u));

    if (on)
    {
        region->data[index / 16u] = (uint16_t)(region->data[index / 16u] | mask);
    }
    else
    {
        region->data[index / 16u] = (uint16_t)(region->data[index / 16u] & (uint16_t)(~mask));
    }
}

/* FC1/FC2：读取位，按 LSB 在前打包成字节流。 */
static void mb_read_bits(mb_frame_t *f, const mb_region_t *region,
                         uint16_t addr, uint16_t count)
{
    uint8_t *payload = &f->buf[MB_F_ACK_BYTES + 1u];
    uint16_t byte_count = (uint16_t)((count + 7u) / 8u);
    uint16_t i;

    for (i = 0u; i < byte_count; i++)
    {
        payload[i] = 0u;
    }
    for (i = 0u; i < count; i++)
    {
        uint8_t bit_in_byte = (uint8_t)(i % 8u);
        uint8_t value       = mb_bit_get(region, (uint16_t)(addr + i)) ? 1u : 0u;

        payload[i / 8u] = (uint8_t)(payload[i / 8u] | (uint8_t)(value << bit_in_byte));
    }

    f->buf[MB_F_ACK_BYTES] = (uint8_t)byte_count;
    f->len = (uint16_t)(MB_F_ACK_BYTES + 1u + byte_count);
}

/* FC3/FC4：读取寄存器，按大端（高字节在前）写入应答。 */
static void mb_read_regs(mb_frame_t *f, const mb_region_t *region,
                         uint16_t addr, uint16_t count)
{
    uint8_t *payload = &f->buf[MB_F_ACK_BYTES + 1u];
    uint16_t i;

    for (i = 0u; i < count; i++)
    {
        uint16_t value = region->data[(uint16_t)(addr - region->start) + i];

        payload[i * 2u]      = (uint8_t)(value >> 8);
        payload[i * 2u + 1u] = (uint8_t)(value & 0x00FFu);
    }

    f->buf[MB_F_ACK_BYTES] = (uint8_t)(count * 2u);
    f->len = (uint16_t)(MB_F_ACK_BYTES + 1u + count * 2u);
}

/*
 * 写应答：前 6 字节（地址 + 功能码 + 地址 + 数量/取值）在请求帧里已经是对的，
 * 所以"改写"只是把长度截到 6 字节（去掉请求多出来的字节数与载荷）。
 */
static void mb_echo_head(mb_frame_t *f)
{
    f->len = (uint16_t)MB_F_RESPONSE_SIZE;
}

void mb_frame_process_request(mb_frame_t *f, const mb_map_t *map, bool broadcast)
{
    uint8_t  fc;
    uint16_t addr;
    uint16_t count;
    uint16_t i;
    const mb_region_t *region;

    if (f == NULL)
    {
        return;
    }

    fc     = f->buf[MB_F_FUNC];
    addr   = mb_word(f->buf[MB_F_ADD_HI], f->buf[MB_F_ADD_LO]);
    count  = mb_word(f->buf[MB_F_NB_HI],  f->buf[MB_F_NB_LO]);
    region = mb_map_region(map, mb_area_for_fc(fc));

    switch (fc)
    {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
        if (!broadcast)
        {
            mb_read_bits(f, region, addr, count);
        }
        break;

    case MB_FC_READ_HOLDING_REGS:
    case MB_FC_READ_INPUT_REGS:
        if (!broadcast)
        {
            mb_read_regs(f, region, addr, count);
        }
        break;

    case MB_FC_WRITE_COIL:
        /* 单点写的 count 字段就是线上取值：0xFF00 合、0x0000 分。 */
        mb_bit_set(region, addr, count == 0xFF00u);
        if (!broadcast)
        {
            mb_echo_head(f);
        }
        break;

    case MB_FC_WRITE_REG:
        region->data[(uint16_t)(addr - region->start)] = count;
        if (!broadcast)
        {
            mb_echo_head(f);
        }
        break;

    case MB_FC_WRITE_COILS:
        for (i = 0u; i < count; i++)
        {
            uint8_t raw = f->buf[MB_F_BYTE_CNT + 1u + (i / 8u)];
            bool    on  = (((raw >> (i % 8u)) & 0x01u) != 0u);

            mb_bit_set(region, (uint16_t)(addr + i), on);
        }
        if (!broadcast)
        {
            mb_echo_head(f);
        }
        break;

    case MB_FC_WRITE_REGS:
        for (i = 0u; i < count; i++)
        {
            uint16_t value = mb_word(f->buf[MB_F_BYTE_CNT + 1u + i * 2u],
                                     f->buf[MB_F_BYTE_CNT + 2u + i * 2u]);

            region->data[(uint16_t)(addr - region->start) + i] = value;
        }
        if (!broadcast)
        {
            mb_echo_head(f);
        }
        break;

    default:
        /* 校验阶段已经挡掉了不支持的功能码，这里理论上到不了。 */
        break;
    }
}