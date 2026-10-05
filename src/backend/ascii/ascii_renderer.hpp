#pragma once

// my3d::backend::ascii —— 字符输出后端（ASCII / UTF-8 字形画到终端）。
//
// ===================== 它为什么和 CpuRenderer 是孪生兄弟 =====================
//
//   CpuRenderer::beginFrame  →  drawScene(...)  →  Framebuffer(线性 RGB)
//   AsciiRenderer::beginFrame →  drawScene(...)  →  Framebuffer(线性 RGB)
//                                                     ↓
//                                              亮度 + 颜色 → 字符 + ANSI 字节
//
// 两者调用的是**同一个** cpu::drawScene，用的是**同一份**剔除 / 排序 / 光栅化 /
// 着色代码。ASCII 后端没有自己的一套场景遍历 —— 那正是「逻辑复用同一套 GPU 显示
// 代码」的物理落点。区别只有两点：
//
//   * 光栅目标小得离谱（80x24 量级，不是 1920x1080）。所以字符格里的锯齿必须
//     用**超采样**（supersample）压掉，否则细长几何体在字符网格上会整格闪烁。
//   * 拿到线性 RGB 之后不编码成 8 位像素，而是「亮度 → 字形 + 颜色 → 转义序列」。
//
// ===================== 它为什么不写文件描述符 =====================
//
// 本类**不碰 fd、不 include 任何 platform/**。它只把「这一帧要发往终端的字节」
// 组装进一个 std::string（output()）。好处有三：
//
//   1. 可以在 /tmp 里被 ASan/UBSan 直接单测 —— 断言字节序列，而不是「盯着终端看」；
//   2. 平台层（TtyWindow / SDL / 离屏）各自决定怎么写、要不要写；
//   3. 「渲染」与「IO」的失败模式分开：渲染崩了和终端写不进是两件事。
//
// 光标隐藏、交替屏幕缓冲、raw mode 全部属于 platform/tty —— 那是终端状态，
// 不是渲染。本层输出的定位序列假定调用方已经进入交替屏幕缓冲。
//
// ===================== 性能模型（别搞反） =====================
//
// 瓶颈**不在光栅化**。80x24 网格即使 4x 超采样也只有 320x96 = 30720 个像素，
// 现代 CPU 上不足 1ms。瓶颈在终端 IO：一帧全量重绘是 80*24*(~20 字节) ≈ 38KB，
// 15fps 就是 570KB/s 灌进 pty，滚动、ssh 高延迟场景会立刻卡。
//
// 所以本后端**默认做差值刷新**：只输出与上一帧不同的字符格。静止画面几乎零字节，
// 转动相机时只有被改写的边缘在传。

#include "asset/image_data.hpp"
#include "backend/ascii/charset.hpp"
#include "backend/cpu/framebuffer.hpp"
#include "backend/cpu/resource_pool.hpp"
#include "backend/cpu/scene_draw.hpp"
#include "render/renderer.hpp"
#include "render/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace my3d::backend::ascii
{

// 盒式降采样：src 的尺寸应当是 dst 的 supersample 倍（每轴）。
//
// 平均在**线性空间**做 —— 先转 sRGB 再平均会让暗部被过度加权，细亮线变灰。
// 这个不变量写反了画面只会「略微发灰」，端到端跑起来根本看不出来；
// 所以它被做成自由函数而不是私有成员，以便脱离渲染器、用构造好的像素直接单测。
// src 尺寸不足时 dst 清为黑（宁可黑帧，不可越界读）。
void downsampleLinear(const cpu::Framebuffer &src, uint32_t supersample, cpu::Framebuffer &dst);

// ASCII 后端**自有**的配置。
//
// 刻意不塞进 render::RenderSettings：字符集、抖动、超采样、色深这些概念对
// Filament 后端毫无意义，把它们放进通用设置会让「通用设置」变成「某一种后端
// 的私货收容所」——那正是抽象开始腐化的地方（见 render/types.hpp 的注释）。
struct AsciiSettings
{
    // 字形表。默认 " .:-=+*#%@"。换 Charset::makeBlocks() 得到 Unicode 块状风格。
    Charset charset = Charset::makeDefault();

    // 量化 / 抖动 / 边缘增强。dither 默认开（Bayer 有序，静止画面不闪）。
    GlyphOptions glyph{};

    // 字符单元的物理宽高比 = 单元宽 / 单元高。
    //
    // 等宽终端字体里一个字符格通常「高约等于宽的 2 倍」，也就是这里的 0.5。
    // 这个值直接决定相机的 aspect：若拿字符列数/行数当宽高比（80/24 ≈ 3.33）
    // 喂给透视投影，画面会被横向拉长一倍多 —— 圆球变成横躺的橄榄。
    // 它属于「终端字体的物理属性」，所以是 ASCII 后端的设置，不是相机的。
    float cellAspectRatio = 0.5f;

    // 每字符格的采样倍数（边长）。1 = 关，2 = 4 倍采样。
    // 80x24 网格开 2 只是 160x48 像素，代价可忽略，但细几何体的稳定性提升明显。
    uint32_t supersample = 2;

    // true  → 24 位真彩 "ESC[38;2;R;G;Bm"
    // false → 256 色 "ESC[38;5;Nm"（xterm 6x6x6 色立方，16..231）
    //
    // 不暴露「色立方档位数」：xterm 的 256 色板是**固定**的 6x6x6，改档位数
    // 只会算出一个不属于该色板的索引。选项要么有意义，要么不存在。
    bool trueColor = true;
};

// 一个字符格的最终结果：画什么字形、什么颜色。
struct AsciiCell
{
    char32_t codePoint = U' ';
    uint8_t level = 0;   // 在字符集中的亮度层级（边缘格也保留）
    bool isEdge = false;
    uint8_t r = 0;       // 前景色，sRGB8，终端实际收到的字节
    uint8_t g = 0;
    uint8_t b = 0;
};

// 一帧的 ASCII 侧统计，供测试断言与 HUD 显示。
struct AsciiStats
{
    uint32_t gridWidth = 0;
    uint32_t gridHeight = 0;
    uint32_t cellsChanged = 0;   // 本帧实际输出（重传）的字符格数
    size_t bytes = 0;            // 本帧 output() 的字节数
    uint32_t glyphs = 0;         // 非空格字符格数
    uint32_t edgeCells = 0;      // 被判定为轮廓的字符格数
    float luminanceMin = 0.0f;
    float luminanceMax = 0.0f;
};

class AsciiRenderer final : public render::IRenderer, public cpu::CpuResourcePool
{
public:
    // ---------------- IRenderer ----------------
    //
    // 注意 resize(width, height) 的单位是**字符列数 / 行数**，不是像素。
    // 网格尺寸为 0 时 beginFrame 返回 false（明确失败，不猜一个默认值）。
    bool beginFrame(const render::FrameInfo &info) override;
    void endFrame() override;
    bool resize(uint32_t width, uint32_t height) override;
    const char *name() const noexcept override;
    bool readbackImage(asset::ImageData &out) override;

    // ---------------- 配置 ----------------
    AsciiSettings &settings() noexcept { return settings_; }
    const AsciiSettings &settings() const noexcept { return settings_; }

    uint32_t gridWidth() const noexcept { return gridWidth_; }
    uint32_t gridHeight() const noexcept { return gridHeight_; }

    // ---------------- 结果访问 ----------------
    const std::vector<AsciiCell> &cells() const noexcept { return cells_; }
    const std::vector<float> &luminanceGrid() const noexcept { return luminance_; }

    // endFrame() 之后有效：本帧要写往终端的字节。
    // 为空表示画面与上一帧完全一致（差值刷新的正常结果，不是错误）。
    const std::string &output() const noexcept { return output_; }

    // 纯字符网格：每行 gridWidth 个 UTF-8 字符，行尾 '\n'，不含任何转义序列。
    // 用途：golden 文本回归（字符画面比像素画面更能定位 ASCII 特有 bug）、
    // 把一帧存成 .txt 直接看、调试边缘增强的开关效果。
    void readbackText(std::string &out) const;

    const cpu::RasterStats &rasterStats() const noexcept { return rasterStats_; }
    const cpu::SceneDrawStats &sceneStats() const noexcept { return sceneStats_; }
    const AsciiStats &stats() const noexcept { return stats_; }
    double lastFrameSeconds() const noexcept { return lastFrameSeconds_; }

private:
    // pixelBuffer_(grid*ss) → gridBuffer_(grid)：在线性空间做盒式降采样。
    void downsample(uint32_t supersample);
    // gridBuffer_ 的亮度图 + 颜色 → cells_。
    void buildCells();
    // cells_ 与 prevCells_ 的差 → output_。
    void buildOutput();

    AsciiSettings settings_;

    uint32_t gridWidth_ = 0;
    uint32_t gridHeight_ = 0;

    cpu::Framebuffer pixelBuffer_;   // 超采样后的线性 RGB（grid*ss 尺寸）
    cpu::Framebuffer gridBuffer_;    // 降采样后的线性 RGB（grid 尺寸）
    std::vector<float> luminance_;   // 行优先，长度 gridW*gridH
    std::vector<AsciiCell> cells_;
    std::vector<AsciiCell> prevCells_;
    std::string output_;

    // 下一帧必须整帧重发（首次绘制 / 网格尺寸变化 / 手动失效）。
    bool fullRedraw_ = true;

    cpu::RasterStats rasterStats_;
    cpu::SceneDrawStats sceneStats_;
    AsciiStats stats_;
    std::vector<render::DrawItem> drawItems_;
    double lastFrameSeconds_ = 0.0;
};

} // namespace my3d::backend::ascii
