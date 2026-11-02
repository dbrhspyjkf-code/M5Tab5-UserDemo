# Web (Emscripten/WASM) platform — included from the repository root CMakeLists
# when configuring with:  emcmake cmake -DTAB5_PLATFORM=web -B build/web
#
# 与 desktop 平台同构: app/ 业务层全量编译, 平台层提供 HalWeb + LVGL 显示驱动。
# 复用 platforms/desktop/compat/ 下的 xiaozhi/led_strip 等 stub。
# 定义 PLATFORM_BUILD_DESKTOP: 继承桌面版已有的 123 处设备裁剪 (rfid/lora/ssh/xiaozhi 等);
# 额外定义 PLATFORM_BUILD_WEB: web 特有差异 (音频流/裁剪 app/字体) 单独判断。

cmake_policy(SET CMP0077 NEW)
set(CMAKE_CXX_STANDARD 17)

add_definitions(-DPLATFORM_BUILD_DESKTOP)
add_definitions(-DPLATFORM_BUILD_WEB)

set(TAB5_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(TAB5_DEPENDENCIES_DIR "${TAB5_PROJECT_ROOT}/dependencies")

# Mooncake
set(MOONCAKE_BUILD_EXAMPLE OFF)
add_subdirectory("${TAB5_DEPENDENCIES_DIR}/mooncake" ${CMAKE_BINARY_DIR}/mooncake)

set(MOONCAKE_EVENT_BUILD_EXAMPLE OFF)
add_subdirectory("${TAB5_DEPENDENCIES_DIR}/mooncake_log" ${CMAKE_BINARY_DIR}/mooncake_log)

set(SMOOTH_UI_TOOLKIT_BUILD_EXAMPLE OFF)
add_subdirectory("${TAB5_DEPENDENCIES_DIR}/smooth_ui_toolkit" ${CMAKE_BINARY_DIR}/smooth_ui_toolkit)

# LVGL — web 专属 lv_conf (LV_USE_SDL=0), 由 platforms/web/lv_conf.h 维护
set(LV_CONF_INCLUDE_SIMPLE OFF)
set(LV_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h")
add_subdirectory("${TAB5_DEPENDENCIES_DIR}/lvgl" ${CMAKE_BINARY_DIR}/lvgl)

# nlohmann json (header only)
include_directories("${TAB5_DEPENDENCIES_DIR}")

# App layer (同 desktop glob)
file(GLOB_RECURSE APP_LAYER_SRCS
    "${TAB5_PROJECT_ROOT}/app/*.c"
    "${TAB5_PROJECT_ROOT}/app/*.cc"
    "${TAB5_PROJECT_ROOT}/app/*.cpp"
)
list(APPEND APP_LAYER_SRCS
    "${TAB5_PROJECT_ROOT}/platforms/tab5/managed_components/78__xiaozhi-fonts/src/font_puhui_20_4.c"
)
set(APP_LAYER_INCS
    "${TAB5_PROJECT_ROOT}/app/"
    "${TAB5_PROJECT_ROOT}/platforms/tab5/components/xiaozhi_core/xiaozhi-esp32/main/"
    "${TAB5_PROJECT_ROOT}/platforms/desktop/compat/"
    "${TAB5_PROJECT_ROOT}/platforms/desktop/hal/"
)

# Web 平台源码 + 桌面 compat stubs (xiaozhi_ctl_stub 等)
file(GLOB_RECURSE WEB_PLATFORM_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/*.cc"
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
)
list(FILTER WEB_PLATFORM_SRCS EXCLUDE REGEX "build.*/.*")
file(GLOB COMPAT_SRCS "${TAB5_PROJECT_ROOT}/platforms/desktop/compat/*.cpp")

add_executable(tab5web ${WEB_PLATFORM_SRCS} ${COMPAT_SRCS} ${APP_LAYER_SRCS})
target_include_directories(tab5web PUBLIC ${APP_LAYER_INCS})
target_compile_options(tab5web PRIVATE -fexceptions)
set_target_properties(tab5web PROPERTIES OUTPUT_NAME "tab5web" SUFFIX ".js")
target_link_options(tab5web PRIVATE
    -fexceptions
    -pthread
    -sPROXY_TO_PTHREAD
    "-sPTHREAD_POOL_SIZE=8"
    "-sINITIAL_MEMORY=134217728"       # 128MB
    "-sMAXIMUM_MEMORY=2147483648"      # 2GB
    -sALLOW_MEMORY_GROWTH=1
    "-sEXPORTED_FUNCTIONS=_malloc,_free"
    "-sENVIRONMENT=web,worker"
    "-sEXIT_RUNTIME=0"
)
target_link_libraries(tab5web PUBLIC
    mooncake
    mooncake_log
    lvgl
    lvgl_examples
    lvgl_demos
    smooth_ui_toolkit
)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
