#pragma once

// my3d::backend::ascii —— 「亮度 → 字符」的映射规则（纯函数，无状态、无输出）。
//
// 为什么这一层单独存在，而不是直接写在 ascii_renderer 里：
//   * 规则必须只有一份。字符集、量化、抖动、边缘方向这些决定画面观感的东西，
//     渲染器、将来的离线导出（PNG/sixel）和单元测试都要用同一套；塞进渲染器
//     就会跟着终端 IO 一起进「不可测的渲染循环」。
//   * 终端输出（ANSI 转义、差值刷新、光标控制）不在这里。本文件不认识
//     「终端」这个概念，只吃一张行优先的 float 亮度图，吐字符。
//   * 依赖边界：只允许 C++ 标准库 + core/math.hpp。不许 include
//     backend/cpu/、render/、scene/、asset/ —— 一旦引入，这层就不再是纯函数，
//     也没法在 /tmp 里用 ASan/UBSan 单独跑（见 docs/architecture.md §6.3）。
//
// 数值约定（改动前先看 docs/architecture.md §6.3）：
//   * luminance 是行优先（row-major）的 float 数组，长度 w*h，语义范围 [0,1]。
//     实际会因 HDR / 曝光越界，所以本层每个入口都 clamp，不假设调用方擦干净了。
//   * 覆盖率表可以整体替换（见 Charset::coverage），本文件只提供手工估计的默认表。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace my3d::backend::ascii
{

// 字符集：按从暗到亮排列的字形，外加每个字形的墨迹覆盖率。
//
// coverage 是本模块唯一的知识来源：量化不是「把 [0,1] 均分成 levels 份」，
// 而是「让画出来的墨迹量正比于像素亮度」。字形之间的墨迹量差得非常不均匀 ——
// ' ' 是 0，'.' 只有几个百分点，而 '#'、'%'、'@' 挤在 40%~62% 附近。
// 等间距索引的后果是亮部三档几乎看不出区别、暗部却一档跳一大截（条带感）。
//
// 默认表是**手工估计值**（近似 ASCII 字形在等宽终端字体下的经验值），不是实测；
// 真实覆盖率应当把目标字体栅格化后统计墨迹像素比，不同字体差异可达 ±5%。
// 所以接口只要求「与 glyphs 等长、暗→亮单调不减」，调用方可以整体替换成实测表。
struct Charset
{
    std::string ramp;                   // UTF-8 源串，仅用于调试打印 / 日志
    std::vector<char32_t> glyphs;       // 解码后的码点，暗 -> 亮
    std::vector<float> coverage;        // 每字形墨迹覆盖率 ∈ [0,1]，与 glyphs 等长（空格为 0）

    static Charset makeDefault();       // " .:-=+*#%@"
    static Charset makeBlocks();        // Unicode 阴影/实心块 " ░▒▓█"

    // 档位数 = 字形数量。上层用它算抖动步长与索引上界。
    // 截断到 255：一个像素只能画一个字符，再多档位没有意义，而 uint8_t
    // 回绕会把 256 档变成 0 档（「空字符集」这种静默故障比截断更难查）。
    uint8_t levels() const noexcept;
};

// 4x4 Bayer 有序抖动阈值，返回 [0,1)。
//
// 用 Bayer 而不是随机噪声：随机噪声每帧都不一样，静止画面也会闪烁（在 80x24
// 这种低分辨率下尤其刺眼），而且时间上不收敛；Bayer 阈值只跟 (x,y) 有关，
// 空间上固定，同一像素每帧拿到同一个值，画面纹丝不动。这是它被选中的唯一理由。
float bayer4x4(uint32_t x, uint32_t y) noexcept;

// 亮度 → 字符集索引（等间距档位，不查覆盖率表）。
// dither=true 时用 bayer4x4 做**阈值抖动**：落在档位区间内比例 frac 的那些格点上
// 取高一档，所以一组 16 个像素的平均亮度精确保持（不是「加噪声再夹紧」）。
// 输入亮度可能因 HDR/曝光 > 1 或 < 0，这里一律 clamp 到 [0,1]。
// 注意：这个函数只保证「第 k 档」，不知道字形墨迹量；真正的字形选择走 selectGlyph。
uint8_t quantize(float luminance01, uint32_t x, uint32_t y, uint8_t levels, bool dither) noexcept;

// 把单个码点编成 UTF-8，返回写入的字节数（1..4）。out 必须至少 4 字节。
// 非法码点（> U+10FFFF、或落在 UTF-16 代理区 U+D800..U+DFFF）被替换成
// U+FFFD（EF BF BD）并返回 3：调用方不需要检查返回值也能保证输出是合法
// UTF-8，终端不会因为半个字符而错位。
size_t encodeUtf8(char32_t codePoint, char out[4]) noexcept;

// Sobel 边缘检测结果。
struct EdgeSample
{
    float magnitude = 0.0f;      // 归一化到约 [0,1]，见 sobel() 的注释
    float angleRadians = 0.0f;   // **梯度**方向（atan2(Gy, Gx)），不是边缘走向
};

// 单像素 Sobel。luminance 为行优先亮度图，长度 w*h。
// 边界像素取不到完整 3x3 邻域，这里 clamp 到边缘像素（repeat 采样），
// 既不越界读（UB），也不跳过（跳过会在画面四周留一条没有边缘的边框，
// 而 3D 物体的轮廓经常正好压在边上）。
EdgeSample sobel(const float *luminance, uint32_t w, uint32_t h, uint32_t x, uint32_t y) noexcept;

// 梯度方向 → 边缘方向字符：'-' '/' '|' '\\'。
// **注意这里做了 90° 旋转**：Sobel 给的是梯度方向，而字符应当沿边缘走向摆放，
// 两者垂直。这是本模块最容易搞反的一处，测试里专门用竖直/水平边缘守着。
char32_t edgeGlyph(float angleRadians) noexcept;

// 一个像素的最终选择。
struct GlyphOptions
{
    bool dither = true;
    bool edges = true;            // 边缘增强（ASCII 版抗锯齿），默认开，见 §6.3
    float edgeThreshold = 0.35f;  // 归一化梯度阈值：局部对比度达到全量程的 35% 即算轮廓
};

struct Glyph
{
    char32_t codePoint = U' ';
    uint8_t level = 0;            // 在字符集中的层级（边缘像素也保留亮度层级）
    bool isEdge = false;
};

// 一个像素的最终选择。luminance 是整张亮度图（Sobel 需要邻域），行优先，长度 w*h。
// 关掉边缘后 codePoint 一定等于 charset.glyphs[level]；开启边缘时，强边缘处的
// codePoint 是方向字符、不在字符集内，但 level 仍然表示「这块有多亮」，
// 渲染器用它取颜色/亮度，用 codePoint 取字形。
Glyph selectGlyph(const Charset &charset, const float *luminance, uint32_t w, uint32_t h,
                  uint32_t x, uint32_t y, const GlyphOptions &options) noexcept;

} // namespace my3d::backend::ascii
