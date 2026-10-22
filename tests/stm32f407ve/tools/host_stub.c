/*
 * host_stub.c
 *
 * 宿主机仿真桩：只给 tools/host_sim.sh 用，不参与 ARM 固件构建。
 *
 * 目的：上板之前先在 PC 上跑一遍 main.c 的用例逻辑——库源码是真实的，
 * 只有板级层被替换成桩。这样如果某个用例在上板后失败，可以确定问题出在
 * 硬件/板级层，而不是用例的期望值写错。
 *
 * RCC/FLASH 寄存器：main.c 的时钟用例会直接读它们，所以在 0x40023000
 * 映射一页内存并按"168 MHz 配置成功"填好，让寄存器复核代码也能执行。
 * 串口写入只把可打印字节送 stdout——自环用例会把原始 Modbus 帧也写进来。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdint.h>
#include "bsp.h"

static uint32_t g_base;
static bool     g_tx;
static bool     g_fake_periph_ok;

static uint32_t mono_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000L));
}

/* main.c 的时钟用例会直接读 RCC/FLASH 寄存器。宿主上把这些外设地址映射成
 * 一块普通内存并按"168 MHz 配置成功"填好，这样寄存器复核代码也能被跑到。 */
static void map_fake_periph(void){
    const uintptr_t base = 0x40023000UL;
    const size_t    size = 0x2000u;
    void *p = mmap((void *)base, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED){ return; }
    g_fake_periph_ok = true;
    *(volatile uint32_t *)(base + 0x0800u) = (1u << 25);      /* RCC->CR: PLLRDY */
    *(volatile uint32_t *)(base + 0x0804u) = 0x07405408u;     /* PLLCFGR */
    *(volatile uint32_t *)(base + 0x0808u) = 0x9408u;         /* CFGR: SWS=PLL, PPRE1/4, PPRE2/2 */
    *(volatile uint32_t *)(base + 0x0C00u) = 0x705u;          /* FLASH->ACR */
}

void bsp_init(void){ g_base = mono_ms(); map_fake_periph(); }
uint32_t bsp_millis(void){ return mono_ms() - g_base; }
void bsp_delay_ms(uint32_t ms){
    struct timespec ts;
    if (ms == 500u){ fflush(stdout); exit(0); }
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}
int bsp_uart_write(const uint8_t *buf, uint16_t len){
    uint16_t i;
    if (buf == NULL){ return -1; }
    for (i = 0u; i < len; i++){
        uint8_t c = buf[i];
        if ((c >= 0x20u) || (c == 0x0Au) || (c == 0x0Du)){ putchar(c); }
    }
    fflush(stdout);
    return 0;
}
uint16_t bsp_uart_read(uint8_t *dst, uint16_t max){ (void)dst; (void)max; return 0u; }
void bsp_uart_flush(void){ }
uint32_t bsp_uart_rx_dropped(void){ return 0u; }
void bsp_rs485_tx(bool enable){ g_tx = enable; }
bool bsp_rs485_is_tx(void){ return g_tx; }
bool bsp_loopback_probe(void){ return false; }
void bsp_led(bool on){ (void)on; }
void bsp_led_toggle(void){ }
uint32_t bsp_irq_save(void){ return 0u; }
void bsp_irq_restore(uint32_t state){ (void)state; }
bool bsp_clock_ok(void){ return g_fake_periph_ok; }
uint32_t bsp_sysclk_hz(void){ return BSP_SYSCLK_HZ; }
int bsp_mb_write(void *u, const uint8_t *b, uint16_t n){ (void)u; return bsp_uart_write(b, n); }
void bsp_mb_set_tx(void *u, bool t){ (void)u; g_tx = t; }
uint32_t bsp_mb_millis(void *u){ (void)u; return bsp_millis(); }
