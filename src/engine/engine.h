/*
 * engine.h
 *
 * 引擎内部接口：从站状态机（engine.c）与主机查询状态机（master.c）之间的
 * 两个接入点。只在库内部使用，不对外暴露。
 */

#ifndef MB_ENGINE_H
#define MB_ENGINE_H

#include "modbus.h"

/* 主机侧：查询超时检查，由 mb_poll() 每轮调用一次。 */
void mb_engine_master_poll(mb_t *mb);

/* 主机侧：处理 mb->work 里已经定好的一个应答帧。 */
void mb_engine_master_frame(mb_t *mb);

#endif /* MB_ENGINE_H */