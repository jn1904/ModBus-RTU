/*
 * main.c
 *
 * STM32F407VE 板端自测固件。
 *
 * 分两层验证这个 Modbus RTU 库：
 *   1. 纯软件回环：用 mb_memory_t 把主站与从站"直接用线接起来"，不需要任何外部
 *      接线就能把协议栈放到 Cortex-M4 上跑一遍（FC1/3/5/6/15/16 六个功能码；
 *      FC2/FC4 由宿主测试覆盖、板端未单独覆盖）、异常应答、广播、
 *      CRC 静默、超时）；
 *   2. 硬件自环：把真实 USART1 的 PA9/PA10 短接，发一个真实 Modbus 请求帧并
 *      逐字节收回。接线是外部条件，固件无法判断"跳线没接"和"串口坏了"，
 *      因此一个字节都收不到时记 SKIP 而不是 FAIL，避免误导。
 *
 * 为什么不直接 return 或 assert：板端一次烧录要等很久，所有用例都跑完、
 * 汇总成一行再决定 LED 状态，才能一次看到全部问题。断言只累计失败数。
 *
 * 约束：不用 printf / malloc / 浮点 / C 库 I/O；所有存储静态分配，
 * 所有输出都走 bsp_uart_write，字符串全部是字面量或 static const 数组。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bsp.h"
#include "stm32f407ve_regs.h"

#include "modbus.h"
#include "modbus_memory.h"
#include "core/mb_core.h"

/* =====================================================================
 * 输出助手
 *
 * 串口是唯一的人机界面，没有 printf 可用，所以整数转换、长度计算都自己写。
 * 好处是这段代码不依赖 newlib 的格式化/字符串运行时——本工程的链接脚本
 * 并不丢弃 libc（-specs=nano.specs 下链接的是 libc_nano.a，memset/memcpy
 * 都来自它），所以这条约束是纪律而不是链接器强制——printf 其实能链进来，
 * 只是会在裸机上跑进 nosys 桩、并且白占几 KB Flash。
 * =================================================================== */

/* 把 value 的十进制写进 dst，不补 '\0'，返回位数（uint32_t 最多 10 位）。 */
static uint8_t u32_to_dec(uint32_t value, char *dst){
    char    reversed[10];
    uint8_t n = 0u;
    uint8_t i;

    if (value == 0u){
        dst[0] = '0';
        return 1u;
    }

    /* 先低位后高位地取，最后反过来写，避免在内存里搬两次。 */
    while ((value != 0u) && (n < (uint8_t)sizeof(reversed))){
        reversed[n] = (char)('0' + (value % 10u));
        value /= 10u;
        n++;
    }
    for (i = 0u; i < n; i++){
        dst[i] = reversed[n - 1u - i];
    }
    return n;
}

/* 发送一个以 '\0' 结尾的字符串。长度自己数，省掉 strlen 依赖。 */
static void out_str(const char *s){
    uint16_t len = 0u;

    if (s == NULL){
        return;
    }
    while ((s[len] != '\0') && (len < 0xFFFFu)){
        len++;
    }
    if (len == 0u){
        return;
    }
    (void)bsp_uart_write((const uint8_t *)s, len);
}

/* 发送十进制无符号整数。 */
static void out_u32(uint32_t value){
    char    buf[10];
    uint8_t len = u32_to_dec(value, buf);

    (void)bsp_uart_write((const uint8_t *)buf, (uint16_t)len);
}

/* 发送 0xXXXX 形式的 16 位十六进制数（大写数字，固定 4 位，便于对齐对照）。 */
static void out_hex16(uint16_t value){
    static const char digits[] = "0123456789ABCDEF";
    uint8_t           buf[4];
    uint8_t           i;

    for (i = 0u; i < 4u; i++){
        uint8_t shift = (uint8_t)((3u - i) * 4u);

        buf[i] = (uint8_t)digits[(value >> shift) & 0x0Fu];
    }
    out_str("0x");
    (void)bsp_uart_write(buf, 4u);
}

/* =====================================================================
 * 迷你测试框架
 *
 * run_case() 负责一行结果；用例内部只累计失败数，不在第一处失败就返回，
 * 这样一次运行能看到一个用例里的全部问题。第一处失败的细节直接接在
 * [FAIL] 行后面（后续失败只计数），避免刷屏。
 * =================================================================== */

static uint16_t g_checks;        /* 累计断言数     */
static uint16_t g_failures;      /* 累计失败断言数 */
static uint16_t g_pass_cases;
static uint16_t g_fail_cases;
static uint16_t g_skip_cases;

static const char *g_case_name;  /* 当前用例名，供断言打印 [FAIL] 行 */
static bool        g_case_failed_printed;
static bool        g_case_skipped;
static const char *g_case_skip_reason;

/* 成功行里追加的实测值（"：..." 开头），由用例自己填。 */
static char    g_note[64];
static uint8_t g_note_len;

static void note_reset(void){
    g_note_len = 0u;
    g_note[0]  = '\0';
}

static void note_str(const char *s){
    while ((s != NULL) && (*s != '\0')
           && (g_note_len < (uint8_t)(sizeof(g_note) - 1u))){
        g_note[g_note_len] = *s;
        g_note_len++;
        s++;
    }
    g_note[g_note_len] = '\0';
}

static void note_u32(uint32_t value){
    char    buf[10];
    uint8_t n = u32_to_dec(value, buf);
    uint8_t i;

    for (i = 0u; (i < n) && (g_note_len < (uint8_t)(sizeof(g_note) - 1u)); i++){
        g_note[g_note_len] = buf[i];
        g_note_len++;
    }
    g_note[g_note_len] = '\0';
}

/*
 * 本用例的第一处失败：打印 [FAIL] 前缀并返回 true（调用者继续打印细节）；
 * 之后的失败返回 false（只计数不打印）。
 */
static bool fail_first(void){
    if (g_case_failed_printed){
        return false;
    }
    g_case_failed_printed = true;
    out_str("[FAIL] ");
    out_str(g_case_name);
    out_str("：");
    return true;
}

#define CHECK_TRUE(label, cond)                                                  \
    do{                                                                          \
        g_checks++;                                                              \
        if (!(cond)){                                                            \
            g_failures++;                                                        \
            if (fail_first()){                                                   \
                out_str(label);                                                  \
            }                                                                    \
        }                                                                        \
    }while (0)

#define CHECK_U16(label, actual, expected)                                       \
    do{                                                                          \
        uint16_t a_ = (uint16_t)(actual);                                        \
        uint16_t e_ = (uint16_t)(expected);                                      \
        g_checks++;                                                              \
        if (a_ != e_){                                                           \
            g_failures++;                                                        \
            if (fail_first()){                                                   \
                out_str(label);                                                  \
                out_str(" ");                                                    \
                out_hex16(a_);                                                   \
                out_str(" 期望 ");                                               \
                out_hex16(e_);                                                   \
            }                                                                    \
        }                                                                        \
    }while (0)

#define CHECK_U32(label, actual, expected)                                       \
    do{                                                                          \
        uint32_t a_ = (uint32_t)(actual);                                        \
        uint32_t e_ = (uint32_t)(expected);                                      \
        g_checks++;                                                              \
        if (a_ != e_){                                                           \
            g_failures++;                                                        \
            if (fail_first()){                                                   \
                out_str(label);                                                  \
                out_str(" ");                                                    \
                out_u32(a_);                                                     \
                out_str(" 期望 ");                                               \
                out_u32(e_);                                                     \
            }                                                                    \
        }                                                                        \
    }while (0)

/* 区间断言：把实测值一起打出来，否则很难判断是"偏一点"还是"完全没动"。 */
#define CHECK_RANGE(label, actual, lo, hi)                                       \
    do{                                                                          \
        uint32_t a_ = (uint32_t)(actual);                                        \
        g_checks++;                                                              \
        if ((a_ < (uint32_t)(lo)) || (a_ > (uint32_t)(hi))){                     \
            g_failures++;                                                        \
            if (fail_first()){                                                   \
                out_str(label);                                                  \
                out_str(" ");                                                    \
                out_u32(a_);                                                     \
                out_str("，应在 ");                                              \
                out_u32((uint32_t)(lo));                                         \
                out_str("..");                                                   \
                out_u32((uint32_t)(hi));                                         \
            }                                                                    \
        }                                                                        \
    }while (0)

/* 库错误码断言：同时打印短名和数值，接串口日志时不用回查头文件。 */
#define CHECK_ERR(label, actual, expected)                                       \
    do{                                                                          \
        mb_err_t a_ = (actual);                                                  \
        mb_err_t e_ = (expected);                                                \
        g_checks++;                                                              \
        if (a_ != e_){                                                           \
            g_failures++;                                                        \
            if (fail_first()){                                                   \
                out_str(label);                                                  \
                out_str(" 返回 ");                                               \
                out_str(mb_err_str(a_));                                         \
                out_str("(");                                                    \
                out_u32((uint32_t)a_);                                           \
                out_str(")，期望 ");                                             \
                out_str(mb_err_str(e_));                                         \
                out_str("(");                                                    \
                out_u32((uint32_t)e_);                                           \
                out_str(")");                                                    \
            }                                                                    \
        }                                                                        \
    }while (0)

/* 记一个 SKIP：用例没有失败、只是外部条件不满足（如自环跳线没接）。 */
static void case_skip(const char *reason){
    g_case_skipped     = true;
    g_case_skip_reason = reason;
}

/*
 * 跑一个用例并打印一行结果。失败优先于 skip：如果用例已经断言失败，
 * 就不应该再把它算成"跳过"。
 */
static void run_case(const char *name, void (*fn)(void)){
    uint16_t failures_before = g_failures;

    g_case_name           = name;
    g_case_failed_printed = false;
    g_case_skipped        = false;
    g_case_skip_reason    = "";
    note_reset();

    fn();

    if (g_failures != failures_before){
        g_fail_cases++;
        /* [FAIL] 行和细节已经在断言处打印了，这里只补行尾。 */
        out_str("\r\n");
        return;
    }

    if (g_case_skipped){
        g_skip_cases++;
        out_str("[skip] ");
        out_str(name);
        out_str("：");
        out_str(g_case_skip_reason);
        out_str("\r\n");
        return;
    }

    g_pass_cases++;
    out_str("[ ok ] ");
    out_str(name);
    out_str(g_note);
    out_str("\r\n");
}

/* =====================================================================
 * 软件回环总线
 *
 * 主站与从站各持一侧的 mb_memory_t：主站 write() 捕获帧并直接推进从站的
 * mb_rx()，从站应答时再反向推进主站。整条"线"上没有真实硬件，但字节流、
 * T3.5 判帧、超时全部走库的真实代码路径。
 * =================================================================== */

#define LOOPBACK_REGS 32u

typedef struct loopback_bus{
    mb_t        master;
    mb_t        slave;
    mb_memory_t mwire;   /* 主站侧：捕获查询，转发给从站 */
    mb_memory_t swire;   /* 从站侧：捕获应答，转发给主站 */
    mb_map_t    map;
} loopback_bus_t;

static loopback_bus_t g_bus;
static uint16_t       g_regs[LOOPBACK_REGS];

/* 每个用例开始前清空内存映像，避免上一个用例的写入残留影响判断。 */
static void clear_regs(void){
    uint16_t i;

    for (i = 0u; i < (uint16_t)LOOPBACK_REGS; i++){
        g_regs[i] = 0u;
    }
}

/*
 * 重新接好一条"软件总线"：清内存、重绑 map、两侧都重新 mb_init。
 * 每个用例都调用它，保证用例之间零污染。
 */
static mb_err_t bus_init(void){
    mb_cfg_t mcfg;
    mb_cfg_t scfg;
    mb_err_t err;

    clear_regs();
    mb_map_shared(&g_bus.map, g_regs, (uint16_t)LOOPBACK_REGS);

    mb_memory_init(&g_bus.mwire, &g_bus.slave);
    mb_memory_init(&g_bus.swire, &g_bus.master);

    /*
     * 逐字段赋值而不是 memset：C 库的 memset 在本工程里要尽量少用，
     * 而且显式写出每个字段，改配置时不容易漏。
     */
    mcfg.mode       = MB_MODE_MASTER;
    mcfg.write      = mb_memory_write;
    mcfg.set_tx     = mb_memory_set_tx;
    mcfg.millis     = bsp_mb_millis;
    mcfg.user       = &g_bus.mwire;
    mcfg.slave_id   = 0u;
    mcfg.map        = NULL;
    mcfg.t35_ms     = 2u;    /* 115200 下 T3.5 = 1.75 ms，取 2 ms */
    mcfg.timeout_ms = 50u;

    scfg.mode       = MB_MODE_SLAVE;
    scfg.write      = mb_memory_write;
    scfg.set_tx     = mb_memory_set_tx;
    scfg.millis     = bsp_mb_millis;
    scfg.user       = &g_bus.swire;
    scfg.slave_id   = 1u;
    scfg.map        = &g_bus.map;
    scfg.t35_ms     = 2u;
    scfg.timeout_ms = 50u;

    err = mb_init(&g_bus.master, &mcfg);
    if (err != MB_OK){
        return err;
    }
    return mb_init(&g_bus.slave, &scfg);
}

/*
 * 泵一轮：先真实等待 3 ms 越过 T3.5（库靠毫秒时钟判帧尾），再依次让从站
 * 定帧/应答、主站定帧/收结果。顺序不能反：从站先把应答写上线，主站才能看到。
 */
static void bus_pump_once(void){
    bsp_delay_ms(3u);
    mb_poll(&g_bus.slave);
    mb_poll(&g_bus.master);
}

/*
 * 发起一笔主站查询并等结果，最多 8 轮。
 * 8 轮约 24 ms，远小于 50 ms 超时；正常应答第 2 轮就能取到。
 * 万一一直没结果，再主动等到超时并把结果取走——否则 waiting/has_result
 * 会残留，下一笔查询会被 MB_ERR_POLLING 拒绝，污染后续用例。
 */
static mb_err_t do_query(uint8_t fc, uint16_t addr, uint16_t count, uint16_t *data){
    mb_query_t q;
    mb_err_t   err;
    uint8_t    i;

    q.slave_id = 1u;
    q.fc       = fc;
    q.addr     = addr;
    q.count    = count;
    q.data     = data;

    err = mb_master_send(&g_bus.master, &q);
    if (err != MB_OK){
        return err;
    }

    for (i = 0u; i < 8u; i++){
        bus_pump_once();
        if (mb_master_done(&g_bus.master, &err)){
            return err;
        }
    }

    /*
     * 兜底：泵满 8 轮还没结果，就让主站自己走到超时（配置是 50 ms），
     * 再把结果取走。否则 mb_master_send 的 waiting 状态会残留，下一个
     * 用例一进来就拿到 MB_ERR_POLLING。
     */
    while (mb_master_busy(&g_bus.master)){
        bsp_delay_ms(5u);
        mb_poll(&g_bus.master);
    }
    if (mb_master_done(&g_bus.master, &err)){
        return err;
    }
    return MB_ERR_TIME_OUT;
}

/* 绕过主站 API，把一帧"裸 body"（不含 CRC）直接灌给从站。 */
static void push_body_to_slave(const uint8_t *body, uint16_t len){
    uint8_t  frame[MB_MAX_FRAME];
    uint16_t crc;
    uint16_t i;

    for (i = 0u; i < len; i++){
        frame[i] = body[i];
    }
    crc             = mb_crc16(body, len);
    frame[len]      = (uint8_t)(crc & 0x00FFu);   /* 线上低字节在前 */
    frame[len + 1u] = (uint8_t)(crc >> 8);

    mb_rx(&g_bus.slave, frame, (uint16_t)(len + 2u));
}

/* 校验一帧末尾的 CRC（线上低字节在前），用于检查从站发出来的原始字节。 */
static bool frame_crc_ok(const uint8_t *buf, uint16_t len){
    uint16_t crc;

    if (len < 4u){
        return false;
    }
    crc = mb_crc16(buf, (uint16_t)(len - 2u));

    return (buf[len - 2u] == (uint8_t)(crc & 0x00FFu))
        && (buf[len - 1u] == (uint8_t)((crc >> 8) & 0x00FFu));
}

/* =====================================================================
 * 用例 1：时钟
 * =================================================================== */

static void test_clock(void){
    uint32_t hz = bsp_sysclk_hz();
    bool     ok = bsp_clock_ok();

    CHECK_TRUE("bsp_clock_ok() 应为真", ok);
    CHECK_U32("SYSCLK(Hz)", hz, BSP_SYSCLK_HZ);

    /*
     * 只比 bsp_sysclk_hz() 是"自证"——那是 board.c 自己写进去的常量。
     * 这里再从 RCC 把 PLL 配置读回来重算一遍：配置写错、没生效、被别的
     * 代码改掉，都会在这一组断言里暴露。
     */
    if (ok){
        uint32_t pllcfgr = RCC->PLLCFGR;
        uint32_t cfgr    = RCC->CFGR;
        uint32_t m    = (pllcfgr & RCC_PLLCFGR_PLLM_Msk) >> RCC_PLLCFGR_PLLM_POS;
        uint32_t n    = (pllcfgr & RCC_PLLCFGR_PLLN_Msk) >> RCC_PLLCFGR_PLLN_POS;
        uint32_t p_enc = (pllcfgr & RCC_PLLCFGR_PLLP_Msk) >> RCC_PLLCFGR_PLLP_POS;
        uint32_t p    = 2u * (p_enc + 1u);      /* 编码 0b00→2、0b01→4 … */

        CHECK_TRUE("PLL 已使能", (RCC->CR & RCC_CR_PLLRDY) != 0u);
        CHECK_U32("PLLM(由 HSE 推导)", m, BSP_HSE_HZ / 1000000u);
        CHECK_U32("PLLN", n, 336u);
        CHECK_U32("由 RCC 重算的 SYSCLK(Hz)", (BSP_HSE_HZ / m) * n / p, BSP_SYSCLK_HZ);
        CHECK_U32("SWS 已选 PLL", cfgr & RCC_CFGR_SWS_Msk, RCC_CFGR_SW_PLL << 2u);
        CHECK_U32("AHB 不分频", cfgr & RCC_CFGR_HPRE_Msk, RCC_CFGR_HPRE_DIV1);
        CHECK_U32("APB1 分频 /4", cfgr & RCC_CFGR_PPRE1_Msk, RCC_CFGR_PPRE1_DIV4);
        CHECK_U32("APB2 分频 /2", cfgr & RCC_CFGR_PPRE2_Msk, RCC_CFGR_PPRE2_DIV2);
        CHECK_U32("FLASH 等待周期", FLASH_R->ACR & FLASH_ACR_LATENCY_Msk,
                  FLASH_ACR_LATENCY_5WS);
    }

    /* 成功行里带上实际主频；退回 HSI 时把真实值也写出来，便于判断。 */
    if (ok && (hz == BSP_SYSCLK_HZ)){
        note_str("：已切到 ");
        note_u32(hz / 1000000u);
        note_str(" MHz");
    }
    else{
        note_str("：SYSCLK ");
        note_u32(hz);
        note_str(" Hz");
    }
}

/* =====================================================================
 * 用例 2：时基
 * =================================================================== */

static void test_millis(void){
    uint32_t t0;
    uint32_t t1;
    uint32_t dt;

    t0 = bsp_millis();
    bsp_delay_ms(10u);
    t1 = bsp_millis();
    dt = (uint32_t)(t1 - t0);   /* 无符号减法，计数回绕也安全 */

    /*
     * ±50% 的窗口：SysTick 与忙等循环的边界误差、中断延迟都算进去还留余量，
     * 但又能一下抓住"HSE 没起振导致 SysTick 重装值不对、时基快/慢 10 倍"这类问题。
     */
    CHECK_RANGE("bsp_delay_ms(10) 实测间隔(ms)", dt, 8u, 25u);

    note_str("：10 ms 延时实测 ");
    note_u32(dt);
    note_str(" ms");
}

/* =====================================================================
 * 用例 3：中断临界区
 * =================================================================== */

static void test_irq_critical(void){
    uint32_t first;
    uint32_t second;

    /*
     * 先把两次 save 都做完再断言：断言失败要往串口写数据，若此时全局中断
     * 还关着，board.c 里依时钟的忙等可能又不可用，属于自找麻烦。
     */
    first = bsp_irq_save();
    bsp_irq_restore(first);       /* 先恢复，后面才能再取一次"干净"的状态 */
    second = bsp_irq_save();
    bsp_irq_restore(0u);          /* 主循环里 PRIMASK 初值就是 0 */

    CHECK_U32("首次 bsp_irq_save() 返回", first, 0u);
    CHECK_U32("恢复后再次 bsp_irq_save() 返回", second, 0u);

    note_str("：PRIMASK 保存/恢复正常");
}

/* =====================================================================
 * 用例 4：CRC16 规范样例
 * =================================================================== */

static void test_crc(void){
    static const uint8_t sample[6] = { 0x11u, 0x03u, 0x00u, 0x6Bu, 0x00u, 0x03u };

    /* MODBUS over serial line V1.02 的官方例子：CRC=0x8776，线上是 76 87。 */
    CHECK_U16("mb_crc16(规范样例)", (uint16_t)mb_crc16(sample, 6u), 0x8776u);

    note_str("：规范样例 0x8776");
}

/* =====================================================================
 * 用例 5：协议内存回环（核心）
 * =================================================================== */

/* FC6：写单个保持寄存器。 */
static void test_loopback_fc6(void){
    uint16_t buf[1];

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    buf[0] = 0xBEEFu;
    CHECK_ERR("FC6 写单个寄存器", do_query(MB_FC_WRITE_REG, 5u, 1u, buf), MB_OK);
    CHECK_U16("写入后 regs[5]", g_regs[5], 0xBEEFu);

    note_str("：写单个寄存器");
}

/* FC3：一次读回 3 个保持寄存器，验证主站把应答数据搬进了 data[]。 */
static void test_loopback_fc3(void){
    uint16_t buf[3];

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    g_regs[4] = 0x1111u;
    g_regs[5] = 0x2222u;
    g_regs[6] = 0x3333u;
    buf[0] = 0u;
    buf[1] = 0u;
    buf[2] = 0u;

    CHECK_ERR("FC3 读 3 个保持寄存器", do_query(MB_FC_READ_HOLDING_REGS, 4u, 3u, buf), MB_OK);
    CHECK_U16("读回 [0]", buf[0], 0x1111u);
    CHECK_U16("读回 [1]", buf[1], 0x2222u);
    CHECK_U16("读回 [2]", buf[2], 0x3333u);

    note_str("：读回 3 个寄存器");
}

/* FC16：一次写 4 个保持寄存器。 */
static void test_loopback_fc16(void){
    uint16_t buf[4];

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    buf[0] = 1u;
    buf[1] = 2u;
    buf[2] = 3u;
    buf[3] = 4u;
    CHECK_ERR("FC16 写 4 个寄存器", do_query(MB_FC_WRITE_REGS, 10u, 4u, buf), MB_OK);
    CHECK_U16("写入后 regs[10]", g_regs[10], 1u);
    CHECK_U16("写入后 regs[11]", g_regs[11], 2u);
    CHECK_U16("写入后 regs[12]", g_regs[12], 3u);
    CHECK_U16("写入后 regs[13]", g_regs[13], 4u);

    note_str("：写 4 个寄存器");
}

/* FC5：置位再复位同一个线圈，验证 0xFF00/0x0000 两个线上取值的处理。 */
static void test_loopback_fc5(void){
    uint16_t buf[1];

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    buf[0] = 1u;
    CHECK_ERR("FC5 置位线圈", do_query(MB_FC_WRITE_COIL, 0u, 1u, buf), MB_OK);
    CHECK_U16("置位后 regs[0] 的位 0", (uint16_t)(g_regs[0] & 0x0001u), 1u);

    buf[0] = 0u;
    CHECK_ERR("FC5 复位线圈", do_query(MB_FC_WRITE_COIL, 0u, 1u, buf), MB_OK);
    CHECK_U16("复位后 regs[0] 的位 0", (uint16_t)(g_regs[0] & 0x0001u), 0u);

    note_str("：置位/复位线圈");
}

/* FC1：读 10 个线圈，验证位在字节/字之间的 LSB 在前打包。 */
static void test_loopback_fc1(void){
    uint16_t buf[1];

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    g_regs[0] = 0x0009u;   /* 位 0 和位 3 为 1 */
    buf[0]    = 0u;
    CHECK_ERR("FC1 读 10 个线圈", do_query(MB_FC_READ_COILS, 0u, 10u, buf), MB_OK);
    CHECK_U16("读回线圈 [0]", buf[0], 0x0009u);

    note_str("：读 10 个线圈");
}

/*
 * FC15：写 5 个线圈，起始地址 16。
 * 位打包约定为 LSB 在前：data[0]=0x0015 的位 0/2/4 落在 Modbus 地址 16/18/20，
 * 也就是 regs[1] 的低 5 位 → regs[1] 应等于 0x0015。
 */
static void test_loopback_fc15(void){
    uint16_t buf[1];

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    buf[0] = 0x0015u;
    CHECK_ERR("FC15 写 5 个线圈", do_query(MB_FC_WRITE_COILS, 16u, 5u, buf), MB_OK);
    CHECK_U16("写入后 regs[1]", g_regs[1], 0x0015u);

    note_str("：写 5 个线圈");
}

/* =====================================================================
 * 用例 6：异常应答
 * =================================================================== */

static void test_exception(void){
    static const uint8_t body[6] = { 0x01u, 0x03u, 0x03u, 0xE8u, 0x00u, 0x01u };
    uint16_t buf[1];
    uint16_t out_before;

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    /* 地址 1000 远超 32 个寄存器：主站屏面上应看到"对端回异常帧"。 */
    buf[0] = 0u;
    CHECK_ERR("FC3 读越界地址 1000",
              do_query(MB_FC_READ_HOLDING_REGS, 1000u, 1u, buf), MB_ERR_EXCEPTION);

    /*
     * 再绕过主站 API 直接给从站灌同样的越界请求，检查异常帧的线上字节。
     * 主站 API 本身会做参数校验，只有裸帧才能验证从站"功能码|0x80"的行为。
     */
    mb_memory_reset(&g_bus.swire);
    out_before = mb_out_count(&g_bus.slave);

    push_body_to_slave(body, 6u);
    bus_pump_once();

    CHECK_U16("异常应答帧长", g_bus.swire.len, 5u);
    CHECK_U16("异常应答从站地址", g_bus.swire.buf[0], 0x01u);
    CHECK_U16("异常应答功能码", g_bus.swire.buf[1], 0x83u);
    CHECK_U16("异常应答异常码", g_bus.swire.buf[2], 0x02u);
    CHECK_TRUE("异常应答 CRC 自洽", frame_crc_ok(g_bus.swire.buf, g_bus.swire.len));
    CHECK_U16("从站应答帧计数 +1", mb_out_count(&g_bus.slave),
              (uint16_t)(out_before + 1u));

    note_str("：01 83 02，CRC 自洽");
}

/* =====================================================================
 * 用例 7：广播
 * =================================================================== */

static void test_broadcast(void){
    static const uint8_t body[6] = { 0x00u, 0x06u, 0x00u, 0x14u, 0x12u, 0x34u };
    uint16_t out_before;

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    mb_memory_reset(&g_bus.swire);
    out_before = mb_out_count(&g_bus.slave);

    push_body_to_slave(body, 6u);   /* 从站地址 0 即广播 */
    bus_pump_once();

    CHECK_U16("广播写入 regs[0x14]", g_regs[0x14], 0x1234u);
    CHECK_U16("从站应答帧计数不变", mb_out_count(&g_bus.slave), out_before);
    CHECK_U16("线上没有应答字节", g_bus.swire.len, 0u);

    note_str("：写入生效且不应答");
}

/* =====================================================================
 * 用例 8：CRC 坏时静默
 * =================================================================== */

static void test_bad_crc(void){
    static const uint8_t body[6] = { 0x01u, 0x06u, 0x00u, 0x14u, 0x12u, 0x34u };
    uint8_t  frame[8];
    uint16_t crc;
    uint16_t out_before;
    uint16_t err_before;
    uint16_t i;

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    mb_memory_reset(&g_bus.swire);
    out_before = mb_out_count(&g_bus.slave);
    err_before = mb_err_count(&g_bus.slave);

    for (i = 0u; i < 6u; i++){
        frame[i] = body[i];
    }
    crc             = mb_crc16(body, 6u);
    frame[6]        = (uint8_t)(crc & 0x00FFu);
    frame[7]        = (uint8_t)(crc >> 8);
    frame[7]       ^= 0xFFu;   /* 破坏线上最后一个 CRC 字节：从站必须一个字节都不回 */

    mb_rx(&g_bus.slave, frame, 8u);
    bus_pump_once();

    CHECK_U16("CRC 坏时不应答", mb_out_count(&g_bus.slave), out_before);
    CHECK_U16("CRC 坏时线上无字节", g_bus.swire.len, 0u);
    CHECK_U16("从站错误计数 +1", mb_err_count(&g_bus.slave),
              (uint16_t)(err_before + 1u));
    CHECK_ERR("最后错误", mb_last_error(&g_bus.slave), MB_ERR_BAD_CRC);
    CHECK_U16("CRC 坏时写入未生效", g_regs[0x14], 0u);

    note_str("：保持静默，错误计入统计");
}

/* =====================================================================
 * 用例 9：主站超时
 *
 * 另建一个主站实例，它的"线"没有对端：字节发出去没人接，查询只能靠超时收尾。
 * 不能复用 g_bus.master——那边连着从站，永远会有应答。
 * =================================================================== */

static mb_t        g_timeout_master;
static mb_memory_t g_timeout_wire;

static void test_timeout(void){
    mb_cfg_t   cfg;
    mb_query_t q;
    uint16_t   buf[1];
    mb_err_t   err;

    mb_memory_init(&g_timeout_wire, NULL);   /* 对端为 NULL：只捕获，不转发 */

    cfg.mode       = MB_MODE_MASTER;
    cfg.write      = mb_memory_write;
    cfg.set_tx     = mb_memory_set_tx;
    cfg.millis     = bsp_mb_millis;
    cfg.user       = &g_timeout_wire;
    cfg.slave_id   = 0u;
    cfg.map        = NULL;
    cfg.t35_ms     = 2u;
    cfg.timeout_ms = 20u;

    CHECK_ERR("超时用主站初始化", mb_init(&g_timeout_master, &cfg), MB_OK);

    buf[0] = 0u;
    q.slave_id = 1u;
    q.fc       = MB_FC_READ_HOLDING_REGS;
    q.addr     = 0u;
    q.count    = 1u;
    q.data     = buf;

    CHECK_ERR("发送查询", mb_master_send(&g_timeout_master, &q), MB_OK);

    /* 20 ms 超时，等 25 ms 后 poll 一次必然已经过期。 */
    bsp_delay_ms(25u);
    mb_poll(&g_timeout_master);

    err = MB_OK;
    CHECK_TRUE("查询应有结果", mb_master_done(&g_timeout_master, &err));
    CHECK_ERR("无应答应超时", err, MB_ERR_TIME_OUT);

    note_str("：MB_ERR_TIME_OUT");
}

/* =====================================================================
 * 用例 10：UART 硬件自环（PA9/PA10 短接时才有意义）
 * =================================================================== */

static void test_uart_loopback(void){
    mb_frame_t f;
    mb_query_t q;
    uint16_t   data[3];
    uint8_t    rx[MB_MAX_FRAME];
    uint16_t   rx_len = 0u;
    uint32_t   t0;
    uint8_t    i;
    bool       match = true;
    bool       write_ok;

    /* 构造函数成功时才会写 len；先清 0，万一把来的参数校验失败也不会拿未初始化长度去发。 */
    f.len = 0u;

    /* 用库自己的构造函数造一个真实 FC3 请求：11 03 006B 0003 + CRC（76 87）。 */
    data[0]    = 0u;
    data[1]    = 0u;
    data[2]    = 0u;
    q.slave_id = 0x11u;
    q.fc       = MB_FC_READ_HOLDING_REGS;
    q.addr     = 0x006Bu;
    q.count    = 3u;
    q.data     = data;

    CHECK_ERR("构造 FC3 请求", mb_frame_build_query(&f, &q), MB_OK);
    mb_frame_seal(&f);
    CHECK_U16("请求帧长", f.len, 8u);

    /*
     * 走板级回调而不是裸 bsp_uart_write：这样"引擎真实的调用链"也被跑到——
     * DE 拉高 → bsp_mb_write 发送并等 TC → DE 拉低。这条路径平时只有接真实
     * RS-485 总线才会执行，--gc-sections 否则会把它整段裁掉。
     */
    bsp_uart_flush();
    bsp_mb_set_tx(NULL, true);
    write_ok = (bsp_mb_write(NULL, f.buf, f.len) == 0);
    bsp_mb_set_tx(NULL, false);
    CHECK_TRUE("bsp_mb_write 返回 0", write_ok);

    t0 = bsp_millis();
    while (((uint32_t)(bsp_millis() - t0) < 200u) && (rx_len < f.len)){
        uint16_t n = bsp_uart_read(&rx[rx_len], (uint16_t)(f.len - rx_len));

        rx_len = (uint16_t)(rx_len + n);
        if (n == 0u){
            bsp_delay_ms(1u);   /* 没收到就先让出 1 ms，别把主循环卡成忙等 */
        }
    }

    if (rx_len == 0u){
        /*
         * 一个字节都没回来。先探一下 PA9–PA10 之间到底有没有跳线：
         * 有跳线还收不到，说明接收中断/引脚复用/波特率真有问题，必须判失败；
         * 没跳线才记 SKIP。否则"串口坏了"会被悄悄当成"没条件测"。
         */
        if (bsp_loopback_probe()){
            CHECK_TRUE("跳线已接却收不到字节（检查 RX 中断/复用/波特率）", false);
            return;
        }
        case_skip("未收到字节（PA9/PA10 未短接）");
        return;
    }

    CHECK_U16("自环收到字节数", rx_len, f.len);
    for (i = 0u; i < rx_len; i++){
        if (rx[i] != f.buf[i]){
            match = false;
        }
    }
    if (rx_len != f.len){
        match = false;
    }
    CHECK_TRUE("自环逐字节相等", match);

    note_str("：");
    note_u32(rx_len);
    note_str(" 字节逐字节一致");
}

/* =====================================================================
 * 用例：RS-485 方向控制
 *
 * 桌面上没法自动测 MAX485 的 A/B 差分电平，但可以确认"我们让 DE 处于什么
 * 状态"与读回值一致，以及连续翻转不会卡在某一态——这条路径一坏，半双工
 * 收发就会整条失效，而且现象往往像"协议不工作"，很难往引脚上想。
 * =================================================================== */

static void test_rs485_dir(void){
    bool    ok;
    uint8_t i;

    bsp_rs485_tx(true);
    CHECK_TRUE("DE 拉高后读回应为发送", bsp_rs485_is_tx());

    bsp_rs485_tx(false);
    CHECK_TRUE("DE 拉低后读回应为接收", !bsp_rs485_is_tx());

    /* 连翻几次：确认不是只有第一次生效（例如 BSRR 位算错时会一直保持）。 */
    ok = true;
    for (i = 0u; i < 4u; i++){
        bsp_rs485_tx(true);
        ok = ok && bsp_rs485_is_tx();
        bsp_rs485_tx(false);
        ok = ok && !bsp_rs485_is_tx();
    }
    CHECK_TRUE("DE 连续翻转 4 次", ok);

    note_str("：DE 电平跟随");
}

/* =====================================================================
 * 用例：统计计数与诊断钩子
 *
 * 一次成功的查询应当让主站 out_count、从站 in_count/out_count、主站
 * in_count 各加一；再故意触发一次用法错误，确认全局钩子被回调——这是
 * 现场排障时唯一能拿到的库内部视角。
 * =================================================================== */

static uint16_t g_hook_calls;

static void on_error(mb_err_t err, const char *where){
    (void)err;
    (void)where;
    g_hook_calls++;
}

static void test_stats(void){
    uint16_t   buf[1];
    mb_query_t q;

    CHECK_ERR("回环初始化", bus_init(), MB_OK);

    g_hook_calls = 0u;
    mb_err_set_hook(on_error);

    buf[0] = 0x0102u;
    CHECK_ERR("FC6 写寄存器", do_query(MB_FC_WRITE_REG, 1u, 1u, buf), MB_OK);

    CHECK_U16("主站 out_count", mb_out_count(&g_bus.master), 1u);
    CHECK_U16("从站 in_count",  mb_in_count(&g_bus.slave), 1u);
    CHECK_U16("从站 out_count", mb_out_count(&g_bus.slave), 1u);
    CHECK_U16("主站 in_count",  mb_in_count(&g_bus.master), 1u);

    /* 从站地址 248 会被主站 API 的参数校验拦住，正好用来触发钩子。 */
    q.slave_id = 248u;
    q.fc       = MB_FC_READ_HOLDING_REGS;
    q.addr     = 0u;
    q.count    = 1u;
    q.data     = buf;
    CHECK_ERR("非法从站 ID", mb_master_send(&g_bus.master, &q), MB_ERR_BAD_SLAVE_ID);
    CHECK_TRUE("诊断钩子被回调", g_hook_calls >= 1u);

    mb_err_set_hook(NULL);
    note_str("：计数与钩子正常");
}
/* =====================================================================
 * main
 * =================================================================== */

int main(void){
    /* board.c 的契约：bsp_init() 之后所有 bsp_* 与接收中断才可用。 */
    bsp_init();

    bsp_led(false);
    out_str("\r\nModbus RTU · STM32F407VE 自测\r\n");
    out_str("SYSCLK = ");
    out_u32(bsp_sysclk_hz());
    out_str(" Hz\r\n\r\n");

    run_case("时钟", test_clock);
    run_case("时基", test_millis);
    run_case("中断临界区", test_irq_critical);
    run_case("CRC16", test_crc);
    run_case("协议回环 FC6", test_loopback_fc6);
    run_case("协议回环 FC3", test_loopback_fc3);
    run_case("协议回环 FC16", test_loopback_fc16);
    run_case("协议回环 FC5", test_loopback_fc5);
    run_case("协议回环 FC1", test_loopback_fc1);
    run_case("协议回环 FC15", test_loopback_fc15);
    run_case("异常应答", test_exception);
    run_case("广播", test_broadcast);
    run_case("CRC 坏静默", test_bad_crc);
    run_case("主站超时", test_timeout);
    run_case("RS-485 方向", test_rs485_dir);
    run_case("统计与钩子", test_stats);
    run_case("UART 自环", test_uart_loopback);

    out_str("\r\n断言：");
    out_u32(g_checks);
    out_str(" 项，失败 ");
    out_u32(g_failures);
    out_str(" 项\r\n");
    out_str("----------------------------------------\r\n");
    out_str("结果：");
    out_u32(g_pass_cases);
    out_str(" 通过，");
    out_u32(g_fail_cases);
    out_str(" 失败，");
    out_u32(g_skip_cases);
    out_str(" 跳过\r\n");

    /*
     * 无人值守时 LED 就是最终结论：有失败常亮（一眼看出需要接调试器），
     * 全绿才进入 500 ms 闪烁。这两个分支都不返回，板子会一直保持这个状态。
     */
    if (g_fail_cases > 0u){
        bsp_led(true);
        for (;;){
            bsp_delay_ms(1000u);
        }
    }

    for (;;){
        bsp_led_toggle();
        bsp_delay_ms(500u);
    }

    return 0;   /* 不可达：上面两个循环都是死循环 */
}
