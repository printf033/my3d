# =============================================================================
# 示例程序 —— 三个 viewer，各证一件事
# =============================================================================

# ---------------------------------------------------------------------------
# viewer_ascii —— 场景 API + 字符终端后端。
#
# 这个 target 的依赖清单就是「API 停留在引擎概念层」这句话的构建期证据：
#
#     viewer_ascii → my3d_backend_ascii → my3d_backend_cpu → my3d_core
#                  → my3d_platform_tty  → my3d_core
#                  → my3d_log           (header-only，零外部依赖)
#
# 多出的 my3d_log 不列在上面那三条判据里：mylog 是项目自有的 submodule，它的
# 头文件不 include 任何第三方头，所以「无 Filament / SDL2 / assimp」依旧成立 ——
# 下面的 ldd / nm 检查就是用来确认这一点的，不是用来假设的。
#
# 没有 Filament、没有 SDL2、没有 Vulkan、没有 assimp。用它链接失败/成功来验证
# 解耦，比读文档可靠：
#
#     cmake --build build --target viewer_ascii --verbose 2>&1 | grep -E 'libfilament|SDL2'
#     ldd build/viewer_ascii
#     nm -C build/viewer_ascii | grep -c 'filament::'
#
# 三条期望输出分别是：无输出、无 libSDL2/vulkan、0。
# ---------------------------------------------------------------------------
add_executable(viewer_ascii viewer_ascii.cpp)

target_link_libraries(viewer_ascii PRIVATE
    my3d_backend_ascii
    my3d_platform_tty
    my3d_log
)

# ---------------------------------------------------------------------------
# viewer_asset —— 新架构：真实模型 → 场景装配 → 字符终端后端。
#
# 与 viewer_ascii 的差别只有一处：场景从哪来。程序化几何 vs 资产导入。
# 链接清单上因此多出 my3d_asset_import（assimp + stb）。
#
# 为什么必须是**独立**可执行文件，而不是给 viewer_ascii 加个参数：
#
#     viewer_ascii 的验收判据是 `ldd` 只有 libstdc++/libgcc_s/libm/libc。
#     assimp 一旦链进来，这条判据在构建期就静默失效了 —— 而它正是
#     「引擎 API 不拖第三方」这句话唯一的可查证据。
#
# 两个 target 各证一件事，谁也不会把谁的证据搞脏：
#
#     viewer_ascii → 引擎概念层的 API 面不含任何第三方
#     viewer_asset → 第三方只在 asset/importer.cpp 一个落点
#
# 注意两边的场景装配走的是同一份代码：render/scene_builder.cpp 属于 my3d_core，
# 它 include 的 asset 头（asset_scene / material / mesh_data）都不含 assimp。
# 所以「第三方只在导入器一个落点」不止是目录结构，是能被编译器验证的事实。
#
# 不定义 STB_IMAGE_IMPLEMENTATION：实现已经落在 my3d_asset_import 的
# asset/importer.cpp 里。再定义一次会在链接期撞出重复符号。
#
# MY3D_ASSET_ROOT 注入仓库根：默认模型路径以它为根，运行时 cwd 不保证是仓库根
# （最常见的就是 `cd build && ./viewer_asset`）。和 viewer_gpu 同一个理由、同一
# 个宏。
# ---------------------------------------------------------------------------
if(TARGET my3d_asset_import)

    add_executable(viewer_asset viewer_asset.cpp)

    target_compile_definitions(viewer_asset PRIVATE
        MY3D_ASSET_ROOT="${CMAKE_SOURCE_DIR}"
    )

    target_link_libraries(viewer_asset PRIVATE
        my3d_asset_import
        my3d_backend_ascii
        my3d_platform_tty
        my3d_log
    )

else()
    message(STATUS "my3d: skipping 'viewer_asset' sample "
                   "(needs assimp; viewer_ascii unaffected)")
endif()

# ---------------------------------------------------------------------------
# viewer_gpu —— 新架构：真实模型 → 场景装配 → **Filament (Vulkan)** 后端。
#
# 这是三个 viewer 里唯一走 GPU 的。它和 viewer_asset 共享的是**同一份场景装配
# 代码**（render/scene_builder.cpp 属于 my3d_core），差只差最后一步把 World
# 交给谁 —— 这就是「引擎 API 停留在引擎概念层」最有说服力的实证：换后端不改
# 上游，连 include 都不用动。
#
# 单独一个 target 而不是给 viewer_asset 加开关，理由和 viewer_ascii / viewer_asset
# 分开的理由一样：viewer_ascii 的验收判据是「ldd 里没有第三方」，Filament 的
# prebuilt 一旦链进来那条判据就废了。三者各证一件事。
#
# MY3D_ASSET_ROOT 注入仓库根：filament_renderer 要按相对路径找
# assets/shader/*.filamat，而运行时 cwd 不保证是仓库根。编译期钉死比运行时猜可靠。
#
# 这个 target 需要 libc++ 共享库（Filament prebuilt 是 libc++ 编的），所以它
# 跟着 my3d_backend_filament 的可用性一起出现/消失 —— 见 src/CMakeLists.txt。
# ---------------------------------------------------------------------------
if(TARGET my3d_backend_filament AND TARGET my3d_platform_sdl AND TARGET my3d_asset_import)

    add_executable(viewer_gpu viewer_gpu.cpp)

    target_compile_definitions(viewer_gpu PRIVATE
        MY3D_ASSET_ROOT="${CMAKE_SOURCE_DIR}"
    )

    target_link_libraries(viewer_gpu PRIVATE
        my3d_asset_import
        my3d_backend_filament
        my3d_platform_sdl
        my3d_log
    )

else()
    message(STATUS "my3d: skipping 'viewer_gpu' sample "
                   "(needs filament prebuilts + libc++; viewers above unaffected)")
endif()
