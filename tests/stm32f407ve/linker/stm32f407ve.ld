/*
 * stm32f407ve.ld —— STM32F407VE（512 KB Flash / 128 KB SRAM + 64 KB CCM）
 *
 * 应用只跑在 Flash 前 512 KB 内；中断向量表放在 Flash 起始处（VTOR 默认值）。
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
    CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
}

/* 栈顶 = SRAM 末端；MSP 从这里向下生长 */
_estack = ORIGIN(RAM) + LENGTH(RAM);

_Min_Stack_Size = 0x1000;   /* 4 KB：链接期就能发现栈被挤爆 */
_Min_Heap_Size  = 0x0;      /* 本库不使用 malloc */

SECTIONS
{
    /* 中断向量表必须是镜像的第一个字节 */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } >FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
    .ARM : {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } >FLASH

    /* .data 的运行地址在 RAM，加载地址紧跟 .text 之后留在 Flash */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT> FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } >RAM

    /* 链接期检查：RAM 装得下 .bss 之后还要留出最小栈 */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } >RAM

    /* 不要 DISCARD libc/libgcc：--gc-sections 已经能丢掉未用代码，
     * 而无条件丢弃 libgcc.a 会在真的需要 __aeabi_* 助手函数时链接失败。 */

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
