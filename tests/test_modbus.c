/*
 * test_modbus.c
 *
 * 本机测试：不需要任何硬件、RTOS 或操作系统。分两层验证：
 *
 *   1. core 层单元测试——直接调用 mb_crc16 / mb_frame_build_query /
 *      mb_frame_validate_request 等私有函数，脱离实例与时钟核对协议细节；
 *   2. 回环联调——用 mb_memory_t 把主站与从站"接"在一起，跑完整的
 *      查询 → 应答 → 取结果流程，以及异常、静默、广播、超时等分支。
 *
 * 时间由假时钟 g_now 驱动，每一轮泵都先把时钟推过 T3.5，mb_poll() 才能定帧。
 */

#include <stdio.h>
#include <string.h>

#include "modbus.h"
#include "modbus_memory.h"
#include "core/mb_core.h"

/* ---------------------------------------------------------------------
 * 极简断言：失败只打印、累计，最后统一给退出码
 * ------------------------------------------------------------------- */

static int g_checks;
static int g_failures;
static int g_tests;

#define CHECK(cond)                                                              \
    do{                                                                          \
        g_checks++;                                                              \
        if (!(cond)){                                                            \
            g_failures++;                                                        \
            (void)printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                        \
    }while (0)

#define CHECK_U(actual, expected)                                                \
    do{                                                                          \
        unsigned a_ = (unsigned)(actual);                                        \
        unsigned e_ = (unsigned)(expected);                                      \
        g_checks++;                                                              \
        if (a_ != e_){                                                           \
            g_failures++;                                                        \
            (void)printf("    FAIL %s:%d: %s == 0x%04X，期望 0x%04X\n",          \
                         __FILE__, __LINE__, #actual, a_, e_);                   \
        }                                                                        \
    }while (0)

static void run(const char *name, void (*fn)(void)){
    int before = g_failures;

    fn();
    g_tests++;
    (void)printf("%s %s\n", (g_failures == before) ? "[ ok ]" : "[FAIL]", name);
}

/* ---------------------------------------------------------------------
 * 假时钟与回环总线
 * ------------------------------------------------------------------- */

static uint32_t g_now;

static uint32_t now_ms(void *user){
    (void)user;
    return g_now;
}

/* 四个区域共享的一整块存储（经典单内存模型）。 */
static uint16_t g_regs[32];

typedef struct bus{
    mb_t        master;
    mb_t        slave;
    mb_memory_t mwire;   /* 主站这一侧的"线"，把字节推给从站 */
    mb_memory_t swire;   /* 从站这一侧的"线"，把字节推给主站 */
    mb_map_t    map;
} bus_t;

/**
 * @param connected false 时主站的字节发出去没人接，用来验证超时。
 */
static void bus_init(bus_t *b, bool connected){
    mb_cfg_t mcfg;
    mb_cfg_t scfg;

    memset(b, 0, sizeof(*b));
    memset(g_regs, 0, sizeof(g_regs));
    g_now = 1000u;                 /* 故意不从 0 开始：时钟起点不该影响判帧 */
    mb_map_shared(&b->map, g_regs, 32u);

    mb_memory_init(&b->mwire, connected ? &b->slave : NULL);
    mb_memory_init(&b->swire, &b->master);

    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.mode   = MB_MODE_MASTER;
    mcfg.write  = mb_memory_write;
    mcfg.set_tx = mb_memory_set_tx;
    mcfg.millis = now_ms;
    mcfg.user   = &b->mwire;

    memset(&scfg, 0, sizeof(scfg));
    scfg.mode     = MB_MODE_SLAVE;
    scfg.write    = mb_memory_write;
    scfg.set_tx   = mb_memory_set_tx;
    scfg.millis   = now_ms;
    scfg.user     = &b->swire;
    scfg.slave_id = 1u;
    scfg.map      = &b->map;

    CHECK(mb_init(&b->master, &mcfg) == MB_OK);
    CHECK(mb_init(&b->slave, &scfg) == MB_OK);
}

/* 推进若干轮：每轮都把时钟推过 T3.5，两侧各跑一次状态机。 */
static void pump(bus_t *b, int rounds){
    int i;

    for (i = 0; i < rounds; i++){
        g_now += 10u;
        mb_poll(&b->slave);
        mb_poll(&b->master);
    }
}

/* 发起一次查询并等到结果被取走。 */
static mb_err_t do_query(bus_t *b, uint8_t fc, uint16_t addr, uint16_t count, uint16_t *data){
    mb_query_t q;
    mb_err_t   err;
    int        i;

    q.slave_id = 1u;
    q.fc       = fc;
    q.addr     = addr;
    q.count    = count;
    q.data     = data;

    err = mb_master_send(&b->master, &q);
    if (err != MB_OK){
        return err;
    }

    for (i = 0; i < 8; i++){
        pump(b, 1);
        if (mb_master_done(&b->master, &err)){
            return err;
        }
    }
    return MB_ERR_TIME_OUT;   /* 没等到结果，按超时处理 */
}

/* 往从站直接灌一帧裸数据（body 不含 CRC），绕过主站状态机。 */
static void push_to_slave(bus_t *b, const uint8_t *body, uint16_t len){
    uint8_t  frame[MB_MAX_FRAME];
    uint16_t crc;

    memcpy(frame, body, (size_t)len);
    crc = mb_crc16(body, len);
    frame[len]      = (uint8_t)(crc & 0x00FFu);
    frame[len + 1u] = (uint8_t)(crc >> 8);

    mb_rx(&b->slave, frame, (uint16_t)(len + 2u));
}

static bool crc_ok(const uint8_t *buf, uint16_t len){
    uint16_t crc;

    if (len < 4u){
        return false;
    }
    crc = mb_crc16(buf, (uint16_t)(len - 2u));

    return (buf[len - 2u] == (uint8_t)(crc & 0x00FFu))
        && (buf[len - 1u] == (uint8_t)(crc >> 8));
}

/* ---------------------------------------------------------------------
 * 1. CRC16：规范例子（MODBUS over serial line V1.02）+ 帧尾自洽
 * ------------------------------------------------------------------- */

static void test_crc(void){
    static const uint8_t request[]  = { 0x11, 0x03, 0x00, 0x6B, 0x00, 0x03 };
    static const uint8_t response[] = { 0x11, 0x03, 0x06, 0xAE, 0x41, 0x56, 0x52, 0x43, 0x40 };
    static const uint8_t except[]   = { 0x11, 0x83, 0x02 };
    uint8_t  frame[16];

    /* 规范里的三个例子；上线的字节顺序是低字节在前。 */
    CHECK_U(mb_crc16(request, 6u), 0x8776u);    /* 线上 76 87 */
    CHECK_U(mb_crc16(response, 9u), 0xAD49u);   /* 线上 49 AD */
    CHECK_U(mb_crc16(except, 3u), 0x34C1u);     /* 线上 C1 34 */

    /* 追加 CRC 之后，整帧的余数必须是 0——这是 RTU 校验的本质。 */
    {
        uint16_t crc = mb_crc16(request, 6u);

        memcpy(frame, request, sizeof(request));
        frame[6] = (uint8_t)(crc & 0x00FFu);
        frame[7] = (uint8_t)(crc >> 8);
    }
    CHECK_U(mb_crc16(frame, 8u), 0u);
    CHECK(crc_ok(frame, 8u));

    /* 单字节翻转必须被 CRC 抓住。 */
    frame[3] ^= 0x01u;
    CHECK(!crc_ok(frame, 8u));
}

/* ---------------------------------------------------------------------
 * 2. 帧构造：只填内容、CRC 由 mb_frame_seal() 统一收尾
 * ------------------------------------------------------------------- */

static void test_frame_build(void){
    mb_frame_t f;
    mb_query_t q;
    uint16_t   buf[4];

    /* FC3 读 3 个保持寄存器 @0x006B，应与规范请求逐字节一致。 */
    memset(&f, 0, sizeof(f));
    q.slave_id = 0x11u;
    q.fc       = MB_FC_READ_HOLDING_REGS;
    q.addr     = 0x006Bu;
    q.count    = 3u;
    q.data     = buf;

    CHECK(mb_frame_build_query(&f, &q) == MB_OK);
    CHECK_U(f.len, 6u);                        /* 构造阶段不含 CRC */
    CHECK_U(f.buf[0], 0x11u);
    CHECK_U(f.buf[1], 0x03u);
    CHECK_U(f.buf[2], 0x00u);
    CHECK_U(f.buf[3], 0x6Bu);
    CHECK_U(f.buf[4], 0x00u);
    CHECK_U(f.buf[5], 0x03u);

    mb_frame_seal(&f);
    CHECK_U(f.len, 8u);
    CHECK_U(f.buf[6], 0x76u);                  /* CRC 低字节在前 */
    CHECK_U(f.buf[7], 0x87u);
    CHECK(crc_ok(f.buf, f.len));

    /* FC16 多寄存器写：地址 + 功能码 + 地址 + 数量 + 字节数 + 载荷。 */
    memset(&f, 0, sizeof(f));
    buf[0] = 0x1234u;
    buf[1] = 0xABCDu;
    q.slave_id = 0x11u;
    q.fc       = MB_FC_WRITE_REGS;
    q.addr     = 0x0001u;
    q.count    = 2u;
    q.data     = buf;

    CHECK(mb_frame_build_query(&f, &q) == MB_OK);
    CHECK_U(f.len, 11u);
    CHECK_U(f.buf[4], 0x00u);
    CHECK_U(f.buf[5], 0x02u);
    CHECK_U(f.buf[6], 0x04u);                  /* 字节数 = 2 * 2 */
    CHECK_U(f.buf[7], 0x12u);
    CHECK_U(f.buf[8], 0x34u);
    CHECK_U(f.buf[9], 0xABu);
    CHECK_U(f.buf[10], 0xCDu);

    /* FC5：data[0] 非 0 就在线上写 0xFF00。 */
    memset(&f, 0, sizeof(f));
    buf[0] = 1u;
    q.fc    = MB_FC_WRITE_COIL;
    q.addr  = 7u;
    q.count = 1u;
    CHECK(mb_frame_build_query(&f, &q) == MB_OK);
    CHECK_U(f.buf[4], 0xFFu);
    CHECK_U(f.buf[5], 0x00u);
}

/* ---------------------------------------------------------------------
 * 3. 请求校验：链路错误 vs 协议异常的分界
 * ------------------------------------------------------------------- */

/* 手工造一个请求帧（地址 + 功能码 + 地址 + 数量 + CRC）。
   校验函数先查 CRC 再看内容，所以每个用例都必须重新盖 CRC，
   直接改已封口的帧只会得到 MB_ERR_BAD_CRC。 */
static mb_frame_t make_request(uint8_t id, uint8_t fc, uint16_t addr, uint16_t count){
    mb_frame_t f;

    memset(&f, 0, sizeof(f));
    f.buf[0] = id;
    f.buf[1] = fc;
    f.buf[2] = (uint8_t)(addr >> 8);
    f.buf[3] = (uint8_t)(addr & 0x00FFu);
    f.buf[4] = (uint8_t)(count >> 8);
    f.buf[5] = (uint8_t)(count & 0x00FFu);
    f.len    = 6u;
    mb_frame_seal(&f);

    return f;
}

static void test_validate_request(void){
    mb_frame_t f;
    mb_map_t   map;
    uint16_t   store[8];

    memset(store, 0, sizeof(store));
    memset(&map, 0, sizeof(map));
    map.holding.data  = store;
    map.holding.start = 0u;
    map.holding.count = 8u;

    /* 合法请求 */
    f = make_request(1u, MB_FC_READ_HOLDING_REGS, 0u, 8u);
    CHECK(mb_frame_validate_request(&f, &map) == MB_OK);

    /* CRC 坏：链路错误，从站要静默 */
    f = make_request(1u, MB_FC_READ_HOLDING_REGS, 0u, 8u);
    f.buf[3] ^= 0xFFu;
    CHECK(mb_frame_validate_request(&f, &map) == MB_ERR_BAD_CRC);

    /* 帧太短 */
    f = make_request(1u, MB_FC_READ_HOLDING_REGS, 0u, 8u);
    f.len = 4u;
    CHECK(mb_frame_validate_request(&f, &map) == MB_ERR_BAD_SIZE);

    /* 不支持的功能码：应回异常码 1 */
    f = make_request(1u, 0x2Bu, 0u, 1u);       /* 读设备标识，本库不实现 */
    CHECK(mb_frame_validate_request(&f, &map) == MB_EXC_FUNC_CODE);

    /* 数量越界：回异常码 3 */
    f = make_request(1u, MB_FC_READ_HOLDING_REGS, 0u, 200u);
    CHECK(mb_frame_validate_request(&f, &map) == MB_EXC_REGS_QUANT);

    /* 地址越界：回异常码 2（8 个寄存器：0..7，地址 8 就已经出界） */
    f = make_request(1u, MB_FC_READ_HOLDING_REGS, 8u, 1u);
    CHECK(mb_frame_validate_request(&f, &map) == MB_EXC_ADDR_RANGE);

    /* 区域没挂存储：一律按地址非法处理 */
    {
        mb_map_t empty;

        memset(&empty, 0, sizeof(empty));
        f = make_request(1u, MB_FC_READ_HOLDING_REGS, 0u, 1u);
        CHECK(mb_frame_validate_request(&f, &empty) == MB_EXC_ADDR_RANGE);
    }
}

/* ---------------------------------------------------------------------
 * 4. 功能码执行（core 层，不经实例）：区域起始地址、FC2/FC4、FC15
 * ------------------------------------------------------------------- */

static void test_func_execute(void){
    uint16_t  holding[4];
    uint16_t  input[4];
    uint16_t  coils[1];
    uint16_t  discrete[1];
    mb_map_t  map;
    mb_frame_t f;
    mb_query_t q;
    uint16_t  buf[4];

    holding[0] = 1u;  holding[1] = 2u;  holding[2] = 3u;  holding[3] = 4u;
    input[0]   = 10u; input[1]   = 20u; input[2]   = 30u; input[3]   = 40u;
    coils[0]    = 0x0005u;    /* 位 0 和位 2 为 1 */
    discrete[0] = 0x000Au;    /* 位 1 和位 3 为 1 */

    memset(&map, 0, sizeof(map));
    map.coils.data = coils;      map.coils.start = 0u;    map.coils.count = 1u;
    map.discrete.data = discrete; map.discrete.start = 16u; map.discrete.count = 1u;
    map.input.data = input;      map.input.start = 0u;    map.input.count = 4u;
    map.holding.data = holding;  map.holding.start = 100u; map.holding.count = 4u;

    /* FC4 读输入寄存器 */
    memset(&f, 0, sizeof(f));
    q.slave_id = 1u; q.fc = MB_FC_READ_INPUT_REGS; q.addr = 0u; q.count = 4u; q.data = buf;
    CHECK(mb_frame_build_query(&f, &q) == MB_OK);
    mb_frame_seal(&f);
    CHECK(mb_frame_validate_request(&f, &map) == MB_OK);
    mb_frame_process_request(&f, &map, false);
    CHECK_U(f.len, 11u);                     /* 功能码 + 字节数 + 8 字节数据 */
    CHECK_U(f.buf[2], 8u);
    CHECK_U(f.buf[3], 0x00u);                /* 10  = 0x000A */
    CHECK_U(f.buf[4], 0x0Au);
    CHECK_U(f.buf[9], 0x00u);                /* 40  = 0x0028 */
    CHECK_U(f.buf[10], 0x28u);

    /* FC2 读离散输入，起始地址 16（区域 start 不是 0 的情况） */
    memset(&f, 0, sizeof(f));
    q.fc = MB_FC_READ_DISCRETE_INPUTS; q.addr = 16u; q.count = 4u;
    CHECK(mb_frame_build_query(&f, &q) == MB_OK);
    mb_frame_seal(&f);
    CHECK(mb_frame_validate_request(&f, &map) == MB_OK);
    mb_frame_process_request(&f, &map, false);
    CHECK_U(f.buf[2], 1u);                   /* 4 个位压成 1 字节 */
    CHECK_U(f.buf[3], 0x0Au);

    /* FC3 读保持寄存器，起始地址 100 */
    memset(&f, 0, sizeof(f));
    q.fc = MB_FC_READ_HOLDING_REGS; q.addr = 100u; q.count = 2u;
    CHECK(mb_frame_build_query(&f, &q) == MB_OK);
    mb_frame_seal(&f);
    CHECK(mb_frame_validate_request(&f, &map) == MB_OK);
    mb_frame_process_request(&f, &map, false);
    CHECK_U(f.buf[3], 0x00u);
    CHECK_U(f.buf[4], 0x01u);
    CHECK_U(f.buf[5], 0x00u);
    CHECK_U(f.buf[6], 0x02u);

    /* FC6 写单个寄存器，地址 103 → holding[3] */
    memset(&f, 0, sizeof(f));
    buf[0] = 0x55AAu;
    q.fc = MB_FC_WRITE_REG; q.addr = 103u; q.count = 1u; q.data = buf;
    CHECK(mb_frame_build_query(&f, &q) == MB_OK);
    mb_frame_seal(&f);
    CHECK(mb_frame_validate_request(&f, &map) == MB_OK);
    mb_frame_process_request(&f, &map, false);
    CHECK_U(holding[3], 0x55AAu);
    CHECK_U(f.len, 6u);                      /* 写应答只回显前 6 字节 */

    /* FC15 写多个线圈：地址 8，5 个位，LSB 在前 */
    memset(&f, 0, sizeof(f));
    buf[0] = 0x0015u;                        /* 位 0、2、4 */
    q.fc = MB_FC_WRITE_COILS; q.addr = 8u; q.count = 5u; q.data = buf;
    CHECK(mb_frame_build_query(&f, &q) == MB_OK);
    mb_frame_seal(&f);
    CHECK(mb_frame_validate_request(&f, &map) == MB_OK);
    mb_frame_process_request(&f, &map, false);
    CHECK_U(coils[0], 0x1505u);              /* 低 8 位里的 0b10101 落到位 8..12 */
}

/* ---------------------------------------------------------------------
 * 5. 回环：寄存器读 / 单写 / 多写
 * ------------------------------------------------------------------- */

static void test_loopback_registers(void){
    bus_t    b;
    uint16_t buf[8];

    bus_init(&b, true);

    /* FC6 写单个寄存器 */
    buf[0] = 0xBEEFu;
    CHECK(do_query(&b, MB_FC_WRITE_REG, 5u, 1u, buf) == MB_OK);
    CHECK_U(g_regs[5], 0xBEEFu);

    /* FC3 读回同一片 */
    memset(buf, 0, sizeof(buf));
    g_regs[4] = 0x1111u;
    g_regs[5] = 0x2222u;
    g_regs[6] = 0x3333u;
    CHECK(do_query(&b, MB_FC_READ_HOLDING_REGS, 4u, 3u, buf) == MB_OK);
    CHECK_U(buf[0], 0x1111u);
    CHECK_U(buf[1], 0x2222u);
    CHECK_U(buf[2], 0x3333u);

    /* FC16 写 4 个寄存器 */
    buf[0] = 1u;
    buf[1] = 2u;
    buf[2] = 3u;
    buf[3] = 4u;
    CHECK(do_query(&b, MB_FC_WRITE_REGS, 10u, 4u, buf) == MB_OK);
    CHECK_U(g_regs[10], 1u);
    CHECK_U(g_regs[11], 2u);
    CHECK_U(g_regs[12], 3u);
    CHECK_U(g_regs[13], 4u);
}

/* ---------------------------------------------------------------------
 * 6. 回环：线圈读 / 单写 / 多写
 * ------------------------------------------------------------------- */

static void test_loopback_coils(void){
    bus_t    b;
    uint16_t buf[8];

    bus_init(&b, true);

    /* FC5 置位 / 复位：位 0、位 3 */
    buf[0] = 1u;
    CHECK(do_query(&b, MB_FC_WRITE_COIL, 0u, 1u, buf) == MB_OK);
    CHECK_U(g_regs[0], 0x0001u);

    buf[0] = 1u;
    CHECK(do_query(&b, MB_FC_WRITE_COIL, 3u, 1u, buf) == MB_OK);
    CHECK_U(g_regs[0], 0x0009u);

    buf[0] = 0u;
    CHECK(do_query(&b, MB_FC_WRITE_COIL, 0u, 1u, buf) == MB_OK);
    CHECK_U(g_regs[0], 0x0008u);

    /* FC1 读 10 个线圈：字里的位按 LSB 在前装进字节流，再打包回字。
       此时只有位 3 是 1，读窗口 0..9 里它也落在位 3。 */
    memset(buf, 0, sizeof(buf));
    CHECK(do_query(&b, MB_FC_READ_COILS, 0u, 10u, buf) == MB_OK);
    CHECK_U(buf[0], 0x0008u);

    /* FC15 写多个线圈：地址 16，5 个位 */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x0015u;                        /* 位 0、2、4 */
    CHECK(do_query(&b, MB_FC_WRITE_COILS, 16u, 5u, buf) == MB_OK);
    CHECK_U(g_regs[1], 0x0015u);
}

/* ---------------------------------------------------------------------
 * 7. 异常应答：越界地址要以"功能码 | 0x80"回帧
 * ------------------------------------------------------------------- */

static void test_exception_reply(void){
    bus_t    b;
    uint8_t  body[6];
    uint16_t buf[2];

    bus_init(&b, true);

    /* 直接从从站这一侧验证异常帧的字节：地址 1 + 0x83 + 异常码 2 + CRC。 */
    body[0] = 0x01u;
    body[1] = 0x03u;
    body[2] = 0x03u;                          /* 地址 0x03E8 = 1000，远超 32 个寄存器 */
    body[3] = 0xE8u;
    body[4] = 0x00u;
    body[5] = 0x01u;
    push_to_slave(&b, body, 6u);
    pump(&b, 2);

    CHECK_U(b.swire.len, 5u);
    CHECK_U(b.swire.buf[0], 0x01u);
    CHECK_U(b.swire.buf[1], 0x83u);            /* 0x03 | 0x80 */
    CHECK_U(b.swire.buf[2], 0x02u);            /* 非法数据地址 */
    CHECK(crc_ok(b.swire.buf, b.swire.len));

    /* 主站侧只看到"对端回了异常帧"，具体异常码由从站日志提供。 */
    memset(buf, 0, sizeof(buf));
    CHECK(do_query(&b, MB_FC_READ_HOLDING_REGS, 1000u, 1u, buf) == MB_ERR_EXCEPTION);

    /* 数量非法：异常码 3 */
    body[2] = 0x00u;
    body[3] = 0x00u;
    body[4] = 0x00u;
    body[5] = 0x00u;                           /* 读 0 个寄存器 */
    push_to_slave(&b, body, 6u);
    pump(&b, 2);
    CHECK_U(b.swire.buf[2], 0x03u);
}

/* ---------------------------------------------------------------------
 * 8. 链路错误：CRC 坏时从站必须保持静默
 * ------------------------------------------------------------------- */

static void test_silent_on_bad_crc(void){
    bus_t    b;
    uint8_t  frame[8];
    uint16_t crc;
    uint16_t out_before;
    uint16_t err_before;
    static const uint8_t body[6] = { 0x01, 0x06, 0x00, 0x02, 0x12, 0x34 };

    bus_init(&b, true);

    mb_memory_reset(&b.swire);
    out_before = mb_out_count(&b.slave);
    err_before = mb_err_count(&b.slave);

    memcpy(frame, body, sizeof(body));
    crc = mb_crc16(body, 6u);
    frame[6] = (uint8_t)(crc & 0x00FFu);
    frame[7] = (uint8_t)(crc >> 8);
    frame[6] ^= 0xFFu;                          /* 故意破坏 CRC */

    mb_rx(&b.slave, frame, 8u);
    pump(&b, 2);

    CHECK_U(mb_out_count(&b.slave), out_before);          /* 没有应答 */
    CHECK_U(b.swire.len, 0u);                             /* 线上一个字节都没发 */
    CHECK_U(mb_err_count(&b.slave), (unsigned)err_before + 1u);
    CHECK(mb_last_error(&b.slave) == MB_ERR_BAD_CRC);
    CHECK_U(g_regs[2], 0u);                               /* 写入没有生效 */
}

/* ---------------------------------------------------------------------
 * 9. 广播：写入生效、不应答
 * ------------------------------------------------------------------- */

static void test_broadcast(void){
    bus_t    b;
    uint16_t out_before;
    static const uint8_t body[6] = { 0x00, 0x06, 0x00, 0x14, 0x12, 0x34 };

    bus_init(&b, true);

    mb_memory_reset(&b.swire);
    out_before = mb_out_count(&b.slave);

    push_to_slave(&b, body, 6u);                /* 从站地址 0 = 广播 */
    pump(&b, 2);

    CHECK_U(g_regs[0x14], 0x1234u);             /* 写入生效 */
    CHECK_U(mb_out_count(&b.slave), out_before); /* 但不应答 */
    CHECK_U(b.swire.len, 0u);

    /* 主站 API 不允许用广播地址发起查询 */
    {
        mb_query_t q;
        uint16_t   v = 0u;

        q.slave_id = 0u;
        q.fc       = MB_FC_WRITE_REG;
        q.addr     = 1u;
        q.count    = 1u;
        q.data     = &v;
        CHECK(mb_master_send(&b.master, &q) == MB_ERR_BAD_ROLE_ID);
    }
}

/* ---------------------------------------------------------------------
 * 10. 超时：对端不存在时，查询必须以 MB_ERR_TIME_OUT 收尾
 * ------------------------------------------------------------------- */

static void test_timeout(void){
    bus_t    b;
    uint16_t buf[2];

    bus_init(&b, false);                        /* 主站的字节发出去没人接 */

    memset(buf, 0, sizeof(buf));
    CHECK(mb_master_send(&b.master, &(mb_query_t){
        .slave_id = 1u,
        .fc       = MB_FC_READ_HOLDING_REGS,
        .addr     = 0u,
        .count    = 1u,
        .data     = buf,
    }) == MB_OK);

    CHECK(mb_master_busy(&b.master));

    g_now += 1001u;                             /* 越过默认的 1000 ms 超时 */
    mb_poll(&b.master);

    CHECK(!mb_master_busy(&b.master));
    {
        mb_err_t err = MB_OK;
        CHECK(mb_master_done(&b.master, &err));
        CHECK(err == MB_ERR_TIME_OUT);
        CHECK(mb_last_error(&b.master) == MB_ERR_TIME_OUT);
    }

    /* 结果取走之后才能发下一笔 */
    CHECK(mb_master_send(&b.master, &(mb_query_t){
        .slave_id = 1u,
        .fc       = MB_FC_READ_HOLDING_REGS,
        .addr     = 0u,
        .count    = 1u,
        .data     = buf,
    }) == MB_OK);
}

/* ---------------------------------------------------------------------
 * 11. 用法错误与错误分级
 * ------------------------------------------------------------------- */

static void test_error_model(void){
    bus_t    b;
    uint16_t buf[2];

    bus_init(&b, true);

    CHECK(mb_err_classify(MB_OK) == MB_CLASS_OK);
    CHECK(mb_err_classify(MB_EXC_ADDR_RANGE) == MB_CLASS_PROTO_EXC);
    CHECK(mb_err_classify(MB_ERR_TIME_OUT) == MB_CLASS_COMM);
    CHECK(mb_err_classify(MB_ERR_BAD_CRC) == MB_CLASS_COMM);
    CHECK(mb_err_classify(MB_ERR_NOT_MASTER) == MB_CLASS_USAGE);
    CHECK(mb_err_classify(MB_ERR_BAD_CONFIG) == MB_CLASS_CONFIG);

    CHECK_U(mb_err_exception_byte(MB_EXC_REGS_QUANT), 3u);
    CHECK_U(mb_err_exception_byte(MB_ERR_TIME_OUT), 0u);
    CHECK(mb_err_str(MB_EXC_ADDR_RANGE) != NULL);
    CHECK(mb_err_str(MB_OK) != NULL);

    /* 从站实例上调主站 API */
    {
        mb_query_t q;

        q.slave_id = 1u;
        q.fc       = MB_FC_READ_HOLDING_REGS;
        q.addr     = 0u;
        q.count    = 1u;
        q.data     = buf;
        CHECK(mb_master_send(&b.slave, &q) == MB_ERR_NOT_MASTER);
    }

    /* 主站参数检查 */
    {
        mb_query_t q;

        q.slave_id = 1u;
        q.fc       = MB_FC_READ_HOLDING_REGS;
        q.addr     = 0u;
        q.count    = 0u;                        /* 数量 0 */
        q.data     = buf;
        CHECK(mb_master_send(&b.master, &q) == MB_ERR_BAD_SIZE);

        q.count    = 1u;
        q.data     = NULL;                      /* 没有数据缓冲区 */
        CHECK(mb_master_send(&b.master, &q) == MB_ERR_NO_DATA);

        q.data     = buf;
        q.slave_id = 248u;                      /* 超出 1..247 */
        CHECK(mb_master_send(&b.master, &q) == MB_ERR_BAD_SLAVE_ID);

        q.slave_id = 1u;
        q.fc       = 0x2Bu;                     /* 本库不支持的功能码 */
        CHECK(mb_master_send(&b.master, &q) == MB_ERR_BAD_FC);
    }

    /* 上一笔结果未取走前不能再发 */
    {
        mb_query_t q;

        q.slave_id = 1u;
        q.fc       = MB_FC_READ_HOLDING_REGS;
        q.addr     = 0u;
        q.count    = 1u;
        q.data     = buf;
        CHECK(mb_master_send(&b.master, &q) == MB_OK);
        CHECK(mb_master_busy(&b.master));
        CHECK(mb_master_send(&b.master, &q) == MB_ERR_POLLING);   /* 在途 */

        pump(&b, 4);                                              /* 跑完，结果就绪 */
        CHECK(mb_master_send(&b.master, &q) == MB_ERR_POLLING);   /* 结果未被取走 */
        CHECK(mb_master_done(&b.master, NULL));

        CHECK(mb_master_send(&b.master, &q) == MB_OK);            /* 取走之后才能再发 */
        pump(&b, 4);
        CHECK(mb_master_done(&b.master, NULL));
    }
}

/* ---------------------------------------------------------------------
 * 12. 配置校验
 * ------------------------------------------------------------------- */

static void test_init_validation(void){
    mb_t     mb;
    mb_cfg_t cfg;
    mb_map_t map;
    uint16_t store[4];

    memset(store, 0, sizeof(store));
    mb_map_shared(&map, store, 4u);

    memset(&cfg, 0, sizeof(cfg));
    cfg.mode   = MB_MODE_SLAVE;
    cfg.write  = mb_memory_write;
    cfg.millis = now_ms;
    cfg.map    = &map;

    CHECK(mb_init(NULL, &cfg) == MB_ERR_BAD_CONFIG);
    CHECK(mb_init(&mb, NULL) == MB_ERR_BAD_CONFIG);

    /* 缺少必需回调 */
    cfg.slave_id = 1u;
    cfg.write    = NULL;
    CHECK(mb_init(&mb, &cfg) == MB_ERR_BAD_CONFIG);

    cfg.write = mb_memory_write;
    cfg.millis = NULL;
    CHECK(mb_init(&mb, &cfg) == MB_ERR_BAD_CONFIG);

    /* 从站地址 0 是广播地址 */
    cfg.millis   = now_ms;
    cfg.slave_id = 0u;
    CHECK(mb_init(&mb, &cfg) == MB_ERR_BAD_CONFIG);

    cfg.slave_id = 248u;
    CHECK(mb_init(&mb, &cfg) == MB_ERR_BAD_CONFIG);

    /* 一个区域都没挂存储 */
    cfg.slave_id = 1u;
    cfg.map      = NULL;
    CHECK(mb_init(&mb, &cfg) == MB_ERR_BAD_CONFIG);

    /* 合法从站配置 */
    cfg.map = &map;
    CHECK(mb_init(&mb, &cfg) == MB_OK);
    CHECK_U(mb.cfg.t35_ms, (unsigned)MB_T35_MS_DEFAULT);   /* 0 → 取默认值 */
    CHECK_U(mb.cfg.timeout_ms, (unsigned)MB_TIMEOUT_MS_DEFAULT);

    /* 主站不需要 map 与 slave_id */
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode   = MB_MODE_MASTER;
    cfg.write  = mb_memory_write;
    cfg.millis = now_ms;
    CHECK(mb_init(&mb, &cfg) == MB_OK);
}

/* ---------------------------------------------------------------------
 * 13. 统计与诊断钩子
 * ------------------------------------------------------------------- */

static int g_hook_calls;
static mb_err_t g_hook_last;

static void on_error(mb_err_t err, const char *where){
    g_hook_calls++;
    g_hook_last = err;
    CHECK(where != NULL);
}

static void test_stats_and_hook(void){
    bus_t    b;
    uint16_t buf[2];

    mb_err_set_hook(on_error);
    g_hook_calls = 0;

    bus_init(&b, true);

    buf[0] = 0x0102u;
    CHECK(do_query(&b, MB_FC_WRITE_REG, 1u, 1u, buf) == MB_OK);

    CHECK_U(mb_out_count(&b.master), 1u);
    CHECK_U(mb_out_count(&b.slave), 1u);
    CHECK_U(mb_in_count(&b.slave), 1u);
    CHECK_U(mb_in_count(&b.master), 1u);

    /* 触发一次用法错误，钩子应当被调用 */
    {
        mb_query_t q;

        q.slave_id = 248u;
        q.fc       = MB_FC_READ_HOLDING_REGS;
        q.addr     = 0u;
        q.count    = 1u;
        q.data     = buf;
        CHECK(mb_master_send(&b.master, &q) == MB_ERR_BAD_SLAVE_ID);
    }
    CHECK(g_hook_calls >= 1);
    CHECK(g_hook_last == MB_ERR_BAD_SLAVE_ID);
    CHECK(mb_last_error(&b.master) == MB_ERR_BAD_SLAVE_ID);

    mb_err_set_hook(NULL);
}

/* ---------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */

int main(void){
    (void)printf("Modbus RTU 本机测试\n");

    run("CRC16 规范样例与帧尾自洽", test_crc);
    run("帧构造（FC3 / FC16 / FC5）", test_frame_build);
    run("请求校验：链路错误与协议异常", test_validate_request);
    run("功能码执行：区域偏移 / FC2 / FC4 / FC15", test_func_execute);
    run("回环：寄存器 FC3 / FC6 / FC16", test_loopback_registers);
    run("回环：线圈 FC1 / FC5 / FC15", test_loopback_coils);
    run("异常应答：功能码 | 0x80", test_exception_reply);
    run("链路错误：CRC 坏时保持静默", test_silent_on_bad_crc);
    run("广播：写入生效且不应答", test_broadcast);
    run("主站超时", test_timeout);
    run("错误模型与用法错误", test_error_model);
    run("配置校验", test_init_validation);
    run("统计与诊断钩子", test_stats_and_hook);

    (void)printf("\n%d 个用例，%d 项断言，%d 项失败\n", g_tests, g_checks, g_failures);

    return (g_failures == 0) ? 0 : 1;
}
