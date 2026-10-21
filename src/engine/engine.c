/*
 * engine.c
 *
 * 库的对外入口：初始化、投喂接收字节、驱动状态机。
 *
 * 判帧沿用 Modbus RTU 的 T3.5 规则：mb_rx() 只负责把字节堆进 rx[]，
 * mb_poll() 检测到"最后一个字节之后静默 >= t35_ms"时，才把 rx[] 定型成一个
 * 完整帧。从站的校验、执行、应答都在这里完成；主机的查询状态机放在
 * master.c，通过 engine.h 暴露的两个钩子接入。
 */

#include <string.h>

#include "core/mb_core.h"
#include "engine/engine.h"

/* 静默期满：把 rx[] 定型成 work 帧，或者丢弃一个超长帧。 */
static void mb_engine_freeze(mb_t *mb, uint32_t now){
    if (mb->rx_len == 0u){
        return;
    }
    if ((uint32_t)(now - mb->rx_last_ms) < (uint32_t)mb->cfg.t35_ms){
        return;   /* 帧还没结束，继续等 */
    }

    if (mb->rx_overflow){
        /*
         * 超长帧只能整体丢弃。这一点很关键：如果拿一个被截断的帧去回应，
         * 之后所有帧的帧边界都会错位，整条链路会持续丢帧。
         */
        MB_REPORT(mb, MB_ERR_BUFF_OVERFLOW);
        mb->rx_overflow = false;
    }
    else{
        memcpy(mb->work.buf, mb->rx, (size_t)mb->rx_len);
        mb->work.len    = mb->rx_len;
        mb->frame_ready = true;
        mb->in_count++;
    }

    mb->rx_len = 0u;
}

/* 从站：校验一个请求，执行它，然后（非广播时）把应答写上线。 */
static void mb_engine_slave(mb_t *mb){
    uint8_t  id = mb->work.buf[MB_F_ID];
    mb_err_t err;

    /* 既不是发给本从站、也不是广播：直接丢掉。 */
    if (id != mb->cfg.slave_id && id != 0u){
        return;
    }

    mb->broadcast = (id == 0u);
    err = mb_frame_validate_request(&mb->work, mb->cfg.map);

    if (mb_err_classify(err) == MB_CLASS_PROTO_EXC){
        if (mb->broadcast){
            return;   /* 广播请求不应答，哪怕它是非法的 */
        }
        mb_frame_build_exception(&mb->work, mb_err_exception_byte(err));
    }
    else if (err != MB_OK){
        /* CRC 错、长度不对等链路层问题：按规范保持静默，让主机超时。 */
        MB_REPORT(mb, err);
        return;
    }
    else{
        mb_frame_process_request(&mb->work, mb->cfg.map, mb->broadcast);
        if (mb->broadcast){
            return;   /* 写入已经生效，但广播不回复 */
        }
    }

    mb_frame_seal(&mb->work);

    if (mb->cfg.set_tx != NULL){
        mb->cfg.set_tx(mb->cfg.user, true);
    }
    if (mb->cfg.write(mb->cfg.user, mb->work.buf, mb->work.len) != 0){
        MB_REPORT(mb, MB_ERR_TRANSPORT);
    }
    if (mb->cfg.set_tx != NULL){
        mb->cfg.set_tx(mb->cfg.user, false);
    }

    mb->out_count++;
}

mb_err_t mb_init(mb_t *mb, const mb_cfg_t *cfg){
    if (mb == NULL || cfg == NULL){
        return MB_FAIL(MB_ERR_BAD_CONFIG);
    }
    if (cfg->write == NULL || cfg->millis == NULL){
        return MB_FAIL(MB_ERR_BAD_CONFIG);
    }
    if (cfg->mode != MB_MODE_SLAVE && cfg->mode != MB_MODE_MASTER){
        return MB_FAIL(MB_ERR_BAD_CONFIG);
    }
    if (cfg->mode == MB_MODE_SLAVE){
        /* 从站地址 0 是广播地址，不能拿来当自己的地址。 */
        if (cfg->slave_id == 0u || cfg->slave_id > 247u){
            return MB_FAIL(MB_ERR_BAD_CONFIG);
        }
        if (!mb_map_has_any(cfg->map)){
            return MB_FAIL(MB_ERR_BAD_CONFIG);
        }
    }

    memset(mb, 0, sizeof(*mb));
    mb->cfg = *cfg;

    /* 配置里填 0 表示"用库的默认值"。 */
    if (mb->cfg.t35_ms == 0u){
        mb->cfg.t35_ms = (uint16_t)MB_T35_MS_DEFAULT;
    }
    if (mb->cfg.timeout_ms == 0u){
        mb->cfg.timeout_ms = (uint16_t)MB_TIMEOUT_MS_DEFAULT;
    }

    mb->last_error = MB_OK;
    mb->result     = MB_OK;
    return MB_OK;
}

void mb_rx(mb_t *mb, const uint8_t *data, uint16_t len){
    uint16_t i;

    if (mb == NULL || data == NULL){
        return;
    }

    /* 这一批字节全部按"刚到"处理：最后时刻决定 T3.5 静默的起点。 */
    mb->rx_last_ms = mb->cfg.millis(mb->cfg.user);

    for (i = 0u; i < len; i++){
        if (mb->rx_len >= (uint16_t)MB_MAX_FRAME){
            /* 只置标志：等静默期满再整帧丢弃，避免在字节流中间截断。 */
            mb->rx_overflow = true;
        }
        else{
            mb->rx[mb->rx_len] = data[i];
            mb->rx_len++;
        }
    }
}

void mb_poll(mb_t *mb){
    uint32_t now;

    if (mb == NULL){
        return;
    }

    now = mb->cfg.millis(mb->cfg.user);

    mb_engine_freeze(mb, now);

    if (mb->frame_ready){
        mb->frame_ready = false;

        if (mb->cfg.mode == MB_MODE_SLAVE){
            mb_engine_slave(mb);
        }
        else{
            mb_engine_master_frame(mb);
        }
    }

    if (mb->cfg.mode == MB_MODE_MASTER){
        mb_engine_master_poll(mb);
    }
}

/* --- 统计接口 ---------------------------------------------------------- */

mb_err_t mb_last_error(const mb_t *mb){
    return (mb == NULL) ? MB_ERR_BAD_CONFIG : mb->last_error;
}

uint16_t mb_err_count(const mb_t *mb){
    return (mb == NULL) ? 0u : mb->err_count;
}

uint16_t mb_in_count(const mb_t *mb){
    return (mb == NULL) ? 0u : mb->in_count;
}

uint16_t mb_out_count(const mb_t *mb){
    return (mb == NULL) ? 0u : mb->out_count;
}
