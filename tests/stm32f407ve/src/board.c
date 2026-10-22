/*
 * board.c
 *
 * STM32F407VE 板级层：HSE 8 MHz → PLL → 168 MHz、1 ms SysTick、
 * USART1 115200 8N1 + 接收中断、RS-485 方向脚、LED、PRIMASK 存取，
 * 以及直接喂给 mb_cfg_t 的三个回调。
 *
 * 为什么全部手写寄存器、不接 CMSIS/HAL：这个工程本身就是为了证明库零依赖，
 * 板级层同样只依赖 stm32f407ve_regs.h 这一份手写寄存器定义；换 MCU 时
 * 只需要按同样的接口重写本文件。
 *
 * 时钟树（HSE 8 MHz）：
 *   HSE 8 MHz / PLLM=8 → 1 MHz   （VCO 输入，必须在 1~2 MHz 之间）
 *   1 MHz × PLLN=336   → 336 MHz （VCO 输出，必须在 100~432 MHz 之间）
 *   336 MHz / PLLP=2   → 168 MHz （SYSCLK；AHB=168、APB1=42、APB2=84 MHz）
 *   336 MHz / PLLQ=7   → 48 MHz  （USB/SDIO 用，本板不用，但取值保持合法）
 *
 * HSE 起振失败时退回 HSI 16 MHz：bsp_clock_ok() 报 false，SysTick 重装值
 * 和 USART 波特率分频都按实际时钟重新计算，保证 1 ms 与 115200 仍然成立。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp.h"
#include "stm32f407ve_regs.h"

/* 中断服务程序：向量表里是弱符号，这里给强定义。前置声明让"谁实现了哪个
 * 中断"一眼可见，也避免 -Wmissing-prototypes 这类告警。 */
void SysTick_Handler(void);
void USART1_IRQHandler(void);

/* 环形缓冲靠 (i + 1) & (size - 1) 取模，所以大小必须是 2 的幂。 */
#if (BSP_UART_RX_BUF == 0u) || ((BSP_UART_RX_BUF & (BSP_UART_RX_BUF - 1u)) != 0u)
#error "BSP_UART_RX_BUF 必须是 2 的幂"
#endif

/* HSI 是芯片内部 RC 振荡器，F407 固定 16 MHz；bsp.h 只声明了板级参数。 */
#define BSP_HSI_HZ            16000000u

/* PLL 参数（见文件头的时钟树）。PLLP 是编码值：0b00=/2、0b01=/4、0b10=/6、0b11=/8。 */
#define BSP_PLL_VCO_IN_HZ     1000000u                          /* VCO 输入目标 1 MHz */
#define BSP_PLLM              (BSP_HSE_HZ / BSP_PLL_VCO_IN_HZ)  /* 8 MHz / 1 MHz = 8 */
#define BSP_PLLN              336u
#define BSP_PLLP              0u                                /* 编码 0b00 → /2 → 168 MHz */
#define BSP_PLLQ              7u                                /* 336 / 7 = 48 MHz */

/*
 * 编译期守住两条前提：
 *   1) HSE 必须是 1 MHz 的整数倍——PLLM 是整数除法，非整数会静默算错频率；
 *   2) 由 HSE 推导出的 SYSCLK 必须等于 bsp.h 里的 BSP_SYSCLK_HZ。
 * 否则 bsp_sysclk_hz() 会报一个假频率，而 SysTick 与 USART 又都按它算，
 * 时基和波特率会一起偏，且没有任何运行期提示。
 */
#if (BSP_HSE_HZ % BSP_PLL_VCO_IN_HZ) != 0u
#error "BSP_HSE_HZ 必须是 1 MHz 的整数倍（PLLM 用整数除法推导）"
#endif

#if (((BSP_HSE_HZ / BSP_PLLM) * BSP_PLLN) / 2u) != BSP_SYSCLK_HZ
#error "BSP_HSE_HZ 推导出的 SYSCLK 与 BSP_SYSCLK_HZ 不一致"
#endif

/*
 * 等时钟就绪的自旋上限。HSE 起振典型小于 2 ms，这里在 HSI 16 MHz 下也远超
 * 该时间；上限只为"晶振损坏时别把启动流程卡死"，超时就退到 HSI。
 */
#define BSP_CLOCK_WAIT_SPIN   100000u

/*
 * UART 单字节超时。115200 下一个字节约 87 us，1e6 次循环是几十 ms 量级，
 * 只在 TX 脚被短路拉死之类的硬件故障时才会用到。
 */
#define BSP_UART_TX_SPIN      1000000u

/*
 * USART1 中断优先级 0x80（4 位有效优先级时即 8），中低优先级。
 * 接收中断只做搬字节，不应该压过时基和将来可能加入的故障处理。
 */
#define BSP_USART1_IRQ_PRIO   0x80u

/* ---------------------------------------------------------------------
 * 板级状态
 * ------------------------------------------------------------------- */

/* SysTick 中断累加；32 位自然对齐，主循环读一个 LDR 即可，不需要关中断。 */
static volatile uint32_t s_millis;

/* 接收环形缓冲：head 只由中断写，tail 只由主循环写，单生产者/单消费者无锁。 */
static volatile uint8_t  s_rx_ring[BSP_UART_RX_BUF];
static volatile uint32_t s_rx_head;
static volatile uint32_t s_rx_tail;
static volatile uint32_t s_rx_dropped;

/* 实际时钟频率：正常是 168 MHz / 84 MHz，HSE 退路下是 16 MHz / 16 MHz。 */
static uint32_t s_sysclk_hz = BSP_HSI_HZ;
static uint32_t s_pclk2_hz  = BSP_HSI_HZ;
static bool     s_clock_ok;

/* ---------------------------------------------------------------------
 * 时钟
 * ------------------------------------------------------------------- */

static void sync_barriers(void){
    /* 换时钟源/分频后，ARMv7-M 要求 dsb + isb，确保流水线和后续取指
     * 看到新时钟；否则可能带着旧时序继续执行几条指令。 */
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");
}

/*
 * HSE/PLL 任一步失败的退路：继续跑 HSI 16 MHz。
 *
 * 顺序很关键：先把 SW 切回 HSI 并等 SWS 确认，才能关 PLL。若当前正由 PLL
 * 供核而先关 PLL，系统会立刻失去时钟（而不是"降频"）。
 * 若连 SWS 都确认不了（现实中不该发生），宁可频率报告不准也不停摆，
 * 此时保留 PLL/HSE 原状。
 */
static bool board_clock_fallback(void){
    uint32_t spin;
    bool     on_hsi = false;

    RCC->CFGR &= ~RCC_CFGR_SW_Msk;                  /* SW = 0b00 → HSI */
    for (spin = BSP_CLOCK_WAIT_SPIN; spin != 0u; spin--){
        if ((RCC->CFGR & RCC_CFGR_SWS_Msk) == 0u){
            on_hsi = true;
            break;
        }
    }

    if (on_hsi){
        RCC->CR &= ~RCC_CR_PLLON;                   /* 退路用不到 PLL */
        RCC->CR &= ~RCC_CR_HSEON;                   /* 8 MHz 晶振也停掉，省一点电流 */
        /* 退路下总线全部不分频：AHB = APB1 = APB2 = 16 MHz，
         * 这样 USART 的 BRR 只需按 16 MHz 重算。 */
        RCC->CFGR &= ~(RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2);
    }

    sync_barriers();

    s_sysclk_hz = BSP_HSI_HZ;
    s_pclk2_hz  = BSP_HSI_HZ;
    return false;
}

static bool board_clock_init(void){
    uint32_t spin;
    bool     ready;

    /* 1. 起 HSE 并等 HSERDY。 */
    RCC->CR |= RCC_CR_HSEON;
    ready = false;
    for (spin = BSP_CLOCK_WAIT_SPIN; spin != 0u; spin--){
        if ((RCC->CR & RCC_CR_HSERDY) != 0u){
            ready = true;
            break;
        }
    }
    if (!ready){
        return board_clock_fallback();
    }

    /* 2. FLASH 等待周期必须在升频之前设好，否则取指会读到未就绪的 Flash。
     *    168 MHz / 2.7~3.6 V 需要 5 个等待周期，再打开预取和 I/D Cache。 */
    FLASH_R->ACR = FLASH_ACR_LATENCY_5WS | FLASH_ACR_PRFTEN
                 | FLASH_ACR_ICEN | FLASH_ACR_DCEN;

    /* 3. PLL 参数。PLL 此时还关着，所以可以整寄存器写。 */
    RCC->PLLCFGR = (BSP_PLLM << RCC_PLLCFGR_PLLM_POS)
                 | (BSP_PLLN << RCC_PLLCFGR_PLLN_POS)
                 | (BSP_PLLP << RCC_PLLCFGR_PLLP_POS)
                 | RCC_PLLCFGR_PLLSRC_Msk
                 | (BSP_PLLQ << RCC_PLLCFGR_PLLQ_POS);

    RCC->CR |= RCC_CR_PLLON;
    ready = false;
    for (spin = BSP_CLOCK_WAIT_SPIN; spin != 0u; spin--){
        if ((RCC->CR & RCC_CR_PLLRDY) != 0u){
            ready = true;
            break;
        }
    }
    if (!ready){
        return board_clock_fallback();
    }

    /* 4. 先写预分频（此时核还由 HSI 供，改分频不影响当前运行），
     *    最后一步才写 SW 切 PLL。直接整寄存器写：MCO/RTC 预分频等字段
     *    本板都不用，保持复位值 0。 */
    RCC->CFGR = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;
    RCC->CFGR |= RCC_CFGR_SW_PLL;

    /* SWS 的编码与 SW 相同，只是位于 bit[3:2]；轮询确认切换真的发生了。 */
    ready = false;
    for (spin = BSP_CLOCK_WAIT_SPIN; spin != 0u; spin--){
        if ((RCC->CFGR & RCC_CFGR_SWS_Msk) == (RCC_CFGR_SW_PLL << 2U)){
            ready = true;
            break;
        }
    }
    if (!ready){
        return board_clock_fallback();
    }

    sync_barriers();

    s_sysclk_hz = BSP_SYSCLK_HZ;
    s_pclk2_hz  = BSP_SYSCLK_HZ / 2u;       /* PPRE2 = /2 → APB2 = 84 MHz */
    return true;
}

/* ---------------------------------------------------------------------
 * SysTick：1 ms 时基
 * ------------------------------------------------------------------- */

static void board_systick_init(uint32_t ticks_per_ms){
    /* 先写 LOAD、清 VAL，最后才使能：避免使能瞬间用旧重装值跑出一次错拍。 */
    SYSTICK_LOAD = ticks_per_ms - 1u;
    SYSTICK_VAL  = 0u;
    SYSTICK_CTRL = SYSTICK_CTRL_CLKSOURCE | SYSTICK_CTRL_TICKINT | SYSTICK_CTRL_ENABLE;
}

void SysTick_Handler(void){
    s_millis++;
}

/* ---------------------------------------------------------------------
 * GPIO
 * ------------------------------------------------------------------- */

static void gpio_set_mode(gpio_regs_t *gpio, uint32_t pin, uint32_t mode){
    uint32_t pos = pin * 2u;

    gpio->MODER = (gpio->MODER & ~(0x3UL << pos)) | (mode << pos);
}

static void gpio_set_speed(gpio_regs_t *gpio, uint32_t pin, uint32_t speed){
    uint32_t pos = pin * 2u;

    gpio->OSPEEDR = (gpio->OSPEEDR & ~(0x3UL << pos)) | (speed << pos);
}

static void gpio_set_pupd(gpio_regs_t *gpio, uint32_t pin, uint32_t pupd){
    uint32_t pos = pin * 2u;

    gpio->PUPDR = (gpio->PUPDR & ~(0x3UL << pos)) | (pupd << pos);
}

static void gpio_set_af(gpio_regs_t *gpio, uint32_t pin, uint32_t af){
    uint32_t idx = pin >> 3u;            /* 0..7 → AFR[0]，8..15 → AFR[1] */
    uint32_t pos = (pin & 0x7u) * 4u;    /* 每个引脚占 4 位 */

    gpio->AFR[idx] = (gpio->AFR[idx] & ~(0xFUL << pos)) | (af << pos);
}

static void board_gpio_init(void){
    /* 先开时钟再碰任何 GPIO/USART 寄存器；读回一次把 posted 写推到外设侧。 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIODEN;
    (void)RCC->AHB1ENR;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;

    /* 顺序有讲究：先把复用功能号和上下拉配好，最后才写 MODER=AF。
     * 反过来会在切到 AF 的瞬间用复位默认的 AF0 驱动引脚，产生一个毛刺。 */

    /* PA9 = USART1_TX（AF7）：复用推挽。用 very high 速度是为了 115200 下
     * 边沿足够陡，减少位宽畸变。 */
    gpio_set_speed(GPIOA, BSP_PIN_UART_TX, GPIO_OSPEED_VHIGH);
    gpio_set_af(GPIOA, BSP_PIN_UART_TX, GPIO_AF7_USART1);
    gpio_set_mode(GPIOA, BSP_PIN_UART_TX, GPIO_MODE_AF);

    /* PA10 = USART1_RX（AF7）：上拉。空闲时 RX 线保持高电平，避免浮空脚
     * 把噪声当成起始位（真发生时表现为 FE/ORE）。 */
    gpio_set_pupd(GPIOA, BSP_PIN_UART_RX, GPIO_PUPD_PULLUP);
    gpio_set_af(GPIOA, BSP_PIN_UART_RX, GPIO_AF7_USART1);
    gpio_set_mode(GPIOA, BSP_PIN_UART_RX, GPIO_MODE_AF);

    /* PD4 = RS-485 DE（接 MAX485 的 DE 与 /RE）：输出、低速即可（控制信号），
     * 初值低 = 接收态，避免初始化期间抢占总线。 */
    gpio_set_mode(GPIOD, BSP_PIN_RS485_DE, GPIO_MODE_OUTPUT);
    gpio_set_speed(GPIOD, BSP_PIN_RS485_DE, 0u);     /* 0b00 = low speed */
    bsp_rs485_tx(false);

    /* PA6 = LED，低电平点亮：先拉高，初始化完成时是灭的。 */
    gpio_set_mode(GPIOA, BSP_PIN_LED, GPIO_MODE_OUTPUT);
    bsp_led(false);
}

/* ---------------------------------------------------------------------
 * USART1
 * ------------------------------------------------------------------- */

static void board_usart1_init(void){
    /*
     * OVER16 模式下 BRR 的整数值就是 fCK / baud（Mantissa 与 Fraction
     * 拼在同一个 16 位字段里），四舍五入写成 (fCK + baud/2) / baud。
     * 84 MHz / 115200 → 729；退路 16 MHz 时自动变成 139，波特率依然正确。
     */
    USART1->BRR = (s_pclk2_hz + (BSP_UART_BAUD / 2u)) / BSP_UART_BAUD;

    /* 8N1 是 CR1 的 M 位与 CR2 的 STOP 位的复位默认值，不用额外写。
     * UE 最后置位：等 BRR 和中断配置都稳定后再使能，避免用半套配置收发。 */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    USART1->CR1 |= USART_CR1_UE;
}

static void board_nvic_init(void){
    /* IRQ 37 → ISER 第 1 组（32..63）的 bit 5。
     * IPR 每 4 个 IRQ 共用一个 32 位寄存器，每个 IRQ 占 1 个字节。 */
    uint32_t ipr = USART1_IRQN / 4u;
    uint32_t pos = (USART1_IRQN % 4u) * 8u;

    NVIC_IPR(ipr) = (NVIC_IPR(ipr) & ~(0xFFUL << pos)) | (BSP_USART1_IRQ_PRIO << pos);
    NVIC_ISER(USART1_IRQN / 32u) = 1UL << (USART1_IRQN % 32u);
}

/* ---------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------- */

void bsp_init(void){
    /* 先有时钟，后面的 SysTick 重装值和 USART 波特率都依赖实际频率。 */
    s_clock_ok = board_clock_init();

    board_gpio_init();
    board_usart1_init();
    board_nvic_init();

    /* SysTick 最后启动：bsp_millis() 从"板级就绪"开始计时。
     * 1 ms 重装值 = 实际 SYSCLK / 1000；两条路都是编译期常量。 */
    board_systick_init(s_clock_ok ? (BSP_SYSCLK_HZ / 1000u) : (BSP_HSI_HZ / 1000u));
}

uint32_t bsp_millis(void){
    return s_millis;
}

bool bsp_clock_ok(void){
    return s_clock_ok;
}

uint32_t bsp_sysclk_hz(void){
    return s_sysclk_hz;
}

void bsp_delay_ms(uint32_t ms){
    uint32_t start;

    if (ms == 0u){
        return;
    }

    start = bsp_millis();
    /* 用无符号差值比较：即使 bsp_millis() 在等待期间回绕，结果仍然正确。 */
    while ((uint32_t)(bsp_millis() - start) < ms){
    }
}

/* ---------------------------------------------------------------------
 * 发送 / 接收
 * ------------------------------------------------------------------- */

/* 等 USART 的状态位置位；超时返回 false，避免硬件故障时死等。 */
static bool uart_wait_flag(uint32_t flag){
    uint32_t spin = BSP_UART_TX_SPIN;

    while ((USART1->SR & flag) == 0u){
        if (--spin == 0u){
            return false;
        }
    }
    return true;
}

int bsp_uart_write(const uint8_t *buf, uint16_t len){
    uint16_t i;

    if (buf == NULL){
        return -1;
    }
    if (len == 0u){
        return 0;   /* 没有字节要发，也就没有 TC 要等 */
    }

    for (i = 0; i < len; i++){
        if (!uart_wait_flag(USART_SR_TXE)){
            return -1;
        }
        USART1->DR = (uint32_t)buf[i];
    }

    /*
     * 关键：等 TC（发送完成，移位寄存器也空了）再返回。
     * RS-485 引擎在 write 返回后立刻把 DE 拉低；如果只等 TXE，最后一个字节
     * 的停止位还没移出去，总线会被提前掐断，对端收到残帧。
     */
    if (!uart_wait_flag(USART_SR_TC)){
        return -1;
    }
    return 0;
}

uint16_t bsp_uart_read(uint8_t *dst, uint16_t max){
    uint16_t n = 0u;

    if (dst == NULL){
        return 0u;
    }

    while (n < max){
        uint32_t tail = s_rx_tail;

        if (tail == s_rx_head){
            break;      /* 缓冲空 */
        }
        dst[n] = s_rx_ring[tail];
        n++;
        s_rx_tail = (tail + 1u) & (BSP_UART_RX_BUF - 1u);
    }
    return n;
}

void bsp_uart_flush(void){
    /* tail 只由主循环写，直接追平 head 即可；不需要动中断。 */
    s_rx_tail = s_rx_head;
}

uint32_t bsp_uart_rx_dropped(void){
    return s_rx_dropped;
}

void USART1_IRQHandler(void){
    uint32_t sr;
    uint8_t  byte;

    /*
     * 先读 SR 再读 DR：读 DR 会一并清掉 RXNE/ORE/FE，这个顺序是 RM0090
     * 的要求；反过来则可能漏清错误标志，导致中断反复触发。
     */
    sr   = USART1->SR;
    byte = (uint8_t)USART1->DR;

    if ((sr & USART_SR_RXNE) != 0u){
        uint32_t next = (s_rx_head + 1u) & (BSP_UART_RX_BUF - 1u);

        if (next == s_rx_tail){
            /*
             * 缓冲满：丢掉新字节。主循环已经跟不上，保住旧数据可以维持
             * 帧的连续性，比覆盖未读数据更不容易让上层解析出乱帧。
             */
            s_rx_dropped++;
        } else {
            s_rx_ring[s_rx_head] = byte;
            s_rx_head = next;
        }
    }
}

/* ---------------------------------------------------------------------
 * RS-485 方向与 LED
 * ------------------------------------------------------------------- */

void bsp_rs485_tx(bool enable){
    if (enable){
        GPIOD->BSRR = 1UL << BSP_PIN_RS485_DE;               /* DE 拉高 → 发送 */
    } else {
        GPIOD->BSRR = 1UL << (BSP_PIN_RS485_DE + 16u);       /* DE 拉低 → 接收 */
    }
}

bool bsp_rs485_is_tx(void){
    /* 读 ODR 而不是 IDR：这里问的是"我们让 DE 处于什么状态"，
     * 引脚被外部强行拉偏时 IDR 会和实际驱动意图不一致。 */
    return (GPIOD->ODR & (1UL << BSP_PIN_RS485_DE)) != 0u;
}

/*
 * 自环跳线探测。PA10 平时是 AF 输入且带上拉；把 PA9 临时拉低后，
 * 只有两脚之间真的有导线时 PA10 才会读到低电平。探测完立刻恢复 PA9 的
 * 复用配置，随后的串口收发不受影响。
 */
bool bsp_loopback_probe(void){
    const uint32_t shift     = BSP_PIN_UART_TX * 2u;
    const uint32_t moder_af  = GPIO_MODE_AF << shift;
    uint32_t       spin;
    bool           present;

    GPIOA->BSRR = 1UL << (BSP_PIN_UART_TX + 16u);        /* 先把输出锁存拉低 */
    GPIOA->MODER = (GPIOA->MODER & ~(0x3UL << shift)) | (GPIO_MODE_OUTPUT << shift);

    /* 给引脚几微秒稳定：几十 pF 的负载用几个空循环足够。 */
    for (spin = 0u; spin < 1000u; spin++){
        __asm volatile ("nop");
    }

    present = (GPIOA->IDR & (1UL << BSP_PIN_UART_RX)) == 0u;

    GPIOA->MODER = (GPIOA->MODER & ~(0x3UL << shift)) | moder_af;   /* 恢复 AF */

    return present;
}

void bsp_led(bool on){
    /* 板上 LED 接成低电平点亮（阳极经限流电阻到 3.3 V）。 */
    if (on){
        GPIOA->BSRR = 1UL << (BSP_PIN_LED + 16u);            /* 复位位 → 输出低 */
    } else {
        GPIOA->BSRR = 1UL << BSP_PIN_LED;                    /* 置位 → 输出高 */
    }
}

void bsp_led_toggle(void){
    bsp_led((GPIOA->ODR & (1UL << BSP_PIN_LED)) != 0u);      /* 当前为高（灭）→ 点亮 */
}

/* ---------------------------------------------------------------------
 * 中断控制
 * ------------------------------------------------------------------- */

uint32_t bsp_irq_save(void){
    return irq_save();
}

void bsp_irq_restore(uint32_t state){
    irq_restore(state);
}

/* ---------------------------------------------------------------------
 * 喂给 mb_cfg_t 的回调
 * ------------------------------------------------------------------- */

int bsp_mb_write(void *user, const uint8_t *buf, uint16_t len){
    (void)user;
    return bsp_uart_write(buf, len);
}

void bsp_mb_set_tx(void *user, bool tx){
    (void)user;
    bsp_rs485_tx(tx);
}

uint32_t bsp_mb_millis(void *user){
    (void)user;
    return bsp_millis();
}
