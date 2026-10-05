cmake_minimum_required(VERSION 3.16)

project(my3d CXX)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)

# 交付 / 日常构建默认 RelWithDebInfo（有优化、有符号）。
#
# 这里原来是硬写 Debug 并把 ASan 绑在 Debug 上 —— 两个后果都不好：不管
# 你怎么配，产物永远是没优化的；而消毒器会改变内存布局与耗时，绑死在配置上
# 意味着「想看真实性能」和「想要消毒器」不能同时成立。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# 消毒器按需显式开启：cmake -B build -DMY3D_SANITIZE=address,undefined
set(MY3D_SANITIZE "" CACHE STRING
    "Sanitizer list passed to -fsanitize (e.g. address,undefined); empty = off")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)

add_subdirectory(src)

add_subdirectory(samples)
