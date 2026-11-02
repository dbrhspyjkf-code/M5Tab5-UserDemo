cmake_minimum_required(VERSION 3.10)
project(MC_APP_BOILERPLATE VERSION 0.1.0 LANGUAGES C CXX)

if(TAB5_PLATFORM STREQUAL "web")
    # Emscripten/WASM 构建: emcmake cmake -DTAB5_PLATFORM=web -B build/web
    include(platforms/web/CMakeLists.txt)
else()
    include(platforms/desktop/CMakeLists.txt)
endif()
