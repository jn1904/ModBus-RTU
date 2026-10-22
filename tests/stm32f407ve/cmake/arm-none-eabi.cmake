# arm-none-eabi.cmake —— STM32F407VE 交叉编译工具链
# 用法：cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake

set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  arm)

# 只编译、不链接的探测方式（裸机没有可用的默认链接环境）
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER    arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER  arm-none-eabi-gcc)
set(CMAKE_OBJCOPY       arm-none-eabi-objcopy)
set(CMAKE_OBJDUMP       arm-none-eabi-objdump)
set(CMAKE_SIZE          arm-none-eabi-size)

# Cortex-M4F / 单精度 FPU / 硬浮点 ABI
set(MCU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

set(CMAKE_C_FLAGS_INIT          "${MCU_FLAGS} -ffunction-sections -fdata-sections -fno-common")
set(CMAKE_ASM_FLAGS_INIT        "${MCU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} -Wl,--gc-sections -nostartfiles")

# 交叉环境下不找宿主机的头文件/库
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
