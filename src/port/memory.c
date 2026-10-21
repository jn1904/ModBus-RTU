/*
 * memory.c
 *
 * 内存传输层：把库写出的每一个字节抓进 buf[]，并可选地转交给一个对端实例。
 *
 * 测试套件靠它让主机和从站"隔空对话"；不想接硬件时也可以用它把协议跑通
 * （仿真、出厂调试、CI 都适用）。
 */

#include <string.h>

#include "modbus_memory.h"

void mb_memory_init(mb_memory_t *m, mb_t *peer){
    if (m == NULL){
        return;
    }

    memset(m, 0, sizeof(*m));
    m->peer = peer;
}

void mb_memory_reset(mb_memory_t *m){
    if (m == NULL){
        return;
    }

    memset(m->buf, 0, sizeof(m->buf));
    m->len      = 0u;
    m->overflow = false;
    m->tx       = false;
}

int mb_memory_write(void *user, const uint8_t *buf, uint16_t len){
    mb_memory_t *m = (mb_memory_t *)user;

    if (m == NULL || buf == NULL){
        return -1;
    }
    if (len > (uint16_t)MB_MAX_FRAME){
        /* 库不会发出超过 MB_MAX_FRAME 的帧，走到这里说明配置被改坏了。 */
        m->overflow = true;
        return -1;
    }

    memcpy(m->buf, buf, (size_t)len);
    m->len = len;

    if (m->peer != NULL){
        /* 直接把字节推进对端的接收缓冲，等于"线"接在一起。 */
        mb_rx(m->peer, buf, len);
    }

    return 0;
}

void mb_memory_set_tx(void *user, bool tx){
    mb_memory_t *m = (mb_memory_t *)user;

    if (m != NULL){
        m->tx = tx;
    }
}
