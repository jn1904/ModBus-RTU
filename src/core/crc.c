/*
 * crc.c
 *
 * 协议核心的字节级原语：Modbus CRC-16 和
 * 双字节组装辅助函数。
 */

#include "core/mb_core.h"

/**
 * @brief Modbus CRC-16（CRC-16/ARC，多项式 0x8005 反射）。
 * 结果是自然 CRC 值：按低字节在前的方式序列化，以匹配
 * Modbus RTU 帧格式。将其追加到报文并
 * 对整个帧重新计算，即可得到 CRC 余数。
 */
uint16_t mb_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)buf[i];
        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if (crc & 0x0001u)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            }
            else
            {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

/**
 * @brief 将两个字节组合为一个 16 位字（大端字节序）。
 */
uint16_t mb_word(uint8_t hi, uint8_t lo)
{
    return (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
}
