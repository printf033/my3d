# my3d

一个多后端实验性渲染引擎：同一套场景层，分别驱动 **Filament/Vulkan** 与
**终端字符（ASCII）** 两个后端。设计文档见 `docs/architecture.md`。

## 构建

```bash
git submodule update --init --recursive   # 取回 Filament 源码与日志库
scripts/prepare_filament.sh               # 下载匹配版本的官方 Linux 预编译 SDK
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
```

依赖全部可选，缺谁就少一个 target，终端路径不受影响：

- `assimp` —— 模型导入（缺则无 `viewer_asset` / `viewer_gpu`）
- `SDL2` —— 图形窗口（缺则无 `viewer_gpu`）
- Filament `v1.77.2`（源码由 `libs/filament` 子模块固定；Linux SDK 解压到
  `build/deps`，缺少 libc++ 时用 `-DMY3D_LIBCXX_SO=/path/to/libc++.so` 指定）
- `libs/mylog` submodule（缺则任何 target 都不编译）

升级 Filament 时，在主仓库把 `libs/filament` 子模块 checkout 到目标 release
tag，并提交新的子模块指针；同时更新 `src/CMakeLists.txt` 的
`MY3D_FILAMENT_VERSION` 和 `scripts/prepare_filament.sh` 中对应版本的 SHA-256，
再运行准备脚本。主仓库提交记录固定子模块 commit 和 SDK release，避免两者漂移。

消毒器按需开：`cmake -S . -B build-asan -DMY3D_SANITIZE=address,undefined`

## 三个示例

```bash
./build/viewer_ascii                      # 程序化立方体 + 地面，纯终端
./build/viewer_asset                      # 真实模型 → 终端
./build/viewer_asset assets/model/car/FINAL_MODEL_B.fbx
./build/viewer_asset --animation "mixamo.com" assets/model/mia/scene.gltf
./build/viewer_gpu   assets/model/car/FINAL_MODEL_B.fbx   # Vulkan 窗口
./build/viewer_gpu --animation "mixamo.com" assets/model/mia/scene.gltf
./build/viewer_gpu --unlit --animation "mixamo.com" assets/model/mia/scene.gltf
```

`viewer_asset` / `viewer_gpu` 的 `--animation <名称>` 可选择模型中的动画片段；
省略时默认播放第一条。

省略模型参数时（`viewer_asset` / `viewer_gpu` 不带参数），默认模型按**仓库根**
解析，不依赖当前工作目录 —— `cd build && ./viewer_asset` 和从仓库根运行等价。
命令行显式给出的路径仍按 cwd 解析。

两个 viewer 的键盘操作统一：`W/S` 前后移动、`A/D` 左右平移、`Q/E` 降/升、
`H/J/K/L` 转向，`F` 线框、`C` 剔除，`Esc` 退出。
方向键、空格、Ctrl 和 `G` 不再作为额外控制键；
GPU 窗口额外支持鼠标转向与滚轮缩放视野。

## 分层

```
core ─┬─► backend_cpu ──► backend_ascii ─┐
      ├─► platform_tty ─────────────────┴─► viewer_ascii / viewer_asset
      └─► asset_import (assimp)

backend_filament ─┐
platform_sdl ─────┴─► viewer_gpu
```

硬性约束：`core` / `backend_cpu` / `backend_ascii` / `platform_tty`
**一个第三方库都不链**。这条可以当场验：

```bash
ldd build/viewer_ascii                            # 只有 libstdc++/libm/libgcc_s/libc
nm -C build/viewer_ascii | grep -c 'filament::'   # 0
```
