/*
 * bsp.h
 *
 * STM32F407VE 板级接口。这一层是库与硬件之间的唯一接触面：
 * 库只认 mb_cfg_t 里的三个回调，剩下的都收敛在这里。
 *
 * 接线（默认，可在编译期改 BOARD_* 宏）：
 *   USART1  PA9  = TX   →  USB-TTL 的 RX
 *           PA10 = RX   ←  USB-TTL 的 TX
 *   RS-485  PD4  = DE/RE（高电平发送，接 MAX485 的 DE 与 /RE）
 *   LED     PA6  = 低电平点亮
 *
 * 硬件自环测试需要把 PA9 与 PA10 短接（USB-TTL 不接也能跑其余用例）。
 */

#ifndef BSP_H
#define BSP_H

#include <stdint.h>
#include <stdbool.h>

/* 板级参数 */
#define BSP_SYSCLK_HZ   168000000u
#define BSP_HSE_HZ      8000000u
#define BSP_UART_BAUD   115200u

/* 接收环形缓冲大小，必须是 2 的幂 */
#define BSP_UART_RX_BUF 128u

/* 引脚（按 STM32F407VE 常见核心板默认值） */
#define BSP_USART        1
#define BSP_PIN_UART_TX  9u      /* PA9  */
#define BSP_PIN_UART_RX  10u     /* PA10 */
#define BSP_PIN_RS485_DE 4u      /* PD4  */
#define BSP_PIN_LED      6u      /* PA6  */

/* ---------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------- */

/**
 * @brief 板级初始化：HSE → PLL → 168 MHz、5 等待周期、SysTick 1 ms、
 *        GPIO、USART1 115200 8N1 并使能接收中断。调用后所有 bsp_* 可用。
 */
void bsp_init(void);

/** @brief 毫秒计数（SysTick 中断累加，回绕安全）。 */
uint32_t bsp_millis(void);

/**
 * @brief 时钟是否成功从 HSE 切到 168 MHz。
 *
 * HSE 起振失败时会退回 HSI 16 MHz（此时 SysTick 重装值也要相应调整，
 * 保证 bsp_millis() 仍是 1 ms 粒度），此时返回 false。
 */
bool bsp_clock_ok(void);

/** @brief 实际 SYSCLK（Hz）：成功时 168000000，退路是 16000000。 */
uint32_t bsp_sysclk_hz(void);

/** @brief 忙等 ms 毫秒。 */
void bsp_delay_ms(uint32_t ms);

/* ---------------------------------------------------------------------
 * USART1
 * ------------------------------------------------------------------- */

/**
 * @brief 阻塞发送。返回时最后一个字节的移位寄存器已经发空（TC 置位），
 *        这一点对 RS-485 的 DE 时序是必须的。
 * @return 0 成功
 */
int bsp_uart_write(const uint8_t *buf, uint16_t len);

/**
 * @brief 从接收环形缓冲取字节（主循环调用）。
 * @return 实际取到的字节数，可能少于 max
 */
uint16_t bsp_uart_read(uint8_t *dst, uint16_t max);

/** @brief 丢弃接收缓冲里所有未读字节。 */
void bsp_uart_flush(void);

/** @brief 因缓冲满而丢弃的字节数（诊断用）。 */
uint32_t bsp_uart_rx_dropped(void);

/* ---------------------------------------------------------------------
 * RS-485 方向与 LED
 * ------------------------------------------------------------------- */

/** @brief 切换 RS-485 收发方向：true = 发送（DE 拉高）。 */
void bsp_rs485_tx(bool enable);

/** @brief 读回 DE 引脚当前电平。 */
bool bsp_rs485_is_tx(void);

/**
 * @brief 探测 USART1 的 PA9–PA10 是否短接（硬件自环跳线）。
 *
 * 做法：临时把 PA9 切成推挽输出并拉低，读 PA10 的输入电平；读到低说明两脚
 * 之间有导线，随后立刻恢复 PA9 的复用功能。用来把"没接跳线"和"串口坏了"
 * 区分开——否则两种情况下自环用例都只能记 skip，会掩盖真实故障。
 *
 * @return true 表示跳线看起来存在
 */
bool bsp_loopback_probe(void);

void bsp_led(bool on);
void bsp_led_toggle(void);

/* ---------------------------------------------------------------------
 * 中断控制
 * ------------------------------------------------------------------- */

/** @brief 关全局中断，返回原 PRIMASK（传给 bsp_irq_restore）。 */
uint32_t bsp_irq_save(void);

/** @brief 恢复 bsp_irq_save() 返回的 PRIMASK。 */
void bsp_irq_restore(uint32_t state);

/* ---------------------------------------------------------------------
 * 直接喂给 mb_cfg_t 的三个回调
 * ------------------------------------------------------------------- */

int      bsp_mb_write(void *user, const uint8_t *buf, uint16_t len);
void     bsp_mb_set_tx(void *user, bool tx);
uint32_t bsp_mb_millis(void *user);

#endif /* BSP_H */
