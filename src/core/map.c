/*
 * map.c
 *
 * 四个 Modbus 内存区域的解析。
 *
 * 数据指针为 NULL 的区域就是不存在：涉及它的请求
 * 会以“非法数据地址”异常应答。
 * mb_map_shared() 辅助函数将每个区域绑定到同一个数组，这就是
 * 经典的单一内存模型，其中线圈和寄存器共享字。
 */

#include "core/mb_core.h"

mb_area_t mb_area_for_fc(uint8_t fc)
{
    switch (fc)
    {
    case MB_FC_READ_COILS:
    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_COILS:
        return MB_AREA_COILS;
    case MB_FC_READ_DISCRETE_INPUTS:
        return MB_AREA_DISCRETE;
    case MB_FC_READ_INPUT_REGS:
        return MB_AREA_INPUT;
    default: /* MB_FC_READ_HOLDING_REGS, MB_FC_WRITE_REG, MB_FC_WRITE_REGS */
        return MB_AREA_HOLDING;
    }
}

const mb_region_t *mb_map_region(const mb_map_t *map, mb_area_t area)
{
    if (map == NULL)
    {
        static const mb_region_t empty = { NULL, 0, 0 };
        return &empty;
    }

    switch (area)
    {
    case MB_AREA_COILS:    return &map->coils;
    case MB_AREA_DISCRETE: return &map->discrete;
    case MB_AREA_INPUT:    return &map->input;
    default:               return &map->holding;
    }
}

bool mb_map_has_any(const mb_map_t *map)
{
    if (map == NULL)
    {
        return false;
    }
    return map->coils.data      != NULL
        || map->discrete.data   != NULL
        || map->input.data      != NULL
        || map->holding.data    != NULL;
}

void mb_map_shared(mb_map_t *map, uint16_t *regs, uint16_t count)
{
    if (map == NULL)
    {
        return;
    }

    map->coils.data    = regs;
    map->coils.start   = 0;
    map->coils.count   = count;

    map->discrete.data = regs;
    map->discrete.start = 0;
    map->discrete.count = count;

    map->input.data    = regs;
    map->input.start   = 0;
    map->input.count   = count;

    map->holding.data  = regs;
    map->holding.start = 0;
    map->holding.count = count;
}
