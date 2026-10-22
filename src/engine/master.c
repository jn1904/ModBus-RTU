/*
 * master.c
 *
 * 主机的查询状态机：发起查询、收应答、判超时、把结果交给应用。
 *
 * 一次只挂一笔查询（半双工 RS-485 的常规用法）：
 *   mb_master_send()         序列化查询、写上线，进入等待；
 *   mb_poll()                每轮调用下面两个钩子：mb_engine_master_poll()
 *                            看超时，mb_engine_master_frame() 处理已定帧的应答；
 *   mb_master_done()         由应用调用一次，把结果取走。
 */

#include <stddef.h>

#include "core/mb_core.h"
#include "engine/engine.h"

/* 读类功能码期望的载荷字节数；写类返回 0（写应答长度固定为 6）。 */
static uint16_t mb_master_expected_bytes(const mb_query_t *q){
    switch (q->fc){
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
        return (uint16_t)((q->count + 7u) / 8u);

    case MB_FC_READ_HOLDING_REGS:
    case MB_FC_READ_INPUT_REGS:
        return (uint16_t)(q->count * 2u);

    default:
        return 0u;
    }
}

/* 把查询里的"取值"换算成 FC5 在线上使用的两个合法取值之一。 */
static uint16_t mb_master_coil_value(const mb_query_t *q){
    return (q->data[0] != 0u) ? 0xFF00u : 0x0000u;
}

mb_err_t mb_master_send(mb_t *mb, const mb_query_t *q){
    mb_err_t err;

    if (mb == NULL || q == NULL){
        return MB_FAIL(MB_ERR_BAD_CONFIG);
    }
    if (mb->cfg.mode != MB_MODE_MASTER){
        return MB_REPORT(mb, MB_ERR_NOT_MASTER);
    }
    /* 上一笔查询还没结束、或者结果已经就绪但还没被 mb_master_done() 取走，
       都不允许再发新查询——否则结果会被覆盖掉。 */
    if (mb->waiting || mb->has_result){
        return MB_REPORT(mb, MB_ERR_POLLING);
    }

    /* 参数与帧长都在这里校验（从站地址、功能码、数量、data 指针）。 */
    err = mb_frame_build_query(&mb->work, q);
    if (err != MB_OK){
        return MB_REPORT(mb, err);
    }

    mb_frame_seal(&mb->work);

    mb->query      = *q;      /* 只保存指针：数据缓冲区由应用负责保活 */
    mb->result     = MB_OK;
    mb->has_result = false;
    mb->waiting    = true;
    mb->deadline   = mb->cfg.millis(mb->cfg.user) + (uint32_t)mb->cfg.timeout_ms;

    if (mb->cfg.set_tx != NULL){
        mb->cfg.set_tx(mb->cfg.user, true);
    }
    if (mb->cfg.write(mb->cfg.user, mb->work.buf, mb->work.len) != 0){
        mb->waiting = false;
        return MB_REPORT(mb, MB_ERR_TRANSPORT);
    }
    if (mb->cfg.set_tx != NULL){
        mb->cfg.set_tx(mb->cfg.user, false);
    }

    mb->out_count++;
    return MB_OK;
}

bool mb_master_busy(const mb_t *mb){
    return (mb != NULL) && mb->waiting;
}

bool mb_master_done(mb_t *mb, mb_err_t *err){
    if (mb == NULL || !mb->has_result){
        return false;
    }

    if (err != NULL){
        *err = mb->result;
    }
    mb->has_result = false;
    return true;
}

void mb_engine_master_poll(mb_t *mb){
    uint32_t now;

    if (mb == NULL || !mb->waiting){
        return;
    }

    now = mb->cfg.millis(mb->cfg.user);

    /* 无符号回绕安全的 "now >= deadline" 判定。 */
    if ((uint32_t)(now - mb->deadline) < 0x80000000u){
        mb->waiting = false;
        mb->result  = MB_REPORT(mb, MB_ERR_TIME_OUT);
        mb->has_result = true;
    }
}

void mb_engine_master_frame(mb_t *mb){
    mb_err_t err;
    uint8_t  fc;
    uint16_t expected;
    uint16_t words;

    if (mb == NULL || !mb->waiting){
        return;   /* 没有在等应答：当噪声丢弃 */
    }

    /* 只接受目标从站的应答。 */
    if (mb->work.buf[MB_F_ID] != mb->query.slave_id){
        return;
    }

    err = mb_frame_validate_answer(&mb->work);

    if (err == MB_ERR_EXCEPTION){
        mb->waiting    = false;
        mb->result     = MB_REPORT(mb, MB_ERR_EXCEPTION);
        mb->has_result = true;
        return;
    }
    if (err != MB_OK){
        /* CRC 坏、长度不对：按规范丢弃该帧继续等，由超时兜底。 */
        MB_REPORT(mb, err);
        return;
    }

    fc = (uint8_t)(mb->work.buf[MB_F_FUNC] & 0x7Fu);
    if (fc != mb->query.fc){
        MB_REPORT(mb, MB_ERR_BAD_SIZE);
        return;
    }

    if (fc <= (uint8_t)MB_FC_READ_INPUT_REGS){
        /* 读应答：字节数必须与查询的数量完全对应。 */
        expected = mb_master_expected_bytes(&mb->query);
        if (expected == 0u || mb->work.buf[MB_F_ACK_BYTES] != (uint8_t)expected){
            MB_REPORT(mb, MB_ERR_BAD_SIZE);
            return;
        }

        /* 位读回时按 16 位字打包，寄存器读回时一对一。 */
        words = (fc <= (uint8_t)MB_FC_READ_DISCRETE_INPUTS)
              ? (uint16_t)((mb->query.count + 15u) / 16u)
              : mb->query.count;

        err = mb_frame_decode_answer(&mb->work, mb->query.data, words);
        if (err != MB_OK){
            MB_REPORT(mb, err);
            return;
        }
    }
    else{
        /* 写应答：核对从站回显的地址与数量/取值。 */
        uint16_t addr = mb_word(mb->work.buf[MB_F_ADD_HI], mb->work.buf[MB_F_ADD_LO]);
        uint16_t echo = mb_word(mb->work.buf[MB_F_NB_HI],  mb->work.buf[MB_F_NB_LO]);

        if (addr != mb->query.addr){
            MB_REPORT(mb, MB_ERR_BAD_SIZE);
            return;
        }

        if (fc == (uint8_t)MB_FC_WRITE_COILS || fc == (uint8_t)MB_FC_WRITE_REGS){
            if (echo != mb->query.count){
                MB_REPORT(mb, MB_ERR_BAD_SIZE);
                return;
            }
        }
        else{
            uint16_t value = (fc == (uint8_t)MB_FC_WRITE_COIL)
                           ? mb_master_coil_value(&mb->query)
                           : mb->query.data[0];

            if (echo != value){
                MB_REPORT(mb, MB_ERR_BAD_SIZE);
                return;
            }
        }
    }

    mb->waiting    = false;
    mb->result     = MB_OK;
    mb->has_result = true;
}
