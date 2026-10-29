# libssh 0.11.5 client-only (mbedtls 后端), 源自 ewpa/LibSSH-ESP32 v5.10 的
# src/ 树: 所有 .c 直接 #include "libssh_esp32_config.h"(即上游 config.h 的
# ESP32 移植版)。相比原打包: 删 server 侧(bind/server/messages/noop 线程)
# 与 Arduino 包装层, WITH_SERVER 关闭。许可: LGPL-2.1(见 COPYING) + BSD。
file(GLOB libssh_CLIENT_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
file(GLOB libssh_EXTERNAL_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/external/*.c")

idf_component_register(
    SRCS "src/threads.c" "src/threads/mbedtls.c" "src/threads/noop.c"
         ${libssh_CLIENT_SRCS} ${libssh_EXTERNAL_SRCS}
    INCLUDE_DIRS "src"
    PRIV_REQUIRES mbedtls lwip
)

# ESP32=1: ewpa/Arduino 构建全局定义了它, socket.c 用它编掉 proxy 子进程
# 分支(waitpid 等)。纯 IDF 构建没有, 此处补上保持行为一致
target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP32=1)

# libssh 0.11 对 mbedtls 3.x 弃用 API 的兼容层在运行路径上会触发警告,
# 属上游已知项, 不当错误处理
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=deprecated-declarations -Wno-error=format
    -Wno-error=char-subscripts
    -Wno-sign-compare -Wno-unused-variable)
