/*
 * stm32f407ve_regs.h
 *
 * STM32F407VE 的最小寄存器定义：只包含本工程用到的外设，不依赖 CMSIS、
 * HAL 或任何厂商 SDK，保持与库本身一致的"零依赖"风格。
 *
 * 所有基址、偏移与位定义都已逐项对照 STM32CubeF4 的 CMSIS 设备头
 * （stm32f407xx.h）核对过，对照方式见 README。
 */

#ifndef STM32F407VE_REGS_H
#define STM32F407VE_REGS_H

#include <stddef.h>
#include <stdint.h>

#define REG32(addr)        (*(volatile uint32_t *)(addr))

/* ---------------------------------------------------------------------
 * 总线与外设基址（RM0090 存储器映射）
 * ------------------------------------------------------------------- */

#define PERIPH_BASE        0x40000000UL
#define APB1PERIPH_BASE    (PERIPH_BASE)
#define APB2PERIPH_BASE    (PERIPH_BASE + 0x00010000UL)
#define AHB1PERIPH_BASE    (PERIPH_BASE + 0x00020000UL)

#define GPIOA_BASE         (AHB1PERIPH_BASE + 0x0000UL)
#define GPIOB_BASE         (AHB1PERIPH_BASE + 0x0400UL)
#define GPIOC_BASE         (AHB1PERIPH_BASE + 0x0800UL)
#define GPIOD_BASE         (AHB1PERIPH_BASE + 0x0C00UL)

#define RCC_BASE           (AHB1PERIPH_BASE + 0x3800UL)
#define FLASH_R_BASE       (AHB1PERIPH_BASE + 0x3C00UL)

#define USART1_BASE        (APB2PERIPH_BASE + 0x1000UL)

/* Cortex-M4 内部（ARMv7-M 架构固定地址） */
#define SCS_BASE           0xE000E000UL
#define SYSTICK_BASE       (SCS_BASE + 0x0010UL)
#define NVIC_BASE          (SCS_BASE + 0x0100UL)

/* ---------------------------------------------------------------------
 * RCC：偏移 0x00 CR、0x04 PLLCFGR、0x08 CFGR、0x30 AHB1ENR、0x44 APB2ENR
 * ------------------------------------------------------------------- */

typedef struct{
    volatile uint32_t CR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB3RSTR;
    volatile uint32_t RESERVED0;
    volatile uint32_t APB1RSTR;
    volatile uint32_t APB2RSTR;
    volatile uint32_t RESERVED1[2];
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
    volatile uint32_t RESERVED2;
    volatile uint32_t APB1ENR;
    volatile uint32_t APB2ENR;
} rcc_regs_t;

#define RCC                ((rcc_regs_t *)RCC_BASE)

/* CR */
#define RCC_CR_HSEON       (0x1UL << 16)
#define RCC_CR_HSERDY      (0x1UL << 17)
#define RCC_CR_PLLON       (0x1UL << 24)
#define RCC_CR_PLLRDY      (0x1UL << 25)

/* PLLCFGR：PLLM[5:0] PLLN[14:6] PLLP[17:16] PLLSRC[22] PLLQ[27:24] */
#define RCC_PLLCFGR_PLLM_POS   0U
#define RCC_PLLCFGR_PLLN_POS   6U
#define RCC_PLLCFGR_PLLP_POS   16U
#define RCC_PLLCFGR_PLLSRC_POS 22U
#define RCC_PLLCFGR_PLLQ_POS   24U
#define RCC_PLLCFGR_PLLM_Msk   (0x3FUL << RCC_PLLCFGR_PLLM_POS)
#define RCC_PLLCFGR_PLLN_Msk   (0x1FFUL << RCC_PLLCFGR_PLLN_POS)
#define RCC_PLLCFGR_PLLP_Msk   (0x3UL << RCC_PLLCFGR_PLLP_POS)
#define RCC_PLLCFGR_PLLSRC_Msk (0x1UL << RCC_PLLCFGR_PLLSRC_POS)
#define RCC_PLLCFGR_PLLQ_Msk   (0xFUL << RCC_PLLCFGR_PLLQ_POS)

/* CFGR：SW[1:0] SWS[3:2] HPRE[7:4] PPRE1[12:10] PPRE2[15:13] */
#define RCC_CFGR_SW_Msk    (0x3UL << 0)
#define RCC_CFGR_HPRE_Msk  (0xFUL << 4)
#define RCC_CFGR_PPRE1_Msk (0x7UL << 10)
#define RCC_CFGR_PPRE2_Msk (0x7UL << 13)
#define RCC_CFGR_SWS_Msk   (0x3UL << 2)
#define RCC_CFGR_SW_PLL    0x00000002UL
#define RCC_CFGR_HPRE_DIV1 0x00000000UL
#define RCC_CFGR_PPRE1_DIV4 0x00001400UL
#define RCC_CFGR_PPRE2_DIV2 0x00008000UL

/* 时钟使能 */
#define RCC_AHB1ENR_GPIOAEN (0x1UL << 0)
#define RCC_AHB1ENR_GPIOBEN (0x1UL << 1)
#define RCC_AHB1ENR_GPIOCEN (0x1UL << 2)
#define RCC_AHB1ENR_GPIODEN (0x1UL << 3)
#define RCC_APB2ENR_USART1EN (0x1UL << 4)

/* ---------------------------------------------------------------------
 * FLASH 接口：ACR 偏移 0x00
 * ------------------------------------------------------------------- */

typedef struct{
    volatile uint32_t ACR;
} flash_regs_t;

#define FLASH_R            ((flash_regs_t *)FLASH_R_BASE)

#define FLASH_ACR_LATENCY_Msk (0x7UL << 0)
#define FLASH_ACR_LATENCY_5WS 0x00000005UL    /* 168 MHz、2.7~3.6 V 需要 5 个等待周期 */
#define FLASH_ACR_PRFTEN      (0x1UL << 8)
#define FLASH_ACR_ICEN        (0x1UL << 9)
#define FLASH_ACR_DCEN        (0x1UL << 10)

/* ---------------------------------------------------------------------
 * GPIO：偏移 0x00 MODER、0x04 OTYPER、0x08 OSPEEDR、0x0C PUPDR、
 *       0x10 IDR、0x14 ODR、0x18 BSRR、0x1C LCKR、0x20 AFR[0]、0x24 AFR[1]
 * ------------------------------------------------------------------- */

typedef struct{
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} gpio_regs_t;

#define GPIOA              ((gpio_regs_t *)GPIOA_BASE)
#define GPIOB              ((gpio_regs_t *)GPIOB_BASE)
#define GPIOC              ((gpio_regs_t *)GPIOC_BASE)
#define GPIOD              ((gpio_regs_t *)GPIOD_BASE)

/* MODER 每脚 2 位；OSPEEDR/PUPDR 同理；AFR 每脚 4 位 */
#define GPIO_MODE_INPUT    0x0UL
#define GPIO_MODE_OUTPUT   0x1UL
#define GPIO_MODE_AF       0x2UL
#define GPIO_MODE_ANALOG   0x3UL

#define GPIO_OSPEED_HIGH   0x2UL
#define GPIO_OSPEED_VHIGH  0x3UL

#define GPIO_PUPD_NONE     0x0UL
#define GPIO_PUPD_PULLUP   0x1UL
#define GPIO_PUPD_PULLDOWN 0x2UL

#define GPIO_AF7_USART1    7UL          /* PA9 / PA10 的复用功能号 */

/* ---------------------------------------------------------------------
 * USART：偏移 0x00 SR、0x04 DR、0x08 BRR、0x0C CR1
 * ------------------------------------------------------------------- */

typedef struct{
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t GTPR;
} usart_regs_t;

#define USART1             ((usart_regs_t *)USART1_BASE)

#define USART_SR_TXE       (0x1UL << 7)
#define USART_SR_TC        (0x1UL << 6)
#define USART_SR_RXNE      (0x1UL << 5)
#define USART_SR_ORE       (0x1UL << 3)
#define USART_SR_FE        (0x1UL << 1)

#define USART_CR1_UE       (0x1UL << 13)
#define USART_CR1_TE       (0x1UL << 3)
#define USART_CR1_RE       (0x1UL << 2)
#define USART_CR1_RXNEIE   (0x1UL << 5)

#define USART_BRR_DIV_Mantissa_Msk (0xFFFUL << 4)
#define USART_BRR_DIV_Fraction_Msk (0xFUL << 0)

/* ---------------------------------------------------------------------
 * Cortex-M4：SysTick / NVIC
 * ------------------------------------------------------------------- */

#define SYSTICK_CTRL       REG32(SYSTICK_BASE + 0x00UL)
#define SYSTICK_LOAD       REG32(SYSTICK_BASE + 0x04UL)
#define SYSTICK_VAL        REG32(SYSTICK_BASE + 0x08UL)
#define SYSTICK_CALIB      REG32(SYSTICK_BASE + 0x0CUL)

#define SYSTICK_CTRL_ENABLE    (0x1UL << 0)
#define SYSTICK_CTRL_TICKINT   (0x1UL << 1)
#define SYSTICK_CTRL_CLKSOURCE (0x1UL << 2)   /* 1 = 处理器时钟 */

#define NVIC_ISER(n)       REG32(NVIC_BASE + 0x000UL + 4UL * (n))
#define NVIC_IPR(n)        REG32(NVIC_BASE + 0x300UL + 4UL * (n))

#define USART1_IRQN        37UL

/* 全局中断开关：PRIMASK */
static inline uint32_t irq_save(void){
    uint32_t primask;

    __asm volatile ("mrs %0, primask" : "=r" (primask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    return primask;
}

static inline void irq_restore(uint32_t primask){
    __asm volatile ("msr primask, %0" :: "r" (primask) : "memory");
}

/* ---------------------------------------------------------------------
 * 结构体偏移的编译期校验
 *
 * 这些断言把"字段顺序 = 寄存器偏移"钉死在编译期：以后谁改了结构体字段
 * 顺序（最容易犯又最难查的错），构建立刻失败，而不是在板子上跑到一半
 * 才发现写错了寄存器。
 *
 * 用 C99 的负数组长度技巧，避免依赖 C11 的 _Static_assert。
 * ------------------------------------------------------------------- */

#define MB_STATIC_ASSERT(cond, tag) typedef char static_assert_##tag[(cond) ? 1 : -1]

MB_STATIC_ASSERT(offsetof(rcc_regs_t, PLLCFGR) == 0x04, rcc_pllcfgr);
MB_STATIC_ASSERT(offsetof(rcc_regs_t, CFGR)    == 0x08, rcc_cfgr);
MB_STATIC_ASSERT(offsetof(rcc_regs_t, AHB1ENR) == 0x30, rcc_ahb1enr);
MB_STATIC_ASSERT(offsetof(rcc_regs_t, APB1ENR) == 0x40, rcc_apb1enr);
MB_STATIC_ASSERT(offsetof(rcc_regs_t, APB2ENR) == 0x44, rcc_apb2enr);

MB_STATIC_ASSERT(offsetof(gpio_regs_t, OTYPER)  == 0x04, gpio_otyper);
MB_STATIC_ASSERT(offsetof(gpio_regs_t, OSPEEDR) == 0x08, gpio_ospeedr);
MB_STATIC_ASSERT(offsetof(gpio_regs_t, PUPDR)   == 0x0C, gpio_pupdr);
MB_STATIC_ASSERT(offsetof(gpio_regs_t, ODR)     == 0x14, gpio_odr);
MB_STATIC_ASSERT(offsetof(gpio_regs_t, BSRR)    == 0x18, gpio_bsrr);
MB_STATIC_ASSERT(offsetof(gpio_regs_t, AFR)     == 0x20, gpio_afr0);
MB_STATIC_ASSERT(offsetof(gpio_regs_t, AFR[1])  == 0x24, gpio_afr1);

MB_STATIC_ASSERT(offsetof(usart_regs_t, DR)  == 0x04, usart_dr);
MB_STATIC_ASSERT(offsetof(usart_regs_t, BRR) == 0x08, usart_brr);
MB_STATIC_ASSERT(offsetof(usart_regs_t, CR1) == 0x0C, usart_cr1);

#endif /* STM32F407VE_REGS_H */
