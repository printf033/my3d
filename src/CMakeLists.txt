# =============================================================================
# my3d 分层构建
# =============================================================================
#
# 依赖方向（见 docs/architecture.md §3）：单向、无环。
#
#     my3d_core ─┬─► my3d_backend_cpu ──► my3d_backend_ascii ─┐
#                ├─► my3d_platform_tty ───────────────────────┴─► viewer_ascii
#                └─► my3d_asset_import  (需要 assimp)
#
#     my3d_asset_import ─┬─► viewer_gpu
#     my3d_backend_filament ┤
#     my3d_platform_sdl ────┘
#
# 最关键的约束：**my3d_core / _backend_cpu / _backend_ascii / _platform_tty
# 一个第三方库都不链**。
#
# 这不是审美。它让「终端后端不依赖 Filament / SDL2 / Vulkan」成为构建期可查的
# 事实，而不是一句需要人记住的话 —— 依赖一旦被允许混进去，抽象会在几个月内
# 悄悄退化成「为了用某个后端才存在的接口」。验证方法见 samples/CMakeLists.txt。
#
# assimp / SDL2 / Filament 全部是 optional：缺任何一项时对应 target 不生成，
# 但终端路径仍然能 configure、能构建、能跑。系统包装不上不是终端路径的错。
# =============================================================================

# ---------------------------------------------------------------------------
# 公共编译选项：不含任何依赖，所有自建 target 都从它继承
# ---------------------------------------------------------------------------
add_library(my3d_flags INTERFACE)

target_compile_features(my3d_flags INTERFACE cxx_std_20)

target_compile_options(my3d_flags INTERFACE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -Wall -Wextra -Wpedantic
        -fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=>
)

# 消毒器按显式开关启用，不绑在 Debug 上（理由见根 CMakeLists.txt）。
if(MY3D_SANITIZE)
    target_compile_options(my3d_flags INTERFACE
        -fsanitize=${MY3D_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(my3d_flags INTERFACE -fsanitize=${MY3D_SANITIZE})
    message(STATUS "my3d: sanitizers enabled -> -fsanitize=${MY3D_SANITIZE}")
endif()

# ---------------------------------------------------------------------------
# my3d_log —— 新架构的日志（libs/mylog，git submodule）。
#
# mylog 是项目自有的 header-only 库，核心层零外部依赖 —— 所以挂上它不会动摇上面
# 那条判据留给终端路径的结论：`ldd build/viewer_ascii` 的输出必须一字不变。这句
# 话是可以当场验证的，不是承诺。
#
# 刻意**不** add_subdirectory(libs/mylog)：上游 src/CMakeLists.txt 里有
#     pkg_check_modules(LUAJIT REQUIRED IMPORTED_TARGET luajit)
# 而它不在任何 option 之后 —— 一旦 add_subdirectory，luajit 就成了本项目
# configure 期的硬依赖：没有 luajit 的机器连终端路径都配置不了，与「系统包装不
# 上不是终端路径的错」直接冲突。这里只取它的头文件目录，Lua 配置组件
# （configer_lua.hpp）因此不可用。这是有意的取舍，不是遗漏。
#
# 代价写明：我们消费的是上游的**头**，不是上游的 CMake 接口。上游若把 logger
# 从 header-only 改成需要单独编译的库，或者给核心层加新的宏/依赖，这里要跟着改。
#
# submodule 缺失时直接失败而不是静默降级：它是项目自带的资产，不是系统包。
# 「日志没生效」若变成一条查不出的谜题，比 configure 报错贵得多。
# ---------------------------------------------------------------------------
set(MY3D_MYLOG_DIR "${CMAKE_SOURCE_DIR}/libs/mylog")

if(NOT EXISTS "${MY3D_MYLOG_DIR}/src/logger.hpp")
    message(FATAL_ERROR
        "my3d: libs/mylog is not checked out.\n"
        "      Run: git submodule update --init libs/mylog")
endif()

# Threads 只为 mylog 的配置发布（std::mutex）而链。glibc 2.34+ 已把 pthread 并入
# libc，CMake 的 FindThreads 在这种情况下不产生额外链接项 —— 仍需实测确认。
find_package(Threads REQUIRED)

add_library(my3d_log INTERFACE)

target_include_directories(my3d_log INTERFACE ${MY3D_MYLOG_DIR}/src)

target_link_libraries(my3d_log INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# my3d_core —— 引擎概念层 + 后端无关的数据。零第三方依赖。
#
# 这里的东西是「所有后端都必须同意」的部分：场景图、相机数学、剔除/排序、
# 材质与网格的引擎侧表示。它不知道窗口、不知道 GPU、也不知道终端。
# ---------------------------------------------------------------------------
add_library(my3d_core STATIC
    scene/world.cpp
    render/toolbox.cpp
    render/camera_controller.cpp
    render/scene_builder.cpp
    render/animation_driver.cpp
    asset/asset_scene.cpp
    asset/animation.cpp
    asset/ibl.cpp
)

target_include_directories(my3d_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(my3d_core PUBLIC my3d_flags)

# ---------------------------------------------------------------------------
# my3d_backend_cpu —— 软件光栅化后端（离屏，无窗口）。
#
# 它同时是 ASCII 后端的复用来源：两者调用同一个 cpu::drawScene，区别只在
# 「拿到线性 RGB 之后干什么」。
# ---------------------------------------------------------------------------
add_library(my3d_backend_cpu STATIC
    backend/cpu/rasterizer.cpp
    backend/cpu/shading.cpp
    backend/cpu/scene_draw.cpp
    backend/cpu/resource_pool.cpp
    backend/cpu/cpu_renderer.cpp
)

target_link_libraries(my3d_backend_cpu PUBLIC my3d_core)

# ---------------------------------------------------------------------------
# my3d_backend_ascii —— 字符终端后端。零第三方依赖。
#
# 它不 include 任何 platform/、不写文件描述符：只把一帧要发往终端的字节组装进
# std::string。于是它可以在没有终端的进程里被 ASan/UBSan 直接单测。
# ---------------------------------------------------------------------------
add_library(my3d_backend_ascii STATIC
    backend/ascii/charset.cpp
    backend/ascii/ascii_renderer.cpp
)

target_link_libraries(my3d_backend_ascii PUBLIC my3d_backend_cpu)

# ---------------------------------------------------------------------------
# my3d_platform_tty —— 终端窗口（termios raw + TIOCGWINSZ + SIGWINCH）。零第三方。
#
# 只做终端状态与输入采集，产出语义按键；不含相机数学、不含渲染。
# ---------------------------------------------------------------------------
add_library(my3d_platform_tty STATIC
    platform/tty/tty_input.cpp
    platform/tty/tty_window.cpp
)

target_link_libraries(my3d_platform_tty PUBLIC my3d_core)

# ===========================================================================
# 第三方依赖：全部 optional
# ===========================================================================

find_package(assimp QUIET)
find_package(SDL2 QUIET)

# ---------------------------------------------------------------------------
# my3d_backend_filament —— 与 libs/filament 子模块版本匹配的官方 Linux SDK，
# 加上本项目的 GPU 后端实现。
#
# 为什么是 STATIC 而不是 INTERFACE：这里开始有真正属于本项目的编译单元
# （backend/filament/filament_renderer.cpp），它 include Filament 头、按 Filament
# 的 ABI 调用。INTERFACE 只能传 flags，传不了「我们自己的实现」——迁移前
# Filament 的调用散在 engine.hpp 里，所以那时 INTERFACE 是够的；现在的架构把
# 实现收进一个 .cpp，这一层就必须有实体。
#
# Filament 源码通过 git submodule 固定到主仓库记录的版本；官方 Linux 预编译
# SDK 从同版本 release 下载到 build/deps，不把平台二进制提交进仓库。
# 链接口径：子库全是 .a，按绝对路径链；`-L` 只为 `-lc++` 服务。
#
# ---------------------------------------------------------------- libc++
# Filament 的 prebuilt 是 libc++ 编的（`std::__1` 符号），而本项目其余部分
# （含 assimp）是 libstdc++。这两套不能互相满足对方的 std 符号，所以必须**同时**
# 链 libc++ 与 libstdc++ —— 各管各的引用，不是一个替掉另一个。
#
# 系统里通常没有 libc++（本项目实测：/usr/lib64 下无 libc++*，/usr/include/c++/v1
# 也不存在）。于是这里用 cache 变量指向上游带过来的那份 .so，找不到就退回
# 探测常见位置；再找不到就**关掉**这个 target 并说清楚原因，而不是留一个
# configure 能过、链接必炸的半成品。
# ---------------------------------------------------------------------------
set(MY3D_FILAMENT_VERSION "1.77.2" CACHE STRING "Pinned Filament release version")
set(MY3D_FILAMENT_SDK_ROOT
    "${CMAKE_BINARY_DIR}/deps/filament-v${MY3D_FILAMENT_VERSION}-linux/filament"
    CACHE PATH "Extracted official Filament Linux SDK root")
set(MY3D_FILAMENT_DIR "${MY3D_FILAMENT_SDK_ROOT}/lib/x86_64")

# 上游 libc++ 共享库。带 SONAME `libc++.so`，所以运行时也要能找到它 —— 下面的
# rpath 就是为这件事写的。设成 cache 变量是为了让机器相关的路径可以覆盖，
# 而不是硬编码进构建脚本。
set(MY3D_LIBCXX_SO "" CACHE FILEPATH
    "Path to the libc++ shared library that satisfies Filament's std::__1 symbols")

if(NOT MY3D_LIBCXX_SO)
    foreach(_cand IN ITEMS
            /usr/lib64/libc++.so
            /usr/lib/x86_64-linux-gnu/libc++.so
            /usr/lib/libc++.so)
        if(EXISTS "${_cand}")
            set(MY3D_LIBCXX_SO "${_cand}")
            break()
        endif()
    endforeach()
endif()

get_filename_component(MY3D_LIBCXX_DIR "${MY3D_LIBCXX_SO}" DIRECTORY)

# ---------------------------------------------------------------- libc++ 名字楔子
# 上游分发的文件名是 `libc++.so.1`，但它的 **SONAME 是 `libc++.so`**（实测
# readelf -d），而目录里既没有 `libc++.so` 文件也没有同名符号链接。
# 两个后果同时发生：
#   * 链接期 `-lc++` 找不到 —— ld 按 `lib<name>.so` 去找文件；
#   * 即使换成 `-l:libc++.so.1` 链上，DT_NEEDED 记录的仍是 SONAME
#     `libc++.so`，运行期 ld.so 照样找不到。
# 所以在**构建目录**里补一个名为 `libc++.so` 的符号链接，把链接期和运行期
# 指回同一个真实文件。
#
# 为什么不直接拷一份二进制进仓库：那是把一份 1.5 MB 的平台相关 .so 当成源码
# 提交，与「机器相关的路径走 cache 变量」这条口径矛盾。符号链接只活在这次
# 构建里，删掉构建目录就干净了。
#
# 为什么不用 patchelf 改 SONAME：那要引入一个新的工具依赖，而这里一个
# file(CREATE_LINK) 就够了。
set(MY3D_LIBCXX_SHIM_DIR "${CMAKE_BINARY_DIR}/libcxx-shim")
if(MY3D_LIBCXX_SO AND EXISTS "${MY3D_LIBCXX_SO}")
    file(MAKE_DIRECTORY "${MY3D_LIBCXX_SHIM_DIR}")
    if(NOT EXISTS "${MY3D_LIBCXX_SHIM_DIR}/libc++.so")
        file(CREATE_LINK "${MY3D_LIBCXX_SO}"
             "${MY3D_LIBCXX_SHIM_DIR}/libc++.so" SYMBOLIC)
    endif()
else()
    # 没有可用 libc++ 时把这个目录也留空：下面的 if 会因此关掉 filament target，
    # 不能让一个悬空的 -L 路径影响别的 target 的链接诊断。
    set(MY3D_LIBCXX_SHIM_DIR "")
endif()

set(MY3D_FILAMENT_LIBS
    filament filaflat filabridge shaders backend smol-v
    image ktxreader basis_transcoder utils zstd bluegl bluevk
)

if(EXISTS "${MY3D_FILAMENT_DIR}/libfilament.a" AND MY3D_LIBCXX_SO AND MY3D_LIBCXX_SHIM_DIR)

    add_library(my3d_backend_filament STATIC
        backend/filament/filament_renderer.cpp
    )

    target_include_directories(my3d_backend_filament PUBLIC
        ${MY3D_FILAMENT_SDK_ROOT}/include
    )

    # my3d_core 带来 src/ 与 C++20；Filament 头在 -Wpedantic 下会喷
    # `#pragma nounroll` 之类的告警，那不是我们的代码，就地静音。
    target_link_libraries(my3d_backend_filament PUBLIC my3d_core)
    target_compile_options(my3d_backend_filament PRIVATE -Wno-unknown-pragmas)

    # 顺序即语义：先 Filament 的 .a，再 libc++ —— 链接器从左往右解析未定义符号。
    foreach(_lib ${MY3D_FILAMENT_LIBS})
        target_link_libraries(my3d_backend_filament PUBLIC
            ${MY3D_FILAMENT_DIR}/lib${_lib}.a)
    endforeach()

    target_link_libraries(my3d_backend_filament PUBLIC
        -L${MY3D_LIBCXX_SHIM_DIR} -lc++
        # 显式保留 libstdc++：assimp 与本项目其余代码都是它的 ABI。
        $<$<CXX_COMPILER_ID:GNU>:-lstdc++>
    )

    # 运行时解析 `libc++.so`。两段 rpath：楔子目录（符号链接所在，第一次
    # 解析就命中）与上游原目录（万一将来上游补上了真的 libc++.so）。用 rpath
    # 而不是要求使用者设 LD_LIBRARY_PATH：「跑之前先 export 一件事」是最容易
    # 忘、也最难从错误信息里看出原因的一步。
    target_link_options(my3d_backend_filament INTERFACE
        "-Wl,-rpath,${MY3D_LIBCXX_SHIM_DIR}"
        "-Wl,-rpath,${MY3D_LIBCXX_DIR}")

    # 预编译二进制里 VulkanPlatformLinux 的 createVkSurfaceKHR 只有 X11 两条路
    # （实测 strings：VK_KHR_xlib_surface / VK_KHR_xcb_surface，**没有** wayland）。
    # 所以这里声明的是 X11 —— 它不是「我们打算用哪个后端」，而是对既有二进制的
    # 如实描述。窗口侧因此必须落在 X11 上，见 samples/viewer_gpu.cpp。
    target_compile_definitions(my3d_backend_filament INTERFACE
        $<$<AND:$<PLATFORM_ID:Linux>,$<NOT:$<PLATFORM_ID:Android>>>:VK_USE_PLATFORM_XCB_KHR>
        $<$<AND:$<PLATFORM_ID:Linux>,$<NOT:$<PLATFORM_ID:Android>>>:VK_USE_PLATFORM_XLIB_KHR>
    )

elseif(NOT EXISTS "${MY3D_FILAMENT_DIR}/libfilament.a")
    message(STATUS "my3d: Filament ${MY3D_FILAMENT_VERSION} SDK not found under "
                   "${MY3D_FILAMENT_SDK_ROOT} -- my3d_backend_filament disabled.\n"
                   "      Run: scripts/prepare_filament.sh")
else()
    message(STATUS "my3d: libc++ shared library not found -- my3d_backend_filament disabled.\n"
                   "      Filament prebuilts are built against libc++ and cannot link without it.\n"
                   "      Point -DMY3D_LIBCXX_SO=<path/to/libc++.so> at one to enable it.")
endif()

# ---------------------------------------------------------------------------
# my3d_platform_sdl —— SDL2 窗口后端。
#
# 与 my3d_platform_tty 平级：都实现 platform::IWindow，都只负责「开窗口 + 采输入」，
# 不碰任何渲染。差别只在底层是终端还是图形窗口。
#
# 这里**不**给 platform/sdl 链 my3d_log：窗口层不需要日志，日志接线只在 sample
# 一个落点（samples/ascii_demo_common.hpp），后端与平台层都不该出现第二处。
# ---------------------------------------------------------------------------
if(SDL2_FOUND)
    add_library(my3d_platform_sdl STATIC platform/sdl/sdl_window.cpp)

    target_link_libraries(my3d_platform_sdl PUBLIC my3d_core SDL2::SDL2)

    # SDL_syswm.h 在 2.32 里同时定义 X11 与 Wayland 两个驱动宏，所以编译期的
    # `#if` 判断不出「实际用的是哪个」——真正的分支必须落在运行时的
    # wmInfo.subsystem 上（见 sdl_window.cpp 的 resolveNativeHandle）。
    # 这里**刻意不再定义** SDL_VIDEO_DRIVER_*：SDL_config_unix.h 已经无条件定义了
    # 两者，再加一条只会让人误以为它起了作用。
    target_compile_options(my3d_platform_sdl PRIVATE -Wno-unknown-pragmas)
else()
    message(STATUS "my3d: SDL2 not found -- my3d_platform_sdl disabled")
endif()

# ---------------------------------------------------------------------------
# my3d_asset_import —— assimp + stb_image。**第三方与本项目的分界线**。
#
# src/asset/importer.cpp 是唯一 include <assimp/...> 与 <stb/stb_image.h> 的文件；
# 它的公开头 importer.hpp 两者都不 include，所以使用 AssetScene 的代码不必拖上
# assimp。把它单独成一个 target，是为了让这句话在链接图上成立，而不只是约定。
# ---------------------------------------------------------------------------
if(assimp_FOUND)

    add_library(my3d_assimp INTERFACE)
    target_link_libraries(my3d_assimp INTERFACE assimp::assimp)

    add_library(my3d_asset_import STATIC asset/importer.cpp)

    # 搜索根是 libs，**不是** libs/stb：代码写的是 <stb/stb_image.h>，所以根必须是
    # 它的父目录。用 libs/stb 会拼出 libs/stb/stb/stb_image.h —— 那是 HEAD 就有的
    # 断裂，只因 assimp 缺席、这段代码从未被编译而一直隐着。
    target_include_directories(my3d_asset_import PRIVATE ${CMAKE_SOURCE_DIR}/libs)

    # stb_image 是 header-only：实现必须恰好落在一个翻译单元里。这里是新架构
    # 的唯一落点，所以链 my3d_asset_import 的 target 不需要（也不许）再定义它。
    target_compile_definitions(my3d_asset_import PRIVATE STB_IMAGE_IMPLEMENTATION)

    target_link_libraries(my3d_asset_import PUBLIC my3d_core PRIVATE my3d_assimp)

else()
    message(STATUS "my3d: assimp not found -- my3d_asset_import disabled "
                   "(asset/importer.cpp is its only consumer)")
endif()
