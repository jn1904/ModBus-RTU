/*
 * startup_stm32f407xx.s
 *
 * STM32F407VE 的启动文件：中断向量表 + Reset_Handler。
 * 只用标准 GNU as 语法，不依赖 ST 的 CMSIS 启动实现。
 *
 * 复位流程：设置 MSP → 使能 FPU → 从 Flash 复制 .data → 清零 .bss →
 * 进入 main()。所有未实现的中断都落到 Default_Handler 的原地跳转上。
 */

    .syntax unified
    .cpu cortex-m4
    .fpu fpv4-sp-d16
    .thumb

/* ---------------------------------------------------------------------
 * 弱符号向量：任何在 C 里定义的同名函数（如 SysTick_Handler）都会覆盖它
 * ------------------------------------------------------------------- */
    .macro VECTOR handler
    .word \handler
    .weak \handler
    .thumb_set \handler, Default_Handler
    .endm

/* ---------------------------------------------------------------------
 * 中断向量表（必须是镜像的第一个字节）
 * ------------------------------------------------------------------- */
    .global g_pfnVectors
    .section .isr_vector,"a",%progbits
    .type g_pfnVectors, %object

g_pfnVectors:
    .word _estack
    .word Reset_Handler
    VECTOR NMI_Handler
    VECTOR HardFault_Handler
    VECTOR MemManage_Handler
    VECTOR BusFault_Handler
    VECTOR UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    VECTOR SVC_Handler
    VECTOR DebugMon_Handler
    .word 0
    VECTOR PendSV_Handler
    VECTOR SysTick_Handler

    /* 外设中断 0..81 */
    VECTOR WWDG_IRQHandler
    VECTOR PVD_IRQHandler
    VECTOR TAMP_STAMP_IRQHandler
    VECTOR RTC_WKUP_IRQHandler
    VECTOR FLASH_IRQHandler
    VECTOR RCC_IRQHandler
    VECTOR EXTI0_IRQHandler
    VECTOR EXTI1_IRQHandler
    VECTOR EXTI2_IRQHandler
    VECTOR EXTI3_IRQHandler
    VECTOR EXTI4_IRQHandler
    VECTOR DMA1_Stream0_IRQHandler
    VECTOR DMA1_Stream1_IRQHandler
    VECTOR DMA1_Stream2_IRQHandler
    VECTOR DMA1_Stream3_IRQHandler
    VECTOR DMA1_Stream4_IRQHandler
    VECTOR DMA1_Stream5_IRQHandler
    VECTOR DMA1_Stream6_IRQHandler
    VECTOR ADC_IRQHandler
    VECTOR CAN1_TX_IRQHandler
    VECTOR CAN1_RX0_IRQHandler
    VECTOR CAN1_RX1_IRQHandler
    VECTOR CAN1_SCE_IRQHandler
    VECTOR EXTI9_5_IRQHandler
    VECTOR TIM1_BRK_TIM9_IRQHandler
    VECTOR TIM1_UP_TIM10_IRQHandler
    VECTOR TIM1_TRG_COM_TIM11_IRQHandler
    VECTOR TIM1_CC_IRQHandler
    VECTOR TIM2_IRQHandler
    VECTOR TIM3_IRQHandler
    VECTOR TIM4_IRQHandler
    VECTOR I2C1_EV_IRQHandler
    VECTOR I2C1_ER_IRQHandler
    VECTOR I2C2_EV_IRQHandler
    VECTOR I2C2_ER_IRQHandler
    VECTOR SPI1_IRQHandler
    VECTOR SPI2_IRQHandler
    VECTOR USART1_IRQHandler
    VECTOR USART2_IRQHandler
    VECTOR USART3_IRQHandler
    VECTOR EXTI15_10_IRQHandler
    VECTOR RTC_Alarm_IRQHandler
    VECTOR OTG_FS_WKUP_IRQHandler
    VECTOR TIM8_BRK_TIM12_IRQHandler
    VECTOR TIM8_UP_TIM13_IRQHandler
    VECTOR TIM8_TRG_COM_TIM14_IRQHandler
    VECTOR TIM8_CC_IRQHandler
    VECTOR DMA1_Stream7_IRQHandler
    VECTOR FSMC_IRQHandler
    VECTOR SDIO_IRQHandler
    VECTOR TIM5_IRQHandler
    VECTOR SPI3_IRQHandler
    VECTOR UART4_IRQHandler
    VECTOR UART5_IRQHandler
    VECTOR TIM6_DAC_IRQHandler
    VECTOR TIM7_IRQHandler
    VECTOR DMA2_Stream0_IRQHandler
    VECTOR DMA2_Stream1_IRQHandler
    VECTOR DMA2_Stream2_IRQHandler
    VECTOR DMA2_Stream3_IRQHandler
    VECTOR DMA2_Stream4_IRQHandler
    VECTOR ETH_IRQHandler
    VECTOR ETH_WKUP_IRQHandler
    VECTOR CAN2_TX_IRQHandler
    VECTOR CAN2_RX0_IRQHandler
    VECTOR CAN2_RX1_IRQHandler
    VECTOR CAN2_SCE_IRQHandler
    VECTOR OTG_FS_IRQHandler
    VECTOR DMA2_Stream5_IRQHandler
    VECTOR DMA2_Stream6_IRQHandler
    VECTOR DMA2_Stream7_IRQHandler
    VECTOR USART6_IRQHandler
    VECTOR I2C3_EV_IRQHandler
    VECTOR I2C3_ER_IRQHandler
    VECTOR OTG_HS_EP1_OUT_IRQHandler
    VECTOR OTG_HS_EP1_IN_IRQHandler
    VECTOR OTG_HS_WKUP_IRQHandler
    VECTOR OTG_HS_IRQHandler
    VECTOR DCMI_IRQHandler
    .word 0
    VECTOR HASH_RNG_IRQHandler
    VECTOR FPU_IRQHandler

g_pfnVectors_end:
    .size g_pfnVectors, g_pfnVectors_end - g_pfnVectors

/* ---------------------------------------------------------------------
 * 复位入口
 * ------------------------------------------------------------------- */
    .global Reset_Handler
    .section .text.Reset_Handler,"ax",%progbits
    .type Reset_Handler, %function
    .thumb_func
Reset_Handler:
    /* 中断向量表第 0 项已经给过初值，这里显式再设一次，便于单独复用 */
    ldr     sp, =_estack

    /* 使能 CP10/CP11（FPU）：硬浮点 ABI 下必须在任何浮点指令之前完成 */
    ldr     r0, =0xE000ED88          /* SCB->CPACR */
    ldr     r1, [r0]
    orr     r1, r1, #(0xF << 20)
    str     r1, [r0]
    dsb
    isb

    /* 向量表固定在主 Flash 起始处。复位后 VTOR 默认就是这个值，但若是经
     * bootloader / 调试器跳转进来，不显式设置就可能进错中断入口。 */
    ldr     r0, =0xE000ED08          /* SCB->VTOR */
    ldr     r1, =0x08000000
    str     r1, [r0]

    /* 从 Flash 复制 .data 到 RAM */
    ldr     r0, =_sidata
    ldr     r1, =_sdata
    ldr     r2, =_edata
    b       2f
1:
    ldr     r3, [r0], #4
    str     r3, [r1], #4
2:
    cmp     r1, r2
    bcc     1b

    /* 清零 .bss */
    ldr     r1, =_sbss
    ldr     r2, =_ebss
    movs    r3, #0
    b       4f
3:
    str     r3, [r1], #4
4:
    cmp     r1, r2
    bcc     3b

    bl      main

    /* main 不该返回；万一返回就停在这里，方便调试器看到 */
5:
    b       5b
    .size Reset_Handler, .-Reset_Handler

/* ---------------------------------------------------------------------
 * 未实现的中断统一入口
 * ------------------------------------------------------------------- */
    .section .text.Default_Handler,"ax",%progbits
    .type Default_Handler, %function
    .thumb_func
Default_Handler:
    b       Default_Handler
    .size Default_Handler, .-Default_Handler
