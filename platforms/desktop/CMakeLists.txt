cmake_minimum_required (VERSION 3.10)
project(DesktopBuild)

cmake_policy(SET CMP0077 NEW)
set(CMAKE_CXX_STANDARD 17)

add_definitions(-DPLATFORM_BUILD_DESKTOP)

# Mooncake
set(MOONCAKE_BUILD_EXAMPLE OFF)
add_subdirectory(../../dependencies/mooncake ${CMAKE_BINARY_DIR}/mooncake)

# Mooncake log
set(MOONCAKE_EVENT_BUILD_EXAMPLE OFF)
add_subdirectory(../../dependencies/mooncake_log ${CMAKE_BINARY_DIR}/mooncake_log)

# Smooth ui shit
set(SMOOTH_UI_TOOLKIT_BUILD_EXAMPLE OFF)
add_subdirectory(../../dependencies/smooth_ui_toolkit ${CMAKE_BINARY_DIR}/smooth_ui_toolkit)

# Lvgl
set(LV_CONF_INCLUDE_SIMPLE OFF)
set(LV_CONF_PATH ../../lv_conf.h)
add_subdirectory(../../dependencies/lvgl ${CMAKE_BINARY_DIR}/lvgl)

# SDL
find_package(SDL2 REQUIRED SDL2)
include_directories(PUBLIC ${SDL2_INCLUDE_DIRS})
target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})

# CURL for HA HTTP client
find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIRS})

# libjpeg-turbo for desktop wallpaper decode (mirrors P4 HW JPEG decoder).
# Homebrew keeps it keg-only, so point at its prefix explicitly if present.
find_package(JPEG REQUIRED)
include_directories(${JPEG_INCLUDE_DIRS})

# nlohmann json (header only) — include parent so #include <nlohmann/json.hpp> resolves
include_directories(../../dependencies)

# App layer
file(GLOB_RECURSE APP_LAYER_SRCS
    ../../app/*.c
    ../../app/*.cc
    ../../app/*.cpp
)
list(APPEND APP_LAYER_SRCS
    ../../platforms/tab5/managed_components/78__xiaozhi-fonts/src/font_puhui_20_4.c
)
set(APP_LAYER_INCS
    ../../app/
    compat/
)

# 桌面端源文件 (exclude any build/ subtree to avoid picking up CMake's own
# CompilerId test sources, which collide on link)
file(GLOB_RECURSE APP_DESKTOP_BUILD_SRCS
    *.cpp
    *.cc
    *.c
)
list(FILTER APP_DESKTOP_BUILD_SRCS EXCLUDE REGEX "build/.*")
add_executable(app_desktop_build ${APP_DESKTOP_BUILD_SRCS} ${APP_LAYER_SRCS})
target_include_directories(app_desktop_build PUBLIC ${APP_LAYER_INCS})
target_link_libraries(app_desktop_build PUBLIC
    mooncake
    mooncake_log
    lvgl
    lvgl_examples
    lvgl_demos
    ${SDL2_LIBRARIES}
    ${CURL_LIBRARIES}
    ${JPEG_LIBRARIES}
    smooth_ui_toolkit
    pthread
)

# 设置构建路径
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/build/desktop)

# 确保构建路径存在
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR})

# 设置生成的可执行文件输出路径
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})

# # 设置生成的库文件输出路径
# set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})
