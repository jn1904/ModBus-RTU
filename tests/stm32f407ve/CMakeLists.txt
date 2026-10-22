cmake_minimum_required(VERSION 3.20)

# STM32F407VE 上的自测固件。这是一个**独立工程**：必须用 arm-none-eabi
# 工具链构建，不能与宿主机的根工程共用同一个 build 目录（一个 build 树
# 只能绑定一套工具链）。
#
#   cmake -S tests/stm32f407ve -B build-stm32f407ve \
#         -DCMAKE_TOOLCHAIN_FILE=tests/stm32f407ve/cmake/arm-none-eabi.cmake
#   cmake --build build-stm32f407ve
#
# 产物是 .elf/.bin/.hex，要在板子上跑，不能 ctest。

project(modbus_stm32f407ve LANGUAGES C ASM)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "本工程必须交叉编译，请用 -DCMAKE_TOOLCHAIN_FILE 指定 tests/stm32f407ve/cmake/arm-none-eabi.cmake")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# 默认 Release：不给 build type 的话库源码是 -O0，而上板跑的代码应该和
# 实际发布一致；--gc-sections 的裁剪结果也才可信。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# 库源码在仓库根部，这里当作子目录引入，避免再维护一份源文件清单。
# BUILD_TESTING 关掉，否则根工程会把宿主机用的测试目标也拉进来。
get_filename_component(MODBUS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${MODBUS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/modbus")

get_filename_component(LINKER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/linker/stm32f407ve.ld" ABSOLUTE)

add_executable(modbus_stm32f407ve_test
    startup/startup_stm32f407xx.s
    src/board.c
    src/main.c
)

target_include_directories(modbus_stm32f407ve_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${MODBUS_ROOT}/src"        # core 层私有头：自测要直接调用协议函数
)

target_link_libraries(modbus_stm32f407ve_test PRIVATE modbus)

target_compile_options(modbus_stm32f407ve_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -g3
)

target_link_options(modbus_stm32f407ve_test PRIVATE
    "-T${LINKER_SCRIPT}"
    -Wl,-Map=modbus_stm32f407ve_test.map
    -Wl,--print-memory-usage
    --specs=nano.specs
    --specs=nosys.specs
)

set_target_properties(modbus_stm32f407ve_test PROPERTIES
    SUFFIX ".elf"
    OUTPUT_NAME "modbus_stm32f407ve_test"
)

# 烧录镜像与体积报告
add_custom_command(TARGET modbus_stm32f407ve_test POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary
            $<TARGET_FILE:modbus_stm32f407ve_test>
            $<TARGET_FILE_DIR:modbus_stm32f407ve_test>/modbus_stm32f407ve_test.bin
    COMMAND "${CMAKE_OBJCOPY}" -O ihex
            $<TARGET_FILE:modbus_stm32f407ve_test>
            $<TARGET_FILE_DIR:modbus_stm32f407ve_test>/modbus_stm32f407ve_test.hex
    COMMAND "${CMAKE_SIZE}" -A -x $<TARGET_FILE:modbus_stm32f407ve_test>
    COMMENT "生成 .bin / .hex 并报告体积"
)

# openocd 烧录（可选，需要接 ST-Link）
find_program(OPENOCD_EXECUTABLE openocd)
if(OPENOCD_EXECUTABLE)
    add_custom_target(flash
        COMMAND "${OPENOCD_EXECUTABLE}"
                -f interface/stlink.cfg -f target/stm32f4x.cfg
                -c "program $<TARGET_FILE:modbus_stm32f407ve_test> verify reset exit"
        DEPENDS modbus_stm32f407ve_test
        COMMENT "通过 ST-Link 烧录并校验（USART1 115200 8N1）"
        USES_TERMINAL
    )
else()
    message(STATUS "未找到 openocd：跳过 flash 目标")
endif()
