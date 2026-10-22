cmake_minimum_required(VERSION 3.16)

project(modbus
        VERSION 1.0.0
        DESCRIPTION "可移植的 Modbus RTU 主/从站库"
        LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MODBUS_WERROR "把编译告警当作错误" ON)

# 库与测试共用的一套告警选项。
set(MODBUS_WARNINGS)
if(MSVC)
    list(APPEND MODBUS_WARNINGS /W4)
    if(MODBUS_WERROR)
        list(APPEND MODBUS_WARNINGS /WX)
    endif()
else()
    list(APPEND MODBUS_WARNINGS -Wall -Wextra -Wpedantic)
    if(MODBUS_WERROR)
        list(APPEND MODBUS_WARNINGS -Werror)
    endif()
endif()

add_library(modbus STATIC
    src/error/modbus_error.c
    src/core/crc.c
    src/core/map.c
    src/core/frame.c
    src/core/func.c
    src/engine/engine.c
    src/engine/master.c
    src/port/memory.c
)

# 公开头文件都放在 include/modbus 下，并且彼此以 "modbus.h" 这种同目录形式
# 互相引用，所以第一个搜索路径必须是 include/modbus。同时把 include 也暴露
# 出去，方便调用方写 #include <modbus/modbus.h>；src 是私有核心层的搜索路径。
target_include_directories(modbus
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include/modbus
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(modbus PRIVATE ${MODBUS_WARNINGS})

# ---------------------------------------------------------------------------
# 测试：不需要硬件，也不需要网络。ctest 直接跑 tests/test_modbus.c，
# 靠 mb_memory_t 把主站与从站接在一起做回环，另有 core 层的纯单元测试。
# ---------------------------------------------------------------------------

include(CTest)

if(BUILD_TESTING)
    add_executable(modbus_tests tests/test_modbus.c)

    target_link_libraries(modbus_tests PRIVATE modbus)
    # core 层（src/core、src/engine）是私有头，测试要直接调用它们，
    # 所以把 src 也加进来；对外 API 仍走 modbus 目标的公共包含目录。
    target_include_directories(modbus_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(modbus_tests PRIVATE ${MODBUS_WARNINGS})

    add_test(NAME modbus COMMAND modbus_tests)
endif()
